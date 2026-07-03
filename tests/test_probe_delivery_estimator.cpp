#include "probe_delivery_estimator.h"
#include "test_support.h"

#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

gcc::SentPacketInfo Sent(uint16_t sequence, uint64_t bytes,
                         uint64_t send_us, bool probe = false) {
    gcc::SentPacketInfo info;
    info.transport_sequence = sequence;
    info.size = 1000U;
    info.send_time_us = send_us;
    info.cumulative_sent_bytes = bytes;
    info.generation = sequence;
    info.probe = probe;
    return info;
}

}  // namespace

int main() {
    gcc::ProbeDeliveryEstimator estimator;
    estimator.ObserveFeedback(Sent(1, 1000, 100000), 1000U);
    estimator.Start(2000U);
    CHECK_TRUE(estimator.rate_events_guarded());
    estimator.OnProbePacketSent(Sent(2, 2000, 110000, true));
    CHECK_TRUE(estimator.BeginFeedback(Sent(2, 2000, 110000, true)));

    estimator.ObserveFeedback(Sent(3, 9000, 119999), 1020U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(0));
    estimator.ObserveFeedback(Sent(4, 9000, 120000), 1040U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(1600));

    estimator.ObserveFeedback(Sent(4, 9000, 120000), 1040U);
    estimator.ObserveFeedback(Sent(3, 8000, 119000), 1039U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(1600));
    estimator.Finish();
    CHECK_TRUE(estimator.rate_events_guarded());
    CHECK_TRUE(estimator.BeginFeedback(Sent(2, 2000, 110000, true)));
    CHECK_TRUE(estimator.BeginFeedback(Sent(4, 9000, 120000)));
    CHECK_TRUE(!estimator.rate_events_guarded());

    estimator.ObserveFeedback(
        Sent(5, 10000, 200000), UINT32_MAX - 9U);
    estimator.Start(3000U);
    estimator.OnProbePacketSent(Sent(6, 11000, 205000, true));
    estimator.ObserveFeedback(Sent(7, 20000, 220000), 30U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(2000));

    estimator.Start(4000U);
    CHECK_EQ(estimator.estimate_kbps(), static_cast<uint32_t>(0));
    estimator.OnProbePacketSent(Sent(8, 21000, 225000, true));
    estimator.ObserveFeedback(Sent(9, 31000, 245000), 50U);
    CHECK_TRUE(estimator.estimate_kbps() > 0U);

    gcc::ProbeDeliveryEstimator extreme;
    extreme.ObserveFeedback(
        Sent(10, 1, 1), std::numeric_limits<uint32_t>::max() - 10U);
    extreme.Start(std::numeric_limits<uint32_t>::max());
    extreme.OnProbePacketSent(Sent(11, 2, 2, true));
    gcc::SentPacketInfo huge = Sent(12, UINT64_MAX, 20001);
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
