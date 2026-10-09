#include "gst_experiment.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace resilience {
namespace {
class Pipeline {
public:
    explicit Pipeline(const std::string& description) {
        GError* error = nullptr;
        pipe_ = gst_parse_launch(description.c_str(), &error);
        if (error) {
            std::string message = error->message;
            g_error_free(error);
            if (pipe_) gst_object_unref(pipe_);
            throw std::runtime_error(message);
        }
        if (!pipe_) throw std::runtime_error("cannot create pipeline");
        src_ = gst_bin_get_by_name(GST_BIN(pipe_), "src");
        sink_ = gst_bin_get_by_name(GST_BIN(pipe_), "sink");
        bus_ = gst_element_get_bus(pipe_);
    }
    ~Pipeline() {
        gst_element_set_state(pipe_, GST_STATE_NULL);
        gst_object_unref(bus_);
        gst_object_unref(src_);
        gst_object_unref(sink_);
        gst_object_unref(pipe_);
    }
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    void run(const std::function<void(GstAppSrc*)>& feed,
             const std::function<void(GstSample*)>& collect) {
        if (gst_element_set_state(pipe_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("pipeline cannot enter PLAYING");
        std::exception_ptr feeder_error;
        std::thread producer([&] {
            try { feed(GST_APP_SRC(src_)); }
            catch (...) { feeder_error = std::current_exception(); }
            gst_app_src_end_of_stream(GST_APP_SRC(src_));
        });
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (true) {
                GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_),
                                                                25 * GST_MSECOND);
                if (sample) {
                    try { collect(sample); }
                    catch (...) { gst_sample_unref(sample); throw; }
                    gst_sample_unref(sample);
                } else if (gst_app_sink_is_eos(GST_APP_SINK(sink_))) break;
                GstMessage* msg = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR);
                if (msg) {
                    GError* error = nullptr;
                    gchar* debug = nullptr;
                    gst_message_parse_error(msg, &error, &debug);
                    std::string message = error->message;
                    g_error_free(error);
                    g_free(debug);
                    gst_message_unref(msg);
                    throw std::runtime_error(message);
                }
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("pipeline timed out after 120 seconds");
            }
        } catch (...) {
            gst_element_set_state(pipe_, GST_STATE_NULL);
            producer.join();
            throw;
        }
        producer.join();
        if (feeder_error) std::rethrow_exception(feeder_error);
    }
private:
    GstElement *pipe_ = nullptr, *src_ = nullptr, *sink_ = nullptr;
    GstBus* bus_ = nullptr;
};

GstBuffer* buffer_from(const Frame& bytes, std::uint64_t pts,
                       std::uint64_t duration = GST_CLOCK_TIME_NONE) {
    GstBuffer* b = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    gst_buffer_fill(b, 0, bytes.data(), bytes.size());
    GST_BUFFER_PTS(b) = pts;
    GST_BUFFER_DTS(b) = pts;
    GST_BUFFER_DURATION(b) = duration;
    return b;
}
void push(GstAppSrc* src, GstBuffer* b) {
    if (gst_app_src_push_buffer(src, b) != GST_FLOW_OK)
        throw std::runtime_error("appsrc push failed");
}
std::uint64_t time_for(std::size_t i, const Clip& c) {
    return gst_util_uint64_scale(i, GST_SECOND * c.fps_d, c.fps_n);
}
}

void require_plugins() {
    for (const char* name : {"appsrc", "appsink", "x265enc", "h265parse",
                              "rtph265pay", "rtph265depay", "avdec_h265", "videoconvert"}) {
        GstElementFactory* f = gst_element_factory_find(name);
        if (!f) throw std::runtime_error(std::string("missing GStreamer plugin: ") + name);
        gst_object_unref(f);
    }
}

