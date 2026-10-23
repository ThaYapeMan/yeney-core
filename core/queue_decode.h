// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "decoder.h"
#include <algorithm>
namespace yeney::detail {
// The sole queue-capability decoder policy. Strict stream defaults and source
// metadata/counting are preserved; no PCM synthesis or DSP lives here.
struct QueueDecodePolicy {
    static bool allowPartialMp4(const DecoderConfig &c) { return c.finitePcmContainers; }
    static bool payloadEnded(const DecoderConfig &c, std::optional<uint64_t> frames, uint64_t at) {
        return c.finitePcmContainers && frames && at >= *frames;
    }
    static bool payloadAtEnd(const DecoderConfig &c, std::optional<uint64_t> frames, uint64_t at) {
        return c.finitePcmContainers && frames && at == *frames;
    }
    static void finishPayload(const DecoderConfig &, std::optional<uint64_t>, uint64_t);
    static bool partialMp4(bool enabled, bool selected, bool inMedia, bool allPackets) {
        return enabled && selected && (inMedia || allPackets);
    }
};
// Queue-only promised-length reconciliation, before any resampling or mixing.
class QueueSourceLength {
    uint64_t produced = 0, removed = 0;
    unsigned packet = 0;
    static bool enabled(const DecoderConfig &);

public:
    void packetFrames(unsigned n) { packet = std::max(packet, n); }
    bool accept(const DecoderConfig &, std::optional<uint64_t>);
    uint64_t finish(const DecoderConfig &, std::optional<uint64_t>, unsigned rate, bool eof,
                    std::string &error);
};
class QueueFlacSource {
    uint64_t total = 0;
    unsigned blockSize = 0;
    Bytes firstBytes;

public:
    void capture(const DecoderConfig &, const uint8_t *, size_t);
    void metadata(uint64_t, unsigned);
    void resolve(const DecoderConfig &, bool sampleNumber, uint64_t number,
                 const std::function<void(uint64_t)> &publish);
};
} // namespace yeney::detail
