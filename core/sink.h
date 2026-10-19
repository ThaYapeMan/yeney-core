// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace yeney {
struct Format {
    uint32_t rate = 48000;
    unsigned bits = 16, channels = 2;
    bool bigEndian = false;
};
struct Frame {
    int32_t left, right;
};
struct TrackDescriptor {
    uint64_t occurrence = 0;
    uint32_t rate = 0;
    std::optional<uint64_t> sourceFrames;
    unsigned transition = 0, period = 0;
    uint64_t transitionFrames = 0;
    char codec = '?';
};
struct ItemBoundary {
    uint64_t occurrence = 0, next = 0, frame = 0, overlap = 0;
    std::string reason;
};
struct AudibleStart {
    uint64_t occurrence = 0, frame = 0;
    // Speaker-confirmed zero is audible even before a positive PCM clock tick.
    bool confirmed = false;
};
enum class PlaybackState { Playing, Paused, Stopped };
struct AudibleState {
    uint64_t occurrence = 0;
    PlaybackState state = PlaybackState::Playing;
};
// All calls occur on the player's event-loop thread. Positions are cumulative
// stereo frames, including across format changes. stop/flush must not rewind them.
// flush discards in-flight output; callbacks must return promptly. Format describes
// the source PCM; write always receives normalized 32-bit stereo frames.
class Sink {
public:
    virtual ~Sink() = default;
    // Queue-only capability. HTTP reads and decode progress never imply audible start.
    virtual bool decodeAhead() const { return false; }
    virtual void trackDescriptor(const TrackDescriptor &) {}
    virtual void resolvedItemBoundary(const ItemBoundary &) {}
    virtual void coreEvent(const std::string &, uint64_t, uint64_t) {}
    virtual bool takeAudibleStart(AudibleStart &) { return false; }
    // Opt-in remote queue clock/state. Defaults preserve existing stream sinks.
    virtual bool speakerDriven() const { return false; }
    virtual bool takeAudibleState(AudibleState &) { return false; }
    virtual std::optional<uint32_t> audibleElapsedMs(uint64_t) const { return std::nullopt; }
    // Queue promise, excluding any retained predecessor prefix. Used only to
    // bound a late outgoing overlap by the actual remaining audible item time.
    virtual std::optional<uint64_t> committedItemFrames(uint64_t) const { return std::nullopt; }
    // A sealed-only host cannot announce an unknown-count item until its
    // outgoing boundary resolves. Its pre-start successor deadline is zero.
    virtual bool awaitingPublication(uint64_t) const { return false; }
    virtual uint32_t maxSampleRate() const { return 48000; }
    virtual void trackBoundary(uint64_t frame, const Format &, bool gaplessCandidate) = 0;
    virtual size_t write(const Frame *, size_t frames) = 0;
    // Authoritative per-block DSP provenance. true means gain/envelope/mixing
    // (or a future converter) was applied; normalization alone is lossless.
    // Existing sinks need not use provenance and retain their write callback.
    virtual size_t write(const Frame *pcm, size_t frames, bool processed) {
        (void)processed;
        return write(pcm, frames);
    }
    // Optional paced-play schedule; firstFrame is the cumulative sink coordinate.
    virtual void playTiming(uint64_t, uint64_t, uint32_t) {}
    virtual void syncPause(uint64_t) { pause(); }
    virtual void syncSkip(uint64_t) {}
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void flush() = 0;
    virtual uint64_t audibleFrames() const = 0;
    // Frames which have reached the output device/feeder start point.
    virtual uint64_t startedFrames() const { return audibleFrames(); }
    // Output completion is distinct from its audible clock for remote sinks.
    virtual bool drained(uint64_t submitted) const { return audibleFrames() >= submitted; }
    // Starvation must include downstream output, even when drained() reports
    // feeder completion for a remote device with a coarse audible clock.
    virtual bool outputEmpty(uint64_t submitted) const { return drained(submitted); }
    virtual void volume(uint32_t left, uint32_t right) = 0; // 16.16 gain, no PCM scaling
    virtual void power(bool enabled) = 0;
    virtual void idle() {}          // No PCM available; optional analysis silence notification.
    virtual bool paced() const = 0; // true: core supplies frames at real time
};
} // namespace yeney
