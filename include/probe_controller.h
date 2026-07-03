#pragma once

#include <cstdint>

namespace gcc {

enum class ProbeState {
    Idle,
    Probing,
    WaitingForResult,
    Accepted,
    Failed
};

struct ProbeConfig {
    double first_scale = 2.0;
    double next_scale = 1.5;
    uint32_t duration_ms = 100U;
    double success_ratio = 0.75;
    uint32_t retry_interval_ms = 5000U;
    uint32_t max_kbps = 8000U;
};

struct ProbeAction {
    bool start = false;
    bool stop = false;
    bool accept = false;
    uint32_t probe_kbps = 0U;
    uint32_t accepted_kbps = 0U;
};

struct ProbeFeedback {
    uint32_t acked_kbps;
    double loss_fraction;
    bool overusing;
    uint64_t now_ms;
};

class ProbeController {
public:
    explicit ProbeController(const ProbeConfig& config = ProbeConfig());

    ProbeAction OnStableTarget(uint32_t target_kbps, uint64_t now_ms);
    ProbeAction OnProbePacketSent(uint64_t bytes, uint64_t now_ms);
    ProbeAction OnFeedback(const ProbeFeedback& feedback);
    ProbeAction OnTimer(uint64_t now_ms);

    ProbeState state() const;
    uint32_t active_probe_kbps() const;
    uint64_t probe_start_ms() const;
    uint64_t probe_bytes_sent() const;

private:
    uint64_t NormalizeTime(uint64_t now_ms);
    ProbeAction AdvanceTime(uint64_t now_ms);
    ProbeAction Fail(uint64_t now_ms, bool stop);
    uint32_t CalculateProbeRate(uint32_t target_kbps, double scale) const;
    uint32_t SuccessThreshold() const;

    ProbeConfig config_;
    ProbeState state_;
    uint32_t active_probe_kbps_;
    uint64_t probe_start_ms_;
    uint64_t probe_bytes_sent_;
    uint64_t failure_time_ms_;
    uint32_t last_probe_kbps_;
    uint64_t accepted_time_ms_;
    uint64_t last_event_time_ms_;
};

}  // namespace gcc
