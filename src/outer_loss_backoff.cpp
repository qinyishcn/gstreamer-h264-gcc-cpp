#include "outer_loss_backoff.h"

#include <algorithm>

namespace gcc {

OuterLossBackoff::OuterLossBackoff(uint32_t min_kbps)
    : min_kbps_(min_kbps) {}

OuterLossDecision OuterLossBackoff::Evaluate(
    uint8_t loss_q8, uint32_t current_kbps, uint32_t rtt_ms,
    uint64_t now_ms, bool enabled) {
    if (!enabled) return {};
    const uint64_t event_ms = std::max(now_ms, last_event_ms_);
    last_event_ms_ = event_ms;
    if (loss_q8 <= 25U) {
        high_loss_episode_ = false;
        last_backoff_ms_ = 0U;
        return {};
    }

    const uint64_t interval_ms =
        std::max<uint64_t>(250U, rtt_ms);
    if (high_loss_episode_ &&
        event_ms - last_backoff_ms_ < interval_ms) {
        return {};
    }
    high_loss_episode_ = true;
    last_backoff_ms_ = event_ms;
    if (current_kbps <= min_kbps_) return {};
    const uint32_t proposed =
        std::max(min_kbps_, current_kbps / 2U + current_kbps % 2U);
    return {proposed < current_kbps, proposed};
}

}  // namespace gcc
