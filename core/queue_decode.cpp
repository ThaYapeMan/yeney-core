// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "queue_decode.h"
#include <algorithm>
#include <stdexcept>
namespace yeney::detail {
void QueueDecodePolicy::finishPayload(const DecoderConfig &c, std::optional<uint64_t> frames, uint64_t at) {
    if (c.finitePcmContainers && frames && at != *frames)
        throw std::runtime_error("truncated PCM container payload");
}
void QueueFlacSource::capture(const DecoderConfig &cfg, const uint8_t *bytes, size_t n) {
    if (cfg.finitePcmContainers && cfg.flacSourceInfo && firstBytes.size() < 64)
        firstBytes.insert(firstBytes.end(), bytes, bytes + std::min(n, 64 - firstBytes.size()));
}
void QueueFlacSource::metadata(uint64_t samples, unsigned block) {
    total = samples;
    blockSize = block;
}
void QueueFlacSource::resolve(const DecoderConfig &cfg, bool sampleNumber, uint64_t number,
                              const std::function<void(uint64_t)> &publish) {
    std::optional<FlacFrameOrigin> rawOrigin;
    if (cfg.finitePcmContainers && cfg.flacSourceInfo) {
        std::optional<FlacSourceInfo> info;
        if (total)
            info = FlacSourceInfo{total, blockSize};
        else
            rawOrigin = flacFrameOrigin(firstBytes.data(), firstBytes.size());
        if (!info && !rawOrigin && cfg.flacLog)
            cfg.flacLog("sealed fallback: first frame source number unavailable");
        // Never turn uncertain sync scanning into an exact promise.
        if (info || rawOrigin) {
            info = cfg.flacSourceInfo(info);
            if (info) {
                total = info->totalSamples;
                blockSize = info->fixedBlockSize;
            }
        }
    }
    if (total) {
        const uint64_t origin =
            rawOrigin      ? (rawOrigin->variable ? rawOrigin->number : rawOrigin->number * blockSize)
            : sampleNumber ? number
                           : uint64_t(number) * blockSize;
        if (origin > total || (rawOrigin && !rawOrigin->variable && !blockSize)) {
            if (cfg.finitePcmContainers && cfg.flacSourceInfo) {
                if (cfg.flacLog)
                    cfg.flacLog("sealed fallback: invalid source count/origin");
            } else
                throw std::runtime_error("FLAC seek origin exceeds source count");
        } else {
            publish(total - origin);
            if (cfg.flacLog)
                cfg.flacLog("exact remaining=" + std::to_string(total - origin) +
                            " first sample=" + std::to_string(origin));
        }
    }
}
} // namespace yeney::detail
