// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "queue_decode.h"
#include "decimator.h"
#include <algorithm>
#include <stdexcept>
namespace yeney::detail {
void QueueDecodePolicy::finishPayload(const DecoderConfig &c, std::optional<uint64_t> frames, uint64_t at) {
    if (c.finitePcmContainers && frames && at != *frames)
        throw std::runtime_error("truncated PCM container payload");
}
bool QueueSourceLength::enabled(const DecoderConfig &c) {
    return c.finitePcmContainers && !c.transcoded && (c.codec == 'l' || c.codec == 'f');
}
bool QueueSourceLength::accept(const DecoderConfig &c, std::optional<uint64_t> exact) {
    if (!enabled(c))
        return true;
    if (exact && produced >= *exact) {
        ++removed;
        return false;
    }
    ++produced;
    return true;
}
uint64_t QueueSourceLength::finish(const DecoderConfig &c, std::optional<uint64_t> exact, unsigned rate,
                                   bool eof, std::string &error) {
    if (!enabled(c) || !exact || !packet || !rate)
        return 0;
    const unsigned target = c.convertToRate ? Decimator::targetRate(rate, c.convertToRate) : rate;
    const unsigned factor = rate / target;
    const auto count = [factor](uint64_t n) { return (n + factor - 1) / factor; };
    if (removed && c.flacLog)
        c.flacLog("source-long truncated " + std::to_string(count(*exact + removed) - count(*exact)) +
                  " frames source_frames=" + std::to_string(removed));
    if (produced >= *exact)
        return 0;
    // Length repair must not turn malformed packets or CRC failures into
    // apparently valid audio. Only an EOF count mismatch is repairable.
    if (!error.empty() && error != "ALAC packets end before declared trim range")
        return 0;
    const uint64_t missing = *exact - produced;
    if (!eof || !produced || missing > std::min(packet, rate / 10))
        throw std::runtime_error("source-short " + std::to_string(count(*exact) - count(produced)) +
                                 " frames");
    if (c.flacLog)
        c.flacLog("source-short padded " + std::to_string(count(*exact) - count(produced)) +
                  " frames source_frames=" + std::to_string(missing));
    error.clear();
    return missing;
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
