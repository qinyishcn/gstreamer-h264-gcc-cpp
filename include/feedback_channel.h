#pragma once
/**
 * Feedback Channel for GCC Video Transmission
 *
 * UDP-based feedback channel carrying razor library feedback data.
 * Protocol v4: portable, explicitly encoded header plus razor payload.
 */

#include "feedback_packet.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <string>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define CLOSE_SOCKET closesocket
  using FeedbackSocket = SOCKET;
  static constexpr FeedbackSocket kInvalidFeedbackSocket = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #define CLOSE_SOCKET close
  using FeedbackSocket = int;
  static constexpr FeedbackSocket kInvalidFeedbackSocket = -1;
#endif

namespace gcc {

inline void ReportFeedbackSocketError(const char* operation) {
#ifdef _WIN32
    std::fprintf(stderr, "FeedbackChannel: %s failed (%d)\n",
                 operation, WSAGetLastError());
#else
    std::perror(operation);
#endif
}

inline uint64_t get_timestamp_us() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

// ── Send Time Tracker ──────────────────────────────────────────────
class SendTimeTracker {
public:
    void record(uint16_t seq, uint64_t wall_clock_us) {
        std::lock_guard<std::mutex> lock(mu_);
        send_times_[seq] = wall_clock_us;
        if (send_times_.size() > 2000) {
            auto it = send_times_.begin();
            send_times_.erase(it);
        }
    }
    bool lookup(uint16_t seq, uint64_t& wall_clock_us) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = send_times_.find(seq);
        if (it != send_times_.end()) {
            wall_clock_us = it->second;
            return true;
        }
        return false;
    }
private:
    std::mutex mu_;
    std::unordered_map<uint16_t, uint64_t> send_times_;
};

// ── Loss Tracker (RTP sequence-based) ──────────────────────────────
class LossTracker {
public:
    void on_packet(uint16_t seq) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) {
            first_ext_seq_ = seq;
            highest_ext_seq_ = seq;
            seen_.fill(std::numeric_limits<uint64_t>::max());
            seen_[seq % kSeenWindow] = seq;
            initialized_ = true;
            return;
        }

        uint64_t extended = (highest_ext_seq_ & ~UINT64_C(0xffff)) | seq;
        if (extended < highest_ext_seq_ &&
            highest_ext_seq_ - extended > 0x8000) {
            extended += 0x10000;
        } else if (extended > highest_ext_seq_ &&
                   extended - highest_ext_seq_ > 0x8000) {
            if (extended < 0x10000) return;
            extended -= 0x10000;
        }
        if (extended < first_ext_seq_) return;

        const uint64_t ext_seq = extended;
        if (ext_seq > highest_ext_seq_) highest_ext_seq_ = ext_seq;
        if (highest_ext_seq_ - ext_seq >= kSeenWindow) return;

        uint64_t& seen_tag = seen_[ext_seq % kSeenWindow];
        if (seen_tag == ext_seq) return;
        seen_tag = ext_seq;
    }

    // Returns the clamped loss fraction mapped to [0, 255].
    uint8_t get_loss_q8() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_) return 0;
        constexpr uint64_t kLossWindow = 256U;
        const uint64_t window_first =
            highest_ext_seq_ - first_ext_seq_ + 1U > kLossWindow
                ? highest_ext_seq_ - (kLossWindow - 1U)
                : first_ext_seq_;
        const uint64_t expected =
            highest_ext_seq_ - window_first + 1U;
        uint64_t received = 0U;
        for (uint64_t sequence = window_first;
             sequence <= highest_ext_seq_; ++sequence) {
            if (seen_[sequence % kSeenWindow] == sequence) {
                ++received;
            }
        }
        double loss = 1.0 -
            static_cast<double>(received) / static_cast<double>(expected);
        if (loss < 0) loss = 0;
        if (loss > 1) loss = 1;
        return static_cast<uint8_t>(loss * 255.0 + 0.5);
    }

    // Temporary source-compatibility alias; remove with legacy callers.
    uint8_t get_loss_pct_x100() {
        return get_loss_q8();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mu_);
        initialized_ = false;
    }

