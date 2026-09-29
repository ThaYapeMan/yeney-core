// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "sink.h"
#include <array>
#include <string>
#include <vector>
namespace yeney {
using Bytes = std::vector<uint8_t>;
uint16_t be16(const uint8_t*);
uint32_t be32(const uint8_t*);
void append16(Bytes&, uint16_t);
void append32(Bytes&, uint32_t);
Bytes packet(const std::string& opcode, const Bytes& body);
Bytes hello(const std::array<uint8_t,6>& mac, bool reconnect, uint64_t received, uint32_t maxRate);
struct Status {
    uint32_t streamSize=0, streamFull=0, jiffies=0, outputSize=0, outputFull=0, elapsed=0;
    uint64_t received=0;
};
Bytes statusPacket(const std::string& event, const Status&, uint32_t timestamp=0);
Format pcmFormat(uint8_t size, uint8_t rate, uint8_t channels, uint8_t endian, uint32_t maxRate);
Frame decodeFrame(const uint8_t*, const Format&);
// Server messages have a two-byte length that includes the four-byte opcode.
class ServerParser {
    Bytes pending_;
public:
    std::vector<Bytes> feed(const uint8_t*, size_t);
};
uint32_t jiffies();
bool due(uint32_t now, uint32_t deadline);
bool discoveryReply(const uint8_t*, size_t);
}
