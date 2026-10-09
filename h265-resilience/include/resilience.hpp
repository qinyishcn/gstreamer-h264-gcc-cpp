#pragma once
#include <cstdint>
#include <random>
#include <vector>

namespace resilience {
using Frame = std::vector<std::uint8_t>;
struct DecodedFrame { std::size_t index; Frame pixels; bool corrupted; };
struct DisplayFrame { Frame pixels; bool inserted; bool black; bool corrupted; };
std::size_t rtp_limit(std::size_t ip_mtu);
class LossMask {
public:
    LossMask(double probability, std::uint64_t seed);
    bool drop();
private:
    double probability_;
    std::mt19937_64 generator_;
};
std::uint64_t squared_error(const Frame&, const Frame&, std::size_t begin,
                            std::size_t count);
double psnr(long double sse, std::uint64_t samples);
Frame black_i420(int width, int height);
std::size_t frame_index(std::uint64_t pts, int fps_n, int fps_d);
std::vector<DisplayFrame> conceal(const std::vector<DecodedFrame>&,
                                std::size_t count, int width, int height,
                                bool hold);
}
