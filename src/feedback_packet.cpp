#include "feedback_packet.h"

#include <algorithm>
#include <utility>

namespace gcc {
namespace {

constexpr uint8_t kFeedbackMagic = 0xcc;
constexpr uint8_t kFeedbackVersion = 4;

void WriteBe16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value >> 8);
    output[1] = static_cast<uint8_t>(value);
}

void WriteBe32(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value >> 24);
    output[1] = static_cast<uint8_t>(value >> 16);
    output[2] = static_cast<uint8_t>(value >> 8);
    output[3] = static_cast<uint8_t>(value);
}

uint16_t ReadBe16(const uint8_t* input) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(input[0]) << 8) |
        static_cast<uint16_t>(input[1]));
}

uint32_t ReadBe32(const uint8_t* input) {
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) |
           static_cast<uint32_t>(input[3]);
}

}  // namespace

std::vector<uint8_t> EncodeFeedbackPacket(const FeedbackPacket& packet) {
    if (packet.razor_payload.size() > kMaxFeedbackPayload) {
        return {};
    }

    std::vector<uint8_t> encoded(
        kFeedbackHeaderSize + packet.razor_payload.size());
    encoded[0] = kFeedbackMagic;
    encoded[1] = kFeedbackVersion;
    WriteBe16(encoded.data() + 2, packet.sequence);
    encoded[4] = packet.loss_q8;
    encoded[5] = packet.flags;
    WriteBe16(encoded.data() + 6,
              static_cast<uint16_t>(packet.razor_payload.size()));
    WriteBe32(encoded.data() + 8, packet.receive_time_ms);
    WriteBe32(encoded.data() + 12, packet.feedback_time_ms);
    std::copy(packet.razor_payload.begin(), packet.razor_payload.end(),
              encoded.begin() + kFeedbackHeaderSize);
    return encoded;
}

bool DecodeFeedbackPacket(const uint8_t* data, size_t size,
                          FeedbackPacket& output) {
    if (data == nullptr || size < kFeedbackHeaderSize ||
        data[0] != kFeedbackMagic || data[1] != kFeedbackVersion) {
        return false;
    }

    const size_t payload_size = ReadBe16(data + 6);
    if (payload_size > kMaxFeedbackPayload ||
        size != kFeedbackHeaderSize + payload_size) {
        return false;
    }

    FeedbackPacket decoded;
    decoded.sequence = ReadBe16(data + 2);
    decoded.loss_q8 = data[4];
    decoded.flags = data[5];
    decoded.receive_time_ms = ReadBe32(data + 8);
    decoded.feedback_time_ms = ReadBe32(data + 12);
    decoded.razor_payload.assign(
        data + kFeedbackHeaderSize, data + size);
    output = std::move(decoded);
    return true;
}

bool ComputeNetworkRtt(uint32_t sender_send_ms,
                       uint32_t sender_feedback_ms,
                       uint32_t receiver_receive_ms,
                       uint32_t receiver_feedback_ms,
                       uint32_t& out_rtt_ms) {
    const uint32_t sender_elapsed = sender_feedback_ms - sender_send_ms;
    const uint32_t receiver_residence =
        receiver_feedback_ms - receiver_receive_ms;
    if (sender_elapsed > 5000 || receiver_residence > sender_elapsed) {
        return false;
    }

    const uint32_t network_rtt = sender_elapsed - receiver_residence;
    if (network_rtt == 0) {
        return false;
    }
    out_rtt_ms = network_rtt;
    return true;
}

}  // namespace gcc
