#include "probe_budget.h"
#include "test_support.h"

#include <cstdint>
#include <limits>

int main() {
    CHECK_TRUE(gcc::ProbeBudgetBytes(8000U, 1000U, 1100U, 100U) ==
               100000U);
    CHECK_TRUE(gcc::ProbeBudgetBytes(8000U, 1000U, 1120U, 120U) ==
               120000U);
    CHECK_TRUE(gcc::ProbeBudgetBytes(8000U, 1000U, 1300U, 299U) ==
               299000U);
    CHECK_TRUE(gcc::ProbeBudgetBytes(8000U, 1000U, 1099U, 120U) ==
               99000U);
    CHECK_TRUE(gcc::ProbeBudgetBytes(8000U, 1000U, 999U, 120U) == 0U);
    CHECK_TRUE(gcc::ProbeBudgetBytes(8000U, 1000U, 1200U, 0U) == 0U);

    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    CHECK_TRUE(gcc::ProbeBudgetBytes(maximum, 0U, maximum, maximum) ==
               maximum);
    CHECK_TRUE(gcc::ProbeBudgetBytes(maximum, maximum, 0U, maximum) == 0U);
    return 0;
}
