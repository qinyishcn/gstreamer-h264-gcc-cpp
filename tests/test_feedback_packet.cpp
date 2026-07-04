#include "feedback_packet.h"
#include "feedback_channel.h"
#include "test_support.h"

#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

namespace {

gcc::FeedbackPacket ExamplePacket() {
    gcc::FeedbackPacket packet;
    packet.sequence = 0x1234;
    packet.loss_q8 = 0x56;
    packet.flags = 0xa5;
    packet.receive_time_ms = 0x01020304;
    packet.feedback_time_ms = 0x89abcdef;
    packet.razor_payload = {0xde, 0xad, 0xbe, 0xef};
    return packet;
}

int TestRoundTripAndWireOrder() {
    const gcc::FeedbackPacket input = ExamplePacket();
    const std::vector<uint8_t> encoded = gcc::EncodeFeedbackPacket(input);
    const std::vector<uint8_t> expected = {
        0xcc, 0x04, 0x12, 0x34, 0x56, 0xa5, 0x00, 0x04,
        0x01, 0x02, 0x03, 0x04, 0x89, 0xab, 0xcd, 0xef,
        0xde, 0xad, 0xbe, 0xef,
    };
    CHECK_TRUE(encoded == expected);

    gcc::FeedbackPacket output;
    CHECK_TRUE(gcc::DecodeFeedbackPacket(encoded.data(), encoded.size(), output));
    CHECK_EQ(output.sequence, input.sequence);
    CHECK_EQ(output.loss_q8, input.loss_q8);
    CHECK_EQ(output.flags, input.flags);
    CHECK_EQ(output.receive_time_ms, input.receive_time_ms);
    CHECK_EQ(output.feedback_time_ms, input.feedback_time_ms);
    CHECK_TRUE(output.razor_payload == input.razor_payload);
    return EXIT_SUCCESS;
}

int TestPayloadBounds() {
    gcc::FeedbackPacket packet = ExamplePacket();
    packet.razor_payload.clear();
    std::vector<uint8_t> encoded = gcc::EncodeFeedbackPacket(packet);
    CHECK_EQ(encoded.size(), static_cast<size_t>(16));
    gcc::FeedbackPacket decoded;
    CHECK_TRUE(gcc::DecodeFeedbackPacket(encoded.data(), encoded.size(), decoded));
    CHECK_TRUE(decoded.razor_payload.empty());

    packet.razor_payload.assign(256, 0x7b);
    encoded = gcc::EncodeFeedbackPacket(packet);
    CHECK_EQ(encoded.size(), static_cast<size_t>(272));
    CHECK_TRUE(gcc::DecodeFeedbackPacket(encoded.data(), encoded.size(), decoded));
    CHECK_EQ(decoded.razor_payload.size(), static_cast<size_t>(256));

    packet.razor_payload.push_back(0);
    CHECK_TRUE(gcc::EncodeFeedbackPacket(packet).empty());
    return EXIT_SUCCESS;
}

int TestRejectsMalformedDatagramsWithoutChangingOutput() {
    const std::vector<uint8_t> valid =
        gcc::EncodeFeedbackPacket(ExamplePacket());
    const gcc::FeedbackPacket sentinel = ExamplePacket();

    gcc::FeedbackPacket output = sentinel;
    CHECK_TRUE(!gcc::DecodeFeedbackPacket(nullptr, valid.size(), output));
    CHECK_TRUE(output.razor_payload == sentinel.razor_payload);
    CHECK_EQ(output.sequence, sentinel.sequence);

    for (size_t size = 0; size < 16; ++size) {
        output = sentinel;
        CHECK_TRUE(!gcc::DecodeFeedbackPacket(valid.data(), size, output));
        CHECK_EQ(output.sequence, sentinel.sequence);
        CHECK_TRUE(output.razor_payload == sentinel.razor_payload);
    }

    for (uint8_t version : {static_cast<uint8_t>(2),
                            static_cast<uint8_t>(3),
                            static_cast<uint8_t>(5)}) {
        std::vector<uint8_t> malformed = valid;
        malformed[1] = version;
        output = sentinel;
        CHECK_TRUE(!gcc::DecodeFeedbackPacket(
            malformed.data(), malformed.size(), output));
        CHECK_EQ(output.sequence, sentinel.sequence);
    }

    std::vector<uint8_t> malformed = valid;
    malformed[0] = 0xcd;
    CHECK_TRUE(!gcc::DecodeFeedbackPacket(
        malformed.data(), malformed.size(), output));

    malformed = valid;
    malformed[6] = 0x01;
    malformed[7] = 0x01;
    CHECK_TRUE(!gcc::DecodeFeedbackPacket(
        malformed.data(), malformed.size(), output));

    malformed = valid;
    malformed[6] = 0;
    malformed[7] = 3;
    CHECK_TRUE(!gcc::DecodeFeedbackPacket(
        malformed.data(), malformed.size(), output));

    malformed = valid;
    malformed[6] = 0;
    malformed[7] = 5;
    CHECK_TRUE(!gcc::DecodeFeedbackPacket(
        malformed.data(), malformed.size(), output));

    malformed = valid;
    malformed.push_back(0);
    CHECK_TRUE(!gcc::DecodeFeedbackPacket(
        malformed.data(), malformed.size(), output));
    return EXIT_SUCCESS;
}

int TestNetworkRtt() {
    uint32_t rtt = 999;
    CHECK_TRUE(gcc::ComputeNetworkRtt(1000, 1100, 20, 50, rtt));
    CHECK_EQ(rtt, static_cast<uint32_t>(70));

    CHECK_TRUE(gcc::ComputeNetworkRtt(
        std::numeric_limits<uint32_t>::max() - 9, 10,
        std::numeric_limits<uint32_t>::max() - 4, 5, rtt));
    CHECK_EQ(rtt, static_cast<uint32_t>(10));

    CHECK_TRUE(gcc::ComputeNetworkRtt(100, 5100, 40, 41, rtt));
    CHECK_EQ(rtt, static_cast<uint32_t>(4999));
    CHECK_TRUE(gcc::ComputeNetworkRtt(100, 5100, 40, 40, rtt));
    CHECK_EQ(rtt, static_cast<uint32_t>(5000));

    rtt = 123;
    CHECK_TRUE(!gcc::ComputeNetworkRtt(100, 5101, 10, 20, rtt));
    CHECK_EQ(rtt, static_cast<uint32_t>(123));
    CHECK_TRUE(!gcc::ComputeNetworkRtt(100, 200, 10, 111, rtt));
    CHECK_TRUE(!gcc::ComputeNetworkRtt(100, 200, 10, 110, rtt));
    CHECK_TRUE(!gcc::ComputeNetworkRtt(100, 100, 10, 10, rtt));
    return EXIT_SUCCESS;
}

int TestLossTrackerQ8() {
    gcc::LossTracker tracker;
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(0));

