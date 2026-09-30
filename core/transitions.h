// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
#pragma once
#include "sink.h"
namespace yeney {
uint32_t rampGain(uint64_t position, uint64_t duration, bool rising);
Frame processFrame(Frame, uint32_t replayGain, uint32_t envelope = 65536);
Frame crossFrame(Frame oldFrame, Frame newFrame, uint32_t oldGain, uint32_t newGain, uint64_t position,
                 uint64_t duration);
} // namespace yeney
