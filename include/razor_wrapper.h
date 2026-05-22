#pragma once
/**
 * C++ Wrapper for razor GCC library (yuanrongxi/razor)
 * Uses razor as the bandwidth estimator; GStreamer handles actual packet transmission.
 *
 * NOTE: razor API uses bps (bits per second) internally.
 * This wrapper converts kbps <-> bps at the boundary.
 */
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

extern "C" {
#include "razor_api.h"
}

namespace razor {

static constexpr uint32_t kBpsPerKbps = 1000;

// Feedback data buffer (razor feedback is variable-length binary)
struct FeedbackData {
    uint8_t data[256];
    int     size = 0;
};

// ── Sender Wrapper ──────────────────────────────────────────────────
class Sender {
public:
    // bitrate_changed_cb: called when razor estimates a new bitrate (kbps)
    Sender(std::function<void(uint32_t kbps, uint8_t loss, uint32_t rtt)> bitrate_changed_cb) {
        bitrate_cb_ = bitrate_changed_cb;
        sender_ = razor_sender_create(
            0,  // gcc_congestion
            0,  // no padding
            this,
            [](void* trigger, uint32_t bitrate_bps, uint8_t loss, uint32_t rtt) {
                auto* self = static_cast<Sender*>(trigger);
                // Convert bps -> kbps for the callback
                if (self->bitrate_cb_)
                    self->bitrate_cb_(bitrate_bps / kBpsPerKbps, loss, rtt);
            },
            nullptr,  // handler (unused, no pacer)
            nullptr,  // pace_send_cb (unused, GStreamer sends)
            0         // queue_ms (unused)
        );
    }

    ~Sender() {
        if (sender_) razor_sender_destroy(sender_);
    }

    bool is_valid() const { return sender_ != nullptr; }

    // Call every 5-10ms
    void heartbeat() {
        if (sender_) sender_->heartbeat(sender_);
    }

    // Notify razor that a packet was sent (transport_seq = RTP seq)
    void on_send(uint16_t transport_seq, size_t size) {
        if (sender_) sender_->on_send(sender_, transport_seq, size);
    }

    // Feed receiver feedback to razor
    void on_feedback(const uint8_t* data, int size) {
        if (sender_) sender_->on_feedback(sender_, const_cast<uint8_t*>(data), size);
    }

    // Update RTT (milliseconds) — call this when you have a real RTT measurement
    void update_rtt(uint32_t rtt_ms) {
        if (sender_) sender_->update_rtt(sender_, static_cast<int32_t>(rtt_ms));
    }

    // Update bitrate range (all parameters in kbps)
    void set_bitrates(uint32_t min_kbps, uint32_t start_kbps, uint32_t max_kbps) {
        if (sender_) {
            // Convert kbps -> bps for razor API
            sender_->set_bitrates(sender_,
                min_kbps * kBpsPerKbps,
                start_kbps * kBpsPerKbps,
                max_kbps * kBpsPerKbps);
        }
    }

private:
    razor_sender_t* sender_ = nullptr;
    std::function<void(uint32_t kbps, uint8_t loss, uint32_t rtt)> bitrate_cb_;
};

// ── Receiver Wrapper ────────────────────────────────────────────────
class Receiver {
public:
    // send_feedback_cb: called when razor generates feedback to send back
    Receiver(std::function<void(const uint8_t* data, int size)> send_feedback_cb) {
        feedback_cb_ = send_feedback_cb;
        receiver_ = razor_receiver_create(
            0,  // gcc_congestion
            200 * kBpsPerKbps,   // min_bitrate (bps)
            8000 * kBpsPerKbps,  // max_bitrate (bps)
            12,    // packet_header_size (RTP header)
            this,
            [](void* handler, const uint8_t* payload, int payload_size) {
                auto* self = static_cast<Receiver*>(handler);
                if (self->feedback_cb_) self->feedback_cb_(payload, payload_size);
            }
        );
    }

    ~Receiver() {
        if (receiver_) razor_receiver_destroy(receiver_);
    }

    bool is_valid() const { return receiver_ != nullptr; }

    // Call every 5ms
    void heartbeat() {
        if (receiver_) receiver_->heartbeat(receiver_);
    }

    // Notify razor of a received packet (remb=1 for delay-based estimation)
    void on_received(uint16_t transport_seq, uint32_t timestamp, size_t size) {
        if (receiver_) receiver_->on_received(receiver_, transport_seq, timestamp, size, 1);
    }

    // Update RTT (milliseconds)
    void update_rtt(uint32_t rtt_ms) {
        if (receiver_) receiver_->update_rtt(receiver_, static_cast<int32_t>(rtt_ms));
    }

private:
    razor_receiver_t* receiver_ = nullptr;
    std::function<void(const uint8_t* data, int size)> feedback_cb_;
};

} // namespace razor