    tracker.on_packet(65534);
    tracker.on_packet(65535);
    tracker.on_packet(0);
    tracker.on_packet(2);
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(51));

    tracker.reset();
    tracker.on_packet(10);
    tracker.on_packet(12);
    tracker.on_packet(12);
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(85));

    tracker.on_packet(11);
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(0));

    tracker.reset();
    tracker.on_packet(10);
    tracker.on_packet(5000);
    const uint8_t loss_before_old_packet = tracker.get_loss_q8();
    tracker.on_packet(11);
    CHECK_EQ(tracker.get_loss_q8(), loss_before_old_packet);
    tracker.on_packet(10);
    CHECK_EQ(tracker.get_loss_q8(), loss_before_old_packet);

    tracker.reset();
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(0));
    tracker.on_packet(100);
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(0));

    tracker.reset();
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
        for (uint32_t sequence = 0; sequence <= 0xffff; ++sequence) {
            tracker.on_packet(static_cast<uint16_t>(sequence));
        }
    }
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(0));
    tracker.on_packet(1000);
    CHECK_TRUE(tracker.get_loss_q8() > static_cast<uint8_t>(250));

    tracker.reset();
    tracker.on_packet(0);
    tracker.on_packet(1000);
    CHECK_TRUE(tracker.get_loss_q8() > static_cast<uint8_t>(250));
    for (uint32_t sequence = 1001; sequence <= 1256; ++sequence) {
        tracker.on_packet(static_cast<uint16_t>(sequence));
    }
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(0));
    tracker.on_packet(1000);
    CHECK_EQ(tracker.get_loss_q8(), static_cast<uint8_t>(0));
    return EXIT_SUCCESS;
}

