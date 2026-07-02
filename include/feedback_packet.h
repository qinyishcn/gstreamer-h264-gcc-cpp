#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gcc {

constexpr size_t kFeedbackHeaderSize = 16;
constexpr size_t kMaxFeedbackPayload = 256;
constexpr size_t kMaxFeedbackPacketSize =
    kFeedbackHeaderSize + kMaxFeedbackPayload;

struct FeedbackPacket {
    uint16_t sequence = 0;
    uint8_t loss_q8 = 0;
    uint8_t flags = 0;
    uint32_t receive_time_ms = 0;
    uint32_t feedback_time_ms = 0;
    std::vector<uint8_t> razor_payload;
};

std::vector<uint8_t> EncodeFeedbackPacket(const FeedbackPacket& packet);

// On failure, output is left unchanged.
bool DecodeFeedbackPacket(const uint8_t* data, size_t size,
                          FeedbackPacket& output);

// On failure, out_rtt_ms is left unchanged.
bool ComputeNetworkRtt(uint32_t sender_send_ms,
                       uint32_t sender_feedback_ms,
                       uint32_t receiver_receive_ms,
                       uint32_t receiver_feedback_ms,
                       uint32_t& out_rtt_ms);

}  // namespace gcc
