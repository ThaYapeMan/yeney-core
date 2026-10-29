// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "core/encoded_input.h"
#include "core/flac_header.h"
#include "core/protocol.h"
#include "core/ring.h"
#include "core/sample_conversion.h"
#include "core/transitions.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <random>
using namespace yeney;
int main() {
    {
        auto parent = std::getenv("TMPDIR");
        std::string pattern = std::string(parent ? parent : "/var/tmp") + "/yeney-input-XXXXXX";
        std::vector<char> directory(pattern.begin(), pattern.end());
        directory.push_back(0);
        assert(mkdtemp(directory.data()));
        EncodedInput input(16);
        input.spool(directory.data(), 32768);
        std::deque<uint8_t> expected;
        std::mt19937 random(214);
        for (unsigned turn = 0; turn < 200; ++turn) {
            std::array<uint8_t, 7000> bytes;
            for (auto &b : bytes)
                b = uint8_t(random());
            auto n = input.push(bytes.data(), bytes.size());
            expected.insert(expected.end(), bytes.begin(), bytes.begin() + n);
            const auto take = std::min<size_t>(expected.size(), turn % 2 ? 5001 : 4137);
            for (size_t i = 0; i < take; ++i)
                assert(input.at(i) == expected[i]);
            input.discard(take);
            expected.erase(expected.begin(), expected.begin() + take);
            assert(input.size() == expected.size() && input.size() + input.free() == 32768);
        }
        for (size_t i = 0; i < expected.size(); ++i)
            assert(input.at(i) == expected[i]);
        input.clear();
        assert(input.capacity() == 16 && !input.size());
        assert(rmdir(directory.data()) == 0); // unlinked backing file leaves no artifacts
        std::cout << "PASS encoded input: disk FIFO exceeds RAM, wraps without loss, bounded capacity, pause "
                     "retention, clear restores RAM and removes backing file\n";
    }
    {
        const uint8_t fixed[] = {0xff, 0xf8, 0x79, 0x88, 4, 6, 0x4f, 0x6f};
        const uint8_t variable[] = {0xff, 0xf9, 0x79, 0x88, 0xe4, 0x80, 0x80, 6, 0x4f, 5};
        auto f = flacFrameOrigin(fixed, sizeof(fixed));
        auto v = flacFrameOrigin(variable, sizeof(variable));
        assert(f && !f->variable && f->number == 4);
        assert(v && v->variable && v->number == 16384);
        for (size_t n = 0; n < sizeof(variable); ++n)
            assert(!flacFrameOrigin(variable, n));
        auto bad = Bytes(variable, variable + sizeof(variable));
        bad.back() ^= 1;
        assert(!flacFrameOrigin(bad.data(), bad.size()));
        bad[4] = 0x80; // Invalid initial continuation byte.
        assert(!flacFrameOrigin(bad.data(), bad.size()));
        std::cout << "PASS FLAC wire origin: fixed/variable numbering, truncated headers and CRC rejection\n";
    }
    using detail::floatToFrame;
    assert(floatToFrame(0.0f) == 0 && floatToFrame(-0.0f) == 0);
    assert(floatToFrame(0.5f) == 1073741824 && floatToFrame(-0.5f) == -1073741824);
    const float half = std::ldexp(1.0f, -32);
    assert(floatToFrame(half) == 1 && floatToFrame(-half) == -1);
    assert(floatToFrame(3 * half) == 2 && floatToFrame(-3 * half) == -2);
    assert(floatToFrame(std::nextafter(half, 0.0f)) == 0);
    assert(floatToFrame(std::nextafter(-half, 0.0f)) == 0);
    assert(floatToFrame(std::nextafter(1.0f, 0.0f)) == 2147483520);
    assert(floatToFrame(std::nextafter(-1.0f, 0.0f)) == -2147483520);
    for (float x : {1.0f, std::nextafter(1.0f, 2.0f), 1.5f, std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::infinity()}) {
        assert(floatToFrame(x) == INT32_MAX);
        assert(floatToFrame(-x) == INT32_MIN);
    }
    assert(floatToFrame(std::numeric_limits<float>::quiet_NaN()) == 0);
    std::cout << "PASS float PCM: nearest rounding, ties away from zero, full-scale/overs saturation, NaN "
                 "silence\n";

    Ring<int> ring(17);
    std::deque<int> reference;
    std::mt19937 random(17);
    for (int k = 0; k < 10000; ++k) {
        int values[24];
        size_t n = random() % 24;
        if (random() % 2) {
            for (size_t i = 0; i < n; ++i)
                values[i] = random();
            size_t accepted = ring.push(values, n);
            assert(accepted == std::min(n, 17 - reference.size()));
            for (size_t i = 0; i < accepted; ++i)
                reference.push_back(values[i]);
        } else {
            size_t removed = ring.pop(values, n);
            assert(removed == std::min(n, reference.size()));
            for (size_t i = 0; i < removed; ++i) {
                assert(values[i] == reference.front());
                reference.pop_front();
            }
        }
        assert(ring.size() == reference.size());
        for (size_t i = 0; i < reference.size(); ++i)
            assert(ring.at(i) == reference[i]);
    }
    ring.clear();
    assert(ring.free() == 17);
    bool rejected = false;
    try {
        Ring<int> invalid(0);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
    rejected = false;
    try {
        ring.discard(1);
    } catch (const std::out_of_range &) {
        rejected = true;
    }
    assert(rejected);
    std::cout << "PASS ring: randomized wraparound, full capacity, partial operations, bounds\n";
    auto h = hello({2, 1, 2, 3, 4, 5}, true, 0x123456789abcdef0ULL, 48000);
    assert(std::string(h.begin(), h.begin() + 4) == "HELO" && be32(h.data() + 4) == h.size() - 8);
    assert(h[8] == 12 && be16(h.data() + 32) == 0x4000 && be32(h.data() + 34) == 0x12345678 &&
           be32(h.data() + 38) == 0x9abcdef0);
    Status s;
    s.streamSize = 4096;
    s.streamFull = 123;
    s.received = 0x123456789ULL;
    s.jiffies = 0xfedcba98;
    s.outputSize = 8000;
    s.outputFull = 400;
    s.elapsed = 12345;
    auto stat = statusPacket("STMt", s, 0x1234abcd);
    assert(stat.size() == 61 && be32(stat.data() + 4) == 53 && be32(stat.data() + 15) == 4096 &&
           be32(stat.data() + 19) == 123);
    assert(be32(stat.data() + 37) == 8000 && be32(stat.data() + 41) == 400 && be32(stat.data() + 45) == 12);
    assert(be32(stat.data() + 51) == 12345 && be32(stat.data() + 55) == 0x1234abcd);
    ServerParser parser;
    Bytes input = {0, 5, 's', 'e', 't', 'd', 0, 0, 4, 'a', 'u', 'd', 'e'};
    size_t count = 0;
    for (auto byte : input)
        for (auto &msg : parser.feed(&byte, 1)) {
            ++count;
            assert(msg.size() == (count == 1 ? 5 : 4));
        }
    assert(count == 2);
    rejected = false;
    try {
        uint8_t bad[] = {0, 3, 0, 0, 0};
        parser.feed(bad, 5);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
    assert(due(10, 0xfffffff0) && !due(0xfffffff0, 10));
    uint8_t reply[] = {'E', 'N', 'A', 'M', 'E', 2, 'O', 'K'};
    assert(discoveryReply(reply, sizeof(reply)));
    assert(!discoveryReply(reply, sizeof(reply) - 1));
    std::cout << "PASS protocol: HELO, STAT offsets, timestamp, fragmented framing, malformed lengths, "
                 "discovery, jiffies wrap\n";
    for (unsigned bits : {8, 16, 24, 32})
        for (bool big : {false, true})
            for (unsigned channels : {1, 2}) {
                Format f{48000, bits, channels, big};
                uint8_t data[8]{};
                data[big ? 0 : bits / 8 - 1] = 0x80;
                auto decoded = decodeFrame(data, f);
                assert(decoded.left == INT32_MIN);
                assert(decoded.right == (channels == 1 ? INT32_MIN : 0));
                std::memset(data, 0xff, sizeof(data));
                decoded = decodeFrame(data, f);
                assert(decoded.left == int32_t(-int64_t(1ULL << (32 - bits))));
            }
    assert(pcmFormat('2', '4', '1', '0', 48000).bits == 24);
    rejected = false;
    try {
        pcmFormat('1', '9', '2', '1', 48000);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
    auto loud = crossFrame({INT32_MAX, INT32_MIN}, {INT32_MAX, INT32_MIN}, 131072, 131072, 1, 2);
    assert(loud.left == INT32_MAX && loud.right == INT32_MIN);
    auto tiny = processFrame({-1, 1}, 32768);
    assert(tiny.left == -1 && tiny.right == 0);
    assert(rampGain(0, 4, true) == 0 && rampGain(1, 4, true) == 16384);
    assert(rampGain(3, 4, false) == 16384 && rampGain(4, 4, true) == 65536);
    std::cout << "PASS DSP: signed fixed-point floor, saturating mix, linear envelope endpoints\n";
    std::cout << "PASS PCM: 8/16/24/32 bits, both byte orders, mono/stereo, signed extremes, maximum rate\n";
}
