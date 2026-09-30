// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
namespace yeney::detail {
inline int32_t floatToFrame(float sample) {
    if (sample >= 1.0f)
        return INT32_MAX;
    if (sample <= -1.0f)
        return INT32_MIN;
    if (std::isnan(sample))
        return 0;
    return static_cast<int32_t>(std::round(static_cast<double>(sample) * 2147483648.0));
}
} // namespace yeney::detail
