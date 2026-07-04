#include "test_support.h"
#include "razor_wrapper.h"

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

extern "C" {
#include "cf_platform.h"
#include "cf_stream.h"
#include "bbr_pacer.h"
#include "estimator_common.h"
#include "remb_sender.h"
#include "sender_congestion_controller.h"
}

namespace {

static_assert(
    std::is_same<sender_get_pacer_queue_ms_func,
                 int (*)(razor_sender_t*)>::value,
    "the existing pacer queue callback ABI must remain int");
static_assert(offsetof(razor_sender_t, add_packet) ==
                  offsetof(razor_sender_t, set_bitrates) +
                      sizeof(sender_set_bitrates),
              "new fields must not shift existing sender operations");
static_assert(offsetof(razor_sender_t, on_send) ==
                  offsetof(razor_sender_t, add_packet) +
                      sizeof(sender_add_packet_func),
              "the legacy on-send offset must remain stable");
static_assert(offsetof(razor_sender_t, on_feedback) ==
                  offsetof(razor_sender_t, on_send) +
                      sizeof(sender_on_send_func),
              "the legacy feedback offset must remain stable");
static_assert(offsetof(razor_sender_t, update_rtt) ==
                  offsetof(razor_sender_t, on_feedback) +
                      sizeof(sender_on_feedback_func),
              "the legacy RTT offset must remain stable");
static_assert(offsetof(razor_sender_t, get_pacer_queue_ms) ==
                  offsetof(razor_sender_t, update_rtt) +
                      sizeof(sender_update_rtt_func),
              "the legacy queue getter offset must remain stable");
static_assert(offsetof(razor_sender_t, get_first_timestamp) ==
                  offsetof(razor_sender_t, get_pacer_queue_ms) +
                      sizeof(sender_get_pacer_queue_ms_func),
              "the legacy first timestamp offset must remain stable");
static_assert(offsetof(razor_sender_t, set_pacing_rate) ==
                  offsetof(razor_sender_t, get_first_timestamp) +
                      sizeof(sender_get_first_ts),
              "new sender operations must be appended");
static_assert(!std::is_copy_constructible<razor::Sender>::value, "");
static_assert(!std::is_copy_assignable<razor::Sender>::value, "");
static_assert(!std::is_move_constructible<razor::Sender>::value, "");
static_assert(!std::is_move_assignable<razor::Sender>::value, "");
static_assert(!std::is_copy_constructible<razor::Receiver>::value, "");
static_assert(!std::is_copy_assignable<razor::Receiver>::value, "");
static_assert(!std::is_move_constructible<razor::Receiver>::value, "");
static_assert(!std::is_move_assignable<razor::Receiver>::value, "");

struct ReentrantPacerContext {
    sender_cc_t* sender = nullptr;
    int calls = 0;
    bool queue_empty_during_callback = false;
};

struct BudgetReentryContext {
    pace_sender_t* pacer = nullptr;
    int64_t now_ts = 0;
    int calls = 0;
};

void BudgetReentryCallback(void* handler, uint32_t, int, size_t, int) {
    auto* context = static_cast<BudgetReentryContext*>(handler);
    ++context->calls;
    if (context->calls == 1) {
        pace_try_transmit(context->pacer, context->now_ts);
    }
}

void ReentrantPacerCallback(void* handler, uint32_t, int, size_t, int) {
    auto* context = static_cast<ReentrantPacerContext*>(handler);
    ++context->calls;
    context->queue_empty_during_callback =
        pace_queue_size(context->sender->pacer) == 0;
    if (context->calls == 1) {
        sender_cc_heartbeat(context->sender);
    }
}

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
    CHECK_EQ(sender->matched_feedback_packets, static_cast<uint64_t>(1));
    razor_sender_stats_t stats = {};
    CHECK_EQ(razor_sender_get_stats(
                 reinterpret_cast<razor_sender_t*>(sender), &stats),
             0);
    CHECK_EQ(stats.matched_feedback_packets, static_cast<uint64_t>(1));

