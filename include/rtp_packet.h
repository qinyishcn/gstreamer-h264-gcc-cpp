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
bool RewriteRtpSequence(
    uint8_t* data, size_t size, uint16_t sequence);
bool RewriteRtpSequence(std::vector<uint8_t>& packet, uint16_t sequence);
std::vector<uint8_t> BuildProbeRtpPacket(
    const RtpHeader& header, uint16_t sequence, size_t size);
bool IsProbeRtpPacket(const uint8_t* data, size_t size);

// Streaming-thread helper that removes received probe sequence numbers from
// the sequence space presented to a media depayloader.
class MediaSequenceTranslator {
public:
    void OnProbe(uint16_t transport_sequence);
    // Returns false without modifying media_sequence when the packet is older
    // than the bounded reorder history and cannot be translated reliably.
    bool TranslateMedia(
        uint16_t transport_sequence, uint16_t& media_sequence);

private:
    int64_t Extend(uint16_t transport_sequence);
    int64_t OldestTranslatable() const;
    void PruneProbes();

    static const int64_t kReorderWindow = 4096;

    bool transport_initialized_ = false;
    int64_t highest_transport_sequence_ = 0;
    bool media_initialized_ = false;
    int64_t highest_media_sequence_ = 0;
    uint64_t retired_probe_count_ = 0;
    std::vector<int64_t> probes_;
};

}  // namespace gcc
