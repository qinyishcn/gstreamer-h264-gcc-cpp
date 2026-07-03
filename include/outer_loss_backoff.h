#pragma once

#include <cstdint>

namespace gcc {

struct OuterLossDecision {
    bool changed = false;
    uint32_t proposed_kbps = 0U;
};

class OuterLossBackoff {
public:
    explicit OuterLossBackoff(uint32_t min_kbps);

    OuterLossDecision Evaluate(
        uint8_t loss_q8, uint32_t current_kbps, uint32_t rtt_ms,
        uint64_t now_ms, bool enabled);

private:
    uint32_t min_kbps_;
    bool high_loss_episode_ = false;
    uint64_t last_backoff_ms_ = 0U;
    uint64_t last_event_ms_ = 0U;
};

}  // namespace gcc
