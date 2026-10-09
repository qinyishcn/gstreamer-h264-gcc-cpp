#pragma once
#include "resilience.hpp"
#include <string>

namespace resilience {
struct Clip {
    std::string name;
    int width = 0, height = 0, fps_n = 0, fps_d = 1;
    std::vector<Frame> frames;
};
struct Packet {
    Frame bytes;
    std::uint64_t pts = 0;
    std::size_t source_frame = 0;
};
struct Encoded {
    std::vector<Packet> packets;
    std::size_t max_packet = 0;
    std::uint64_t rtp_bytes = 0;
    int slices = 1, gop = 120;
};
struct DecodeResult {
    std::vector<DecodedFrame> frames;
    std::size_t lost = 0;
};
Clip read_y4m(const std::string& path, std::size_t max_frames);
Encoded encode(const Clip&, int slices, int gop, int bitrate, int ip_mtu);
DecodeResult decode(const Clip&, const Encoded&, double loss,
                    std::uint64_t seed, bool strict);
void save_y4m(const std::string&, const Clip&, const std::vector<DisplayFrame>&);
void require_plugins();
}
