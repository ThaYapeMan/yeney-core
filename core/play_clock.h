// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace yeney {
// Reconstruct the pacer's millisecond tick on CLOCK_MONOTONIC, including
// uint32 tick wrap. Remaining credit is audio whose scheduled interval ends
// at that tick; callbacks must not substitute the later export timestamp.
inline uint64_t scheduledPlayNs(uint32_t tick, uint64_t monoNs, double credit) {
    const uint64_t ms = monoNs / 1000000;
    const uint64_t end = (ms - uint32_t(uint32_t(ms) - tick)) * 1000000;
    const uint64_t lead = uint64_t(std::llround(std::max(0., credit) * 1e9));
    return end > lead ? end - lead : 0;
}
} // namespace yeney
