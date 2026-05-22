/**
 * H.264 + GCC Video Sender (GStreamer C++ + razor GCC)
 *
 * Uses razor library for standard GCC bandwidth estimation.
 * GStreamer handles actual RTP transmission; razor only estimates bitrate.
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
static void signal_handler(int) { running.store(false); }

static bool parse_rtp_seq(const uint8_t* data, size_t len, uint16_t& seq) {
    if (len < 12) return false;
    seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    return true;
}

struct SenderProbeData {
    uint16_t* last_seq;
};

static GstPadProbeReturn rtp_out_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
    auto* d = static_cast<SenderProbeData*>(user_data);
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    uint16_t seq = 0;
    if (parse_rtp_seq(map.data, map.size, seq)) {
        *d->last_seq = seq;
    }
    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

struct SenderConfig {
    const char* host = "127.0.0.1";
    int rtp_port = 5004;
    int bitrate = 2000;
    int duration = 20;
    const char* preset = "ultrafast";
    int threads = 2;
};

static void parse_args(int argc, char* argv[], SenderConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0 && i+1 < argc) cfg.host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i+1 < argc) cfg.rtp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i+1 < argc) cfg.bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc) cfg.duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--preset") == 0 && i+1 < argc) cfg.preset = argv[++i];
        else if (strcmp(argv[i], "--threads") == 0 && i+1 < argc) cfg.threads = atoi(argv[++i]);
    }
}

int main(int argc, char* argv[]) {
    SenderConfig cfg;
    parse_args(argc, argv, cfg);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    gst_init(&argc, &argv);

    int feedback_port = cfg.rtp_port + 1;

    printf("============================================================\n");
    printf("H.264 + GCC Video Sender (razor GCC v4.0)\n");
    printf("============================================================\n");
    printf("  Target:       %s:%d\n", cfg.host, cfg.rtp_port);
    printf("  Bitrate:      %d kbps\n", cfg.bitrate);
    printf("  Duration:     %d seconds\n", cfg.duration);
    printf("  Feedback:     listening on port %d\n", feedback_port);
    printf("============================================================\n\n");

    // ── GStreamer pipeline ──
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
        return 1;
    }

    g_object_set(src, "is-live", TRUE, "pattern", 1, nullptr);
    g_object_set(encoder,
        "tune", 0x4, "speed-preset", 1,
        "bitrate", cfg.bitrate, "key-int-max", 60,
        "bframes", 0, "threads", cfg.threads,
        "byte-stream", TRUE, "aud", TRUE, "rc-lookahead", 0, nullptr);
    g_object_set(parser, "config-interval", -1, nullptr);
    g_object_set(payloader, "pt", 96, "mtu", 1400, "config-interval", -1, nullptr);
    g_object_set(sink, "host", cfg.host, "port", cfg.rtp_port, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), src, convert, encoder, parser, payloader, sink, nullptr);

    if (!gst_element_link_many(src, convert, encoder, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link src->convert->encoder\n");
        return 1;
    }
    GstCaps* h264_caps = gst_caps_new_simple("video/x-h264", "profile", G_TYPE_STRING, "baseline", nullptr);
    if (!gst_element_link_filtered(encoder, parser, h264_caps)) {
        fprintf(stderr, "ERROR: Failed to link encoder->parser\n");
        gst_caps_unref(h264_caps);
        return 1;
    }
    gst_caps_unref(h264_caps);
    if (!gst_element_link_many(parser, payloader, sink, nullptr)) {
        fprintf(stderr, "ERROR: Failed to link parser->payloader->sink\n");
        return 1;
    }

    // ── Razor sender with encoder callback ──
    // Store encoder pointer for bitrate adjustment
    GstElement* enc_ref = encoder;  // keep ref valid (pipeline owns it)

    // Loss-based bitrate smoothing state
    double smoothed_loss = 0.0;
    int current_br = static_cast<int>(cfg.bitrate);
    int last_disp_br = current_br;
    auto last_adapt_time = std::chrono::steady_clock::now();
    bool got_first_feedback = false;

    razor::Sender razor_sender([&](uint32_t new_bitrate_kbps, uint8_t loss_pct, uint32_t rtt_ms) {
        got_first_feedback = true;
        // loss_pct is 0-255, convert to 0.0-1.0
        double instant_loss = loss_pct / 255.0;
        smoothed_loss = 0.3 * instant_loss + 0.7 * smoothed_loss;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_adapt_time).count();
        if (elapsed < 0.5) return;
        last_adapt_time = now;

        // Use razor's estimate as base, apply loss reduction only if needed
        int br = static_cast<int>(new_bitrate_kbps);
        if (br > 8000) br = 8000;
        if (br < 100) br = 100;

        // Loss-based reduction: reduce razor's estimate proportionally
        if (smoothed_loss > 0.02) {
            double factor = 1.0 - (smoothed_loss - 0.02) * 2.0;
            if (factor < 0.3) factor = 0.3;
            br = static_cast<int>(br * factor);
        }

        if (br > 8000) br = 8000;
        if (br < 100) br = 100;

        // Smooth transition: max 20% change per step
        int diff = br - current_br;
        int max_step = current_br / 5;
        if (max_step < 100) max_step = 100;
        if (diff > max_step) br = current_br + max_step;
        else if (diff < -max_step) br = current_br - max_step;

        if (br != current_br) {
            g_object_set(enc_ref, "bitrate", br, nullptr);
            current_br = br;
        }
    });
    if (!razor_sender.is_valid()) {
        fprintf(stderr, "ERROR: Failed to create razor sender\n");
        return 1;
    }
    razor_sender.set_bitrates(200, cfg.bitrate, 8000);

    // ── Feedback channel ──
    gcc::FeedbackChannel feedback("0.0.0.0", feedback_port, true);
    if (!feedback.start()) {
        fprintf(stderr, "ERROR: Failed to start feedback channel on port %d\n", feedback_port);
        return 1;
    }

    // ── Pad probe: track outgoing RTP seq ──
    uint16_t last_rtp_seq = 0;
    uint16_t prev_rtp_seq = 0;  // track last registered seq
    SenderProbeData probe_data = { &last_rtp_seq };
    GstPad* payloader_src = gst_element_get_static_pad(payloader, "src");
    if (payloader_src) {
        gst_pad_add_probe(payloader_src, GST_PAD_PROBE_TYPE_BUFFER, rtp_out_probe, &probe_data, nullptr);
        gst_object_unref(payloader_src);
    }

    // ── Start ──
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_signal_watch(bus);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);

    printf("  Sender PLAYING\n");
    printf("  Waiting for receiver feedback...\n\n");

    auto start = std::chrono::steady_clock::now();
    int feedback_count = 0;
    auto last_heartbeat = start;
    auto last_fb_time = start;
    bool printed_fb = false;

    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed >= cfg.duration) break;

        // GStreamer error check
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
            razor_sender.heartbeat();
            last_heartbeat = hb_now;
        }

        // Notify razor of outgoing packet (only when new seq arrives)
        if (last_rtp_seq != prev_rtp_seq) {
            razor_sender.on_send(last_rtp_seq, 1400);
            prev_rtp_seq = last_rtp_seq;
        }

        // Poll for receiver feedback
        gcc::FeedbackChannel::ReceivedFeedback fb;
        if (feedback.poll(fb)) {
            feedback_count++;
            last_fb_time = std::chrono::steady_clock::now();
            printed_fb = false;

            // Estimate RTT from feedback timestamps
            if (fb.rx_ts_ms > 0 && fb.fb_ts_ms > 0) {
                uint32_t now_ms = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
                // RTT ≈ time from receiver got packet to sender got feedback
                // (receiver_delay + network_delay) * 2
                uint32_t rtt_ms = now_ms - fb.rx_ts_ms;
                if (rtt_ms > 0 && rtt_ms < 5000) {
                    razor_sender.update_rtt(rtt_ms);
                }
            }

            // Feed razor feedback to razor
            if (fb.razor_size > 0) {
                razor_sender.on_feedback(fb.razor_data, fb.razor_size);
            }
        }

        int sec = static_cast<int>(elapsed);
        auto since_fb = std::chrono::duration<double>(now - last_fb_time).count();
        if (feedback_count > 0 && since_fb < 2.0) {
            if (current_br != last_disp_br) {
                printf("  [%3ds] Bitrate adjusted: %d -> %d kbps  FB:%d\n",
                       sec, last_disp_br, current_br, feedback_count);
                last_disp_br = current_br;
            } else {
                printf("  [%3ds] Feedback received  FB:%d\n", sec, feedback_count);
            }
        } else if (!printed_fb && since_fb > 1.0) {
            printf("  [%3ds] Waiting for receiver feedback...\n", sec);
            printed_fb = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ── Stop ──
    printf("\nStopping sender...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    feedback.stop();
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    printf("\n============================================================\n");
    printf("  Final Statistics\n");
    printf("============================================================\n");
    printf("  Feedback reports: %d\n", feedback_count);
    printf("============================================================\n");
    return 0;
}
