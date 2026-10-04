// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
#include "sink.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>
namespace yeney {
namespace {
template <class T> void put(uint8_t *p, T value) { std::memcpy(p, &value, sizeof(value)); }
void put64(uint8_t *p, uint64_t value) { put(p, value); }
shm_v1::TimingBlock &timing(shm_v1::Layout *p) { return reinterpret_cast<shm_v1::TimedLayout *>(p)->timing; }
void begin(shm_v1::Layout *p) {
    auto seq = __atomic_load_n(&p->extension.sequence, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->extension.sequence, shm_v1::oddSuccessor(seq), __ATOMIC_SEQ_CST);
}
void end(shm_v1::Layout *p) { __atomic_fetch_add(&p->extension.sequence, 1u, __ATOMIC_SEQ_CST); }
std::runtime_error unavailable(const std::string &why) {
    return std::runtime_error("SHM v1 sink unavailable: " + why);
}
} // namespace
int16_t shm_v1::sample16(int32_t value) {
    int64_t wide = value;
    // Nearest integer, half-way away from zero; no signed shifts or overflow.
    int64_t rounded = wide < 0 ? -((-wide + 32768) / 65536) : (wide + 32768) / 65536;
    return int16_t(std::clamp<int64_t>(rounded, -32768, 32767));
}
uint64_t ShmV1Sink::secureGeneration() {
    uint64_t value;
    auto *out = reinterpret_cast<uint8_t *>(&value);
    size_t done = 0;
    while (done < sizeof(value)) {
        ssize_t n = ::getrandom(out + done, sizeof(value) - done, GRND_NONBLOCK);
        if (n > 0)
            done += size_t(n);
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    if (done == sizeof(value))
        return value;
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw unavailable("secure generation failed (getrandom and /dev/urandom)");
    done = 0;
    while (done < sizeof(value)) {
        ssize_t n = ::read(fd, out + done, sizeof(value) - done);
        if (n > 0)
            done += size_t(n);
        else if (n < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    ::close(fd);
    if (done != sizeof(value))
        throw unavailable("secure generation read failed");
    return value;
}
std::string ShmV1Sink::segmentName(const std::array<uint8_t, 6> &mac) {
    char path[40];
    std::snprintf(path, sizeof(path),
                  "/squeeze"
                  "lite-%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return path;
}
ShmV1Sink::ShmV1Sink(const std::array<uint8_t, 6> &mac, uint32_t maximum) : maximum_(maximum) {
    int fd = ::shm_open(segmentName(mac).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (fd < 0)
        throw unavailable(std::strerror(errno));
    if (::ftruncate(fd, sizeof(shm_v1::TimedLayout)) != 0) {
        auto why = std::string(std::strerror(errno));
        ::close(fd);
        throw unavailable(why);
    }
    void *address = ::mmap(nullptr, sizeof(shm_v1::TimedLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (address == MAP_FAILED)
        throw unavailable(std::strerror(errno));
    mapping_ = static_cast<shm_v1::Layout *>(address);
    begin(mapping_); // Existing even/odd sequence survives until this odd successor.
    try {
        uint64_t generation = secureGeneration();
        pthread_rwlockattr_t attrs;
        int rc = ::pthread_rwlockattr_init(&attrs);
        if (rc)
            throw unavailable("rwlock attributes");
        rc = ::pthread_rwlockattr_setpshared(&attrs, PTHREAD_PROCESS_SHARED);
        if (!rc)
            rc = ::pthread_rwlock_init(&mapping_->lock, &attrs);
        ::pthread_rwlockattr_destroy(&attrs);
        if (rc)
            throw unavailable("process-shared rwlock initialization");
        rc = ::pthread_rwlock_trywrlock(&mapping_->lock);
        if (rc)
            throw unavailable("initial rwlock acquisition");
        mapping_->capacity = 16384;
        mapping_->index = 0;
        mapping_->running = 0;
        std::memset(mapping_->reserved, 0, sizeof(mapping_->reserved));
        mapping_->rate = rate_;
        mapping_->updated = 0;
        std::memset(mapping_->pcm, 0, sizeof(mapping_->pcm));
        mapping_->extension.magic = shm_v1::magic;
        mapping_->extension.version = 1;
        mapping_->extension.flags = 0;
        put64(mapping_->extension.generation, generation);
        put64(mapping_->extension.position, 0);
        put64(mapping_->extension.gaps, 0);
        std::memset(mapping_->extension.padding, 0, sizeof(mapping_->extension.padding));
        std::memset(&timing(mapping_), 0, sizeof(shm_v1::TimingBlock));
        put(timing(mapping_).magic, shm_v1::timingMagic);
        put(timing(mapping_).version, uint16_t(1));
        end(mapping_);
        ::pthread_rwlock_unlock(&mapping_->lock);
    } catch (...) {
        ::munmap(mapping_, sizeof(shm_v1::TimedLayout));
        mapping_ = nullptr;
        throw;
    }
}
ShmV1Sink::~ShmV1Sink() {
    if (mapping_) {
        stop();
        ::munmap(mapping_, sizeof(shm_v1::TimedLayout));
    }
    // LampaStream owns unlink and orphan cleanup; do not destroy the shared lock.
}
void ShmV1Sink::publish(const Frame *frames, size_t count) {
    if (::pthread_rwlock_trywrlock(&mapping_->lock) != 0) {
        ++pendingGaps_;
        return;
    }
    begin(mapping_);
    for (size_t i = 0; i < count; ++i) {
        uint32_t scalar = uint32_t((exported_ + i) % 8192) * 2;
        mapping_->pcm[scalar] = shm_v1::sample16(frames[i].left);
        mapping_->pcm[scalar + 1] = shm_v1::sample16(frames[i].right);
    }
    const uint64_t first = exported_;
    exported_ += count;
    gaps_ += pendingGaps_;
    pendingGaps_ = 0;
    mapping_->running = active_;
    if (count) {
        mapping_->index = uint32_t(exported_ % 8192) * 2;
        mapping_->rate = rate_; // New format is published with its first actual PCM.
        mapping_->updated = ::time(nullptr);
    }
    put64(mapping_->extension.position, exported_);
    put64(mapping_->extension.gaps, gaps_);
    auto &t = timing(mapping_);
    if (timed_ && count) {
        mapping_->extension.flags |= 1;
        put64(t.anchor_abs_frame, first);
        put64(t.anchor_play_mono_ns, playNs_);
        put(t.rate_milli_hz, timingRate_);
    }
    put(t.event_seq, eventSeq_);
    put(t.event_flags, eventFlags_);
    put64(t.event_abs_frame, eventFrame_);
    put(t.event_value, eventValue_);
    end(mapping_);
    ::pthread_rwlock_unlock(&mapping_->lock);
}
void ShmV1Sink::event(uint32_t flags, int64_t value) {
    ++eventSeq_;
    eventFlags_ = flags;
    eventValue_ = value;
    eventFrame_ = exported_;
}
void ShmV1Sink::playTiming(uint64_t, uint64_t ns, uint32_t rate) {
    timed_ = ns && rate;
    playNs_ = ns;
    timingRate_ = rate;
}
void ShmV1Sink::syncPause(uint64_t ns) {
    syncPaused_ = true;
    paused_ = true;
    active_ = false;
    event(shm_v1::SYNC_PAUSE, int64_t(ns));
    publish(nullptr, 0);
}
void ShmV1Sink::syncSkip(uint64_t frames) {
    event(shm_v1::SYNC_SKIP, int64_t(frames));
    publish(nullptr, 0);
}
void ShmV1Sink::trackBoundary(uint64_t, const Format &format, bool) {
    rate_ = format.rate;
    event(shm_v1::DISCONTINUITY);
}
size_t ShmV1Sink::write(const Frame *frames, size_t count) {
    if (paused_)
        return 0;
    if (!count)
        return 0;
    active_ = true;
    publish(frames, count);
    audible_ += count; // Skipped analysis exports do not stall the playback clock.
    return count;
}
void ShmV1Sink::pause() {
    syncPaused_ = false;
    event(shm_v1::PAUSE);
    paused_ = true;
    active_ = false;
    publish(nullptr, 0);
}
void ShmV1Sink::resume() {
    paused_ = false;
    if (!syncPaused_) {
        event(shm_v1::RESUME);
        publish(nullptr, 0);
    }
    syncPaused_ = false;
} // running becomes true only with audio.
void ShmV1Sink::stop() {
    syncPaused_ = false;
    event(shm_v1::FLUSH);
    active_ = false;
    paused_ = false;
    publish(nullptr, 0);
}
void ShmV1Sink::power(bool enabled) {
    if (!enabled)
        idle();
}
void ShmV1Sink::idle() {
    active_ = false;
    publish(nullptr, 0);
}
void ShmV1Sink::flush() {
    syncPaused_ = false;
    event(shm_v1::FLUSH);
    active_ = false;
    publish(nullptr, 0);
}
} // namespace yeney
