#include "rate_policy.h"
#include "test_support.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

using gcc::RateDecision;
using gcc::RateInput;
using gcc::RatePolicy;
using gcc::RateReason;

RateDecision Apply(
    RatePolicy& policy, RateReason reason, uint32_t proposal,
    uint32_t rtt_ms, uint64_t now_ms) {
    return policy.Apply({reason, proposal, rtt_ms, now_ms});
}

int TestCoreSequence() {
    RatePolicy policy(100, 10000);
    policy.Reset(4000, 0);

    RateDecision decision =
        Apply(policy, RateReason::DelayOveruse, 2500, 120, 1000);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(2500));

    decision = Apply(policy, RateReason::Loss, 500, 120, 1100);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1250));

    decision = Apply(policy, RateReason::Normal, 3000, 120, 1150);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1250));

    decision = Apply(policy, RateReason::ProbeAccepted, 3000, 120, 1200);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(3000));
    CHECK_EQ(decision.encoder_kbps, static_cast<uint32_t>(2760));
    return EXIT_SUCCESS;
}

int TestBoundsAndNormalizedRange() {
    RatePolicy policy(5000, 1000);
    policy.Reset(100, 0);
    RateDecision decision =
        Apply(policy, RateReason::ProbeAccepted, 9000, 0, 1);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(5000));

    policy.Reset(9000, 2);
    decision = Apply(policy, RateReason::Loss, 0, 0, 3);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(2500));

    policy.Reset(1000, 4);
    decision = Apply(policy, RateReason::DelayOveruse, 0, 0, 5);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));
    return EXIT_SUCCESS;
}

int TestNormalGrowthLimit() {
    RatePolicy policy(100, 10000);
    policy.Reset(1000, 1000);

    RateDecision decision =
        Apply(policy, RateReason::Normal, 5000, 0, 2000);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1080));

    decision = Apply(policy, RateReason::Normal, 5000, 0, 2500);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1122));

    decision = Apply(policy, RateReason::Normal, 1122, 0, 2501);
    CHECK_TRUE(!decision.changed);
    return EXIT_SUCCESS;
}

int TestSameTimestampCannotCreateGrowth() {
    RatePolicy policy(100, 10000);
    policy.Reset(1000, 1000);
    RateDecision decision{false, 0, 0};
    for (int i = 0; i < 100; ++i) {
        decision =
            Apply(policy, RateReason::Normal, 5000, 0, 1000);
    }
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));
    return EXIT_SUCCESS;
}

int TestMillisecondCallsAccumulateTimeNotCallCount() {
    RatePolicy policy(100, 10000);
    policy.Reset(1000, 0);
    RateDecision decision{false, 0, 0};
    for (uint64_t now_ms = 1; now_ms <= 1000; ++now_ms) {
        decision =
            Apply(policy, RateReason::Normal, 5000, 0, now_ms);
    }
    CHECK_TRUE(decision.network_kbps >= static_cast<uint32_t>(1079));
    CHECK_TRUE(decision.network_kbps <= static_cast<uint32_t>(1080));
    return EXIT_SUCCESS;
}

int TestProposalLimitDoesNotStoreHiddenGrowth() {
    RatePolicy policy(100, 10000);
    policy.Reset(1000, 0);
    RateDecision decision =
        Apply(policy, RateReason::Normal, 1050, 0, 1000);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1050));

    decision = Apply(policy, RateReason::Normal, 5000, 0, 1000);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1050));
    return EXIT_SUCCESS;
}

int TestRecoveryWaitsForRtt() {
    RatePolicy policy(100, 10000);
    policy.Reset(2000, 0);
    CHECK_EQ(Apply(policy, RateReason::Loss, 1000, 0, 1000).network_kbps,
             static_cast<uint32_t>(1000));

    RateDecision decision =
        Apply(policy, RateReason::Normal, 3000, 120, 1119);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));

    decision = Apply(policy, RateReason::Normal, 3000, 120, 1120);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));

    decision = Apply(policy, RateReason::Normal, 3000, 120, 1133);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1001));
    return EXIT_SUCCESS;
}

