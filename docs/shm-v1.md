# SHM v1 output for LampaStream

This is our independently written producer. GPL producer code was read only as
interface/behaviour evidence; none is incorporated. No ALSA output is opened.
The core pacer supplies real-time timing, including with `-o hw:CARD=Dummy,DEV=0`.

## Wire layout

The POSIX name is `/squeezelite-<mac>`, backed by `/dev/shm/squeezelite-<mac>`.
MAC bytes render as lowercase two-digit hex separated by colons. The mapping is
32952 bytes; the original 32888-byte prefix is unchanged, little endian. `sinks/shm_v1/layout.h` statically asserts
every field offset, total size, little endian and the 56-byte pthread lock ABI.
Unsupported host ABIs fail compilation rather than publishing an incompatible
segment.

| Byte offset | Field | Representation |
|---:|---|---|
| 0–55 | lock | process-shared pthread_rwlock_t |
| 56 | buf_size | uint32, 16384 int16 scalars |
| 60 | buf_index | uint32, next scalar index |
| 64 | running | uint8, zero/one; three zero padding bytes |
| 68 | rate | uint32 Hz |
| 72 | updated | int64 Unix seconds, legacy diagnostic only |
| 80–32847 | PCM | 8192 stereo frames, interleaved int16 L/R |
| 32848 | magic | uint32 0x48555345; bytes 45 53 55 48 |
| 32852 | abi_version | uint16, 1 |
| 32854 | flags | uint16, bit 0 indicates valid player timing |
| 32856 | write_seq | uint32, odd writer/even stable |
| 32860 | generation | uint64, secure random lifetime ID |
| 32868 | abs_write_pos | uint64, exclusive exported stereo-frame position |
| 32876 | gap_seq | uint64, skipped-export update count |
| 32884–32887 | padding | four zero bytes |

The unaligned uint64 extension values are written as eight bytes, avoiding
unaligned typed accesses. One exported stereo frame increments abs_write_pos by
one and buf_index by two modulo 16384. Ring laps never reset absolute position.

## Initialization and coherent updates

An existing segment may be reused after its prior producer retires. Initialization
forces the prior sequence to its next odd successor: even +1 or odd +2, modulo
2^32. In particular 0xffffffff becomes 1 during initialization, then 2 on success.
Legacy metadata/PCM and the extension are initialized inside that odd window;
legacy fields are written under an initialized process-shared write lock.
The ring/index/counters reset to zero, running is false, and initial rate is 44100
as in the fork. A new generation comes from getrandom, or /dev/urandom if needed.
There is no PID/time fallback. Entropy failure leaves sequence odd, reports
`SHM v1 sink unavailable`, and aborts player startup. The unavailable segment is
retained for manager cleanup. Only one producer may own a MAC at a time; concurrent
producer initialization is not supported by this ABI.

Every successful update takes pthread_rwlock_trywrlock, publishes odd sequence,
updates metadata/PCM, then publishes even sequence before unlocking. This supports
legacy lock-taking readers and modern seqlock readers. Normal update wrap is
0xfffffffe -> 0xffffffff -> 0. Initialization recovers an already-odd sequence
using the odd successor rule. The sequence stores use sequentially consistent
atomics so preceding/following payload stores stay inside the publication window.

Never block the event loop waiting for a legacy reader. A failed write-lock attempt
skips the export and retains a producer-local pending gap count. Its next successful
update adds that count to gap_seq. Audible playback still consumes those frames;
abs_write_pos counts only exported PCM, matching the fork. The consumer invalidates
on the gap before adopting a fresh baseline. Generation remains constant during
the sink lifetime and changes probabilistically on restart, including inode reuse.

## PCM, rates and lifecycle

Signed int32 stereo is converted to int16 by nearest rounding, with halfway values
away from zero, then saturation to [-32768,32767]. This intentionally follows the
requested rounding contract rather than the fork's truncating shift. Volume and
power never scale PCM; volume is ignored for full-scale analysis. Power-off publishes
silence state. `paced()` is true; maxSampleRate defaults to 48000 and follows the
app's `--max-rate` option.

A boundary records the next rate privately. The first PCM write at that rate
publishes the samples, rate, running=true and timestamp together. Same-rate gapless
boundaries do not reset positions, generation, gap_seq or running. A rate change
causes the production consumer to invalidate its old epoch and adopt the new
baseline; samples in that first rate-changing snapshot are intentionally not
returned by the consumer.

Pause publishes running=false and accepts no PCM until resume. Resume alone does
not set running=true; actual PCM does. Stop and flush publish running=false without
clearing the ring, rewinding either position, changing generation, or manufacturing
an export gap. Further actual PCM restarts running. Slimproto strm f retains the
current audible track under the existing core flush-streaming contract; it does
not call Sink::flush when merely removing a queued continuation. The optional
Sink::idle callback publishes running=false at end-of-playlist and output underrun without changing
playback state. Other sinks inherit a no-op idle callback. This represents the
fork's silence exports without synthesizing PCM or stalling the core clock.

On destruction publish stop and unmap, but do not unlink or destroy the shared lock.
LampaStream owns normal unlink and orphan cleanup. If a legacy reader prevents
publication during shutdown, it can leave the prior stable state; no blocking lock
or unprotected payload write is used. The manager removes the object after process
exit. This is the nonblocking shutdown tradeoff.

## Drop-in arguments and verification

