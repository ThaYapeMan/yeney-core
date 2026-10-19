// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/player.h"
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
static std::atomic_bool done{false};
static void stop(int) { done = true; }
class Speaker : public yeney::Sink {
    std::string path;
    unsigned serial = 0;
    uint64_t produced = 0;
    uint32_t elapsed = 0;
    bool started = false, startPending = false, statePending = false;
    yeney::PlaybackState state = yeney::PlaybackState::Playing;
    void read() {
        std::ifstream input(path);
        unsigned next, ms;
        char value;
        if (!(input >> next >> ms >> value) || next <= serial)
            return;
        serial = next;
        elapsed = ms;
        state = value == 'p'   ? yeney::PlaybackState::Paused
                : value == 's' ? yeney::PlaybackState::Stopped
                               : yeney::PlaybackState::Playing;
        if (!started && state == yeney::PlaybackState::Playing)
            startPending = started = true;
        statePending = true;
    }

public:
    explicit Speaker(std::string file) : path(std::move(file)) {}
    bool decodeAhead() const override { return true; }
    bool speakerDriven() const override { return true; }
    bool takeAudibleStart(yeney::AudibleStart &s) override {
        read();
        if (!startPending || !produced)
            return false;
        startPending = false;
        s = {1, 0, true};
        return true;
    }
    bool takeAudibleState(yeney::AudibleState &s) override {
        if (!statePending)
            return false;
        statePending = false;
        s = {1, state};
        return true;
    }
    std::optional<uint32_t> audibleElapsedMs(uint64_t id) const override {
        return id == 1 ? std::optional<uint32_t>(elapsed) : std::nullopt;
    }
    void trackBoundary(uint64_t, const yeney::Format &, bool) override {}
    size_t write(const yeney::Frame *, size_t n) override {
        produced += n;
        return n;
    }
    void pause() override {}
    void resume() override {}
    void stop() override {}
    void flush() override {}
    uint64_t audibleFrames() const override { return 0; }
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override {}
    bool paced() const override { return false; }
};
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    yeney::Config config;
    config.server = "127.0.0.1";
    config.port = std::stoul(argv[1]);
    Speaker sink(argv[2]);
    std::signal(SIGTERM, stop);
    yeney::Player player(config, sink);
    player.run(done);
}
