// Copyright (c) 2026 Jaap van Vliet
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decoder.h"
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>
int main() {
    std::vector<yeney::Bytes> inputs;
    for (const char *path : {"tests/fixtures/gapless-0.mp3", "tests/fixtures/gapless-7013.mp3"}) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return 2;
        inputs.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    for (int trial = 1; trial <= 5; ++trial) {
        auto wall = std::chrono::steady_clock::now();
        auto cpu = std::clock();
        uint64_t frames = 0;
        for (int repeat = 0; repeat < 100; ++repeat)
            for (const auto &input : inputs) {
                yeney::DecoderConfig cfg;
                cfg.codec = 'm';
                auto decoder = yeney::makeDecoder(cfg);
                size_t at = 0;
                for (;;) {
                    at += decoder->feed(input.data() + at, input.size() - at);
                    if (at == input.size())
                        decoder->finish();
                    yeney::Frame block[4096];
                    auto n = decoder->take(block, 4096);
                    frames += n;
                    if (!decoder->error().empty())
                        return 3;
                    if (decoder->done())
                        break;
                    if (!n)
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        if (frames != 1501900)
            return 4;
        std::cout
            << "trial=" << trial << " frames=" << frames
            << " cpu_ms=" << 1000.0 * (std::clock() - cpu) / CLOCKS_PER_SEC << " wall_ms="
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall).count()
            << '\n';
    }
}
