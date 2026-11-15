// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decoder.h"
#include <chrono>
#include <iostream>
#include <iterator>
#include <thread>
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6)
        return 2;
    yeney::DecoderConfig config;
    config.codec = 'p';
    config.size = argv[1][0];
    config.rate = argv[2][0];
    config.channels = argv[3][0];
    config.endian = argv[4][0];
    config.maxRate = 192000;
    if (argc == 6)
        config.pcmSourceStart = [] { return std::optional<uint64_t>(0); };
    yeney::Bytes bytes((std::istreambuf_iterator<char>(std::cin)), {});
    auto decoder = yeney::makeDecoder(config);
    size_t offset = 0, count = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (count < 4096 && !decoder->done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return 3;
        offset += decoder->feed(bytes.data() + offset, bytes.size() - offset);
        if (offset == bytes.size())
            decoder->finish();
        yeney::Frame frames[256];
        count += decoder->take(frames, 256);
        if (!decoder->error().empty())
            return 4;
        std::this_thread::yield();
    }
    uint64_t frame = 0;
    bool known = decoder->sourceStart(frame);
    std::cout << "{\"known\":" << known << ",\"frame\":" << frame << "}\n";
}
