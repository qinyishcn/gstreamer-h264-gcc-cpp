#include "transport_sender.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <deque>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace gcc {
namespace {

constexpr size_t kSendHistoryLimit = 2000;

struct UdpSocketState {
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
    bool winsock_started = false;

    ~UdpSocketState() {
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
        }
        if (winsock_started) {
            WSACleanup();
        }
    }
#else
    int socket = -1;

    ~UdpSocketState() {
        if (socket >= 0) {
            close(socket);
        }
    }
#endif
    sockaddr_in destination{};
};

}  // namespace

struct TransportSender::Impl {
    struct Packet {
        std::vector<uint8_t> bytes;
        bool probe = false;
        uint64_t probe_epoch = 0U;
        RtpHeader header;
    };

    struct HistoryEntry {
        uint16_t sequence;
        uint64_t generation;
        uint64_t cumulative_sent_bytes;
        uint32_t history_ordinal;
    };

    struct ScheduledPacket {
        uint32_t packet_id;
        uint64_t scheduled_time_us;
    };

    Impl(SendFunction send_function, ClockFunction clock_function,
         size_t byte_limit, uint32_t initial_packet_id,
         uint64_t initial_generation,
         uint64_t initial_cumulative_sent_bytes,
         uint32_t initial_history_ordinal)
        : send(std::move(send_function)),
          clock(std::move(clock_function)),
          max_ingress_bytes(byte_limit),
          next_packet_id(initial_packet_id),
          next_transport_sequence(
              static_cast<uint16_t>(initial_packet_id)),
          next_send_generation(initial_generation),
          cumulative_sent_bytes(initial_cumulative_sent_bytes),
          next_history_ordinal(initial_history_ordinal) {
        send_times.reserve(kSendHistoryLimit);
    }

    bool Enqueue(std::vector<uint8_t> packet, bool probe,
                 uint64_t probe_epoch = 0U) {
        RtpHeader header;
        if ((probe &&
             !IsProbeRtpPacket(packet.data(), packet.size())) ||
            !ParseRtpHeader(packet.data(), packet.size(), header)) {
            ++dropped;
            return false;
        }

        std::lock_guard<std::mutex> lock(ingress_mutex);
        if (packet.size() > max_ingress_bytes - ingress_byte_count) {
            ++dropped;
            return false;
        }
        const size_t packet_size = packet.size();
        ingress.push_back(
            Packet{std::move(packet), probe, probe_epoch, header});
        ingress_byte_count += packet_size;
        queued_byte_count.fetch_add(packet_size);
        return true;
    }

    SendFunction send;
    ClockFunction clock;
    const size_t max_ingress_bytes;

    mutable std::mutex ingress_mutex;
    std::deque<Packet> ingress;
    size_t ingress_byte_count = 0;
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> sent_count{0};
    std::atomic<size_t> queued_byte_count{0};

    uint32_t next_packet_id;
    uint16_t next_transport_sequence;
    uint64_t next_send_generation = 0;
    uint64_t cumulative_sent_bytes = 0;
    uint32_t next_history_ordinal = 0;
    std::unordered_map<uint32_t, Packet> paced;
    std::deque<ScheduledPacket> scheduled_probe;
    std::deque<ScheduledPacket> scheduled_media;
    std::unordered_set<uint32_t> scheduled_ids;
    std::unordered_set<uint32_t> cancelled_ids;
    std::unordered_set<uint32_t> vendor_callback_pending;
    uint32_t pacing_rate_kbps = 0U;
    uint64_t next_send_time_us = 0U;
    uint64_t next_probe_epoch = 0U;
    uint64_t active_probe_epoch = 0U;
    bool has_latest_media_header = false;
    RtpHeader latest_media;
    std::deque<HistoryEntry> history;
    std::unordered_map<uint16_t, SentPacketInfo> send_times;
};

TransportSender::TransportSender(
    SendFunction send, ClockFunction clock, size_t max_ingress_bytes)
    : TransportSender(
          std::move(send), std::move(clock), max_ingress_bytes, 0,
          0U, 0U, 0U) {}

