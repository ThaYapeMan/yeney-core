// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_flac_origin.h"
#include <cstddef>
#include <cstdint>
#include <optional>
namespace yeney {
struct FlacSourceInfo {
    uint64_t totalSamples = 0;
    unsigned fixedBlockSize = 0;
};
// Keep the existing header-only API for hosts and standalone wire fixtures.
inline std::optional<FlacFrameOrigin> flacFrameOrigin(const uint8_t *p, size_t n) {
    return detail::queueFlacFrameOrigin(p, n);
}
} // namespace yeney
