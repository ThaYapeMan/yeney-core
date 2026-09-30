// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "core/decoder.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
int main(int argc, char **argv) {
    if (argc < 4)
        return 2;
    try {
        yeney::DecoderConfig cfg;
        cfg.codec = argv[1][0];
        if (argc > 4)
            cfg.earlyMediaCap = std::stoull(argv[4]);
        auto decoder = yeney::makeDecoder(cfg);
        std::ifstream in(argv[2], std::ios::binary);
        std::ofstream out(argv[3], std::ios::binary);
        if (!in || !out)
            throw std::runtime_error("fixture file open");
        std::mt19937 random(42);
        yeney::Bytes pending;
        size_t at = 0;
        bool eof = false;
        uint64_t frames = 0;
        bool printed = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (true) {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("decoder timeout");
            if (at == pending.size() && !eof) {
                pending.resize(1 + random() % 65536);
                in.read(reinterpret_cast<char *>(pending.data()), pending.size());
                pending.resize(in.gcount());
                at = 0;
                eof = in.eof();
            }
            at += decoder->feed(pending.empty() ? nullptr : pending.data() + at, pending.size() - at);
            if (eof && at == pending.size())
                decoder->finish();
            auto error = decoder->error();
            if (!error.empty())
                throw std::runtime_error(error);
            yeney::Format f;
            if (!printed && decoder->format(f)) {
                std::cout << "FORMAT " << f.rate << ' ' << f.bits << ' ' << f.channels << '\n';
                printed = true;
            }
            yeney::Frame data[1024];
            auto n = decoder->take(data, 1024);
            frames += n;
            for (size_t i = 0; i < n; ++i)
                for (uint32_t v : {uint32_t(data[i].left), uint32_t(data[i].right)})
                    for (unsigned j = 0; j < 4; ++j)
                        out.put(v >> (j * 8));
            if (decoder->done())
                break;
            if (!n)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        std::cout << "FRAMES " << frames << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
