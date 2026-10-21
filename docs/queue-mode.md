# Queue mode internals and extraction inventory

This is a behavior-preserving extraction from core `085c8c1`. The public
`Player`, `Config`, `Sink`, decoder and MP4 interfaces keep their existing names,
defaults and semantics. The bridge still supplies the same queue sink and
controller. Stream, WAV and SHM sinks keep their existing path and bytes.

## Inventory at 085c8c1

The following is the queue-only branch/state inventory, including compatibility
plumbing which must remain active when the sink changes back to stream.
Function names identify the original locations in `core/player.cpp` unless a
separate file is named.

| Original area | State and branches | Extracted owner |
|---|---|---|
| `outputCapacity`, `queuedSeconds`, `futureSeconds`, constructor | `futurePcmSeconds` 1–600 validation; decode-ahead capacity includes two transition windows; PCM seconds subtract at most two mixer look-behind windows before applying the future cap. Ordinary output-frame/transition caps stay unchanged. | `QueueMode::validate`, capacity/decode-room policy |
| `Track`, `start`, `format`, `decode` | Monotonic `occurrence`; `TrackDescriptor` codec/rate/source count/transition/period/window; `described`, `itemEnded`, `startReported`, `doneReported`, `failed`, `decodedAt`, `audibleAt`. Descriptors are resent when exact counts resolve. Source counts come from decoders, not LMS duration or HTTP byte offsets. | `QueueTrackState`, start/format/describe hooks |
| `format` | Queue capability sets `finitePcmContainers`; FLAC metadata resolver/log callbacks are attached before decoding. `sourceIsTranscoded` is deliberately still consulted outside queue mode, as before. | `QueueMode::configureDecoder` |
| `itemEnd`, `boundary`, `outputStep` | `lastItem`, `transitionReason`, `transitionOverlap`; exactly one resolved rendered boundary per item, including next occurrence, submitted frame and overlap. Empty/unplayed, source fault, no-transition, sealed-startup and successor-deadline reasons are preserved. | Boundary and output-ended hooks |
| `reportDecoded`, `audibleEvents` | Decode completion and audible start are separate. Queue STMd waits for STMs; `doneReported` prevents duplicates, failed items do not report successful decode. `coreEvent` strm/codec/decode-complete/STMs/STMd/decoder-track-end events preserve their positions relative to callbacks and STAT packets. | Decoded-report and evidence hooks |
| `elapsed`, `stat` | Speaker-driven elapsed overrides the cumulative frame calculation; playing queue STMs/STMt retain the 1 ms protocol interpolation seed without changing internal item zero. | Remote-clock hooks |
| `audibleEvents`, `boundary` | `retainedTracks` keeps at most 60000 occurrence identities and drops unused decoder state, never holding new PCM buffers. A confirmed cached item may start again; duplicate current signals are ignored. Starts require matching occurrence/first frame and played PCM; unconfirmed signals also require a positive audible tick. | `QueueMode::audibleEvents`, retention hook |
| `audibleEvents` | Speaker state is authoritative for pause/resume/stop. Occurrence checks precede updates to playing/paused/userPaused/wake/credit and STMp/STMr/STMu. HTTP reads and decoding do not generate these states. | Evidence hook |
| `command`, `start` | Command logging precedes the existing observer; remote `queueCommand` can consume s/p/u/q/f and q/f still acknowledge STMf. Unconsumed queue flush clears rather than using stream flush. | Command/flush hooks |
| `start` | One-shot `takeQueueRefetch` is queried only with both capabilities, even if no incomplete producer exists. An authorized refetch cancels only incomplete producer PCM/decoders, marks those tracks failed, clears overlap/reservation/last-item state and preserves audible assets/metadata. Unauthorized starts retain ordinary clear behavior. | `QueueMode::beforeStart` |
| `decode` | Completion is observed before its worker error. A nonempty source short by at most one native second is padded to its exact count and emits the same fault event. Empty sources and other errors fail; silence-only starts are not manufactured. `decodedAt` and produced counts are retained. | Decode-completion hook |
| `failStream`, `audibleEvents` | Fault notification precedes HTTP teardown. `faults` defers STMn for a source with produced PCM until its rendered end is known and the sink drains; empty faults report immediately. | Fault hooks and evidence hook |
| `prepareOverlap`, `waitSuccessor`, `successorDeadline` | Smart same-album decisions, type-0/envelope/different-rate/insufficient-PCM reasons, pending successor descriptors and incoming PCM waits. The deadline uses committed item frames (or produced frames), native rate and audibleAt minus 100 ms. Unknown sealed startup can resolve without a successor; short startup retains a frame to establish STMs. Common gain/envelope/mixing arithmetic is unchanged. | Overlap/wait policy hooks |
| `outputStep` | Speaker-driven state suppresses synthesized end-of-playback; failed queue fetches do not produce successful STMu. Existing stream draining and starvation stay in the shared output loop. | `QueueMode::mayDrain` |
| `clear`, authorized refetch | History, deferred faults and last-item references reset in the original order. Occurrence and transition diagnostics keep their original lifetime/reset behavior rather than gaining new resets. | Queue state reset hooks |
| `run` | `modeGeneration` is independent of decodeAhead: handoff must work after selecting stream. Each changed generation clears old producer/decoder/PCM first, sends the same capability HELO only when connected, then calls `playbackReady`. Default generation zero never switches. | `beginRun` / `pollGeneration` |
| `core/decoder.cpp` PCM worker | With `finitePcmContainers`, validated WAV/AIFF payload counts stop decoding at the declared end, discard container trailers, and reject short payloads. The >= pre-read and == post-frame checks remain distinct. Strict stream/container parsing is unchanged. | `QueueDecodePolicy` |
| `core/decoder.cpp` FLAC worker, `core/flac_header.h` | Queue resolver captures at most 64 initial bytes and resolves metadata before first output. Wire fixed-frame or variable-sample origin, original block size and total samples determine exact remaining count. Uncertain/invalid queue origins retain sealed fallback and existing logs. Strict native STREAMINFO counting/error handling remains available outside queue mode; exact-count publication still precedes logging. | `QueueFlacSource`, `queue_flac_origin.h` |
| `core/decoder.cpp` ALAC adapter, `core/mp4.cpp::finish` | Queue capability enables partial-media MP4 finish for a selected rewritten seek mdat or completed packet list. Complete packets survive; existing decoder source-short/empty handling decides the outcome. Default MP4 truncation checks and all table/trim arithmetic remain shared and strict. | `QueueDecodePolicy` |