private:
    static constexpr size_t kSeenWindow = 4096;
    std::mutex mu_;
    bool initialized_ = false;
    uint64_t first_ext_seq_ = 0;
    uint64_t highest_ext_seq_ = 0;
    std::array<uint64_t, kSeenWindow> seen_{};
};

// ── FeedbackChannel ────────────────────────────────────────────────
class FeedbackChannel {
public:
    FeedbackChannel(const char* addr, uint16_t port, bool is_sender)
        : port_(port), bound_port_(0), is_sender_(is_sender),
          sock_(kInvalidFeedbackSocket), running_(false) {
        if (addr) target_addr_ = addr;
    }

    ~FeedbackChannel() { stop(); }

    FeedbackChannel(const FeedbackChannel&) = delete;
    FeedbackChannel& operator=(const FeedbackChannel&) = delete;

    bool start() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        winsock_started_ = true;
#endif
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == kInvalidFeedbackSocket) {
            ReportFeedbackSocketError("socket");
            return false;
        }

        int opt = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        if (is_sender_) {
            struct sockaddr_in bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(port_);
            bind_addr.sin_addr.s_addr = inet_addr(target_addr_.c_str());
            if (bind(sock_, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
                ReportFeedbackSocketError("bind");
                CLOSE_SOCKET(sock_);
                sock_ = kInvalidFeedbackSocket;
                return false;
            }
            struct sockaddr_in actual_addr;
            memset(&actual_addr, 0, sizeof(actual_addr));
            socklen_t actual_addr_len = sizeof(actual_addr);
            if (getsockname(sock_,
                            reinterpret_cast<struct sockaddr*>(&actual_addr),
                            &actual_addr_len) < 0) {
                ReportFeedbackSocketError("getsockname");
                CLOSE_SOCKET(sock_);
                sock_ = kInvalidFeedbackSocket;
                return false;
            }
            bound_port_.store(ntohs(actual_addr.sin_port));
            running_.store(true);
            recv_thread_ = std::thread(&FeedbackChannel::recv_loop, this);
            printf("  Feedback: listening on %s:%d\n",
                   target_addr_.c_str(), bound_port_.load());
        } else {
            printf("  Feedback: sending to %s:%d\n", target_addr_.c_str(), port_);
        }
        return true;
    }

    void stop() {
        running_.store(false);
        if (recv_thread_.joinable()) recv_thread_.join();
        {
            std::lock_guard<std::mutex> socket_lock(socket_mutex_);
            if (sock_ != kInvalidFeedbackSocket) {
                CLOSE_SOCKET(sock_);
                sock_ = kInvalidFeedbackSocket;
            }
        }
#ifdef _WIN32
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
#endif
    }

    // ── Receiver side: send protocol-v4 feedback ──
    bool send_feedback(const FeedbackPacket& packet) {
        const std::vector<uint8_t> encoded = EncodeFeedbackPacket(packet);
        if (encoded.empty()) return false;

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port_);
        dest.sin_addr.s_addr = inet_addr(target_addr_.c_str());
        std::lock_guard<std::mutex> socket_lock(socket_mutex_);
        if (sock_ == kInvalidFeedbackSocket) return false;
