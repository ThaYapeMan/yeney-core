// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
#pragma once
#include "core/sink.h"
#include "layout.h"
#include <array>
#include <string>
namespace yeney {
class ShmV1Sink : public Sink {
    shm_v1::Layout *mapping_ = nullptr;
    uint32_t maximum_, rate_ = 44100;
    uint64_t audible_ = 0, exported_ = 0, gaps_ = 0, pendingGaps_ = 0;
    bool paused_ = false, active_ = false, syncPaused_ = false, timed_ = false;
    uint64_t playNs_ = 0, eventFrame_ = 0;
    uint32_t timingRate_ = 0, eventSeq_ = 0, eventFlags_ = 0;
    int64_t eventValue_ = 0;
    void event(uint32_t flags, int64_t value = 0);
    void publish(const Frame *, size_t);

public:
    static uint64_t secureGeneration();
    static std::string segmentName(const std::array<uint8_t, 6> &);
    ShmV1Sink(const std::array<uint8_t, 6> &, uint32_t maximum = 48000);
    ~ShmV1Sink() override;
    uint32_t maxSampleRate() const override { return maximum_; }
    void trackBoundary(uint64_t, const Format &, bool) override;
    size_t write(const Frame *, size_t) override;
    void playTiming(uint64_t, uint64_t, uint32_t) override;
    void syncPause(uint64_t) override;
    void syncSkip(uint64_t) override;
    void pause() override;
    void resume() override;
    void stop() override;
    void flush() override;
    uint64_t audibleFrames() const override { return audible_; }
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override;
    void idle() override;
    bool paced() const override { return true; }
};
} // namespace yeney
