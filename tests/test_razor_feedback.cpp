#include "test_support.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include "cf_platform.h"
#include "cf_stream.h"
#include "estimator_common.h"
#include "remb_sender.h"
#include "sender_congestion_controller.h"
}

namespace {

size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

feedback_msg_t MakeFeedback(uint8_t flag, uint8_t samples_num = 2) {
    feedback_msg_t message = {};
    message.flag = flag;
    message.remb = 456000;
    message.fraction_loss = 64;
    message.packet_num = 20;
    message.base_seq = 42;
    message.min_ts = 1000;
    message.samples_num = samples_num;
    for (uint8_t i = 0; i < samples_num && i < MAX_FEELBACK_COUNT; ++i) {
        message.samples[i].seq = static_cast<uint16_t>(200 + i);
        message.samples[i].ts = message.min_ts + i + 1;
    }
    return message;
}

std::vector<uint8_t> EncodeFeedback(feedback_msg_t* message) {
    bin_stream_t stream;
    if (bin_stream_init(&stream) != 0) {
        return {};
    }
    feedback_msg_encode(&stream, message);
    const std::vector<uint8_t> encoded(stream.data, stream.data + stream.used);
    bin_stream_destroy(&stream);
    return encoded;
}

int DecodeFeedback(const std::vector<uint8_t>& encoded,
                   feedback_msg_t* message) {
    bin_stream_t stream;
    if (bin_stream_init(&stream) != 0) {
        return -1;
    }
    if (!encoded.empty()) {
        mach_block_write(&stream, encoded.data(), encoded.size());
    }
    bin_stream_rewind(&stream, 0);
    const int result = feedback_msg_decode(&stream, message);
    bin_stream_destroy(&stream);
    return result;
}

std::vector<uint8_t> MakeRawProxyFeedback(int64_t min_ts,
                                          uint8_t samples_num,
                                          uint16_t delta_ts) {
    bin_stream_t stream;
    if (bin_stream_init(&stream) != 0) {
        return {};
    }
    mach_uint8_write(&stream, proxy_ts_msg);
    mach_int64_write(&stream, 42);
    mach_int64_write(&stream, min_ts);
    mach_uint8_write(&stream, samples_num);
    if (samples_num == 1) {
        mach_uint16_write(&stream, 200);
        mach_uint16_write(&stream, delta_ts);
    }
    const std::vector<uint8_t> encoded(stream.data, stream.data + stream.used);
    bin_stream_destroy(&stream);
    return encoded;
}

int TestDecoderRoundTripsAllFlagCombinations() {
    for (uint8_t flag = 0; flag <= 7; ++flag) {
        feedback_msg_t original = MakeFeedback(flag);
        const std::vector<uint8_t> encoded = EncodeFeedback(&original);
        CHECK_TRUE(!encoded.empty());

        feedback_msg_t decoded = {};
        CHECK_EQ(DecodeFeedback(encoded, &decoded), 0);
        CHECK_EQ(decoded.flag, flag);
        if ((flag & remb_msg) != 0) {
            CHECK_EQ(decoded.remb, original.remb);
        }
        if ((flag & loss_info_msg) != 0) {
            CHECK_EQ(decoded.fraction_loss, original.fraction_loss);
            CHECK_EQ(decoded.packet_num, original.packet_num);
        }
        if ((flag & proxy_ts_msg) != 0) {
            CHECK_EQ(decoded.base_seq, original.base_seq);
            CHECK_EQ(decoded.min_ts, original.min_ts);
            CHECK_EQ(decoded.samples_num, original.samples_num);
            for (uint8_t i = 0; i < original.samples_num; ++i) {
                CHECK_EQ(decoded.samples[i].seq, original.samples[i].seq);
                CHECK_EQ(decoded.samples[i].ts, original.samples[i].ts);
            }
        }
    }
    return EXIT_SUCCESS;
}

int TestDecoderRejectsEveryTruncatedPrefix() {
    for (uint8_t flag = 0; flag <= 7; ++flag) {
        feedback_msg_t message = MakeFeedback(flag);
        const std::vector<uint8_t> encoded = EncodeFeedback(&message);
        CHECK_TRUE(!encoded.empty());
        for (size_t size = 0; size < encoded.size(); ++size) {
            const std::vector<uint8_t> truncated(encoded.begin(),
                                                  encoded.begin() + size);
            feedback_msg_t decoded = {};
            CHECK_TRUE(DecodeFeedback(truncated, &decoded) != 0);
        }
    }
    return EXIT_SUCCESS;
}

int TestDecoderSampleLimits() {
    feedback_msg_t maximum = MakeFeedback(proxy_ts_msg, MAX_FEELBACK_COUNT);
    const std::vector<uint8_t> encoded = EncodeFeedback(&maximum);
    feedback_msg_t decoded = {};
    CHECK_EQ(DecodeFeedback(encoded, &decoded), 0);
    CHECK_EQ(decoded.samples_num, static_cast<uint8_t>(MAX_FEELBACK_COUNT));
    CHECK_EQ(decoded.samples[MAX_FEELBACK_COUNT - 1].seq,
             maximum.samples[MAX_FEELBACK_COUNT - 1].seq);

    const std::vector<uint8_t> too_many =
        MakeRawProxyFeedback(1000, MAX_FEELBACK_COUNT + 1, 0);
    CHECK_TRUE(DecodeFeedback(too_many, &decoded) != 0);
    return EXIT_SUCCESS;
}

int TestDecoderRejectsNullArguments() {
    feedback_msg_t message = {};
    CHECK_TRUE(feedback_msg_decode(nullptr, &message) != 0);

    bin_stream_t stream;
    CHECK_EQ(bin_stream_init(&stream), 0);
    CHECK_TRUE(feedback_msg_decode(&stream, nullptr) != 0);
    bin_stream_destroy(&stream);
    return EXIT_SUCCESS;
}

int TestDecoderRejectsTimestampOverflow() {
    const std::vector<uint8_t> encoded = MakeRawProxyFeedback(
        std::numeric_limits<int64_t>::max(), 1, 1);
    feedback_msg_t decoded = {};
    CHECK_TRUE(DecodeFeedback(encoded, &decoded) != 0);
    return EXIT_SUCCESS;
}

int TestFeedbackHandlersRejectMalformedInputs() {
    uint8_t empty_feedback = 0;
    sender_on_feedback(nullptr, &empty_feedback, 1);
    remb_sender_on_feedback(nullptr, &empty_feedback, 1);

    sender_cc_t* sender =
        sender_cc_create(nullptr, nullptr, nullptr, nullptr, 250);
    CHECK_TRUE(sender != nullptr);
    sender_on_feedback(sender, nullptr, 1);
    sender_on_feedback(sender, &empty_feedback, 0);
    sender_on_feedback(sender, &empty_feedback, -1);
    sender_on_feedback(sender, &empty_feedback, 1473);
    sender_cc_destroy(sender);

    remb_sender_t* remb =
        remb_sender_create(nullptr, nullptr, nullptr, nullptr, 250, 0);
    CHECK_TRUE(remb != nullptr);
    remb_sender_on_feedback(remb, nullptr, 1);
    remb_sender_on_feedback(remb, &empty_feedback, 0);
    remb_sender_on_feedback(remb, &empty_feedback, -1);
    remb_sender_on_feedback(remb, &empty_feedback, 1473);
    remb_sender_destroy(remb);
    return EXIT_SUCCESS;
}

int TestCombinedFeedbackProcessesEverySection() {
    sender_cc_t* sender =
        sender_cc_create(nullptr, nullptr, nullptr, nullptr, 250);
    CHECK_TRUE(sender != nullptr);
    sender_cc_set_bitrates(sender, 10000, 300000, 1000000);
    sender_on_send_packet(sender, 77, 1200);

    feedback_msg_t message =
        MakeFeedback(proxy_ts_msg | remb_msg | loss_info_msg, 1);
    message.min_ts = GET_SYS_MS();
    message.samples[0].seq = 77;
    message.samples[0].ts = message.min_ts;
    const std::vector<uint8_t> encoded = EncodeFeedback(&message);
    CHECK_TRUE(!encoded.empty());

    sender_on_feedback(sender, const_cast<uint8_t*>(encoded.data()),
                       static_cast<int>(encoded.size()));
    CHECK_EQ(sender->adapter.num, 1);
    CHECK_EQ(sender->adapter.packets[0].sequence_number,
             static_cast<uint16_t>(77));
    CHECK_EQ(sender->ack->sum, 1200);
    CHECK_EQ(sender->bitrate_controller->est->bwe_incoming, message.remb);
    CHECK_EQ(sender->bitrate_controller->est->last_fraction_loss,
             message.fraction_loss);

    sender_cc_destroy(sender);
    return EXIT_SUCCESS;
}

int TestSenderProcessesFeedbackOnce(const char* source_path) {
    std::ifstream source_file(source_path);
    CHECK_TRUE(source_file.good());

    const std::string source((std::istreambuf_iterator<char>(source_file)),
                             std::istreambuf_iterator<char>());
    const size_t begin = source.find("void sender_on_feedback");
    const size_t end = source.find("void sender_cc_update_rtt", begin);
    CHECK_TRUE(begin != std::string::npos);
    CHECK_TRUE(end != std::string::npos);

    const std::string function = source.substr(begin, end - begin);
    CHECK_EQ(CountOccurrences(function, "cc_feedback_on_feedback("),
             static_cast<size_t>(1));
    CHECK_EQ(CountOccurrences(function, "bitrate_controller_on_report("),
             static_cast<size_t>(1));
    CHECK_EQ(CountOccurrences(function, "bitrate_controller_on_remb("),
             static_cast<size_t>(1));
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    CHECK_EQ(argc, 2);
    CHECK_EQ(TestDecoderRoundTripsAllFlagCombinations(), EXIT_SUCCESS);
    CHECK_EQ(TestDecoderRejectsEveryTruncatedPrefix(), EXIT_SUCCESS);
    CHECK_EQ(TestDecoderSampleLimits(), EXIT_SUCCESS);
    CHECK_EQ(TestDecoderRejectsNullArguments(), EXIT_SUCCESS);
    CHECK_EQ(TestDecoderRejectsTimestampOverflow(), EXIT_SUCCESS);
    CHECK_EQ(TestFeedbackHandlersRejectMalformedInputs(), EXIT_SUCCESS);
    CHECK_EQ(TestCombinedFeedbackProcessesEverySection(), EXIT_SUCCESS);
    CHECK_EQ(TestSenderProcessesFeedbackOnce(argv[1]), EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
