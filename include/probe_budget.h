#pragma once

#include <cstdint>

namespace gcc {

uint64_t ProbeBudgetBytes(uint64_t probe_kbps,
                          uint64_t probe_start_ms,
                          uint64_t now_ms,
                          uint64_t duration_ms) noexcept;

uint64_t ProbePaddingDeficitBytes(uint64_t desired_aggregate_bytes,
                                  uint64_t aggregate_sent_bytes,
                                  uint64_t queued_padding_bytes) noexcept;

}  // namespace gcc
