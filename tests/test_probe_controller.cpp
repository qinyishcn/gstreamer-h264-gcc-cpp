#include "probe_controller.h"
#include "test_support.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using gcc::ProbeAction;
using gcc::ProbeConfig;
using gcc::ProbeController;
using gcc::ProbeFeedback;
using gcc::ProbeState;

int TestPublicTypesUseGccNamespace() {
    gcc::ProbeController controller;
    CHECK_EQ(controller.state(), gcc::ProbeState::Idle);
    return EXIT_SUCCESS;
}

ProbeFeedback Feedback(uint32_t acked_kbps,
                       double loss_fraction,
                       bool overusing,
                       uint64_t now_ms) {
    return ProbeFeedback{acked_kbps, loss_fraction, overusing, now_ms};
}

int TestSuccessfulProbeSequence() {
    ProbeController controller;

    const ProbeAction first = controller.OnStableTarget(1000U, 0U);
    CHECK_TRUE(first.start);
    CHECK_EQ(first.probe_kbps, 2000U);
    CHECK_EQ(controller.state(), ProbeState::Probing);
    CHECK_EQ(controller.probe_start_ms(), 0U);

    controller.OnProbePacketSent(1200U, 20U);
    CHECK_EQ(controller.probe_bytes_sent(), 1200U);

    const ProbeAction accepted =
        controller.OnFeedback(Feedback(1700U, 0.0, false, 120U));
    CHECK_TRUE(accepted.stop);
    CHECK_TRUE(accepted.accept);
    CHECK_EQ(accepted.accepted_kbps, 1700U);
    CHECK_EQ(controller.state(), ProbeState::Accepted);

    const ProbeAction next = controller.OnStableTarget(1700U, 121U);
    CHECK_TRUE(next.start);
    CHECK_EQ(next.probe_kbps, 2550U);
    return EXIT_SUCCESS;
}

int TestLowAckFailsOnlyAfterProbePeriod() {
    ProbeController controller;
    controller.OnStableTarget(1000U, 0U);

    const ProbeAction early =
        controller.OnFeedback(Feedback(1499U, 0.0, false, 50U));
    CHECK_EQ(controller.state(), ProbeState::Probing);
    CHECK_TRUE(!early.stop);

    const ProbeAction stop = controller.OnTimer(100U);
    CHECK_TRUE(stop.stop);
    CHECK_EQ(controller.state(), ProbeState::WaitingForResult);

    const ProbeAction failed =
        controller.OnFeedback(Feedback(1499U, 0.0, false, 101U));
    CHECK_EQ(controller.state(), ProbeState::Failed);
    CHECK_TRUE(!failed.accept);
    return EXIT_SUCCESS;
}

int TestAckIsEvaluatedOnlyAfterProbePeriod() {
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction early =
            controller.OnFeedback(Feedback(2000U, 0.0, false, 99U));
        CHECK_TRUE(!early.stop);
        CHECK_TRUE(!early.accept);
        CHECK_EQ(controller.state(), ProbeState::Probing);

        const ProbeAction threshold =
            controller.OnFeedback(Feedback(1500U, 0.0, false, 100U));
        CHECK_TRUE(threshold.stop);
        CHECK_TRUE(threshold.accept);
        CHECK_EQ(threshold.accepted_kbps, 1500U);
        CHECK_EQ(controller.state(), ProbeState::Accepted);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction early =
            controller.OnFeedback(Feedback(1499U, 0.0, false, 99U));
        CHECK_TRUE(!early.stop);
        CHECK_TRUE(!early.accept);
        CHECK_EQ(controller.state(), ProbeState::Probing);

        const ProbeAction below_threshold =
            controller.OnFeedback(Feedback(1499U, 0.0, false, 100U));
        CHECK_TRUE(below_threshold.stop);
        CHECK_TRUE(!below_threshold.accept);
        CHECK_EQ(controller.state(), ProbeState::Failed);
    }
    return EXIT_SUCCESS;
}

int TestBadFeedbackAbortsImmediately() {
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction action =
            controller.OnFeedback(Feedback(2000U, 0.100001, false, 1U));
        CHECK_TRUE(action.stop);
        CHECK_EQ(controller.state(), ProbeState::Failed);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction action =
            controller.OnFeedback(Feedback(2000U, 0.10, false, 1U));
        CHECK_TRUE(!action.stop);
        CHECK_TRUE(!action.accept);
        CHECK_EQ(controller.state(), ProbeState::Probing);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction action =
            controller.OnFeedback(Feedback(2000U, 0.0, true, 1U));
        CHECK_TRUE(action.stop);
        CHECK_EQ(controller.state(), ProbeState::Failed);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction action = controller.OnFeedback(
            Feedback(2000U, std::numeric_limits<double>::quiet_NaN(), false, 1U));
        CHECK_TRUE(action.stop);
        CHECK_EQ(controller.state(), ProbeState::Failed);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction action =
            controller.OnFeedback(Feedback(2000U, -1.0, false, 1U));
        CHECK_TRUE(!action.accept);
        CHECK_EQ(controller.state(), ProbeState::Probing);
    }
    return EXIT_SUCCESS;
}

