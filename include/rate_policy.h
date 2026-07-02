#pragma once

#include <cstdint>

namespace gcc {

enum class RateReason {
    Normal,
    DelayOveruse,
    Loss,
    ProbeAccepted,
};

struct RateInput {
    RateReason reason;
    uint32_t proposed_network_kbps;
    uint32_t rtt_ms;
    uint64_t now_ms;
};

struct RateDecision {
    bool changed;
    uint32_t network_kbps;
    uint32_t encoder_kbps;
};

class RatePolicy {
public:
    RatePolicy(
        uint32_t min_kbps, uint32_t max_kbps,
        double media_utilization = 0.92);

    void Reset(uint32_t network_kbps, uint64_t now_ms);
    RateDecision Apply(const RateInput& input);

private:
    uint32_t Clamp(uint32_t value) const noexcept;
    uint32_t EncoderRate() const noexcept;
    RateDecision Decision(uint32_t previous) const noexcept;

    uint32_t min_kbps_;
    uint32_t max_kbps_;
    double media_utilization_;
    uint32_t network_kbps_;
    double growth_cap_kbps_;
    uint64_t last_apply_ms_;
    uint64_t last_major_decrease_ms_;
    bool has_major_decrease_;
};

}  // namespace gcc
