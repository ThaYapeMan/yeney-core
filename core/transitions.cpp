// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
#include "transitions.h"
#include <algorithm>
#include <limits>
namespace yeney {
namespace {
int32_t clipped(int64_t n) { return int32_t(std::clamp<int64_t>(n, INT32_MIN, INT32_MAX)); }
int32_t scaled(int32_t value, uint32_t gain) {
    int64_t product = int64_t(value) * gain;
    int64_t result = product >= 0 ? product / 65536 : -((-product + 65535) / 65536);
    return clipped(result);
}
} // namespace
uint32_t rampGain(uint64_t position, uint64_t duration, bool rising) {
    uint32_t up = !duration || position >= duration ? 65536 : uint32_t(position * 65536 / duration);
    return rising ? up : 65536 - up;
}
Frame processFrame(Frame frame, uint32_t replayGain, uint32_t envelope) {
    uint32_t gain = uint32_t(uint64_t(replayGain ? replayGain : 65536) * envelope / 65536);
    return {scaled(frame.left, gain), scaled(frame.right, gain)};
}
Frame crossFrame(Frame oldFrame, Frame newFrame, uint32_t oldGain, uint32_t newGain, uint64_t position,
                 uint64_t duration) {
    uint32_t up = rampGain(position, duration, true);
    auto old = processFrame(oldFrame, oldGain, 65536 - up);
    auto next = processFrame(newFrame, newGain, up);
    return {clipped(int64_t(old.left) + next.left), clipped(int64_t(old.right) + next.right)};
}
} // namespace yeney