int TestCongestionCanDecreaseRepeatedlyButNeverIncrease() {
    RatePolicy policy(100, 10000);
    policy.Reset(4000, 0);
    CHECK_EQ(Apply(policy, RateReason::Loss, 0, 0, 1).network_kbps,
             static_cast<uint32_t>(2000));
    CHECK_EQ(Apply(policy, RateReason::DelayOveruse, 0, 0, 2).network_kbps,
             static_cast<uint32_t>(1000));

    RateDecision decision =
        Apply(policy, RateReason::Loss, 5000, 0, 3);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));
    return EXIT_SUCCESS;
}

int TestProbeOnlyIncreases() {
    RatePolicy policy(100, 10000);
    policy.Reset(2000, 0);
    RateDecision decision =
        Apply(policy, RateReason::ProbeAccepted, 1000, 0, 1);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(2000));

    decision = Apply(policy, RateReason::ProbeAccepted, 3000, 0, 2);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(3000));
    return EXIT_SUCCESS;
}

int TestBackwardTimeHoldsNormalGrowth() {
    RatePolicy policy(100, 10000);
    policy.Reset(1000, 1000);
    RateDecision decision =
        Apply(policy, RateReason::Normal, 5000, 0, 900);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));

    decision = Apply(policy, RateReason::Normal, 5000, 0, 2000);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1080));
    return EXIT_SUCCESS;
}

int TestBackwardTimeOnlyFreezesNormal() {
    RatePolicy loss(100, 10000);
    loss.Reset(4000, 1000);
    RateDecision decision =
        Apply(loss, RateReason::Loss, 500, 120, 900);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(2000));
    decision = Apply(loss, RateReason::Normal, 5000, 120, 950);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(2000));

    RatePolicy delay(100, 10000);
    delay.Reset(3001, 1000);
    decision =
        Apply(delay, RateReason::DelayOveruse, 100, 0, 900);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1501));

    RatePolicy probe(100, 10000);
    probe.Reset(1000, 1000);
    decision =
        Apply(probe, RateReason::ProbeAccepted, 5000, 0, 900);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(5000));

    RatePolicy normal(100, 10000);
    normal.Reset(1000, 1000);
    decision = Apply(normal, RateReason::Normal, 5000, 0, 900);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));
    return EXIT_SUCCESS;
}

int TestGrowthHandlesIntegerAndElapsedExtremes() {
    RatePolicy near_max(0, UINT32_MAX);
    near_max.Reset(UINT32_MAX - 1, 0);
    RateDecision decision =
        Apply(near_max, RateReason::Normal, UINT32_MAX, 0, 1);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, UINT32_MAX);

    RatePolicy huge_elapsed(1, UINT32_MAX);
    huge_elapsed.Reset(1000, 0);
    decision = Apply(
        huge_elapsed, RateReason::Normal, UINT32_MAX, 0, UINT64_MAX);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, UINT32_MAX);
    return EXIT_SUCCESS;
}

int TestZeroGrowthBaselineNeedsExplicitIncrease() {
    RatePolicy zero(0, UINT32_MAX);
    zero.Reset(0, 0);
    RateDecision decision = Apply(
        zero, RateReason::Normal, UINT32_MAX, 0, 10000000);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(0));

    RatePolicy extreme(0, UINT32_MAX);
    extreme.Reset(0, 0);
    decision = Apply(
        extreme, RateReason::Normal, UINT32_MAX, 0, UINT64_MAX);
    CHECK_TRUE(!decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(0));

    RatePolicy probe(0, UINT32_MAX);
    probe.Reset(0, 0);
    decision =
        Apply(probe, RateReason::ProbeAccepted, 1000, 0, 100);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));
    decision = Apply(probe, RateReason::Normal, 5000, 0, 1100);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1080));
    return EXIT_SUCCESS;
}

