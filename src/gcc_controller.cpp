#include "gcc_controller.h"
#include <cmath>
#include <algorithm>
#include <chrono>

namespace gcc {

Controller::Controller(
    int initial_bitrate_kbps,
    int min_bitrate_kbps,
    int max_bitrate_kbps,
    double gradient_threshold,
    double hold_threshold,
    double kalman_gain,
    double ai_step_kbps,
    double md_factor)
    : gradient_threshold_(gradient_threshold)
    , hold_threshold_(hold_threshold)
    , kalman_gain_(kalman_gain)
    , ai_step_kbps_(ai_step_kbps)
    , md_factor_(md_factor)
    , min_bitrate_kbps_(min_bitrate_kbps)
    , max_bitrate_kbps_(max_bitrate_kbps)
    , current_bitrate_kbps_(initial_bitrate_kbps)
    , current_state_(State::HOLD)
    , smoothing_slope_(0.0)
    , prev_delay_ms_(-1.0)
    , loss_fraction_(0.0)
    , total_steps_(0)
    , total_increases_(0)
    , total_decreases_(0)
{
}

int Controller::step(const FeedbackReport& report) {
    total_steps_++;

    // Update loss estimate
    loss_fraction_ = report.loss_fraction;

    // Compute delay gradient
    if (prev_delay_ms_ >= 0.0) {
        double delay_gradient = report.delay_ms - prev_delay_ms_;

        // Store gradient
        delay_gradients_.push_back({delay_gradient, static_cast<double>(report.timestamp_us)});

        // Trim history
        while (delay_gradients_.size() > kMaxGradientHistory) {
            delay_gradients_.pop_front();
        }

        // Update overuse detector
        update_overuse_detector();
    }

    prev_delay_ms_ = report.delay_ms;

    // Apply rate control based on state
    switch (current_state_) {
        case State::DECREASE:
            current_bitrate_kbps_ = static_cast<int>(
                current_bitrate_kbps_ * md_factor_);
            total_decreases_++;
            break;

        case State::INCREASE:
            current_bitrate_kbps_ += static_cast<int>(ai_step_kbps_);
            total_increases_++;
            break;

        case State::HOLD:
            // Keep current bitrate
            break;
    }

    // Clamp bitrate
    current_bitrate_kbps_ = std::max(min_bitrate_kbps_,
                                      std::min(max_bitrate_kbps_, current_bitrate_kbps_));

    return current_bitrate_kbps_;
}

void Controller::update_overuse_detector() {
    if (delay_gradients_.empty()) return;

    // Compute weighted average of recent gradients
    size_t count = std::min<size_t>(10, delay_gradients_.size());
    double total_weight = 0.0;
    double weighted_sum  = 0.0;

    for (size_t i = 0; i < count; ++i) {
        double weight = 1.0 + static_cast<double>(i) * 0.1;
        weighted_sum  += delay_gradients_[delay_gradients_.size() - count + i].gradient * weight;
        total_weight  += weight;
    }

    double avg_gradient = (total_weight > 0.0) ? (weighted_sum / total_weight) : 0.0;

    // Kalman filter: smooth the gradient estimate
    double error = avg_gradient - smoothing_slope_;
    smoothing_slope_ += kalman_gain_ * error;

    // State transitions
    if (std::abs(smoothing_slope_) < hold_threshold_) {
        if (current_state_ == State::DECREASE) {
            current_state_ = State::INCREASE;  // AIMD: after decrease, start increasing
        } else if (current_state_ != State::INCREASE) {
            current_state_ = State::HOLD;
        }
    } else if (smoothing_slope_ > gradient_threshold_) {
        current_state_ = State::DECREASE;  // Overuse detected
    } else if (smoothing_slope_ < -gradient_threshold_) {
        current_state_ = State::INCREASE;  // Underuse detected
    }
}

Stats Controller::get_stats() const {
    Stats s;
    s.total_steps        = total_steps_;
    s.total_increases    = total_increases_;
    s.total_decreases    = total_decreases_;
    s.current_bitrate_kbps = current_bitrate_kbps_;
    s.current_state      = current_state_;
    s.loss_fraction      = loss_fraction_;
    s.smoothing_slope    = smoothing_slope_;
    return s;
}

} // namespace gcc