int TestRetryBoundaryAndNoOverlap() {
    ProbeController controller;
    controller.OnStableTarget(1000U, 100U);
    controller.OnFeedback(Feedback(0U, 1.0, false, 200U));

    CHECK_TRUE(!controller.OnStableTarget(1000U, 5199U).start);
    CHECK_TRUE(controller.OnStableTarget(1000U, 5200U).start);
    CHECK_TRUE(!controller.OnStableTarget(2000U, 5201U).start);
    CHECK_EQ(controller.active_probe_kbps(), 2000U);
    return EXIT_SUCCESS;
}

int TestMaxClampAndMeaninglessProbe() {
    ProbeConfig config;
    config.max_kbps = 3000U;
    ProbeController controller(config);
    const ProbeAction clamped = controller.OnStableTarget(2000U, 0U);
    CHECK_TRUE(clamped.start);
    CHECK_EQ(clamped.probe_kbps, 3000U);

    ProbeController at_max(config);
    CHECK_TRUE(!at_max.OnStableTarget(3000U, 0U).start);

    ProbeConfig rounded_config;
    rounded_config.first_scale = 1.0000000001;
    ProbeController rounded(rounded_config);
    CHECK_TRUE(!rounded.OnStableTarget(1U, 0U).start);
    return EXIT_SUCCESS;
}

int TestTimerStopAndTimeout() {
    ProbeController controller;
    controller.OnStableTarget(1000U, 10U);
    CHECK_TRUE(!controller.OnTimer(109U).stop);
    const ProbeAction stop = controller.OnTimer(110U);
    CHECK_TRUE(stop.stop);
    CHECK_EQ(controller.state(), ProbeState::WaitingForResult);
    CHECK_EQ(controller.state(), ProbeState::WaitingForResult);
    controller.OnTimer(309U);
    CHECK_EQ(controller.state(), ProbeState::WaitingForResult);
    controller.OnTimer(310U);
    CHECK_EQ(controller.state(), ProbeState::Failed);
    return EXIT_SUCCESS;
}

int TestInvalidLongDurationIsNormalizedAndDeadlineStillApplies() {
    ProbeConfig config;
    config.duration_ms = 1000U;

    {
        ProbeController controller(config);
        controller.OnStableTarget(1000U, 0U);
        CHECK_TRUE(!controller.OnTimer(99U).stop);
        const ProbeAction normalized_stop = controller.OnTimer(100U);
        CHECK_TRUE(normalized_stop.stop);
        CHECK_EQ(controller.state(), ProbeState::WaitingForResult);
        controller.OnTimer(300U);
        CHECK_EQ(controller.state(), ProbeState::Failed);
    }
    {
        ProbeController controller(config);
        controller.OnStableTarget(1000U, 0U);
        const ProbeAction deadline =
            controller.OnFeedback(Feedback(2000U, 0.0, false, 300U));
        CHECK_TRUE(deadline.stop);
        CHECK_TRUE(!deadline.accept);
        CHECK_EQ(controller.state(), ProbeState::Failed);
    }
    return EXIT_SUCCESS;
}

int TestIgnoredEventsDoNotAdvanceTime() {
    {
        ProbeController controller;
        controller.OnFeedback(Feedback(0U, 0.0, false, 100000U));
        controller.OnProbePacketSent(10U, 100000U);
        controller.OnTimer(100000U);
        const ProbeAction start = controller.OnStableTarget(1000U, 0U);
        CHECK_TRUE(start.start);
        CHECK_EQ(controller.probe_start_ms(), 0U);
        const ProbeAction deadline = controller.OnTimer(300U);
        CHECK_TRUE(deadline.stop);
        CHECK_EQ(controller.state(), ProbeState::Failed);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        CHECK_TRUE(!controller.OnStableTarget(2000U, 100000U).start);
        CHECK_TRUE(!controller.OnTimer(1U).stop);
        CHECK_EQ(controller.state(), ProbeState::Probing);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 0U);
        controller.OnTimer(100U);
        controller.OnProbePacketSent(10U, 100000U);
        const ProbeAction accepted =
            controller.OnFeedback(Feedback(1500U, 0.0, false, 101U));
        CHECK_TRUE(accepted.accept);
        CHECK_EQ(controller.state(), ProbeState::Accepted);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 100U);
        controller.OnFeedback(Feedback(1500U, 0.0, false, 200U));
        controller.OnProbePacketSent(10U, 100000U);
        const ProbeAction next = controller.OnStableTarget(1500U, 201U);
        CHECK_TRUE(next.start);
        CHECK_EQ(controller.probe_start_ms(), 201U);
    }
    {
        ProbeController controller;
        controller.OnStableTarget(1000U, 100U);
        controller.OnFeedback(Feedback(0U, 1.0, false, 200U));
        controller.OnFeedback(Feedback(0U, 0.0, false, 100000U));
        CHECK_TRUE(!controller.OnStableTarget(1000U, 5199U).start);
        CHECK_TRUE(controller.OnStableTarget(1000U, 5200U).start);
        CHECK_EQ(controller.probe_start_ms(), 5200U);
    }
    return EXIT_SUCCESS;
}

