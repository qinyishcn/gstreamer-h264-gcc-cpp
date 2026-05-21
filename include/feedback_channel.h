#pragma once
/**
 * Feedback Channel for GCC Video Transmission
 *
 * UDP-based feedback channel carrying razor library feedback data.
 * Protocol v2: variable-length razor payload.
 *
 * FeedbackPacket layout (header 10 bytes + up to 256 bytes payload):
 *   magic(1) + version(1) + seq(2) + loss_pct_x100(1) + fb_size(2) + reserved(3) + fb_data(N)
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define CLOSE_SOCKET closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #define CLOSE_SOCKET close
#endif

namespace gcc {

// ── Feedback Packet Protocol v2 ────────────────────────────────────
//  Header: 10 bytes fixed
//  Payload: variable-length razor feedback (0-256 bytes)

#pragma pack(push, 1)
struct FeedbackHeader {
    uint8_t  magic;           // 0xCC
    uint8_t  version;         // 2 = razor
    uint16_t seq;             // latest RTP seq
    uint8_t  loss_pct_x100;   // loss % * 100
    uint16_t fb_size;         // razor feedback data size
    uint8_t  _pad[3];         // reserved
};
#pragma pack(pop)

static constexpr int kFeedbackHeaderSize = 10;
static constexpr int kMaxFeedbackPayload = 256;
static constexpr int kMaxFeedbackPacketSize = kFeedbackHeaderSize + kMaxFeedbackPayload;

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

// ── FeedbackChannel ────────────────────────────────────────────────
class FeedbackChannel {
public:
    FeedbackChannel(const char* addr, uint16_t port, bool is_sender)
        : port_(port), is_sender_(is_sender), sock_(-1), running_(false) {
        if (addr) target_addr_ = addr;
    }

    ~FeedbackChannel() { stop(); }

    FeedbackChannel(const FeedbackChannel&) = delete;
    FeedbackChannel& operator=(const FeedbackChannel&) = delete;

    bool start() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ < 0) { perror("FeedbackChannel: socket"); return false; }

        int opt = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        if (is_sender_) {
            struct sockaddr_in bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_port = htons(port_);
            bind_addr.sin_addr.s_addr = inet_addr(target_addr_.c_str());
            if (bind(sock_, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
                perror("FeedbackChannel: bind");
                CLOSE_SOCKET(sock_); sock_ = -1; return false;
            }
            running_.store(true);
            recv_thread_ = std::thread(&FeedbackChannel::recv_loop, this);
            printf("  Feedback: listening on %s:%d\n", target_addr_.c_str(), port_);
        } else {
            printf("  Feedback: sending to %s:%d\n", target_addr_.c_str(), port_);
        }
        return true;
    }

    void stop() {
        running_.store(false);
        if (recv_thread_.joinable()) recv_thread_.join();
        if (sock_ >= 0) { CLOSE_SOCKET(sock_); sock_ = -1; }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // ── Receiver side: send feedback ──
    void send_feedback(uint16_t seq, uint8_t loss_pct, const uint8_t* razor_data, int razor_size) {
        uint8_t buf[kMaxFeedbackPacketSize];
        FeedbackHeader* hdr = reinterpret_cast<FeedbackHeader*>(buf);
        hdr->magic = 0xCC;
        hdr->version = 2;
        hdr->seq = seq;
        hdr->loss_pct_x100 = loss_pct;
        hdr->fb_size = static_cast<uint16_t>(razor_size);
        memset(hdr->_pad, 0, sizeof(hdr->_pad));

        int total = kFeedbackHeaderSize;
        if (razor_size > 0 && razor_size <= kMaxFeedbackPayload) {
            memcpy(buf + kFeedbackHeaderSize, razor_data, razor_size);
            total += razor_size;
        }

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port_);
        dest.sin_addr.s_addr = inet_addr(target_addr_.c_str());
        sendto(sock_, (const char*)buf, total, 0, (struct sockaddr*)&dest, sizeof(dest));
    }

    // ── Sender side: poll for feedback ──
    struct ReceivedFeedback {
        uint16_t seq;
        uint8_t  loss_pct;
        uint8_t  razor_data[kMaxFeedbackPayload];
        int      razor_size;
    };

    bool poll(ReceivedFeedback& out) {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        if (!rx_ready_) return false;
        out = rx_latest_;
        rx_ready_ = false;
        return true;
    }

    bool is_running() const { return running_.load(); }

private:
    void recv_loop() {
        char buf[kMaxFeedbackPacketSize + 64];
        while (running_.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock_, &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;  // 200ms timeout

            int sel = select(sock_ + 1, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&from, &from_len);
            if (n < kFeedbackHeaderSize) continue;

            const FeedbackHeader* hdr = reinterpret_cast<const FeedbackHeader*>(buf);
            if (hdr->magic != 0xCC || hdr->version != 2) continue;

            int fb_size = hdr->fb_size;
            if (fb_size > kMaxFeedbackPayload) continue;
            if (n < kFeedbackHeaderSize + fb_size) continue;

            std::lock_guard<std::mutex> lock(rx_mutex_);
            rx_latest_.seq = hdr->seq;
            rx_latest_.loss_pct = hdr->loss_pct_x100;
            rx_latest_.razor_size = fb_size;
            if (fb_size > 0) {
                memcpy(rx_latest_.razor_data, buf + kFeedbackHeaderSize, fb_size);
            }
            rx_ready_ = true;
        }
    }

    uint16_t     port_;
    bool         is_sender_;
    std::string  target_addr_;
    int          sock_;
    std::atomic<bool> running_;
    std::thread       recv_thread_;

    // Sender-side received data
    std::mutex        rx_mutex_;
    ReceivedFeedback  rx_latest_{};
    bool              rx_ready_ = false;
};

} // namespace gcc
