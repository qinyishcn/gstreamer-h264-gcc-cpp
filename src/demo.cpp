/**
 * H.264 + GCC Video Transmission Demo (GStreamer C++)
 *
 * Demonstrates real GCC congestion control with a feedback channel.
 * Sender and receiver run in the same process, connected via UDP feedback.
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

#include <gst/gst.h>
#include "gst_compat.h"

#include "gcc_controller.h"
#include "feedback_channel.h"

static std::atomic<bool> running{true};
static std::atomic<uint64_t> demo_frame_count{0};

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

// ── Pad probes ──────────────────────────────────────────────────────

struct TxProbeData {
    gcc::SendTimeTracker* tracker;
};

/** Sender-side: record wall clock time for each outgoing RTP packet */
static GstPadProbeReturn
rtp_out_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
    auto* data = reinterpret_cast<TxProbeData*>(user_data);
    if (!data || !data->tracker) return GST_PAD_PROBE_OK;

    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    uint16_t seq = 0;
    if (parse_rtp_seq(map.data, map.size, seq)) {
        data->tracker->record(seq, gcc::get_timestamp_us());
    }

    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

struct RxProbeData {
    gcc::FeedbackChannel* channel;
    LossTracker*          loss_tracker;
};

/** Receiver-side: record arrival time for each incoming RTP packet */
static GstPadProbeReturn
rtp_in_probe(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer user_data) {
    auto* data = reinterpret_cast<RxProbeData*>(user_data);
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

static GstPadProbeReturn
frame_count_probe(GstPad* /*pad*/, GstPadProbeInfo* /*info*/, gpointer /*user_data*/) {
    demo_frame_count.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

// ── Config ──────────────────────────────────────────────────────────

struct DemoConfig {
    int    duration        = 20;
    int    bitrate         = 2000;
    int    min_bitrate     = 200;
    int    max_bitrate     = 8000;
    double gradient_thresh = 0.4;
    double hold_thresh     = 0.15;
    const char* host       = "127.0.0.1";
    int    rtp_port        = 5004;
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

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    DemoConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    gst_init(&argc, &argv);

    int feedback_port = cfg.rtp_port + 1;

    printf("======================================================================\n");
    printf("H.264 + GCC Video Transmission Demo (Real Feedback)\n");
    printf("======================================================================\n");
    printf("  Duration:      %ds\n", cfg.duration);
    printf("  Bitrate:       %d → [%d, %d] kbps\n", cfg.bitrate, cfg.min_bitrate, cfg.max_bitrate);
    printf("  Host:          %s\n", cfg.host);
    printf("  RTP port:      %d\n", cfg.rtp_port);
    printf("  Feedback port: %d\n", feedback_port);
    printf("  Thresholds:    grad=%.2f hold=%.2f\n", cfg.gradient_thresh, cfg.hold_thresh);
    printf("======================================================================\n\n");

    // ── Create GCC Controller ──
    gcc::Controller gcc_ctrl(
        cfg.bitrate, cfg.min_bitrate, cfg.max_bitrate,
        cfg.gradient_thresh, cfg.hold_thresh,
        0.5
    );

    // ── Shared send-time tracker (sender probe writes, feedback reads) ──
    gcc::SendTimeTracker send_tracker;

    // ── Feedback Channels ──
    gcc::FeedbackChannel sender_fb("0.0.0.0", feedback_port, true, &send_tracker);
    gcc::FeedbackChannel receiver_fb(cfg.host, feedback_port, false);

    if (!sender_fb.start()) {
        fprintf(stderr, "ERROR: Failed to start sender feedback channel\n");
        return 1;
    }
    if (!receiver_fb.start()) {
        fprintf(stderr, "ERROR: Failed to start receiver feedback channel\n");
        sender_fb.stop();
        return 1;
    }

    LossTracker loss_tracker;

    // ── Build Receiver Pipeline (element-by-element) ──
    GstElement *rx_udpsrc, *rx_depay, *rx_parser, *rx_decoder, *rx_convert, *rx_sink;
    GstElement* rx_pipeline_raw = gst_pipeline_new("rx-pipeline");
    rx_udpsrc   = gst_element_factory_make("udpsrc",         "rx-rtp-src");
    rx_depay    = gst_element_factory_make("rtph264depay",   "rx-rtp-depay");
    rx_parser   = gst_element_factory_make("h264parse",      "rx-h264-parser");
    rx_decoder  = gst_element_factory_make("avdec_h264",     "rx-h264-decoder");
    rx_convert  = gst_element_factory_make("videoconvert",   "rx-video-convert");
    rx_sink     = gst_element_factory_make("fakesink",       "rx-sink");

    if (!rx_pipeline_raw || !rx_udpsrc || !rx_depay || !rx_parser || !rx_decoder || !rx_convert || !rx_sink) {
        fprintf(stderr, "ERROR: Failed to create receiver elements\n");
        sender_fb.stop(); receiver_fb.stop();
        return 1;
    }

    GstCaps* rtp_caps = gst_caps_new_simple("application/x-rtp",
        "media", G_TYPE_STRING, "video",
        "encoding-name", G_TYPE_STRING, "H264",
        "clock-rate", G_TYPE_INT, 90000, nullptr);
    g_object_set(rx_udpsrc, "port", cfg.rtp_port, "caps", rtp_caps, nullptr);
    gst_caps_unref(rtp_caps);
    g_object_set(rx_parser, "config-interval", -1, nullptr);
    g_object_set(rx_sink, "sync", FALSE, "silent", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(rx_pipeline_raw),
        rx_udpsrc, rx_depay, rx_parser, rx_decoder, rx_convert, rx_sink, nullptr);

    if (!gst_element_link_many(rx_udpsrc, rx_depay, rx_parser, rx_decoder, rx_convert, rx_sink, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link receiver pipeline\n");
        sender_fb.stop(); receiver_fb.stop();
        gst_object_unref(rx_pipeline_raw);
        return 1;
    }

    // ── Build Sender Pipeline (element-by-element) ──
    GstElement *tx_src, *tx_convert, *tx_encoder, *tx_parser, *tx_payloader, *tx_sink;
    GstElement* tx_pipeline_raw = gst_pipeline_new("tx-pipeline");
    tx_src       = gst_element_factory_make("videotestsrc",  "tx-video-src");
    tx_convert   = gst_element_factory_make("videoconvert",  "tx-video-convert");
    tx_encoder   = gst_element_factory_make("x264enc",       "tx-encoder");
    tx_parser    = gst_element_factory_make("h264parse",     "tx-h264-parser");
    tx_payloader = gst_element_factory_make("rtph264pay",    "tx-rtp-payloader");
    tx_sink      = gst_element_factory_make("udpsink",       "tx-udp-sink");

    if (!tx_pipeline_raw || !tx_src || !tx_convert || !tx_encoder || !tx_parser || !tx_payloader || !tx_sink) {
        fprintf(stderr, "ERROR: Failed to create sender elements\n");
        sender_fb.stop(); receiver_fb.stop();
        gst_object_unref(rx_pipeline_raw);
        return 1;
    }

    g_object_set(tx_src, "is-live", TRUE, "pattern", 1, nullptr);
    g_object_set(tx_encoder,
        "tune", 0x4, "speed-preset", 1,
        "bitrate", cfg.bitrate, "key-int-max", 60,
        "bframes", 0, "threads", 2,
        "byte-stream", TRUE, "aud", TRUE, "rc-lookahead", 0, nullptr);
    g_object_set(tx_parser, "config-interval", -1, nullptr);
    g_object_set(tx_payloader, "pt", 96, "mtu", 1400, "config-interval", -1, nullptr);
    g_object_set(tx_sink, "host", cfg.host, "port", cfg.rtp_port, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(tx_pipeline_raw),
        tx_src, tx_convert, tx_encoder, tx_parser, tx_payloader, tx_sink, nullptr);

    if (!gst_element_link_many(tx_src, tx_convert, tx_encoder, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link tx src → convert → encoder\n");
        sender_fb.stop(); receiver_fb.stop();
        gst_object_unref(rx_pipeline_raw); gst_object_unref(tx_pipeline_raw);
        return 1;
    }

    GstCaps* h264_caps = gst_caps_new_simple("video/x-h264",
        "profile", G_TYPE_STRING, "baseline", nullptr);
    if (!gst_element_link_filtered(tx_encoder, tx_parser, h264_caps)) {
        fprintf(stderr, "ERROR: Failed to link encoder → parser\n");
        gst_caps_unref(h264_caps);
        sender_fb.stop(); receiver_fb.stop();
        gst_object_unref(rx_pipeline_raw); gst_object_unref(tx_pipeline_raw);
        return 1;
    }
    gst_caps_unref(h264_caps);

    if (!gst_element_link_many(tx_parser, tx_payloader, tx_sink, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link tx parser → payloader → sink\n");
        sender_fb.stop(); receiver_fb.stop();
        gst_object_unref(rx_pipeline_raw); gst_object_unref(tx_pipeline_raw);
        return 1;
    }

    // ── Add Pad Probes ──

    // Sender: record send times for each RTP packet
    TxProbeData tx_probe;
    tx_probe.tracker = &send_tracker;

    GstPad* tx_payloader_src = gst_element_get_static_pad(tx_payloader, "src");
    if (tx_payloader_src) {
        gst_pad_add_probe(tx_payloader_src, GST_PAD_PROBE_TYPE_BUFFER,
                          rtp_out_probe, &tx_probe, nullptr);
        gst_object_unref(tx_payloader_src);
    }

    // Receiver: record arrival times + loss
    RxProbeData rx_probe;
    rx_probe.channel      = &receiver_fb;
    rx_probe.loss_tracker = &loss_tracker;

    GstPad* rx_udpsrc_src = gst_element_get_static_pad(rx_udpsrc, "src");
    if (rx_udpsrc_src) {
        gst_pad_add_probe(rx_udpsrc_src, GST_PAD_PROBE_TYPE_BUFFER,
                          rtp_in_probe, &rx_probe, nullptr);
        gst_object_unref(rx_udpsrc_src);
    }

    // Receiver: frame counting
    GstPad* rx_sink_pad = gst_element_get_static_pad(rx_sink, "sink");
    if (rx_sink_pad) {
        gst_pad_add_probe(rx_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          frame_count_probe, nullptr, nullptr);
        gst_object_unref(rx_sink_pad);
    }

    // ── Start Pipelines ──
    printf("🚀 Starting receiver...\n");
    gst_element_set_state(GST_ELEMENT(rx_pipeline_raw), GST_STATE_PLAYING);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    printf("🚀 Starting sender...\n");
    gst_element_set_state(GST_ELEMENT(tx_pipeline_raw), GST_STATE_PLAYING);
    printf("✅ Both pipelines PLAYING\n\n");

    // ── GCC Control Loop ──
    printf("📊 GCC Control Loop (%ds) — Real Feedback Channel\n", cfg.duration);
    printf("======================================================================\n");

    auto start = std::chrono::steady_clock::now();
    int feedback_count = 0;

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed >= cfg.duration) break;

        // Poll for feedback from receiver
        gcc::FeedbackReport report;
        if (sender_fb.poll(report)) {
            feedback_count++;
            int new_bitrate = gcc_ctrl.step(report);
            g_object_set(G_OBJECT(tx_encoder), "bitrate", new_bitrate, nullptr);

            auto st = gcc_ctrl.get_stats();
            uint64_t rendered = demo_frame_count.load(std::memory_order_relaxed);
            printf("  [%5.1f] %6d kbps  %-10s  Loss:%4.1f%%  Delay:%7.3fms  RX:%lu  FB:%d\n",
                   elapsed, new_bitrate,
                   st.current_state == gcc::State::DECREASE ? "decrease" :
                   st.current_state == gcc::State::INCREASE ? "increase" : "hold",
                   st.loss_fraction * 100.0,
                   report.delay_ms,
                   (unsigned long)rendered,
                   feedback_count);
        }

        // Send receiver feedback report
        double loss = loss_tracker.loss_fraction();
        receiver_fb.send_report(loss);

        // Reset loss tracker every second
        if (static_cast<int>(elapsed) % 2 == 0 && static_cast<int>(elapsed) > 0) {
            loss_tracker.reset();
        }
    }

    // ── Stop ──
    printf("======================================================================\n");
    gst_element_set_state(GST_ELEMENT(tx_pipeline_raw), GST_STATE_NULL);
    gst_element_set_state(GST_ELEMENT(rx_pipeline_raw), GST_STATE_NULL);

    sender_fb.stop();
    receiver_fb.stop();

    gst_object_unref(tx_pipeline_raw);
    gst_object_unref(rx_pipeline_raw);

    auto stats = gcc_ctrl.get_stats();
    printf("\n======================================================================\n");
    printf("📊 Final GCC Statistics\n");
    printf("======================================================================\n");
    printf("  Total steps:      %d\n", stats.total_steps);
    printf("  Increases:        %d\n", stats.total_increases);
    printf("  Decreases:        %d\n", stats.total_decreases);
    printf("  Final bitrate:    %d kbps\n", stats.current_bitrate_kbps);
    printf("  Feedback reports: %d\n", feedback_count);
    printf("  Frames received:  %lu\n", (unsigned long)demo_frame_count.load());
    printf("  Final state:      %s\n",
           stats.current_state == gcc::State::DECREASE ? "decrease" :
           stats.current_state == gcc::State::INCREASE ? "increase" : "hold");
    printf("======================================================================\n");
    printf("✅ Demo complete!\n");
    return 0;
}