Clip read_y4m(const std::string& path, std::size_t max_frames) {
    std::ifstream in(path, std::ios::binary);
    std::string header;
    if (!std::getline(in, header) || header.rfind("YUV4MPEG2 ", 0) != 0)
        throw std::runtime_error("invalid Y4M header: " + path);
    Clip c;
    std::istringstream tokens(header.substr(10));
    std::string t, chroma = "420jpeg";
    while (tokens >> t) {
        if (t[0] == 'W') c.width = std::stoi(t.substr(1));
        if (t[0] == 'H') c.height = std::stoi(t.substr(1));
        if (t[0] == 'F') {
            auto colon = t.find(':');
            c.fps_n = std::stoi(t.substr(1, colon - 1));
            c.fps_d = std::stoi(t.substr(colon + 1));
        }
        if (t[0] == 'C') chroma = t.substr(1);
        if (t[0] == 'I' && t != "Ip") throw std::runtime_error("only progressive Y4M supported");
    }
    if (chroma != "420" && chroma != "420jpeg" && chroma != "420mpeg2" && chroma != "420paldv")
        throw std::runtime_error("requires 8-bit 4:2:0 Y4M");
    Frame f = black_i420(c.width, c.height);
    if (c.fps_n <= 0 || c.fps_d <= 0) throw std::runtime_error("invalid Y4M framerate");
    while (c.frames.size() < max_frames && std::getline(in, t)) {
        if (t.rfind("FRAME", 0) != 0) throw std::runtime_error("invalid Y4M frame header");
        if (!in.read(reinterpret_cast<char*>(f.data()), f.size())) break;
        c.frames.push_back(f);
    }
    if (c.frames.size() != max_frames)
        throw std::runtime_error("Y4M prefix too short for requested frame count");
    return c;
}

Encoded encode(const Clip& c, int slices, int gop, int bitrate, int ip_mtu) {
    std::ostringstream s;
    s << "appsrc name=src format=time block=true max-bytes=8388608 "
      << "caps=video/x-raw,format=I420,width=" << c.width << ",height=" << c.height
      << ",framerate=" << c.fps_n << '/' << c.fps_d
      << ",colorimetry=bt709 ! x265enc speed-preset=ultrafast tune=zerolatency "
      << "bitrate=" << bitrate << " key-int-max=" << gop
      << " option-string=\"pools=4:frame-threads=1:scenecut=0:bframes=0:open-gop=0:"
      << "repeat-headers=1:slices=" << slices << "\" ! h265parse ! rtph265pay "
      << "mtu=" << rtp_limit(ip_mtu)
      << " config-interval=-1 aggregate-mode=none pt=96 seqnum-offset=0 timestamp-offset=0 "
      << "! appsink name=sink sync=false async=false max-buffers=32";
    Pipeline p(s.str());
    Encoded out;
    out.slices = slices;
    out.gop = gop;
    bool first = true;
    std::uint32_t first_timestamp = 0;
    p.run([&](GstAppSrc* src) {
        for (std::size_t i = 0; i < c.frames.size(); ++i)
            push(src, buffer_from(c.frames[i], time_for(i, c), time_for(i+1, c)-time_for(i, c)));
    }, [&](GstSample* sample) {
        GstBuffer* b = gst_sample_get_buffer(sample);
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        if (!gst_rtp_buffer_map(b, GST_MAP_READ, &rtp))
            throw std::runtime_error("encoder produced invalid RTP");
        auto ts = gst_rtp_buffer_get_timestamp(&rtp);
        gst_rtp_buffer_unmap(&rtp);
        if (first) { first_timestamp = ts; first = false; }
        std::uint32_t elapsed = ts - first_timestamp;
        Packet packet;
        packet.source_frame = static_cast<std::size_t>(std::llround(
            static_cast<long double>(elapsed) * c.fps_n / (90000.L * c.fps_d)));
        if (packet.source_frame >= c.frames.size())
            throw std::runtime_error("encoded RTP timestamp out of source range");
        packet.pts = time_for(packet.source_frame, c);
        packet.bytes.resize(gst_buffer_get_size(b));
        gst_buffer_extract(b, 0, packet.bytes.data(), packet.bytes.size());
        if (packet.bytes.size() > rtp_limit(ip_mtu)) throw std::runtime_error("RTP exceeds MTU");
        out.max_packet = std::max(out.max_packet, packet.bytes.size());
        out.rtp_bytes += packet.bytes.size();
        out.packets.push_back(std::move(packet));
    });
    return out;
}

