/**
 * H.264 + GCC Video Sender (GStreamer C++)
 *
 * Features:
 *   - Real-time GCC bitrate adjustment based on receiver feedback
 *   - UDP feedback channel listens for receiver reports
 *   - RTP timestamp probe for delay gradient calculation
 *
 * Usage:
 *   ./sender [--host HOST] [--port PORT] [--bitrate KBPS] [--duration SEC]
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
#include "gcc_controller.h"
#include "feedback_channel.h"

static std::atomic<bool> running{true};

static void signal_handler(int /*sig*/) {
    running.store(false);
}

// ── RTP header parsing ──────────────────────────────────────────────

static bool parse_rtp_seq(const uint8_t* data, size_t len, uint16_t& seq) {
    if (len < 12) return false;
    seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    return true;
}

// ── Pad probe: record outgoing RTP packets ───────────────────────────

struct SenderProbeData {
    gcc::SendTimeTracker* tracker;
};

static GstPadProbeReturn
rtp_out_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
    auto* data = reinterpret_cast<SenderProbeData*>(user_data);
    if (!data || !data->tracker) return GST_PAD_PROBE_OK;

    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    uint16_t seq = 0;
    if (parse_rtp_seq(map.data, map.size, seq)) {
        // Record sender's wall clock time for this seq
        data->tracker->record(seq, gcc::get_timestamp_us());
    }

    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

// ── Config ──────────────────────────────────────────────────────────

struct SenderConfig {
    const char* host        = "127.0.0.1";
    int         rtp_port    = 5004;
    int         bitrate     = 2000;
    int         key_int     = 60;
    int         duration    = 20;
    const char* preset      = "ultrafast";
    int         threads     = 2;
};

