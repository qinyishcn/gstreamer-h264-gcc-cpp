/**
 * H.264 + GCC Video Receiver (GStreamer C++)
 *
 * Features:
 *   - RTP sequence number extraction via pad probe
 *   - Packet loss calculation from sequence number gaps
 *   - Periodic feedback reports to sender via UDP channel
 *   - Frame counting via sink pad probe
 *
 * Usage:
 *   ./receiver [--port PORT] [--sender-host HOST] [--output fakesink|display]
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

static std::atomic<bool> running{true};
static std::atomic<uint64_t> frame_count{0};

static void signal_handler(int /*sig*/) {
    running.store(false);
}

// ── RTP header parsing ──────────────────────────────────────────────

static bool parse_rtp_seq(const uint8_t* data, size_t len, uint16_t& seq) {
    if (len < 12) return false;
    seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    return true;
}

// ── Packet loss tracker ─────────────────────────────────────────────

struct LossTracker {
    uint32_t received    = 0;
    uint32_t expected    = 0;
    uint16_t last_seq    = 0;
    bool     initialized = false;

    void on_packet(uint16_t seq) {
        if (!initialized) {
            last_seq = seq;
            initialized = true;
            received = 1;
            expected = 1;
            return;
        }
        uint32_t diff = static_cast<uint32_t>(static_cast<uint16_t>(seq - last_seq));
        if (diff > 0 && diff < 0x8000) {
            expected += diff;
            received += 1;
            last_seq = seq;
        } else if (diff == 0) {
            received += 1;
        }
    }

    double loss_fraction() const {
        if (expected == 0) return 0.0;
        if (received >= expected) return 0.0;
        return 1.0 - static_cast<double>(received) / static_cast<double>(expected);
    }

    void reset() { received = 0; expected = 0; initialized = false; }
};

// ── Pad probe: extract RTP seq + feed feedback channel ───────────────

struct ReceiverProbeData {
    gcc::FeedbackChannel* channel;
    LossTracker*          loss_tracker;
};

static GstPadProbeReturn
rtp_in_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
    auto* data = reinterpret_cast<ReceiverProbeData*>(user_data);
    if (!data) return GST_PAD_PROBE_OK;

    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    uint16_t seq = 0;
    if (parse_rtp_seq(map.data, map.size, seq)) {
        uint64_t arrival_us = gcc::get_timestamp_us();

        if (data->channel) {
            data->channel->on_rtp_packet(seq, arrival_us);
        }
        if (data->loss_tracker) {
            data->loss_tracker->on_packet(seq);
        }
    }

    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

// ── Frame counter probe ─────────────────────────────────────────────

static GstPadProbeReturn
count_probe(GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer /*user_data*/) {
    frame_count.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

// ── Config ──────────────────────────────────────────────────────────

struct ReceiverConfig {
    int         rtp_port    = 5004;
    const char* sender_host = "127.0.0.1";
    const char* output      = "fakesink";
};

static void parse_args(int argc, char* argv[], ReceiverConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) cfg.rtp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sender-host") == 0 && i + 1 < argc) cfg.sender_host = argv[++i];
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) cfg.output = argv[++i];
    }
}

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    ReceiverConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    gst_init(&argc, &argv);

    int feedback_port = cfg.rtp_port + 1;

    printf("============================================================\n");
    printf("H.264 + GCC Video Receiver (with feedback)\n");
    printf("============================================================\n");
    printf("  RTP port:       %d\n", cfg.rtp_port);
    printf("  Feedback port:  %s:%d\n", cfg.sender_host, feedback_port);
    printf("  Output:         %s\n", cfg.output);
    printf("============================================================\n\n");

    // ── Feedback Channel (receiver side: sends reports, no bind) ──
    gcc::FeedbackChannel feedback(cfg.sender_host, feedback_port, false);
    if (!feedback.start()) {
        fprintf(stderr, "ERROR: Failed to start feedback channel\n");
        return 1;
    }

    LossTracker loss_tracker;

    // ── Build receiver pipeline (element-by-element) ──
    GstElement *pipeline, *udpsrc, *depay, *parser, *decoder, *convert, *sink;
    pipeline = gst_pipeline_new("receiver-pipeline");
    udpsrc   = gst_element_factory_make("udpsrc",         "rtp-src");
    depay    = gst_element_factory_make("rtph264depay",   "rtp-depay");
    parser   = gst_element_factory_make("h264parse",      "h264-parser");
    decoder  = gst_element_factory_make("avdec_h264",     "h264-decoder");
    convert  = gst_element_factory_make("videoconvert",   "video-convert");
    sink     = gst_element_factory_make(cfg.output,       "video-sink");

    if (!pipeline || !udpsrc || !depay || !parser || !decoder || !convert || !sink) {
        fprintf(stderr, "ERROR: Failed to create receiver elements\n");
        feedback.stop();
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

    gst_bin_add_many(GST_BIN(pipeline),
        udpsrc, depay, parser, decoder, convert, sink, nullptr);

    if (!gst_element_link_many(udpsrc, depay, parser, decoder, convert, sink, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link receiver pipeline\n");
        feedback.stop();
        gst_object_unref(pipeline);
        return 1;
    }

    // ── RTP input pad probe ──
    ReceiverProbeData probe_data;
    probe_data.channel      = &feedback;
    probe_data.loss_tracker = &loss_tracker;

    GstPad* udpsrc_src = gst_element_get_static_pad(udpsrc, "src");
    if (udpsrc_src) {
        gst_pad_add_probe(udpsrc_src, GST_PAD_PROBE_TYPE_BUFFER,
                          rtp_in_probe, &probe_data, nullptr);
        gst_object_unref(udpsrc_src);
    }

    // ── Frame counting ──
    GstPad* sink_pad = gst_element_get_static_pad(sink, "sink");
    if (sink_pad) {
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe, nullptr, nullptr);
        gst_object_unref(sink_pad);
    }

    // ── Start ──
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    printf("✅ Receiver PLAYING on port %d\n", cfg.rtp_port);
    printf("📊 Sending feedback to %s:%d every 500ms\n\n", cfg.sender_host, feedback_port);

    auto start = std::chrono::steady_clock::now();
    int last_sec = -1;
    int report_count = 0;

    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();

        GstMessage* msg = gst_bus_pop_filtered(bus,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* err = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                fprintf(stderr, "❌ Error: %s\n", err->message);
                g_error_free(err);
                g_free(debug);
                running.store(false);
            }
            gst_message_unref(msg);
            break;
        }

        // Send feedback report
        double loss = loss_tracker.loss_fraction();
        feedback.send_report(loss);
        report_count++;

        int sec = static_cast<int>(elapsed);
        if (sec != last_sec && sec > 0) {
            last_sec = sec;
            printf("  [%3ds] RX:%lu frames  Loss:%4.1f%%  Reports:%d\n",
                   sec, (unsigned long)frame_count.load(std::memory_order_relaxed),
                   loss * 100.0, report_count);
            loss_tracker.reset();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    printf("\n🛑 Stopping receiver...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    feedback.stop();
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    printf("✅ Receiver stopped — %lu total frames received\n",
           (unsigned long)frame_count.load(std::memory_order_relaxed));
    return 0;
}
