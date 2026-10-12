// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "core/player.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <fstream>
#include <iostream>
std::atomic_bool stopped{false};
extern "C" void signalStop(int) { stopped.store(true); }
class RecordingSink : public yeney::Sink {
    std::ofstream pcm_, events_;
    uint64_t frames_ = 0;
    std::string mode_;
    unsigned rate_ = 48000;
    int processed_ = -1;
    uint64_t pendingBoundary_ = 0;
    using Clock = std::chrono::steady_clock;
    struct Delivery {
        Clock::time_point at;
        uint64_t frames;
    };
    mutable std::deque<Delivery> delivery_;
    mutable uint64_t audible_ = 0;
    Clock::time_point lastDelivery_{}, blockedUntil_{}, pausedAt_{};
    bool paused_ = false;
    std::deque<yeney::AudibleStart> starts_;
    std::deque<uint64_t> described_;
    uint64_t describedLast_ = 0;

public:
    explicit RecordingSink(const std::string &path, std::string mode)
        : pcm_(path + ".pcm", std::ios::binary), events_(path + ".events"), mode_(std::move(mode)) {}
    bool decodeAhead() const override { return mode_ == "queue"; }
    void trackDescriptor(const yeney::TrackDescriptor &d) override {
        if (describedLast_ != d.occurrence) {
            described_.push_back(d.occurrence);
            describedLast_ = d.occurrence;
        }
        events_ << "descriptor " << d.occurrence << ' ' << d.rate << ' '
                << (d.sourceFrames ? std::to_string(*d.sourceFrames) : "unknown") << std::endl;
    }
    void resolvedItemBoundary(const yeney::ItemBoundary &b) override {
        events_ << "item-end " << b.occurrence << ' ' << b.next << ' ' << b.frame << ' ' << b.overlap << ' '
                << b.reason << std::endl;
    }
    void coreEvent(const std::string &kind, uint64_t id, uint64_t frame) override {
        events_ << kind << ' ' << id << ' ' << frame << std::endl;
        if (kind == "strm")
            pendingOccurrence_ = id;
    }
    uint64_t pendingOccurrence_ = 0;
    bool takeAudibleStart(yeney::AudibleStart &s) override {
        if (starts_.empty() || audibleFrames() <= starts_.front().frame)
            return false;
        s = starts_.front();
        starts_.pop_front();
        return true;
    }
    void trackBoundary(uint64_t frame, const yeney::Format &f, bool gapless) override {
        rate_ = f.rate;
        if (decodeAhead()) {
            starts_.push_back({described_.front(), frame});
            described_.pop_front();
        }
        if (mode_ == "overrun" && frame) {
            pendingBoundary_ = frame;
            blockedUntil_ = Clock::now() + std::chrono::milliseconds(350);
        }
        if (mode_ == "blocked")
            blockedUntil_ = Clock::now() + std::chrono::milliseconds(120);
        events_ << "boundary " << frame << ' ' << f.rate << ' ' << f.bits << ' ' << f.channels << ' '
                << gapless << std::endl;
    }
    size_t write(const yeney::Frame *p, size_t n, bool processed) override {
        if (processed_ != int(processed)) {
            events_ << "processed " << processed << std::endl;
            processed_ = processed;
        }
        return write(p, n);
    }
    size_t write(const yeney::Frame *p, size_t n) override {
        // Exercise repeated partial acceptance on every write.
        if (mode_ == "blocked" && Clock::now() < blockedUntil_)
            return 0;
        n = std::min<size_t>(n, 37);
        for (size_t i = 0; i < n; ++i)
            for (uint32_t v : {uint32_t(p[i].left), uint32_t(p[i].right)})
                for (unsigned j = 0; j < 4; ++j)
                    pcm_.put(v >> (j * 8));
        frames_ += n;
        if (mode_ == "delayed" || mode_ == "staged" || mode_ == "queue") {
            if (delivery_.empty())
                lastDelivery_ = std::max(
                    lastDelivery_, Clock::now() + std::chrono::milliseconds(mode_ == "queue" ? 600 : 60));
            lastDelivery_ +=
                std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(n) / rate_));
            delivery_.push_back({lastDelivery_, frames_});
        }
        return n;
    }
    void pause() override {
        if (!paused_)
            pausedAt_ = Clock::now();
        paused_ = true;
        events_ << "pause" << std::endl;
    }
    void resume() override {
        if (paused_) {
            auto interval = Clock::now() - pausedAt_;
            for (auto &d : delivery_)
                d.at += interval;
            lastDelivery_ += interval;
        }
        paused_ = false;
        events_ << "resume" << std::endl;
    }
    void stop() override { events_ << "stop" << std::endl; }
    void flush() override {
        audibleFrames();
        delivery_.clear();
        frames_ = audible_;
        events_ << "flush" << std::endl;
    }
    uint64_t audibleFrames() const override {
        if (mode_ != "delayed" && mode_ != "staged" && mode_ != "queue")
            return audible_ = frames_;
        auto now = paused_ ? pausedAt_ : Clock::now();
        while (!delivery_.empty() && delivery_.front().at <= now) {
            audible_ = delivery_.front().frames;
            delivery_.pop_front();
        }
        return audible_;
    }
    void volume(uint32_t l, uint32_t r) override { events_ << "volume " << l << ' ' << r << std::endl; }
    void power(bool on) override { events_ << "power " << on << std::endl; }
    uint64_t startedFrames() const override {
        return mode_ == "overrun" && Clock::now() < blockedUntil_ ? pendingBoundary_ : frames_;
    }
    bool paced() const override {
        return mode_ != "delayed" && mode_ != "staged" && mode_ != "queue" && mode_ != "overrun" &&
               mode_ != "queue";
    }
};
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5)
        return 2;
    try {
        yeney::Config cfg;
        cfg.name = "Fixture";
        cfg.mac = {2, 1, 2, 3, 4, 5};
        cfg.server = std::string(argv[1]) == "discover" ? "" : argv[1];
        cfg.port = std::stoul(argv[2]);
        cfg.discoveryAddress = "127.0.0.1";
        cfg.streamBytes = 32768;
        cfg.queueStartupMs = 300;
        if (std::getenv("YENEY_TEST_SMART"))
            cfg.smartSameAlbum = [](uint64_t, uint64_t) { return true; };
        cfg.outputFrames = 96000;
        cfg.log = [](const std::string &s) { std::cout << s << std::endl; };
        if (std::getenv("YENEY_TEST_OBSERVER"))
            cfg.observeCommand = [](const yeney::Command &c) {
                std::cout << "observe " << c.letter << ' ' << c.value << ' ' << c.playing << std::endl;
            };
        if (std::getenv("YENEY_TEST_OBSERVER"))
            cfg.decoderFactory = [](const yeney::DecoderConfig &c) {
                std::cout << "factory " << c.codec << std::endl;
                return yeney::makeDecoder(c);
            };
        cfg.startOnSubmit =
            argc == 5 && (std::string(argv[4]) == "staged" || std::string(argv[4]) == "overrun");
        RecordingSink sink(argv[3], argc == 5 ? argv[4] : "normal");
        std::signal(SIGINT, signalStop);
        std::signal(SIGTERM, signalStop);
        yeney::Player player(std::move(cfg), sink);
        player.run(stopped);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