TransportSender::TransportSender(
    SendFunction send, ClockFunction clock, size_t max_ingress_bytes,
    uint32_t initial_packet_id)
    : impl_(new Impl(
          std::move(send), std::move(clock), max_ingress_bytes,
          initial_packet_id, 0U, 0U, 0U)) {}

TransportSender::TransportSender(
    SendFunction send, ClockFunction clock, size_t max_ingress_bytes,
    uint32_t initial_packet_id, uint64_t initial_generation,
    uint64_t initial_cumulative_sent_bytes,
    uint32_t initial_history_ordinal)
    : impl_(new Impl(
          std::move(send), std::move(clock), max_ingress_bytes,
          initial_packet_id, initial_generation,
          initial_cumulative_sent_bytes, initial_history_ordinal)) {}

TransportSender::~TransportSender() = default;

bool TransportSender::EnqueueMedia(std::vector<uint8_t> packet) {
    return impl_->Enqueue(std::move(packet), false);
}

bool TransportSender::EnqueueProbe(std::vector<uint8_t> packet) {
    return impl_->Enqueue(std::move(packet), true);
}

bool TransportSender::EnqueueProbe(
    std::vector<uint8_t> packet, uint64_t probe_epoch) {
    if (probe_epoch == 0U) {
        return false;
    }
    return impl_->Enqueue(std::move(packet), true, probe_epoch);
}

uint64_t TransportSender::BeginProbeEpoch() {
    if (impl_->next_probe_epoch <
        std::numeric_limits<uint64_t>::max()) {
        ++impl_->next_probe_epoch;
    }
    impl_->active_probe_epoch = impl_->next_probe_epoch;
    return impl_->active_probe_epoch;
}

size_t TransportSender::CancelProbeEpoch(uint64_t probe_epoch) {
    if (probe_epoch == 0U) {
        return 0U;
    }
    if (impl_->active_probe_epoch == probe_epoch) {
        impl_->active_probe_epoch = 0U;
    }
    size_t cancelled = 0U;
    {
        std::lock_guard<std::mutex> lock(impl_->ingress_mutex);
        auto current = impl_->ingress.begin();
        while (current != impl_->ingress.end()) {
            if (current->probe && current->probe_epoch == probe_epoch) {
                impl_->ingress_byte_count -= current->bytes.size();
                impl_->queued_byte_count.fetch_sub(current->bytes.size());
                current = impl_->ingress.erase(current);
                ++cancelled;
            } else {
                ++current;
            }
        }
    }
    for (auto current = impl_->paced.begin();
         current != impl_->paced.end();) {
        if (current->second.probe &&
            current->second.probe_epoch == probe_epoch) {
            impl_->queued_byte_count.fetch_sub(
                current->second.bytes.size());
            impl_->cancelled_ids.insert(current->first);
            current = impl_->paced.erase(current);
            ++cancelled;
        } else {
            ++current;
        }
    }
    return cancelled;
}

size_t TransportSender::DrainIngress(const PacerAddFunction& add) {
    std::deque<Impl::Packet> drained;
    {
        std::lock_guard<std::mutex> lock(impl_->ingress_mutex);
        drained.swap(impl_->ingress);
        impl_->ingress_byte_count = 0;
    }

    size_t accepted = 0;
    for (Impl::Packet& packet : drained) {
        const uint32_t packet_id = impl_->next_packet_id++;
        if (!packet.probe) {
            impl_->latest_media = packet.header;
            impl_->has_latest_media_header = true;
        }

        const size_t packet_size = packet.bytes.size();
        if (impl_->paced.find(packet_id) != impl_->paced.end()) {
            impl_->queued_byte_count.fetch_sub(packet_size);
            ++impl_->dropped;
            continue;
        }
        const auto inserted =
            impl_->paced.emplace(packet_id, std::move(packet));
        if (!inserted.second) {
            impl_->queued_byte_count.fetch_sub(packet_size);
            ++impl_->dropped;
            continue;
        }
        if (inserted.first->second.probe) {
            // Probe padding must not sit behind an aged vendor queue. The
            // strict C++ egress remains the aggregate wire-rate authority;
            // razor still receives the packet for its send history.
            SchedulePaced(packet_id);
            impl_->vendor_callback_pending.insert(packet_id);
        }

        bool pacer_accepted = false;
        try {
            pacer_accepted = static_cast<bool>(add) &&
                             add(packet_id, packet_size);
        } catch (...) {
            pacer_accepted = false;
        }
        if (impl_->paced.find(packet_id) == impl_->paced.end()) {
            // A synchronous pacer callback consumed the packet. It was
            // accepted regardless of the callback's later return/exception.
            ++accepted;
            continue;
        }
        if (!pacer_accepted) {
            impl_->vendor_callback_pending.erase(packet_id);
            impl_->paced.erase(packet_id);
            impl_->queued_byte_count.fetch_sub(packet_size);
            ++impl_->dropped;
            continue;
        }
        ++accepted;
    }
    return accepted;
}