#ifdef _WIN32
        const int sent = sendto(
            sock_, reinterpret_cast<const char*>(encoded.data()),
            static_cast<int>(encoded.size()), 0,
            reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
#else
        const ssize_t sent = sendto(
            sock_, encoded.data(), encoded.size(), 0,
            reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
#endif
        return sent >= 0 && static_cast<size_t>(sent) == encoded.size();
    }

    // Temporary source-compatibility overload; remove in Tasks 8/9.
    void send_feedback(uint16_t seq, uint8_t loss_pct,
                       uint32_t rx_ts_ms, uint32_t fb_ts_ms,
                       const uint8_t* razor_data, int razor_size) {
        if (razor_size < 0 ||
            static_cast<size_t>(razor_size) > kMaxFeedbackPayload ||
            (razor_size > 0 && razor_data == nullptr)) {
            return;
        }
        FeedbackPacket packet;
        packet.sequence = seq;
        packet.loss_q8 = loss_pct;
        packet.receive_time_ms = rx_ts_ms;
        packet.feedback_time_ms = fb_ts_ms;
        if (razor_size > 0) {
            packet.razor_payload.assign(razor_data, razor_data + razor_size);
        }
        send_feedback(packet);
    }

    // ── Sender side: poll for feedback ──
    struct ReceivedFeedback {
        uint16_t seq;
        uint8_t  loss_pct;
        uint32_t rx_ts_ms;      // when receiver got the packet
        uint32_t fb_ts_ms;      // when receiver sent feedback
        uint8_t  razor_data[kMaxFeedbackPayload];
        int      razor_size;
    };

    bool poll(FeedbackPacket& out) {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        if (rx_queue_.empty()) return false;
        out = std::move(rx_queue_.front());
        rx_queue_.pop_front();
        return true;
    }

    // Temporary source-compatibility adapter; remove in Tasks 8/9.
    bool poll(ReceivedFeedback& out) {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        if (rx_queue_.empty()) return false;
        FeedbackPacket packet = std::move(rx_queue_.back());
        rx_queue_.clear();
        out.seq = packet.sequence;
        out.loss_pct = packet.loss_q8;
        out.rx_ts_ms = packet.receive_time_ms;
        out.fb_ts_ms = packet.feedback_time_ms;
        out.razor_size = static_cast<int>(packet.razor_payload.size());
        if (!packet.razor_payload.empty()) {
            memcpy(out.razor_data, packet.razor_payload.data(),
                   packet.razor_payload.size());
        }
        return true;
    }

    bool is_running() const { return running_.load(); }
    uint16_t bound_port() const { return bound_port_.load(); }
    size_t queued_feedback_count() {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        return rx_queue_.size();
    }
    uint64_t dropped_feedback_count() const {
        return dropped_feedback_.load();
    }

private:
    void recv_loop() {
        uint8_t buf[kMaxFeedbackPacketSize + 64];
        while (running_.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock_, &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;  // 200ms timeout

#ifdef _WIN32
            int sel = select(0, &fds, nullptr, nullptr, &tv);
#else
            int sel = select(sock_ + 1, &fds, nullptr, nullptr, &tv);
#endif
            if (sel <= 0) continue;

            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            const auto n = recvfrom(
                sock_, reinterpret_cast<char*>(buf),
#ifdef _WIN32
                static_cast<int>(sizeof(buf)),
#else
                sizeof(buf),
#endif
                0, (struct sockaddr*)&from, &from_len);
            if (n <= 0) continue;

            FeedbackPacket packet;
            if (!DecodeFeedbackPacket(
                    buf, static_cast<size_t>(n), packet)) {
                continue;
            }
            std::lock_guard<std::mutex> lock(rx_mutex_);
            if (rx_queue_.size() == kMaxQueuedFeedback) {
                rx_queue_.pop_front();
                dropped_feedback_.fetch_add(1);
            }
            rx_queue_.push_back(std::move(packet));
        }
    }

    uint16_t     port_;
    std::atomic<uint16_t> bound_port_;
    bool         is_sender_;
    std::string  target_addr_;
    FeedbackSocket sock_;
    std::mutex socket_mutex_;
    std::atomic<bool> running_;
    std::thread       recv_thread_;
#ifdef _WIN32
    bool winsock_started_ = false;
#endif

    // Sender-side received data
    std::mutex        rx_mutex_;
    static constexpr size_t kMaxQueuedFeedback = 64;
    std::deque<FeedbackPacket> rx_queue_;
    std::atomic<uint64_t> dropped_feedback_{0};
};

} // namespace gcc
