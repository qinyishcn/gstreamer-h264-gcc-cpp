#pragma once

#include <cstdint>

namespace gcc {

uint64_t ProbeBudgetBytes(uint64_t probe_kbps,
                          uint64_t probe_start_ms,
                          uint64_t now_ms,
                          uint64_t duration_ms) noexcept;

}  // namespace gcc