bool TransportSender::SendPaced(uint32_t packet_id, SentPacket& out) {
    const auto found = impl_->paced.find(packet_id);
    if (found == impl_->paced.end()) {
        return false;
    }

    Impl::Packet packet = std::move(found->second);
    impl_->paced.erase(found);
    impl_->queued_byte_count.fetch_sub(packet.bytes.size());

    const uint16_t sequence = impl_->next_transport_sequence;
    if (!RewriteRtpSequence(packet.bytes, sequence)) {
        ++impl_->dropped;
        return false;
    }

    uint64_t send_time_us = 0;
    try {
        if (!impl_->clock) {
            ++impl_->dropped;
            return false;
        }
        send_time_us = impl_->clock();
    } catch (...) {
        ++impl_->dropped;
        return false;
    }

    bool sent = false;
    try {
        sent = static_cast<bool>(impl_->send) &&
               impl_->send(packet.bytes.data(), packet.bytes.size());
    } catch (...) {
        sent = false;
    }
    if (!sent) {
        ++impl_->dropped;
        return false;
    }

    ++impl_->next_transport_sequence;
    const uint64_t generation = impl_->next_send_generation;
    if (impl_->next_send_generation <
        std::numeric_limits<uint64_t>::max()) {
        ++impl_->next_send_generation;
    }
    const uint32_t history_ordinal = impl_->next_history_ordinal;
    ++impl_->next_history_ordinal;
    const uint64_t room =
        std::numeric_limits<uint64_t>::max() -
        impl_->cumulative_sent_bytes;
    impl_->cumulative_sent_bytes +=
        std::min<uint64_t>(packet.bytes.size(), room);
    const SentPacketInfo info{
        sequence, packet.bytes.size(), send_time_us, packet.probe,
        impl_->cumulative_sent_bytes, generation, history_ordinal,
        impl_->active_probe_epoch != 0U};
    impl_->history.push_back(
        {sequence, generation, impl_->cumulative_sent_bytes,
         history_ordinal});
    impl_->send_times[sequence] = info;
    if (impl_->history.size() > kSendHistoryLimit) {
        const Impl::HistoryEntry oldest = impl_->history.front();
        impl_->history.pop_front();
        const auto current = impl_->send_times.find(oldest.sequence);
        if (current != impl_->send_times.end() &&
            current->second.generation == oldest.generation) {
            impl_->send_times.erase(current);
        }
    }

    out = info;
    ++impl_->sent_count;
    return true;
}

void TransportSender::SetPacingRate(uint32_t bitrate_kbps) {
    impl_->pacing_rate_kbps = bitrate_kbps;
}

