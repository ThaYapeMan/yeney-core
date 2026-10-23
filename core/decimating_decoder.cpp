// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "decimating_decoder.h"
#include "decimator.h"
#include "ring.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
namespace yeney {
struct DecimationTail {
    std::mutex mutex;
    uint32_t rate = 0;
    uint64_t total = 0, budget = 0, phase = 0;
    size_t needed = 0;
    bool linked = false, final = false;
    std::vector<Frame> history, future;
};
namespace {
class Converted : public Decoder {
    std::unique_ptr<Decoder> raw;
    DecoderConfig config;
    mutable std::mutex mutex;
    std::condition_variable changed;
    Ring<Frame> output{outputCapacity};
    std::thread worker;
    std::atomic_bool cancel{false}, waiting{false}, requested{false};
    bool finished = false, haveFormat = false, converted = false;
    Format convertedFormat;
    std::optional<uint64_t> exact;
    std::string failure;
    std::shared_ptr<DecimationTail> tail = std::make_shared<DecimationTail>(), previous;
    void emit(const Frame *p, size_t n) {
        while (n && !cancel) {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(2), [&] { return cancel || output.free(); });
            const auto accepted = output.push(p, n);
            p += accepted;
            n -= accepted;
        }
    }
    void run() {
        try {
            Format source;
            while (!cancel && !raw->format(source)) {
                if (!raw->error().empty() || raw->done())
                    throw std::runtime_error(raw->error());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (cancel)
                return;
            const auto target = Decimator::targetRate(source.rate, config.convertToRate);
            converted = target != source.rate;
            uint64_t phase = 0;
            std::vector<Frame> prefix;
            if (config.conversion) {
                std::lock_guard<std::mutex> chainLock(config.conversion->mutex);
                previous = config.conversion->previous.lock();
                if (previous) {
                    auto keepAlive = previous;
                    std::lock_guard<std::mutex> lock(keepAlive->mutex);
                    if (converted && config.gaplessSource && previous->rate == source.rate &&
                        !previous->final) {
                        prefix = previous->history;
                        phase = previous->phase + previous->budget * (source.rate / target) - previous->total;
                        previous->linked = true;
                    } else {
                        previous->final = true;
                        previous.reset();
                    }
                }
                tail->rate = source.rate;
                tail->phase = phase;
                if (converted)
                    config.conversion->previous = tail;
                else
                    config.conversion->previous.reset();
            }
            std::unique_ptr<Decimator> fir;
            if (converted)
                fir = std::make_unique<Decimator>(source.rate, phase, prefix);
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto promised = raw->exactFrames();
                if (promised)
                    exact = (*promised + (fir ? fir->factor() : 1) - 1) / (fir ? fir->factor() : 1);
                convertedFormat = source;
                convertedFormat.rate = target;
                if (converted)
                    convertedFormat.bits = 32;
                haveFormat = true;
            }
            if (fir && config.flacLog)
                config.flacLog("decimation source=" + std::to_string(source.rate) + " target=" +
                               std::to_string(target) + " factor=" + std::to_string(fir->factor()) +
                               " taps=" + std::to_string(fir->coefficients().size()));
            Frame block[1024], ready[1024];
            uint64_t received = 0;
            std::vector<Frame> history;
            bool ended = false, padded = false;
            size_t futureUsed = 0;
            std::chrono::steady_clock::time_point deadline{};
            while (!cancel) {
                bool progress = false;
                if (!ended) {
                    auto count = raw->take(block, 1024);
                    if (count) {
                        progress = true;
                        received += count;
                        if (previous) {
                            std::lock_guard<std::mutex> lock(previous->mutex);
                            auto n = std::min(count, previous->needed - previous->future.size());
                            previous->future.insert(previous->future.end(), block, block + n);
                        }
                        if (fir) {
                            fir->push(block, count);
                            history.insert(history.end(), block, block + count);
                            if (history.size() > fir->delayFrames())
                                history.erase(history.begin(), history.end() - fir->delayFrames());
                        } else
                            emit(block, count);
                    }
                    auto countPromise = raw->exactFrames();
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (countPromise)
                            exact =
                                (*countPromise + (fir ? fir->factor() : 1) - 1) / (fir ? fir->factor() : 1);
                    }
                    if (raw->done()) {
                        if (!raw->error().empty())
                            throw std::runtime_error(raw->error());
                        ended = true;
                        if (previous) {
                            std::lock_guard<std::mutex> lock(previous->mutex);
                            if (previous->future.size() < previous->needed)
                                previous->final = true;
                        }
                        if (fir) {
                            const auto budget = (received + fir->factor() - 1) / fir->factor();
                            fir->end(budget);
                            std::lock_guard<std::mutex> lock(tail->mutex);
                            tail->total = received;
                            tail->budget = budget;
                            tail->history = history;
                            tail->needed = fir->delayFrames() + size_t(phase) + fir->factor();
                            // Unknown counts keep sealed publication; tiny clips cannot
                            // establish a speaker start before their entire FIR tail.
                            tail->final =
                                !config.expectSuccessor || !countPromise || received <= fir->delayFrames();
                            waiting = !tail->final;
                        }
                    }
                }
                if (fir) {
                    if (ended && !padded) {
                        std::lock_guard<std::mutex> lock(tail->mutex);
                        if (requested && deadline == std::chrono::steady_clock::time_point{})
                            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        if (!tail->linked && deadline != std::chrono::steady_clock::time_point{} &&
                            std::chrono::steady_clock::now() >= deadline)
                            tail->final = true;
                        if (tail->future.size() > futureUsed) {
                            fir->lookahead(tail->future.data() + futureUsed,
                                           tail->future.size() - futureUsed);
                            futureUsed = tail->future.size();
                        }
                        if (tail->final) {
                            fir->pad();
                            padded = true;
                            waiting = false;
                        } else if (futureUsed >= tail->needed)
                            waiting = false;
                    }
                    size_t n;
                    while (!cancel && (n = fir->take(ready, 1024))) {
                        progress = true;
                        emit(ready, n);
                    }
                }
                if (ended && (!fir || fir->done()))
                    break;
                if (!progress)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (fir && fir->clips() && config.flacLog)
                config.flacLog("decimation clipping guard frames=" + std::to_string(fir->clips()));
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(mutex);
            failure = e.what();
        }
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }

public:
    Converted(std::unique_ptr<Decoder> d, DecoderConfig c) : raw(std::move(d)), config(std::move(c)) {
        worker = std::thread([this] { run(); });
    }
    ~Converted() override {
        cancel = true;
        changed.notify_all();
        worker.join();
    }
    size_t feed(const uint8_t *p, size_t n) override { return raw->feed(p, n); }
    void finish() override { raw->finish(); }
    size_t take(Frame *p, size_t n) override {
        std::lock_guard<std::mutex> lock(mutex);
        auto count = output.pop(p, n);
        changed.notify_all();
        return count;
    }
    bool format(Format &f) const override {
        std::lock_guard<std::mutex> lock(mutex);
        f = convertedFormat;
        return haveFormat;
    }
    std::optional<uint64_t> exactFrames() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return exact;
    }
    bool done() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return finished && !output.size();
    }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return failure;
    }
    size_t inputBuffered() const override { return raw->inputBuffered(); }
    size_t outputBuffered() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return output.size();
    }
    bool needsSuccessor() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return waiting && !output.size();
    }
    void successorRequested() override { requested = true; }
    bool processed() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return converted;
    }
};
} // namespace
std::unique_ptr<Decoder> decimatingDecoder(std::unique_ptr<Decoder> raw, const DecoderConfig &config) {
    return std::make_unique<Converted>(std::move(raw), config);
}
} // namespace yeney