    sender_cc_destroy(sender);
    return EXIT_SUCCESS;
}

int TestPacerConfigurationAndCppStats() {
    pace_sender_t* raw_pacer = pace_create(nullptr, nullptr, 250);
    CHECK_TRUE(raw_pacer != nullptr);
    CHECK_EQ(raw_pacer->pacing_factor_x100, static_cast<uint32_t>(100));
    pace_set_bitrate_limits(raw_pacer, 200000);
    pace_set_estimate_bitrate(raw_pacer, 1000000);
    CHECK_EQ(raw_pacer->pacing_bitrate_kpbs, static_cast<uint32_t>(1000));
    pace_set_pacing_rate(raw_pacer, 200000);
    CHECK_EQ(raw_pacer->pacing_bitrate_kpbs, static_cast<uint32_t>(400));
    CHECK_EQ(pace_queue_ms(raw_pacer), static_cast<int64_t>(0));
    pace_destroy(raw_pacer);

    sender_cc_t* short_queue =
        sender_cc_create(nullptr, nullptr, nullptr, nullptr, 100);
    CHECK_TRUE(short_queue != nullptr);
    CHECK_EQ(short_queue->accepted_queue_ms, 100);
    CHECK_EQ(short_queue->pacer->que.max_que_ms, static_cast<uint32_t>(100));
    sender_cc_destroy(short_queue);

    sender_cc_t* default_queue =
        sender_cc_create(nullptr, nullptr, nullptr, nullptr, 0);
    CHECK_TRUE(default_queue != nullptr);
    CHECK_EQ(default_queue->accepted_queue_ms, 250);
    CHECK_EQ(default_queue->pacer->que.max_que_ms,
             static_cast<uint32_t>(250));
    sender_cc_destroy(default_queue);

    sender_cc_t* clamped_queue =
        sender_cc_create(nullptr, nullptr, nullptr, nullptr, 500);
    CHECK_TRUE(clamped_queue != nullptr);
    CHECK_EQ(clamped_queue->accepted_queue_ms, 250);
    CHECK_EQ(clamped_queue->pacer->que.max_que_ms,
             static_cast<uint32_t>(250));
    sender_cc_destroy(clamped_queue);

    std::vector<uint32_t> paced_packets;
    razor::PaceSendCallback pace_cb =
        [&paced_packets](uint32_t packet_id, int, size_t, bool) {
            paced_packets.push_back(packet_id);
        };
    std::function<void(uint32_t, uint8_t, uint32_t)> bitrate_cb;
    razor::Sender sender(bitrate_cb, pace_cb, 250);
    CHECK_TRUE(sender.is_valid());
    sender.set_bitrates(200, 1000, 8000);
    sender.set_pacing_rate(1200);

    CHECK_TRUE(sender.add_packet(7, false, 1200));
    CHECK_TRUE(sender.pacer_queue_ms() >= 0);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (paced_packets.empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sender.heartbeat();
    }
    CHECK_EQ(paced_packets.size(), static_cast<size_t>(1));
    CHECK_EQ(paced_packets[0], static_cast<uint32_t>(7));
    CHECK_EQ(sender.pacer_queue_ms(), static_cast<int64_t>(0));

    const razor::SenderStats stats = sender.stats();
    CHECK_EQ(stats.acked_bitrate_kbps, static_cast<uint32_t>(0));
    CHECK_EQ(stats.matched_feedback_packets, static_cast<uint64_t>(0));
    CHECK_EQ(stats.pacer_queue_ms, static_cast<int64_t>(0));

    razor::Sender legacy_sender(bitrate_cb);
    CHECK_TRUE(legacy_sender.is_valid());
    return EXIT_SUCCESS;
}