bool TransportSender::SchedulePaced(uint32_t packet_id) {
    const auto cancelled = impl_->cancelled_ids.find(packet_id);
    if (cancelled != impl_->cancelled_ids.end()) {
        impl_->cancelled_ids.erase(cancelled);
        impl_->vendor_callback_pending.erase(packet_id);
        return true;
    }
    const auto vendor_pending =
        impl_->vendor_callback_pending.find(packet_id);
    if (vendor_pending != impl_->vendor_callback_pending.end()) {
        impl_->vendor_callback_pending.erase(vendor_pending);
        if (impl_->paced.find(packet_id) == impl_->paced.end() ||
            impl_->scheduled_ids.find(packet_id) !=
                impl_->scheduled_ids.end()) {
            return true;
        }
    }
    const auto found = impl_->paced.find(packet_id);
    if (found == impl_->paced.end()) {
        return false;
    }
    if (impl_->scheduled_ids.find(packet_id) !=
        impl_->scheduled_ids.end()) {
        return true;
    }
    uint64_t now_us = 0U;
    try {
        if (!impl_->clock) {
            return false;
        }
        now_us = impl_->clock();
    } catch (...) {
        return false;
    }
    if (found->second.probe &&
        impl_->active_probe_epoch != 0U) {
        impl_->scheduled_probe.push_back({packet_id, now_us});
    } else {
        impl_->scheduled_media.push_back({packet_id, now_us});
    }
    impl_->scheduled_ids.insert(packet_id);
    return true;
}

size_t TransportSender::FlushPaced(const SentFunction& on_sent) {
    if (impl_->pacing_rate_kbps == 0U || !impl_->clock) {
        return 0U;
    }
    uint64_t now_us = 0U;
    try {
        now_us = impl_->clock();
    } catch (...) {
        return 0U;
    }

    size_t sent_count = 0U;
    while (!impl_->scheduled_probe.empty() ||
           !impl_->scheduled_media.empty()) {
        // Media sent during an active epoch is itself probe-cluster traffic.
        // Keep ready media ahead of synthetic padding; padding only fills
        // aggregate probe-rate deficits when no media packet is ready.
        std::deque<Impl::ScheduledPacket>* queue =
            !impl_->scheduled_media.empty()
                ? &impl_->scheduled_media
                : &impl_->scheduled_probe;
        const uint32_t packet_id =
            queue->front().packet_id;
        const auto found = impl_->paced.find(packet_id);
        if (found == impl_->paced.end()) {
            queue->pop_front();
            impl_->scheduled_ids.erase(packet_id);
            continue;
        }
        if (impl_->next_send_time_us != 0U &&
            now_us < impl_->next_send_time_us) {
            break;
        }

        queue->pop_front();
        impl_->scheduled_ids.erase(packet_id);
        SentPacket sent;
        if (!SendPaced(packet_id, sent)) {
            continue;
        }
        ++sent_count;
        if (on_sent) {
            try {
                on_sent(sent);
            } catch (...) {
                // Sending is already complete; callbacks cannot roll it back.
            }
        }

        constexpr uint64_t kMicrosPerByteAtOneKbps = 8000U;
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        const uint64_t numerator =
            sent.size > maximum / kMicrosPerByteAtOneKbps
                ? maximum
                : static_cast<uint64_t>(sent.size) *
                      kMicrosPerByteAtOneKbps;
        const uint64_t interval_us =
            numerator / impl_->pacing_rate_kbps +
            (numerator % impl_->pacing_rate_kbps != 0U ? 1U : 0U);
        const uint64_t base =
            impl_->next_send_time_us == 0U ||
                    sent.send_time_us > impl_->next_send_time_us
                ? sent.send_time_us
                : impl_->next_send_time_us;
        impl_->next_send_time_us =
            interval_us > maximum - base ? maximum : base + interval_us;
    }
    return sent_count;
}

