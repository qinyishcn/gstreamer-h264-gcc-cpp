#include "rtp_packet.h"

#include <algorithm>

namespace gcc {
namespace {

constexpr uint8_t kRtpVersion = 2;
constexpr size_t kFixedHeaderSize = 12;
constexpr size_t kProbeMagicSize = 4;
constexpr size_t kMinimumProbeSize =
    kFixedHeaderSize + kProbeMagicSize + 1;
constexpr size_t kMaximumUdpPayloadSize = 1472;
constexpr uint8_t kProbeMagic[kProbeMagicSize] = {'G', 'C', 'C', 'P'};

uint16_t ReadUint16(const uint8_t* data) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) |
        static_cast<uint16_t>(data[1]));
}

uint32_t ReadUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void WriteUint16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void WriteUint32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

}  // namespace

bool ParseRtpHeader(const uint8_t* data, size_t size, RtpHeader& header) {
    if (data == nullptr || size < kFixedHeaderSize) {
        return false;
    }
    if ((data[0] >> 6) != kRtpVersion) {
        return false;
    }

    const size_t csrc_size = static_cast<size_t>(data[0] & 0x0f) * 4;
    size_t header_size = kFixedHeaderSize;
    if (csrc_size > size - header_size) {
        return false;
    }
    header_size += csrc_size;

    if ((data[0] & 0x10) != 0) {
        constexpr size_t kExtensionPrefixSize = 4;
        if (kExtensionPrefixSize > size - header_size) {
            return false;
        }
        const size_t extension_size =
            static_cast<size_t>(ReadUint16(data + header_size + 2)) * 4;
        header_size += kExtensionPrefixSize;
        if (extension_size > size - header_size) {
            return false;
        }
        header_size += extension_size;
    }

    const bool padding = (data[0] & 0x20) != 0;
    if (padding) {
        const uint8_t padding_size = data[size - 1];
        if (padding_size == 0 ||
            static_cast<size_t>(padding_size) > size - header_size) {
            return false;
        }
    }

    RtpHeader parsed;
    parsed.padding = padding;
    parsed.marker = (data[1] & 0x80) != 0;
    parsed.payload_type = data[1] & 0x7f;
    parsed.sequence = ReadUint16(data + 2);
    parsed.timestamp = ReadUint32(data + 4);
    parsed.ssrc = ReadUint32(data + 8);
    parsed.header_size = header_size;
    header = parsed;
    return true;
}

bool RewriteRtpSequence(std::vector<uint8_t>& packet, uint16_t sequence) {
    RtpHeader header;
    if (!ParseRtpHeader(packet.data(), packet.size(), header)) {
        return false;
    }
    WriteUint16(packet.data() + 2, sequence);
    return true;
}

std::vector<uint8_t> BuildProbeRtpPacket(
    const RtpHeader& header, uint16_t sequence, size_t size) {
    if (size < kMinimumProbeSize || size > kMaximumUdpPayloadSize) {
        return {};
    }

    std::vector<uint8_t> packet(size, 0);
    packet[0] = static_cast<uint8_t>((kRtpVersion << 6) | 0x20);
    packet[1] = header.payload_type & 0x7f;
    WriteUint16(packet.data() + 2, sequence);
    WriteUint32(packet.data() + 4, header.timestamp);
    WriteUint32(packet.data() + 8, header.ssrc);
    std::copy(kProbeMagic, kProbeMagic + kProbeMagicSize,
              packet.begin() + kFixedHeaderSize);
    packet.back() = 1;
    return packet;
}

bool IsProbeRtpPacket(const uint8_t* data, size_t size) {
    RtpHeader header;
    if (!ParseRtpHeader(data, size, header) || !header.padding) {
        return false;
    }

    const size_t padding_size = data[size - 1];
    if (kProbeMagicSize > size - header.header_size ||
        padding_size > size - header.header_size - kProbeMagicSize) {
        return false;
    }
    return std::equal(
        kProbeMagic, kProbeMagic + kProbeMagicSize,
        data + header.header_size);
}

}  // namespace gcc
