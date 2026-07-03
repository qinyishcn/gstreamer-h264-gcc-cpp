#include "probe_delivery_estimator.h"

#include <algorithm>
#include <limits>

namespace gcc {
namespace {

uint32_t Saturate(uint64_t value) {
    return value > std::numeric_limits<uint32_t>::max()
               ? std::numeric_limits<uint32_t>::max()
               : static_cast<uint32_t>(value);
}

}  // namespace

void ProbeDeliveryEstimator::Start(uint32_t) {
    active_ = true;
    awaiting_first_probe_ = true;
    rate_guard_active_ = true;
    has_last_probe_packet_ = false;
    first_probe_ = Point{};
    receiver_baseline_ = Point{};
    estimate_kbps_ = 0U;
}

void ProbeDeliveryEstimator::OnProbePacketSent(
    const SentPacketInfo& packet) {
    if (!active_ || !packet.probe) {
        return;
    }
    if (awaiting_first_probe_) {
        first_probe_.cumulative_bytes = packet.cumulative_sent_bytes;
        first_probe_.send_time_us = packet.send_time_us;
        first_probe_.generation = packet.generation;
        first_probe_.valid = true;
        awaiting_first_probe_ = false;
    }
    has_last_probe_packet_ = true;
    last_probe_cumulative_bytes_ = packet.cumulative_sent_bytes;
    last_probe_generation_ = packet.generation;
}

bool ProbeDeliveryEstimator::BeginFeedback(
    const SentPacketInfo& packet) {
    if (active_) {
        return first_probe_.valid &&
               packet.generation >= first_probe_.generation;
    }
    if (!has_last_probe_packet_) {
        rate_guard_active_ = false;
        return false;
    }
    if (rate_guard_active_) {
        if (packet.generation > last_probe_generation_ &&
            packet.cumulative_sent_bytes >=
                last_probe_cumulative_bytes_) {
            rate_guard_active_ = false;
        } else {
            return true;
        }
    }
    return IsProbeGeneration(packet);
}

bool ProbeDeliveryEstimator::BeginUnmatchedFeedback(
    const SentHistoryWatermark& watermark) {
    const bool related = active_ || rate_guard_active_;
    if (!active_ && rate_guard_active_ && has_last_probe_packet_ &&
        watermark.valid &&
        watermark.oldest_generation > last_probe_generation_ &&
        watermark.newest_generation > last_probe_generation_ &&
        watermark.cumulative_sent_bytes >= last_probe_cumulative_bytes_) {
        rate_guard_active_ = false;
    }
    return related;
}

bool ProbeDeliveryEstimator::IsProbeGeneration(
    const SentPacketInfo& packet) const {
    return packet.probe && first_probe_.valid && has_last_probe_packet_ &&
           packet.generation >= first_probe_.generation &&
           packet.generation <= last_probe_generation_;
}

void ProbeDeliveryEstimator::ObserveFeedback(
    const SentPacketInfo& packet, uint32_t receiver_receive_ms) {
    Point current;
    current.cumulative_bytes = packet.cumulative_sent_bytes;
    current.send_time_us = packet.send_time_us;
    current.generation = packet.generation;
    current.receiver_receive_ms = receiver_receive_ms;
    current.valid = true;

    if (!active_ || awaiting_first_probe_ || !first_probe_.valid ||
        current.generation < first_probe_.generation ||
        current.cumulative_bytes < first_probe_.cumulative_bytes ||
        current.send_time_us < first_probe_.send_time_us) {
        return;
    }
    if (!receiver_baseline_.valid) {
        receiver_baseline_ = current;
        return;
    }
    if (current.generation <= receiver_baseline_.generation ||
        current.cumulative_bytes <= receiver_baseline_.cumulative_bytes ||
        current.send_time_us <= first_probe_.send_time_us) {
        return;
    }

    const uint64_t send_delta_us =
        current.send_time_us - first_probe_.send_time_us;
    const uint32_t receive_delta_ms =
        current.receiver_receive_ms -
        receiver_baseline_.receiver_receive_ms;
    if (send_delta_us < UINT64_C(20000) ||
        send_delta_us > UINT64_C(10000000) ||
        receive_delta_ms < 20U ||
        receive_delta_ms > UINT32_C(10000)) {
        return;
    }
    const uint64_t send_bytes =
        current.cumulative_bytes - first_probe_.cumulative_bytes;
    const uint64_t receive_bytes =
        current.cumulative_bytes - receiver_baseline_.cumulative_bytes;
    const uint64_t send_kbps =
        send_bytes > std::numeric_limits<uint64_t>::max() / UINT64_C(8000)
            ? std::numeric_limits<uint64_t>::max()
            : send_bytes * UINT64_C(8000) / send_delta_us;
    const uint64_t receive_kbps =
        receive_bytes > std::numeric_limits<uint64_t>::max() / UINT64_C(8)
            ? std::numeric_limits<uint64_t>::max()
            : receive_bytes * UINT64_C(8) / receive_delta_ms;
    estimate_kbps_ = std::max(
        estimate_kbps_,
        Saturate(std::min(send_kbps, receive_kbps)));
}

void ProbeDeliveryEstimator::Finish() {
    active_ = false;
    awaiting_first_probe_ = false;
    receiver_baseline_ = Point{};
    if (!has_last_probe_packet_) {
        rate_guard_active_ = false;
    }
}

uint32_t ProbeDeliveryEstimator::estimate_kbps() const {
    return estimate_kbps_;
}

bool ProbeDeliveryEstimator::active() const {
    return active_;
}

bool ProbeDeliveryEstimator::rate_events_guarded() const {
    return rate_guard_active_;
}

}  // namespace gcc
