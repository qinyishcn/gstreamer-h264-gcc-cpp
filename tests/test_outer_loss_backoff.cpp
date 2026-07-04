#include "outer_loss_backoff.h"
#include "test_support.h"

#include <cstdint>

int main() {
    gcc::OuterLossBackoff razor_then_fallback(200U);
    CHECK_TRUE(!razor_then_fallback.Evaluate(
        80U, 4000U, 50U, 100U, true, true).changed);
    gcc::OuterLossDecision after_razor =
        razor_then_fallback.Evaluate(
            80U, 3600U, 50U, 101U, true, false);
    CHECK_TRUE(after_razor.changed);
    CHECK_EQ(after_razor.proposed_kbps, static_cast<uint32_t>(1800));
    CHECK_TRUE(!razor_then_fallback.Evaluate(
        80U, 1800U, 50U, 102U, true, false).changed);

    gcc::OuterLossBackoff disabled_rearm(200U);
    CHECK_TRUE(disabled_rearm.Evaluate(
        80U, 4000U, 50U, 100U, true).changed);
    CHECK_TRUE(!disabled_rearm.Evaluate(
        0U, 2000U, 50U, 101U, false).changed);
    CHECK_TRUE(disabled_rearm.Evaluate(
        80U, 2000U, 50U, 102U, true).changed);

    gcc::OuterLossBackoff backoff(200U);
    gcc::OuterLossDecision decision =
        backoff.Evaluate(26U, 4001U, 100U, 1000U, true);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.proposed_kbps, static_cast<uint32_t>(2001));

    CHECK_TRUE(!backoff.Evaluate(
        26U, 2001U, 100U, 1249U, true).changed);
    CHECK_TRUE(!backoff.Evaluate(
        26U, 2001U, 100U, 1250U, true).changed);

    CHECK_TRUE(!backoff.Evaluate(
        25U, 2001U, 500U, 1500U, false).changed);
    decision = backoff.Evaluate(
        26U, 2001U, 500U, 1500U, true);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.proposed_kbps, static_cast<uint32_t>(1001));
    CHECK_TRUE(!backoff.Evaluate(
        25U, 2001U, 500U, 1750U, true).changed);
    decision = backoff.Evaluate(26U, 2001U, 500U, 1751U, true);
    CHECK_TRUE(decision.changed);
    CHECK_EQ(decision.proposed_kbps, static_cast<uint32_t>(1001));

    CHECK_TRUE(!backoff.Evaluate(
        25U, 1001U, 0U, 1800U, true).changed);
    CHECK_TRUE(backoff.Evaluate(
        26U, 500U, 0U, 1801U, true).changed);
    CHECK_TRUE(!backoff.Evaluate(
        26U, 250U, 0U, 1700U, true).changed);
    return EXIT_SUCCESS;
}
