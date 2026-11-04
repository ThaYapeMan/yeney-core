// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decoder.h"
#include "sinks/shm_v1/sink.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
using namespace yeney;
template <class T> T get(const uint8_t *p) {
    T value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}
int main(int argc, char **argv) {
    assert(argc == 5);
    DecoderConfig config;
    config.codec = argv[1][0];
    auto decoder = makeDecoder(config);
    const uint64_t expected = std::stoull(argv[3]);
    const bool known = std::stoi(argv[4]);
    std::ifstream file(argv[2], std::ios::binary);
    Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    assert(file && !bytes.empty());
    std::array<uint8_t, 6> mac{2, 0x7c, 0, uint8_t(getpid() >> 8), uint8_t(getpid()), 1};
    auto name = ShmV1Sink::segmentName(mac);
    {
        ShmV1Sink sink(mac);
        auto controlMac = mac;
        controlMac[5] = 2;
        auto controlName = ShmV1Sink::segmentName(controlMac);
        ShmV1Sink control(controlMac);
        int controlFd = shm_open(controlName.c_str(), O_RDONLY, 0);
        auto *baseline = static_cast<const shm_v1::TimedLayout *>(
            mmap(nullptr, sizeof(shm_v1::TimedLayout), PROT_READ, MAP_SHARED, controlFd, 0));
        assert(baseline != MAP_FAILED);
        close(controlFd);
        int fd = shm_open(name.c_str(), O_RDONLY, 0);
        auto *p = static_cast<const shm_v1::TimedLayout *>(
            mmap(nullptr, sizeof(shm_v1::TimedLayout), PROT_READ, MAP_SHARED, fd, 0));
        assert(p != MAP_FAILED);
        close(fd);
        size_t offset = 0;
        uint64_t played = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!decoder->done()) {
            assert(std::chrono::steady_clock::now() < deadline);
            offset += decoder->feed(bytes.data() + offset, bytes.size() - offset);
            if (offset == bytes.size())
                decoder->finish();
            Frame frames[256];
            const auto count = decoder->take(frames, 256);
            assert(decoder->error().empty());
            if (!count) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            uint64_t start = 0;
            assert(decoder->sourceStart(start) == known);
            if (known)
                assert(start == expected);
            sink.playTiming(played, 1000000000 + played * 1000000000 / 44100, 44100000);
            control.playTiming(played, 1000000000 + played * 1000000000 / 44100, 44100000);
            control.write(frames, count);
            sink.contentPosition(start + played, known);
            assert(sink.write(frames, count) == count);
            assert(get<uint64_t>(p->timing.content_frame) == start + played);
            assert(get<uint32_t>(p->timing.content_flags) == unsigned(known));
            // Ignore process-shared lock internals, random generation and wall-clock seconds.
            // Everything else in the v1 prefix and the prior 52 tail bytes is identical.
            auto left = p->legacy, right = baseline->legacy;
            std::memset(&left.lock, 0, sizeof(left.lock));
            std::memset(&right.lock, 0, sizeof(right.lock));
            std::memset(left.extension.generation, 0, 8);
            std::memset(right.extension.generation, 0, 8);
            left.updated = right.updated = 0;
            assert(std::memcmp(&left, &right, sizeof(left)) == 0);
            assert(std::memcmp(&p->timing, &baseline->timing, 52) == 0);
            played += count;
        }
        assert(played);
        munmap(const_cast<shm_v1::TimedLayout *>(baseline), sizeof(*baseline));
        shm_unlink(controlName.c_str());
        // A mixed block must explicitly clear validity; no stale known position.
        Frame frames[1]{};
        sink.contentPosition(123, false);
        sink.write(frames, 1);
        assert(get<uint32_t>(p->timing.content_flags) == 0);
        munmap(const_cast<shm_v1::TimedLayout *>(p), sizeof(*p));
    }
    shm_unlink(name.c_str());
    std::cout << "PASS content tail codec=" << config.codec << " start=" << expected << " known=" << known
              << '\n';
}
