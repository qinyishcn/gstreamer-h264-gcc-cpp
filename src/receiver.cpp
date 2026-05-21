/**
 * H.264 + GCC Video Receiver (GStreamer C++ + razor GCC)
 *
 * Uses razor library for standard GCC bandwidth estimation.
 * Razor generates feedback; we send it back via UDP feedback channel.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

#include <gst/gst.h>
#include "gst_compat.h"
#include "feedback_channel.h"
#include "razor_wrapper.h"

static std::atomic<bool> running{true};
static std::atomic<uint64_t> frame_count{0};
static void signal_handler(int) { running.store(false); }

static bool parse_rtp_seq(const uint8_t* data, size_t len, uint16_t& seq) {
    if (len < 12) return false;
    seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    return true;
}

struct ReceiverProbeData {
    razor::Receiver* razor_rx;
    uint16_t* last_seq;
    uint64_t* total_rx_packets;
};

static GstPadProbeReturn rtp_in_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
    auto* d = static_cast<ReceiverProbeData*>(user_data);
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    uint16_t seq = 0;
    if (parse_rtp_seq(map.data, map.size, seq)) {
        *d->last_seq = seq;
        (*d->total_rx_packets)++;
        // Notify razor of received packet
        // timestamp = 0 (razor computes inter-arrival internally)
        uint32_t ts = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
        d->razor_rx->on_received(seq, ts, map.size);
    }

    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn count_probe(GstPad*, GstPadProbeInfo*, gpointer) {
    frame_count.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

struct ReceiverConfig {
    int rtp_port = 5004;
    const char* sender_host = "127.0.0.1";
};

static void parse_args(int argc, char* argv[], ReceiverConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i+1 < argc) cfg.rtp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sender-host") == 0 && i+1 < argc) cfg.sender_host = argv[++i];
    }
}

int main(int argc, char* argv[]) {
    ReceiverConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    gst_init(&argc, &argv);

    int feedback_port = cfg.rtp_port + 1;

    printf("============================================================\n");
    printf("H.264 + GCC Video Receiver (razor GCC v3.0)\n");
    printf("============================================================\n");
    printf("  RTP port:       %d\n", cfg.rtp_port);
    printf("  Feedback port:  %s:%d\n", cfg.sender_host, feedback_port);
    printf("============================================================\n\n");

    // ── Feedback channel (sends to sender) ──
    gcc::FeedbackChannel feedback(cfg.sender_host, feedback_port, false);
    if (!feedback.start()) {
        fprintf(stderr, "ERROR: Failed to start feedback channel\n");
        return 1;
    }

    // ── Razor receiver (generates GCC feedback) ──
    // The send_feedback callback is called by razor when it has feedback ready
    razor::Receiver razor_rx([&](const uint8_t* data, int size) {
        // Send razor's feedback to sender via UDP
        // We need the latest seq and loss info
        // razor provides the feedback payload; we wrap it in our protocol
        feedback.send_feedback(0, 0, data, size);
    });
    if (!razor_rx.is_valid()) {
        fprintf(stderr, "ERROR: Failed to create razor receiver\n");
        return 1;
    }

    // ── GStreamer receiver pipeline ──
    GstElement *pipeline, *udpsrc, *depay, *parser, *decoder, *convert, *sink;
    pipeline = gst_pipeline_new("receiver-pipeline");
    udpsrc   = gst_element_factory_make("udpsrc",         "rtp-src");
    depay    = gst_element_factory_make("rtph264depay",   "rtp-depay");
    parser   = gst_element_factory_make("h264parse",      "h264-parser");
    decoder  = gst_element_factory_make("avdec_h264",     "h264-decoder");
    convert  = gst_element_factory_make("videoconvert",   "video-convert");
    sink     = gst_element_factory_make("fakesink",       "video-sink");

    if (!pipeline || !udpsrc || !depay || !parser || !decoder || !convert || !sink) {
        fprintf(stderr, "ERROR: Failed to create receiver elements\n");
        return 1;
    }

    GstCaps* rtp_caps = gst_caps_new_simple("application/x-rtp",
        "media", G_TYPE_STRING, "video",
        "encoding-name", G_TYPE_STRING, "H264",
        "clock-rate", G_TYPE_INT, 90000, nullptr);
    g_object_set(udpsrc, "port", cfg.rtp_port, "caps", rtp_caps, nullptr);
    gst_caps_unref(rtp_caps);
    g_object_set(parser, "config-interval", -1, nullptr);
    g_object_set(sink, "sync", FALSE, "silent", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), udpsrc, depay, parser, decoder, convert, sink, nullptr);
    if (!gst_element_link_many(udpsrc, depay, parser, decoder, convert, sink, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link receiver pipeline\n");
        return 1;
    }

    // ── Pad probes ──
    uint16_t last_rtp_seq = 0;
    uint64_t total_rx_packets = 0;
    ReceiverProbeData probe_data = { &razor_rx, &last_rtp_seq, &total_rx_packets };

    GstPad* udpsrc_src = gst_element_get_static_pad(udpsrc, "src");
    if (udpsrc_src) {
        gst_pad_add_probe(udpsrc_src, GST_PAD_PROBE_TYPE_BUFFER, rtp_in_probe, &probe_data, nullptr);
        gst_object_unref(udpsrc_src);
    }
    GstPad* sink_pad = gst_element_get_static_pad(sink, "sink");
    if (sink_pad) {
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe, nullptr, nullptr);
        gst_object_unref(sink_pad);
    }

    // ── Start ──
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    printf("  Receiver PLAYING on port %d\n", cfg.rtp_port);
    printf("  Razor GCC feedback active\n\n");

    auto start = std::chrono::steady_clock::now();
    auto last_heartbeat = start;
    int last_sec = -1;

    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();

        GstMessage* msg = gst_bus_pop_filtered(bus, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* err = nullptr; gchar* debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                fprintf(stderr, "Error: %s\n", err->message);
                g_error_free(err); g_free(debug);
                running.store(false);
            }
            gst_message_unref(msg);
            break;
        }

        // Razor heartbeat (every ~5ms)
        auto hb_now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(hb_now - last_heartbeat).count() >= 0.005) {
            razor_rx.heartbeat();
            last_heartbeat = hb_now;
        }

        // Print stats every second
        int sec = static_cast<int>(elapsed);
        if (sec != last_sec && sec > 0) {
            last_sec = sec;
            printf("  [%3ds] RX:%lu frames  Packets:%lu\n",
                   sec,
                   (unsigned long)frame_count.load(std::memory_order_relaxed),
                   (unsigned long)total_rx_packets);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    printf("\nStopping receiver...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    feedback.stop();
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    printf("  Receiver stopped — %lu frames, %lu packets\n",
           (unsigned long)frame_count.load(),
           (unsigned long)total_rx_packets);
    return 0;
}
