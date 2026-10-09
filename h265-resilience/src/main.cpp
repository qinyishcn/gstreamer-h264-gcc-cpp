#include "gst_experiment.hpp"
#include <gst/gst.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace resilience;
struct Mode {
    std::string name;
    int slices, gop;
    bool strict, hold;
};
const std::vector<Mode> modes{
    {"strict_black", 1, 120, true, false},
    {"native_black", 1, 120, false, false},
    {"native_hold", 1, 120, false, true},
    {"slices_hold", 4, 120, false, true},
    {"short_gop_hold", 1, 30, false, true},
    {"slices_short_gop_hold", 4, 30, false, true}};

static std::vector<std::string> split(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string part;
    while (std::getline(in, part, ',')) out.push_back(part);
    return out;
}

static void packet_manifest(const fs::path& path, const Encoded& e) {
    std::ofstream out(path);
    out << "ordinal,source_frame,rtp_bytes,sequence,rtp_timestamp,nal_type,marker\n";
    for (std::size_t i = 0; i < e.packets.size(); ++i) {
        const auto& p = e.packets[i];
        const auto& b = p.bytes;
        if (b.size() < 14) throw std::runtime_error("RTP packet too small");
        std::uint32_t ts = (std::uint32_t(b[4]) << 24) | (std::uint32_t(b[5]) << 16) |
                           (std::uint32_t(b[6]) << 8) | b[7];
        int nal = (b[12] >> 1) & 63;
        if (nal == 49 && b.size() > 14) nal = b[14] & 63;
        out << i << ',' << p.source_frame << ',' << b.size() << ','
            << ((unsigned(b[2]) << 8) | b[3]) << ',' << ts << ',' << nal << ','
            << (b[1] >> 7) << '\n';
    }
}

static void measure(const Clip& clip, const Encoded& encoded, const DecodeResult& decoded,
                    const Mode& mode, double loss, std::uint64_t seed,
                    std::ofstream& trials, std::ofstream& frame_csv, const fs::path& outdir,
                    bool save) {
    auto display = conceal(decoded.frames, clip.frames.size(), clip.width, clip.height, mode.hold);
    std::uint64_t sse_y = 0, sse_all = 0;
    std::size_t missing = 0, hold = 0, black = 0, corrupted = 0, run = 0, longest = 0;
    const std::size_t y_size = static_cast<std::size_t>(clip.width) * clip.height;
    for (std::size_t i = 0; i < display.size(); ++i) {
        const auto& f = display[i];
        auto sy = squared_error(clip.frames[i], f.pixels, 0, y_size);
        auto sa = squared_error(clip.frames[i], f.pixels, 0, y_size * 3 / 2);
        sse_y += sy;
        sse_all += sa;
        missing += f.inserted;
        hold += f.inserted && !f.black;
        black += f.black;
        corrupted += !f.inserted && f.corrupted;
        run = f.inserted ? run + 1 : 0;
        longest = std::max(longest, run);
        frame_csv << clip.name << ',' << mode.name << ',' << loss << ',' << seed << ',' << i
                  << ',' << psnr(sy, y_size) << ',' << psnr(sa, y_size * 3 / 2)
                  << ',' << f.inserted << ',' << f.black << ',' << f.corrupted << '\n';
    }
    const double seconds = static_cast<double>(clip.frames.size()) * clip.fps_d / clip.fps_n;
    const double py = psnr(sse_y, y_size * clip.frames.size());
    const double pall = psnr(sse_all, y_size * 3 / 2 * clip.frames.size());
    const auto outside = static_cast<std::size_t>(std::count_if(decoded.frames.begin(), decoded.frames.end(),
        [&](const DecodedFrame& f) { return f.index >= clip.frames.size(); }));
    // Actual source frames count is the denominator; no damaged/missing frame is excluded.
    trials << clip.name << ',' << mode.name << ',' << loss << ',' << seed << ','
           << clip.width << ',' << clip.height << ',' << clip.fps_n << ',' << clip.fps_d << ','
           << clip.frames.size() << ',' << encoded.packets.size() << ',' << decoded.lost << ','
           << static_cast<double>(decoded.lost) / encoded.packets.size() << ','
           << encoded.slices << ',' << encoded.gop << ',' << encoded.max_packet << ','
           << encoded.rtp_bytes * 8 / (seconds * 1000) << ','
           << (encoded.rtp_bytes + encoded.packets.size() * 28) * 8 / (seconds * 1000) << ','
           << display.size() - missing << ',' << decoded.frames.size() << ','
           << decoded.frames.size() - (display.size() - missing) - outside << ',' << outside << ','
           << missing << ',' << hold << ',' << black << ','
           << corrupted << ',' << longest * 1000.0 * clip.fps_d / clip.fps_n << ','
           << sse_y << ',' << sse_all << ',' << py << ',' << pall << '\n';
    trials.flush();
    frame_csv.flush();
    std::cout << clip.name << " " << mode.name << " loss=" << loss * 100 << "% seed=" << seed
              << " PSNR-Y=" << std::fixed << std::setprecision(2) << py
              << " decoded=" << display.size() - missing << '/' << clip.frames.size()
              << " hold=" << hold << " black=" << black << std::endl;
    if (save) {
        fs::create_directories(outdir / "examples");
        save_y4m((outdir / "examples" / (clip.name + "_" + mode.name + "_loss2_seed42.y4m")).string(),
                 clip, display);
    }
}