`Config::queueStartupMs` remains an unchanged public compatibility field; the
core did not read it at the starting commit, so the extraction adds no timer.
FLAC remaining counts describe the actual seek stream. Source-time origins and
HTTP Range recovery remain host concerns; no Range-to-seek inference is added.

## Internal boundary

`queue_mode.h/.cpp` owns queue state and policy. `playback_state.h` contains only
the private track/mixer data types used by both the shared player and policy.
`PlaybackBuffer` is a non-owning reference view of audio/control state; it
exposes no sockets, HTTP headers, parser or outbound protocol buffer. The three
callbacks permit logging, STAT reporting and internal clear. It introduces no thread, buffer, timer or new I/O path.

Hooks follow the existing lifecycle stages: configuration/start, commands,
decode, boundary/overlap/output, speaker evidence, clear and generation handoff.
Capability reads are not cached: the same sink object can switch capabilities
at a generation handoff. STMd reporting keeps its original non-queue behavior
inside the adapter rather than duplicating it. Queue validation is still at the
original constructor point, including its behavior for non-queue Config values.

`queue_decode.h/.cpp` is the single decoder exception policy. FLAC source state
also retains strict native counting, which is part of the decoder contract in
both modes. Worker callbacks borrow the existing Config, not the policy object,
so Config and mutable callback state retain their lifetime. Public
`flacFrameOrigin` remains a header-only thin forwarder to the unchanged parser;
standalone bridge wire tests need no extra link dependency.

The shared player still owns networking, protocol framing, rings, decoder
workers, playback queues, generic DSP, pacing, starvation and stream flushing.
`Sink`, `Player`, `Config`, `DecoderConfig` and `Mp4Demuxer` public declarations
are unchanged. The bridge needs only a new core pin.

## Before/after proof

Existing tests and helpers are unchanged. `tests/queue_mode_compare.py` adds a
separate capture/compare driver using the existing loopback Session/SongQueue
helpers. Save the unchanged executable before rebuilding the extraction:

```sh
mkdir -p /tmp/yeney-core-before
cp test-player /tmp/yeney-core-before/test-player
PYTHONDONTWRITEBYTECODE=1 python3 tests/queue_mode_compare.py capture --binary /tmp/yeney-core-before/test-player --output /tmp/yeney-before-pcm
# Rebuild the refactored core, then:
PYTHONDONTWRITEBYTECODE=1 python3 tests/queue_mode_compare.py capture --binary ./test-player --output /tmp/yeney-after-pcm
python3 tests/queue_mode_compare.py compare /tmp/yeney-before-pcm /tmp/yeney-after-pcm
```

