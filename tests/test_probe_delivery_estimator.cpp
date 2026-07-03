#include "probe_delivery_estimator.h"
#include "test_support.h"

#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

gcc::SentPacketInfo Sent(uint16_t sequence, uint64_t bytes,
                         uint64_t send_us, bool probe = false,
                         uint32_t history_ordinal = 0U) {
    gcc::SentPacketInfo info;
    info.transport_sequence = sequence;
    info.size = 1000U;
    info.send_time_us = send_us;
    info.cumulative_sent_bytes = bytes;
    info.generation = sequence;
    info.probe = probe;
    info.history_ordinal = history_ordinal;
    return info;
}

}  // namespace

int main() {
    gcc::ProbeDeliveryEstimator estimator;
    estimator.ObserveFeedback(Sent(1, 1000, 100000), 1000U);
    estimator.Start(2000U);
    CHECK_TRUE(estimator.rate_events_guarded());
    estimator.ObserveFeedback(Sent(2, 2000, 105000), 1010U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(0));
    estimator.OnProbePacketSent(Sent(2, 2000, 110000, true));
    CHECK_TRUE(estimator.BeginFeedback(Sent(2, 2000, 110000, true)));

    estimator.ObserveFeedback(Sent(2, 2000, 110000, true), 1020U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(0));
    estimator.ObserveFeedback(Sent(3, 9000, 129999), 1040U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(0));
    estimator.ObserveFeedback(Sent(4, 10000, 130000), 1060U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(1600));

    estimator.ObserveFeedback(Sent(4, 10000, 130000), 1060U);
    estimator.ObserveFeedback(Sent(3, 8000, 129000), 1059U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(1600));
    estimator.Finish();
    CHECK_TRUE(estimator.rate_events_guarded());
    CHECK_TRUE(estimator.BeginFeedback(Sent(2, 2000, 110000, true)));
    CHECK_TRUE(!estimator.BeginFeedback(Sent(4, 10000, 130000)));
    CHECK_TRUE(!estimator.rate_events_guarded());

    estimator.ObserveFeedback(
        Sent(5, 10000, 200000), UINT32_MAX - 9U);
    estimator.Start(3000U);
    estimator.OnProbePacketSent(Sent(6, 11000, 205000, true));
    estimator.ObserveFeedback(
        Sent(6, 11000, 205000, true), UINT32_MAX - 4U);
    estimator.ObserveFeedback(Sent(7, 20000, 225000), 15U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(3600));

    estimator.Start(4000U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(0));
    estimator.OnProbePacketSent(Sent(8, 21000, 225000, true));
    estimator.ObserveFeedback(Sent(7, 20000, 225000), 15U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(0));
    estimator.ObserveFeedback(Sent(8, 21000, 225000, true), 30U);
    estimator.ObserveFeedback(Sent(9, 31000, 245000), 50U);
    CHECK_TRUE(estimator.estimate_kbps() > 0U);

    gcc::ProbeDeliveryEstimator skipped_boundary_feedback;
    skipped_boundary_feedback.Start(2000U);
    skipped_boundary_feedback.OnProbePacketSent(
        Sent(20, 100000, 100000, true));
    skipped_boundary_feedback.ObserveFeedback(
        Sent(21, 104000, 120000), 1020U);
    CHECK_EQ(skipped_boundary_feedback.estimate_kbps(),
             static_cast<uint32_t>(0));
    skipped_boundary_feedback.ObserveFeedback(
        Sent(22, 108000, 140000), 1040U);
    CHECK_EQ(skipped_boundary_feedback.estimate_kbps(),
             static_cast<uint32_t>(1600));

    gcc::ProbeDeliveryEstimator missing_feedback;
    missing_feedback.Start(2000U);
    gcc::SentPacketInfo missing_first =
        Sent(100, 100000, 100000, true);
    missing_first.generation = 100U;
    missing_feedback.OnProbePacketSent(missing_first);
    gcc::SentPacketInfo missing_last =
        Sent(101, 101000, 101000, true);
    missing_last.generation = 101U;
    missing_feedback.OnProbePacketSent(missing_last);
    missing_feedback.Finish();
    gcc::SentHistoryWatermark retained;
    retained.valid = true;
    retained.oldest_generation = 100U;
    retained.oldest_cumulative_sent_bytes = 100000U;
    retained.oldest_history_ordinal = 100U;
    retained.newest_generation = 150U;
    retained.cumulative_sent_bytes = 150000U;
    retained.newest_history_ordinal = 150U;
    CHECK_TRUE(missing_feedback.BeginUnmatchedFeedback(retained));
    CHECK_TRUE(missing_feedback.rate_events_guarded());
    gcc::SentHistoryWatermark evicted;
    evicted.valid = true;
    evicted.oldest_generation = 102U;
    evicted.oldest_cumulative_sent_bytes = 102000U;
    evicted.oldest_history_ordinal = 102U;
    evicted.newest_generation = 2102U;
    evicted.cumulative_sent_bytes = 2102000U;
    evicted.newest_history_ordinal = 2102U;
    CHECK_TRUE(missing_feedback.BeginUnmatchedFeedback(evicted));
    CHECK_TRUE(!missing_feedback.rate_events_guarded());
    CHECK_TRUE(!missing_feedback.BeginFeedback(missing_last));
    gcc::SentPacketInfo interleaved_normal =
        Sent(100, 100500, 100500, false);
    interleaved_normal.generation = 100U;
    CHECK_TRUE(!missing_feedback.BeginFeedback(interleaved_normal));
    gcc::SentPacketInfo definitely_new =
        Sent(102, 102000, 102000, false);
    definitely_new.generation = 102U;
    CHECK_TRUE(!missing_feedback.BeginFeedback(definitely_new));

    gcc::ProbeDeliveryEstimator reordered;
    reordered.Start(2000U);
    gcc::SentPacketInfo reordered_first =
        Sent(200, 200000, 200000, true);
    reordered_first.generation = 200U;
    reordered.OnProbePacketSent(reordered_first);
    gcc::SentPacketInfo reordered_last =
        Sent(201, 201000, 201000, true);
    reordered_last.generation = 201U;
    reordered.OnProbePacketSent(reordered_last);
    reordered.Finish();
    gcc::SentPacketInfo post_probe =
        Sent(202, 202000, 202000, false);
    post_probe.generation = 202U;
    CHECK_TRUE(!reordered.BeginFeedback(post_probe));
    CHECK_TRUE(!reordered.rate_events_guarded());
    CHECK_TRUE(reordered.BeginFeedback(reordered_last));
    gcc::SentPacketInfo later_normal =
        Sent(203, 203000, 203000, false);
    later_normal.generation = 203U;
    CHECK_TRUE(!reordered.BeginFeedback(later_normal));

    gcc::ProbeDeliveryEstimator no_feedback;
    no_feedback.Start(2000U);
    gcc::SentPacketInfo no_feedback_probe =
        Sent(250, 250000, 250000, true, 250U);
    no_feedback_probe.generation = 250U;
    no_feedback.OnProbePacketSent(no_feedback_probe);
    no_feedback.Finish();
    CHECK_TRUE(no_feedback.rate_events_guarded());
    gcc::SentHistoryWatermark no_feedback_progress;
    no_feedback_progress.valid = true;
    no_feedback_progress.oldest_generation = 251U;
    no_feedback_progress.oldest_cumulative_sent_bytes = 251000U;
    no_feedback_progress.oldest_history_ordinal = 251U;
    no_feedback_progress.newest_generation = 2250U;
    no_feedback_progress.cumulative_sent_bytes = 2250000U;
    no_feedback_progress.newest_history_ordinal = 2250U;
    no_feedback.AdvanceSendProgress(no_feedback_progress);
    CHECK_TRUE(!no_feedback.rate_events_guarded());

    gcc::ProbeDeliveryEstimator consecutive_tiers;
    consecutive_tiers.Start(2000U);
    gcc::SentPacketInfo first_tier =
        Sent(10, 10000, 10000, true, 10U);
    first_tier.generation = 10U;
    consecutive_tiers.OnProbePacketSent(first_tier);
    consecutive_tiers.Finish();
    consecutive_tiers.Start(3000U);
    gcc::SentPacketInfo second_tier =
        Sent(20, 20000, 20000, true, 20U);
    second_tier.generation = 20U;
    consecutive_tiers.OnProbePacketSent(second_tier);
    CHECK_TRUE(consecutive_tiers.BeginFeedback(first_tier));
    gcc::SentPacketInfo active_tier_media =
        Sent(15, 15000, 15000, false, 15U);
    active_tier_media.generation = 15U;
    CHECK_TRUE(!consecutive_tiers.BeginFeedback(active_tier_media));
    consecutive_tiers.Finish();
    gcc::SentPacketInfo after_tiers =
        Sent(21, 21000, 21000, false, 21U);
    after_tiers.generation = 21U;
    CHECK_TRUE(!consecutive_tiers.BeginFeedback(after_tiers));
    CHECK_TRUE(consecutive_tiers.BeginFeedback(first_tier));
    CHECK_TRUE(consecutive_tiers.BeginFeedback(second_tier));
    gcc::SentPacketInfo tier_media =
        Sent(15, 15000, 15000, false, 15U);
    tier_media.generation = 15U;
    CHECK_TRUE(!consecutive_tiers.BeginFeedback(tier_media));
    CHECK_TRUE(!consecutive_tiers.BeginFeedback(after_tiers));

    gcc::ProbeDeliveryEstimator bounded_ranges;
    gcc::SentPacketInfo oldest_bounded;
    gcc::SentPacketInfo newest_bounded;
    for (uint32_t generation = 1U; generation <= 2001U; ++generation) {
        bounded_ranges.Start(2000U);
        gcc::SentPacketInfo packet = Sent(
            static_cast<uint16_t>(generation),
            static_cast<uint64_t>(generation) * 1000U,
            static_cast<uint64_t>(generation) * 1000U,
            true, generation);
        packet.generation = generation;
        bounded_ranges.OnProbePacketSent(packet);
        bounded_ranges.Finish();
        if (generation == 1U) oldest_bounded = packet;
        newest_bounded = packet;
    }
    gcc::SentPacketInfo bounded_crossing =
        Sent(2002, 2002000U, 2002000U, false, 2002U);
    bounded_crossing.generation = 2002U;
    CHECK_TRUE(!bounded_ranges.BeginFeedback(bounded_crossing));
    CHECK_TRUE(!bounded_ranges.BeginFeedback(oldest_bounded));
    CHECK_TRUE(bounded_ranges.BeginFeedback(newest_bounded));

    gcc::ProbeDeliveryEstimator saturated;
    saturated.Start(2000U);
    gcc::SentPacketInfo saturated_probe =
        Sent(300, UINT64_MAX, 300000, true, 100U);
    saturated_probe.generation = 300U;
    saturated.OnProbePacketSent(saturated_probe);
    saturated.Finish();
    gcc::SentPacketInfo saturated_crossing =
        Sent(301, UINT64_MAX, 301000, false, 101U);
    saturated_crossing.generation = 301U;
    CHECK_TRUE(!saturated.BeginFeedback(saturated_crossing));
    CHECK_TRUE(!saturated.rate_events_guarded());

    gcc::ProbeDeliveryEstimator doubly_saturated;
    doubly_saturated.Start(2000U);
    gcc::SentPacketInfo saturated_range =
        Sent(400, UINT64_MAX, 400000, true, UINT32_MAX);
    saturated_range.generation = UINT64_MAX;
    doubly_saturated.OnProbePacketSent(saturated_range);
    doubly_saturated.Finish();
    gcc::SentHistoryWatermark saturated_watermark;
    saturated_watermark.valid = true;
    saturated_watermark.oldest_generation = UINT64_MAX;
    saturated_watermark.oldest_cumulative_sent_bytes = UINT64_MAX;
    saturated_watermark.oldest_history_ordinal = 0U;
    saturated_watermark.newest_generation = UINT64_MAX;
    saturated_watermark.cumulative_sent_bytes = UINT64_MAX;
    saturated_watermark.newest_history_ordinal = 1U;
    doubly_saturated.AdvanceSendProgress(saturated_watermark);
    CHECK_TRUE(!doubly_saturated.rate_events_guarded());

    gcc::ProbeDeliveryEstimator extreme;
    extreme.ObserveFeedback(
        Sent(10, 1, 1), std::numeric_limits<uint32_t>::max() - 10U);
    extreme.Start(std::numeric_limits<uint32_t>::max());
    extreme.OnProbePacketSent(Sent(11, 2, 2, true));
    extreme.ObserveFeedback(Sent(11, 2, 2, true), UINT32_MAX - 10U);
    gcc::SentPacketInfo huge = Sent(12, UINT64_MAX, 20002);
    huge.generation = 12;
    extreme.ObserveFeedback(huge, 10U);
    CHECK_EQ(extreme.estimate_kbps(),
             std::numeric_limits<uint32_t>::max());

    gcc::ProbeDeliveryEstimator empty;
    empty.Start(1000U);
    empty.Finish();
    CHECK_TRUE(!empty.rate_events_guarded());
    return EXIT_SUCCESS;
}
