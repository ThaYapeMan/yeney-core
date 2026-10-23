// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decimating_decoder.h"
#include "core/decimator.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
using namespace yeney;
static Bytes wave(const std::vector<Frame> &frames, uint32_t rate) {
    Bytes bytes;
    auto word = [&](uint32_t v, unsigned n) {
        for (unsigned i = 0; i < n; ++i)
            bytes.push_back(uint8_t(v >> (8 * i)));
    };
    auto tag = [&](const char *p) { bytes.insert(bytes.end(), p, p + 4); };
    tag("RIFF");
    word(36 + frames.size() * 8, 4);
    tag("WAVE");
    tag("fmt ");
    word(16, 4);
    word(1, 2);
    word(2, 2);
    word(rate, 4);
    word(rate * 8, 4);
    word(8, 2);
    word(32, 2);
    tag("data");
    word(frames.size() * 8, 4);
    for (auto f : frames) {
        word(uint32_t(f.left), 4);
        word(uint32_t(f.right), 4);
    }
    return bytes;
}
int main() {
    for (auto rate : {88200u, 96000u, 176400u, 192000u}) {
        const auto target = Decimator::targetRate(rate), factor = rate / target;
        auto chain = std::make_shared<DecimationChain>();
        std::vector<std::unique_ptr<Decoder>> decoders;
        std::vector<std::vector<Frame>> sources(4), outputs(4);
        std::vector<Frame> joined;
        for (unsigned track = 0; track < 4; ++track) {
            for (unsigned i = 0; i < 4003 + track; ++i) {
                auto n = joined.size();
                sources[track].push_back(
                    {int32_t((n * 1234567) % 1000000000) & ~255, int32_t((n * 7654321) % 1000000000) & ~255});
                joined.push_back(sources[track].back());
            }
            DecoderConfig c;
            c.codec = 'a';
            c.maxRate = 192000;
            c.convertToRate = 48000;
            c.conversion = chain;
            c.finitePcmContainers = true;
            c.gaplessSource = track != 0;
            c.expectSuccessor = track < 3;
            decoders.push_back(makeDecoder(c));
            auto input = wave(sources[track], rate);
            size_t sent = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (true) {
                auto &d = *decoders[track];
                sent += d.feed(input.data() + sent, input.size() - sent);
                if (sent == input.size())
                    d.finish();
                for (unsigned j = 0; j <= track; ++j) {
                    Frame block[2048];
                    auto n = decoders[j]->take(block, 2048);
                    outputs[j].insert(outputs[j].end(), block, block + n);
                    assert(decoders[j]->error().empty());
                }
                if (sent == input.size() && (track < 3 ? d.needsSuccessor() : d.done())) {
                    if (track < 3)
                        d.successorRequested();
                    break;
                }
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        for (size_t j = 0; j < decoders.size(); ++j) {
            Frame block[1024];
            size_t n;
            while ((n = decoders[j]->take(block, 1024)))
                outputs[j].insert(outputs[j].end(), block, block + n);
        }
        uint64_t total = 0;
        for (unsigned i = 0; i < 4; ++i) {
            auto expected = (sources[i].size() + factor - 1) / factor;
            assert(outputs[i].size() == expected);
            assert(decoders[i]->exactFrames() == expected);
            Format f;
            assert(decoders[i]->format(f) && f.rate == target);
            total += expected;
        }
        Decimator reference(rate);
        reference.push(joined.data(), joined.size());
        reference.end(total);
        reference.pad();
        std::vector<Frame> expected(total);
        assert(reference.take(expected.data(), total) == total);
        std::vector<Frame> actual;
        for (auto &part : outputs)
            actual.insert(actual.end(), part.begin(), part.end());
        assert(actual.size() == expected.size() &&
               std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(Frame)) == 0);
        std::cout << "PASS native decimation " << rate << " -> " << target
                  << ": early exact ceil counts, four-track odd-frame gapless continuity equals one FIR "
                     "stream, no missing/extra join frames\n";
    }
    for (unsigned n = 1; n < 20; ++n) {
        Decimator f(192000);
        std::vector<Frame> input(n, {1, 1});
        f.push(input.data(), n);
        f.end((n + 3) / 4);
        f.pad();
        Frame out[20];
        assert(f.take(out, 20) == (n + 3) / 4 && f.done());
    }
    std::cout << "PASS decimator short tails: exact ceil for every source length 1..19\n";
}
