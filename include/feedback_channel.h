#pragma once

/**
 * Feedback Channel for GCC Video Transmission
 *
 * Provides a UDP-based feedback channel between receiver and sender.
 * The receiver measures delay gradients and packet loss from the RTP stream,
 * then sends periodic feedback reports back to the sender via a separate UDP port.
 *
 * Protocol: lightweight binary UDP packets (26 bytes).
 *
 * Time handling:
 *   - sender_timestamp_us: sender's local wall clock (steady_clock) when sending
 *   - receiver_timestamp_us: receiver's local wall clock when receiving
 *   - delay = receiver_timestamp_us - sender_timestamp_us (on loopback ≈ 0)
 *   - delay_gradient = delay[n] - delay[n-1] (GCC overuse detector input)
 */

#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <chrono>
#include <unordered_map>
#include "gcc_controller.h"

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

// ── Feedback Packet Protocol (24 bytes) ─────────────────────────────
//
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |     magic     |     version   |         seq                   |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |           sender_timestamp_us (8 bytes, wall clock)           |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |           receiver_timestamp_us (8 bytes, wall clock)         |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |  loss_pct_x100  |          padding (3 bytes)                 |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

#pragma pack(push, 1)
struct FeedbackPacket {
    uint8_t  magic;                    // 0xCC
    uint8_t  version;                  // 1
    uint16_t seq;                      // RTP sequence number
    uint64_t sender_timestamp_us;      // Sender's wall clock when sending (microseconds)
    uint64_t receiver_timestamp_us;    // Receiver's wall clock when receiving (microseconds)
    uint8_t  loss_pct_x100;            // Loss percentage * 100
    uint8_t  _pad[3];                  // Reserved
};
#pragma pack(pop)

static_assert(sizeof(FeedbackPacket) == 24, "FeedbackPacket must be 24 bytes");

// ── Helper: get current time in microseconds ──────────────────────────

inline uint64_t get_timestamp_us() {
    auto now = std::chrono::steady_clock::now();
    auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                   now.time_since_epoch()).count();
    return static_cast<uint64_t>(us);
}

// ── Sender-side send-time tracker ─────────────────────────────────────
// Maps RTP seq → sender's wall clock time when that packet was sent.
// The sender's probe populates this; the feedback poll looks it up.

class SendTimeTracker {
public:
    /** Record that seq was sent at wall_clock_us. */
    void record(uint16_t seq, uint64_t wall_clock_us) {
        std::lock_guard<std::mutex> lock(mu_);
        send_times_[seq] = wall_clock_us;
        // Keep only last 1000 entries
        if (send_times_.size() > 1000) {
            // Evict oldest (approximate: just clear if too big)
            auto it = send_times_.begin();
            send_times_.erase(it);
        }
    }

    /** Look up send time for seq. Returns true if found. */
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

// ── FeedbackChannel ──────────────────────────────────────────────────

class FeedbackChannel {
public:
    /**
     * @param addr        Sender side: bind address ("0.0.0.0"). Receiver side: sender's IP.
     * @param port        UDP port (sender listens, receiver sends to this port)
     * @param is_sender   true = sender side (listens for reports), false = receiver side (sends reports)
     * @param tracker     Sender-side send time tracker (only used when is_sender=true)
     */
    FeedbackChannel(const char* addr, uint16_t port, bool is_sender,
                    SendTimeTracker* tracker = nullptr)
        : port_(port)
        , is_sender_(is_sender)
        , sock_(-1)
        , running_(false)
        , recv_report_ready_(false)
        , tracker_(tracker)
    {
        if (addr) target_addr_ = addr;
    }

    ~FeedbackChannel() { stop(); }

    FeedbackChannel(const FeedbackChannel&) = delete;
    FeedbackChannel& operator=(const FeedbackChannel&) = delete;

    /** Initialize and start the UDP socket + recv thread (sender) or just socket (receiver). */
    bool start() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "FeedbackChannel: WSAStartup failed\n");
            return false;
        }
