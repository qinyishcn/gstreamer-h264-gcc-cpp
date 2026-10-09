#include "gst_experiment.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace resilience;
namespace {
struct GstDeleter {
    void operator()(GstElement* e) const {
        if (e) { gst_element_set_state(e, GST_STATE_NULL); gst_object_unref(e); }
    }
};
using Pipe = std::unique_ptr<GstElement, GstDeleter>;
Pipe make_pipe(const std::string& desc) {
    GError* error = nullptr;
    Pipe p(gst_parse_launch(desc.c_str(), &error));
    if (error) {
        std::string message(error->message);
        g_error_free(error);
        throw std::runtime_error(message);
    }
    if (!p) throw std::runtime_error("cannot create live pipeline");
    return p;
}
void start(GstElement* p) {
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("cannot start live pipeline");
}
void check_bus(GstElement* p) {
    GstBus* bus = gst_element_get_bus(p);
    GstMessage* m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!m) return;
    GError* e = nullptr;
    gchar* detail = nullptr;
    gst_message_parse_error(m, &e, &detail);
    std::string message(e->message);
    g_error_free(e);
    g_free(detail);
    gst_message_unref(m);
    throw std::runtime_error(message);
}
void push_frame(GstElement* src, const Frame& frame, std::uint64_t pts, std::uint64_t duration) {
    GstBuffer* b = gst_buffer_new_allocate(nullptr, frame.size(), nullptr);
    gst_buffer_fill(b, 0, frame.data(), frame.size());
    GST_BUFFER_PTS(b) = pts;
    GST_BUFFER_DURATION(b) = duration;
    if (gst_app_src_push_buffer(GST_APP_SRC(src), b) != GST_FLOW_OK)
        throw std::runtime_error("live appsrc stopped");
}
struct Impairment {
    LossMask loss;
    std::size_t packets = 0, lost = 0;
    explicit Impairment(double p, std::uint64_t seed) : loss(p, seed) {}
    bool drop() { ++packets; bool d = loss.drop(); lost += d; return d; }
};
GstPadProbeReturn impair(GstPad*, GstPadProbeInfo* info, gpointer userdata) {
    auto& state = *static_cast<Impairment*>(userdata);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
        return state.drop() ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList* list = gst_buffer_list_make_writable(GST_PAD_PROBE_INFO_BUFFER_LIST(info));
        GST_PAD_PROBE_INFO_DATA(info) = list;
        guint i = 0;
        while (i < gst_buffer_list_length(list)) {
            if (state.drop()) gst_buffer_list_remove(list, i, 1);
            else ++i;
        }
        return gst_buffer_list_length(list) ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
}
Frame copy_sample(GstSample* s, int w, int h) {
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(s)) ||
        GST_VIDEO_INFO_WIDTH(&info) != w || GST_VIDEO_INFO_HEIGHT(&info) != h)
        throw std::runtime_error("receiver resolution differs from --width/--height");
    GstVideoFrame vf;
    if (!gst_video_frame_map(&vf, &info, gst_sample_get_buffer(s), GST_MAP_READ))
        throw std::runtime_error("cannot map live decoded frame");
    Frame f(static_cast<std::size_t>(w) * h * 3 / 2);
    std::size_t offset = 0;
    for (int plane = 0; plane < 3; ++plane) {
        int pw = plane ? w / 2 : w, ph = plane ? h / 2 : h;
        auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, plane));
        int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, plane);
        for (int y = 0; y < ph; ++y) {
            std::memcpy(f.data() + offset, data + y * stride, pw);
            offset += pw;
        }
    }
    gst_video_frame_unmap(&vf);
    return f;
}
}

