// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/play_clock.h"
#include "sinks/shm_v1/sink.h"
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>
using namespace yeney;
template <class T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
int main() {
    // Sixty seconds, jittered export latency, sub-frame credit, and tick wrap.
    for (unsigned rate : {44100u, 48000u, 192000u}) {
        uint64_t frames = 0, start = uint64_t(UINT32_MAX - 30000) * 1000000;
        double credit = 0;
        for (unsigned ms = 1; ms <= 60000; ++ms) {
            credit += .001;
            size_t count = size_t(std::floor(credit * rate));
            uint64_t at = scheduledPlayNs(uint32_t(start / 1000000 + ms),
                                          start + ms * 1000000ull + (ms % 997) * 1000, credit);
            auto truth = start + uint64_t(std::llround(double(frames) / rate * 1e9));
            assert(std::abs(double(at) - double(truth)) <= 1);
            frames += count;
            credit -= double(count) / rate;
        }
    }
    std::array<uint8_t, 6> mac{2, 0x7d, 0, uint8_t(getpid() >> 8), uint8_t(getpid()), 1};
    auto name = ShmV1Sink::segmentName(mac);
    {
        ShmV1Sink sink(mac);
        int fd = shm_open(name.c_str(), O_RDONLY, 0);
        auto *p = static_cast<const shm_v1::TimedLayout *>(
            mmap(nullptr, sizeof(shm_v1::TimedLayout), PROT_READ, MAP_SHARED, fd, 0));
        assert(p != MAP_FAILED);
        close(fd);
        const auto &t = p->timing;
        Frame pcm[240]{};
        sink.trackBoundary(0, Format{48000}, false);
        sink.playTiming(0, 1000000000, 48000000);
        sink.write(pcm, 240);
        assert(p->legacy.extension.flags == 1 && !(p->legacy.extension.sequence & 1));
        assert(get<uint64_t>(t.anchor_abs_frame) == 0 && get<uint64_t>(t.anchor_play_mono_ns) == 1000000000);
        sink.syncPause(30000000);
        auto seq = get<uint32_t>(t.event_seq);
        assert(!p->legacy.running && get<uint32_t>(t.event_flags) == shm_v1::SYNC_PAUSE);
        assert(get<int64_t>(t.event_value) == 30000000 && get<uint64_t>(t.event_abs_frame) == 240);
        sink.resume();
        sink.playTiming(240, 1035000000, 48000000);
        sink.write(pcm, 240);
        assert(get<uint32_t>(t.event_seq) == seq && get<uint64_t>(t.anchor_abs_frame) == 240);
        assert(get<uint64_t>(t.anchor_play_mono_ns) == 1035000000);
        sink.syncSkip(960);
        assert(get<uint32_t>(t.event_flags) == shm_v1::SYNC_SKIP && get<int64_t>(t.event_value) == 960);
        sink.playTiming(480, 1040000000, 48000000);
        sink.write(pcm, 240);
        assert(get<uint64_t>(t.anchor_abs_frame) == 480 &&
               get<uint64_t>(t.anchor_play_mono_ns) == 1040000000);
        sink.trackBoundary(720, Format{44100}, true);
        sink.playTiming(720, 1045000000, 44100000);
        sink.write(pcm, 240);
        assert(get<uint32_t>(t.event_flags) == shm_v1::DISCONTINUITY &&
               get<uint64_t>(t.event_abs_frame) == 720);
        assert(get<uint32_t>(t.rate_milli_hz) == 44100000 && p->legacy.rate == 44100);
        // An uncommanded pacer gap must not silently retime older unread PCM.
        sink.playTiming(960, 1100000000, 44100000);
        sink.write(pcm, 240);
        assert(get<uint32_t>(t.event_flags) == shm_v1::DISCONTINUITY);
        assert(get<uint64_t>(t.event_abs_frame) == 960);
        sink.pause();
        assert(get<uint32_t>(t.event_flags) == shm_v1::PAUSE);
        sink.resume();
        assert(get<uint32_t>(t.event_flags) == shm_v1::RESUME);
        sink.flush();
        assert(get<uint32_t>(t.event_flags) == shm_v1::FLUSH);
        munmap(const_cast<shm_v1::TimedLayout *>(p), sizeof(shm_v1::TimedLayout));
    }
    shm_unlink(name.c_str());
    std::cout << "PASS player clock: 60 s schedule, tick wrap, latency independence, anchors, sync events, "
                 "gapless/rate, lifecycle\n";
}
