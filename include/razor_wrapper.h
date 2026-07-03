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
#include <limits>
#include <vector>

extern "C" {
#include "razor_api.h"
}

namespace razor {

static constexpr uint32_t kBpsPerKbps = 1000;

namespace detail {

constexpr uint32_t KbpsToBps(uint32_t kbps) {
    return kbps > std::numeric_limits<uint32_t>::max() / kBpsPerKbps
               ? std::numeric_limits<uint32_t>::max()
               : kbps * kBpsPerKbps;
}

constexpr int QueueMsToInt(uint32_t queue_ms) {
    return queue_ms > static_cast<uint32_t>(std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max()
               : static_cast<int>(queue_ms);
}

}  // namespace detail

using PaceSendCallback = std::function<void(uint32_t, int, size_t, bool)>;

struct SenderStats {
    uint32_t acked_bitrate_kbps = 0;
    int delay_state = 0;
    uint64_t matched_feedback_packets = 0;
    int64_t pacer_queue_ms = 0;
};

// Feedback data buffer (razor feedback is variable-length binary)
struct FeedbackData {
    uint8_t data[256];
    int     size = 0;
};

// ── Sender Wrapper ──────────────────────────────────────────────────
class Sender {
public:
    // Sender, its methods, and all callbacks must run on one serialized
    // control thread. Bitrate and pacing callbacks must not destroy Sender.
    // bitrate_changed_cb: called when razor estimates a new bitrate (kbps)
    Sender(std::function<void(uint32_t kbps, uint8_t loss, uint32_t rtt)> bitrate_changed_cb,
           PaceSendCallback pace_send_cb = PaceSendCallback(),
           uint32_t queue_ms = 250) {
        bitrate_cb_ = bitrate_changed_cb;
        pace_cb_ = pace_send_cb;
        sender_ = razor_sender_create(
            0,  // gcc_congestion
            0,  // no padding
            this,
            [](void* trigger, uint32_t bitrate_bps, uint8_t loss, uint32_t rtt) {
                auto* self = static_cast<Sender*>(trigger);
                // Convert bps -> kbps for the callback
                try {
                    if (self->bitrate_cb_)
                        self->bitrate_cb_(
                            bitrate_bps / kBpsPerKbps, loss, rtt);
                } catch (...) {
                    // Never allow a C++ exception to cross the C callback ABI.
                }
                self->RestoreExplicitPacingRate();
            },
            this,
            [](void* handler, uint32_t packet_id, int retrans, size_t size,
               int padding) {
                auto* self = static_cast<Sender*>(handler);
                if (!self || self->in_pace_callback_)
                    return;
                self->in_pace_callback_ = true;
                try {
                    if (self->pace_cb_)
                        self->pace_cb_(packet_id, retrans, size, padding != 0);
                } catch (...) {
                    // Never allow a C++ exception to cross the C callback ABI.
                }
                self->in_pace_callback_ = false;
            },
            detail::QueueMsToInt(queue_ms)
        );
    }

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&&) = delete;
    Sender& operator=(Sender&&) = delete;

    ~Sender() {
        if (sender_) razor_sender_destroy(sender_);
    }

    bool is_valid() const { return sender_ != nullptr; }

    // Call every 5-10ms
    void heartbeat() {
        if (sender_ && !in_pace_callback_) sender_->heartbeat(sender_);
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
                detail::KbpsToBps(min_kbps),
                detail::KbpsToBps(start_kbps),
                detail::KbpsToBps(max_kbps));
            RestoreExplicitPacingRate();
        }
    }

    void set_pacing_rate(uint32_t bitrate_kbps) {
        configured_pacing_rate_kbps_ = bitrate_kbps;
        has_explicit_pacing_rate_ = true;
        RestoreExplicitPacingRate();
    }

    bool add_packet(uint32_t packet_id, bool retrans, size_t size) {
        return sender_ && sender_->add_packet &&
               sender_->add_packet(sender_, packet_id, retrans ? 1 : 0, size) == 0;
    }

    int64_t pacer_queue_ms() const {
        if (!sender_ || !sender_->get_pacer_queue_ms)
            return 0;
        const int64_t queue_ms = sender_->get_pacer_queue_ms(sender_);
        return queue_ms > 0 ? queue_ms : 0;
    }

    SenderStats stats() const {
        SenderStats result;
        razor_sender_stats_t raw = {};
        if (razor_sender_get_stats(sender_, &raw) == 0) {
            result.acked_bitrate_kbps =
                raw.acked_bitrate_bps / kBpsPerKbps;
            result.delay_state = raw.delay_state;
            result.matched_feedback_packets =
                raw.matched_feedback_packets;
            result.pacer_queue_ms =
                raw.pacer_queue_ms > 0 ? raw.pacer_queue_ms : 0;
        }
        return result;
    }

private:
    void RestoreExplicitPacingRate() {
        if (sender_ && has_explicit_pacing_rate_) {
            razor_sender_set_pacing_rate(
                sender_,
                detail::KbpsToBps(configured_pacing_rate_kbps_));
        }
    }

    razor_sender_t* sender_ = nullptr;
    std::function<void(uint32_t kbps, uint8_t loss, uint32_t rtt)> bitrate_cb_;
    PaceSendCallback pace_cb_;
    bool in_pace_callback_ = false;
    bool has_explicit_pacing_rate_ = false;
    uint32_t configured_pacing_rate_kbps_ = 0U;
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

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    Receiver(Receiver&&) = delete;
    Receiver& operator=(Receiver&&) = delete;

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
