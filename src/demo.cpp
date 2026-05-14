/**
 * H.264 + GCC Video Transmission Demo (GStreamer C++)
 *
 * Demonstrates GCC congestion control with simulated network conditions.
 *
 * Pipeline:
 *   Sender: videotestsrc → x264enc → RTP → UDP
 *   Receiver: UDP → depayload → decode → fakesink
 *   GCC: monitors delay gradients, adjusts encoder bitrate
 *
 * Usage:
 *   ./demo [--duration SEC] [--bitrate KBPS]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>

#include <gst/gst.h>
#include "gst_compat.h"

#include "gcc_controller.h"

static std::atomic<bool> running{true};

static void signal_handler(int /*sig*/) {
    running.store(false);
}

// ── Simulated Network ──────────────────────────────────────────────

struct SimNetwork {
    double delay_ms      = 10.0;
    double loss_fraction = 0.0;
    double _prev_delay   = 10.0;
};

static void update_network(SimNetwork& net, double elapsed) {
    net._prev_delay = net.delay_ms;
    if (elapsed < 5.0) {
        net.delay_ms = 10.0; net.loss_fraction = 0.0;
    } else if (elapsed < 10.0) {
        double p = (elapsed - 5.0) / 5.0;
        net.delay_ms = 10.0 + p * 80.0;
        net.loss_fraction = p * 0.08;
    } else if (elapsed < 15.0) {
        double p = (elapsed - 10.0) / 5.0;
        net.delay_ms = 90.0 - p * 80.0;
        net.loss_fraction = 0.08 - p * 0.08;
    } else {
        net.delay_ms = 10.0; net.loss_fraction = 0.0;
    }
}

static const char* get_phase(double elapsed) {
    if (elapsed < 5.0)  return "stable";
    if (elapsed < 10.0) return "congest";
    if (elapsed < 15.0) return "recovery";
    return "stable";
}

// ── Main ───────────────────────────────────────────────────────────

struct DemoConfig {
    int    duration         = 20;
    int    bitrate          = 2000;
    int    min_bitrate      = 200;
    int    max_bitrate      = 8000;
    double gradient_thresh  = 0.4;
    double hold_thresh      = 0.15;
    const char* host        = "127.0.0.1";
    int    rtp_port         = 5004;
};

static void parse_args(int argc, char* argv[], DemoConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) cfg.duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) cfg.bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "--min-bitrate") == 0 && i + 1 < argc) cfg.min_bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-bitrate") == 0 && i + 1 < argc) cfg.max_bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gradient-threshold") == 0 && i + 1 < argc) cfg.gradient_thresh = atof(argv[++i]);
        else if (strcmp(argv[i], "--hold-threshold") == 0 && i + 1 < argc) cfg.hold_thresh = atof(argv[++i]);
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) cfg.host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) cfg.rtp_port = atoi(argv[++i]);
    }
}