int TestUtilizationValidationAndRounding() {
    RatePolicy invalid(0, 10000, std::numeric_limits<double>::quiet_NaN());
    invalid.Reset(1001, 0);
    CHECK_EQ(Apply(invalid, RateReason::Normal, 1001, 0, 0).encoder_kbps,
             static_cast<uint32_t>(921));

    RatePolicy infinity(
        0, 10000, std::numeric_limits<double>::infinity());
    infinity.Reset(1000, 0);
    CHECK_EQ(Apply(infinity, RateReason::Normal, 1000, 0, 0).encoder_kbps,
             static_cast<uint32_t>(920));

    RatePolicy zero(0, 10000, 0.0);
    zero.Reset(1000, 0);
    CHECK_EQ(Apply(zero, RateReason::Normal, 1000, 0, 0).encoder_kbps,
             static_cast<uint32_t>(920));

    RatePolicy capped(0, UINT32_MAX, 4.0);
    capped.Reset(UINT32_MAX, 0);
    CHECK_EQ(Apply(capped, RateReason::Normal, UINT32_MAX, 0, 0).encoder_kbps,
             UINT32_MAX);
    return EXIT_SUCCESS;
}

int TestEncoderRateMayBeBelowNetworkMinimum() {
    RatePolicy policy(1000, 5000, 0.92);
    policy.Reset(1000, 0);
    const RateDecision decision =
        Apply(policy, RateReason::Normal, 1000, 0, 0);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(1000));
    CHECK_EQ(decision.encoder_kbps, static_cast<uint32_t>(920));
    return EXIT_SUCCESS;
}

int TestResetClearsRecoveryStateAndChangedIsExact() {
    RatePolicy policy(100, 10000);
    policy.Reset(2000, 0);
    CHECK_TRUE(Apply(policy, RateReason::Loss, 1000, 0, 100).changed);
    policy.Reset(2000, 1000);

    RateDecision decision =
        Apply(policy, RateReason::Normal, 5000, 5000, 2000);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.network_kbps, static_cast<uint32_t>(2160));

    decision =
        Apply(policy, RateReason::Normal, decision.network_kbps, 0, 2001);
    CHECK_TRUE(!decision.changed);
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    CHECK_EQ(TestCoreSequence(), EXIT_SUCCESS);
    CHECK_EQ(TestBoundsAndNormalizedRange(), EXIT_SUCCESS);
    CHECK_EQ(TestNormalGrowthLimit(), EXIT_SUCCESS);
    CHECK_EQ(TestSameTimestampCannotCreateGrowth(), EXIT_SUCCESS);
    CHECK_EQ(TestMillisecondCallsAccumulateTimeNotCallCount(), EXIT_SUCCESS);
    CHECK_EQ(TestProposalLimitDoesNotStoreHiddenGrowth(), EXIT_SUCCESS);
    CHECK_EQ(TestRecoveryWaitsForRtt(), EXIT_SUCCESS);
    CHECK_EQ(TestCongestionCanDecreaseRepeatedlyButNeverIncrease(),
             EXIT_SUCCESS);
    CHECK_EQ(TestProbeOnlyIncreases(), EXIT_SUCCESS);
    CHECK_EQ(TestBackwardTimeHoldsNormalGrowth(), EXIT_SUCCESS);
    CHECK_EQ(TestBackwardTimeOnlyFreezesNormal(), EXIT_SUCCESS);
    CHECK_EQ(TestGrowthHandlesIntegerAndElapsedExtremes(), EXIT_SUCCESS);
    CHECK_EQ(TestZeroGrowthBaselineNeedsExplicitIncrease(), EXIT_SUCCESS);
    CHECK_EQ(TestUtilizationValidationAndRounding(), EXIT_SUCCESS);
    CHECK_EQ(TestEncoderRateMayBeBelowNetworkMinimum(), EXIT_SUCCESS);
    CHECK_EQ(TestResetClearsRecoveryStateAndChangedIsExact(), EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
