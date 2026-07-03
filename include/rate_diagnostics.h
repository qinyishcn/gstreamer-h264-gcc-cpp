#pragma once

#include <cstdint>

#include "rate_policy.h"

namespace gcc {

bool ShouldReportRateTransition(RateReason reason,
                                bool has_last_report,
                                RateReason last_reason,
                                uint64_t now_ms,
                                uint64_t last_report_ms) noexcept;

}  // namespace gcc
