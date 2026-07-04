#include "probe_controller.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gcc {
namespace {

constexpr double kDefaultFirstScale = 2.0;
constexpr double kDefaultNextScale = 1.5;
constexpr uint32_t kDefaultDurationMs = 100U;
constexpr double kDefaultSuccessRatio = 0.75;
constexpr double kMaximumLossFraction = 0.10;
constexpr uint64_t kResultDeadlineMs = 300U;

double NormalizeScale(double value, double fallback) {
    return std::isfinite(value) && value > 1.0 ? value : fallback;
}

double NormalizeSuccessRatio(double value) {
    return std::isfinite(value) && value > 0.0 && value <= 1.0
               ? value
               : kDefaultSuccessRatio;
}

bool HasElapsed(uint64_t now_ms, uint64_t start_ms, uint64_t interval_ms) {
    return now_ms >= start_ms && now_ms - start_ms >= interval_ms;
}

}  // namespace

ProbeController::ProbeController(const ProbeConfig& config)
    : config_(config),
      state_(ProbeState::Idle),
      active_probe_kbps_(0U),
      probe_start_ms_(0U),
      probe_bytes_sent_(0U),
      failure_time_ms_(0U),
      last_probe_kbps_(0U),
      accepted_time_ms_(0U),
      last_event_time_ms_(0U) {
    config_.first_scale =
        NormalizeScale(config_.first_scale, kDefaultFirstScale);
    config_.next_scale = NormalizeScale(config_.next_scale, kDefaultNextScale);
    if (config_.duration_ms == 0U ||
        static_cast<uint64_t>(config_.duration_ms) >= kResultDeadlineMs) {
        config_.duration_ms = kDefaultDurationMs;
    }
    config_.success_ratio = NormalizeSuccessRatio(config_.success_ratio);
}

ProbeAction ProbeController::OnStableTarget(uint32_t target_kbps,
                                            uint64_t now_ms) {
    if (state_ == ProbeState::Probing ||
        state_ == ProbeState::WaitingForResult) {
        return ProbeAction{};
    }
    const uint64_t event_time = NormalizeTime(now_ms);
    if (state_ == ProbeState::Failed &&
        !HasElapsed(event_time, failure_time_ms_,
                    static_cast<uint64_t>(config_.retry_interval_ms))) {
        return ProbeAction{};
    }

    const double scale =
        state_ == ProbeState::Accepted ? config_.next_scale : config_.first_scale;
    const uint32_t probe_kbps = CalculateProbeRate(target_kbps, scale);
    if (probe_kbps <= target_kbps) {
        return ProbeAction{};
    }
    if (state_ == ProbeState::Accepted &&
        probe_kbps <= last_probe_kbps_ &&
        !HasElapsed(event_time, accepted_time_ms_,
                    static_cast<uint64_t>(config_.retry_interval_ms))) {
        return ProbeAction{};
    }

    state_ = ProbeState::Probing;
    active_probe_kbps_ = probe_kbps;
    last_probe_kbps_ = probe_kbps;
    probe_start_ms_ = event_time;
    probe_bytes_sent_ = 0U;

    ProbeAction action;
    action.start = true;
    action.probe_kbps = probe_kbps;
    return action;
}

ProbeAction ProbeController::OnProbePacketSent(uint64_t bytes,
                                               uint64_t now_ms) {
    if (state_ != ProbeState::Probing) {
        return ProbeAction{};
    }
    ProbeAction action = AdvanceTime(NormalizeTime(now_ms));
    if (state_ != ProbeState::Probing) {
        return action;
    }
    const uint64_t room =
        std::numeric_limits<uint64_t>::max() - probe_bytes_sent_;
    probe_bytes_sent_ += std::min(bytes, room);
    return action;
}