#endif

        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ < 0) {
            perror("FeedbackChannel: socket");
            return false;
        }

        int opt = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        if (is_sender_) {
            // Sender: bind to receive feedback
            struct sockaddr_in bind_addr;
            memset(&bind_addr, 0, sizeof(bind_addr));
            bind_addr.sin_family      = AF_INET;
            bind_addr.sin_port        = htons(port_);
            bind_addr.sin_addr.s_addr = inet_addr(target_addr_.c_str());

            if (bind(sock_, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
                perror("FeedbackChannel: bind");
                CLOSE_SOCKET(sock_);
                sock_ = -1;
                return false;
            }

            // Start recv thread
            running_.store(true);
            recv_thread_ = std::thread(&FeedbackChannel::recv_loop, this);
            printf("  📡 Feedback: listening on %s:%d\n", target_addr_.c_str(), port_);
        } else {
            // Receiver: no bind needed, just send to sender
            printf("  📡 Feedback: sending to %s:%d\n", target_addr_.c_str(), port_);
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

    // ── Receiver side API ─────────────────────────────────────────

    /**
     * Record an RTP packet arrival. Call from pad probe.
     * @param seq               RTP sequence number
     * @param arrival_us        Receiver's local wall clock (microseconds)
     */
    void on_rtp_packet(uint16_t seq, uint64_t arrival_us) {
        std::lock_guard<std::mutex> lock(pkt_mutex_);
        recent_pkts_.push_back({seq, arrival_us});
        while (recent_pkts_.size() > 500) {
            recent_pkts_.pop_front();
        }
    }

    /**
     * Build and send a feedback report to the sender.
     * @param loss_fraction  Current packet loss fraction [0.0, 1.0]
     */
    void send_report(double loss_fraction) {
        std::lock_guard<std::mutex> lock(pkt_mutex_);
        if (recent_pkts_.empty()) return;

        FeedbackPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic   = 0xCC;
        pkt.version = 1;

        // Use latest packet's info
        auto& latest = recent_pkts_.back();
        pkt.seq = latest.seq;

        // Sender timestamp: the sender's wall clock when it sent this packet.
        // We don't know this on the receiver side — sender will look it up
        // from its own tracker. For the packet, we store 0 here.
        pkt.sender_timestamp_us = 0;

        // Receiver timestamp: receiver's local wall clock when we got this packet
        pkt.receiver_timestamp_us = latest.arrival;

        // Loss percentage * 100
        double loss_pct = loss_fraction * 100.0;
        if (loss_pct > 100.0) loss_pct = 100.0;
        pkt.loss_pct_x100 = static_cast<uint8_t>(loss_pct + 0.5);

        // Send to sender
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(port_);
        dest.sin_addr.s_addr = inet_addr(target_addr_.c_str());

        sendto(sock_, (const char*)&pkt, sizeof(pkt), 0,
               (struct sockaddr*)&dest, sizeof(dest));
    }

    // ── Sender side API ───────────────────────────────────────────

    /**
     * Poll for a new feedback report.
     * Computes one-way delay using sender's send time tracker.
     */
    bool poll(FeedbackReport& report) {
        std::lock_guard<std::mutex> lock(report_mutex_);
        if (!recv_report_ready_) return false;
        report = last_report_;
        recv_report_ready_ = false;
        return true;
    }

    bool is_running() const { return running_.load(); }

private:
    void recv_loop() {
        char buf[256];
        while (running_.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock_, &fds);
            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 500000;

            int sel = select(sock_ + 1, &fds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&from, &from_len);
            if (n < static_cast<ssize_t>(sizeof(FeedbackPacket))) continue;

            FeedbackPacket* pkt = reinterpret_cast<FeedbackPacket*>(buf);
            if (pkt->magic != 0xCC || pkt->version != 1) continue;

            // Compute one-way delay using sender's send time tracker
            double delay_ms = 0.0;
            if (tracker_) {
                uint64_t send_time_us = 0;
                if (tracker_->lookup(pkt->seq, send_time_us)) {
                    // delay = receiver_arrival - sender_send (both wall clock)
                    delay_ms = static_cast<double>(pkt->receiver_timestamp_us)
                             - static_cast<double>(send_time_us);
                    delay_ms /= 1000.0;  // us → ms
                }
            }

            // Convert to FeedbackReport
            FeedbackReport report;
            report.timestamp_us  = pkt->receiver_timestamp_us;
            report.delay_ms      = delay_ms;
            report.loss_fraction = pkt->loss_pct_x100 / 100.0;

            std::lock_guard<std::mutex> lock(report_mutex_);
            last_report_ = report;
            recv_report_ready_ = true;
        }
    }

    // Config
    uint16_t     port_;
    bool         is_sender_;
    std::string  target_addr_;

    // Socket
    int sock_;

    // Sender-side send time tracker
    SendTimeTracker* tracker_;

    // Thread
    std::atomic<bool> running_;
    std::thread       recv_thread_;

    // Receiver-side packet records
    struct PktRecord {
        uint16_t seq;
        uint64_t arrival;
    };
    std::mutex              pkt_mutex_;
    std::deque<PktRecord>   recent_pkts_;

    // Sender-side received reports
    std::mutex        report_mutex_;
    FeedbackReport    last_report_;
    bool              recv_report_ready_;
};

} // namespace gcc
