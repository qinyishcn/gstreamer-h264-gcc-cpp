/**
 * H.264 + GCC Video Receiver (GStreamer C++)
 *
 * Pipeline: udpsrc → rtph264depay → h264parse → avdec_h264 → videoconvert → fakesink
 *
 * Uses pad probe for frame counting (works on all GStreamer versions).
 *
 * Usage:
 *   ./receiver [--port PORT] [--output fakesink|display]
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
static std::atomic<uint64_t> frame_count{0};

static void signal_handler(int /*sig*/) {
    running.store(false);
}

/**
 * Pad probe callback: counts each buffer that arrives at the sink pad.
 * Works on ALL GStreamer versions (no fakesink stats property needed).
 */
static GstPadProbeReturn
count_probe(GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer /*user_data*/) {
    frame_count.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

struct ReceiverConfig {
    int         rtp_port = 5004;
    const char* output   = "fakesink";
};

static void parse_args(int argc, char* argv[], ReceiverConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) cfg.rtp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) cfg.output = argv[++i];
    }
}

int main(int argc, char* argv[]) {
    ReceiverConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    gst_init(&argc, &argv);

    printf("============================================================\n");
    printf("H.264 + GCC Video Receiver\n");
    printf("============================================================\n");
    printf("  Port:    %d\n", cfg.rtp_port);
    printf("  Output:  %s\n", cfg.output);
    printf("============================================================\n\n");

    // Build pipeline using gst_parse_launch
    GError* error = nullptr;
    gchar* pipeline_str = g_strdup_printf(
        "udpsrc port=%d caps=application/x-rtp,media=video,encoding-name=H264,clock-rate=90000 ! "
        "rtph264depay ! "
        "h264parse config-interval=-1 ! "
        "avdec_h264 ! "
        "videoconvert ! "
        "%s name=video_sink sync=false",
        cfg.rtp_port, cfg.output
    );

    GstPipeline* pipeline = GST_PIPELINE(gst_parse_launch(pipeline_str, &error));
    g_free(pipeline_str);

    if (!pipeline || error) {
        fprintf(stderr, "ERROR: Failed to create pipeline: %s\n",
                error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }

    // Get sink and add pad probe for frame counting
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "video_sink");
    if (sink) {
        GstPad* sink_pad = gst_element_get_static_pad(sink, "sink");
        if (sink_pad) {
            gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe, nullptr, nullptr);
            gst_object_unref(sink_pad);
        }
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);

    // Start pipeline
    GstStateChangeReturn ret = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "ERROR: Failed to set pipeline to PLAYING\n");
        if (sink) gst_object_unref(sink);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        return 1;
    }

    printf("✅ Receiver listening on port %d\n\n", cfg.rtp_port);

    auto start = std::chrono::steady_clock::now();
    int last_sec = -1;

    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();

        // Check for errors
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

        // Print stats every second
        int sec = static_cast<int>(elapsed);
        if (sec != last_sec && sec > 0) {
            last_sec = sec;
            printf("  [%3ds] Received: %lu frames\n", sec,
                   (unsigned long)frame_count.load(std::memory_order_relaxed));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    printf("\n🛑 Stopping receiver...\n");
    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);

    if (sink) gst_object_unref(sink);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    printf("✅ Receiver stopped — %lu total frames received\n",
           (unsigned long)frame_count.load(std::memory_order_relaxed));
    return 0;
}
