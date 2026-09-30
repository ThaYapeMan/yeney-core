// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "decoder.h"
#include "mp4.h"
#include "ring.h"
#include "third_party/alac/codec/ALACBitUtilities.h"
#include "third_party/alac/codec/ALACDecoder.h"
#include <FLAC/stream_decoder.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "third_party/minimp3/minimp3.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace yeney {
namespace {
struct Cancelled {};
class Worker : public Decoder {
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Ring<uint8_t> bytes_{inputCapacity};
    Ring<Frame> frames_{outputCapacity};
    std::thread thread_;
    bool eof_ = false, cancel_ = false, done_ = false, haveFormat_ = false;
    Format format_;
    std::string error_;

protected:
    DecoderConfig cfg;
    explicit Worker(DecoderConfig c) : cfg(c) {}
    virtual void decode() = 0;
    void start() {
        thread_ = std::thread([this] {
            try {
                decode();
            } catch (const Cancelled &) {
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = e.what();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                done_ = true;
            }
            cv_.notify_all();
        });
    }
    void cancel() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancel_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }
    size_t read(uint8_t *p, size_t n) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return cancel_ || eof_ || bytes_.size(); });
        if (cancel_)
            throw Cancelled{};
        size_t count = bytes_.pop(p, n);
        lock.unlock();
        cv_.notify_all();
        return count;
    }
    bool exact(uint8_t *p, size_t n, bool allowEnd = false) {
        size_t got = 0;
        while (got < n) {
            size_t count = read(p + got, n - got);
            if (!count) {
                if (!got && allowEnd)
                    return false;
                throw std::runtime_error("truncated codec input");
            }
            got += count;
        }
        return true;
    }
    void setFormat(Format f) {
        if (!f.rate || f.rate > cfg.maxRate || f.channels < 1 || f.channels > 2)
            throw std::runtime_error("unsupported rate or more than 2 channels");
        std::lock_guard<std::mutex> lock(mutex_);
        if (haveFormat_ &&
            (f.rate != format_.rate || f.channels != format_.channels || f.bits != format_.bits))
            throw std::runtime_error("format changes within a track are unsupported");
        format_ = f;
        haveFormat_ = true;
    }
    void emit(Frame f) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return cancel_ || frames_.free(); });
        if (cancel_)
            throw Cancelled{};
        frames_.push(&f, 1);
    }

