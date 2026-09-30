// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "decoder.h"
#include "protocol.h"
#include <atomic>
#include <functional>
#include <memory>
namespace yeney {
struct Command {
    char letter;
    uint32_t value; // Big-endian field decoded from opcode offset 18.
    bool playing;   // Running, not paused, and frames submitted since stop/flush.
};
struct Config {
    std::string name = "YeneY", server;
    uint16_t port = 3483;
    std::string discoveryAddress = "255.255.255.255";
    std::array<uint8_t, 6> mac{0x02, 0, 0, 0, 0, 1};
    size_t transitionMaxFrames = 2 * 1024 * 1024; // per-track window cap
    size_t earlyMediaBytes = 256ull * 1024 * 1024;
    size_t streamBytes = 256 * 1024, outputFrames = 48000 * 8;
    std::function<void(const std::string &)> log;
    // Event-loop callback, before processing every complete strm command.
    // Must not block or call back into Player.
    std::function<void(const Command &)> observeCommand;
    // Network outputs can announce boundaries on submission while keeping
    // elapsed tied exclusively to the sink's audible coordinate.
    bool startOnSubmit = false;
    // Optional host codec selection; defaults to makeDecoder.
    std::function<std::unique_ptr<Decoder>(const DecoderConfig &)> decoderFactory;
};
class Player {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    Player(Config, Sink &);
    ~Player();
    // Network operations, discovery, and backoff never block shutdown.
    // Sink callbacks and the optional logger must also remain nonblocking.
    void run(const std::atomic_bool &stop);
};
} // namespace yeney
