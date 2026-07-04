/**
 * H.264 + GCC loopback demo.
 *
 * Streaming callbacks only copy media or queue parsed arrivals. The main
 * control loop owns both razor controllers and all feedback/rate decisions.
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
#include <mutex>
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

constexpr uint32_t kMinKbps = 200U;
constexpr uint32_t kMaxKbps = 8000U;
constexpr uint32_t kMaxPacingKbps = 9600U;
constexpr size_t kIngressBytes = 4U * 1024U * 1024U;
constexpr size_t kMaxArrivals = 4096U;
constexpr size_t kMaxFeedbackPerLoop = 64U;
constexpr size_t kMaxControlEvents = 100000U;
constexpr uint64_t kDrainTimeoutUs = UINT64_C(2000000);
constexpr size_t kMinProbeBytes = 17U;
constexpr size_t kMaxProbeBytes = 267U;

std::atomic<bool> running{true};
std::atomic<uint64_t> frames{0U};

void SignalHandler(int) { running.store(false); }

uint64_t SteadyMicros() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t SteadyMillis() { return SteadyMicros() / 1000U; }

uint32_t Low32Millis(uint64_t time_us) {
    return static_cast<uint32_t>((time_us / 1000U) & UINT64_C(0xffffffff));
}

int ParseBoundedInt(const char* text, int minimum, int maximum, int fallback) {
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') return fallback;
    return static_cast<int>(
        std::max<long>(minimum, std::min<long>(value, maximum)));
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

struct Config {
    int duration = 20;
    int bitrate = 2000;
    int port = 5004;
    gcc::ProbeConfig probe;
};

enum class ParseResult { Run, Help, Error };

void PrintUsage(const char* executable) {
    std::printf(
        "Usage: %s [options]\n"
        "  --duration SECONDS          run time (1..86400)\n"
        "  --bitrate KBPS              initial bitrate (200..8000)\n"
        "  --port PORT                 RTP port (1..65534)\n"
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
        "--duration", "--bitrate", "--port", "--probe-first-scale",
        "--probe-next-scale", "--probe-duration-ms",
        "--probe-success-ratio", "--probe-retry-ms",
        "--probe-max-kbps"};
    for (const char* candidate : options) {
        if (std::strcmp(option, candidate) == 0) return true;
    }
    return false;
}

ParseResult ParseArgs(int argc, char* argv[], Config& config) {
    for (int i = 1; i < argc; ++i) {
        const char* option = argv[i];
        if (std::strcmp(option, "--help") == 0) return ParseResult::Help;
        if (RequiresValue(option) &&
            (i + 1 >= argc ||
             std::strncmp(argv[i + 1], "--", 2U) == 0)) {
            std::fprintf(stderr, "ERROR: missing value for %s\n", option);
            return ParseResult::Error;
        }
        if (std::strcmp(option, "--duration") == 0) {
            config.duration = ParseBoundedInt(argv[++i], 1, 86400, config.duration);
        } else if (std::strcmp(option, "--bitrate") == 0) {
            config.bitrate = ParseBoundedInt(
                argv[++i], static_cast<int>(kMinKbps),
                static_cast<int>(kMaxKbps), config.bitrate);
        } else if (std::strcmp(option, "--port") == 0) {
            config.port = ParseBoundedInt(argv[++i], 1, 65534, config.port);
        } else if (std::strcmp(option, "--probe-first-scale") == 0) {
            config.probe.first_scale = ParseBoundedDouble(
                argv[++i], 1.0, 100.0, config.probe.first_scale, true);
        } else if (std::strcmp(option, "--probe-next-scale") == 0) {
            config.probe.next_scale = ParseBoundedDouble(
                argv[++i], 1.0, 100.0, config.probe.next_scale, true);
        } else if (std::strcmp(option, "--probe-duration-ms") == 0) {
            config.probe.duration_ms = static_cast<uint32_t>(
                ParseBoundedInt(argv[++i], 1, 299,
                                static_cast<int>(config.probe.duration_ms)));
        } else if (std::strcmp(option, "--probe-success-ratio") == 0) {
            config.probe.success_ratio = ParseBoundedDouble(
                argv[++i], 0.0, 1.0, config.probe.success_ratio, true);
        } else if (std::strcmp(option, "--probe-retry-ms") == 0) {
            config.probe.retry_interval_ms = static_cast<uint32_t>(
                ParseBoundedInt(
                    argv[++i], 0, 600000,
                    static_cast<int>(config.probe.retry_interval_ms)));
        } else if (std::strcmp(option, "--probe-max-kbps") == 0) {
            config.probe.max_kbps = static_cast<uint32_t>(
                ParseBoundedInt(argv[++i], 0,
                                static_cast<int>(kMaxKbps),
                                static_cast<int>(config.probe.max_kbps)));
        } else {
            std::fprintf(stderr, "ERROR: unknown option: %s\n", option);
            return ParseResult::Error;
        }
    }
    return ParseResult::Run;
}

struct PacketArrival {
    uint16_t transport_sequence;
    uint32_t arrival_ms;
    size_t size;
    bool probe;
};

class ArrivalQueue {
public:
    bool Push(const PacketArrival& arrival) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= kMaxArrivals) {
                overflow_.fetch_add(1U);
                return false;
            }
            queue_.push_back(arrival);
            return true;
        } catch (...) {
            overflow_.fetch_add(1U);
            return false;
        }
    }

    void Drain(std::deque<PacketArrival>& output) {
        std::lock_guard<std::mutex> lock(mutex_);
        output.clear();
        output.swap(queue_);
    }

    uint64_t overflow() const noexcept { return overflow_.load(); }

private:
    std::mutex mutex_;
    std::deque<PacketArrival> queue_;
    std::atomic<uint64_t> overflow_{0U};
};

struct ReceiverProbeContext {
    ArrivalQueue* arrivals = nullptr;
    gcc::MediaSequenceTranslator translator;
    std::atomic<uint64_t> media_reorder_drops{0U};
};

GstPadProbeReturn OnRtpPacket(
    GstPad*, GstPadProbeInfo* info, gpointer user_data) {
    ReceiverProbeContext* context =
        static_cast<ReceiverProbeContext*>(user_data);
    if (context == nullptr || context->arrivals == nullptr) {
        return GST_PAD_PROBE_DROP;
    }
    try {
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buffer == nullptr) return GST_PAD_PROBE_DROP;
        GstMapInfo map = GST_MAP_INFO_INIT;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            return GST_PAD_PROBE_DROP;
        }
        gcc::RtpHeader header;
        const bool parsed = gcc::ParseRtpHeader(map.data, map.size, header);
        const bool probe =
            parsed && gcc::IsProbeRtpPacket(map.data, map.size);
        const size_t packet_size = map.size;
        gst_buffer_unmap(buffer, &map);
        if (!parsed) return GST_PAD_PROBE_OK;

        context->arrivals->Push(
            {header.sequence, Low32Millis(SteadyMicros()),
             packet_size, probe});
        if (probe) {
            context->translator.OnProbe(header.sequence);
            return GST_PAD_PROBE_DROP;
        }

        uint16_t media_sequence = 0U;
        if (!context->translator.TranslateMedia(
                header.sequence, media_sequence)) {
            context->media_reorder_drops.fetch_add(1U);
            return GST_PAD_PROBE_DROP;
        }
        if (media_sequence != header.sequence) {
            GstBuffer* writable = gst_buffer_make_writable(buffer);
            if (writable == nullptr) return GST_PAD_PROBE_DROP;
            GST_PAD_PROBE_INFO_DATA(info) = writable;
            GstMapInfo write_map = GST_MAP_INFO_INIT;
            if (!gst_buffer_map(writable, &write_map, GST_MAP_READWRITE)) {
                return GST_PAD_PROBE_DROP;
            }
            const bool rewritten = gcc::RewriteRtpSequence(
                write_map.data, write_map.size, media_sequence);
            gst_buffer_unmap(writable, &write_map);
            if (!rewritten) return GST_PAD_PROBE_DROP;
        }
        return GST_PAD_PROBE_OK;
    } catch (...) {
        return GST_PAD_PROBE_DROP;
    }
}

GstPadProbeReturn CountFrame(
    GstPad*, GstPadProbeInfo*, gpointer) {
    frames.fetch_add(1U);
    return GST_PAD_PROBE_OK;
}

struct AppSinkContext {
    gcc::TransportSender* transport = nullptr;
    std::atomic<bool> accepting{true};
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
    if (buffer == nullptr ||
        !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    GstFlowReturn result = GST_FLOW_OK;
    try {
        if (map.data == nullptr || map.size == 0U) {
            result = GST_FLOW_ERROR;
        } else {
            std::vector<uint8_t> packet(map.data, map.data + map.size);
            context->transport->EnqueueMedia(std::move(packet));
        }
    } catch (...) {
        result = GST_FLOW_ERROR;
    }
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return result;
}

struct RawRateEvent {
    uint32_t proposed_kbps;
    uint8_t loss_q8;
    uint32_t rtt_ms;
    uint64_t now_ms;
    bool probe_related;
    bool delay_overusing;
};

uint32_t NormalPacing(uint32_t target) {
    return static_cast<uint32_t>(std::min<uint64_t>(
        (static_cast<uint64_t>(target) * 12U + 9U) / 10U,
        kMaxPacingKbps));
}

const char* ProbeStateName(gcc::ProbeState state) {
    switch (state) {
        case gcc::ProbeState::Idle: return "idle";
        case gcc::ProbeState::Probing: return "probing";
        case gcc::ProbeState::WaitingForResult: return "waiting";
        case gcc::ProbeState::Accepted: return "accepted";
        case gcc::ProbeState::Failed: return "failed";
    }
    return "unknown";
}

const char* RateReasonName(gcc::RateReason reason) {
    switch (reason) {
        case gcc::RateReason::Normal: return "normal";
        case gcc::RateReason::DelayOveruse: return "delay_overuse";
        case gcc::RateReason::Loss: return "loss";
        case gcc::RateReason::ProbeAccepted: return "probe_accepted";
    }
    return "unknown";
}

void MergeProbeAction(gcc::ProbeAction& into, const gcc::ProbeAction& from) {
    if (from.start) {
        into.start = true;
        into.probe_kbps = from.probe_kbps;
    }
    into.stop = into.stop || from.stop;
    if (from.accept) {
        into.accept = true;
        into.accepted_kbps = from.accepted_kbps;
    }
}

enum class BusEvent {
    None,
    Eos,
    Error,
};

BusEvent PollBus(GstBus* bus, const char* name) {
    if (bus == nullptr) return BusEvent::None;
    GstMessage* message = gst_bus_pop_filtered(
        bus, static_cast<GstMessageType>(
                 GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (message == nullptr) return BusEvent::None;
    const bool failed = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
    if (failed) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::fprintf(stderr, "ERROR: %s pipeline: %s\n", name,
                     error != nullptr ? error->message : "unknown");
        if (error != nullptr) g_error_free(error);
        g_free(debug);
    }
    gst_message_unref(message);
    return failed ? BusEvent::Error : BusEvent::Eos;
}

void UnrefUnparented(
    GstElement* pipeline, const std::vector<GstElement*>& elements) {
    for (GstElement* element : elements) {
        if (element != nullptr) gst_object_unref(element);
    }
    if (pipeline != nullptr) gst_object_unref(pipeline);
}

}  // namespace

int main(int argc, char* argv[]) {
    Config config;
    const ParseResult parse_result = ParseArgs(argc, argv, config);
    if (parse_result == ParseResult::Help) {
        PrintUsage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (parse_result == ParseResult::Error) return EXIT_FAILURE;
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    gst_init(&argc, &argv);

    const uint16_t rtp_port = static_cast<uint16_t>(config.port);
    const uint16_t feedback_port =
        static_cast<uint16_t>(config.port + 1);
    gcc::TransportSender::SendFunction udp_send =
        gcc::CreateUdpSendFunction("127.0.0.1", rtp_port);
    if (!udp_send) return EXIT_FAILURE;
    gcc::TransportSender transport(
        std::move(udp_send), SteadyMicros, kIngressBytes);

    gcc::FeedbackChannel sender_feedback(
        "0.0.0.0", feedback_port, true);
    gcc::FeedbackChannel receiver_feedback(
        "127.0.0.1", feedback_port, false);
    if (!sender_feedback.start() || !receiver_feedback.start()) {
        std::fprintf(stderr, "ERROR: Failed to start feedback channels\n");
        sender_feedback.stop();
        receiver_feedback.stop();
        return EXIT_FAILURE;
    }

    gcc::RatePolicy rate_policy(kMinKbps, kMaxKbps, 0.92);
    uint32_t target_kbps = static_cast<uint32_t>(config.bitrate);
    rate_policy.Reset(target_kbps, SteadyMillis());
    const gcc::RateDecision initial = rate_policy.Apply(
        {gcc::RateReason::Normal, target_kbps, 0U, SteadyMillis()});
    target_kbps = initial.network_kbps;
    uint32_t encoder_kbps = initial.encoder_kbps;
    uint32_t pacing_kbps = NormalPacing(target_kbps);

    const gcc::ProbeConfig probe_config = config.probe;
    gcc::ProbeController probe_controller(probe_config);
    gcc::ProbeDeliveryEstimator probe_delivery;
    gcc::OuterLossBackoff outer_loss_backoff(kMinKbps);
    std::printf(
        "probe_config first_scale=%.3f next_scale=%.3f duration_ms=%u "
        "success_ratio=%.3f retry_ms=%u max_kbps=%u\n",
        probe_config.first_scale, probe_config.next_scale,
        probe_config.duration_ms, probe_config.success_ratio,
        probe_config.retry_interval_ms, probe_config.max_kbps);
    std::fflush(stdout);
    gcc::ProbeAction deferred_probe_action;
    uint64_t probe_bytes_enqueued = 0U;
    uint64_t probe_bytes_sent = 0U;
    uint64_t probe_sent_baseline = 0U;
    uint64_t probe_epoch = 0U;
    std::deque<RawRateEvent> pending_rates;
    std::deque<gcc::SentPacket> deferred_sends;
    bool control_failed = false;
    bool scoped_probe_feedback = false;
    uint64_t media_sent = 0U;
    uint64_t probe_sent = 0U;
    uint64_t probe_cluster_sent = 0U;
    uint64_t feedback_count = 0U;
    uint64_t applied_loss_decisions = 0U;
    uint8_t loss_q8 = 0U;
    uint32_t rtt_ms = 0U;
    uint32_t validated_probe_kbps = 0U;
    std::array<uint64_t, 65536> consumed_send_times{};

    const auto advance_probe_send_progress = [&]() {
        gcc::SentHistoryWatermark watermark;
        if (transport.GetSentHistoryWatermark(watermark)) {
            probe_delivery.AdvanceSendProgress(watermark);
        }
    };

    razor::Sender* razor_tx_view = nullptr;
    razor::Sender razor_tx(
        [&](uint32_t proposed, uint8_t loss, uint32_t rtt) {
            try {
                advance_probe_send_progress();
                pending_rates.push_back(
                    {proposed, loss, rtt, SteadyMillis(),
                     scoped_probe_feedback ||
                         probe_delivery.rate_events_guarded(),
                     razor_tx_view != nullptr &&
                         razor_tx_view->stats().delay_state == 2});
            } catch (...) {
                control_failed = true;
                running.store(false);
            }
        },
        [&](uint32_t packet_id, int, size_t, bool) {
            if (!transport.SchedulePaced(packet_id)) {
                control_failed = true;
                running.store(false);
            }
        },
        250U);
    razor_tx_view = &razor_tx;
    if (!razor_tx.is_valid()) return EXIT_FAILURE;
    razor_tx.set_bitrates(kMinKbps, target_kbps, kMaxKbps);
    razor_tx.set_pacing_rate(pacing_kbps);
    transport.SetPacingRate(pacing_kbps);

    gcc::LossTracker loss_tracker;
    bool has_processed = false;
    uint16_t last_processed_sequence = 0U;
    uint32_t last_processed_arrival_ms = 0U;
    razor::Receiver razor_rx([&](const uint8_t* data, int size) {
        try {
            if (!has_processed || size < 0 ||
                static_cast<size_t>(size) > gcc::kMaxFeedbackPayload ||
                (size > 0 && data == nullptr)) {
                return;
            }
            gcc::FeedbackPacket packet;
            packet.sequence = last_processed_sequence;
            packet.loss_q8 = loss_tracker.get_loss_q8();
            packet.receive_time_ms = last_processed_arrival_ms;
            packet.feedback_time_ms = Low32Millis(SteadyMicros());
            if (size > 0) packet.razor_payload.assign(data, data + size);
            receiver_feedback.send_feedback(packet);
        } catch (...) {
        }
    });
    if (!razor_rx.is_valid()) return EXIT_FAILURE;

    GstElement* rx_pipeline = gst_pipeline_new("demo-receiver");
    GstElement* rx_source = gst_element_factory_make("udpsrc", "rx-source");
    GstElement* rx_depay = gst_element_factory_make("rtph264depay", "rx-depay");
    GstElement* rx_parser = gst_element_factory_make("h264parse", "rx-parser");
    GstElement* rx_decoder = gst_element_factory_make("avdec_h264", "rx-decoder");
    GstElement* rx_convert = gst_element_factory_make("videoconvert", "rx-convert");
    GstElement* rx_sink = gst_element_factory_make("fakesink", "rx-sink");
    GstElement* tx_pipeline = gst_pipeline_new("demo-sender");
    GstElement* tx_source = gst_element_factory_make("videotestsrc", "tx-source");
    GstElement* tx_convert = gst_element_factory_make("videoconvert", "tx-convert");
    GstElement* tx_encoder = gst_element_factory_make("x264enc", "tx-encoder");
    GstElement* tx_parser = gst_element_factory_make("h264parse", "tx-parser");
    GstElement* tx_pay = gst_element_factory_make("rtph264pay", "tx-pay");
    GstElement* tx_sink = gst_element_factory_make("appsink", "tx-appsink");
    const std::vector<GstElement*> rx_elements = {
        rx_source, rx_depay, rx_parser, rx_decoder, rx_convert, rx_sink};
    const std::vector<GstElement*> tx_elements = {
        tx_source, tx_convert, tx_encoder, tx_parser, tx_pay, tx_sink};
    if (rx_pipeline == nullptr || rx_source == nullptr ||
        rx_depay == nullptr || rx_parser == nullptr ||
        rx_decoder == nullptr || rx_convert == nullptr ||
        rx_sink == nullptr || tx_pipeline == nullptr ||
        tx_source == nullptr || tx_convert == nullptr ||
        tx_encoder == nullptr || tx_parser == nullptr ||
        tx_pay == nullptr || tx_sink == nullptr) {
        std::fprintf(stderr, "ERROR: Failed to create GStreamer elements\n");
        UnrefUnparented(rx_pipeline, rx_elements);
        UnrefUnparented(tx_pipeline, tx_elements);
        return EXIT_FAILURE;
    }

    GstCaps* rtp_caps = gst_caps_new_simple(
        "application/x-rtp", "media", G_TYPE_STRING, "video",
        "encoding-name", G_TYPE_STRING, "H264",
        "clock-rate", G_TYPE_INT, 90000, nullptr);
    g_object_set(rx_source, "port", static_cast<gint>(rtp_port),
                 "caps", rtp_caps, nullptr);
    gst_caps_unref(rtp_caps);
    g_object_set(rx_parser, "config-interval", -1, nullptr);
    g_object_set(rx_sink, "sync", FALSE, "silent", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(rx_pipeline), rx_source, rx_depay, rx_parser,
                     rx_decoder, rx_convert, rx_sink, nullptr);
    const bool rx_linked = gst_element_link_many(
        rx_source, rx_depay, rx_parser, rx_decoder, rx_convert,
        rx_sink, nullptr);

    g_object_set(tx_source, "is-live", TRUE, "pattern", 1, nullptr);
    g_object_set(tx_encoder, "tune", 0x4, "speed-preset", 1,
                 "bitrate", static_cast<guint>(encoder_kbps),
                 "key-int-max", 60, "bframes", 0, "threads", 2,
                 "byte-stream", TRUE, "aud", TRUE,
                 "rc-lookahead", 0, nullptr);
    g_object_set(tx_parser, "config-interval", -1, nullptr);
    g_object_set(tx_pay, "pt", 96, "mtu", 1400,
                 "config-interval", -1, nullptr);
    g_object_set(tx_sink, "emit-signals", TRUE, "sync", FALSE,
                 "max-buffers", static_cast<guint>(64),
                 "drop", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(tx_pipeline), tx_source, tx_convert,
                     tx_encoder, tx_parser, tx_pay, tx_sink, nullptr);
    bool tx_linked = gst_element_link_many(
        tx_source, tx_convert, tx_encoder, nullptr);
    GstCaps* h264_caps = gst_caps_new_simple(
        "video/x-h264", "profile", G_TYPE_STRING, "baseline", nullptr);
    tx_linked = tx_linked &&
        gst_element_link_filtered(tx_encoder, tx_parser, h264_caps);
    gst_caps_unref(h264_caps);
    tx_linked = tx_linked &&
        gst_element_link_many(tx_parser, tx_pay, tx_sink, nullptr);
    if (!rx_linked || !tx_linked) {
        std::fprintf(stderr, "ERROR: Failed to link demo pipelines\n");
        gst_object_unref(rx_pipeline);
        gst_object_unref(tx_pipeline);
        return EXIT_FAILURE;
    }

    ArrivalQueue arrival_queue;
    ReceiverProbeContext probe_context;
    probe_context.arrivals = &arrival_queue;
    GstPad* rx_source_pad = gst_element_get_static_pad(rx_source, "src");
    GstPad* rx_sink_pad = gst_element_get_static_pad(rx_sink, "sink");
    if (rx_source_pad == nullptr || rx_sink_pad == nullptr) {
        if (rx_source_pad != nullptr) gst_object_unref(rx_source_pad);
        if (rx_sink_pad != nullptr) gst_object_unref(rx_sink_pad);
        gst_object_unref(rx_pipeline);
        gst_object_unref(tx_pipeline);
        return EXIT_FAILURE;
    }
    const gulong rtp_probe_id = gst_pad_add_probe(
        rx_source_pad, GST_PAD_PROBE_TYPE_BUFFER,
        OnRtpPacket, &probe_context, nullptr);
    const gulong frame_probe_id = gst_pad_add_probe(
        rx_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        CountFrame, nullptr, nullptr);
    AppSinkContext sink_context;
    sink_context.transport = &transport;
    const gulong sample_handler = g_signal_connect(
        tx_sink, "new-sample", G_CALLBACK(OnNewSample), &sink_context);
    GstBus* rx_bus = gst_pipeline_get_bus(GST_PIPELINE(rx_pipeline));
    GstBus* tx_bus = gst_pipeline_get_bus(GST_PIPELINE(tx_pipeline));
    bool cleaned_up = false;
    bool sender_stopped = false;
    const auto stop_sender_production = [&]() {
        if (sender_stopped) return;
        sender_stopped = true;
        sink_context.accepting.store(false);
        if (tx_pipeline != nullptr) {
            gst_element_set_state(tx_pipeline, GST_STATE_NULL);
            gst_element_get_state(
                tx_pipeline, nullptr, nullptr, 2U * GST_SECOND);
        }
        if (sample_handler != 0U && tx_sink != nullptr &&
            g_signal_handler_is_connected(tx_sink, sample_handler)) {
            g_signal_handler_disconnect(tx_sink, sample_handler);
        }
    };
    const auto cleanup_pipelines = [&]() {
        if (cleaned_up) return;
        cleaned_up = true;
        stop_sender_production();
        if (rx_pipeline != nullptr) {
            gst_element_set_state(rx_pipeline, GST_STATE_NULL);
            gst_element_get_state(
                rx_pipeline, nullptr, nullptr, 2U * GST_SECOND);
        }
        if (rtp_probe_id != 0U && rx_source_pad != nullptr) {
            gst_pad_remove_probe(rx_source_pad, rtp_probe_id);
        }
        if (frame_probe_id != 0U && rx_sink_pad != nullptr) {
            gst_pad_remove_probe(rx_sink_pad, frame_probe_id);
        }
        if (rx_source_pad != nullptr) gst_object_unref(rx_source_pad);
        if (rx_sink_pad != nullptr) gst_object_unref(rx_sink_pad);
        if (rx_bus != nullptr) gst_object_unref(rx_bus);
        if (tx_bus != nullptr) gst_object_unref(tx_bus);
        if (rx_pipeline != nullptr) gst_object_unref(rx_pipeline);
        if (tx_pipeline != nullptr) gst_object_unref(tx_pipeline);
    };
    if (rtp_probe_id == 0U || frame_probe_id == 0U ||
        sample_handler == 0U || rx_bus == nullptr || tx_bus == nullptr ||
        gst_element_set_state(rx_pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "ERROR: Failed to start receiver pipeline\n");
        cleanup_pipelines();
        return EXIT_FAILURE;
    }
    if (gst_element_get_state(
            rx_pipeline, nullptr, nullptr, 2U * GST_SECOND) ==
        GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "ERROR: Receiver failed to reach PLAYING\n");
        cleanup_pipelines();
        return EXIT_FAILURE;
    }
    if (gst_element_set_state(tx_pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "ERROR: Failed to start sender pipeline\n");
        cleanup_pipelines();
        return EXIT_FAILURE;
    }
    if (gst_element_get_state(
            tx_pipeline, nullptr, nullptr, 2U * GST_SECOND) ==
        GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "ERROR: Sender failed to reach PLAYING\n");
        cleanup_pipelines();
        return EXIT_FAILURE;
    }

    const uint64_t start_us = SteadyMicros();
    uint64_t last_heartbeat_us = start_us;
    uint64_t last_stats_us = start_us;
    uint64_t arrivals_processed = 0U;
    bool loop_error = false;
    std::deque<PacketArrival> arrivals;

    const auto set_pacing = [&](uint32_t kbps) {
        pacing_kbps = std::max(
            kMinKbps, std::min(kbps, kMaxPacingKbps));
        razor_tx.set_pacing_rate(pacing_kbps);
        transport.SetPacingRate(pacing_kbps);
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
    const auto apply_rate = [&](const gcc::RateDecision& decision,
                                gcc::RateReason reason) {
        if (!decision.changed) return;
        const uint32_t previous = target_kbps;
        target_kbps = decision.network_kbps;
        encoder_kbps = decision.encoder_kbps;
        const uint64_t now_ms = SteadyMillis();
        if (gcc::ShouldReportRateTransition(
                reason, has_reported_rate, reported_rate_reason,
                now_ms, last_rate_transition_ms)) {
            std::printf(
                "gcc_transition event=rate reason=%s from_kbps=%u "
                "to_kbps=%u encoder_kbps=%u\n",
                RateReasonName(reason), previous, target_kbps, encoder_kbps);
            std::fflush(stdout);
            has_reported_rate = true;
            reported_rate_reason = reason;
            last_rate_transition_ms = now_ms;
        }
        g_object_set(tx_encoder, "bitrate",
                     static_cast<guint>(encoder_kbps), nullptr);
        if (probe_controller.state() != gcc::ProbeState::Probing) {
            set_pacing(NormalPacing(target_kbps));
        }
    };
    const auto handle_probe = [&](const gcc::ProbeAction& action) {
        if (action.stop) {
            transport.CancelProbeEpoch(probe_epoch);
            set_pacing(NormalPacing(target_kbps));
        }
        if (action.accept) {
            validated_probe_kbps = action.accepted_kbps;
            probe_delivery.Finish();
            apply_rate(
                rate_policy.Apply(
                    {gcc::RateReason::ProbeAccepted, action.accepted_kbps,
                     rtt_ms, SteadyMillis()}),
                gcc::RateReason::ProbeAccepted);
            razor_tx.set_bitrates(kMinKbps, target_kbps, kMaxKbps);
            set_pacing(NormalPacing(target_kbps));
        }
        if (action.start) {
            probe_bytes_enqueued = 0U;
            probe_bytes_sent = 0U;
            probe_epoch = transport.BeginProbeEpoch();
            gcc::SentHistoryWatermark watermark;
            probe_sent_baseline =
                transport.GetSentHistoryWatermark(watermark)
                    ? watermark.cumulative_sent_bytes
                    : 0U;
            probe_delivery.Start(action.probe_kbps);
            set_pacing(action.probe_kbps);
        }
        if (probe_controller.state() == gcc::ProbeState::Failed) {
            transport.CancelProbeEpoch(probe_epoch);
            probe_delivery.Finish();
            set_pacing(NormalPacing(target_kbps));
        }
    };
    const auto process_control_events = [&]() {
        for (size_t round = 0U; round < 1024U; ++round) {
            size_t processed = 0U;
            while (!deferred_sends.empty() || !pending_rates.empty()) {
                if (++processed > kMaxControlEvents) {
                    control_failed = true;
                    return false;
                }
                if (!deferred_sends.empty()) {
                    const gcc::SentPacket sent = deferred_sends.front();
                    deferred_sends.pop_front();
                    razor_tx.on_send(sent.transport_sequence, sent.size);
                    if (sent.probe) {
                        ++probe_sent;
                        probe_bytes_sent += sent.size;
                    } else {
                        ++media_sent;
                    }
                    if (sent.probe_cluster) {
                        ++probe_cluster_sent;
                        probe_delivery.OnProbePacketSent(sent);
                        MergeProbeAction(
                            deferred_probe_action,
                            probe_controller.OnProbePacketSent(
                                sent.size, sent.send_time_us / 1000U));
                    }
                    continue;
                }
                advance_probe_send_progress();
                const RawRateEvent event = pending_rates.front();
                pending_rates.pop_front();
                if (event.probe_related) {
                    continue;
                }
                if (!gcc::ShouldApplyRawRateEvent(
                        event.proposed_kbps, target_kbps,
                        event.delay_overusing)) {
                    continue;
                }
                const gcc::RateReason reason =
                    event.proposed_kbps < target_kbps
                        ? (event.loss_q8 > 25U
                               ? gcc::RateReason::Loss
                               : gcc::RateReason::DelayOveruse)
                        : gcc::RateReason::Normal;
                const gcc::RateDecision decision =
                    rate_policy.Apply(
                        {reason, event.proposed_kbps,
                         event.rtt_ms, event.now_ms});
                apply_rate(decision, reason);
                if (reason == gcc::RateReason::Loss &&
                    decision.changed) {
                    ++applied_loss_decisions;
                }
            }
            const gcc::ProbeAction action = deferred_probe_action;
            deferred_probe_action = gcc::ProbeAction{};
            if (action.start || action.stop || action.accept) {
                handle_probe(action);
                report_probe_transition();
                continue;
            }
            report_probe_transition();
            return true;
        }
        control_failed = true;
        return false;
    };
    const auto flush_transport = [&]() {
        transport.FlushPaced(
            [&](const gcc::SentPacket& sent) {
                try {
                    deferred_sends.push_back(sent);
                } catch (...) {
                    control_failed = true;
                    running.store(false);
                }
            });
    };
    const auto process_arrivals = [&]() {
        arrival_queue.Drain(arrivals);
        for (const PacketArrival& arrival : arrivals) {
            loss_tracker.on_packet(arrival.transport_sequence);
            last_processed_sequence = arrival.transport_sequence;
            last_processed_arrival_ms = arrival.arrival_ms;
            has_processed = true;
            razor_rx.on_received(
                arrival.transport_sequence, arrival.arrival_ms,
                arrival.size);
            ++arrivals_processed;
        }
        arrivals.clear();
    };
    const auto print_stats = [&]() {
        const razor::SenderStats stats = razor_tx.stats();
        gcc::SentHistoryWatermark watermark;
        const uint64_t wire_bytes =
            transport.GetSentHistoryWatermark(watermark)
                ? watermark.cumulative_sent_bytes
                : 0U;
        std::printf(
            "demo_stats encoder_kbps=%u target_kbps=%u pacing_kbps=%u "
            "acked_kbps=%u probe_delivery_kbps=%u "
            "validated_probe_kbps=%u loss_q8=%u "
            "rtt_ms=%u media_sent=%llu "
            "probe_sent=%llu probe_cluster_sent=%llu wire_bytes=%llu "
            "feedback=%llu "
            "feedback_matched=%llu "
            "frames=%llu arrivals=%llu arrival_overflow=%llu "
            "media_reorder_drops=%llu pacer_queue_ms=%lld "
            "ingress_bytes=%zu probe_state=%s\n",
            encoder_kbps, target_kbps, pacing_kbps,
            stats.acked_bitrate_kbps, probe_delivery.estimate_kbps(),
            validated_probe_kbps,
            static_cast<unsigned>(loss_q8),
            rtt_ms, static_cast<unsigned long long>(media_sent),
            static_cast<unsigned long long>(probe_sent),
            static_cast<unsigned long long>(probe_cluster_sent),
            static_cast<unsigned long long>(wire_bytes),
            static_cast<unsigned long long>(feedback_count),
            static_cast<unsigned long long>(stats.matched_feedback_packets),
            static_cast<unsigned long long>(frames.load()),
            static_cast<unsigned long long>(arrivals_processed),
            static_cast<unsigned long long>(arrival_queue.overflow()),
            static_cast<unsigned long long>(
                probe_context.media_reorder_drops.load()),
            static_cast<long long>(stats.pacer_queue_ms),
            transport.ingress_bytes(),
            ProbeStateName(probe_controller.state()));
        std::fflush(stdout);
    };

    while (running.load()) {
        const uint64_t now_us = SteadyMicros();
        const uint64_t now_ms = now_us / 1000U;
        if (now_us - start_us >=
            static_cast<uint64_t>(config.duration) * UINT64_C(1000000)) {
            break;
        }
        const BusEvent rx_event = PollBus(rx_bus, "receiver");
        const BusEvent tx_event = PollBus(tx_bus, "sender");
        if (rx_event != BusEvent::None ||
            tx_event != BusEvent::None) {
            loop_error =
                rx_event == BusEvent::Error ||
                tx_event == BusEvent::Error;
            break;
        }

        process_arrivals();

        gcc::FeedbackPacket feedback_packet;
        const size_t feedback_budget = std::min(
            sender_feedback.queued_feedback_count(),
            kMaxFeedbackPerLoop);
        for (size_t i = 0U;
             i < feedback_budget &&
             sender_feedback.poll(feedback_packet); ++i) {
            ++feedback_count;
            loss_q8 = feedback_packet.loss_q8;
            const uint64_t loss_decisions_before =
                applied_loss_decisions;
            uint64_t send_time_us = 0U;
            uint64_t& consumed =
                consumed_send_times[feedback_packet.sequence];
            if (transport.LookupSendTime(
                    feedback_packet.sequence, send_time_us) &&
                send_time_us != consumed) {
                consumed = send_time_us;
                const uint64_t feedback_us = SteadyMicros();
                if (feedback_us >= send_time_us &&
                    feedback_us - send_time_us <= UINT64_C(5000000)) {
                    uint32_t measured = 0U;
                    if (gcc::ComputeNetworkRtt(
                            Low32Millis(send_time_us),
                            Low32Millis(feedback_us),
                            feedback_packet.receive_time_ms,
                            feedback_packet.feedback_time_ms, measured) &&
                        measured >= 1U && measured <= 5000U) {
                        rtt_ms = measured;
                        razor_tx.update_rtt(rtt_ms);
                        process_control_events();
                    }
                }
            }
            gcc::SentPacketInfo feedback_info;
            bool probe_related_batch =
                probe_delivery.rate_events_guarded();
            if (transport.LookupSentPacket(
                    feedback_packet.sequence, feedback_info)) {
                probe_related_batch =
                    probe_delivery.BeginFeedback(feedback_info);
                scoped_probe_feedback = probe_related_batch;
                probe_delivery.ObserveFeedback(
                    feedback_info, feedback_packet.receive_time_ms);
            } else {
                gcc::SentHistoryWatermark watermark;
                transport.GetSentHistoryWatermark(watermark);
                probe_related_batch =
                    probe_delivery.BeginUnmatchedFeedback(watermark);
                scoped_probe_feedback = probe_related_batch;
            }
            if (!feedback_packet.razor_payload.empty()) {
                razor_tx.on_feedback(
                    feedback_packet.razor_payload.data(),
                    static_cast<int>(
                        feedback_packet.razor_payload.size()));
                process_control_events();
            }
            scoped_probe_feedback = false;
            // Keep the emergency fallback armed when razor handled this
            // feedback itself; it may act once on a later high-loss feedback
            // only if razor no longer supplies a decrease.
            const gcc::OuterLossDecision outer_loss =
                outer_loss_backoff.Evaluate(
                    loss_q8, target_kbps, rtt_ms, now_ms,
                    !probe_related_batch,
                    applied_loss_decisions != loss_decisions_before);
            if (outer_loss.changed) {
                const gcc::RateDecision decision =
                    rate_policy.Apply(
                        {gcc::RateReason::Loss,
                         outer_loss.proposed_kbps, rtt_ms, now_ms});
                apply_rate(decision, gcc::RateReason::Loss);
                if (decision.changed) ++applied_loss_decisions;
            }
            const razor::SenderStats stats = razor_tx.stats();
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
            deferred_probe_action,
            probe_controller.OnPacerQueueDelay(
                std::max(
                    transport.PacingQueueDelayMs(),
                    static_cast<uint32_t>(std::min<int64_t>(
                        razor_tx.pacer_queue_ms(),
                        std::numeric_limits<uint32_t>::max()))),
                now_ms));
        MergeProbeAction(
            deferred_probe_action, probe_controller.OnTimer(now_ms));
        process_control_events();
        gcc::RtpHeader media_header;
        if (transport.latest_media_header(media_header)) {
            MergeProbeAction(
                deferred_probe_action,
                probe_controller.OnStableTarget(target_kbps, now_ms));
            process_control_events();
        }
        if (probe_controller.state() == gcc::ProbeState::Probing &&
            transport.latest_media_header(media_header)) {
            const uint64_t desired = gcc::ProbeBudgetBytes(
                probe_controller.active_probe_kbps(),
                probe_controller.probe_start_ms(), now_ms,
                probe_config.duration_ms);
            gcc::SentHistoryWatermark watermark;
            const uint64_t aggregate_sent =
                transport.GetSentHistoryWatermark(watermark) &&
                        watermark.cumulative_sent_bytes >=
                            probe_sent_baseline
                    ? watermark.cumulative_sent_bytes -
                          probe_sent_baseline
                    : 0U;
            const uint64_t queued_probe =
                probe_bytes_enqueued >= probe_bytes_sent
                    ? probe_bytes_enqueued - probe_bytes_sent
                    : 0U;
            uint64_t deficit = gcc::ProbePaddingDeficitBytes(
                desired, aggregate_sent, queued_probe);
            while (deficit >= kMinProbeBytes) {
                const size_t packet_size = static_cast<size_t>(
                    std::min<uint64_t>(deficit, kMaxProbeBytes));
                std::vector<uint8_t> probe = gcc::BuildProbeRtpPacket(
                    media_header, 0U, packet_size);
                if (probe.empty() ||
                    !transport.EnqueueProbe(
                        std::move(probe), probe_epoch)) break;
                probe_bytes_enqueued += packet_size;
                deficit -= packet_size;
            }
        }
        transport.DrainIngress(
            [&](uint32_t packet_id, size_t size) {
                const bool accepted =
                    razor_tx.add_packet(packet_id, false, size);
                process_control_events();
                return accepted;
            });
        flush_transport();
        process_control_events();

        const uint64_t heartbeat_us = SteadyMicros();
        if (heartbeat_us - last_heartbeat_us >= 5000U) {
            razor_rx.heartbeat();
            advance_probe_send_progress();
            razor_tx.heartbeat();
            flush_transport();
            process_control_events();
            last_heartbeat_us = heartbeat_us;
        }
        if (now_us - last_stats_us >= UINT64_C(1000000)) {
            print_stats();
            last_stats_us = now_us;
        }
        if (control_failed) {
            loop_error = true;
            break;
        }
        transport.WaitForPacingOrIngress(2000U);
    }

    handle_probe(probe_controller.Cancel(SteadyMillis()));
    report_probe_transition();
    stop_sender_production();
    transport.DiscardPending();
    razor_tx.set_pacing_rate(kMaxPacingKbps);
    process_control_events();
    const uint64_t drain_deadline = SteadyMicros() + kDrainTimeoutUs;
    bool drained = false;
    while (!control_failed && SteadyMicros() < drain_deadline) {
        advance_probe_send_progress();
        razor_tx.heartbeat();
        flush_transport();
        process_control_events();
        process_arrivals();
        razor_rx.heartbeat();
        if (transport.ingress_bytes() == 0U &&
            transport.pending_packets() == 0U &&
            deferred_sends.empty() && pending_rates.empty() &&
            razor_tx.pacer_queue_ms() == 0) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!drained) {
        std::fprintf(stderr, "ERROR: Timed out draining demo sender\n");
        loop_error = true;
    }

    cleanup_pipelines();
    process_arrivals();
    razor_rx.heartbeat();
    sender_feedback.stop();
    receiver_feedback.stop();
    print_stats();

    return loop_error || control_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