public:
    ~Worker() override { cancel(); }
    size_t feed(const uint8_t *p, size_t n) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (eof_ || done_)
            return 0;
        auto count = bytes_.push(p, n);
        cv_.notify_all();
        return count;
    }
    void finish() override {
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = true;
        cv_.notify_all();
    }
    size_t take(Frame *p, size_t n) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto count = frames_.pop(p, n);
        cv_.notify_all();
        return count;
    }
    bool format(Format &f) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        f = format_;
        return haveFormat_;
    }
    bool done() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return done_ && !frames_.size();
    }
    std::string error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }
    size_t inputBuffered() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_.size();
    }
    size_t outputBuffered() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }
};
class PcmWorker : public Worker {
    Ring<uint8_t> input{32768};
    struct Track {
        Format format;
        bool headerPcm = false, known = false, wave = false, eof = false;
    };
    bool containerHeader(Track &t);
    void decode() override {
        Track t;
        t.headerPcm = cfg.codec == 'a' || cfg.size == '?' || cfg.rate == '?' || cfg.channels == '?' ||
                      cfg.endian == '?';
        if (!t.headerPcm) {
            t.format = pcmFormat(cfg.size, cfg.rate, cfg.channels, cfg.endian, cfg.maxRate);
            t.known = true;
        }
        uint8_t bytes[4096];
        // LMS advertises aif but sends it through wire codec p. Container
        // detection must therefore also work when strm supplies known PCM fields.
        if (!t.headerPcm) {
            uint8_t prefix[12];
            size_t got = 0;
            while (got < sizeof(prefix)) {
                size_t n = read(prefix + got, sizeof(prefix) - got);
                if (!n) {
                    t.eof = true;
                    break;
                }
                got += n;
            }
            input.push(prefix, got);
            t.headerPcm =
                got == 12 && ((!std::memcmp(prefix, "RIFF", 4) && !std::memcmp(prefix + 8, "WAVE", 4)) ||
                              (!std::memcmp(prefix, "FORM", 4) &&
                               (!std::memcmp(prefix + 8, "AIFF", 4) || !std::memcmp(prefix + 8, "AIFC", 4))));
        }
        while (t.headerPcm) {
            if (containerHeader(t))
                break;
            size_t n = read(bytes, std::min(sizeof(bytes), input.free()));
            if (!n)
                t.eof = true;
            else
                input.push(bytes, n);
        }
        setFormat(t.format);
        unsigned width = t.format.bits / 8 * t.format.channels;
        Bytes pending;
        while (input.size()) {
            uint8_t byte;
            input.pop(&byte, 1);
            pending.push_back(byte);
        }
        bool end = t.eof;
        while (true) {
            size_t used = 0;
            while (pending.size() - used >= width) {
                auto f = decodeFrame(pending.data() + used, t.format);
                used += width;
                if (t.wave && t.format.bits == 8) {
                    f.left = int32_t(uint32_t(f.left) ^ 0x80000000u);
                    f.right = int32_t(uint32_t(f.right) ^ 0x80000000u);
                }
                emit(f);
            }
            pending.erase(pending.begin(), pending.begin() + used);
            if (end) {
                if (!pending.empty())
                    throw std::runtime_error("partial PCM frame at EOF");
                break;
            }
            size_t n = read(bytes, sizeof(bytes));
            end = !n;
            pending.insert(pending.end(), bytes, bytes + n);
        }
    }

public:
    explicit PcmWorker(DecoderConfig c) : Worker(c) { start(); }
    ~PcmWorker() override { cancel(); }
};
bool PcmWorker::containerHeader(Track &t) {
    if (input.size() < 12) {
        if (t.eof)
            throw std::invalid_argument("truncated PCM container");
        return false;
    }
    auto tag = [&](size_t at, const char *id) {
        for (size_t i = 0; i < 4; ++i)
            if (input.at(at + i) != uint8_t(id[i]))
                return false;
        return true;
    };
    bool wave = tag(0, "RIFF") && tag(8, "WAVE");
    bool aiff = tag(0, "FORM") && (tag(8, "AIFF") || tag(8, "AIFC"));
    if (!wave && !aiff)
        throw std::invalid_argument("PCM '?' requires WAV or AIFF");
    auto number = [&](size_t at, unsigned width) {
        uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value = (value << 8) | input.at(at + (wave ? width - 1 - i : i));
        return value;
    };
    auto validate = [&](const Format &f) {
        if (f.channels < 1 || f.channels > 2 || !f.rate || f.rate > cfg.maxRate || f.bits < 8 ||
            f.bits > 32 || f.bits % 8)
            throw std::invalid_argument("unsupported PCM container format");
    };
    size_t pos = 12;
    bool found = false;
    Format f;
    while (input.size() >= pos + 8) {
        uint32_t length = number(pos + 4, 4);
        if (wave && tag(pos, "data")) {
            if (!found)
                throw std::invalid_argument("WAV data before fmt");
            input.discard(pos + 8);
            t.format = f;
            t.known = true;
            t.headerPcm = false;
            t.wave = true;
            // Streaming WAV lengths can be sentinel values, so transport EOF
            // remains authoritative, as it does for raw PCM.
            return true;
        }
        if (aiff && tag(pos, "SSND")) {
            if (!found || length < 8)
                throw std::invalid_argument("invalid AIFF sound chunk");
            if (input.size() < pos + 16)
                break;
            uint32_t offset = number(pos + 8, 4);
            if (offset > length - 8 || offset > input.capacity() - std::min(input.capacity(), pos + 16))
                throw std::invalid_argument("AIFF sound offset too large");
            if (input.size() < pos + 16 + offset)
                break;
            input.discard(pos + 16 + offset);
            t.format = f;
            t.known = true;
            t.headerPcm = false;
            return true;
        }
        if (length > input.capacity() - std::min(pos + 8, input.capacity()))
            throw std::invalid_argument("PCM container header exceeds ring capacity");
        if (input.size() < pos + 8 + length + (length & 1))
            break;
        if (wave && tag(pos, "fmt ")) {
            if (length < 16 || number(pos + 8, 2) != 1)
                throw std::invalid_argument("WAV must be integer PCM");
            f.channels = number(pos + 10, 2);
            f.rate = number(pos + 12, 4);
            f.bits = number(pos + 22, 2);
            f.bigEndian = false;
            if (number(pos + 20, 2) != f.channels * f.bits / 8)
                throw std::invalid_argument("WAV block alignment");
            validate(f);
            found = true;
        }
        if (aiff && tag(pos, "COMM")) {
            if (length < 18)
                throw std::invalid_argument("short AIFF common chunk");
            f.channels = number(pos + 8, 2);
            f.bits = number(pos + 14, 2);
            f.bigEndian = true;
            uint16_t exponent = number(pos + 16, 2);
            uint64_t mantissa = (uint64_t(number(pos + 18, 4)) << 32) | number(pos + 22, 4);
            double rate = std::ldexp(double(mantissa), int(exponent & 0x7fff) - 16383 - 63);
            if (exponent & 0x8000 || !std::isfinite(rate) || rate < 1 || rate > cfg.maxRate ||
                rate != std::floor(rate))
                throw std::invalid_argument("unsupported AIFF sample rate");
            f.rate = uint32_t(rate);
            if (tag(8, "AIFC")) {
                if (length < 22 || (!tag(pos + 26, "NONE") && !tag(pos + 26, "sowt")))
                    throw std::invalid_argument("compressed AIFC unsupported");
                f.bigEndian = !tag(pos + 26, "sowt");
            }
            validate(f);
            found = true;
        }
        pos += 8 + length + (length & 1);
    }
    if (t.eof)
        throw std::invalid_argument("truncated PCM container header");
    if (input.free() == 0)
        throw std::invalid_argument("PCM container header fills ring");
    return false;
}

