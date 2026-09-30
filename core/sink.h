// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include <cstddef>
#include <cstdint>

namespace yeney {
struct Format {
    uint32_t rate = 48000;
    unsigned bits = 16, channels = 2;
    bool bigEndian = false;
};
struct Frame {
    int32_t left, right;
};
// All calls occur on the player's event-loop thread. Positions are cumulative
// stereo frames, including across format changes. stop/flush must not rewind them.
// flush discards in-flight output; callbacks must return promptly. Format describes
// the source PCM; write always receives normalized 32-bit stereo frames.
class Sink {
public:
    virtual ~Sink() = default;
    virtual uint32_t maxSampleRate() const { return 48000; }
    virtual void trackBoundary(uint64_t frame, const Format &, bool gaplessCandidate) = 0;
    virtual size_t write(const Frame *, size_t frames) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void flush() = 0;
    virtual uint64_t audibleFrames() const = 0;
    // Frames which have reached the output device/feeder start point.
    virtual uint64_t startedFrames() const { return audibleFrames(); }
    // Output completion is distinct from its audible clock for remote sinks.
    virtual bool drained(uint64_t submitted) const { return audibleFrames() >= submitted; }
    virtual void volume(uint32_t left, uint32_t right) = 0; // 16.16 gain, no PCM scaling
    virtual void power(bool enabled) = 0;
    virtual void idle() {}          // No PCM available; optional analysis silence notification.
    virtual bool paced() const = 0; // true: core supplies frames at real time
};
} // namespace yeney
