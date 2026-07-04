#include "outer_loss_backoff.h"

#include <algorithm>

namespace gcc {

OuterLossBackoff::OuterLossBackoff(uint32_t min_kbps)
    : min_kbps_(min_kbps) {}

OuterLossDecision OuterLossBackoff::Evaluate(
    uint8_t loss_q8, uint32_t current_kbps, uint32_t,
    uint64_t, bool enabled, bool base_controller_reduced) {
    if (!enabled) return {};
    if (loss_q8 <= 25U) {
        high_loss_episode_ = false;
        return {};
    }

    // A base-controller reduction already handled this feedback. Keep the
    // emergency fallback armed in case high loss persists after the base
    // controller stops producing lower targets.
    if (base_controller_reduced) return {};
    if (high_loss_episode_) return {};
    high_loss_episode_ = true;
    if (current_kbps <= min_kbps_) return {};
    const uint32_t proposed =
        std::max(min_kbps_, current_kbps / 2U + current_kbps % 2U);
    return {proposed < current_kbps, proposed};
}

}  // namespace gcc
