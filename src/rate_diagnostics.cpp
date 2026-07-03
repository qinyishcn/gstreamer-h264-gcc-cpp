#include "rate_diagnostics.h"

namespace gcc {

bool ShouldReportRateTransition(RateReason reason,
                                bool has_last_report,
                                RateReason last_reason,
                                uint64_t now_ms,
                                uint64_t last_report_ms) noexcept {
    if (reason != RateReason::Normal) return true;
    if (!has_last_report || last_reason != RateReason::Normal) return true;
    return now_ms >= last_report_ms &&
           now_ms - last_report_ms >= 1000U;
}

}  // namespace gcc