bool WaitForQueueSize(gcc::FeedbackChannel& channel, size_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (channel.queued_feedback_count() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return channel.queued_feedback_count() == expected;
}

bool WaitForDroppedCount(gcc::FeedbackChannel& channel, uint64_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (channel.dropped_feedback_count() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return channel.dropped_feedback_count() == expected;
}

int TestFeedbackChannelQueuePolicies() {
    gcc::FeedbackChannel listener("127.0.0.1", 0, true);
    CHECK_TRUE(listener.start());
    CHECK_TRUE(listener.bound_port() != 0);
    gcc::FeedbackChannel sender(
        "127.0.0.1", listener.bound_port(), false);
    CHECK_TRUE(sender.start());

    for (uint16_t sequence = 1; sequence <= 3; ++sequence) {
        gcc::FeedbackPacket packet;
        packet.sequence = sequence;
        CHECK_TRUE(sender.send_feedback(packet));
    }
    CHECK_TRUE(WaitForQueueSize(listener, 3));

    gcc::FeedbackPacket packet;
    CHECK_TRUE(listener.poll(packet));
    CHECK_EQ(packet.sequence, static_cast<uint16_t>(1));
    CHECK_TRUE(listener.poll(packet));
    CHECK_EQ(packet.sequence, static_cast<uint16_t>(2));
    CHECK_TRUE(listener.poll(packet));
    CHECK_EQ(packet.sequence, static_cast<uint16_t>(3));
    CHECK_TRUE(!listener.poll(packet));

    for (uint16_t sequence = 10; sequence <= 12; ++sequence) {
        packet.sequence = sequence;
        CHECK_TRUE(sender.send_feedback(packet));
    }
    CHECK_TRUE(WaitForQueueSize(listener, 3));
    gcc::FeedbackChannel::ReceivedFeedback legacy;
    CHECK_TRUE(listener.poll(legacy));
    CHECK_EQ(legacy.seq, static_cast<uint16_t>(12));
    CHECK_EQ(listener.queued_feedback_count(), static_cast<size_t>(0));

    for (uint16_t sequence = 100; sequence < 170; ++sequence) {
        packet.sequence = sequence;
        CHECK_TRUE(sender.send_feedback(packet));
    }
    CHECK_TRUE(WaitForDroppedCount(listener, 6));
    CHECK_EQ(listener.queued_feedback_count(), static_cast<size_t>(64));
    CHECK_EQ(listener.dropped_feedback_count(), static_cast<uint64_t>(6));
    for (uint16_t sequence = 106; sequence < 170; ++sequence) {
        CHECK_TRUE(listener.poll(packet));
        CHECK_EQ(packet.sequence, sequence);
    }
    CHECK_TRUE(!listener.poll(packet));
    sender.stop();
    listener.stop();
    return EXIT_SUCCESS;
}

int TestConcurrentSendAndStop() {
    gcc::FeedbackChannel listener("127.0.0.1", 0, true);
    CHECK_TRUE(listener.start());
    gcc::FeedbackChannel sender(
        "127.0.0.1", listener.bound_port(), false);
    CHECK_TRUE(sender.start());
    gcc::FeedbackPacket packet;
    std::atomic<bool> keep_sending{true};
    std::atomic<uint32_t> send_attempts{0};
    std::thread send_thread([&]() {
        while (keep_sending.load()) {
            sender.send_feedback(packet);
            send_attempts.fetch_add(1);
        }
    });
    while (send_attempts.load() < 100) {
        std::this_thread::yield();
    }
    sender.stop();
    keep_sending.store(false);
    send_thread.join();
    CHECK_TRUE(!sender.send_feedback(packet));
    listener.stop();
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    CHECK_EQ(TestRoundTripAndWireOrder(), EXIT_SUCCESS);
    CHECK_EQ(TestPayloadBounds(), EXIT_SUCCESS);
    CHECK_EQ(TestRejectsMalformedDatagramsWithoutChangingOutput(), EXIT_SUCCESS);
    CHECK_EQ(TestNetworkRtt(), EXIT_SUCCESS);
    CHECK_EQ(TestLossTrackerQ8(), EXIT_SUCCESS);
    CHECK_EQ(TestFeedbackChannelQueuePolicies(), EXIT_SUCCESS);
    CHECK_EQ(TestConcurrentSendAndStop(), EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
