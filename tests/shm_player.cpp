// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "apps/yeney-player/main.h"
// This fixture's generated raw PCM resources always begin at source frame zero.
// The production player has no such evidence and supplies no PCM origin.
int main(int argc, char **argv) {
    return runPlayer(argc, argv, [](yeney::Config &config) {
        config.decoderFactory = [](yeney::DecoderConfig decoder) {
            if (decoder.codec == 'p')
                decoder.pcmSourceStart = [] { return std::optional<uint64_t>(0); };
            return yeney::makeDecoder(decoder);
        };
    });
}
