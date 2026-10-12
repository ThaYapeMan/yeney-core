// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace yeney {
// Track elapsed must never consume a successor's frames or sample rate.
inline uint32_t trackElapsedMs(uint64_t frames, uint64_t first, uint64_t end, uint64_t skipped,
                               uint32_t rate) {
    frames = std::min(frames, end);
    return uint32_t((frames >= first ? frames - first + skipped : 0) * 1000 / rate);
}
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
