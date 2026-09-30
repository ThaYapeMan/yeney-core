// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "protocol.h"
#include <memory>
#include <string>
namespace yeney {
struct DecoderConfig {
    char codec = 'p';
    uint8_t size = '1', rate = '4', channels = '2', endian = '1';
    uint32_t maxRate = 48000;
    size_t earlyMediaCap = 256ull * 1024 * 1024;
};
// One worker per track. feed/take/status never wait for input or output; a full
// queue accepts fewer bytes. finish is idempotent; destruction cancels and joins.
class Decoder {
public:
    static constexpr size_t inputCapacity = 65536, outputCapacity = 8192;
    virtual ~Decoder() = default;
    virtual size_t feed(const uint8_t *, size_t) = 0;
    virtual void finish() = 0;
    virtual size_t take(Frame *, size_t) = 0;
    virtual bool format(Format &) const = 0;
    virtual bool done() const = 0;
    virtual std::string error() const = 0;
    virtual size_t inputBuffered() const = 0;
    virtual size_t outputBuffered() const = 0;
};
std::unique_ptr<Decoder> makeDecoder(const DecoderConfig &);
} // namespace yeney
