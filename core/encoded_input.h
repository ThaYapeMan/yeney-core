// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "ring.h"
#include "sink.h"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <string>
#include <unistd.h>
namespace yeney {
// Event-loop owned input FIFO. Ordinary hosts retain the exact RAM ring.
// A finite remote queue host can reserve real disk capacity, so pausing PCM
// output does not falsely advertise a full 256 KiB remote-source buffer to LMS.
class EncodedInput {
    Ring<uint8_t> ram;
    int fd = -1;
    uint64_t readAt = 0, written = 0;
    size_t limit = 0;
    uint64_t allocated = 0;
    std::function<void(uint64_t)> allocate;
    mutable std::array<uint8_t, 8192> cache{};
    mutable uint64_t cachedAt = 0;
    mutable size_t cachedSize = 0;
    static void transfer(int file, void *data, size_t n, uint64_t at, bool write) {
        auto p = static_cast<uint8_t *>(data);
        while (n) {
            auto got = write ? pwrite(file, p, n, off_t(at)) : pread(file, p, n, off_t(at));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                throw std::runtime_error("encoded spool disk I/O failed");
            p += got;
            at += got;
            n -= size_t(got);
        }
    }

public:
    explicit EncodedInput(size_t capacity) : ram(capacity) {}
    ~EncodedInput() { clear(); }
    EncodedInput(const EncodedInput &) = delete;
    EncodedInput &operator=(const EncodedInput &) = delete;
    bool diskBacked() const { return fd >= 0; }
    size_t capacity() const { return diskBacked() ? limit : ram.capacity(); }
    size_t size() const { return diskBacked() ? size_t(written - readAt) : ram.size(); }
    size_t free() const { return capacity() - size(); }
    void clear() {
        if (fd >= 0)
            close(fd);
        fd = -1;
        readAt = written = cachedAt = 0;
        limit = cachedSize = 0;
        allocated = 0;
        allocate = {};
        ram.clear();
    }
    void spool(const std::string &directory, uint64_t bytes, std::function<void(uint64_t)> reservation = {}) {
        if (!bytes || bytes > UINT32_MAX - 65536ull || bytes > SIZE_MAX || size())
            throw std::invalid_argument("invalid encoded spool reservation");
        clear();
        auto name = directory + "/encoded-XXXXXX";
        std::vector<char> path(name.begin(), name.end());
        path.push_back(0);
        fd = mkstemp(path.data());
        if (fd < 0)
            throw std::runtime_error("encoded spool create failed");
        // The open descriptor survives speaker disconnect/pause, with no stale
        // names or source bytes left behind after process exit/crash.
        if (unlink(path.data()) || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            clear();
            throw std::runtime_error("encoded spool setup failed");
        }
        limit = size_t(bytes);
        allocate = std::move(reservation);
    }
    size_t push(const uint8_t *p, size_t n) {
        if (!diskBacked())
            return ram.push(p, n);
        n = std::min(n, free());
        const auto offset = written % limit;
        const auto first = std::min<uint64_t>(n, limit - offset);
        const auto high = std::max<uint64_t>(allocated, offset + first);
        if (high > allocated && allocate)
            allocate(high);
        allocated = high;
        transfer(fd, const_cast<uint8_t *>(p), first, offset, true);
        if (n > first)
            transfer(fd, const_cast<uint8_t *>(p + first), n - first, 0, true);
        written += n;
        return n;
    }
    uint8_t at(size_t i) const {
        if (!diskBacked())
            return ram.at(i);
        if (i >= size())
            throw std::out_of_range("encoded spool index");
        auto absolute = readAt + i;
        if (!cachedSize || absolute < cachedAt || absolute >= cachedAt + cachedSize) {
            cachedAt = absolute;
            cachedSize = std::min<uint64_t>({cache.size(), written - absolute, limit - absolute % limit});
            transfer(fd, cache.data(), cachedSize, absolute % limit, false);
        }
        return cache[size_t(absolute - cachedAt)];
    }
    void discard(size_t n) {
        if (!diskBacked()) {
            ram.discard(n);
            return;
        }
        if (n > size())
            throw std::out_of_range("encoded spool discard");
        readAt += n;
        if (readAt >= cachedAt + cachedSize)
            cachedSize = 0;
    }
};
} // namespace yeney
