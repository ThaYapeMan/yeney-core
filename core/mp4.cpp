// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "mp4.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace yeney {
namespace {
constexpr size_t metadataCap = 32 * 1024 * 1024, tableCap = 1000000, packetCap = 16 * 1024 * 1024;
[[noreturn]] void bad(const std::string &what) { throw std::runtime_error("MP4: " + what); }
uint64_t add(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a)
        bad("length overflow");
    return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
    if (a && b > UINT64_MAX / a)
        bad("table duration overflow");
    return a * b;
}
struct View {
    const uint8_t *p;
    size_t n;
    void need(size_t at, size_t count) const {
        if (at > n || count > n - at)
            bad("truncated box or table");
    }
    uint32_t u32(size_t at) const {
        need(at, 4);
        return be32(p + at);
    }
    uint64_t u64(size_t at) const { return (uint64_t(u32(at)) << 32) | u32(at + 4); }
    uint16_t u16(size_t at) const {
        need(at, 2);
        return be16(p + at);
    }
    View sub(size_t at, size_t count) const {
        need(at, count);
        return {p + at, count};
    }
    bool tag(size_t at, const char *s) const {
        need(at, 4);
        return !std::memcmp(p + at, s, 4);
    }
};
struct Box {
    std::string type;
    View data;
};
std::vector<Box> boxes(View v, size_t depth = 0) {
    if (depth > 32)
        bad("box nesting exceeds limit");
    std::vector<Box> out;
    for (size_t at = 0; at < v.n;) {
        v.need(at, 8);
        uint64_t length = v.u32(at);
        size_t header = 8;
        if (length == 1) {
            length = v.u64(at + 8);
            header = 16;
        }
        if (length == 0)
            length = v.n - at;
        if (length < header || length > v.n - at)
            bad("box length exceeds parent");
        out.push_back({std::string(reinterpret_cast<const char *>(v.p + at + 4), 4),
                       v.sub(at + header, length - header)});
        if (out.size() > tableCap)
            bad("too many boxes");
        at += size_t(length);
    }
    return out;
}
uint32_t count(View v, size_t width, size_t prefix = 8) {
    v.need(0, prefix);
    uint32_t n = v.u32(4);
    if (n > tableCap || n > (v.n - prefix) / width)
        bad("table count exceeds box");
    return n;
}
struct Track {
    bool audio = false;
    uint32_t timescale = 0, description = 0;
    Bytes cookie;
    std::vector<uint32_t> sizes;
    std::vector<uint64_t> chunks;
    struct Run {
        uint32_t first, samples, description;
    };
    std::vector<Run> runs;
    uint64_t ticks = 0, timedPackets = 0;
    struct Edit {
        uint64_t duration;
        int64_t time;
    };
    std::vector<Edit> edits;
    bool haveStsz = false, haveStts = false, haveStsc = false, haveOffsets = false, haveStsd = false;
};
void unique(bool &flag) {
    if (flag)
        bad("duplicate sample table");
    flag = true;
}
void cookieBoxes(View v, Bytes &cookie, size_t depth = 0) {
    for (auto b : boxes(v, depth)) {
        if (b.type == "alac") {
            b.data.need(0, 28);
            if (!cookie.empty())
                bad("duplicate ALAC cookie");
            cookie.assign(b.data.p + 4, b.data.p + 28);
        } else if (b.type == "wave")
            cookieBoxes(b.data, cookie, depth + 1);
    }
}
void trackBoxes(View v, Track &t, size_t depth = 0) {
    for (auto b : boxes(v, depth)) {
        auto d = b.data;
        if (b.type == "mdia" || b.type == "minf" || b.type == "stbl" || b.type == "edts")
            trackBoxes(d, t, depth + 1);
        else if (b.type == "hdlr") {
            d.need(0, 24);
            t.audio = d.tag(8, "soun");
        } else if (b.type == "mdhd") {
            d.need(0, 4);
            if (d.p[0] > 1)
                bad("mdhd version");
            d.need(0, d.p[0] ? 36 : 24);
            t.timescale = d.u32(d.p[0] ? 20 : 12);
        } else if (b.type == "stsd") {
            unique(t.haveStsd);
            d.need(0, 8);
            auto entries = boxes(d.sub(8, d.n - 8), depth + 1);
            if (d.u32(4) != entries.size())
                bad("stsd count mismatch");
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].type == "alac" && !t.description) {
                    auto e = entries[i].data;
                    e.need(0, 28);
                    auto version = e.u16(8);
                    if (version > 1)
                        bad("unsupported audio sample entry version");
                    size_t start = version ? 44 : 28;
                    e.need(0, start);
                    cookieBoxes(e.sub(start, e.n - start), t.cookie, depth + 1);
                    t.description = i + 1;
                }
        } else if (b.type == "stsz") {
            unique(t.haveStsz);
            d.need(0, 12);
            uint32_t size = d.u32(4), n = d.u32(8);
            if (n > tableCap || (!size && n > (d.n - 12) / 4))
                bad("stsz count exceeds box");
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t value = size ? size : d.u32(12 + size_t(i) * 4);
                if (!value || value > packetCap)
                    bad("invalid sample size");
                t.sizes.push_back(value);
            }
        } else if (b.type == "stco" || b.type == "co64") {
            unique(t.haveOffsets);
            size_t width = b.type == "co64" ? 8 : 4;
            uint32_t n = count(d, width);
            for (uint32_t i = 0; i < n; ++i)
                t.chunks.push_back(width == 8 ? d.u64(8 + size_t(i) * 8) : d.u32(8 + size_t(i) * 4));
        } else if (b.type == "stsc") {
            unique(t.haveStsc);
            uint32_t n = count(d, 12);
            for (uint32_t i = 0; i < n; ++i) {
                size_t at = 8 + size_t(i) * 12;
                Track::Run r{d.u32(at), d.u32(at + 4), d.u32(at + 8)};
                if (!r.first || !r.samples || !r.description || (!i && r.first != 1) ||
                    (i && r.first <= t.runs.back().first))
                    bad("invalid stsc run");
                t.runs.push_back(r);
            }
        } else if (b.type == "stts") {
            unique(t.haveStts);
            uint32_t n = count(d, 8);
            for (uint32_t i = 0; i < n; ++i) {
                size_t at = 8 + size_t(i) * 8;
                uint32_t samples = d.u32(at), delta = d.u32(at + 4);
                if (!samples || !delta)
                    bad("invalid stts entry");
                t.ticks = add(t.ticks, mul(samples, delta));
                t.timedPackets = add(t.timedPackets, samples);
            }
        } else if (b.type == "elst") {
            if (!t.edits.empty())
                bad("duplicate elst");
            d.need(0, 8);
            if (d.p[0] > 1)
                bad("elst version");
            bool wide = d.p[0] == 1;
            uint32_t n = count(d, wide ? 20 : 12);
            for (uint32_t i = 0; i < n; ++i) {
                size_t at = 8 + size_t(i) * (wide ? 20 : 12);
                uint64_t duration = wide ? d.u64(at) : d.u32(at);
                uint64_t time = wide ? d.u64(at + 8) : uint64_t(int64_t(int32_t(d.u32(at + 4))));
                int64_t signedTime;
                std::memcpy(&signedTime, &time, 8);
                if (d.u32(at + (wide ? 16 : 8)) != 0x10000 || signedTime < -1)
                    bad("unsupported edit rate or time");
                t.edits.push_back({duration, signedTime});
            }
        }
    }
}
void metadata(View v, std::string &smpb, size_t depth = 0) {
    for (auto b : boxes(v, depth)) {
        if (b.type == "udta" || b.type == "ilst")
            metadata(b.data, smpb, depth + 1);
        else if (b.type == "meta") {
            b.data.need(0, 4);
            metadata(b.data.sub(4, b.data.n - 4), smpb, depth + 1);
        } else if (b.type == "----") {
            bool named = false;
            std::string value;
            for (auto child : boxes(b.data, depth + 1)) {
                if (child.type == "name") {
                    child.data.need(0, 4);
                    named = std::string(reinterpret_cast<const char *>(child.data.p + 4), child.data.n - 4) ==
                            "iTunSMPB";
                }
                if (child.type == "data") {
                    child.data.need(0, 8);
                    value.assign(reinterpret_cast<const char *>(child.data.p + 8), child.data.n - 8);
                }
            }
            if (named) {
                if (!smpb.empty())
                    bad("duplicate iTunSMPB");
                smpb = value;
            }
        }
    }
}
} // namespace
struct Mp4Demuxer::Impl {
    Header onHeader;
    Packet onPacket;
    size_t cap;
    Bytes header, payload, packet;
    std::string type;
    uint64_t offset = 0, remaining = 0;
    bool active = false, toEnd = false, selected = false, finished = false;
    struct Sample {
        uint64_t offset;
        uint32_t bytes;
    };
    std::vector<Sample> samples;
    size_t next = 0;
    struct Media {
        uint64_t start;
        Bytes bytes;
    };
    std::vector<Media> early;
    size_t earlyBytes = 0;
    Impl(Header h, Packet p, size_t c) : onHeader(std::move(h)), onPacket(std::move(p)), cap(c) {}
    void movie() {
        if (selected)
            bad("multiple moov boxes");
        View v{payload.data(), payload.size()};
        std::vector<Track> tracks;
        uint32_t movieScale = 0;
        std::string smpb;
        for (auto b : boxes(v)) {
            if (b.type == "trak") {
                Track t;
                trackBoxes(b.data, t);
                tracks.push_back(std::move(t));
            } else if (b.type == "mvhd") {
                b.data.need(0, 4);
                if (b.data.p[0] > 1)
                    bad("mvhd version");
                b.data.need(0, b.data.p[0] ? 112 : 100);
                movieScale = b.data.u32(b.data.p[0] ? 20 : 12);
            }
        }
        metadata(v, smpb);
        auto it = std::find_if(tracks.begin(), tracks.end(),
                               [](const Track &t) { return t.audio && t.description; });
        if (it == tracks.end())
            bad("no ALAC audio track");
        auto &t = *it;
        if (t.cookie.size() != 24)
            bad("selected ALAC sample entry has no magic cookie");
        if (!t.haveStsz || !t.haveStts || !t.haveStsc || !t.haveOffsets || !t.timescale ||
            t.timedPackets != t.sizes.size())
            bad("incomplete or inconsistent sample tables");
        Mp4Info info;
        info.cookie = t.cookie;
        info.framesPerPacket = be32(t.cookie.data());
        info.format.bits = t.cookie[5];
        info.format.channels = t.cookie[9];
        info.format.rate = be32(t.cookie.data() + 20);
        if (t.cookie[4] != 0 || t.cookie[8] > 31 || !info.framesPerPacket || info.framesPerPacket > 65536 ||
            !info.format.rate || !info.format.channels || info.format.channels > 8 ||
            (info.format.bits != 16 && info.format.bits != 20 && info.format.bits != 24 &&
             info.format.bits != 32))
            bad("invalid ALAC cookie");
        info.totalFrames = mul(t.ticks, info.format.rate) / t.timescale;
        info.trimEnd = info.totalFrames;
        size_t sample = 0, run = 0;
        uint64_t previousEnd = 0;
        if (t.chunks.empty() != t.sizes.empty() || (!t.chunks.empty() && t.runs.empty()))
            bad("missing chunks or stsc runs");
        if (!t.runs.empty() && t.runs.back().first > t.chunks.size())
            bad("stsc run exceeds chunks");
        for (size_t chunk = 0; chunk < t.chunks.size(); ++chunk) {
            while (run + 1 < t.runs.size() && t.runs[run + 1].first <= chunk + 1)
                ++run;
            const auto &r = t.runs[run];
            if (r.description != t.description)
                bad("stsc selects a different sample description");
            if (r.samples > t.sizes.size() - sample)
                bad("stsc sample count exceeds stsz");
            uint64_t at = t.chunks[chunk];
            if (chunk && at < previousEnd)
                bad("backwards or overlapping chunk offsets");
            for (uint32_t j = 0; j < r.samples; ++j) {
                uint32_t n = t.sizes[sample++];
                samples.push_back({at, n});
                at = add(at, n);
            }
            previousEnd = at;
        }
        if (sample != t.sizes.size())
            bad("stsc does not cover stsz");
        bool edit = false;
        for (auto e : t.edits)
            if (e.time >= 0) {
                if (edit || !movieScale)
                    bad("multiple media edits or missing movie timescale");
                edit = true;
                info.trimBegin =
                    std::min(info.totalFrames, mul(uint64_t(e.time), info.format.rate) / t.timescale);
                info.trimEnd = std::min(info.totalFrames,
                                        add(info.trimBegin, mul(e.duration, info.format.rate) / movieScale));
            }
        if (!edit && !smpb.empty()) {
            uint64_t reserved, delay, padding;
            std::istringstream text(smpb);
            text >> std::hex >> reserved >> delay >> padding;
            if (!text)
                bad("invalid iTunSMPB");
            info.trimBegin = std::min(info.totalFrames, delay);
            info.trimEnd = std::max(info.trimBegin, info.totalFrames - std::min(info.totalFrames, padding));
        }
        selected = true;
        onHeader(info);
        // These ranges contain only previously buffered mdat payload, never moov.
        for (auto &media : early)
            mediaData(media.bytes.data(), media.bytes.size(), media.start);
        early.clear();
        earlyBytes = 0;
    }
    void mediaData(const uint8_t *p, size_t n, uint64_t at) {
        if (!selected) {
            if (n > cap - earlyBytes)
                bad("mdat-before-moov exceeds configured buffering cap");
            if (early.empty() || add(early.back().start, early.back().bytes.size()) != at)
                early.push_back({at, {}});
            early.back().bytes.insert(early.back().bytes.end(), p, p + n);
            earlyBytes += n;
            return;
        }
        while (n && next < samples.size()) {
            auto s = samples[next];
            uint64_t wanted = add(s.offset, packet.size());
            if (wanted < at)
                bad("sample offset lies outside media or requires backwards access");
            if (wanted > at) {
                size_t skip = size_t(std::min<uint64_t>(n, wanted - at));
                at += skip;
                p += skip;
                n -= skip;
                continue;
            }
            size_t take = std::min(n, size_t(s.bytes) - packet.size());
            packet.insert(packet.end(), p, p + take);
            at += take;
            p += take;
            n -= take;
            if (packet.size() == s.bytes) {
                onPacket(packet);
                packet.clear();
                ++next;
            }
        }
    }
    void closeBox() {
        if (type == "moov")
            movie();
        if (type == "mdat" && !packet.empty())
            bad("packet crosses mdat boundary");
        payload.clear();
        active = false;
        toEnd = false;
        type.clear();
    }
    void feed(const uint8_t *p, size_t n) {
        if (finished)
            bad("feed after finish");
        while (n) {
            if (!active) {
                size_t goal = header.size() < 8 ? 8 : (be32(header.data()) == 1 ? 16 : 8);
                size_t take = std::min(n, goal - header.size());
                header.insert(header.end(), p, p + take);
                offset = add(offset, take);
                p += take;
                n -= take;
                if (header.size() < goal)
                    continue;
                if (header.size() == 8 && be32(header.data()) == 1)
                    continue;
                uint64_t length = be32(header.data());
                if (length == 1)
                    length = (uint64_t(be32(header.data() + 8)) << 32) | be32(header.data() + 12);
                toEnd = length == 0;
                if (!toEnd && length < header.size())
                    bad("box smaller than its header");
                type.assign(reinterpret_cast<const char *>(header.data() + 4), 4);
                remaining = toEnd ? UINT64_MAX : length - header.size();
                if (type == "moov" && !toEnd && remaining > metadataCap)
                    bad("moov exceeds metadata cap");
                header.clear();
                active = true;
                if (!remaining) {
                    closeBox();
                    continue;
                }
            }
            size_t take = size_t(std::min<uint64_t>(n, remaining));
            if (type == "moov") {
                if (take > metadataCap - payload.size())
                    bad("moov exceeds metadata cap");
                payload.insert(payload.end(), p, p + take);
            } else if (type == "mdat")
                mediaData(p, take, offset);
            offset = add(offset, take);
            if (!toEnd)
                remaining -= take;
            p += take;
            n -= take;
            if (!remaining)
                closeBox();
        }
    }
    void finish() {
        if (finished)
            bad("duplicate finish");
        if (!header.empty() || (active && !toEnd))
            bad("truncated top-level box");
        if (active)
            closeBox();
        if (!selected || next != samples.size() || !packet.empty())
            bad("missing moov or truncated media packets");
        finished = true;
    }
};
Mp4Demuxer::Mp4Demuxer(Header h, Packet p, size_t cap)
    : impl_(std::make_unique<Impl>(std::move(h), std::move(p), cap)) {}
Mp4Demuxer::~Mp4Demuxer() = default;
void Mp4Demuxer::feed(const uint8_t *p, size_t n) { impl_->feed(p, n); }
void Mp4Demuxer::finish() { impl_->finish(); }
} // namespace yeney