int TestCheckedPacingRateApiAndOverflowConversion() {
    CHECK_EQ(razor_sender_set_pacing_rate(nullptr, 1200000), -1);

    razor_sender_t* bbr = razor_sender_create(
        bbr_congestion, 0, nullptr, nullptr, nullptr, nullptr, 0);
    CHECK_TRUE(bbr != nullptr);
    bbr->set_bitrates(bbr, 10000, 300000, 1000000);
    CHECK_EQ(bbr->add_packet(bbr, 55, 0, 100), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    bbr->heartbeat(bbr);
    CHECK_EQ(razor_sender_set_pacing_rate(bbr, 1200000), -1);
    razor_sender_destroy(bbr);

    bbr_pacer_t* bbr_pacer =
        bbr_pacer_create(nullptr, nullptr, nullptr, nullptr, 0, 0);
    CHECK_TRUE(bbr_pacer != nullptr);
    CHECK_EQ(bbr_pacer->que.max_que_ms, static_cast<uint32_t>(250));
    bbr_pacer_set_pacing_rate(bbr_pacer, 1200);
    bbr_pacer->last_update_ts = 1000;
    CHECK_EQ(bbr_pacer_insert_packet(bbr_pacer, 56, 0, 100, 0), 0);
    bbr_pacer_try_transmit(bbr_pacer, 1005);
    CHECK_TRUE(bbr_pacer_queue_size(bbr_pacer) <= 100);
    bbr_pacer_destroy(bbr_pacer);

    razor_sender_t* remb = razor_sender_create(
        remb_congestion, 0, nullptr, nullptr, nullptr, nullptr, 100);
    CHECK_TRUE(remb != nullptr);
    CHECK_EQ(razor_sender_set_pacing_rate(remb, 1200000), -1);
    razor_sender_destroy(remb);

    razor_sender_t* sender = razor_sender_create(
        gcc_congestion, 0, nullptr, nullptr, nullptr, nullptr, 100);
    CHECK_TRUE(sender != nullptr);
    CHECK_EQ(razor_sender_set_pacing_rate(sender, 1200000), 0);
    CHECK_EQ(reinterpret_cast<sender_cc_t*>(sender)
                 ->pacer->pacing_bitrate_kpbs,
             static_cast<uint32_t>(1200));
    razor_sender_destroy(sender);

    CHECK_EQ(razor::detail::KbpsToBps(
                 std::numeric_limits<uint32_t>::max()),
             std::numeric_limits<uint32_t>::max());
    CHECK_EQ(razor::detail::QueueMsToInt(
                 std::numeric_limits<uint32_t>::max()),
             INT_MAX);
    return EXIT_SUCCESS;
}

int TestPacingCallbacksAreReentrantAndExceptionSafe() {
    BudgetReentryContext budget_context;
    budget_context.pacer =
        pace_create(&budget_context, BudgetReentryCallback, 100);
    CHECK_TRUE(budget_context.pacer != nullptr);
    pace_set_estimate_bitrate(budget_context.pacer, 80000);
    budget_context.pacer->last_update_ts = 1000;
    budget_context.now_ts = 1005;
    for (uint32_t packet_id = 0; packet_id < 10; ++packet_id) {
        CHECK_EQ(pace_insert_packet(
                     budget_context.pacer, packet_id, 0, 100, 0),
                 0);
    }
    pace_try_transmit(budget_context.pacer, budget_context.now_ts);
    CHECK_EQ(budget_context.calls, 1);
    CHECK_EQ(pace_queue_size(budget_context.pacer),
             static_cast<size_t>(900));
    pace_destroy(budget_context.pacer);

    ReentrantPacerContext c_context;
    c_context.sender = sender_cc_create(
        nullptr, nullptr, &c_context, ReentrantPacerCallback, 100);
    CHECK_TRUE(c_context.sender != nullptr);
    sender_cc_set_bitrates(c_context.sender, 200000, 1000000, 8000000);
    sender_cc_set_pacing_rate(c_context.sender, 1200000);
    CHECK_EQ(sender_cc_add_pace_packet(c_context.sender, 8, 0, 100), 0);
    const auto c_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (c_context.calls == 0 &&
           std::chrono::steady_clock::now() < c_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sender_cc_heartbeat(c_context.sender);
    }
    CHECK_EQ(c_context.calls, 1);
    CHECK_TRUE(c_context.queue_empty_during_callback);
    sender_cc_destroy(c_context.sender);

    std::function<void(uint32_t, uint8_t, uint32_t)> bitrate_cb;
    razor::Sender* reentrant_sender = nullptr;
    int reentrant_calls = 0;
    razor::Sender reentrant(
        bitrate_cb,
        [&reentrant_sender, &reentrant_calls](uint32_t, int, size_t, bool) {
            ++reentrant_calls;
            reentrant_sender->heartbeat();
        },
        100);
    reentrant_sender = &reentrant;
    reentrant.set_bitrates(200, 1000, 8000);
    reentrant.set_pacing_rate(1200);
    CHECK_TRUE(reentrant.add_packet(9, false, 100));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (reentrant_calls == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        reentrant.heartbeat();
    }
    CHECK_EQ(reentrant_calls, 1);
    CHECK_EQ(reentrant.pacer_queue_ms(), static_cast<int64_t>(0));

    int throwing_calls = 0;
    razor::Sender throwing(
        bitrate_cb,
        [&throwing_calls](uint32_t, int, size_t, bool) {
            ++throwing_calls;
            throw std::runtime_error("callback failure");
        },
        100);
    throwing.set_bitrates(200, 1000, 8000);
    throwing.set_pacing_rate(1200);
    CHECK_TRUE(throwing.add_packet(10, false, 100));

    const auto throwing_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (throwing_calls == 0 &&
           std::chrono::steady_clock::now() < throwing_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        throwing.heartbeat();
    }
    CHECK_EQ(throwing_calls, 1);
    CHECK_EQ(throwing.pacer_queue_ms(), static_cast<int64_t>(0));

    int bitrate_calls = 0;
    razor::Sender throwing_bitrate(
        [&bitrate_calls](uint32_t, uint8_t, uint32_t) {
            ++bitrate_calls;
            throw std::runtime_error("bitrate callback failure");
        });
    bool bitrate_exception_escaped = false;
    try {
        throwing_bitrate.set_bitrates(200, 1000, 8000);
    } catch (...) {
        bitrate_exception_escaped = true;
    }
    CHECK_TRUE(!bitrate_exception_escaped);
    CHECK_EQ(bitrate_calls, 1);
    CHECK_TRUE(throwing_bitrate.is_valid());
    throwing_bitrate.set_pacing_rate(1200);
    CHECK_TRUE(throwing_bitrate.add_packet(11, false, 100));
    (void)throwing_bitrate.stats();
    return EXIT_SUCCESS;
}

int TestStatsApiRejectsInvalidAndUnsupportedSendersSafely() {
    razor_sender_stats_t stats;
    std::memset(&stats, 0x7f, sizeof(stats));
    CHECK_EQ(razor_sender_get_stats(nullptr, &stats), -1);
    CHECK_EQ(stats.acked_bitrate_bps, static_cast<uint32_t>(0));
    CHECK_EQ(stats.delay_state, 0);
    CHECK_EQ(stats.matched_feedback_packets, static_cast<uint64_t>(0));
    CHECK_EQ(stats.pacer_queue_ms, static_cast<int64_t>(0));
    CHECK_EQ(razor_sender_get_stats(nullptr, nullptr), -1);

    razor_sender_t unsupported = {};
    unsupported.type = bbr_congestion;
    std::memset(&stats, 0x7f, sizeof(stats));
    CHECK_EQ(razor_sender_get_stats(&unsupported, &stats), -1);
    CHECK_EQ(stats.acked_bitrate_bps, static_cast<uint32_t>(0));
    CHECK_EQ(stats.delay_state, 0);
    CHECK_EQ(stats.matched_feedback_packets, static_cast<uint64_t>(0));
    CHECK_EQ(stats.pacer_queue_ms, static_cast<int64_t>(0));
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

int TestWrapperRestoresExplicitPacingAfterBitrateCallback() {
    std::vector<uint32_t> applied_pacing_bps;
    std::vector<char> events;
    razor::PacingRateApplier pacing_applier =
        [&applied_pacing_bps, &events](
            razor_sender_t* raw, uint32_t bitrate_bps) {
            events.push_back('P');
            applied_pacing_bps.push_back(bitrate_bps);
            return razor_sender_set_pacing_rate(raw, bitrate_bps);
        };
    int bitrate_callbacks = 0;
    razor::Sender sender(
        [&bitrate_callbacks, &events](uint32_t, uint8_t, uint32_t) {
            ++bitrate_callbacks;
            events.push_back('C');
        },
        razor::PaceSendCallback(), 250, pacing_applier);
    CHECK_TRUE(sender.is_valid());
    sender.set_bitrates(200, 1000, 8000);
    sender.set_pacing_rate(4000);
    applied_pacing_bps.clear();
    events.clear();
    const int callbacks_before = bitrate_callbacks;

    sender.set_bitrates(200, 1500, 8000);

    CHECK_EQ(bitrate_callbacks, callbacks_before + 1);
    CHECK_EQ(events.size(), static_cast<size_t>(3));
    CHECK_EQ(events[0], 'C');
    CHECK_EQ(events[1], 'P');
    CHECK_EQ(events[2], 'P');
    CHECK_EQ(applied_pacing_bps.size(), static_cast<size_t>(2));
    CHECK_EQ(applied_pacing_bps[0], static_cast<uint32_t>(4000000));
    CHECK_EQ(applied_pacing_bps[1], static_cast<uint32_t>(4000000));
    return EXIT_SUCCESS;
}

int TestThrowingPacingApplierCannotEscapeBitrateCallback() {
    int apply_calls = 0;
    bool throw_on_next_apply = false;
    razor::PacingRateApplier pacing_applier =
        [&apply_calls, &throw_on_next_apply](
            razor_sender_t* raw, uint32_t bitrate_bps) {
            ++apply_calls;
            if (throw_on_next_apply) {
                throw_on_next_apply = false;
                throw std::runtime_error("pacing applier failure");
            }
            return razor_sender_set_pacing_rate(raw, bitrate_bps);
        };
    int bitrate_callbacks = 0;
    razor::Sender sender(
        [&bitrate_callbacks](uint32_t, uint8_t, uint32_t) {
            ++bitrate_callbacks;
        },
        razor::PaceSendCallback(), 250, pacing_applier);
    CHECK_TRUE(sender.is_valid());
    sender.set_bitrates(200, 1000, 8000);
    sender.set_pacing_rate(4000);
    apply_calls = 0;
    throw_on_next_apply = true;
    bool escaped = false;
    try {
        sender.set_bitrates(200, 1500, 8000);
    } catch (...) {
        escaped = true;
    }
    CHECK_TRUE(!escaped);
    CHECK_EQ(apply_calls, 2);
    CHECK_TRUE(bitrate_callbacks >= 2);
    sender.set_pacing_rate(1300);
    CHECK_TRUE(sender.add_packet(99, false, 100));
    (void)sender.stats();
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
    CHECK_EQ(TestPacerConfigurationAndCppStats(), EXIT_SUCCESS);
    CHECK_EQ(TestCheckedPacingRateApiAndOverflowConversion(), EXIT_SUCCESS);
    CHECK_EQ(TestPacingCallbacksAreReentrantAndExceptionSafe(), EXIT_SUCCESS);
    CHECK_EQ(TestStatsApiRejectsInvalidAndUnsupportedSendersSafely(),
             EXIT_SUCCESS);
    CHECK_EQ(TestSenderProcessesFeedbackOnce(argv[1]), EXIT_SUCCESS);
    CHECK_EQ(TestWrapperRestoresExplicitPacingAfterBitrateCallback(),
             EXIT_SUCCESS);
    CHECK_EQ(TestThrowingPacingApplierCannotEscapeBitrateCallback(),
             EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
