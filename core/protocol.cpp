// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "protocol.h"
#include <chrono>
#include <cstring>
#include <stdexcept>
namespace yeney {
uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) * 256 + p[1]; }
uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void append16(Bytes &b, uint16_t n) {
    b.push_back(n >> 8);
    b.push_back(n);
}
void append32(Bytes &b, uint32_t n) {
    for (int i = 24; i >= 0; i -= 8)
        b.push_back(n >> i);
}
Bytes packet(const std::string &op, const Bytes &body) {
    if (op.size() != 4)
        throw std::invalid_argument("opcode length");
    Bytes b(op.begin(), op.end());
    append32(b, body.size());
    b.insert(b.end(), body.begin(), body.end());
    return b;
}
Bytes hello(const std::array<uint8_t, 6> &mac, bool reconnect, uint64_t received, uint32_t maxRate) {
    Bytes b{12, 0};
    b.insert(b.end(), mac.begin(), mac.end());
    // Stable, locally generated UUID derived from the identity MAC. RFC 4122 layout.
    std::array<uint8_t, 16> uuid{0x59, 0x65, 0x6e, 0x65, 0x59, 0, 0x40, 0, 0x80, 0};
    std::copy(mac.begin(), mac.end(), uuid.begin() + 10);
    b.insert(b.end(), uuid.begin(), uuid.end());
    append16(b, reconnect ? 0x4000 : 0);
    append32(b, received >> 32);
    append32(b, received);
    b.push_back('E');
    b.push_back('N');
    std::string caps =
        "Model=yeney,ModelName=YeneY,AccuratePlayPoints=1,MaxSampleRate=" + std::to_string(maxRate) +
        ",alc,flc,mp3,aif,pcm";
    b.insert(b.end(), caps.begin(), caps.end());
    return packet("HELO", b);
}
Bytes statusPacket(const std::string &event, const Status &s, uint32_t stamp) {
    if (event.size() != 4)
        throw std::invalid_argument("event length");
    Bytes b(event.begin(), event.end());
    b.insert(b.end(), 3, 0);
    append32(b, s.streamSize);
    append32(b, s.streamFull);
    append32(b, s.received >> 32);
    append32(b, s.received);
    append16(b, 0xffff);
    append32(b, s.jiffies);
    append32(b, s.outputSize);
    append32(b, s.outputFull);
    append32(b, s.elapsed / 1000);
    append16(b, 0);
    append32(b, s.elapsed);
    append32(b, stamp);
    append16(b, 0);
    return packet("STAT", b);
}
Format pcmFormat(uint8_t size, uint8_t rate, uint8_t channels, uint8_t endian, uint32_t maxRate) {
    static constexpr uint32_t rates[] = {11025,  22050, 32000,  44100,  48000,   8000,   12000,
                                         16000,  24000, 96000,  88200,  176400,  192000, 352800,
                                         384000, 0,     705600, 768000, 1411200, 1536000};
    if (size < '0' || size > '3' || rate < '0' || rate >= '0' + sizeof(rates) / sizeof(*rates) ||
        channels < '1' || channels > '2' || (endian != '0' && endian != '1'))
        throw std::invalid_argument("unsupported PCM parameters");
    Format f{rates[rate - '0'], unsigned(size - '0' + 1) * 8, unsigned(channels - '0'), endian == '0'};
    if (!f.rate || f.rate > maxRate)
        throw std::invalid_argument("PCM rate exceeds sink maximum");
    return f;
}
Frame decodeFrame(const uint8_t *p, const Format &f) {
    auto sample = [&](const uint8_t *q) {
        uint32_t v = 0;
        unsigned bytes = f.bits / 8;
        for (unsigned i = 0; i < bytes; ++i)
            v = (v << 8) | q[f.bigEndian ? i : bytes - 1 - i];
        v <<= 32 - f.bits;
        int32_t result;
        std::memcpy(&result, &v, 4);
        return result;
    };
    int32_t l = sample(p);
    return {l, f.channels == 1 ? l : sample(p + f.bits / 8)};
}
std::vector<Bytes> ServerParser::feed(const uint8_t *p, size_t n) {
    pending_.insert(pending_.end(), p, p + n);
    std::vector<Bytes> messages;
    size_t at = 0;
    while (pending_.size() - at >= 2) {
        size_t length = be16(pending_.data() + at);
        if (length < 4)
            throw std::runtime_error("short server packet");
        if (pending_.size() - at < length + 2)
            break;
        messages.emplace_back(pending_.begin() + at + 2, pending_.begin() + at + 2 + length);
        at += length + 2;
    }
    pending_.erase(pending_.begin(), pending_.begin() + at);
    return messages;
}
uint32_t jiffies() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
bool discoveryReply(const uint8_t *p, size_t n) {
    if (!n || p[0] != 'E')
        return false;
    for (size_t i = 1; i < n;) {
        if (n - i < 5 || p[i + 4] > n - i - 5)
            return false;
        i += 5 + p[i + 4];
    }
    return true;
}
} // namespace yeney
