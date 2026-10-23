// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "decoder.h"
#include <mutex>
namespace yeney {
struct DecimationTail;
struct DecimationChain {
    std::mutex mutex;
    std::weak_ptr<DecimationTail> previous;
    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        previous.reset();
    }
};
std::unique_ptr<Decoder> decimatingDecoder(std::unique_ptr<Decoder>, const DecoderConfig &);
} // namespace yeney