class FlacWorker : public Worker {
    std::string callbackError;
    bool hadFrame = false;
    static FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder *, FLAC__byte *bytes, size_t *n,
                                                void *self) {
        try {
            *n = static_cast<FlacWorker *>(self)->read(bytes, *n);
            return *n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE
                      : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        } catch (...) {
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
    }
    static FLAC__StreamDecoderWriteStatus writeCb(const FLAC__StreamDecoder *, const FLAC__Frame *f,
                                                  const FLAC__int32 *const *samples, void *self) {
        auto &w = *static_cast<FlacWorker *>(self);
        try {
            Format format{f->header.sample_rate, f->header.bits_per_sample, f->header.channels, false};
            if (format.bits != 16 && format.bits != 24)
                throw std::runtime_error("FLAC requires 16 or 24 bits");
            w.setFormat(format);
            w.hadFrame = true;
            for (unsigned i = 0; i < f->header.blocksize; ++i)
                w.emit({int32_t(uint32_t(samples[0][i]) << (32 - format.bits)),
                        int32_t(uint32_t(samples[format.channels == 1 ? 0 : 1][i]) << (32 - format.bits))});
            return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        } catch (const std::exception &e) {
            w.callbackError = e.what();
        } catch (...) {
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    static void errorCb(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus status, void *self) {
        // Frame-aligned LMS seeks can omit STREAMINFO. Initial sync scanning is
        // allowed, but CRC/header errors or sync loss after audio are failures.
        auto &w = *static_cast<FlacWorker *>(self);
        if (status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC || w.hadFrame)
            w.callbackError = FLAC__StreamDecoderErrorStatusString[status];
    }
    void decode() override {
        auto *decoder = FLAC__stream_decoder_new();
        if (!decoder)
            throw std::runtime_error("libFLAC allocation");
        std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)> guard(
            decoder, FLAC__stream_decoder_delete);
        FLAC__stream_decoder_set_md5_checking(decoder, false);
        if (FLAC__stream_decoder_init_stream(decoder, readCb, nullptr, nullptr, nullptr, nullptr, writeCb,
                                             nullptr, errorCb, this) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
            throw std::runtime_error("libFLAC initialization");
        bool ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);
        FLAC__stream_decoder_finish(decoder);
        if (!callbackError.empty())
            throw std::runtime_error("FLAC: " + callbackError);
        if (!ok || !hadFrame)
            throw std::runtime_error("FLAC: no complete audio frames");
    }

public:
    explicit FlacWorker(DecoderConfig c) : Worker(c) { start(); }
    ~FlacWorker() override { cancel(); }
};
class Mp3Worker : public Worker {
    void decode() override {
        mp3dec_t decoder;
        mp3dec_init(&decoder);
        Bytes pending;
        uint8_t chunk[4096];
        bool eof = false, first = true, checkedId3 = false, hadAudio = false;
        uint64_t decoded = 0, begin = 0, end = UINT64_MAX;
        unsigned tailPadding = 0;
        std::deque<Frame> tail;
        while (true) {
            if (!eof && pending.size() < 16384) {
                size_t n = read(chunk, sizeof(chunk));
                eof = !n;
                pending.insert(pending.end(), chunk, chunk + n);
            }
            if (!checkedId3) {
                if (pending.size() < 10 && !eof)
                    continue;
                if (pending.size() >= 10 && !std::memcmp(pending.data(), "ID3", 3)) {
                    uint32_t length = 0;
                    for (size_t i = 6; i < 10; ++i) {
                        if (pending[i] & 128)
                            throw std::runtime_error("invalid ID3v2 size");
                        length = (length << 7) | pending[i];
                    }
                    uint64_t skip = uint64_t(length) + 10 + ((pending[5] & 16) ? 10 : 0);
                    size_t n = std::min<uint64_t>(skip, pending.size());
                    pending.erase(pending.begin(), pending.begin() + n);
                    skip -= n;
                    while (skip) {
                        size_t amount = read(chunk, std::min<uint64_t>(skip, sizeof(chunk)));
                        if (!amount)
                            throw std::runtime_error("truncated ID3v2");
                        skip -= amount;
                    }
                }
                checkedId3 = true;
            }
            if (pending.empty() && eof)
                break;
            mp3dec_frame_info_t info{};
            mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int frames = mp3dec_decode_frame(&decoder, pending.data(), pending.size(), pcm, &info);
            if (!info.frame_bytes) {
                if (eof) {
                    if (!pending.empty() &&
                        !(pending.size() == 128 && !std::memcmp(pending.data(), "TAG", 3)))
                        throw std::runtime_error("truncated MP3 frame");
                    break;
                }
                if (pending.size() >= 65536)
                    throw std::runtime_error("MP3 sync not found");
                size_t n = read(chunk, sizeof(chunk));
                eof = !n;
                pending.insert(pending.end(), chunk, chunk + n);
                continue;
            }
            bool tag = false;
            if (first && frames) {
                first = false;
                size_t h = info.frame_offset;
                auto *p = pending.data() + h;
                size_t size = info.frame_bytes - h;
                bool mpeg1 = (p[1] & 24) == 24;
                bool mono = (p[3] & 192) == 192;
                size_t at = 4 + ((p[1] & 1) ? 0 : 2) + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
                if (at + 8 <= size && (!std::memcmp(p + at, "Xing", 4) || !std::memcmp(p + at, "Info", 4))) {
                    tag = true;
                    uint32_t flags = be32(p + at + 4);
                    size_t pos = at + 8;
                    uint32_t count = 0;
                    for (auto field : {std::pair<uint32_t, size_t>{1, 4}, {2, 4}, {4, 100}, {8, 4}})
                        if (flags & field.first) {
                            if (field.second > size - pos)
                                throw std::runtime_error("truncated Xing tag");
                            if (field.first == 1)
                                count = be32(p + pos);
                            pos += field.second;
                        }
                    if (pos + 24 <= size && p[pos]) {
                        unsigned delay = (unsigned(p[pos + 21]) << 4) | (p[pos + 22] >> 4);
                        unsigned padding = ((p[pos + 22] & 15) << 8) | p[pos + 23];
                        begin = delay + 529;
                        tailPadding = padding > 529 ? padding - 529 : 0;
                        if (count) {
                            uint64_t total = uint64_t(count) * (mpeg1 ? 1152 : 576);
                            if (begin + tailPadding > total)
                                throw std::runtime_error("invalid MP3 gapless range");
                            end = total - tailPadding;
                        }
                    }
                }
            }
            if (frames && !tag) {
                Format format{uint32_t(info.hz), 16, unsigned(info.channels), false};
                setFormat(format);
                hadAudio = true;
                for (int i = 0; i < frames; ++i, ++decoded)
                    if (decoded >= begin && decoded < end) {
                        Frame f{int32_t(pcm[i * info.channels]) * 65536,
                                int32_t(pcm[i * info.channels + (info.channels == 1 ? 0 : 1)]) * 65536};
                        if (end == UINT64_MAX && tailPadding) {
                            tail.push_back(f);
                            if (tail.size() > tailPadding) {
                                emit(tail.front());
                                tail.pop_front();
                            }
                        } else
                            emit(f);
                    }
            }
            pending.erase(pending.begin(), pending.begin() + info.frame_bytes);
        }
        if (!hadAudio)
            throw std::runtime_error("MP3: no audio frames");
        if (end != UINT64_MAX && decoded < end)
            throw std::runtime_error("MP3 ends before Xing sample count");
    }

public:
    explicit Mp3Worker(DecoderConfig c) : Worker(c) { start(); }
    ~Mp3Worker() override { cancel(); }
};
class AlacWorker : public Worker {
    void decode() override {
        ALACDecoder apple;
        Mp4Info info;
        uint64_t position = 0;
        std::vector<uint8_t> output;
        Mp4Demuxer demux(
            [&](const Mp4Info &h) {
                info = h;
                if (info.format.channels > 2)
                    throw std::runtime_error("ALAC: more than 2 channels unsupported");
                setFormat(info.format);
                if (apple.Init(info.cookie.data(), info.cookie.size()))
                    throw std::runtime_error("ALAC cookie initialization failed");
                output.resize(size_t(info.framesPerPacket) * info.format.channels * 4);
            },
            [&](const Bytes &packet) {
                // Apple performs word lookahead. Padding is outside the advertised
                // packet byte count and never forms an extra compressed packet.
                // Validate the first audio element's declared frame count before
                // handing it to Apple's decoder, whose partial count overrides
                // the caller's frame count. This protects its fixed work buffers.
                auto field = [&](size_t at, unsigned count) {
                    if (at + count > packet.size() * 8)
                        throw std::runtime_error("truncated ALAC packet header");
                    uint32_t value = 0;
                    for (unsigned j = 0; j < count; ++j)
                        value = (value << 1) | ((packet[(at + j) / 8] >> (7 - (at + j) % 8)) & 1);
                    return value;
                };
                unsigned element = field(0, 3);
                if (element != (info.format.channels == 1 ? 0u : 1u) || field(7, 12) != 0 ||
                    field(20, 2) == 3)
                    throw std::runtime_error("unsupported ALAC packet element");
                if (field(19, 1)) {
                    uint32_t count = field(23, 32);
                    if (!count || count > info.framesPerPacket)
                        throw std::runtime_error("ALAC partial frame count exceeds cookie");
                }
                Bytes padded = packet;
                padded.resize(packet.size() + 64, 0);
                BitBuffer bits{};
                BitBufferInit(&bits, padded.data(), packet.size());
                uint32_t frames = 0;
                if (apple.Decode(&bits, output.data(), info.framesPerPacket, info.format.channels, &frames) ||
                    frames > info.framesPerPacket)
                    throw std::runtime_error("ALAC packet decode failed");
                unsigned storageBits = info.format.bits == 20 ? 24 : info.format.bits;
                Format storage{info.format.rate, storageBits, info.format.channels, false};
                for (uint32_t i = 0; i < frames; ++i, ++position)
                    if (position >= info.trimBegin && position < info.trimEnd)
                        emit(decodeFrame(output.data() + size_t(i) * info.format.channels * (storageBits / 8),
                                         storage));
            },
            cfg.earlyMediaCap);
        uint8_t bytes[8192];
        while (size_t n = read(bytes, sizeof(bytes)))
            demux.feed(bytes, n);
        demux.finish();
        if (position < info.trimEnd)
            throw std::runtime_error("ALAC packets end before declared trim range");
    }

public:
    explicit AlacWorker(DecoderConfig c) : Worker(c) { start(); }
    ~AlacWorker() override { cancel(); }
};
} // namespace
std::unique_ptr<Decoder> makeDecoder(const DecoderConfig &c) {
    switch (c.codec) {
    case 'p':
    case 'a':
        return std::make_unique<PcmWorker>(c);
    case 'f':
        return std::make_unique<FlacWorker>(c);
    case 'm':
        return std::make_unique<Mp3Worker>(c);
    case 'l':
        return std::make_unique<AlacWorker>(c);
    default:
        throw std::runtime_error("unsupported codec");
    }
}
} // namespace yeney