`-v` and `--sink shm` select this sink; explicit sink selectors use the last one
on the command line. `-o <device>` is accepted, ignored, and reports one info line
to stderr. The app is otherwise quiet by default; errors remain on stderr and
`-d 1` enables event logging. Name, MAC, discovery/server selection and --max-rate
remain available. Select/install yeney-player in place of squeezelite externally;
this round makes no changes to LampaStream or YeneY.

Tests cover layout, sequence wrap, entropy syscall fallback/failure, generation
and inode reuse, multiple laps, rounded samples, rates, lifecycle and lock gaps.
An independent Python reader validates fake-LMS PCM across gapless and rate changes.
The unmodified LampaStream SqueezeliteShmStereoSource(require_v1=True) receives the
same data, including exact sample-position checks and the expected rate invalidation.
Missing sibling checkout or Python dependencies prints an explicit SKIP.

The local checkout is named ../SqueezeHue but its origin is ThaYapeMan/LampaStream;
the cross-repo test recognizes that verified alias after checking ../LampaStream.
No consumer code is copied or modified. The device script retains all prior phases
and adds FLAC with -v, 11 stable snapshots over ten seconds, generation/rate checks
and a 5% frame-position pacing check. `shm-extension.txt` includes all 40 extension
bytes as hex for every snapshot in the existing tarball. No production test,
SSH or deployment is performed by make test.

## Evidence references

LampaStream checkout 2122692cb5e7c7ccda2f68a7f50d0b4766dc6487:

- squeezelite/README.md:44–71 defines the exact layout/units; :73–91 specifies
  initialization, sequence wrap, secure generation and pending gaps; :108–120
  describes diagnostics and the limits of local validation.
- src/lampastream/pcm_source.py:113–172 defines legacy/extension formats;
  :401–481 opens require_v1 mappings; :521–542 obtains coherent metadata;
  :728–867 checks rate, restart, generation, gaps, overruns and post-copy sequence,
  then normalizes int16 PCM to stereo float32.
- src/lampastream/player_manager.py:914–936 owns unlink/orphan cleanup;
  :1279–1315 supplies the drop-in arguments; :1317–1331 waits for the segment.

Producer evidence is ThaYapeMan/squeezelite at
0e1667ead996834e355fc51f6a8eb2ea7e55f44b:

- output_vis.c:108–124 defers gaps and marks silence without PCM; :131–158
  publishes rate with audio; :166–175 publishes stop; :182–219 specifies naming,
  shared locking, initial rate and secure initialization.
- output_vis_v1.c:72–124 defines odd-successor initialization and completion;
  :143–175 defines sequence updates and deferred export gaps.
- output.c:85–115 treats pause, future start and missing frames as silence;
  :276–279 exports the resulting audio/silence state.

These references identify protocol facts and observable behaviour, not reused code.

## Optional player-clock block (YNPT v1)

The 64 bytes at offset 32888 are optional. Readers must check mapping length,
extension flags bit 0, magic `YNPT`, version 1 and a nonzero rate. Legacy readers
can still map exactly 32888 bytes. All timing fields are byte arrays on the writer
and little endian on the wire; the same extension write_seq covers PCM, positions,
timing and events. Initial flags remain zero until a paced PCM export supplies an
anchor. Existing sinks inherit no-op timing callbacks, so YeneY can compile this
sink without supplying timing and its prefix reader remains unchanged.

| Relative byte | Field | Type |
|---:|---|---|
| 0 | magic | four bytes YNPT |
| 4 | version | uint16, 1 |
| 6 | reserved | two zero bytes |
| 8 | anchor_abs_frame | uint64, exported stereo-frame coordinate |
| 16 | anchor_play_mono_ns | uint64, CLOCK_MONOTONIC nanoseconds |
| 24 | rate_milli_hz | uint32, effective pacer Hz times 1000 |
| 28 | event_seq | uint32, advances for each event |
| 32 | event_flags | uint32 |
| 36 | event_abs_frame | uint64, exclusive export position at event |
| 44 | event_value | int64 |
| 52 | reserved | twelve zero bytes |

For exported frame F, play time is anchor_play_mono_ns +
(F - anchor_abs_frame) * 1e12 / rate_milli_hz. The anchor refers to the first
frame in the latest successful export. The core's accumulated pacing credit
covers the interval ending at its current millisecond monotonic tick; the first
frame's schedule is that tick minus remaining credit. This is the schedule whose
consumption advances Sink::audibleFrames and LMS elapsed, not the later memcpy
completion. There is no additional block look-ahead: blocks consume already-due
credit (at most 20 ms), in batches of at most 256 frames. Millisecond scheduling
quantisation is preserved; this extension does not change pacing or elapsed.

Events are FLUSH=1, PAUSE=2, RESUME=4, DISCONTINUITY=8, SYNC_PAUSE=64,
SYNC_SKIP=128. SYNC_PAUSE carries nanoseconds and SYNC_SKIP carries skipped
source frames. A sync pause retains its event through automatic resume, so a
reader never mistakes its running=false/true transition for a seek. Subsequent
anchors include the pause. A skip removes content before export: export positions
remain continuous, while subsequent PCM represents source content earlier than
it would have without the skip. No fictitious export positions are inserted.
Track boundaries, including gapless boundaries, publish DISCONTINUITY with the
first PCM of the next track; generation and positions retain their existing
semantics. Rate changes likewise publish with their first PCM. User pauses and
resumes carry their distinct events; stop and sink flush carry FLUSH.

The block retains the latest event, not an event queue. Readers should poll fast
enough to see corrections, retain the prior anchor for unread PCM preceding
an event boundary, and fail closed if events were lost. A missed export is still
reported through gap_seq. Event counters wrap modulo 2^32.