ProbeAction ProbeController::OnFeedback(const ProbeFeedback& feedback) {
    if (state_ != ProbeState::Probing &&
        state_ != ProbeState::WaitingForResult) {
        return ProbeAction{};
    }

    const uint64_t event_time = NormalizeTime(feedback.now_ms);
    const bool was_probing = state_ == ProbeState::Probing;
    const double loss =
        std::isfinite(feedback.loss_fraction)
            ? std::max(0.0, feedback.loss_fraction)
            : std::numeric_limits<double>::infinity();
    if (feedback.overusing || loss > kMaximumLossFraction) {
        return Fail(event_time, was_probing);
    }

    ProbeAction action = AdvanceTime(event_time);
    if (state_ != ProbeState::Probing &&
        state_ != ProbeState::WaitingForResult) {
        return action;
    }
    if (state_ != ProbeState::WaitingForResult) {
        return action;
    }

    if (feedback.acked_kbps >= SuccessThreshold()) {
        action.accept = true;
        action.accepted_kbps =
            std::min(
                std::min(feedback.acked_kbps, active_probe_kbps_),
                config_.max_kbps);
        accepted_time_ms_ = event_time;
        state_ = ProbeState::Accepted;
        active_probe_kbps_ = 0U;
        return action;
    }

    return action;
}

ProbeAction ProbeController::OnTimer(uint64_t now_ms) {
    if (state_ != ProbeState::Probing &&
        state_ != ProbeState::WaitingForResult) {
        return ProbeAction{};
    }
    return AdvanceTime(NormalizeTime(now_ms));
}

ProbeAction ProbeController::Cancel(uint64_t now_ms) {
    const uint64_t event_ms = NormalizeTime(now_ms);
    if (state_ != ProbeState::Probing &&
        state_ != ProbeState::WaitingForResult) {
        return ProbeAction{};
    }
    return Fail(event_ms, true);
}

ProbeAction ProbeController::OnPacerQueueDelay(
    uint32_t queue_delay_ms, uint64_t now_ms) {
    constexpr uint32_t kMaximumPacerQueueMs = 250U;
    if (state_ != ProbeState::Probing ||
        queue_delay_ms <= kMaximumPacerQueueMs) {
        return ProbeAction{};
    }
    return Fail(NormalizeTime(now_ms), true);
}

ProbeState ProbeController::state() const {
    return state_;
}

uint32_t ProbeController::active_probe_kbps() const {
    return active_probe_kbps_;
}

uint64_t ProbeController::probe_start_ms() const {
    return probe_start_ms_;
}

uint64_t ProbeController::probe_bytes_sent() const {
    return probe_bytes_sent_;
}

uint64_t ProbeController::NormalizeTime(uint64_t now_ms) {
    last_event_time_ms_ = std::max(last_event_time_ms_, now_ms);
    return last_event_time_ms_;
}

ProbeAction ProbeController::AdvanceTime(uint64_t now_ms) {
    ProbeAction action;
    if ((state_ == ProbeState::Probing ||
         state_ == ProbeState::WaitingForResult) &&
        HasElapsed(now_ms, probe_start_ms_, kResultDeadlineMs)) {
        return Fail(now_ms, state_ == ProbeState::Probing);
    }
    if (state_ == ProbeState::Probing &&
        HasElapsed(now_ms, probe_start_ms_,
                   static_cast<uint64_t>(config_.duration_ms))) {
        state_ = ProbeState::WaitingForResult;
        action.stop = true;
    }
    return action;
}

ProbeAction ProbeController::Fail(uint64_t now_ms, bool stop) {
    state_ = ProbeState::Failed;
    active_probe_kbps_ = 0U;
    failure_time_ms_ = now_ms;
    ProbeAction action;
    action.stop = stop;
    return action;
}

uint32_t ProbeController::CalculateProbeRate(uint32_t target_kbps,
                                             double scale) const {
    if (config_.max_kbps == 0U || target_kbps >= config_.max_kbps) {
        return 0U;
    }
    const long double product =
        static_cast<long double>(target_kbps) * static_cast<long double>(scale);
    const long double maximum = static_cast<long double>(config_.max_kbps);
    if (!std::isfinite(product) || product >= maximum) {
        return config_.max_kbps;
    }
    const long double rounded = std::floor(product + 0.5L);
    if (rounded <= 0.0L) {
        return 0U;
    }
    return static_cast<uint32_t>(rounded);
}

uint32_t ProbeController::SuccessThreshold() const {
    const long double threshold =
        std::ceil(static_cast<long double>(active_probe_kbps_) *
                  static_cast<long double>(config_.success_ratio));
    const long double maximum =
        static_cast<long double>(std::numeric_limits<uint32_t>::max());
    return threshold >= maximum ? std::numeric_limits<uint32_t>::max()
                                : static_cast<uint32_t>(threshold);
}

}  // namespace gcc
