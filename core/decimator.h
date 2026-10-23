// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "sink.h"
#include <vector>
namespace yeney {
// Original double-precision, symmetric polyphase FIR. Only the output phase
// is evaluated; no filtered samples are computed merely to discard them.
class Decimator {
    struct Stereo {
        double left, right;
    };
    uint32_t source, target;
    unsigned divisor, radius;
    std::vector<double> filter;
    std::vector<Stereo> samples;
    int64_t base = 0, center = 0;
    uint64_t received = 0, emitted = 0, budget = UINT64_MAX, clipped = 0;
    Frame convolve(size_t);

public:
    static uint32_t targetRate(uint32_t source, uint32_t cap = 48000);
    explicit Decimator(uint32_t rate, uint64_t phase = 0, const std::vector<Frame> &history = {});
    unsigned factor() const { return divisor; }
    unsigned delayFrames() const { return radius; }
    uint32_t outputRate() const { return target; }
    const std::vector<double> &coefficients() const { return filter; }
    void push(const Frame *, size_t);
    // End the source, retaining a delayed tail until lookahead or final padding.
    void end(uint64_t frames);
    void lookahead(const Frame *, size_t);
    void pad();
    size_t take(Frame *, size_t);
    uint64_t produced() const { return emitted; }
    uint64_t clips() const { return clipped; }
    uint64_t frames() const { return budget; }
    bool done() const { return budget != UINT64_MAX && emitted == budget; }
};
} // namespace yeney
