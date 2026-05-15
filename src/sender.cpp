/**
 * H.264 + GCC Video Sender (GStreamer C++)
 *
 * Pipeline: videotestsrc → videoconvert → x264enc → h264parse → rtph264pay → udpsink
 *
 * Builds pipeline element-by-element to avoid gst_parse_launch quoting issues.
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

static std::atomic<bool> running{true};

static void signal_handler(int /*sig*/) {
    running.store(false);
}

struct SenderConfig {
    const char* host        = "127.0.0.1";
    int         rtp_port    = 5004;
    int         bitrate     = 2000;    // kbps
    int         key_int     = 60;      // keyframe interval
    int         duration    = 20;      // seconds
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

int main(int argc, char* argv[]) {
    SenderConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    gst_init(&argc, &argv);

    printf("============================================================\n");
    printf("H.264 + GCC Video Sender\n");
    printf("============================================================\n");
    printf("  Target:    %s:%d\n", cfg.host, cfg.rtp_port);
    printf("  Bitrate:   %d kbps\n", cfg.bitrate);
    printf("  Duration:  %d seconds\n", cfg.duration);
    printf("  Preset:    %s\n", cfg.preset);
    printf("  Key int:   %d frames\n", cfg.key_int);
    printf("============================================================\n\n");

    // ── Build pipeline element-by-element (avoids gst_parse_launch quoting issues) ──

    GstElement *pipeline, *src, *convert, *encoder, *parser, *payloader, *sink;
    pipeline  = gst_pipeline_new("sender-pipeline");
    src       = gst_element_factory_make("videotestsrc",  "video-src");
    convert   = gst_element_factory_make("videoconvert",  "video-convert");
    encoder   = gst_element_factory_make("x264enc",       "encoder");
    parser    = gst_element_factory_make("h264parse",     "h264-parser");
    payloader = gst_element_factory_make("rtph264pay",    "rtp-payloader");
    sink      = gst_element_factory_make("udpsink",       "udp-sink");

    // Verify all elements were created
    if (!pipeline || !src || !convert || !encoder || !parser || !payloader || !sink) {
        fprintf(stderr, "ERROR: Failed to create elements:\n");
        if (!src)       fprintf(stderr, "  ✗ videotestsrc  (check: gstreamer1.0-plugins-base)\n");
        if (!convert)   fprintf(stderr, "  ✗ videoconvert  (check: gstreamer1.0-plugins-base)\n");
        if (!encoder)   fprintf(stderr, "  ✗ x264enc       (check: gstreamer1.0-plugins-ugly)\n");
        if (!parser)    fprintf(stderr, "  ✗ h264parse     (check: gstreamer1.0-plugins-bad)\n");
        if (!payloader) fprintf(stderr, "  ✗ rtph264pay    (check: gstreamer1.0-plugins-good)\n");
        if (!sink)      fprintf(stderr, "  ✗ udpsink       (check: gstreamer1.0-plugins-good)\n");
        gst_object_unref(pipeline);
        return 1;
    }

    // Configure videotestsrc
    g_object_set(src,
        "is-live", TRUE,
        "pattern", 1,  // ball pattern
        nullptr);

    // Configure x264enc
    // Use string enum values for cross-version compatibility
    g_object_set(encoder,
        "tune",         "zerolatency",
        "speed-preset", "ultrafast",
        "bitrate",      cfg.bitrate,
        "key-int-max",  cfg.key_int,
        "bframes",      0,
        "threads",      cfg.threads,
        "byte-stream",  TRUE,
        "aud",          TRUE,
        "rc-lookahead", 0,
        nullptr);

    // Set x264 profile via option-string
    g_object_set(encoder, "option-string", "profile=baseline", nullptr);

    // Configure h264parse
    g_object_set(parser, "config-interval", -1, nullptr);

    // Configure rtph264pay
    g_object_set(payloader,
        "pt",              96,
        "mtu",             1400,
        "config-interval", -1,
        nullptr);

    // Configure udpsink
    g_object_set(sink,
        "host", cfg.host,
        "port", cfg.rtp_port,
        "sync", FALSE,
        "async", FALSE,
        nullptr);

    // Add all elements to pipeline and link
    gst_bin_add_many(GST_BIN(pipeline),
        src, convert, encoder, parser, payloader, sink, nullptr);

    // Link: src → convert → encoder → parser → payloader → sink
    if (!gst_element_link_many(src, convert, encoder, parser, payloader, sink, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link pipeline elements\n");
        gst_object_unref(pipeline);
        return 1;
    }

    // Set up bus watch for errors
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);

    // Start pipeline
    GstStateChangeReturn state_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "ERROR: Failed to set pipeline to PLAYING\n");
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        return 1;
    }

    // Wait for state change to complete (important for live sources)
    GstState state;
    GstStateChangeReturn pending_ret = gst_element_get_state(pipeline, &state, nullptr, 2 * GST_SECOND);
    if (pending_ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "ERROR: Pipeline failed to reach PLAYING state\n");
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        return 1;
    }

    printf("✅ Sender started → %s:%d\n\n", cfg.host, cfg.rtp_port);

    // Run for specified duration
    auto start = std::chrono::steady_clock::now();

    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();

        if (elapsed >= cfg.duration) break;

        // Check for bus errors
        GstMessage* msg = gst_compat_bus_pop_filtered(bus,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* err = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                fprintf(stderr, "❌ Error: %s\n", err->message);
                if (debug) fprintf(stderr, "   Debug: %s\n", debug);
                g_error_free(err);
                g_free(debug);
                running.store(false);
            }
            gst_message_unref(msg);
            break;
        }

        // Print status every second
        static int last_printed = -1;
        int sec = static_cast<int>(elapsed);
        if (sec != last_printed) {
            last_printed = sec;
            printf("  [%3ds] Sending... bitrate=%d kbps\n", sec, cfg.bitrate);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    printf("\n🛑 Stopping sender...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);

    gst_object_unref(bus);
    gst_object_unref(pipeline);

    printf("✅ Sender stopped\n");
    return 0;
}
