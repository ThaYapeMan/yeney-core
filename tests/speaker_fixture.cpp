// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/player.h"
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <utility>
static std::atomic_bool done{false};
static void stop(int) { done = true; }
class Speaker : public yeney::Sink {
    std::string path;
    bool modes = false;
    uint64_t generation = 1;
    unsigned serial = 0;
    uint64_t produced = 0;
    uint32_t elapsed = 0;
    bool remote = std::getenv("YENEY_TEST_REMOTE") != nullptr;
    bool started = false, startPending = false, statePending = false, refetchPending = false,
         cachedPending = false, deferredPending = false;
    yeney::PlaybackState state = yeney::PlaybackState::Playing;
    void read() {
        std::ifstream input(path);
        unsigned next, ms;
        char value;
        if (!(input >> next >> ms >> value) || next <= serial)
            return;
        serial = next;
        if (value == 'd')
            deferredPending = true;
        if (value == 'c')
            startPending = true;
        elapsed = ms;
        if (value == 'x')
            refetchPending = true;
        state = value == 'p'   ? yeney::PlaybackState::Paused
                : value == 's' ? yeney::PlaybackState::Stopped
                               : yeney::PlaybackState::Playing;
        if (!started && state == yeney::PlaybackState::Playing)
            startPending = started = true;
        statePending = true;
    }

public:
    explicit Speaker(std::string file, bool handoff = false) : path(std::move(file)), modes(handoff) {}
    uint64_t playbackGeneration() const override {
        if (!modes)
            return 0;
        uint64_t value = generation;
        std::ifstream(path + ".generation") >> value;
        return value;
    }
    void playbackReady(uint64_t value) override {
        generation = value;
        produced = 0;
        started = startPending = statePending = refetchPending = false;
        std::cout << "MODE READY " << value << std::endl;
    }
    uint32_t maxSampleRate() const override { return modes && playbackGeneration() == 1 ? 192000 : 48000; }
    bool decodeAhead() const override { return !modes || generation == 1; }
    bool speakerDriven() const override { return !modes || generation == 1; }
    bool queueCommand(char c, uint32_t) override {
        if (std::getenv("YENEY_TEST_HOST_CONTROL") && (c == 'u' || c == 'p' || c == 'q'))
            return true;
        if (std::getenv("YENEY_TEST_CACHED") && c == 's' && produced)
            return cachedPending = true;
        return std::getenv("YENEY_TEST_CACHED") && c == 'q';
    }
    bool takeQueueStart() override {
        read();
        bool pending = deferredPending;
        deferredPending = false;
        return pending;
    }
    bool takeQueueCachedStart() override {
        bool pending = cachedPending;
        cachedPending = false;
        return pending;
    }
    bool takeQueueRefetch() override {
        read();
        const bool result = refetchPending;
        refetchPending = false;
        return result;
    }
    void coreEvent(const std::string &event, uint64_t, uint64_t) override {
        if (event == "strm")
            std::cout << "TRACK" << std::endl;
        if (event == "queue-refetch")
            std::cout << "REFETCH" << std::endl;
    }
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
    void flush() override { std::cout << "FLUSH" << std::endl; }
    uint64_t audibleFrames() const override {
        return remote ? uint64_t(elapsed) * 8 : modes && generation > 1 ? produced : 0;
    }
    void volume(uint32_t, uint32_t) override {}
    void power(bool) override {}
    bool paced() const override { return false; }
};
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4)
        return 2;
    yeney::Config config;
    config.server = "127.0.0.1";
    config.port = std::stoul(argv[1]);
    bool defer = std::getenv("YENEY_TEST_DEFER") != nullptr;
    config.deferQueueStart = [&] { return std::exchange(defer, false); };
    config.queueRelativeClock = std::getenv("YENEY_TEST_RELATIVE") != nullptr;
    config.queueRemoteSource = [](uint64_t) { return std::getenv("YENEY_TEST_REMOTE") != nullptr; };
    Speaker sink(argv[2], argc == 4);
    std::signal(SIGTERM, stop);
    yeney::Player player(config, sink);
    player.run(done);
}