int main(int argc, char** argv) {
    try {
        gst_init(&argc, &argv);
        std::string input, name = "custom", data_dir, output;
        std::size_t frames = 300;
        int bitrate = 3000, mtu = 1500;
        bool save = false;
        std::vector<double> losses{0, .001, .005, .01, .02, .05, .1};
        std::vector<std::uint64_t> seeds{17, 42, 2026};
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--save-examples") { save = true; continue; }
            if (key == "--help") {
                std::cout << "h265_resilience --data-dir DIR --out DIR [--frames 300] [--bitrate 3000] "
                          << "[--ip-mtu 1500] [--losses 0,.001,.005,.01,.02,.05,.1] "
                          << "[--seeds 17,42,2026] [--save-examples]\n"
                          << "Custom sequence: --input video.y4m --name NAME --out DIR\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing argument for " + key);
            std::string value = argv[++i];
            if (key == "--input") input = value;
            else if (key == "--name") name = value;
            else if (key == "--data-dir") data_dir = value;
            else if (key == "--out") output = value;
            else if (key == "--frames") frames = std::stoull(value);
            else if (key == "--bitrate") bitrate = std::stoi(value);
            else if (key == "--ip-mtu") mtu = std::stoi(value);
            else if (key == "--losses") {
                losses.clear();
                for (const auto& v : split(value)) losses.push_back(std::stod(v));
            } else if (key == "--seeds") {
                seeds.clear();
                for (const auto& v : split(value)) seeds.push_back(std::stoull(v));
            } else throw std::invalid_argument("unknown option " + key);
        }
        if (output.empty() || (input.empty() && data_dir.empty()) ||
            (!input.empty() && !data_dir.empty()) || !frames || frames > 10000 ||
            bitrate < 1 || bitrate > 102400 || losses.empty() || seeds.empty())
            throw std::invalid_argument("invalid arguments; use --help");
        rtp_limit(mtu);
        for (double loss : losses) { LossMask validate(loss, 0); }
        require_plugins();
        fs::create_directories(output);
        const fs::path outdir(output);
        if (fs::exists(outdir / "trials.csv"))
            throw std::runtime_error("output already contains trials.csv; use a fresh output directory");
        std::ofstream trials(outdir / "trials.csv"), frame_csv(outdir / "frames.csv");
        if (!trials || !frame_csv) throw std::runtime_error("cannot open result CSV");
        trials << std::setprecision(12);
        frame_csv << std::setprecision(12);
        trials << "clip,mode,loss_probability,seed,width,height,fps_n,fps_d,source_frames,"
                  "sent_packets,lost_packets,actual_loss,slices,gop,max_rtp_bytes,rtp_kbps,ip_kbps,"
                  "decoded_frames,raw_decoder_outputs,duplicate_outputs,out_of_range_outputs,missing_frames,hold_frames,black_frames,flagged_corrupt_frames,"
                  "longest_no_output_ms,sse_y,sse_yuv,psnr_y,psnr_yuv\n";
        frame_csv << "clip,mode,loss_probability,seed,frame,psnr_y,psnr_yuv,inserted,black,corrupted\n";
        std::vector<std::pair<std::string, std::string>> inputs;
        if (!input.empty()) inputs.emplace_back(name, input);
        else for (const std::string n : {"FourPeople", "KristenAndSara"})
            inputs.emplace_back(n, (fs::path(data_dir) / (n + ".y4m")).string());
        for (const auto& item : inputs) {
            Clip clip = read_y4m(item.second, frames);
            clip.name = item.first;
            std::cout << "Loaded " << clip.name << ' ' << clip.width << 'x' << clip.height
                      << " frames=" << frames << " fps=" << clip.fps_n << '/' << clip.fps_d << std::endl;
            for (auto variant : {std::pair<int,int>{1,120}, {4,120}, {1,30}, {4,30}}) {
                auto encoded = encode(clip, variant.first, variant.second, bitrate, mtu);
                packet_manifest(outdir / (clip.name + "_s" + std::to_string(variant.first) +
                    "_g" + std::to_string(variant.second) + "_packets.csv"), encoded);
                for (double loss : losses) for (auto seed : seeds) {
                    auto normal = decode(clip, encoded, loss, seed, false);
                    if (loss == 0 && normal.frames.size() != clip.frames.size())
                        throw std::runtime_error("zero-loss decode lost source frames");
                    for (const auto& mode : modes) {
                        if (mode.slices != variant.first || mode.gop != variant.second) continue;
                        bool example = save && std::abs(loss - .02) < 1e-10 && seed == 42;
                        if (mode.strict) {
                            auto strict = decode(clip, encoded, loss, seed, true);
                            measure(clip, encoded, strict, mode, loss, seed, trials, frame_csv, outdir, example);
                        } else {
                            measure(clip, encoded, normal, mode, loss, seed, trials, frame_csv, outdir, example);
                        }
                    }
                }
            }
        }
        if (!trials || !frame_csv) throw std::runtime_error("result CSV write failed");
        std::cout << "Completed. Results: " << fs::absolute(outdir).string() << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
