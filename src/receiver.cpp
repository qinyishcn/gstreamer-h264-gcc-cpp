/**
 * H.264 + GCC video receiver.
 *
 * The GStreamer streaming thread only parses/re-writes RTP and queues packet
 * arrivals. All razor calls and feedback generation run on the main control
 * thread.
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "feedback_channel.h"
#include "feedback_packet.h"
#include "razor_wrapper.h"
#include "rtp_packet.h"

namespace {

constexpr size_t kMaxArrivalEvents = 4096U;

std::atomic<bool> running{true};
std::atomic<uint64_t> frame_count{0U};

void SignalHandler(int) {
    running.store(false);
}

uint32_t SteadyMillisLow32() {
    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return static_cast<uint32_t>(now_ms & UINT64_C(0xffffffff));
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

struct ReceiverConfig {
    int rtp_port = 5004;
    const char* sender_host = "127.0.0.1";
};

void ParseArgs(int argc, char* argv[], ReceiverConfig& config) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--port") == 0 &&
            index + 1 < argc) {
            config.rtp_port =
                ParseBoundedInt(argv[++index], 1, 65534, config.rtp_port);
        } else if (std::strcmp(argv[index], "--sender-host") == 0 &&
                   index + 1 < argc) {
            config.sender_host = argv[++index];
        }
    }
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
            if (queue_.size() >= kMaxArrivalEvents) {
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

    uint64_t overflow() const noexcept {
        return overflow_.load();
    }

private:
    std::mutex mutex_;
    std::deque<PacketArrival> queue_;
    std::atomic<uint64_t> overflow_{0U};
};

struct ReceiverProbeContext {
    ArrivalQueue* arrivals;
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
        if (buffer == nullptr) {
            return GST_PAD_PROBE_DROP;
        }

        GstMapInfo read_map = GST_MAP_INFO_INIT;
        if (!gst_buffer_map(buffer, &read_map, GST_MAP_READ)) {
            return GST_PAD_PROBE_DROP;
        }

        gcc::RtpHeader header;
        const bool parsed = gcc::ParseRtpHeader(
            read_map.data, read_map.size, header);
        const bool probe =
            parsed && gcc::IsProbeRtpPacket(
                          read_map.data, read_map.size);
        const size_t packet_size = read_map.size;
        gst_buffer_unmap(buffer, &read_map);
        if (!parsed) {
            return GST_PAD_PROBE_OK;
        }

        const PacketArrival arrival{
            header.sequence, SteadyMillisLow32(), packet_size, probe};
        context->arrivals->Push(arrival);
        if (probe) {
            context->translator.OnProbe(header.sequence);
            return GST_PAD_PROBE_DROP;
        }

        uint16_t media_sequence = 0U;
        if (!context->translator.TranslateMedia(
                header.sequence, media_sequence)) {
            context->media_reorder_drops.fetch_add(
                1U, std::memory_order_relaxed);
            return GST_PAD_PROBE_DROP;
        }
        if (media_sequence != header.sequence) {
            GstBuffer* writable = gst_buffer_make_writable(buffer);
            if (writable == nullptr) {
                return GST_PAD_PROBE_DROP;
            }
            GST_PAD_PROBE_INFO_DATA(info) = writable;

            GstMapInfo write_map = GST_MAP_INFO_INIT;
            if (!gst_buffer_map(writable, &write_map, GST_MAP_READWRITE)) {
                return GST_PAD_PROBE_DROP;
            }
            const bool rewritten = gcc::RewriteRtpSequence(
                write_map.data, write_map.size, media_sequence);
            gst_buffer_unmap(writable, &write_map);
            if (!rewritten) {
                return GST_PAD_PROBE_DROP;
            }
        }

        return GST_PAD_PROBE_OK;
    } catch (...) {
        return GST_PAD_PROBE_DROP;
    }
}

GstPadProbeReturn CountFrame(
    GstPad*, GstPadProbeInfo*, gpointer) {
    frame_count.fetch_add(1U, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

void UnrefCreatedElements(
    GstElement* pipeline, const std::vector<GstElement*>& elements) {
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
    ReceiverConfig config;
    ParseArgs(argc, argv, config);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    gst_init(&argc, &argv);

    const uint16_t rtp_port =
        static_cast<uint16_t>(config.rtp_port);
    const uint16_t feedback_port =
        static_cast<uint16_t>(config.rtp_port + 1);

    std::printf("============================================================\n");
    std::printf("H.264 + GCC Video Receiver (razor GCC v4.0)\n");
    std::printf("============================================================\n");
    std::printf("  RTP port:       %u\n", rtp_port);
    std::printf("  Feedback port:  %s:%u\n",
                config.sender_host, feedback_port);
    std::printf("============================================================\n\n");

    gcc::FeedbackChannel feedback(
        config.sender_host, feedback_port, false);
    if (!feedback.start()) {
        std::fprintf(stderr, "ERROR: Failed to start feedback channel\n");
        return EXIT_FAILURE;
    }

    gcc::LossTracker loss_tracker;
    bool has_processed_arrival = false;
    uint16_t last_processed_sequence = 0U;
    uint32_t last_processed_arrival_ms = 0U;
    razor::Receiver razor_receiver(
        [&](const uint8_t* data, int size) {
            try {
                if (!has_processed_arrival || size < 0 ||
                    static_cast<size_t>(size) >
                        gcc::kMaxFeedbackPayload ||
                    (size > 0 && data == nullptr)) {
                    return;
                }
                gcc::FeedbackPacket packet;
                packet.sequence = last_processed_sequence;
                packet.loss_q8 = loss_tracker.get_loss_q8();
                packet.receive_time_ms =
                    last_processed_arrival_ms;
                packet.feedback_time_ms = SteadyMillisLow32();
                if (size > 0) {
                    packet.razor_payload.assign(data, data + size);
                }
                feedback.send_feedback(packet);
            } catch (...) {
                // Never let allocation or socket exceptions cross the C ABI.
            }
        });
    if (!razor_receiver.is_valid()) {
        std::fprintf(stderr, "ERROR: Failed to create razor receiver\n");
        feedback.stop();
        return EXIT_FAILURE;
    }

    GstElement* pipeline = gst_pipeline_new("receiver-pipeline");
    GstElement* udp_source =
        gst_element_factory_make("udpsrc", "rtp-source");
    GstElement* depayloader =
        gst_element_factory_make("rtph264depay", "rtp-depayloader");
    GstElement* parser =
        gst_element_factory_make("h264parse", "h264-parser");
    GstElement* decoder =
        gst_element_factory_make("avdec_h264", "h264-decoder");
    GstElement* convert =
        gst_element_factory_make("videoconvert", "video-convert");
    GstElement* sink =
        gst_element_factory_make("fakesink", "video-sink");
    const std::vector<GstElement*> created = {
        udp_source, depayloader, parser, decoder, convert, sink};
    if (pipeline == nullptr || udp_source == nullptr ||
        depayloader == nullptr || parser == nullptr ||
        decoder == nullptr || convert == nullptr || sink == nullptr) {
        std::fprintf(stderr, "ERROR: Failed to create receiver elements\n");
        UnrefCreatedElements(pipeline, created);
        feedback.stop();
        return EXIT_FAILURE;
    }

    GstCaps* rtp_caps = gst_caps_new_simple(
        "application/x-rtp", "media", G_TYPE_STRING, "video",
        "encoding-name", G_TYPE_STRING, "H264",
        "clock-rate", G_TYPE_INT, 90000, nullptr);
    g_object_set(
        udp_source, "port", static_cast<gint>(rtp_port),
        "caps", rtp_caps, nullptr);
    gst_caps_unref(rtp_caps);
    g_object_set(parser, "config-interval", -1, nullptr);
    g_object_set(sink, "sync", FALSE, "silent", TRUE, nullptr);

    gst_bin_add_many(
        GST_BIN(pipeline), udp_source, depayloader, parser, decoder,
        convert, sink, nullptr);
    if (!gst_element_link_many(
            udp_source, depayloader, parser, decoder, convert, sink,
            nullptr)) {
        std::fprintf(stderr, "ERROR: Failed to link receiver pipeline\n");
        gst_object_unref(pipeline);
        feedback.stop();
        return EXIT_FAILURE;
    }

    ArrivalQueue arrival_queue;
    ReceiverProbeContext probe_context{&arrival_queue, {}};
    GstPad* source_pad =
        gst_element_get_static_pad(udp_source, "src");
    GstPad* sink_pad =
        gst_element_get_static_pad(sink, "sink");
    if (source_pad == nullptr || sink_pad == nullptr) {
        std::fprintf(stderr, "ERROR: Failed to get receiver pads\n");
        if (source_pad != nullptr) {
            gst_object_unref(source_pad);
        }
        if (sink_pad != nullptr) {
            gst_object_unref(sink_pad);
        }
        gst_object_unref(pipeline);
        feedback.stop();
        return EXIT_FAILURE;
    }
    const gulong rtp_probe_id = gst_pad_add_probe(
        source_pad, GST_PAD_PROBE_TYPE_BUFFER,
        OnRtpPacket, &probe_context, nullptr);
    const gulong frame_probe_id = gst_pad_add_probe(
        sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        CountFrame, nullptr, nullptr);
    if (rtp_probe_id == 0U || frame_probe_id == 0U) {
        std::fprintf(stderr, "ERROR: Failed to install receiver probes\n");
        if (rtp_probe_id != 0U) {
            gst_pad_remove_probe(source_pad, rtp_probe_id);
        }
        if (frame_probe_id != 0U) {
            gst_pad_remove_probe(sink_pad, frame_probe_id);
        }
        gst_object_unref(source_pad);
        gst_object_unref(sink_pad);
        gst_object_unref(pipeline);
        feedback.stop();
        return EXIT_FAILURE;
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    if (bus == nullptr ||
        gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "ERROR: Failed to start receiver pipeline\n");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_element_get_state(
            pipeline, nullptr, nullptr, 2U * GST_SECOND);
        if (rtp_probe_id != 0U) {
            gst_pad_remove_probe(source_pad, rtp_probe_id);
        }
        if (frame_probe_id != 0U) {
            gst_pad_remove_probe(sink_pad, frame_probe_id);
        }
        if (bus != nullptr) {
            gst_object_unref(bus);
        }
        gst_object_unref(source_pad);
        gst_object_unref(sink_pad);
        gst_object_unref(pipeline);
        feedback.stop();
        return EXIT_FAILURE;
    }
    gst_element_get_state(pipeline, nullptr, nullptr, 2U * GST_SECOND);

    std::printf("  Receiver PLAYING on port %u\n", rtp_port);
    std::printf("  Razor GCC feedback active\n\n");
    std::fflush(stdout);

    uint64_t total_rx_packets = 0U;
    bool loop_error = false;
    auto last_stats = std::chrono::steady_clock::now();
    auto last_heartbeat = last_stats;
    std::deque<PacketArrival> arrivals;

    const auto process_arrivals = [&]() {
        arrival_queue.Drain(arrivals);
        for (const PacketArrival& arrival : arrivals) {
            loss_tracker.on_packet(arrival.transport_sequence);
            last_processed_sequence = arrival.transport_sequence;
            last_processed_arrival_ms = arrival.arrival_ms;
            has_processed_arrival = true;
            razor_receiver.on_received(
                arrival.transport_sequence, arrival.arrival_ms,
                arrival.size);
            ++total_rx_packets;
        }
        arrivals.clear();
    };

    while (running.load()) {
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

        process_arrivals();
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_heartbeat)
                .count() >= 5) {
            razor_receiver.heartbeat();
            last_heartbeat = now;
        }
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_stats)
                .count() >= 1) {
            std::printf(
                "receiver_stats frames=%llu packets=%llu "
                "arrival_overflow=%llu media_reorder_drops=%llu "
                "last_sequence=%u\n",
                static_cast<unsigned long long>(
                    frame_count.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(total_rx_packets),
                static_cast<unsigned long long>(
                    arrival_queue.overflow()),
                static_cast<unsigned long long>(
                    probe_context.media_reorder_drops.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned>(last_processed_sequence));
            std::fflush(stdout);
            last_stats = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_element_get_state(pipeline, nullptr, nullptr, 2U * GST_SECOND);
    if (rtp_probe_id != 0U) {
        gst_pad_remove_probe(source_pad, rtp_probe_id);
    }
    if (frame_probe_id != 0U) {
        gst_pad_remove_probe(sink_pad, frame_probe_id);
    }
    process_arrivals();
    razor_receiver.heartbeat();
    feedback.stop();

    std::printf(
        "receiver_stats frames=%llu packets=%llu arrival_overflow=%llu "
        "media_reorder_drops=%llu last_sequence=%u\n",
        static_cast<unsigned long long>(
            frame_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(total_rx_packets),
        static_cast<unsigned long long>(arrival_queue.overflow()),
        static_cast<unsigned long long>(
            probe_context.media_reorder_drops.load(
                std::memory_order_relaxed)),
        static_cast<unsigned>(last_processed_sequence));
    std::fflush(stdout);

    gst_object_unref(source_pad);
    gst_object_unref(sink_pad);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return loop_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
