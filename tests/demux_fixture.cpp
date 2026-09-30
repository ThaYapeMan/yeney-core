// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "core/mp4.h"
#include <fstream>
#include <iostream>
#include <random>
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    try {
        uint64_t packets = 0, bytes = 0;
        yeney::Mp4Demuxer demux(
            [](const yeney::Mp4Info &h) {
                std::cout << "TRIM " << h.totalFrames << ' ' << h.trimBegin << ' ' << h.trimEnd << '\n';
            },
            [&](const yeney::Bytes &p) {
                ++packets;
                bytes += p.size();
            },
            argc > 2 ? std::stoull(argv[2]) : 256ull * 1024 * 1024);
        std::ifstream file(argv[1], std::ios::binary);
        if (!file)
            throw std::runtime_error("file open");
        std::mt19937 random(argc > 3 ? std::stoul(argv[3]) : 42);
        uint8_t data[65536];
        // First bytes arrive individually, then random 1..64 KiB pieces.
        for (unsigned step = 0;; ++step) {
            size_t n = step < 32 ? 1 : 1 + random() % sizeof(data);
            file.read(reinterpret_cast<char *>(data), n);
            n = file.gcount();
            if (!n)
                break;
            demux.feed(data, n);
        }
        demux.finish();
        std::cout << "PACKETS " << packets << " BYTES " << bytes << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
