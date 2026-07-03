#include "rate_diagnostics.h"
#include "test_support.h"

int main() {
    using gcc::RateReason;
    CHECK_TRUE(gcc::ShouldReportRateTransition(
        RateReason::Normal, false, RateReason::Normal, 100U, 0U));
    CHECK_TRUE(!gcc::ShouldReportRateTransition(
        RateReason::Normal, true, RateReason::Normal, 1099U, 100U));
    CHECK_TRUE(gcc::ShouldReportRateTransition(
        RateReason::Normal, true, RateReason::Normal, 1100U, 100U));
    CHECK_TRUE(gcc::ShouldReportRateTransition(
        RateReason::Normal, true, RateReason::Loss, 101U, 100U));
    CHECK_TRUE(!gcc::ShouldReportRateTransition(
        RateReason::Normal, true, RateReason::Normal, 99U, 100U));

    CHECK_TRUE(gcc::ShouldReportRateTransition(
        RateReason::Loss, true, RateReason::Loss, 101U, 100U));
    CHECK_TRUE(gcc::ShouldReportRateTransition(
        RateReason::DelayOveruse, true, RateReason::DelayOveruse,
        101U, 100U));
    CHECK_TRUE(gcc::ShouldReportRateTransition(
        RateReason::ProbeAccepted, true, RateReason::ProbeAccepted,
        101U, 100U));
    return 0;
}
