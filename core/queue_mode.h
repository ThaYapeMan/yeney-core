// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#pragma once
#include "player.h"
#include <deque>
#include <map>
namespace yeney::detail {
struct PlaybackTrack;
struct Overlap;
// Borrowed playback state only: queue policy has no sockets, parser, HTTP,
// outbound bytes or control over the event-loop cadence.
struct PlaybackBuffer {
    size_t &reservedWindow;
    Overlap &overlap;
    std::deque<std::shared_ptr<PlaybackTrack>> &output, &boundaries;
    std::shared_ptr<PlaybackTrack> &fetching, &audible;
    size_t &queued;
    uint64_t &submitted;
    bool &paused, &playing, &userPaused, &ended;
    uint32_t &wake;
    double &credit;
};

struct QueueTrackState {
    TrackDescriptor descriptor;
    bool described = false, itemEnded = false, startReported = false, doneReported = false, failed = false;
    uint32_t decodedAt = 0, audibleAt = 0;
};
// Internal policy hooks. Sink capability is queried at the original call sites,
// never cached: a room generation handoff can select a different sink at runtime.
class QueueMode {
    using Track = PlaybackTrack;
    Config &cfg;
    Sink &sink;
    PlaybackBuffer state;

public:
    struct Hooks {
        std::function<void(const std::string &)> log, stat;
        std::function<void()> reset;
    };

private:
    Hooks hooks;
    Bytes lastStart;
    uint64_t nextStartCredits = 0;
    uint64_t occurrence = 0, transitionOverlap = 0, modeGeneration = 0;
    std::string transitionReason = "gapless";
    std::shared_ptr<Track> lastItem;
    std::map<uint64_t, std::shared_ptr<Track>> retainedTracks;
    std::deque<std::shared_ptr<Track>> faults;
    void log(const std::string &s) { hooks.log(s); }
    void stat(const std::string &s) { hooks.stat(s); }
    double queuedSeconds() const;
    double futureSeconds() const;
    void itemEnd(const std::shared_ptr<Track> &, uint64_t, uint64_t, const std::string &);
    uint32_t successorDeadline(const Track &) const;

public:
    QueueMode(Config &, Sink &, PlaybackBuffer, Hooks);
    ~QueueMode();
    std::optional<size_t> outputCapacity() const;
    std::optional<uint32_t> elapsed() const;
    bool remoteClock() const;
    bool defersDecoderErrors() const;
    void validate() const;
    void reset();
    void streamFault(const std::string &);
    void markFailed(Track &);
    bool deferFault();
    void configureDecoder(Track &, DecoderConfig &);
    void commandObserved(char);
    bool repeatedStart(const Bytes &);
    bool remoteCommand(char, uint32_t);
    bool flush();
    bool beforeStart();
    void startTrack(Track &);
    void trackIntent(Track &, char);
    void describe(Track &);
    size_t decodeRoom(const Track &, size_t) const;
    bool decodeComplete(Track &, std::string &);
    void decoded(Track &);
    void reportDecoded(Track &);
    bool audibleEvents();
    void beforeBoundary(const std::shared_ptr<Track> &);
    void afterBoundary(const std::shared_ptr<Track> &);
    bool waitOverlapHeader(const Track &, const Track &) const;
    bool sameAlbum(const Track &, const Track &) const;
    void transitionRejected(const Track &, const Track &, bool);
    bool waitOverlapPcm(const Track &) const;
    void insufficientIncoming();
    void crossfade(uint64_t);
    bool waitSuccessor(const Track &) const;
    void outputEnded(const std::shared_ptr<Track> &);
    bool mayDrain() const;
    void beginRun();
    void pollGeneration();
};
} // namespace yeney::detail
