/**
 * H.264 + GCC video sender.
 *
 * GStreamer produces RTP into an appsink. TransportSender assigns the
 * transport sequence and owns packets until razor's pacer sends them.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "feedback_channel.h"
#include "feedback_packet.h"
#include "outer_loss_backoff.h"
#include "probe_budget.h"
#include "probe_controller.h"
#include "probe_delivery_estimator.h"
#include "rate_diagnostics.h"
#include "rate_policy.h"
#include "razor_wrapper.h"
#include "rtp_packet.h"
#include "transport_sender.h"

namespace {

constexpr uint32_t kMinBitrateKbps = 200U;
constexpr uint32_t kMaxBitrateKbps = 8000U;
constexpr uint32_t kMaxNormalPacingKbps = 9600U;
constexpr size_t kIngressLimitBytes = 4U * 1024U * 1024U;
constexpr size_t kMaxProbePacketBytes = 267U;
constexpr size_t kMinProbePacketBytes = 17U;
constexpr size_t kMaxControlEventsPerPump = 100000U;
constexpr size_t kMaxFeedbackPerLoop = 64U;
constexpr uint64_t kDrainTimeoutUs = UINT64_C(2000000);

std::atomic<bool> running{true};

void SignalHandler(int) {
    running.store(false);
}

uint64_t SteadyMicros() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t SteadyMillis() {
    return SteadyMicros() / 1000U;
}

uint32_t Low32Millis(uint64_t time_us) {
    return static_cast<uint32_t>((time_us / 1000U) & UINT64_C(0xffffffff));
}

int ParseBoundedInt(const char* text, int minimum, int maximum,
                    int fallback) {
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return fallback;
    }
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return static_cast<int>(value);
}

double ParseBoundedDouble(const char* text, double minimum,
                          double maximum, double fallback,
                          bool minimum_exclusive = false) {
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) ||
        (minimum_exclusive ? value <= minimum : value < minimum)) {
        return fallback;
    }
    return std::min(value, maximum);
}

struct SenderConfig {
    std::string host = "127.0.0.1";
    int rtp_port = 5004;
    int bitrate = 2000;
    int duration = 20;
    std::string preset = "ultrafast";
    int threads = 2;
    gcc::ProbeConfig probe;
};

enum class ParseResult { Run, Help, Error };

void PrintUsage(const char* executable) {
    std::printf(
        "Usage: %s [options]\n"
        "  --host HOST                 destination (default 127.0.0.1)\n"
        "  --port PORT                 RTP port (1..65534)\n"
        "  --bitrate KBPS              initial bitrate (200..8000)\n"
        "  --duration SECONDS          run time (1..86400)\n"
        "  --preset NAME               x264 preset\n"
        "  --threads COUNT             x264 threads (1..64)\n"
        "  --probe-first-scale VALUE   finite value >1 (default 2.0)\n"
        "  --probe-next-scale VALUE    finite value >1 (default 1.5)\n"
        "  --probe-duration-ms MS      1..299 (default 100)\n"
        "  --probe-success-ratio VALUE (0,1] (default 0.75)\n"
        "  --probe-retry-ms MS         0..600000 (default 5000)\n"
        "  --probe-max-kbps KBPS       0..8000; 0 disables probing\n"
        "  --help                      show this help\n",
        executable);
}

bool RequiresValue(const char* option) {
    const char* options[] = {
        "--host", "--port", "--bitrate", "--duration", "--preset",
        "--threads", "--probe-first-scale", "--probe-next-scale",
        "--probe-duration-ms", "--probe-success-ratio",
        "--probe-retry-ms", "--probe-max-kbps"};
    for (const char* candidate : options) {
        if (std::strcmp(option, candidate) == 0) return true;
    }
    return false;
}

ParseResult ParseArgs(int argc, char* argv[], SenderConfig& config) {
    for (int index = 1; index < argc; ++index) {
        const char* option = argv[index];
        if (std::strcmp(option, "--help") == 0) return ParseResult::Help;
        if (RequiresValue(option) &&
            (index + 1 >= argc ||
             std::strncmp(argv[index + 1], "--", 2U) == 0)) {
            std::fprintf(stderr, "ERROR: missing value for %s\n", option);
            return ParseResult::Error;
        }
        if (std::strcmp(option, "--host") == 0) {
            config.host = argv[++index];
        } else if (std::strcmp(option, "--port") == 0) {
            config.rtp_port =
                ParseBoundedInt(argv[++index], 1, 65534, config.rtp_port);
        } else if (std::strcmp(option, "--bitrate") == 0) {
            config.bitrate = ParseBoundedInt(
                argv[++index], static_cast<int>(kMinBitrateKbps),
                static_cast<int>(kMaxBitrateKbps), config.bitrate);
        } else if (std::strcmp(option, "--duration") == 0) {
            config.duration =
                ParseBoundedInt(argv[++index], 1, 86400, config.duration);
        } else if (std::strcmp(option, "--preset") == 0) {
            config.preset = argv[++index];
        } else if (std::strcmp(option, "--threads") == 0) {
            config.threads =
                ParseBoundedInt(argv[++index], 1, 64, config.threads);
        } else if (std::strcmp(option, "--probe-first-scale") == 0) {
            config.probe.first_scale = ParseBoundedDouble(
                argv[++index], 1.0, 100.0, config.probe.first_scale, true);
        } else if (std::strcmp(option, "--probe-next-scale") == 0) {
            config.probe.next_scale = ParseBoundedDouble(
                argv[++index], 1.0, 100.0, config.probe.next_scale, true);
        } else if (std::strcmp(option, "--probe-duration-ms") == 0) {
            config.probe.duration_ms = static_cast<uint32_t>(
                ParseBoundedInt(argv[++index], 1, 299,
                                static_cast<int>(config.probe.duration_ms)));
        } else if (std::strcmp(option, "--probe-success-ratio") == 0) {
            config.probe.success_ratio = ParseBoundedDouble(
                argv[++index], 0.0, 1.0, config.probe.success_ratio, true);
        } else if (std::strcmp(option, "--probe-retry-ms") == 0) {
            config.probe.retry_interval_ms = static_cast<uint32_t>(
                ParseBoundedInt(
                    argv[++index], 0, 600000,
                    static_cast<int>(config.probe.retry_interval_ms)));
        } else if (std::strcmp(option, "--probe-max-kbps") == 0) {
            config.probe.max_kbps = static_cast<uint32_t>(
                ParseBoundedInt(argv[++index], 0,
                                static_cast<int>(kMaxBitrateKbps),
                                static_cast<int>(config.probe.max_kbps)));
        } else {
            std::fprintf(stderr, "ERROR: unknown option: %s\n", option);
            return ParseResult::Error;
        }
    }
    return ParseResult::Run;
}

struct AppSinkContext {
    gcc::TransportSender* transport = nullptr;
    std::atomic<bool> accepting{true};
};

struct RawRateEvent {
    uint32_t proposed_kbps;
    uint8_t loss_q8;
    uint32_t rtt_ms;
    uint64_t now_ms;
    bool probe_related;
};

GstFlowReturn OnNewSample(GstAppSink* sink, gpointer user_data) {
    AppSinkContext* context = static_cast<AppSinkContext*>(user_data);
    if (context == nullptr || context->transport == nullptr ||
        !context->accepting.load()) {
        return GST_FLOW_FLUSHING;
    }

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return gst_app_sink_is_eos(sink) ? GST_FLOW_EOS : GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map = GST_MAP_INFO_INIT;
    if (buffer == nullptr || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    if (map.data == nullptr || map.size == 0U) {
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstFlowReturn result = GST_FLOW_OK;
    try {
        std::vector<uint8_t> packet(map.data, map.data + map.size);
        if (!context->transport->EnqueueMedia(std::move(packet))) {
            result = GST_FLOW_OK;
        }
    } catch (...) {
        result = GST_FLOW_ERROR;
    }

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return result;
}

uint32_t NormalPacingRate(uint32_t network_kbps) {
    const uint64_t scaled =
        (static_cast<uint64_t>(network_kbps) * 12U + 9U) / 10U;
    return static_cast<uint32_t>(
        std::min<uint64_t>(scaled, kMaxNormalPacingKbps));
}

const char* ProbeStateName(gcc::ProbeState state) {
    switch (state) {
        case gcc::ProbeState::Idle:
            return "idle";
        case gcc::ProbeState::Probing:
            return "probing";
        case gcc::ProbeState::WaitingForResult:
            return "waiting";
        case gcc::ProbeState::Accepted:
            return "accepted";
        case gcc::ProbeState::Failed:
            return "failed";
    }
    return "unknown";
}

const char* RateReasonName(gcc::RateReason reason) {
    switch (reason) {
        case gcc::RateReason::Normal:
            return "normal";
        case gcc::RateReason::DelayOveruse:
            return "delay_overuse";
        case gcc::RateReason::Loss:
            return "loss";
        case gcc::RateReason::ProbeAccepted:
            return "probe_accepted";
    }
    return "unknown";
}

void MergeProbeAction(gcc::ProbeAction& destination,
                      const gcc::ProbeAction& source) {
    if (source.start) {
        destination.start = true;
        destination.probe_kbps = source.probe_kbps;
    }
    destination.stop = destination.stop || source.stop;
    if (source.accept) {
        destination.accept = true;
        destination.accepted_kbps = source.accepted_kbps;
    }
}

void UnrefCreatedElements(GstElement* pipeline,
                          const std::vector<GstElement*>& elements) {
    for (GstElement* element : elements) {
        if (element != nullptr) {
            gst_object_unref(element);
        }
    }
    if (pipeline != nullptr) {
        gst_object_unref(pipeline);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    SenderConfig config;
    const ParseResult parse_result = ParseArgs(argc, argv, config);
    if (parse_result == ParseResult::Help) {
        PrintUsage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (parse_result == ParseResult::Error) return EXIT_FAILURE;

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    gst_init(&argc, &argv);

    const uint16_t rtp_port = static_cast<uint16_t>(config.rtp_port);
    const uint16_t feedback_port =
        static_cast<uint16_t>(config.rtp_port + 1);
    gcc::TransportSender::SendFunction udp_send =
        gcc::CreateUdpSendFunction(config.host, rtp_port);
    if (!udp_send) {
        std::fprintf(stderr, "ERROR: Invalid UDP destination %s:%u\n",
                     config.host.c_str(), rtp_port);
        return EXIT_FAILURE;
    }

    gcc::TransportSender transport(
        std::move(udp_send), SteadyMicros, kIngressLimitBytes);
    gcc::RatePolicy rate_policy(
        kMinBitrateKbps, kMaxBitrateKbps, 0.92);
    const uint64_t initial_now_ms = SteadyMillis();
    uint32_t current_network_kbps =
        static_cast<uint32_t>(config.bitrate);
    rate_policy.Reset(current_network_kbps, initial_now_ms);
    const gcc::RateDecision initial_rate = rate_policy.Apply(
        {gcc::RateReason::Normal, current_network_kbps, 0U,
         initial_now_ms});
    current_network_kbps = initial_rate.network_kbps;
    uint32_t current_encoder_kbps = initial_rate.encoder_kbps;
    uint32_t current_pacing_kbps =
        NormalPacingRate(current_network_kbps);

    const gcc::ProbeConfig probe_config = config.probe;
    gcc::ProbeController probe_controller(probe_config);
    gcc::ProbeDeliveryEstimator probe_delivery;
    gcc::OuterLossBackoff outer_loss_backoff(kMinBitrateKbps);
    gcc::ProbeAction deferred_probe_action;
    uint64_t probe_bytes_enqueued = 0U;

    std::deque<RawRateEvent> pending_rate_events;
    std::deque<gcc::SentPacket> deferred_sent_packets;
    bool control_queue_failed = false;
    bool scoped_probe_feedback = false;
    uint8_t loss_q8 = 0U;
    uint32_t rtt_ms = 0U;
    std::array<uint64_t, 65536> consumed_rtt_send_times{};
    uint64_t media_sent = 0U;
    uint64_t probe_sent = 0U;
    uint64_t feedback_count = 0U;
    uint64_t applied_loss_decisions = 0U;

    razor::Sender razor_sender(
        [&](uint32_t proposed_kbps, uint8_t callback_loss,
            uint32_t callback_rtt_ms) {
            try {
                pending_rate_events.push_back(
                    {proposed_kbps, callback_loss, callback_rtt_ms,
                     SteadyMillis(),
                     scoped_probe_feedback ||
                         probe_delivery.rate_events_guarded()});
            } catch (...) {
                control_queue_failed = true;
                running.store(false);
            }
        },
        [&](uint32_t packet_id, int, size_t, bool) {
            gcc::SentPacket sent;
            if (!transport.SendPaced(packet_id, sent)) {
                return;
            }
            try {
                deferred_sent_packets.push_back(sent);
            } catch (...) {
                control_queue_failed = true;
                running.store(false);
            }
        });
    if (!razor_sender.is_valid()) {
        std::fprintf(stderr, "ERROR: Failed to create razor sender\n");
        return EXIT_FAILURE;
    }
    razor_sender.set_bitrates(
        kMinBitrateKbps, current_network_kbps, kMaxBitrateKbps);
    razor_sender.set_pacing_rate(current_pacing_kbps);

    gcc::FeedbackChannel feedback(
        "0.0.0.0", feedback_port, true);
    if (!feedback.start()) {
        std::fprintf(stderr,
                     "ERROR: Failed to start feedback channel on port %u\n",
                     feedback_port);
        return EXIT_FAILURE;
    }

    std::printf("============================================================\n");
    std::printf("H.264 + GCC Video Sender (razor GCC v4.0)\n");
    std::printf("============================================================\n");
    std::printf("  Target:       %s:%u\n", config.host.c_str(), rtp_port);
    std::printf("  Network rate: %u kbps\n", current_network_kbps);
    std::printf("  Encoder rate: %u kbps\n", current_encoder_kbps);
    std::printf("  Duration:     %d seconds\n", config.duration);
    std::printf("  Feedback:     listening on port %u\n", feedback_port);
    std::printf(
        "probe_config first_scale=%.3f next_scale=%.3f duration_ms=%u "
        "success_ratio=%.3f retry_ms=%u max_kbps=%u\n",
        probe_config.first_scale, probe_config.next_scale,
        probe_config.duration_ms, probe_config.success_ratio,
        probe_config.retry_interval_ms, probe_config.max_kbps);
    std::printf("============================================================\n\n");

    GstElement* pipeline = gst_pipeline_new("sender-pipeline");
    GstElement* source =
        gst_element_factory_make("videotestsrc", "video-src");
    GstElement* convert =
        gst_element_factory_make("videoconvert", "video-convert");
    GstElement* encoder =
        gst_element_factory_make("x264enc", "encoder");
    GstElement* parser =
        gst_element_factory_make("h264parse", "h264-parser");
    GstElement* payloader =
        gst_element_factory_make("rtph264pay", "rtp-payloader");
    GstElement* app_sink =
        gst_element_factory_make("appsink", "rtp-app-sink");
    const std::vector<GstElement*> created = {
        source, convert, encoder, parser, payloader, app_sink};

    if (pipeline == nullptr || source == nullptr || convert == nullptr ||
        encoder == nullptr || parser == nullptr || payloader == nullptr ||
        app_sink == nullptr) {
        std::fprintf(stderr, "ERROR: Failed to create GStreamer elements\n");
        UnrefCreatedElements(pipeline, created);
        feedback.stop();
        return EXIT_FAILURE;
    }

    g_object_set(source, "is-live", TRUE, "pattern", 1, nullptr);
    g_object_set(
        encoder, "tune", 0x4, "speed-preset", 1,
        "bitrate", static_cast<guint>(current_encoder_kbps),
        "key-int-max", 60, "bframes", 0, "threads", config.threads,
        "byte-stream", TRUE, "aud", TRUE, "rc-lookahead", 0, nullptr);
    g_object_set(parser, "config-interval", -1, nullptr);
    g_object_set(
        payloader, "pt", 96, "mtu", 1400, "config-interval", -1,
        nullptr);
    g_object_set(
        app_sink, "emit-signals", TRUE, "sync", FALSE,
        "max-buffers", static_cast<guint>(64), "drop", FALSE, nullptr);

    gst_bin_add_many(
        GST_BIN(pipeline), source, convert, encoder, parser, payloader,
        app_sink, nullptr);

    bool linked = gst_element_link_many(source, convert, encoder, nullptr);
    GstCaps* h264_caps = gst_caps_new_simple(
        "video/x-h264", "profile", G_TYPE_STRING, "baseline", nullptr);
    linked = linked &&
             gst_element_link_filtered(encoder, parser, h264_caps);
    gst_caps_unref(h264_caps);
    linked = linked &&
             gst_element_link_many(parser, payloader, app_sink, nullptr);
    if (!linked) {
        std::fprintf(stderr, "ERROR: Failed to link GStreamer pipeline\n");
        gst_object_unref(pipeline);
        feedback.stop();
        return EXIT_FAILURE;
    }

    AppSinkContext sink_context;
    sink_context.transport = &transport;
    const gulong sample_handler = g_signal_connect(
        app_sink, "new-sample", G_CALLBACK(OnNewSample), &sink_context);
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    if (bus == nullptr ||
        gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "ERROR: Failed to start GStreamer pipeline\n");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_element_get_state(
            pipeline, nullptr, nullptr, 2U * GST_SECOND);
        sink_context.accepting.store(false);
        if (g_signal_handler_is_connected(app_sink, sample_handler)) {
            g_signal_handler_disconnect(app_sink, sample_handler);
        }
        if (bus != nullptr) {
            gst_object_unref(bus);
        }
        gst_object_unref(pipeline);
        feedback.stop();
        return EXIT_FAILURE;
    }
    gst_element_get_state(pipeline, nullptr, nullptr, 2U * GST_SECOND);

    std::printf("  Sender PLAYING\n\n");
    std::fflush(stdout);

    const uint64_t start_us = SteadyMicros();
    uint64_t last_heartbeat_us = start_us;
    uint64_t last_stats_us = start_us;
    bool loop_error = false;

    const auto set_pacing = [&](uint32_t kbps) {
        current_pacing_kbps =
            std::max(kMinBitrateKbps,
                     std::min(kbps, kMaxNormalPacingKbps));
        razor_sender.set_pacing_rate(current_pacing_kbps);
    };
    gcc::ProbeState reported_probe_state = probe_controller.state();
    bool has_reported_rate = false;
    gcc::RateReason reported_rate_reason = gcc::RateReason::Normal;
    uint64_t last_rate_transition_ms = 0U;
    const auto report_probe_transition = [&]() {
        const gcc::ProbeState current = probe_controller.state();
        if (current == reported_probe_state) return;
        std::printf(
            "gcc_transition event=probe_state from=%s to=%s "
            "probe_kbps=%u\n",
            ProbeStateName(reported_probe_state), ProbeStateName(current),
            probe_controller.active_probe_kbps());
        std::fflush(stdout);
        reported_probe_state = current;
    };
    const auto apply_rate_decision =
        [&](const gcc::RateDecision& decision, gcc::RateReason reason) {
            if (!decision.changed) {
                return;
            }
            const uint32_t previous = current_network_kbps;
            current_network_kbps = decision.network_kbps;
            current_encoder_kbps = decision.encoder_kbps;
            const uint64_t now_ms = SteadyMillis();
            if (gcc::ShouldReportRateTransition(
                    reason, has_reported_rate, reported_rate_reason,
                    now_ms, last_rate_transition_ms)) {
                std::printf(
                    "gcc_transition event=rate reason=%s from_kbps=%u "
                    "to_kbps=%u encoder_kbps=%u\n",
                    RateReasonName(reason), previous, current_network_kbps,
                    current_encoder_kbps);
                std::fflush(stdout);
                has_reported_rate = true;
                reported_rate_reason = reason;
                last_rate_transition_ms = now_ms;
            }
            g_object_set(
                encoder, "bitrate",
                static_cast<guint>(current_encoder_kbps), nullptr);
            if (probe_controller.state() != gcc::ProbeState::Probing) {
                set_pacing(NormalPacingRate(current_network_kbps));
            }
        };
    // Pacer and bitrate callbacks only append raw events. Pump outside the C
    // callbacks, prioritizing completed sends and deriving rate reasons
    // against the target current at consumption time.
    const auto pump_control_events = [&]() {
        size_t processed = 0U;
        while (!deferred_sent_packets.empty() ||
               !pending_rate_events.empty()) {
            if (++processed > kMaxControlEventsPerPump) {
                control_queue_failed = true;
                running.store(false);
                return false;
            }
            if (!deferred_sent_packets.empty()) {
                const gcc::SentPacket sent =
                    deferred_sent_packets.front();
                deferred_sent_packets.pop_front();
                razor_sender.on_send(
                    sent.transport_sequence, sent.size);
                if (sent.probe) {
                    ++probe_sent;
                    probe_delivery.OnProbePacketSent(sent);
                    MergeProbeAction(
                        deferred_probe_action,
                        probe_controller.OnProbePacketSent(
                            sent.size, sent.send_time_us / 1000U));
                } else {
                    ++media_sent;
                }
                continue;
            }

            const RawRateEvent event = pending_rate_events.front();
            pending_rate_events.pop_front();
            if (event.probe_related) {
                continue;
            }
            const gcc::RateReason reason =
                event.proposed_kbps < current_network_kbps
                    ? (event.loss_q8 > 25U
                           ? gcc::RateReason::Loss
                           : gcc::RateReason::DelayOveruse)
                    : gcc::RateReason::Normal;
            const gcc::RateDecision decision =
                rate_policy.Apply(
                    {reason, event.proposed_kbps, event.rtt_ms,
                     event.now_ms});
            apply_rate_decision(decision, reason);
            if (reason == gcc::RateReason::Loss && decision.changed) {
                ++applied_loss_decisions;
            }
        }
        return true;
    };
    const auto handle_probe_action =
        [&](const gcc::ProbeAction& action) {
            if (action.stop) {
                set_pacing(NormalPacingRate(current_network_kbps));
            }
            if (action.accept) {
                probe_delivery.Finish();
                apply_rate_decision(
                    rate_policy.Apply(
                        {gcc::RateReason::ProbeAccepted,
                         action.accepted_kbps, rtt_ms, SteadyMillis()}),
                    gcc::RateReason::ProbeAccepted);
                razor_sender.set_bitrates(
                    kMinBitrateKbps, current_network_kbps,
                    kMaxBitrateKbps);
                set_pacing(NormalPacingRate(current_network_kbps));
            }
            if (action.start) {
                probe_bytes_enqueued = 0U;
                probe_delivery.Start(action.probe_kbps);
                set_pacing(action.probe_kbps);
            }
            if (probe_controller.state() == gcc::ProbeState::Failed) {
                probe_delivery.Finish();
                set_pacing(NormalPacingRate(current_network_kbps));
            }
        };
    const auto process_control_events = [&]() {
        for (size_t round = 0U; round < 1024U; ++round) {
            if (!pump_control_events()) {
                return false;
            }
            const gcc::ProbeAction action = deferred_probe_action;
            deferred_probe_action = gcc::ProbeAction{};
            if (action.start || action.stop || action.accept) {
                handle_probe_action(action);
                report_probe_transition();
                continue;
            }
            report_probe_transition();
            return true;
        }
        control_queue_failed = true;
        running.store(false);
        return false;
    };
    const auto print_stats = [&]() {
        const razor::SenderStats stats = razor_sender.stats();
        std::printf(
            "gcc_stats encoder_kbps=%u target_kbps=%u pacing_kbps=%u "
            "acked_kbps=%u probe_delivery_kbps=%u loss_q8=%u "
            "rtt_ms=%u delay_state=%d "
            "media_sent=%llu probe_sent=%llu feedback=%llu "
            "feedback_matched=%llu pacer_queue_ms=%lld "
            "ingress_bytes=%zu probe_state=%s\n",
            current_encoder_kbps, current_network_kbps,
            current_pacing_kbps, stats.acked_bitrate_kbps,
            probe_delivery.estimate_kbps(),
            static_cast<unsigned>(loss_q8), rtt_ms, stats.delay_state,
            static_cast<unsigned long long>(media_sent),
            static_cast<unsigned long long>(probe_sent),
            static_cast<unsigned long long>(feedback_count),
            static_cast<unsigned long long>(
                stats.matched_feedback_packets),
            static_cast<long long>(stats.pacer_queue_ms),
            transport.ingress_bytes(), ProbeStateName(probe_controller.state()));
        std::fflush(stdout);
    };

    while (running.load()) {
        const uint64_t now_us = SteadyMicros();
        const uint64_t now_ms = now_us / 1000U;
        if (now_us - start_us >=
            static_cast<uint64_t>(config.duration) * UINT64_C(1000000)) {
            break;
        }

        GstMessage* message = gst_bus_pop_filtered(
            bus, static_cast<GstMessageType>(
                     GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message != nullptr) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                std::fprintf(stderr, "ERROR: %s\n",
                             error != nullptr ? error->message : "unknown");
                if (error != nullptr) {
                    g_error_free(error);
                }
                g_free(debug);
                loop_error = true;
            }
            gst_message_unref(message);
            break;
        }

        gcc::FeedbackPacket packet;
        const size_t feedback_budget = std::min(
            feedback.queued_feedback_count(), kMaxFeedbackPerLoop);
        for (size_t feedback_index = 0U;
             feedback_index < feedback_budget &&
             feedback.poll(packet);
             ++feedback_index) {
            ++feedback_count;
            loss_q8 = packet.loss_q8;
            const uint64_t loss_decisions_before =
                applied_loss_decisions;

            uint64_t send_time_us = 0U;
            uint64_t& consumed_send_time_us =
                consumed_rtt_send_times[packet.sequence];
            if (transport.LookupSendTime(
                    packet.sequence, send_time_us) &&
                send_time_us != consumed_send_time_us) {
                // Consume the sample before validation. Repeated or reordered
                // feedback for this sequence generation cannot age into a
                // fake RTT, while other sequences may arrive out of order.
                consumed_send_time_us = send_time_us;
                const uint64_t sender_feedback_us = SteadyMicros();
                if (sender_feedback_us >= send_time_us &&
                    sender_feedback_us - send_time_us <=
                        UINT64_C(5000000)) {
                    uint32_t measured_rtt_ms = 0U;
                    if (gcc::ComputeNetworkRtt(
                            Low32Millis(send_time_us),
                            Low32Millis(sender_feedback_us),
                            packet.receive_time_ms,
                            packet.feedback_time_ms, measured_rtt_ms) &&
                        measured_rtt_ms >= 1U &&
                        measured_rtt_ms <= 5000U) {
                        rtt_ms = measured_rtt_ms;
                        razor_sender.update_rtt(rtt_ms);
                        process_control_events();
                    }
                }
            }

            gcc::SentPacketInfo feedback_info;
            bool probe_related_batch =
                probe_delivery.rate_events_guarded();
            if (transport.LookupSentPacket(
                    packet.sequence, feedback_info)) {
                probe_related_batch =
                    probe_delivery.BeginFeedback(feedback_info);
                scoped_probe_feedback = probe_related_batch;
                probe_delivery.ObserveFeedback(
                    feedback_info, packet.receive_time_ms);
            } else {
                gcc::SentHistoryWatermark watermark;
                transport.GetSentHistoryWatermark(watermark);
                probe_related_batch =
                    probe_delivery.BeginUnmatchedFeedback(watermark);
                scoped_probe_feedback = probe_related_batch;
            }
            if (!packet.razor_payload.empty()) {
                razor_sender.on_feedback(
                    packet.razor_payload.data(),
                    static_cast<int>(packet.razor_payload.size()));
                process_control_events();
            }
            scoped_probe_feedback = false;
            const gcc::OuterLossDecision outer_loss =
                outer_loss_backoff.Evaluate(
                    loss_q8, current_network_kbps, rtt_ms, now_ms,
                    !probe_related_batch);
            if (outer_loss.changed &&
                applied_loss_decisions == loss_decisions_before) {
                const gcc::RateDecision decision =
                    rate_policy.Apply(
                        {gcc::RateReason::Loss,
                         outer_loss.proposed_kbps, rtt_ms, now_ms});
                apply_rate_decision(decision, gcc::RateReason::Loss);
                if (decision.changed) ++applied_loss_decisions;
            }
            const razor::SenderStats stats = razor_sender.stats();
            const uint64_t loss_adjusted_delivery =
                static_cast<uint64_t>(probe_delivery.estimate_kbps()) *
                static_cast<uint64_t>(255U - loss_q8) / 255U;
            const uint32_t probe_acked_kbps =
                static_cast<uint32_t>(loss_adjusted_delivery);
            MergeProbeAction(
                deferred_probe_action,
                probe_controller.OnFeedback(
                    {probe_acked_kbps,
                     static_cast<double>(loss_q8) / 255.0,
                     stats.delay_state == 2, now_ms}));
            process_control_events();
        }

        MergeProbeAction(
            deferred_probe_action, probe_controller.OnTimer(now_ms));
        process_control_events();

        gcc::RtpHeader latest_media_header;
        if (transport.latest_media_header(latest_media_header)) {
            MergeProbeAction(
                deferred_probe_action,
                probe_controller.OnStableTarget(
                    current_network_kbps, now_ms));
            process_control_events();
        }

        if (probe_controller.state() == gcc::ProbeState::Probing &&
            transport.latest_media_header(latest_media_header)) {
            const uint64_t desired_bytes = gcc::ProbeBudgetBytes(
                probe_controller.active_probe_kbps(),
                probe_controller.probe_start_ms(), now_ms,
                probe_config.duration_ms);
            while (probe_bytes_enqueued < desired_bytes) {
                const uint64_t deficit =
                    desired_bytes - probe_bytes_enqueued;
                if (deficit < kMinProbePacketBytes) {
                    break;
                }
                const size_t packet_size = static_cast<size_t>(
                    std::min<uint64_t>(deficit, kMaxProbePacketBytes));
                std::vector<uint8_t> probe = gcc::BuildProbeRtpPacket(
                    latest_media_header, 0U, packet_size);
                if (probe.empty() ||
                    !transport.EnqueueProbe(std::move(probe))) {
                    break;
                }
                probe_bytes_enqueued += packet_size;
            }
        }

        transport.DrainIngress(
            [&](uint32_t packet_id, size_t size) {
                const bool accepted =
                    razor_sender.add_packet(packet_id, false, size);
                process_control_events();
                return accepted;
            });
        process_control_events();

        const uint64_t heartbeat_now_us = SteadyMicros();
        if (heartbeat_now_us - last_heartbeat_us >= 5000U) {
            razor_sender.heartbeat();
            process_control_events();
            last_heartbeat_us = heartbeat_now_us;
        }

        if (now_us - last_stats_us >= UINT64_C(1000000)) {
            print_stats();
            last_stats_us = now_us;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Stop production before draining every packet already accepted by the
    // transport and razor pacer.
    sink_context.accepting.store(false);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_element_get_state(pipeline, nullptr, nullptr, 2U * GST_SECOND);
    if (g_signal_handler_is_connected(app_sink, sample_handler)) {
        g_signal_handler_disconnect(app_sink, sample_handler);
    }

    transport.DrainIngress(
        [&](uint32_t packet_id, size_t size) {
            const bool accepted =
                razor_sender.add_packet(packet_id, false, size);
            process_control_events();
            return accepted;
        });
    process_control_events();

    const uint64_t drain_deadline_us =
        SteadyMicros() + kDrainTimeoutUs;
    bool drained = false;
    while (!control_queue_failed) {
        razor_sender.heartbeat();
        process_control_events();
        if (transport.ingress_bytes() == 0U &&
            transport.pending_packets() == 0U &&
            deferred_sent_packets.empty() &&
            pending_rate_events.empty() &&
            razor_sender.pacer_queue_ms() == 0) {
            drained = true;
            break;
        }
        if (SteadyMicros() >= drain_deadline_us) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!drained) {
        std::fprintf(
            stderr,
            "ERROR: Timed out draining sender: ingress_bytes=%zu "
            "pending_packets=%zu deferred_sends=%zu pending_rates=%zu "
            "pacer_queue_ms=%lld\n",
            transport.ingress_bytes(), transport.pending_packets(),
            deferred_sent_packets.size(), pending_rate_events.size(),
            static_cast<long long>(razor_sender.pacer_queue_ms()));
        loop_error = true;
    }
    if (control_queue_failed) {
        std::fprintf(
            stderr,
            "ERROR: Failed to pump a synchronous razor callback event\n");
        loop_error = true;
    }
    feedback.stop();
    print_stats();
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return loop_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
