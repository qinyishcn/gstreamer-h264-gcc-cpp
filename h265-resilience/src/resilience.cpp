#include "resilience.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace resilience {
std::size_t rtp_limit(std::size_t mtu) {
    if (mtu < 64 || mtu > 65535) throw std::invalid_argument("invalid IP MTU");
    return mtu - 20 - 8;
}
LossMask::LossMask(double probability, std::uint64_t seed)
    : probability_(probability), generator_(seed) {
    if (!std::isfinite(probability) || probability < 0 || probability > 1)
        throw std::invalid_argument("loss probability must be in [0,1]");
}
bool LossMask::drop() {
    // Define the conversion explicitly: repeatable across standard libraries.
    double u = static_cast<double>(generator_() >> 11) * 0x1.0p-53;
    return u < probability_;
}
std::uint64_t squared_error(const Frame& a, const Frame& b, std::size_t begin,
                            std::size_t count) {
    if (a.size() != b.size() || begin > a.size() || count > a.size() - begin)
        throw std::invalid_argument("frame/SSE range mismatch");
    std::uint64_t sse = 0;
    for (std::size_t i = begin; i < begin + count; ++i) {
        int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        sse += static_cast<std::uint64_t>(d * d);
    }
    return sse;
}
double psnr(long double sse, std::uint64_t samples) {
    if (!samples || sse < 0) throw std::invalid_argument("invalid PSNR samples");
    if (sse == 0) return std::numeric_limits<double>::infinity();
    return static_cast<double>(10 * std::log10(255.L * 255.L * samples / sse));
}
Frame black_i420(int w, int h) {
    if (w <= 0 || h <= 0 || w % 2 || h % 2)
        throw std::invalid_argument("I420 requires positive even dimensions");
    std::size_t y = static_cast<std::size_t>(w) * h;
    Frame result(y * 3 / 2, 128);
    std::fill_n(result.begin(), y, 16);
    return result;
}
std::size_t frame_index(std::uint64_t pts, int n, int d) {
    if (n <= 0 || d <= 0) throw std::invalid_argument("invalid fps");
    return static_cast<std::size_t>(std::llround(
        static_cast<long double>(pts) * n / (1000000000.L * d)));
}
std::vector<DisplayFrame> conceal(const std::vector<DecodedFrame>& decoded,
                                std::size_t count, int w, int h, bool hold) {
    const Frame black = black_i420(w, h);
    std::vector<const DecodedFrame*> slots(count, nullptr);
    for (const auto& f : decoded) {
        // An extrapolated PTS after a damaged final AU is outside the experiment
        // timeline. Do not shift it onto a valid reference frame to improve PSNR.
        if (f.index >= count) continue;
        if (f.pixels.size() != black.size()) throw std::runtime_error("decoder frame size changed");
        // Corrupt HEVC AUs can yield duplicate decoder PTS. A presentation slot
        // is displayed once: the first output wins, later duplicates are counted
        // separately by the experiment, not credited as new source frames.
        if (!slots[f.index]) slots[f.index] = &f;
    }
    std::vector<DisplayFrame> out;
    out.reserve(count);
    const DecodedFrame* previous = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        if (slots[i]) {
            previous = slots[i];
            out.push_back({previous->pixels, false, false, previous->corrupted});
        } else if (hold && previous) {
            out.push_back({previous->pixels, true, false, previous->corrupted});
        } else {
            out.push_back({black, true, true, false});
        }
    }
    return out;
}
}
