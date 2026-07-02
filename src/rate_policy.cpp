#include "rate_policy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace gcc {
namespace {

constexpr double kDefaultMediaUtilization = 0.92;
constexpr double kGrowthPerSecond = 1.08;
constexpr uint32_t kMinimumRecoveryWaitMs = 50;

}  // namespace

RatePolicy::RatePolicy(
    uint32_t min_kbps, uint32_t max_kbps, double media_utilization)
    : min_kbps_(min_kbps),
      max_kbps_(max_kbps),
      media_utilization_(media_utilization),
      network_kbps_(min_kbps),
      growth_cap_kbps_(min_kbps),
      last_apply_ms_(0),
      last_major_decrease_ms_(0),
      has_major_decrease_(false) {
    if (min_kbps_ > max_kbps_) {
        std::swap(min_kbps_, max_kbps_);
    }
    if (!std::isfinite(media_utilization_) || media_utilization_ <= 0.0) {
        media_utilization_ = kDefaultMediaUtilization;
    } else if (media_utilization_ > 1.0) {
        media_utilization_ = 1.0;
    }
    network_kbps_ = min_kbps_;
    growth_cap_kbps_ = network_kbps_;
}

void RatePolicy::Reset(uint32_t network_kbps, uint64_t now_ms) {
    network_kbps_ = Clamp(network_kbps);
    growth_cap_kbps_ = network_kbps_;
    last_apply_ms_ = now_ms;
    last_major_decrease_ms_ = 0;
    has_major_decrease_ = false;
}

RateDecision RatePolicy::Apply(const RateInput& input) {
    const uint32_t previous = network_kbps_;
    const uint64_t event_now_ms =
        std::max(input.now_ms, last_apply_ms_);
    const uint32_t proposal = Clamp(input.proposed_network_kbps);
    const uint64_t elapsed_ms = event_now_ms - last_apply_ms_;

    switch (input.reason) {
        case RateReason::DelayOveruse:
        case RateReason::Loss:
            if (proposal < network_kbps_) {
                const uint32_t half_floor =
                    network_kbps_ / 2 + network_kbps_ % 2;
                network_kbps_ = Clamp(std::max(proposal, half_floor));
                if (network_kbps_ < previous) {
                    growth_cap_kbps_ = network_kbps_;
                    last_major_decrease_ms_ = event_now_ms;
                    has_major_decrease_ = true;
                }
            }
            break;

        case RateReason::ProbeAccepted:
            if (proposal > network_kbps_) {
                network_kbps_ = proposal;
            }
            growth_cap_kbps_ = network_kbps_;
            break;

        case RateReason::Normal:
            if (proposal > network_kbps_) {
                const uint64_t recovery_wait_ms =
                    std::max<uint64_t>(
                        input.rtt_ms, kMinimumRecoveryWaitMs);
                const bool recovery_allowed =
                    !has_major_decrease_ ||
                    (event_now_ms >= last_major_decrease_ms_ &&
                     event_now_ms - last_major_decrease_ms_ >=
                         recovery_wait_ms);
                if (recovery_allowed) {
                    if (growth_cap_kbps_ <
                        static_cast<double>(network_kbps_)) {
                        growth_cap_kbps_ = network_kbps_;
                    }
                    if (elapsed_ms > 0 && growth_cap_kbps_ > 0.0 &&
                        growth_cap_kbps_ <
                            static_cast<double>(max_kbps_)) {
                        const double factor = std::pow(
                            kGrowthPerSecond,
                            static_cast<double>(elapsed_ms) / 1000.0);
                        const double scaled = growth_cap_kbps_ * factor;
                        if (!std::isfinite(factor) ||
                            !std::isfinite(scaled) ||
                            scaled >= static_cast<double>(max_kbps_)) {
                            growth_cap_kbps_ = max_kbps_;
                        } else {
                            growth_cap_kbps_ = scaled;
                        }
                    }
                    const uint32_t growth_limit =
                        growth_cap_kbps_ >=
                                static_cast<double>(max_kbps_)
                            ? max_kbps_
                            : static_cast<uint32_t>(
                                  std::floor(growth_cap_kbps_));
                    network_kbps_ = Clamp(
                        std::min(proposal, growth_limit));
                    if (network_kbps_ == proposal &&
                        growth_cap_kbps_ >
                            static_cast<double>(proposal)) {
                        growth_cap_kbps_ = network_kbps_;
                    }
                }
            } else {
                growth_cap_kbps_ = network_kbps_;
            }
            break;
    }

    last_apply_ms_ = event_now_ms;
    return Decision(previous);
}

uint32_t RatePolicy::Clamp(uint32_t value) const noexcept {
    return std::max(min_kbps_, std::min(value, max_kbps_));
}

uint32_t RatePolicy::EncoderRate() const noexcept {
    const long double scaled =
        static_cast<long double>(network_kbps_) * media_utilization_;
    uint64_t rounded = static_cast<uint64_t>(std::round(scaled));
    rounded = std::min<uint64_t>(rounded, network_kbps_);
    return static_cast<uint32_t>(rounded);
}

RateDecision RatePolicy::Decision(uint32_t previous) const noexcept {
    return {
        network_kbps_ != previous,
        network_kbps_,
        EncoderRate(),
    };
}

}  // namespace gcc