DecodeResult decode(const Clip& c, const Encoded& e, double loss,
                    std::uint64_t seed, bool strict) {
    LossMask mask(loss, seed);
    std::vector<const Packet*> survivors;
    DecodeResult out;
    for (const auto& packet : e.packets) {
        if (mask.drop()) ++out.lost;
        else survivors.push_back(&packet);
    }
    if (survivors.empty()) return out;
    std::ostringstream s;
    s << "appsrc name=src format=time block=true max-bytes=8388608 "
      << "caps=application/x-rtp,media=video,encoding-name=H265,clock-rate=90000,payload=96 "
      << "! rtph265depay request-keyframe=false wait-for-keyframe=false "
      << "! video/x-h265,stream-format=byte-stream,alignment=au "
      << "! h265parse ! avdec_h265 max-errors=-1 max-threads=1 "
      << "output-corrupt=" << (strict ? "false" : "true")
      << " discard-corrupted-frames=" << (strict ? "true" : "false")
      << " ! videoconvert ! video/x-raw,format=I420 "
      << "! appsink name=sink sync=false async=false max-buffers=8";
    Pipeline p(s.str());
    p.run([&](GstAppSrc* src) {
        const Packet* previous = nullptr;
        for (const Packet* packet : survivors) {
            GstBuffer* b = buffer_from(packet->bytes, packet->pts);
            if (!previous || packet != previous + 1) GST_BUFFER_FLAG_SET(b, GST_BUFFER_FLAG_DISCONT);
            push(src, b);
            previous = packet;
        }
    }, [&](GstSample* sample) {
        GstBuffer* b = gst_sample_get_buffer(sample);
        if (!GST_BUFFER_PTS_IS_VALID(b)) throw std::runtime_error("decoder output without PTS");
        DecodedFrame f;
        f.index = frame_index(GST_BUFFER_PTS(b), c.fps_n, c.fps_d);
        f.corrupted = GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_CORRUPTED);
        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)))
            throw std::runtime_error("invalid decoder video caps");
        if (GST_VIDEO_INFO_WIDTH(&info) != c.width || GST_VIDEO_INFO_HEIGHT(&info) != c.height)
            throw std::runtime_error("decoder changed dimensions");
        GstVideoFrame vf;
        if (!gst_video_frame_map(&vf, &info, b, GST_MAP_READ))
            throw std::runtime_error("cannot map decoded frame");
        f.pixels.resize(static_cast<std::size_t>(c.width) * c.height * 3 / 2);
        std::size_t offset = 0;
        for (int plane = 0; plane < 3; ++plane) {
            const int w = plane ? c.width / 2 : c.width;
            const int h = plane ? c.height / 2 : c.height;
            auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, plane));
            int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, plane);
            for (int y = 0; y < h; ++y) {
                std::memcpy(f.pixels.data() + offset, data + y * stride, w);
                offset += w;
            }
        }
        gst_video_frame_unmap(&vf);
        out.frames.push_back(std::move(f));
    });
    return out;
}

void save_y4m(const std::string& path, const Clip& c, const std::vector<DisplayFrame>& frames) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << "YUV4MPEG2 W" << c.width << " H" << c.height << " F" << c.fps_n << ':'
        << c.fps_d << " Ip A1:1 C420jpeg\n";
    for (const auto& f : frames) {
        out << "FRAME\n";
        out.write(reinterpret_cast<const char*>(f.pixels.data()), f.pixels.size());
    }
    if (!out) throw std::runtime_error("failed writing " + path);
}
}