int main(int argc, char** argv) {
    try {
        gst_init(&argc, &argv);
        std::string role, input, host = "127.0.0.1", record;
        int port = 5004, w = 1280, h = 720, fps = 60, seconds = 7;
        int gop = 30, slices = 4, bitrate = 3000, mtu = 1500, latency = 80;
        std::size_t frames = 300;
        double loss = 0;
        std::uint64_t seed = 42;
        bool headless = false;
        for (int i = 1; i < argc; ++i) {
            std::string key(argv[i]);
            if (key == "--headless") { headless = true; continue; }
            if (key == "--help") {
                std::cout << "h265_live --role sender --input video.y4m [--host 127.0.0.1 --port 5004] "
                          << "[--frames 300 --slices 4 --gop 30 --bitrate 3000 --ip-mtu 1500] "
                          << "[--loss .02 --seed 42]\n"
                          << "h265_live --role receiver [--port 5004 --width 1280 --height 720 --fps 60] "
                          << "[--seconds 7 --latency-ms 80 --record out.y4m --headless]\n";
                return 0;
            }
            if (++i >= argc) throw std::invalid_argument("missing value for " + key);
            std::string v(argv[i]);
            if (key == "--role") role = v;
            else if (key == "--input") input = v;
            else if (key == "--host") host = v;
            else if (key == "--port") port = std::stoi(v);
            else if (key == "--width") w = std::stoi(v);
            else if (key == "--height") h = std::stoi(v);
            else if (key == "--fps") fps = std::stoi(v);
            else if (key == "--seconds") seconds = std::stoi(v);
            else if (key == "--gop") gop = std::stoi(v);
            else if (key == "--slices") slices = std::stoi(v);
            else if (key == "--bitrate") bitrate = std::stoi(v);
            else if (key == "--ip-mtu") mtu = std::stoi(v);
            else if (key == "--latency-ms") latency = std::stoi(v);
            else if (key == "--frames") frames = std::stoull(v);
            else if (key == "--loss") loss = std::stod(v);
            else if (key == "--seed") seed = std::stoull(v);
            else if (key == "--record") record = v;
            else throw std::invalid_argument("unknown option " + key);
        }
        if (port < 1 || port > 65535 || fps < 1 || fps > 240 || seconds < 1 ||
            seconds > 3600 || gop < 1 || slices < 1 || slices > 16 || bitrate < 1 ||
            latency < 0 || latency > 10000 || !frames)
            throw std::invalid_argument("invalid live option range");
        rtp_limit(mtu);
        require_plugins();
        if (role == "sender") {
            Clip clip = read_y4m(input, frames);
            Impairment state(loss, seed);
            std::ostringstream s;
            s << "appsrc name=src is-live=true format=time block=true max-bytes=8388608 "
              << "caps=video/x-raw,format=I420,width=" << clip.width << ",height=" << clip.height
              << ",framerate=" << clip.fps_n << '/' << clip.fps_d << ",colorimetry=bt709 "
              << "! x265enc speed-preset=ultrafast tune=zerolatency bitrate=" << bitrate
              << " key-int-max=" << gop << " option-string=\"pools=4:frame-threads=1:"
              << "scenecut=0:bframes=0:open-gop=0:repeat-headers=1:slices=" << slices << "\" "
              << "! h265parse ! rtph265pay config-interval=-1 aggregate-mode=none mtu=" << rtp_limit(mtu)
              << " ! identity name=impair ! udpsink host=\"" << host << "\" port=" << port
              << " sync=true async=false";
            auto pipe = make_pipe(s.str());
            GstElement* impairment = gst_bin_get_by_name(GST_BIN(pipe.get()), "impair");
            GstPad* pad = gst_element_get_static_pad(impairment, "sink");
            gulong probe = gst_pad_add_probe(pad, static_cast<GstPadProbeType>(
                GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST), impair, &state, nullptr);
            GstElement* src = gst_bin_get_by_name(GST_BIN(pipe.get()), "src");
            start(pipe.get());
            for (std::size_t i = 0; i < clip.frames.size(); ++i) {
                check_bus(pipe.get());
                auto pts = gst_util_uint64_scale(i, GST_SECOND * clip.fps_d, clip.fps_n);
                auto end = gst_util_uint64_scale(i+1, GST_SECOND * clip.fps_d, clip.fps_n);
                push_frame(src, clip.frames[i], pts, end - pts);
            }
            gst_app_src_end_of_stream(GST_APP_SRC(src));
            GstBus* bus = gst_element_get_bus(pipe.get());
            GstMessage* end = gst_bus_timed_pop_filtered(bus,
                (static_cast<std::uint64_t>(clip.frames.size()) * clip.fps_d / clip.fps_n + 30) * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (!end) throw std::runtime_error("sender EOS timeout");
            bool error = GST_MESSAGE_TYPE(end) == GST_MESSAGE_ERROR;
            gst_message_unref(end);
            gst_object_unref(bus);
            gst_element_set_state(pipe.get(), GST_STATE_NULL);
            gst_pad_remove_probe(pad, probe);
            gst_object_unref(pad);
            gst_object_unref(impairment);
            gst_object_unref(src);
            if (error) throw std::runtime_error("sender pipeline error");
            std::cout << "sent_packet_attempts=" << state.packets << " dropped=" << state.lost << '\n';
        } else if (role == "receiver") {
            black_i420(w, h);
            std::ostringstream s;
            s << "udpsrc port=" << port << " buffer-size=4194304 "
              << "caps=application/x-rtp,media=video,encoding-name=H265,clock-rate=90000,payload=96 "
              << "! rtpjitterbuffer latency=" << latency << " do-lost=true "
              << "! rtph265depay wait-for-keyframe=false request-keyframe=false "
              << "! video/x-h265,alignment=au ! h265parse "
              << "! avdec_h265 output-corrupt=true discard-corrupted-frames=false max-errors=-1 max-threads=1 "
              << "! videoconvert ! video/x-raw,format=I420 "
              << "! appsink name=decoded sync=false async=false max-buffers=4 drop=true";
            auto receive = make_pipe(s.str());
            GstElement* sink = gst_bin_get_by_name(GST_BIN(receive.get()), "decoded");
            std::ostringstream o;
            o << "appsrc name=src is-live=true format=time block=true "
              << "caps=video/x-raw,format=I420,width=" << w << ",height=" << h << ",framerate=" << fps << "/1 ";
            if (!record.empty())
                o << "! y4menc ! filesink sync=false async=false location=\"" << record << "\"";
            else if (headless) o << "! fakesink sync=false async=false";
            else o << "! videoconvert ! autovideosink sync=false";
            auto output = make_pipe(o.str());
            GstElement* source = gst_bin_get_by_name(GST_BIN(output.get()), "src");
            start(receive.get());
            std::cout << "receiver_ready port=" << port << std::endl;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            auto next = std::chrono::steady_clock::now();
            std::size_t emitted = 0, received = 0, repeated = 0;
            Frame last;
            bool started = false;
            while (std::chrono::steady_clock::now() < deadline) {
                check_bus(receive.get());
                check_bus(output.get());
                bool fresh = false;
                while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) {
                    try { last = copy_sample(sample, w, h); }
                    catch (...) { gst_sample_unref(sample); throw; }
                    gst_sample_unref(sample);
                    fresh = true;
                    ++received;
                }
                if (!last.empty()) {
                    if (!started) { start(output.get()); started = true; next = std::chrono::steady_clock::now(); }
                    auto pts = gst_util_uint64_scale(emitted, GST_SECOND, fps);
                    auto end = gst_util_uint64_scale(emitted+1, GST_SECOND, fps);
                    push_frame(source, last, pts, end - pts);
                    ++emitted;
                    repeated += !fresh;
                }
                next += std::chrono::nanoseconds(GST_SECOND / fps);
                std::this_thread::sleep_until(next);
            }
            gst_app_src_end_of_stream(GST_APP_SRC(source));
            if (started) {
                GstBus* bus = gst_element_get_bus(output.get());
                GstMessage* done = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
                gst_object_unref(bus);
                if (!done) throw std::runtime_error("receiver output EOS timeout");
                bool error = GST_MESSAGE_TYPE(done) == GST_MESSAGE_ERROR;
                gst_message_unref(done);
                if (error) throw std::runtime_error("receiver output error");
            }
            gst_object_unref(source);
            gst_object_unref(sink);
            std::cout << "decoded_outputs=" << received << " display_frames=" << emitted
                      << " repeated_frames=" << repeated << " startup_no_image=" << !started << '\n';
        } else throw std::invalid_argument("--role must be sender or receiver; use --help");
        return 0;
    } catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << '\n'; return 1; }
}
