// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "queue_mode.h"
#include "playback_state.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
namespace yeney::detail {
QueueMode::QueueMode(Config &c, Sink &s, PlaybackBuffer p, Hooks h)
    : cfg(c), sink(s), state(p), hooks(std::move(h)) {}
QueueMode::~QueueMode() = default;
void QueueMode::validate() const {
    if (!std::isfinite(cfg.futurePcmSeconds) || cfg.futurePcmSeconds < 1 || cfg.futurePcmSeconds > 600)
        throw std::invalid_argument("invalid future PCM seconds");
}
double QueueMode::queuedSeconds() const {
    auto &p = state;
    double seconds = 0;
    for (const auto &t : p.output)
        seconds += double(t->output.size()) / t->format.rate;
    return seconds;
}
double QueueMode::futureSeconds() const {
    auto &p = state;
    // Two bounded tail windows are mixer look-behind, not future PCM.
    double reserve = 0;
    unsigned count = 0;
    for (const auto &t : p.output) {
        if (++count > 2)
            break;
        reserve += double(std::min<uint64_t>(t->output.size(), cfg.transitionMaxFrames)) / t->format.rate;
    }
    return std::max(0.0, queuedSeconds() - reserve);
}
void QueueMode::itemEnd(const std::shared_ptr<Track> &t, uint64_t next, uint64_t overlapFrames,
                        const std::string &reason) {
    auto &p = state;
    if (!sink.decodeAhead() || !t || t->itemEnded)
        return;
    log("queue item-end occurrence=" + std::to_string(t->descriptor.occurrence) + " reason=" + reason);
    t->itemEnded = true;
    t->end = p.submitted;
    sink.resolvedItemBoundary({t->descriptor.occurrence, next, p.submitted, overlapFrames, reason});
}
void QueueMode::reportDecoded(Track &t) {
    if (!t.decoded || t.failed || t.doneReported ||
        (sink.decodeAhead() && (!t.startReported || selectionPending)))
        return;
    if (sink.decodeAhead()) {
        if (round <= t.startRound)
            return; // put STMs on the wire one event-loop round before STMd
        if (sink.speakerDriven() && cfg.queueRemoteSource && cfg.queueRemoteSource(t.descriptor.occurrence)) {
            const auto total = sink.committedItemFrames(t.descriptor.occurrence).value_or(t.produced);
            const auto audible = sink.audibleFrames();
            const auto played = audible > t.first ? audible - t.first : 0;
            const auto left = total > played ? total - played : 0;
            if (left * 1000 >= uint64_t(cfg.queueNextDelayMs) * t.format.rate)
                return;
        }
    }
    t.doneReported = true;
    if (sink.decodeAhead() && sink.speakerDriven())
        ++nextStartCredits;
    stat("STMd");
    if (sink.decodeAhead())
        sink.coreEvent("STMd", t.descriptor.occurrence, t.produced);
}
uint32_t QueueMode::successorDeadline(const Track &t) const {
    const auto frames = sink.committedItemFrames(t.descriptor.occurrence).value_or(t.produced);
    return t.audibleAt + uint32_t(std::max(0.0, double(frames) / t.format.rate * 1000 - 100));
}
bool QueueMode::waitSuccessor(const Track &t) const {
    auto &p = state;
    if (!sink.decodeAhead() || p.output.size() != 1 || t.transition != 1 || t.failed)
        return false;
    if (!t.decoded)
        return true;
    if (!t.startReported && !t.descriptor.sourceFrames && sink.awaitingPublication(t.descriptor.occurrence))
        return false;
    // The held tail must be resolved before its audible deadline. A short
    // first item keeps one frame to establish STMs before requesting more.
    const auto position = sink.audibleFrames();
    const auto end = t.first + t.produced;
    const auto remaining = end > position ? end - position : 0;
    return !t.startReported ||
           (remaining > std::max<uint64_t>(1, t.format.rate / 10) && !due(jiffies(), successorDeadline(t)));
}
bool QueueMode::audibleEvents() {
    auto &p = state;
    if (!sink.decodeAhead())
        return false;
    if (cachedConnectionPending)
        return true; // RESP must reach LMS before a cached speaker start
    // History keeps track identity/timing, not decoder or PCM buffers.
    for (auto &entry : retainedTracks)
        if (entry.second != p.fetching && entry.second->decoded && entry.second->output.empty())
            entry.second->decoder.reset();
    bool freshStart = false;
    AudibleStart signal;
    while (sink.takeAudibleStart(signal)) {
        auto retained = retainedTracks.find(signal.occurrence);
        if (sink.speakerDriven() && signal.confirmed && retained != retainedTracks.end() &&
            retained->second->first == signal.frame && retained->second->played) {
            if (p.audible == retained->second && !selectionPending)
                continue;
            if (selectionPending)
                retained->second->doneReported = false;
            selectionPending = false;
            p.playing = true;
            p.paused = p.ended = false;
            p.audible = retained->second;
            auto pending = std::find(p.boundaries.begin(), p.boundaries.end(), p.audible);
            if (pending != p.boundaries.end())
                p.boundaries.erase(p.boundaries.begin(), std::next(pending));
            p.audible->clockOriginMs = sink.audibleElapsedMs(signal.occurrence).value_or(0);
            freshStart = true;
            stat("STMs");
            sink.coreEvent("STMs", signal.occurrence, signal.frame);
            p.audible->startReported = true;
            p.audible->startRound = round;
            p.audible->audibleAt = jiffies();
            reportDecoded(*p.audible);
            continue;
        }
        auto it = std::find_if(p.boundaries.begin(), p.boundaries.end(),
                               [&](const auto &t) { return t->descriptor.occurrence == signal.occurrence; });
        if (it == p.boundaries.end() || (*it)->first != signal.frame || !(*it)->played ||
            (!signal.confirmed && signal.frame >= sink.audibleFrames())) {
            log("queue audible signal rejected");
            continue;
        }
        p.audible = *it;
        p.boundaries.erase(p.boundaries.begin(), std::next(it));
        p.audible->clockOriginMs = sink.audibleElapsedMs(signal.occurrence).value_or(0);
        freshStart = true;
        stat("STMs");
        sink.coreEvent("STMs", signal.occurrence, signal.frame);
        p.audible->startReported = true;
        p.audible->startRound = round;
        p.audible->audibleAt = jiffies();
        reportDecoded(*p.audible);
    }
    std::vector<AudibleState> states;
    AudibleState value;
    while (sink.speakerDriven() && sink.takeAudibleState(value))
        states.push_back(value);
    for (size_t index = 0; index < states.size(); ++index) {
        const auto &state = states[index];
        if (freshStart && std::any_of(states.begin() + index + 1, states.end(), [&](const auto &later) {
                return later.occurrence == state.occurrence;
            }))
            continue; // a confirmed new run supersedes its buffered old stop/pause
        if (!p.audible || state.occurrence != p.audible->descriptor.occurrence)
            continue;
        const bool wasPaused = p.paused;
        const bool wasPlaying = p.playing;
        p.paused = state.state == PlaybackState::Paused;
        p.playing = state.state != PlaybackState::Stopped;
        p.userPaused = p.paused;
        p.wake = 0;
        p.credit = 0;
        if (p.paused && !wasPaused)
            stat("STMp");
        else if (!p.playing && wasPlaying) {
            stat("STMu");
            p.ended = true;
        } else if (p.playing && !p.paused && (wasPaused || !wasPlaying)) {
            stat("STMr");
            p.ended = false;
        }
    }
    while (!faults.empty() && faults.front()->end != UINT64_MAX && sink.drained(faults.front()->end)) {
        // Received audio is a stream fault, never StreamingFailed/STMn.
        sink.coreEvent("fault-tail-drained", faults.front()->descriptor.occurrence, faults.front()->end);
        faults.pop_front();
    }
    if (p.audible)
        reportDecoded(*p.audible);
    return true;
}
std::optional<size_t> QueueMode::outputCapacity() const {
    if (sink.decodeAhead())
        return size_t(cfg.futurePcmSeconds * sink.maxSampleRate()) + cfg.transitionMaxFrames * 2;
    return {};
}
std::optional<uint32_t> QueueMode::elapsed() const {
    auto &p = state;
    if (sink.decodeAhead() && sink.speakerDriven()) {
        auto ms = sink.audibleElapsedMs(p.audible->descriptor.occurrence);
        if (ms && cfg.queueRelativeClock)
            *ms = *ms >= p.audible->clockOriginMs ? *ms - p.audible->clockOriginMs : 0;
        return ms;
    }
    return {};
}
bool QueueMode::remoteClock() const { return sink.decodeAhead() && sink.speakerDriven(); }
bool QueueMode::defersDecoderErrors() const { return sink.decodeAhead(); }
void QueueMode::reset() {
    selectionPending = cachedConnectionPending = false;
    resumedAcknowledged = receivedAcknowledged = false;
    retainedTracks.clear();
    faults.clear();
    lastItem.reset();
}
void QueueMode::streamFault(const std::string &why) {
    auto &p = state;
    if (sink.decodeAhead() && p.fetching)
        sink.coreEvent("fault:" + why, p.fetching->descriptor.occurrence, p.submitted);
}
void QueueMode::markFailed(Track &t) {
    if (sink.decodeAhead())
        t.failed = true;
}
bool QueueMode::deferFault() {
    auto &p = state;
    if (sink.decodeAhead() && p.fetching && p.fetching->produced) {
        p.fetching->failed = true;
        p.fetching->decodedAt = jiffies();
        faults.push_back(p.fetching);
        return true;
    }
    return false;
}
void QueueMode::configureDecoder(Track &t, DecoderConfig &c) {
    auto &p = state;
    t.descriptor.codec = c.codec;
    if (sink.decodeAhead())
        sink.coreEvent("codec:" + std::string(1, c.codec), t.descriptor.occurrence, p.submitted);
    c.finitePcmContainers = sink.decodeAhead();
    if (sink.decodeAhead() && cfg.flacSourceInfo) {
        const auto id = t.descriptor.occurrence;
        auto *config = &cfg;
        c.flacLog = [config, id](const std::string &text) {
            if (config->log)
                config->log("queue FLAC occurrence=" + std::to_string(id) + " " + text);
        };
        c.flacSourceInfo = [config, id](std::optional<FlacSourceInfo> info) {
            return config->flacSourceInfo(id, info);
        };
    }
    c.transcoded = cfg.sourceIsTranscoded && cfg.sourceIsTranscoded(t.descriptor.occurrence);
}
void QueueMode::receivedStartCommand(const Bytes &request) {
    if (sink.playbackGeneration() || sink.decodeAhead()) {
        receivedStart = request;
        receivedAcknowledged = false;
    }
}
Bytes QueueMode::takeStart() {
    if (sink.takeQueueStart()) {
        resumedAcknowledged = receivedAcknowledged;
        selectionPending = cachedConnectionPending = false;
        return std::exchange(receivedStart, {});
    }
    return {};
}
Bytes QueueMode::takeCachedStart() {
    if (sink.takeQueueCachedStart())
        return std::exchange(receivedStart, {});
    return {};
}
bool QueueMode::resumedStartAck() { return std::exchange(resumedAcknowledged, false); }
bool QueueMode::repeatedStart(const Bytes &request) {
    if (!sink.decodeAhead() || !sink.speakerDriven() || request.size() <= 28)
        return false;
    // LMS may reuse the exact HTTP endpoint/request for every song. Each
    // STMd authorizes a successor, even when its wire request is identical.
    const bool successor = nextStartCredits != 0;
    if (successor)
        --nextStartCredits;
    const bool repeat =
        !successor && request == lastStart && !state.ended && state.fetching && !state.fetching->failed;
    lastStart = request;
    if (repeat)
        log("queue repeated strm s: existing occurrence retained");
    return repeat;
}
void QueueMode::commandObserved(char c) {
    if (c == 'q' || c == 'f') {
        lastStart.clear();
        nextStartCredits = 0;
    }
    if (sink.decodeAhead())
        log("queue command strm " + std::string(1, c));
}
bool QueueMode::remoteCommand(char c, uint32_t value) {
    if (sink.decodeAhead() && sink.speakerDriven() && sink.queueCommand(c, value)) {
        if (c == 'q') {
            state.playing = state.paused = state.userPaused = false;
            state.ended = true;
        }
        if (c == 'u') {
            // Autostart=0 must release PCM production; the speaker still
            // supplies every audible start/state acknowledgement.
            state.playing = true;
            state.paused = state.userPaused = false;
            state.wake = 0;
        }
        if (c == 's') {
            // Cached audio still starts a new LMS streaming run. Acknowledge
            // its connection before a speaker STMs, without decoding it again.
            stat("STMf");
            stat("STMc");
            receivedAcknowledged = selectionPending = cachedConnectionPending = true;
        } else if (c == 'q' || c == 'f')
            stat("STMf");
        return true;
    }
    return false;
}
bool QueueMode::flush() {
    if (!sink.decodeAhead())
        return false;
    hooks.reset();
    stat("STMf");
    return true;
}
bool QueueMode::beforeStart() {
    auto &p = state;
    const bool refetch = sink.decodeAhead() && sink.speakerDriven() && sink.takeQueueRefetch();
    if (p.fetching && !p.fetching->decoded) {
        if (refetch) {
            // Keep immutable host assets, audible clock and cached track
            // identities. Only the superseded producer's PCM is discarded.
            for (const auto &t : p.output) {
                if (!t->decoded) {
                    sink.coreEvent("queue-refetch", t->descriptor.occurrence, p.submitted);
                    t->failed = true; // cancellation must not report STMd
                }
                t->output.clear();
                t->decoder.reset();
            }
            p.output.clear();
            p.fetching.reset();
            lastItem.reset();
            p.overlap = {};
            p.reservedWindow = p.queued = 0;
            transitionOverlap = 0;
        } else
            return false;
    }
    return true;
}
void QueueMode::startTrack(Track &t) {
    selectionPending = false;
    auto &p = state;
    t.descriptor.occurrence = ++occurrence;
    if (sink.decodeAhead())
        sink.coreEvent("strm", occurrence, p.submitted);
}
void QueueMode::trackIntent(Track &t, char codec) {
    t.descriptor.transition = t.transition;
    t.descriptor.period = t.period;
    t.descriptor.codec = codec;
}
void QueueMode::describe(Track &t) {
    if (sink.decodeAhead()) {
        const auto exact = t.decoder->exactFrames();
        if (!t.described || t.descriptor.sourceFrames != exact) {
            t.descriptor.transitionFrames = t.window;
            t.descriptor.rate = t.format.rate;
            t.descriptor.sourceFrames = exact;
            sink.trackDescriptor(t.descriptor);
            t.described = true;
        }
    }
}
size_t QueueMode::decodeRoom(const Track &t, size_t room) const {
    if (sink.decodeAhead())
        room = std::min(room, size_t(std::max(0.0, cfg.futurePcmSeconds - futureSeconds()) * t.format.rate));
    return room;
}
bool QueueMode::decodeComplete(Track &t, std::string &error) {
    auto &p = state;
    // Observe completion before its error: the worker publishes the
    // error before done. Reuse this snapshot below so a completion
    // racing this poll cannot bypass queue fault handling.
    const bool queueComplete = sink.decodeAhead() && t.decoder->done();
    if (queueComplete)
        error = t.decoder->error();
    if (queueComplete && !error.empty()) {
        const auto exact = t.decoder->exactFrames();
        if (t.produced && exact && *exact > t.produced && *exact - t.produced <= t.format.rate) {
            const auto shortfall = *exact - t.produced;
            t.output.insert(t.output.end(), shortfall, Frame{});
            p.queued += shortfall;
            t.produced += shortfall;
            const auto message = "source-short " + std::to_string(shortfall) + " frames";
            log(message);
            sink.coreEvent("fault:" + message, t.descriptor.occurrence, t.produced);
        } else
            throw std::runtime_error(error);
    }
    bool complete = sink.decodeAhead() ? queueComplete : t.decoder->done();
    if (sink.decodeAhead() && complete && !t.produced)
        throw std::runtime_error("source-empty");
    return complete;
}
void QueueMode::decoded(Track &t) {
    t.decodedAt = jiffies();
    if (sink.decodeAhead())
        sink.coreEvent("decode-complete", t.descriptor.occurrence, t.produced);
}
void QueueMode::beforeBoundary(const std::shared_ptr<Track> &t) {
    if (sink.decodeAhead()) {
        itemEnd(lastItem, t->descriptor.occurrence, transitionOverlap, transitionReason);
        lastItem = t;
        transitionOverlap = 0;
        transitionReason = "gapless";
    }
}
void QueueMode::afterBoundary(const std::shared_ptr<Track> &t) {
    if (sink.decodeAhead() && sink.speakerDriven()) {
        retainedTracks[t->descriptor.occurrence] = t;
        // Match the host queue cap and retain metadata only, never PCM.
        while (retainedTracks.size() > 60000)
            retainedTracks.erase(retainedTracks.begin());
    }
}
bool QueueMode::waitOverlapHeader(const Track &old, const Track &next) const {
    return sink.decodeAhead() && !next.described && old.transition == 1 &&
           !due(jiffies(), successorDeadline(old));
}
bool QueueMode::sameAlbum(const Track &old, const Track &next) const {
    return sink.decodeAhead() && cfg.smartSameAlbum &&
           cfg.smartSameAlbum(old.descriptor.occurrence, next.descriptor.occurrence);
}
void QueueMode::transitionRejected(const Track &old, const Track &next, bool smart) {
    if (sink.decodeAhead())
        transitionReason = smart                                 ? "smart-same-album"
                           : next.transition == 0                ? "transition-type-0"
                           : old.format.rate != next.format.rate ? "different-rates"
                                                                 : "envelope-only";
}
bool QueueMode::waitOverlapPcm(const Track &old) const {
    return sink.decodeAhead() && !due(jiffies(), successorDeadline(old));
}
void QueueMode::insufficientIncoming() {
    if (sink.decodeAhead())
        transitionReason = "insufficient-incoming-pcm";
}
void QueueMode::crossfade(uint64_t length) {
    transitionOverlap = length;
    transitionReason = "crossfade";
}
void QueueMode::outputEnded(const std::shared_ptr<Track> &t) {
    auto &p = state;
    if (!t->itemEnded)
        t->end = p.submitted;
    if (sink.decodeAhead()) {
        sink.coreEvent("decoder-track-end", t->descriptor.occurrence, p.submitted);
        if (!t->boundary)
            itemEnd(t, p.output.size() > 1 ? p.output[1]->descriptor.occurrence : 0, 0,
                    t->failed ? "source-fault" : "unplayed");
        else if (p.output.size() == 1)
            itemEnd(t, 0, 0,
                    t->transition == 0 ? "no-transition"
                    : !t->startReported && !t->descriptor.sourceFrames &&
                            sink.awaitingPublication(t->descriptor.occurrence)
                        ? "sealed-startup"
                        : "successor-deadline");
    }
}
bool QueueMode::mayDrain() const {
    auto &p = state;
    return !sink.speakerDriven() && (!sink.decodeAhead() || !p.fetching->failed);
}
void QueueMode::beginRun() { modeGeneration = sink.playbackGeneration(); }
void QueueMode::pollGeneration() {
    ++round;
    const auto mode = sink.playbackGeneration();
    if (mode != modeGeneration) {
        lastStart.clear();
        nextStartCredits = 0;
        hooks.reset();
        modeGeneration = mode;
        sink.playbackReady(mode);
    }
}
} // namespace yeney::detail
