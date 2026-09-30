// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "sinks.h"
#include <stdexcept>
#include <vector>
namespace yeney {
namespace {
void le16(std::ostream &out, uint16_t v) {
    out.put(v);
    out.put(v >> 8);
}
void le32(std::ostream &out, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        out.put(v >> (8 * i));
}
void header(std::ostream &out, uint32_t rate, uint32_t bytes) {
    out.write("RIFF", 4);
    le32(out, 36 + bytes);
    out.write("WAVEfmt ", 8);
    le32(out, 16);
    le16(out, 1);
    le16(out, 2);
    le32(out, rate);
    le32(out, rate * 8);
    le16(out, 8);
    le16(out, 32);
    out.write("data", 4);
    le32(out, bytes);
}
} // namespace
void WavSink::finish() {
    if (file_.is_open()) {
        file_.seekp(0);
        header(file_, rate_, bytes_);
        file_.close();
        if (file_.fail())
            throw std::runtime_error("cannot finalize WAV");
    }
}
WavSink::~WavSink() {
    try {
        finish();
    } catch (...) {
    }
}
void WavSink::trackBoundary(uint64_t, const Format &f, bool) {
    if (file_.is_open() && rate_ == f.rate)
        return;
    finish();
    rate_ = f.rate;
    bytes_ = 0;
    std::string path = path_ + (segment_ ? "." + std::to_string(segment_) + ".wav" : "");
    ++segment_;
    file_.clear();
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_)
        throw std::runtime_error("cannot open WAV: " + path);
    header(file_, rate_, 0);
}
size_t WavSink::write(const Frame *p, size_t n) {
    if (bytes_ + n * 8 > UINT32_MAX - 36)
        throw std::runtime_error("WAV exceeds RIFF size limit");
    for (size_t i = 0; i < n; ++i) {
        le32(file_, uint32_t(p[i].left));
        le32(file_, uint32_t(p[i].right));
    }
    if (!file_)
        throw std::runtime_error("WAV write failed");
    bytes_ += n * 8;
    frames_ += n;
    return n;
}
} // namespace yeney
