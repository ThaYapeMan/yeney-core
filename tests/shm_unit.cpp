// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
#include "sinks/shm_v1/sink.h"
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
// Link-time syscall wrappers exercise real production entropy fallback/failure.
static bool denyRandom = false, denyUrandom = false;
extern "C" ssize_t __real_getrandom(void *, size_t, unsigned);
extern "C" int __real_open(const char *, int, ...);
extern "C" ssize_t __wrap_getrandom(void *p, size_t n, unsigned flags) {
    if (denyRandom) {
        errno = ENOSYS;
        return -1;
    }
    return __real_getrandom(p, n, flags);
}
extern "C" int __wrap_open(const char *path, int flags, ...) {
    if (denyUrandom && std::strcmp(path, "/dev/urandom") == 0) {
        errno = EACCES;
        return -1;
    }
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        auto mode = va_arg(args, int);
        va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}
using namespace yeney;
uint64_t get64(const uint8_t *p) {
    uint64_t n;
    std::memcpy(&n, p, 8);
    return n;
}
int main() {
    std::array<uint8_t, 6> mac{2, 0x7e, 0x3a, uint8_t(getpid() >> 8), uint8_t(getpid()), 1};
    std::string name = ShmV1Sink::segmentName(mac);
    ::shm_unlink(name.c_str());
    int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    assert(fd >= 0 && ::ftruncate(fd, sizeof(shm_v1::Layout)) == 0);
    auto *p = static_cast<shm_v1::Layout *>(
        ::mmap(nullptr, sizeof(shm_v1::Layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    assert(p != MAP_FAILED);
    struct stat st;
    assert(::fstat(fd, &st) == 0 && st.st_size == 32888);
    auto inode = st.st_ino;
    p->extension.sequence = 0xffffffffu;
    uint64_t gen;
    {
        ShmV1Sink sink(mac);
        gen = get64(p->extension.generation);
        assert(p->extension.sequence == 2 && p->extension.magic == 0x48555345);
        assert(p->extension.version == 1 && p->extension.flags == 0 && p->capacity == 16384);
        assert(!p->running && p->rate == 44100 && p->index == 0);
        assert(sink.paced() && sink.maxSampleRate() == 48000);
        assert(shm_v1::oddSuccessor(2) == 3 && shm_v1::oddSuccessor(3) == 5);
        std::vector<Frame> frames(8192 * 3 + 39);
        for (size_t i = 0; i < frames.size(); ++i)
            frames[i] = {int32_t(i * 65536), -int32_t(i * 65536)};
        sink.trackBoundary(0, Format{48000}, false);
        assert(p->rate == 44100); // publish rate with the first audio, not early
        assert(sink.write(frames.data(), frames.size()) == frames.size());
        assert(get64(p->extension.position) == frames.size() && p->index == 78 && p->running);
        assert(p->rate == 48000 && p->updated > 0 && get64(p->extension.generation) == gen);
        for (size_t i = frames.size() - 8192; i < frames.size(); ++i) {
            assert(p->pcm[2 * (i % 8192)] == shm_v1::sample16(frames[i].left));
            assert(p->pcm[2 * (i % 8192) + 1] == shm_v1::sample16(frames[i].right));
        }
        uint64_t pos = get64(p->extension.position);
        sink.pause();
        assert(!p->running && sink.write(frames.data(), 5) == 0);
        assert(get64(p->extension.position) == pos);
        sink.resume();
        assert(!p->running);
        sink.write(frames.data(), 1);
        assert(p->running);
        sink.stop();
        assert(!p->running);
        sink.resume();
        sink.write(frames.data(), 1);
        sink.flush();
        assert(!p->running && get64(p->extension.position) == pos + 2);
        int ready[2], release[2];
        assert(::pipe(ready) == 0 && ::pipe(release) == 0);
        pid_t child = ::fork();
        assert(child >= 0);
        if (!child) {
            assert(::pthread_rwlock_rdlock(&p->lock) == 0);
            char byte = 1;
            assert(::write(ready[1], &byte, 1) == 1);
            assert(::read(release[0], &byte, 1) == 1);
            ::pthread_rwlock_unlock(&p->lock);
            ::_exit(0);
        }
        char byte;
        assert(::read(ready[0], &byte, 1) == 1);
        uint32_t sequence = p->extension.sequence;
        sink.write(frames.data(), 10);
        assert(p->extension.sequence == sequence && get64(p->extension.gaps) == 0);
        assert(sink.audibleFrames() == pos + 12);
        assert(::write(release[1], &byte, 1) == 1);
        int childStatus;
        assert(::waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus) &&
               WEXITSTATUS(childStatus) == 0);
        for (int descriptor : {ready[0], ready[1], release[0], release[1]})
            ::close(descriptor);
        sink.write(frames.data(), 1);
        assert(get64(p->extension.position) == pos + 3 && get64(p->extension.gaps) == 1);
        sink.trackBoundary(sink.audibleFrames(), Format{44100}, true);
        assert(p->rate == 48000);
        sink.write(frames.data(), 1);
        assert(p->rate == 44100);
        p->extension.sequence = 0xfffffffeu;
        sink.write(frames.data(), 1);
        assert(p->extension.sequence == 0);
        sink.idle();
        assert(!p->running);
    }
    assert(!p->running);
    {
        ShmV1Sink sink(mac, 192000);
        assert(get64(p->extension.generation) != gen && get64(p->extension.position) == 0);
        assert(p->index == 0 && sink.maxSampleRate() == 192000);
        assert(::fstat(fd, &st) == 0 && st.st_ino == inode);
    }
    denyRandom = true;
    {
        ShmV1Sink sink(mac);
        assert(!(p->extension.sequence & 1));
    }
    denyUrandom = true;
    try {
        ShmV1Sink sink(mac);
        assert(false);
    } catch (const std::runtime_error &e) {
        assert(p->extension.sequence & 1);
        assert(std::string(e.what()).find("secure generation failed") != std::string::npos);
    }
    denyRandom = denyUrandom = false;
    for (auto pair : std::vector<std::pair<int32_t, int16_t>>{{0, 0},
                                                              {32767, 0},
                                                              {32768, 1},
                                                              {-32767, 0},
                                                              {-32768, -1},
                                                              {65535, 1},
                                                              {-65535, -1},
                                                              {INT32_MAX, 32767},
                                                              {INT32_MIN, -32768}})
        assert(shm_v1::sample16(pair.first) == pair.second);
    ::munmap(p, sizeof(shm_v1::Layout));
    ::close(fd);
    ::shm_unlink(name.c_str());
    std::cout << "PASS SHM v1: layout, sequence wrap, secure generation/reuse/failure, laps, rounding, rate, "
                 "lifecycle, lock gaps\n";
}
