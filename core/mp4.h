// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "protocol.h"
#include <functional>
#include <memory>
namespace yeney {
struct Mp4Info {
    Bytes cookie;
    Format format;
    uint32_t framesPerPacket = 0;
    uint64_t totalFrames = 0, trimBegin = 0, trimEnd = 0;
};
// Own incremental box/table parser. Packet callbacks run synchronously and may
// apply backpressure. No seeking, and no ownership of the decoder or network.
class Mp4Demuxer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    using Header = std::function<void(const Mp4Info &)>;
    using Packet = std::function<void(const Bytes &)>;
    Mp4Demuxer(Header, Packet, size_t earlyMediaCap = 256ull * 1024 * 1024);
    ~Mp4Demuxer();
    void feed(const uint8_t *, size_t);
    void finish();
};
} // namespace yeney
