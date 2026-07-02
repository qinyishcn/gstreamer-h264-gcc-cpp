#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gcc {

struct RtpHeader {
    bool padding = false;
    bool marker = false;
    uint8_t payload_type = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    size_t header_size = 0;
};

bool ParseRtpHeader(const uint8_t* data, size_t size, RtpHeader& header);
bool RewriteRtpSequence(std::vector<uint8_t>& packet, uint16_t sequence);
std::vector<uint8_t> BuildProbeRtpPacket(
    const RtpHeader& header, uint16_t sequence, size_t size);
bool IsProbeRtpPacket(const uint8_t* data, size_t size);

}  // namespace gcc
