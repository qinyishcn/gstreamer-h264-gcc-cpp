#pragma once

/**
 * Google Congestion Control (GCC) - C++ Implementation
 *
 * Based on RFC 8298: Google Congestion Control for Real-time Web Communication
 *
 * Components:
 * 1. Delay-based bandwidth estimation (overuse detector + Kalman filter)
 * 2. Loss-based bandwidth estimation
 * 3. AIMD (Additive Increase Multiplicative Decrease) rate control
 */

#include <cstdint>
#include <vector>
#include <deque>
#include <mutex>

namespace gcc {

enum class State : uint8_t {
    HOLD     = 0,
    INCREASE = 1,
    DECREASE = 2,
};

struct FeedbackReport {
    uint64_t timestamp_us;          // Feedback timestamp in microseconds
    double   delay_ms;              // One-way delay estimate in ms
    double   loss_fraction;         // Packet loss fraction [0.0, 1.0]
};

struct Stats {
    int    total_steps;
    int    total_increases;
    int    total_decreases;
    int    current_bitrate_kbps;
    State  current_state;
    double loss_fraction;
    double smoothing_slope;
};

class Controller {
public:
    explicit Controller(
        int initial_bitrate_kbps  = 2000,
        int min_bitrate_kbps      = 200,
        int max_bitrate_kbps      = 8000,
        double gradient_threshold = 0.4,
        double hold_threshold     = 0.15,
        double kalman_gain        = 0.1,
        double ai_step_kbps       = 50.0,
        double md_factor          = 0.85
    );

    /**
     * Process a feedback report and return the new target bitrate in kbps.
     */
    int step(const FeedbackReport& report);

    // Getters
    int   get_bitrate() const { return current_bitrate_kbps_; }
    State get_state()  const { return current_state_; }
    Stats get_stats()  const;

private:
    void update_overuse_detector();

    // Configuration
    double gradient_threshold_;
    double hold_threshold_;
    double kalman_gain_;
    double ai_step_kbps_;
    double md_factor_;
    int    min_bitrate_kbps_;
    int    max_bitrate_kbps_;

    // State
    int    current_bitrate_kbps_;
    State  current_state_;
    double smoothing_slope_;
    double prev_delay_ms_;
    double loss_fraction_;

    // Gradient history
    struct DelayGradient {
        double gradient;
        double timestamp;
    };
    std::deque<DelayGradient> delay_gradients_;
    static constexpr size_t kMaxGradientHistory = 60;

    // Statistics
    int total_steps_;
    int total_increases_;
    int total_decreases_;
};

} // namespace gcc
