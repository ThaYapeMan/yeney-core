// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decoder.h"
#include <chrono>
#include <iostream>
#include <iterator>
#include <thread>
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    const std::string mode = argv[1];
    yeney::DecoderConfig config;
    config.codec = 'm';
#ifdef YENEY_MP3_ORIGIN_API
    config.mp3FromStart = [mode] { return mode == "from-start"; };
#endif
    yeney::Bytes bytes((std::istreambuf_iterator<char>(std::cin)), {});
    auto decoder = yeney::makeDecoder(config);
    std::vector<yeney::Frame> pcm;
    size_t offset = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!decoder->done() && (mode != "endless" || pcm.size() < 4096)) {
        if (std::chrono::steady_clock::now() > deadline)
            return 3;
        offset += decoder->feed(bytes.data() + offset, bytes.size() - offset);
        if (offset == bytes.size() && mode != "endless")
            decoder->finish();
        yeney::Frame block[256];
        auto n = decoder->take(block, 256);
        pcm.insert(pcm.end(), block, block + n);
        if (!decoder->error().empty())
            break;
        if (!n)
            std::this_thread::yield();
    }
    uint64_t origin = 0;
    const uint64_t known = decoder->sourceStart(origin);
    auto exact = decoder->exactFrames();
    for (uint64_t value : {known, origin, uint64_t(pcm.size()), exact.value_or(UINT64_MAX),
                           uint64_t(!decoder->error().empty())})
        for (unsigned shift : {0, 8, 16, 24, 32, 40, 48, 56})
            std::cout.put(char(value >> shift));
    std::cout.write(reinterpret_cast<const char *>(pcm.data()), pcm.size() * sizeof(yeney::Frame));
}
