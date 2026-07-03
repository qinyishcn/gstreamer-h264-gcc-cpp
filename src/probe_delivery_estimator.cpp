#include "probe_delivery_estimator.h"

#include <algorithm>
#include <limits>

namespace gcc {
namespace {

constexpr size_t kMaximumProbeRanges = 2000U;

uint32_t Saturate(uint64_t value) {
    return value > std::numeric_limits<uint32_t>::max()
               ? std::numeric_limits<uint32_t>::max()
               : static_cast<uint32_t>(value);
}

bool OrdinalAfter(uint32_t candidate, uint32_t reference) {
    const uint32_t distance = candidate - reference;
    return distance != 0U && distance < UINT32_C(0x80000000);
}

bool PositionAfter(
    uint64_t candidate_generation, uint64_t candidate_bytes,
    uint32_t candidate_ordinal, uint64_t reference_generation,
    uint64_t reference_bytes, uint32_t reference_ordinal) {
    if (candidate_generation != reference_generation) {
        return candidate_generation > reference_generation;
    }
    if (candidate_bytes != reference_bytes) {
        return candidate_bytes > reference_bytes;
    }
    return OrdinalAfter(candidate_ordinal, reference_ordinal);
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
        if (probe_ranges_.size() == kMaximumProbeRanges) {
            probe_ranges_.pop_front();
        }
        probe_ranges_.push_back(
            {packet.generation, packet.cumulative_sent_bytes,
             packet.history_ordinal, packet.generation,
             packet.cumulative_sent_bytes, packet.history_ordinal});
    }
    has_last_probe_packet_ = true;
    last_probe_cumulative_bytes_ = packet.cumulative_sent_bytes;
    last_probe_generation_ = packet.generation;
    last_probe_history_ordinal_ = packet.history_ordinal;
    ProbeRange& range = probe_ranges_.back();
    range.last_generation = packet.generation;
    range.last_cumulative_bytes = packet.cumulative_sent_bytes;
    range.last_history_ordinal = packet.history_ordinal;
}

bool ProbeDeliveryEstimator::BeginFeedback(
    const SentPacketInfo& packet) {
    if (active_) {
        return IsProbeGeneration(packet);
    }
    if (!has_last_probe_packet_) {
        rate_guard_active_ = false;
        return false;
    }
    if (rate_guard_active_) {
        if (PositionAfter(
                packet.generation, packet.cumulative_sent_bytes,
                packet.history_ordinal, last_probe_generation_,
                last_probe_cumulative_bytes_,
                last_probe_history_ordinal_)) {
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
    AdvanceSendProgress(watermark);
    return related;
}

void ProbeDeliveryEstimator::AdvanceSendProgress(
    const SentHistoryWatermark& watermark) {
    if (!watermark.valid) return;
    const size_t current_ranges = active_ && has_last_probe_packet_ ? 1U : 0U;
    while (probe_ranges_.size() > current_ranges) {
        const ProbeRange& range = probe_ranges_.front();
        if (!WatermarkPast(
                watermark, range.last_generation,
                range.last_cumulative_bytes,
                range.last_history_ordinal)) {
            break;
        }
        probe_ranges_.pop_front();
    }
    if (!active_ && rate_guard_active_ && has_last_probe_packet_ &&
        WatermarkPast(
            watermark, last_probe_generation_,
            last_probe_cumulative_bytes_,
            last_probe_history_ordinal_)) {
        rate_guard_active_ = false;
    }
}

bool ProbeDeliveryEstimator::WatermarkPast(
    const SentHistoryWatermark& watermark, uint64_t generation,
    uint64_t cumulative_bytes, uint32_t history_ordinal) const {
    return watermark.valid &&
           PositionAfter(
               watermark.oldest_generation,
               watermark.oldest_cumulative_sent_bytes,
               watermark.oldest_history_ordinal, generation,
               cumulative_bytes, history_ordinal);
}

bool ProbeDeliveryEstimator::IsProbeGeneration(
    const SentPacketInfo& packet) const {
    if (!packet.probe) return false;
    for (const ProbeRange& range : probe_ranges_) {
        const bool before_range = PositionAfter(
            range.first_generation, range.first_cumulative_bytes,
            range.first_history_ordinal, packet.generation,
            packet.cumulative_sent_bytes, packet.history_ordinal);
        const bool after_range = PositionAfter(
            packet.generation, packet.cumulative_sent_bytes,
            packet.history_ordinal, range.last_generation,
            range.last_cumulative_bytes, range.last_history_ordinal);
        if (!before_range && !after_range) {
            return true;
        }
    }
    return false;
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