The 14 captures cover transition types 0–4 in normal and queue modes, gapless,
native rate changes, and queue boundaries. Comparison reads every PCM byte and
also compares structural boundary callbacks; SHA-256 and byte counts are printed
for review. Wall-clock timestamps are deliberately excluded. The existing tests
continue to check ordering, elapsed clocks, faults, controls and wire behavior.
Full bridge `make test` runs the full core suite as its final nested target.
Set `LAMPASTREAM_CHECKOUT` explicitly and use `PYTHONDONTWRITEBYTECODE=1` so the
real read-only LampaStream consumer cannot be silently skipped.

Extraction size (lines, including comments and blanks): `core/player.cpp` moves
from 1247 to 987. New internal files are `queue_mode.h` (100), `queue_mode.cpp`
(380), `playback_state.h` (30), `queue_decode.h` (36), `queue_decode.cpp` (62),
and `queue_flac_origin.h` (57). The extra interface and inventory comments favor
explicit ownership over minimizing the total source line count.

## Same-connection rearm correction (20 October 2026)

The historical extraction preserved a generation-triggered capability HELO.
That behavior was incorrect: LMS interprets HELO as a reconnect and restarts its
current playlist item with strm q/s, which can recursively rearm the room.
Generation polling now only resets producer/decoder/PCM state and acknowledges
`playbackReady`; it sends no protocol announcement. HELO belongs exclusively to
genuine connection establishment, including its startup capability selection.

The common fake LMS now responds to an extra connected-player HELO while playing
with q/s and a fresh HTTP source for the audible/current item. The generation
test asserts exactly one connection HELO while preserving reset-before-ready and
no stale start/advance. A separate oracle test proves the fake actually performs
the reconnect reaction; no queue test opts out of it.

An identical queue/speaker strm s request (including HTTP endpoint, request and
format) is idempotent while its producer occurrence is still valid and no STMd
has requested another source. Each STMd grants a successor-start credit: real
LMS can reuse exactly the same player stream URL for successive tracks. Stop/flush,
generation change, failure or an ended run permits a fresh start; changed source,
seek or processing parameters remain normal starts. This narrowly scopes request
identity to the queue capability and never changes stream handling.

Managed sinks can resume the most recently received `strm s` through
`takeQueueStart()`. The core retains its request bytes and starts that request
locally after host readiness/selection; it never asks LMS for another start.
The default callback is false and ordinary stream sinks retain their wire path.

Queue status reporting follows LMS's separate Started and StreamingComplete
states. `startRound` fences STMd to a later event-loop round than STMs. Managed
remote sources additionally wait for speaker-frame remaining time below
`Config::queueNextDelayMs` (20 seconds), after decode completion. The bridge
classifies sources with `queueRemoteSource`; default sinks do not use this gate.
A source fault with any received audio bytes never emits STMn. Produced PCM
still drains and emits the internal `fault-tail-drained` observation; only an
empty source is reported as LMS StreamingFailed. Fault tests explicitly direct
the subsequent source rather than using an incorrect partial-audio STMn.

Cached selections acknowledge a new LMS run with STMc and an actual nonblocking
HTTP/RESP handshake, discarding the encoded body without opening a decoder or
allocating another PCM occurrence. Speaker evidence remains gated until RESP;
STMs then precedes STMd by one event-loop round. `takeQueueCachedStart()` defaults
to false. `deferQueueStart` lets managed mode readiness retain a received start
before any stream output, then resume it once through `takeQueueStart()`.
Autostart-zero `u` releases PCM production without inventing STMr or audibility.
Managed `queueRelativeClock` reports elapsed since the run's confirmed source
origin: LMS adds its seek offset, so the core must not add that offset again.
All new callbacks/options are inert for normal stream and SHM sinks.

## Thin relay rebuild (after queue-full-v1)

The bridge now uses an LMS-driven relay instead of a mode handoff controller.
`QueueMode::modeGeneration` and generation-change producer reset/readiness
notification have been removed. `beginRun` is an inert compatibility hook and
`pollGeneration` only advances the STMs/STMd ordering round. Public Sink
`playbackGeneration`/`playbackReady` defaults remain source-compatible but are
not consumed. The generation-only speaker test is parked at `queue-full-v1`;
all rendering, audible-state, refetch and protocol tests remain.

A sink may report an explicit renderer rejection once with `takeQueueFailure`.
Only decode-ahead speaker-driven sinks consume it: they send STMn, mark the
producer failed and stop reporting success for that run. Defaults leave all
stream/WAV/SHM protocol bytes untouched. This represents a failed renderer
acceptance, not a decoder failure inferred from received bytes.
