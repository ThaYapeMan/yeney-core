// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "core/protocol.h"
#include "core/ring.h"
#include "core/transitions.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <random>
using namespace yeney;
int main() {
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