static void parse_args(int argc, char* argv[], SenderConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) cfg.host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) cfg.rtp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) cfg.bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "--key-int") == 0 && i + 1 < argc) cfg.key_int = atoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) cfg.duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) cfg.preset = argv[++i];
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) cfg.threads = atoi(argv[++i]);
    }
}

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    SenderConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    gst_init(&argc, &argv);

    int feedback_port = cfg.rtp_port + 1;

    printf("============================================================\n");
    printf("H.264 + GCC Video Sender (with feedback)\n");
    printf("============================================================\n");
    printf("  Target:       %s:%d\n", cfg.host, cfg.rtp_port);
    printf("  Bitrate:      %d kbps\n", cfg.bitrate);
    printf("  Duration:     %d seconds\n", cfg.duration);
    printf("  Feedback:     listening on port %d\n", feedback_port);
    printf("  Preset:       %s\n", cfg.preset);
    printf("============================================================\n\n");

    // ── Create GCC Controller ──
    gcc::Controller gcc_ctrl(cfg.bitrate, 200, 8000, 0.4, 0.15, 0.5);

    // ── Create Send Time Tracker + Feedback Channel ──
    gcc::SendTimeTracker send_tracker;
    gcc::FeedbackChannel feedback("0.0.0.0", feedback_port, true, &send_tracker);
    if (!feedback.start()) {
        fprintf(stderr, "ERROR: Failed to start feedback channel on port %d\n", feedback_port);
        return 1;
    }

    // ── Build pipeline element-by-element ──
    GstElement *pipeline, *src, *convert, *encoder, *parser, *payloader, *sink;
    pipeline  = gst_pipeline_new("sender-pipeline");
    src       = gst_element_factory_make("videotestsrc",  "video-src");
    convert   = gst_element_factory_make("videoconvert",  "video-convert");
    encoder   = gst_element_factory_make("x264enc",       "encoder");
    parser    = gst_element_factory_make("h264parse",     "h264-parser");
    payloader = gst_element_factory_make("rtph264pay",    "rtp-payloader");
    sink      = gst_element_factory_make("udpsink",       "udp-sink");

    if (!pipeline || !src || !convert || !encoder || !parser || !payloader || !sink) {
        fprintf(stderr, "ERROR: Failed to create elements\n");
        feedback.stop();
        return 1;
    }

    g_object_set(src, "is-live", TRUE, "pattern", 1, nullptr);
    g_object_set(encoder,
        "tune", 0x4, "speed-preset", 1,
        "bitrate", cfg.bitrate, "key-int-max", cfg.key_int,
        "bframes", 0, "threads", cfg.threads,
        "byte-stream", TRUE, "aud", TRUE, "rc-lookahead", 0, nullptr);
    g_object_set(parser, "config-interval", -1, nullptr);
    g_object_set(payloader, "pt", 96, "mtu", 1400, "config-interval", -1, nullptr);
    g_object_set(sink, "host", cfg.host, "port", cfg.rtp_port, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline),
        src, convert, encoder, parser, payloader, sink, nullptr);

    if (!gst_element_link_many(src, convert, encoder, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link src → convert → encoder\n");
        feedback.stop();
        gst_object_unref(pipeline);
        return 1;
    }

    GstCaps* h264_caps = gst_caps_new_simple("video/x-h264",
        "profile", G_TYPE_STRING, "baseline", nullptr);
    if (!gst_element_link_filtered(encoder, parser, h264_caps)) {
        fprintf(stderr, "ERROR: Failed to link encoder → parser\n");
        gst_caps_unref(h264_caps);
        feedback.stop();
        gst_object_unref(pipeline);
        return 1;
    }
    gst_caps_unref(h264_caps);

    if (!gst_element_link_many(parser, payloader, sink, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link parser → payloader → sink\n");
        feedback.stop();
        gst_object_unref(pipeline);
        return 1;
    }

    // ── RTP output pad probe: record send times ──
    SenderProbeData probe_data;
    probe_data.tracker = &send_tracker;

    GstPad* payloader_src = gst_element_get_static_pad(payloader, "src");
    if (payloader_src) {
        gst_pad_add_probe(payloader_src, GST_PAD_PROBE_TYPE_BUFFER,
                          rtp_out_probe, &probe_data, nullptr);
        gst_object_unref(payloader_src);
    }

    // ── Start pipeline ──
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstState state;
    gst_element_get_state(pipeline, &state, nullptr, 2 * GST_SECOND);

    printf("✅ Sender PLAYING → %s:%d\n", cfg.host, cfg.rtp_port);
    printf("📊 Waiting for receiver feedback...\n\n");

    // ── Main control loop ──
    auto start = std::chrono::steady_clock::now();
    int last_printed = -1;
    int feedback_count = 0;

    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed >= cfg.duration) break;

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

        // Poll for feedback
        gcc::FeedbackReport report;
        if (feedback.poll(report)) {
            feedback_count++;
            int new_bitrate = gcc_ctrl.step(report);
            g_object_set(G_OBJECT(encoder), "bitrate", new_bitrate, nullptr);

            auto st = gcc_ctrl.get_stats();
            int sec = static_cast<int>(elapsed);
            if (sec != last_printed) {
                last_printed = sec;
                printf("  [%3ds] %6d kbps  %-10s  Loss:%4.1f%%  Delay:%7.2fms  FB:%d\n",
                       sec, new_bitrate,
                       st.current_state == gcc::State::DECREASE ? "decrease" :
                       st.current_state == gcc::State::INCREASE ? "increase" : "hold",
                       st.loss_fraction * 100.0,
                       report.delay_ms,
                       feedback_count);
            }
        } else {
            int sec = static_cast<int>(elapsed);
            if (sec != last_printed) {
                last_printed = sec;
                printf("  [%3ds] Waiting for receiver feedback...\n", sec);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── Stop ──
    printf("\n🛑 Stopping sender...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    feedback.stop();
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    auto stats = gcc_ctrl.get_stats();
    printf("\n============================================================\n");
    printf("📊 Final GCC Statistics\n");
    printf("============================================================\n");
    printf("  Total steps:      %d\n", stats.total_steps);
    printf("  Increases:        %d\n", stats.total_increases);
    printf("  Decreases:        %d\n", stats.total_decreases);
    printf("  Final bitrate:    %d kbps\n", stats.current_bitrate_kbps);
    printf("  Feedback reports: %d\n", feedback_count);
    printf("  Final state:      %s\n",
           stats.current_state == gcc::State::DECREASE ? "decrease" :
           stats.current_state == gcc::State::INCREASE ? "increase" : "hold");
    printf("============================================================\n");
    printf("✅ Sender stopped\n");
    return 0;
}
