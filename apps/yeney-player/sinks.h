// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "core/sink.h"
#include <fstream>
#include <string>
namespace yeney {
class NullSink : public Sink {
protected:
    uint64_t frames_ = 0;

public:
    void trackBoundary(uint64_t, const Format &, bool) override {}
    size_t write(const Frame *, size_t n) override {
        frames_ += n;
        return n;
    }
    void pause() override {}
    void resume() override {}
    void stop() override {}
    void flush() override {}
    uint64_t audibleFrames() const override { return frames_; }
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override {}
    bool paced() const override { return true; }
};
class WavSink : public NullSink {
    std::string path_;
    std::ofstream file_;
    uint32_t rate_ = 0;
    uint64_t bytes_ = 0;
    unsigned segment_ = 0;
    void finish();

public:
    explicit WavSink(std::string path) : path_(std::move(path)) {}
    ~WavSink() override;
    void close() { finish(); }
    void trackBoundary(uint64_t, const Format &, bool) override;
    size_t write(const Frame *, size_t) override;
};
} // namespace yeney
