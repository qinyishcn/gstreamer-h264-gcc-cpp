#include "rtp_packet.h"

#include <algorithm>

namespace gcc {
namespace {

constexpr uint8_t kRtpVersion = 2;
constexpr size_t kFixedHeaderSize = 12;
constexpr size_t kProbeMagicSize = 4;
constexpr size_t kMinimumProbeSize =
    kFixedHeaderSize + kProbeMagicSize + 1;
constexpr size_t kMaximumProbeSize =
    kFixedHeaderSize + static_cast<size_t>(UINT8_MAX);
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

bool RewriteRtpSequence(
    uint8_t* data, size_t size, uint16_t sequence) {
    RtpHeader header;
    if (!ParseRtpHeader(data, size, header)) {
        return false;
    }
    WriteUint16(data + 2, sequence);
    return true;
}

bool RewriteRtpSequence(std::vector<uint8_t>& packet, uint16_t sequence) {
    return RewriteRtpSequence(
        packet.data(), packet.size(), sequence);
}

std::vector<uint8_t> BuildProbeRtpPacket(
    const RtpHeader& header, uint16_t sequence, size_t size) {
    if (size < kMinimumProbeSize || size > kMaximumProbeSize) {
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
    packet.back() =
        static_cast<uint8_t>(size - kFixedHeaderSize);
    return packet;
}

bool IsProbeRtpPacket(const uint8_t* data, size_t size) {
    RtpHeader header;
    if (!ParseRtpHeader(data, size, header) || !header.padding) {
        return false;
    }

    const size_t padding_size = data[size - 1];
    if (size - header.header_size != padding_size ||
        padding_size < kProbeMagicSize + 1U) {
        return false;
    }
    return std::equal(
        kProbeMagic, kProbeMagic + kProbeMagicSize,
        data + header.header_size);
}

void MediaSequenceTranslator::OnProbe(uint16_t transport_sequence) {
    const int64_t extended = Extend(transport_sequence);
    PruneProbes();

    if (media_initialized_ && extended <= highest_media_sequence_) {
        return;
    }
    if (extended < OldestTranslatable()) {
        return;
    }

    const auto position =
        std::lower_bound(probes_.begin(), probes_.end(), extended);
    if (position == probes_.end() || *position != extended) {
        probes_.insert(position, extended);
    }
}

bool MediaSequenceTranslator::TranslateMedia(
    uint16_t transport_sequence, uint16_t& media_sequence) {
    const int64_t extended = Extend(transport_sequence);
    PruneProbes();
    if (extended < OldestTranslatable()) {
        return false;
    }
    if (!media_initialized_ || extended > highest_media_sequence_) {
        media_initialized_ = true;
        highest_media_sequence_ = extended;
    }

    const auto position =
        std::lower_bound(probes_.begin(), probes_.end(), extended);
    const uint64_t probe_count =
        retired_probe_count_ +
        static_cast<uint64_t>(position - probes_.begin());
    media_sequence = static_cast<uint16_t>(
        transport_sequence - static_cast<uint16_t>(probe_count));
    return true;
}

int64_t MediaSequenceTranslator::Extend(uint16_t transport_sequence) {
    const int64_t kSequenceRange = int64_t{1} << 16;
    const int64_t kHalfRange = kSequenceRange / 2;
    if (!transport_initialized_) {
        transport_initialized_ = true;
        highest_transport_sequence_ = transport_sequence;
        return highest_transport_sequence_;
    }

    const int64_t epoch =
        (highest_transport_sequence_ / kSequenceRange) * kSequenceRange;
    int64_t extended =
        epoch + static_cast<int64_t>(transport_sequence);
    if (extended > highest_transport_sequence_) {
        if (extended - highest_transport_sequence_ > kHalfRange) {
            extended -= kSequenceRange;
        }
    } else if (highest_transport_sequence_ - extended > kHalfRange) {
        if (extended <= INT64_MAX - kSequenceRange) {
            extended += kSequenceRange;
        }
    }

    if (extended > highest_transport_sequence_) {
        highest_transport_sequence_ = extended;
    }
    return extended;
}

int64_t MediaSequenceTranslator::OldestTranslatable() const {
    return highest_transport_sequence_ - (kReorderWindow - 1);
}

void MediaSequenceTranslator::PruneProbes() {
    const int64_t retained_floor = OldestTranslatable();
    const auto first_retained =
        std::lower_bound(probes_.begin(), probes_.end(), retained_floor);
    retired_probe_count_ +=
        static_cast<uint64_t>(first_retained - probes_.begin());
    probes_.erase(probes_.begin(), first_retained);
}

}  // namespace gcc
