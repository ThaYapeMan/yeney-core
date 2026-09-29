// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "protocol.h"
#include <atomic>
#include <functional>
#include <memory>
namespace yeney {
struct Config {
    std::string name="YeneY", server;
    uint16_t port=3483;
    std::string discoveryAddress="255.255.255.255";
    std::array<uint8_t,6> mac{0x02,0,0,0,0,1};
    size_t streamBytes=256*1024, outputFrames=48000*8;
    std::function<void(const std::string&)> log;
};
class Player {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Player(Config, Sink&);
    ~Player();
    // Network operations, discovery, and backoff never block shutdown.
    // Sink callbacks and the optional logger must also remain nonblocking.
    void run(const std::atomic_bool& stop);
};
}