int TestTimeRollbackIsSafe() {
    ProbeController controller;
    controller.OnStableTarget(1000U, 1000U);
    controller.OnTimer(1050U);
    CHECK_TRUE(!controller.OnTimer(1U).stop);
    CHECK_EQ(controller.state(), ProbeState::Probing);

    const ProbeAction abort =
        controller.OnFeedback(Feedback(0U, 0.0, true, 2U));
    CHECK_TRUE(abort.stop);
    CHECK_EQ(controller.state(), ProbeState::Failed);

    CHECK_TRUE(!controller.OnStableTarget(1000U, 6001U).start);
    CHECK_TRUE(controller.OnStableTarget(1000U, 6050U).start);
    return EXIT_SUCCESS;
}

int TestByteCountingSaturatesAndIgnoresInactive() {
    ProbeController controller;
    controller.OnProbePacketSent(10U, 0U);
    CHECK_EQ(controller.probe_bytes_sent(), 0U);

    controller.OnStableTarget(1000U, 0U);
    controller.OnProbePacketSent(std::numeric_limits<uint64_t>::max() - 5U, 1U);
    controller.OnProbePacketSent(10U, 2U);
    CHECK_EQ(controller.probe_bytes_sent(),
             std::numeric_limits<uint64_t>::max());

    controller.OnFeedback(Feedback(2000U, 0.0, false, 3U));
    controller.OnProbePacketSent(10U, 4U);
    CHECK_EQ(controller.probe_bytes_sent(),
             std::numeric_limits<uint64_t>::max());
    return EXIT_SUCCESS;
}

int TestInvalidConfigurationAndNumericLimits() {
    ProbeConfig invalid;
    invalid.first_scale = std::numeric_limits<double>::infinity();
    invalid.next_scale = 1.0;
    invalid.duration_ms = 0U;
    invalid.success_ratio = 0.0;
    ProbeController normalized(invalid);
    CHECK_EQ(normalized.OnStableTarget(1000U, 0U).probe_kbps, 2000U);
    CHECK_TRUE(!normalized.OnTimer(99U).stop);
    CHECK_TRUE(normalized.OnTimer(100U).stop);
    CHECK_TRUE(normalized.OnFeedback(Feedback(1499U, 0.0, false, 101U)).accept ==
               false);

    ProbeConfig zero_max;
    zero_max.max_kbps = 0U;
    ProbeController disabled(zero_max);
    CHECK_TRUE(!disabled.OnStableTarget(1000U, 0U).start);

    ProbeConfig huge_scale;
    huge_scale.first_scale = std::numeric_limits<double>::max();
    huge_scale.max_kbps = std::numeric_limits<uint32_t>::max();
    ProbeController capped(huge_scale);
    const ProbeAction action =
        capped.OnStableTarget(std::numeric_limits<uint32_t>::max() - 1U, 0U);
    CHECK_TRUE(action.start);
    CHECK_EQ(action.probe_kbps, std::numeric_limits<uint32_t>::max());

    ProbeController time_limit;
    const uint64_t near_max = std::numeric_limits<uint64_t>::max() - 50U;
    time_limit.OnStableTarget(1000U, near_max);
    CHECK_TRUE(!time_limit.OnTimer(std::numeric_limits<uint64_t>::max()).stop);
    CHECK_EQ(time_limit.state(), ProbeState::Probing);
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    CHECK_EQ(TestPublicTypesUseGccNamespace(), EXIT_SUCCESS);
    CHECK_EQ(TestSuccessfulProbeSequence(), EXIT_SUCCESS);
    CHECK_EQ(TestLowAckFailsOnlyAfterProbePeriod(), EXIT_SUCCESS);
    CHECK_EQ(TestAckIsEvaluatedOnlyAfterProbePeriod(), EXIT_SUCCESS);
    CHECK_EQ(TestBadFeedbackAbortsImmediately(), EXIT_SUCCESS);
    CHECK_EQ(TestRetryBoundaryAndNoOverlap(), EXIT_SUCCESS);
    CHECK_EQ(TestMaxClampAndMeaninglessProbe(), EXIT_SUCCESS);
    CHECK_EQ(TestTimerStopAndTimeout(), EXIT_SUCCESS);
    CHECK_EQ(TestInvalidLongDurationIsNormalizedAndDeadlineStillApplies(),
             EXIT_SUCCESS);
    CHECK_EQ(TestIgnoredEventsDoNotAdvanceTime(), EXIT_SUCCESS);
    CHECK_EQ(TestTimeRollbackIsSafe(), EXIT_SUCCESS);
    CHECK_EQ(TestByteCountingSaturatesAndIgnoresInactive(), EXIT_SUCCESS);
    CHECK_EQ(TestInvalidConfigurationAndNumericLimits(), EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
