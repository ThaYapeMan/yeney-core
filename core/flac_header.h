// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
namespace yeney {
struct FlacSourceInfo {
    uint64_t totalSamples = 0;
    unsigned fixedBlockSize = 0;
};
struct FlacFrameOrigin {
    bool variable;
    uint64_t number;
};
// Original RFC 9639 frame-header parser. Decode the wire number rather than
// libFLAC's normalized sample number: a final short fixed block needs the
// original STREAMINFO block size, not the short frame's size.
inline std::optional<FlacFrameOrigin> flacFrameOrigin(const uint8_t *p, size_t n) {
    if (n < 6 || p[0] != 0xff || (p[1] & 0xfe) != 0xf8 || (p[3] & 1))
        return {};
    unsigned block = p[2] >> 4, rate = p[2] & 15;
    if (!block || rate == 15 || (p[3] >> 4) > 10)
        return {};
    size_t at = 4;
    unsigned c = p[at++], count = 0;
    uint64_t number = c;
    if (c & 128) {
        unsigned mask = 128;
        while (c & mask) {
            ++count;
            mask >>= 1;
        }
        if (count < 2 || count > 7 || at + count - 1 > n)
            return {};
        number = c & (mask - 1);
        for (unsigned i = 1; i < count; ++i) {
            c = p[at++];
            if ((c & 0xc0) != 0x80)
                return {};
            number = (number << 6) | (c & 63);
        }
        const uint64_t minimum[] = {0, 0, 128, 2048, 65536, 2097152, 67108864, 2147483648ull};
        if (number < minimum[count])
            return {};
    }
    if (number >= (uint64_t(1) << ((p[1] & 1) ? 36 : 31)))
        return {};
    at += block == 6 ? 1 : block == 7 ? 2 : 0;
    at += rate == 12 ? 1 : rate == 13 || rate == 14 ? 2 : 0;
    if (at >= n)
        return {};
    uint8_t crc = 0;
    for (size_t i = 0; i < at; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 128) ? uint8_t((crc << 1) ^ 7) : uint8_t(crc << 1);
    }
    if (crc != p[at])
        return {};
    return FlacFrameOrigin{bool(p[1] & 1), number};
}
} // namespace yeney
