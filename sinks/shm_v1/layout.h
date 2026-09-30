// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
#pragma once
#include <cstddef>
#include <cstdint>
#include <pthread.h>
namespace yeney::shm_v1 {
// Byte arrays give unaligned uint64 wire fields their exact ABI positions.
struct Extension {
    uint32_t magic;
    uint16_t version, flags;
    uint32_t sequence;
    uint8_t generation[8], position[8], gaps[8], padding[4];
};
struct Layout {
    pthread_rwlock_t lock;
    uint32_t capacity, index;
    uint8_t running, reserved[3];
    uint32_t rate;
    int64_t updated;
    int16_t pcm[16384];
    Extension extension;
};
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "SHM v1 requires little endian");
static_assert(sizeof(pthread_rwlock_t) == 56, "unsupported legacy pthread lock ABI");
static_assert(offsetof(Layout, lock) == 0);
static_assert(offsetof(Layout, capacity) == 56);
static_assert(offsetof(Layout, index) == 60);
static_assert(offsetof(Layout, running) == 64);
static_assert(offsetof(Layout, reserved) == 65);
static_assert(offsetof(Layout, rate) == 68);
static_assert(offsetof(Layout, updated) == 72);
static_assert(offsetof(Layout, pcm) == 80);
static_assert(offsetof(Layout, extension) == 32848);
static_assert(offsetof(Layout, extension) + offsetof(Extension, magic) == 32848);
static_assert(offsetof(Layout, extension) + offsetof(Extension, version) == 32852);
static_assert(offsetof(Layout, extension) + offsetof(Extension, flags) == 32854);
static_assert(offsetof(Layout, extension) + offsetof(Extension, sequence) == 32856);
static_assert(offsetof(Layout, extension) + offsetof(Extension, generation) == 32860);
static_assert(offsetof(Layout, extension) + offsetof(Extension, position) == 32868);
static_assert(offsetof(Layout, extension) + offsetof(Extension, gaps) == 32876);
static_assert(offsetof(Layout, extension) + offsetof(Extension, padding) == 32884);
static_assert(sizeof(Extension) == 40);
static_assert(sizeof(Layout) == 32888);
constexpr uint32_t magic = 0x48555345;
constexpr uint32_t oddSuccessor(uint32_t n) { return n + (n & 1 ? 2u : 1u); }
int16_t sample16(int32_t);
} // namespace yeney::shm_v1
