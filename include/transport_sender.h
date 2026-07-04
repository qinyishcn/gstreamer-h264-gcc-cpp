#pragma once

#include "rtp_packet.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gcc {

struct PacerEnqueue {
    uint32_t packet_id;
    size_t size;
};

struct SentPacket {
    uint16_t transport_sequence;
    size_t size;
    uint64_t send_time_us;
    bool probe;
    uint64_t cumulative_sent_bytes = 0U;
    uint64_t generation = 0U;
    uint32_t history_ordinal = 0U;
    bool probe_cluster = false;
};
using SentPacketInfo = SentPacket;

struct SentHistoryWatermark {
    bool valid = false;
    uint64_t oldest_generation = 0U;
    uint64_t oldest_cumulative_sent_bytes = 0U;
    uint32_t oldest_history_ordinal = 0U;
    uint64_t newest_generation = 0U;
    uint64_t cumulative_sent_bytes = 0U;
    uint32_t newest_history_ordinal = 0U;
};

class TransportSender {
public:
    using SendFunction = std::function<bool(const uint8_t*, size_t)>;
    using ClockFunction = std::function<uint64_t()>;
    using PacerAddFunction = std::function<bool(uint32_t, size_t)>;
    using SentFunction = std::function<void(const SentPacket&)>;

    TransportSender(
        SendFunction send, ClockFunction clock, size_t max_ingress_bytes);
    // Initial packet ID overload supports deterministic state restoration and
    // wrap-boundary tests; normal senders should use the three-argument form.
    TransportSender(
        SendFunction send, ClockFunction clock, size_t max_ingress_bytes,
        uint32_t initial_packet_id);
    TransportSender(
        SendFunction send, ClockFunction clock, size_t max_ingress_bytes,
        uint32_t initial_packet_id, uint64_t initial_generation,
        uint64_t initial_cumulative_sent_bytes,
        uint32_t initial_history_ordinal);
    ~TransportSender();

    TransportSender(const TransportSender&) = delete;
    TransportSender& operator=(const TransportSender&) = delete;
    TransportSender(TransportSender&&) = delete;
    TransportSender& operator=(TransportSender&&) = delete;

    // EnqueueMedia and EnqueueProbe are the producer-thread API.
    bool EnqueueMedia(std::vector<uint8_t> packet);
    bool EnqueueProbe(std::vector<uint8_t> packet);
    bool EnqueueProbe(std::vector<uint8_t> packet, uint64_t probe_epoch);
    uint64_t BeginProbeEpoch();
    size_t CancelProbeEpoch(uint64_t probe_epoch);

    // All methods below except the atomic/mutex-backed metric getters are
    // control-thread-only. Pacer callbacks must return on the calling thread.
    // Packet IDs advance modulo uint32_t and identify pacer entries only.
    // Successful UDP sends receive an independent contiguous RTP sequence.
    // Every pacer-accepted ID must eventually be passed to SendPaced (or be
    // cancelled by CancelProbeEpoch). TransportSender does not independently
    // age-evict media entries because doing so would desynchronize its packet
    // map from the razor pacer queue.
    size_t DrainIngress(const PacerAddFunction& add);
    bool SendPaced(uint32_t packet_id, SentPacket& out);
    // Razor may request an aged queue in a burst. SchedulePaced records those
    // requests, while FlushPaced applies the application's strict aggregate
    // wire-rate ceiling. One packet may be sent immediately after an idle
    // period; subsequent packets are serialized at the configured rate.
    void SetPacingRate(uint32_t bitrate_kbps);
    bool SchedulePaced(uint32_t packet_id);
    size_t FlushPaced(const SentFunction& on_sent);
    uint32_t PacingQueueDelayMs();
    bool LookupSentPacket(uint16_t sequence, SentPacketInfo& out) const;
    bool GetSentHistoryWatermark(SentHistoryWatermark& out) const;
    bool LookupSendTime(uint16_t sequence, uint64_t& out_us) const;
    bool latest_media_header(RtpHeader& out) const;
    size_t ingress_bytes() const;
    size_t pending_packets() const;
    uint64_t dropped_packets() const;
    uint64_t sent_packets() const;
    size_t queued_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns an empty function when host, port, or socket setup is invalid.
TransportSender::SendFunction CreateUdpSendFunction(
    const std::string& host, uint16_t port);

}  // namespace gcc
