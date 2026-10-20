// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "queue_mode.h"
#include <deque>
namespace yeney::detail {
struct PlaybackTrack : QueueTrackState {
    Format format;
    std::unique_ptr<Decoder> decoder;
    bool decoderFinished = false;
    std::deque<Frame> output;
    size_t threshold = 0, outputThreshold = 0;
    int autostart = 1;
    bool known = false, headerPcm = false, wave = false, ready = false, eof = false, decoded = false,
         boundary = false, gapless = false;
    uint64_t first = 0, end = UINT64_MAX, skipped = 0, played = 0, produced = 0;
    uint32_t replayGain = 0;
    unsigned transition = 0, period = 0;
    uint64_t window = 0, outStart = 0, outLength = 0;
    bool crossChecked = false;
};
struct Overlap {
    std::shared_ptr<PlaybackTrack> old, next;
    uint64_t length = 0, position = 0;
};
} // namespace yeney::detail
