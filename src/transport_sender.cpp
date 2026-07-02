#include "transport_sender.h"

#include <atomic>
#include <climits>
#include <deque>
#include <mutex>
#include <unordered_map>
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
        RtpHeader header;
    };

    struct HistoryEntry {
        uint16_t sequence;
        uint64_t send_time_us;
        uint64_t generation;
    };

    struct SendTimeRecord {
        uint64_t send_time_us;
        uint64_t generation;
    };

    Impl(SendFunction send_function, ClockFunction clock_function,
         size_t byte_limit, uint32_t initial_packet_id)
        : send(std::move(send_function)),
          clock(std::move(clock_function)),
          max_ingress_bytes(byte_limit),
          next_packet_id(initial_packet_id) {
        send_times.reserve(kSendHistoryLimit);
    }

    bool Enqueue(std::vector<uint8_t> packet, bool probe) {
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
        ingress.push_back(Packet{std::move(packet), probe, header});
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
    uint64_t next_send_generation = 0;
    std::unordered_map<uint32_t, Packet> paced;
    bool has_latest_media_header = false;
    RtpHeader latest_media;
    std::deque<HistoryEntry> history;
    std::unordered_map<uint16_t, SendTimeRecord> send_times;
};

TransportSender::TransportSender(
    SendFunction send, ClockFunction clock, size_t max_ingress_bytes)
    : TransportSender(
          std::move(send), std::move(clock), max_ingress_bytes, 0) {}

TransportSender::TransportSender(
    SendFunction send, ClockFunction clock, size_t max_ingress_bytes,
    uint32_t initial_packet_id)
    : impl_(new Impl(
          std::move(send), std::move(clock), max_ingress_bytes,
          initial_packet_id)) {}

TransportSender::~TransportSender() = default;

bool TransportSender::EnqueueMedia(std::vector<uint8_t> packet) {
    return impl_->Enqueue(std::move(packet), false);
}

bool TransportSender::EnqueueProbe(std::vector<uint8_t> packet) {
    return impl_->Enqueue(std::move(packet), true);
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
        if (!RewriteRtpSequence(
                packet.bytes, static_cast<uint16_t>(packet_id))) {
            impl_->queued_byte_count.fetch_sub(packet.bytes.size());
            ++impl_->dropped;
            continue;
        }
        if (!packet.probe) {
            impl_->latest_media = packet.header;
            impl_->latest_media.sequence =
                static_cast<uint16_t>(packet_id);
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

    const uint16_t sequence = static_cast<uint16_t>(packet_id);
    const uint64_t generation = impl_->next_send_generation++;
    impl_->history.push_back({sequence, send_time_us, generation});
    impl_->send_times[sequence] = {send_time_us, generation};
    if (impl_->history.size() > kSendHistoryLimit) {
        const Impl::HistoryEntry oldest = impl_->history.front();
        impl_->history.pop_front();
        const auto current = impl_->send_times.find(oldest.sequence);
        if (current != impl_->send_times.end() &&
            current->second.generation == oldest.generation) {
            impl_->send_times.erase(current);
        }
    }

    out = SentPacket{
        sequence, packet.bytes.size(), send_time_us, packet.probe};
    ++impl_->sent_count;
    return true;
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
