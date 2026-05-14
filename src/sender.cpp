/**
 * H.264 + GCC Video Sender (GStreamer C++)
 *
 * Pipeline: videotestsrc → videoconvert → x264enc → h264parse → rtph264pay → udpsink
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

    // Build pipeline using gst_parse_launch
    GError* error = nullptr;
    gchar* pipeline_str = g_strdup_printf(
        "videotestsrc is-live=true pattern=ball ! "
        "videoconvert ! "
        "x264enc tune=zerolatency speed-preset=%s bitrate=%d key-int-max=%d "
        "bframes=0 threads=%d byte-stream=true aud=true rc-lookahead=0 "
        "option-string=\"profile=baseline\" ! "
        "h264parse config-interval=-1 ! "
        "rtph264pay pt=96 mtu=1400 config-interval=-1 ! "
        "udpsink host=%s port=%d sync=false async=false",
        cfg.preset, cfg.bitrate, cfg.key_int, cfg.threads,
        cfg.host, cfg.rtp_port
    );

    GstPipeline* pipeline = GST_PIPELINE(gst_parse_launch(pipeline_str, &error));
    g_free(pipeline_str);

    if (!pipeline || error) {
        fprintf(stderr, "ERROR: Failed to create pipeline: %s\n",
                error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }

    // Set up bus watch for errors
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);

    // Start pipeline
    GstStateChangeReturn ret = gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "ERROR: Failed to set pipeline to PLAYING\n");
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
        GstMessage* msg = gst_bus_pop_filtered(bus,
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
    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);

    gst_object_unref(bus);
    gst_object_unref(pipeline);

    printf("✅ Sender stopped\n");
    return 0;
}