uint32_t TransportSender::PacingQueueDelayMs() {
    if (!impl_->clock) {
        return 0U;
    }
    uint64_t now_us = 0U;
    try {
        now_us = impl_->clock();
    } catch (...) {
        return 0U;
    }
    const auto delay_for =
        [&](const std::deque<Impl::ScheduledPacket>& queue) {
            for (const Impl::ScheduledPacket& scheduled : queue) {
                if (impl_->paced.find(scheduled.packet_id) ==
                    impl_->paced.end()) {
                    continue;
                }
                if (now_us <= scheduled.scheduled_time_us) {
                    return uint32_t{0};
                }
                const uint64_t delay_ms =
                    (now_us - scheduled.scheduled_time_us) / 1000U;
                return delay_ms >
                               std::numeric_limits<uint32_t>::max()
                           ? std::numeric_limits<uint32_t>::max()
                           : static_cast<uint32_t>(delay_ms);
            }
            return uint32_t{0};
        };
    const uint32_t probe_delay = delay_for(impl_->scheduled_probe);
    const uint32_t media_delay = delay_for(impl_->scheduled_media);
    return std::max(probe_delay, media_delay);
}

bool TransportSender::LookupSendTime(
    uint16_t sequence, uint64_t& out_us) const {
    const auto found = impl_->send_times.find(sequence);
    if (found == impl_->send_times.end()) {
        return false;
    }
    out_us = found->second.send_time_us;
    return true;
}

bool TransportSender::LookupSentPacket(
    uint16_t sequence, SentPacketInfo& out) const {
    const auto found = impl_->send_times.find(sequence);
    if (found == impl_->send_times.end()) {
        return false;
    }
    out = found->second;
    return true;
}

bool TransportSender::GetSentHistoryWatermark(
    SentHistoryWatermark& out) const {
    out = SentHistoryWatermark{};
    if (impl_->history.empty()) {
        return false;
    }
    out.valid = true;
    out.oldest_generation = impl_->history.front().generation;
    out.oldest_cumulative_sent_bytes =
        impl_->history.front().cumulative_sent_bytes;
    out.oldest_history_ordinal =
        impl_->history.front().history_ordinal;
    out.newest_generation = impl_->history.back().generation;
    out.cumulative_sent_bytes = impl_->cumulative_sent_bytes;
    out.newest_history_ordinal =
        impl_->history.back().history_ordinal;
    return true;
}

bool TransportSender::latest_media_header(RtpHeader& out) const {
    if (!impl_->has_latest_media_header) {
        return false;
    }
    out = impl_->latest_media;
    return true;
}

size_t TransportSender::ingress_bytes() const {
    std::lock_guard<std::mutex> lock(impl_->ingress_mutex);
    return impl_->ingress_byte_count;
}

size_t TransportSender::pending_packets() const {
    return impl_->paced.size();
}

uint64_t TransportSender::dropped_packets() const {
    return impl_->dropped.load();
}

uint64_t TransportSender::sent_packets() const {
    return impl_->sent_count.load();
}

size_t TransportSender::queued_bytes() const {
    return impl_->queued_byte_count.load();
}

TransportSender::SendFunction CreateUdpSendFunction(
    const std::string& host, uint16_t port) {
    if (host.empty() || port == 0) {
        return {};
    }

    const std::shared_ptr<UdpSocketState> state =
        std::make_shared<UdpSocketState>();
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return {};
    }
    state->winsock_started = true;
    state->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (state->socket == INVALID_SOCKET) {
        return {};
    }
#else
    state->socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->socket < 0) {
        return {};
    }
#endif

    state->destination.sin_family = AF_INET;
    state->destination.sin_port = htons(port);
    if (inet_pton(
            AF_INET, host.c_str(),
            &state->destination.sin_addr) != 1) {
        return {};
    }

    return [state](const uint8_t* data, size_t size) {
        if (data == nullptr || size == 0) {
            return false;
        }
#ifdef _WIN32
        if (size > static_cast<size_t>(INT_MAX)) {
            return false;
        }
        const int sent = sendto(
            state->socket, reinterpret_cast<const char*>(data),
            static_cast<int>(size), 0,
            reinterpret_cast<const sockaddr*>(&state->destination),
            sizeof(state->destination));
        return sent >= 0 && static_cast<size_t>(sent) == size;
#else
        const ssize_t sent = sendto(
            state->socket, data, size, 0,
            reinterpret_cast<const sockaddr*>(&state->destination),
            sizeof(state->destination));
        return sent >= 0 && static_cast<size_t>(sent) == size;
#endif
    };
}

}  // namespace gcc
