#pragma once

#include "transport_sender.h"

#include <cstdint>

namespace gcc {

class ProbeDeliveryEstimator {
public:
    void Start(uint32_t probe_kbps);
    void OnProbePacketSent(const SentPacketInfo& packet);
    void ObserveFeedback(
        const SentPacketInfo& packet, uint32_t receiver_receive_ms);
    bool BeginFeedback(const SentPacketInfo& packet);
    bool BeginUnmatchedFeedback(const SentHistoryWatermark& watermark);
    void Finish();

    uint32_t estimate_kbps() const;
    bool active() const;
    bool rate_events_guarded() const;

private:
    bool IsProbeGeneration(const SentPacketInfo& packet) const;

    struct Point {
        uint64_t cumulative_bytes = 0U;
        uint64_t send_time_us = 0U;
        uint64_t generation = 0U;
        uint32_t receiver_receive_ms = 0U;
        bool valid = false;
    };

    Point first_probe_;
    Point receiver_baseline_;
    bool active_ = false;
    bool awaiting_first_probe_ = false;
    bool rate_guard_active_ = false;
    bool has_last_probe_packet_ = false;
    uint64_t last_probe_cumulative_bytes_ = 0U;
    uint64_t last_probe_generation_ = 0U;
    uint32_t estimate_kbps_ = 0U;
};

}  // namespace gcc