int main(int argc, char* argv[]) {
    DemoConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    gst_init(&argc, &argv);

    printf("======================================================================\n");
    printf("H.264 + Google Congestion Control Video Transmission Demo\n");
    printf("======================================================================\n");
    printf("  Duration:    %ds\n", cfg.duration);
    printf("  Bitrate:     %d → [%d, %d] kbps\n", cfg.bitrate, cfg.min_bitrate, cfg.max_bitrate);
    printf("  Host:        %s\n", cfg.host);
    printf("  RTP port:    %d\n", cfg.rtp_port);
    printf("  Thresholds:  grad=%.2f hold=%.2f\n", cfg.gradient_thresh, cfg.hold_thresh);
    printf("======================================================================\n\n");

    // ── Create GCC Controller ──
    gcc::Controller gcc(
        cfg.bitrate, cfg.min_bitrate, cfg.max_bitrate,
        cfg.gradient_thresh, cfg.hold_thresh,
        0.5   // kalman_gain — fast response for demo
    );

    SimNetwork net;

    // ── Build Receiver Pipeline ──
    GError* error = nullptr;
    gchar* rx_str = g_strdup_printf(
        "udpsrc port=%d caps=application/x-rtp,media=video,encoding-name=H264,clock-rate=90000 ! "
        "rtph264depay ! h264parse config-interval=-1 ! avdec_h264 ! "
        "videoconvert ! fakesink name=rx_sink sync=false silent=true",
        cfg.rtp_port
    );
    GstPipeline* rx_pipeline = GST_PIPELINE(gst_parse_launch(rx_str, &error));
    g_free(rx_str);
    if (!rx_pipeline || error) {
        fprintf(stderr, "ERROR: Receiver pipeline: %s\n", error ? error->message : "?");
        return 1;
    }

    // ── Build Sender Pipeline ──
    gchar* tx_str = g_strdup_printf(
        "videotestsrc is-live=true pattern=ball ! "
        "videoconvert ! "
        "x264enc tune=zerolatency speed-preset=ultrafast bitrate=%d "
        "key-int-max=60 bframes=0 threads=2 byte-stream=true aud=true rc-lookahead=0 "
        "option-string=\"profile=baseline\" name=encoder ! "
        "h264parse config-interval=-1 ! "
        "rtph264pay pt=96 mtu=1400 config-interval=-1 ! "
        "udpsink host=%s port=%d sync=false async=false",
        cfg.bitrate, cfg.host, cfg.rtp_port
    );
    GstPipeline* tx_pipeline = GST_PIPELINE(gst_parse_launch(tx_str, &error));
    g_free(tx_str);
    if (!tx_pipeline || error) {
        fprintf(stderr, "ERROR: Sender pipeline: %s\n", error ? error->message : "?");
        gst_object_unref(rx_pipeline);
        return 1;
    }

    GstElement* encoder = gst_bin_get_by_name(GST_BIN(tx_pipeline), "encoder");
    GstElement* rx_sink = gst_bin_get_by_name(GST_BIN(rx_pipeline), "rx_sink");

    // ── Start Pipelines ──
    printf("🚀 Starting receiver...\n");
    gst_element_set_state(GST_ELEMENT(rx_pipeline), GST_STATE_PLAYING);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    printf("🚀 Starting sender...\n");
    gst_element_set_state(GST_ELEMENT(tx_pipeline), GST_STATE_PLAYING);
    printf("✅ Both pipelines PLAYING\n\n");

    // ── GCC Control Loop ──
    printf("📊 GCC Control Loop (%ds)\n", cfg.duration);
    printf("----------------------------------------------------------------------\n");

    auto start = std::chrono::steady_clock::now();
    int step = 0;

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed >= cfg.duration) break;

        step++;

        // Update simulated network
        double prev_delay = net.delay_ms;
        update_network(net, elapsed);

        // Create feedback report
        // Gradient = delay change between consecutive steps (ms per step)
        double delay_gradient = net.delay_ms - prev_delay;

        gcc::FeedbackReport report;
        report.timestamp_us  = static_cast<uint64_t>(elapsed * 1e6);
        report.delay_ms      = delay_gradient;  // Pass gradient directly
        report.loss_fraction = net.loss_fraction;

        // Run GCC step
        int new_bitrate = gcc.step(report);

        // Adjust encoder bitrate dynamically
        if (encoder) {
            g_object_set(G_OBJECT(encoder), "bitrate", new_bitrate, nullptr);
        }

        // Get RX stats
        uint64_t rendered = 0;
        if (rx_sink) {
            GValue val = G_VALUE_INIT;
            g_object_get_property(G_OBJECT(rx_sink), "stats", &val);
            if (G_VALUE_TYPE(&val) == GST_TYPE_STRUCTURE) {
                const GstStructure* stats = (const GstStructure*)g_value_get_boxed(&val);
                gst_structure_get_uint64(stats, "rendered", &rendered);
                g_value_unset(&val);
            }
        }

        auto st = gcc.get_stats();
        printf("  [%5.1f] %-8s  %6d kbps  %-10s  Loss:%4.1f%%  Delay:%5.1fms  RX:%lu\n",
               elapsed, get_phase(elapsed),
               new_bitrate,
               st.current_state == gcc::State::DECREASE ? "decrease" :
               st.current_state == gcc::State::INCREASE ? "increase" : "hold",
               net.loss_fraction * 100.0,
               net.delay_ms,
               (unsigned long)rendered);
    }

    // ── Stop ──
    printf("----------------------------------------------------------------------\n");
    gst_element_set_state(GST_ELEMENT(tx_pipeline), GST_STATE_NULL);
    gst_element_set_state(GST_ELEMENT(rx_pipeline), GST_STATE_NULL);

    if (encoder) gst_object_unref(encoder);
    if (rx_sink) gst_object_unref(rx_sink);
    gst_object_unref(tx_pipeline);
    gst_object_unref(rx_pipeline);

    // ── Final Stats ──
    auto stats = gcc.get_stats();
    printf("\n======================================================================\n");
    printf("📊 Final GCC Statistics\n");
    printf("======================================================================\n");
    printf("  Total steps:    %d\n", stats.total_steps);
    printf("  Increases:      %d\n", stats.total_increases);
    printf("  Decreases:      %d\n", stats.total_decreases);
    printf("  Final bitrate:  %d kbps\n", stats.current_bitrate_kbps);
    printf("  Final state:    %s\n",
           stats.current_state == gcc::State::DECREASE ? "decrease" :
           stats.current_state == gcc::State::INCREASE ? "increase" : "hold");
    printf("======================================================================\n");
    printf("✅ Demo complete!\n");

    return 0;
}
