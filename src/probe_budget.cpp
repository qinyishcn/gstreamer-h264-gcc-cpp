#include "probe_budget.h"

#include <limits>

namespace gcc {
namespace {

uint64_t SaturatingMultiply(uint64_t left, uint64_t right) noexcept {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    if (left != 0U && right > maximum / left) return maximum;
    return left * right;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    if (right > maximum - left) return maximum;
    return left + right;
}

}  // namespace

uint64_t ProbeBudgetBytes(uint64_t probe_kbps,
                          uint64_t probe_start_ms,
                          uint64_t now_ms,
                          uint64_t duration_ms) noexcept {
    if (now_ms < probe_start_ms || duration_ms == 0U ||
        probe_kbps == 0U) {
        return 0U;
    }
    const uint64_t elapsed_ms =
        now_ms - probe_start_ms < duration_ms
            ? now_ms - probe_start_ms
            : duration_ms;

    const uint64_t whole = SaturatingMultiply(probe_kbps / 8U, elapsed_ms);
    const uint64_t remainder = probe_kbps % 8U;
    const uint64_t remainder_whole =
        SaturatingMultiply(remainder, elapsed_ms / 8U);
    const uint64_t remainder_tail =
        remainder * (elapsed_ms % 8U) / 8U;
    return SaturatingAdd(
        whole, SaturatingAdd(remainder_whole, remainder_tail));
}

uint64_t ProbePaddingDeficitBytes(uint64_t desired_aggregate_bytes,
                                  uint64_t aggregate_sent_bytes,
                                  uint64_t queued_padding_bytes) noexcept {
    if (aggregate_sent_bytes >= desired_aggregate_bytes) {
        return 0U;
    }
    const uint64_t remaining =
        desired_aggregate_bytes - aggregate_sent_bytes;
    return queued_padding_bytes >= remaining
               ? 0U
               : remaining - queued_padding_bytes;
}

}  // namespace gcc
