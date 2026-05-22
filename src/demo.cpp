/**
 * H.264 + GCC Video Transmission Demo (razor GCC v3.0)
 * Sender + Receiver in same process.
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
static std::atomic<uint64_t> demo_frame_count{0};
static void signal_handler(int) { running.store(false); }

static bool parse_rtp_seq(const uint8_t* data, size_t len, uint16_t& seq) {
    if (len < 12) return false;
    seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    return true;
}

// Sender probe
struct TxProbeData { uint16_t* last_seq; };
static GstPadProbeReturn rtp_out_probe(GstPad*, GstPadProbeInfo* info, gpointer ud) {
    auto* d = static_cast<TxProbeData*>(ud);
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    uint16_t seq = 0;
    if (parse_rtp_seq(map.data, map.size, seq)) *d->last_seq = seq;
    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

// Receiver probe
struct RxProbeData { razor::Receiver* rx; uint16_t* last_seq; uint64_t* pkt_count; std::atomic<uint32_t>* last_rx_ts_ms; };
static GstPadProbeReturn rtp_in_probe(GstPad*, GstPadProbeInfo* info, gpointer ud) {
    auto* d = static_cast<RxProbeData*>(ud);
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    uint16_t seq = 0;
    if (parse_rtp_seq(map.data, map.size, seq)) {
        *d->last_seq = seq;
        (*d->pkt_count)++;
        uint32_t now_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
        d->last_rx_ts_ms->store(now_ms);
        d->rx->on_received(seq, now_ms, map.size);
    }
    gst_buffer_unmap(buf, &map);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn frame_count_probe(GstPad*, GstPadProbeInfo*, gpointer) {
    demo_frame_count.fetch_add(1, std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

int main(int argc, char* argv[]) {
    int duration = 20, bitrate = 2000, rtp_port = 5004;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--duration") == 0 && i+1 < argc) duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i+1 < argc) bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i+1 < argc) rtp_port = atoi(argv[++i]);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    gst_init(&argc, &argv);

    int fb_port = rtp_port + 1;
    printf("======================================================================\n");
    printf("H.264 + GCC Demo (razor GCC v4.0)\n");
    printf("  Duration: %ds | Bitrate: %d kbps | Port: %d\n", duration, bitrate, rtp_port);
    printf("======================================================================\n\n");

    // Feedback channels
    gcc::FeedbackChannel sender_fb("0.0.0.0", fb_port, true);
    gcc::FeedbackChannel receiver_fb("127.0.0.1", fb_port, false);

    if (!sender_fb.start() || !receiver_fb.start()) {
        fprintf(stderr, "ERROR: Failed to start feedback channels\n");
        return 1;
    }

    // Razor sender with loss-based adaptation
    int current_br = bitrate;
    double smoothed_loss = 0.0;
    GstElement* enc_ref = nullptr;  // set after encoder is created
    razor::Sender razor_tx([&](uint32_t br_kbps, uint8_t loss, uint32_t rtt) {
        // Razor's estimate as base
        int br = static_cast<int>(br_kbps);
        if (br > 8000) br = 8000;
        if (br < 100) br = 100;

        // Loss-based reduction
        double instant_loss = loss / 255.0;
        smoothed_loss = 0.3 * instant_loss + 0.7 * smoothed_loss;
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
            printf("  [RAZOR] %d -> %d kbps (loss=%.1f%% rtt=%u)\n",
                   current_br, br, smoothed_loss * 100, rtt);
            current_br = br;
        }
    });
    razor_tx.set_bitrates(200, bitrate, 8000);

    // Razor receiver — sends feedback via channel with RTT timestamps
    std::atomic<uint32_t> last_rx_ts_ms{0};
    razor::Receiver razor_rx([&](const uint8_t* data, int size) {
        uint32_t now_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
        receiver_fb.send_feedback(0, 0, last_rx_ts_ms.load(), now_ms, data, size);
    });

    // Receiver pipeline
    GstElement *rx_pipeline, *rx_udpsrc, *rx_depay, *rx_parser, *rx_decoder, *rx_convert, *rx_sink;
    rx_pipeline = gst_pipeline_new("rx");
    rx_udpsrc   = gst_element_factory_make("udpsrc",       "rx-src");
    rx_depay    = gst_element_factory_make("rtph264depay", "rx-depay");
    rx_parser   = gst_element_factory_make("h264parse",    "rx-parser");
    rx_decoder  = gst_element_factory_make("avdec_h264",   "rx-decoder");
    rx_convert  = gst_element_factory_make("videoconvert", "rx-convert");
    rx_sink     = gst_element_factory_make("fakesink",     "rx-sink");

    GstCaps* rtp_caps = gst_caps_new_simple("application/x-rtp",
        "media", G_TYPE_STRING, "video", "encoding-name", G_TYPE_STRING, "H264",
        "clock-rate", G_TYPE_INT, 90000, nullptr);
    g_object_set(rx_udpsrc, "port", rtp_port, "caps", rtp_caps, nullptr);
    gst_caps_unref(rtp_caps);
    g_object_set(rx_sink, "sync", FALSE, "silent", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(rx_pipeline), rx_udpsrc, rx_depay, rx_parser, rx_decoder, rx_convert, rx_sink, nullptr);
    gst_element_link_many(rx_udpsrc, rx_depay, rx_parser, rx_decoder, rx_convert, rx_sink, nullptr);
    // NOTE: On GStreamer 1.16, decoder may stall under heavy loss.
    // The pipeline still works; fakesink drops frames silently.

    // Sender pipeline
    GstElement *tx_pipeline, *tx_src, *tx_conv, *tx_enc, *tx_parser, *tx_pay, *tx_sink;
    tx_pipeline = gst_pipeline_new("tx");
    tx_src      = gst_element_factory_make("videotestsrc",  "tx-src");
    tx_conv     = gst_element_factory_make("videoconvert",  "tx-conv");
    tx_enc      = gst_element_factory_make("x264enc",       "tx-enc");
    tx_parser   = gst_element_factory_make("h264parse",     "tx-parser");
    tx_pay      = gst_element_factory_make("rtph264pay",    "tx-pay");
    tx_sink     = gst_element_factory_make("udpsink",       "tx-sink");

    g_object_set(tx_src, "is-live", TRUE, "pattern", 1, nullptr);
    g_object_set(tx_enc, "tune", 0x4, "speed-preset", 1, "bitrate", bitrate,
        "key-int-max", 60, "bframes", 0, "threads", 2, "byte-stream", TRUE, nullptr);
    enc_ref = tx_enc;  // connect to razor callback for dynamic adjustment
    g_object_set(tx_pay, "pt", 96, "mtu", 1400, nullptr);
    g_object_set(tx_sink, "host", "127.0.0.1", "port", rtp_port, "sync", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(tx_pipeline), tx_src, tx_conv, tx_enc, tx_parser, tx_pay, tx_sink, nullptr);
    gst_element_link_many(tx_src, tx_conv, tx_enc, nullptr);
    GstCaps* h264_caps = gst_caps_new_simple("video/x-h264", "profile", G_TYPE_STRING, "baseline", nullptr);
    gst_element_link_filtered(tx_enc, tx_parser, h264_caps);
    gst_caps_unref(h264_caps);
    gst_element_link_many(tx_parser, tx_pay, tx_sink, nullptr);

    // Probes
    uint16_t tx_seq = 0, rx_seq = 0;
    uint64_t rx_pkt_count = 0;
    TxProbeData tx_pd = { &tx_seq };
    RxProbeData rx_pd = { &razor_rx, &rx_seq, &rx_pkt_count, &last_rx_ts_ms };

    GstPad* tx_src_pad = gst_element_get_static_pad(tx_pay, "src");
    gst_pad_add_probe(tx_src_pad, GST_PAD_PROBE_TYPE_BUFFER, rtp_out_probe, &tx_pd, nullptr);
    gst_object_unref(tx_src_pad);

    GstPad* rx_src_pad = gst_element_get_static_pad(rx_udpsrc, "src");
    gst_pad_add_probe(rx_src_pad, GST_PAD_PROBE_TYPE_BUFFER, rtp_in_probe, &rx_pd, nullptr);
    gst_object_unref(rx_src_pad);

    GstPad* rx_sink_pad = gst_element_get_static_pad(rx_sink, "sink");
    gst_pad_add_probe(rx_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, frame_count_probe, nullptr, nullptr);
    gst_object_unref(rx_sink_pad);

    // Start
    printf("Starting receiver...\n");
    gst_element_set_state(rx_pipeline, GST_STATE_PLAYING);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    printf("Starting sender...\n");
    gst_element_set_state(tx_pipeline, GST_STATE_PLAYING);
    printf("Both pipelines PLAYING\n\n");

    auto start = std::chrono::steady_clock::now();
    auto last_hb = start;
    int fb_count = 0;

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (elapsed >= duration) break;

        // Heartbeats
        if (std::chrono::duration<double>(now - last_hb).count() >= 0.005) {
            razor_tx.heartbeat();
            razor_rx.heartbeat();
            last_hb = now;
        }

        razor_tx.on_send(tx_seq, 1400);

        // Poll feedback
        gcc::FeedbackChannel::ReceivedFeedback fb;
        if (sender_fb.poll(fb)) {
            fb_count++;
            // Estimate RTT from feedback timestamps
            if (fb.rx_ts_ms > 0 && fb.fb_ts_ms > 0) {
                uint32_t now_ms = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
                uint32_t rtt_ms = now_ms - fb.rx_ts_ms;
                if (rtt_ms > 0 && rtt_ms < 5000) {
                    razor_tx.update_rtt(rtt_ms);
                }
            }
            if (fb.razor_size > 0) {
                razor_tx.on_feedback(fb.razor_data, fb.razor_size);
            }
        }

        int sec = static_cast<int>(elapsed);
        static int last_print = -1;
        if (sec != last_print) {
            last_print = sec;
            printf("  [%3ds] RX:%lu frames  Pkts:%lu  FB:%d\n",
                   sec, (unsigned long)demo_frame_count.load(),
                   (unsigned long)rx_pkt_count, fb_count);
        }
    }

    printf("\nStopping...\n");
    gst_element_set_state(tx_pipeline, GST_STATE_NULL);
    gst_element_set_state(rx_pipeline, GST_STATE_NULL);
    sender_fb.stop(); receiver_fb.stop();
    gst_object_unref(tx_pipeline); gst_object_unref(rx_pipeline);
    printf("Demo complete! %lu frames, %d feedback reports\n",
           (unsigned long)demo_frame_count.load(), fb_count);
    return 0;
}
