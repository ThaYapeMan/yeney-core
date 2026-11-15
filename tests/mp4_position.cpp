// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decoder.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iterator>
#include <thread>
// Media stays in memory, including private fixture inputs and decoded samples.
int main(int argc, char **argv) {
    yeney::Bytes bytes((std::istreambuf_iterator<char>(std::cin)), {});
    yeney::DecoderConfig config;
    config.codec = argc > 1 && std::string(argv[1]) == "aac" ? 'a' : 'l';
    if (argc > 1 && (std::string(argv[1]) == "flac" || std::string(argv[1]) == "convert-flac"))
        config.codec = 'f';
    if (bytes.size() >= 4 && !std::memcmp(bytes.data(), "fLaC", 4))
        config.codec = 'f';
    config.maxRate = 192000;
    config.finitePcmContainers = true;
    if (argc > 1 && (std::string(argv[1]) == "convert" || std::string(argv[1]) == "convert-flac"))
        config.convertToRate = 48000;
    if (argc > 1 && std::string(argv[1]).rfind("prefetch", 0) == 0) {
        size_t cursor = 0;
        while (cursor + 8 <= bytes.size()) {
            uint64_t length = yeney::be32(bytes.data() + cursor);
            size_t header = 8;
            if (length == 1) {
                if (cursor + 16 > bytes.size())
                    return 5;
                length = uint64_t(yeney::be32(bytes.data() + cursor + 8)) << 32 |
                         yeney::be32(bytes.data() + cursor + 12);
                header = 16;
            }
            if (length < header || length > bytes.size() - cursor)
                return 5;
            if (!std::memcmp(bytes.data() + cursor + 4, "moov", 4)) {
                yeney::Bytes movie(bytes.begin() + cursor + header, bytes.begin() + cursor + length);
                config.mp4Metadata = [movie] { return movie; };
                if (std::string(argv[1]) == "prefetch-mismatch")
                    bytes[cursor + size_t(length) - 1] ^= 1;
                if (std::string(argv[1]) == "prefetch-truncated")
                    bytes.resize(cursor);
                break;
            }
            cursor += size_t(length);
        }
    }
    const bool full = argc > 2;
    auto decoder = yeney::makeDecoder(config);
    size_t offset = 0;
    std::vector<yeney::Frame> first;
    uint64_t count = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (full || count < 4096) {
        if (std::chrono::steady_clock::now() > deadline)
            return 3;
        offset += decoder->feed(bytes.data() + offset, bytes.size() - offset);
        if (offset == bytes.size())
            decoder->finish();
        yeney::Frame block[256];
        auto n = decoder->take(block, full ? 256 : std::min(uint64_t(256), 4096 - count));
        count += n;
        first.insert(first.end(), block, block + (full ? n : std::min(n, size_t(4096) - first.size())));
        if (!decoder->error().empty() || decoder->done())
            break;
        if (!n)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    uint64_t start = UINT64_MAX;
    const uint64_t known = decoder->sourceStart(start), failed = !decoder->error().empty();
    std::cerr << "warmup=" << decoder->warmupFrames() << "\n";
    for (uint64_t v : {known, start, count, failed})
        for (unsigned j = 0; j < 8; ++j)
            std::cout.put(v >> (j * 8));
    std::cout.write(reinterpret_cast<const char *>(first.data()), first.size() * sizeof(yeney::Frame));
    if (failed)
        std::cerr << decoder->error() << '\n';
}
