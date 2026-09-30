# ReplayGain and transitions

All pipeline, arithmetic and scheduling code is ours. Squeezelite/LMS were read
only for interface facts and observable behaviour. DSP runs on raw normalized
int32 stereo immediately before the sink, so WAV and SHM receive processed PCM.
LMS volume (`audg`) is still never applied to PCM. LampaStream's separate pinned
0c4b369 build is unaffected; this repo does not change LampaStream or YeneY.

## Wire fields and arithmetic

In a complete server `strm` packet including its four-byte opcode, byte 13 is
transition_period (uint8 seconds), byte 14 is transition_type (ASCII digit), and
bytes 18–21 are replay_gain (big-endian unsigned 16.16). The zero ReplayGain
sentinel means unity, not silence. Every track owns these fields independently.
No tag parser or local gain choice overrides LMS's supplied value.

For source sample x, raw gain g, and envelope e in [0,65536]:

- G = g if g != 0, otherwise 65536.
- combined = floor(G * e / 65536), calculated in uint64.
- y = clamp(floor(x * combined / 65536), INT32_MIN, INT32_MAX).

The signed product uses int64, with explicit mathematical floor for negatives;
there are no implementation-dependent signed shifts or overflowing int32 products.
Each crossfade branch uses that arithmetic, then their sum is clamped in int64.
For example -1 at half gain becomes -1; +1 becomes 0. Gain 26112 corresponds to
20*log10(26112/65536), approximately -7.993 dB. Transition none with gain 0 or
65536 is bit-exact. Gain changes at an ordinary gapless boundary, never at fetch
or decode time. During overlap each branch retains its own track's gain.

## Shapes and end-relative scheduling

For a window of N frames, at integer frame i in [0,N):

- rising envelope U(i) = floor(65536*i/N).
- falling envelope D(i) = 65536-U(i).

These are linear amplitude ramps, not equal-power/sine curves. The first rising
sample is zero; its last sample is below unity. Falling begins at unity and its
last sample is above zero; the next track's rising window starts at zero.
A zero-length window does no processing.

| Type | Wire | Behaviour |
|---|---|---|
| None | '0' | ReplayGain only |
| Crossfade | '1' | Incoming head blends with outgoing final N frames |
| Fade in | '2' | Rising window at the track's first frame |
| Fade out | '3' | Falling window ending at the track's last frame |
| In-and-out | '4' | Half the period at each end, rounded down to whole frames |

Full-period N is rate*period; type 4 divides that by two. The effective window
is capped by Config::transitionMaxFrames (default 2097152 frames per track).
Fade-out is scheduled when decoder EOF is known, beginning N frames before the
end of remaining raw audio. A tail window is retained while decoding to avoid
retroactively changing samples already submitted. If less than N remains, shorten
the window to remaining frames. Fade-in also waits for its head window or EOF;
short tracks shorten that window to their decoded length. When a short type-4
track's windows overlap, the outgoing ramp takes precedence in the tail region.
Individual tracks retain their own periods, even when adjacent settings differ.

Crossfade is selected by the incoming track's type 1. Its window is limited by
requested period, the outgoing unsubmitted tail and (at incoming EOF) available
incoming frames. Start is old end minus this effective window. Consume one raw
frame from each track per output frame; output length is old length + new length
minus the overlap. The incoming overlapped head is not replayed after completion.
The mix is clamp(process(old,D(i)) + process(new,U(i))). No crossfade occurs across
sample rates: keep a normal format boundary and emit both full tracks.

Outgoing type 1 retains a tail so a following type 1 can obtain its requested
window. If only the incoming command requests crossfade, use whatever outgoing
raw audio remains; future settings cannot be predicted. A late incoming command
can shorten overlap. If its head is not buffered at the planned overlap start,
log and fall back to ordinary continuation rather than stall playback or invent
silence. The normal stream-underrun handling still applies if no PCM is available.
The first track has no prior audio to crossfade, and the final track emits its
remaining tail normally when there is no continuation.

Raw PCM queues grow by twice the largest effective window observed since the
last full stop/clear, on top of Config::outputFrames. This bounded high-water
capacity holds tail and head without reallocating old samples into processed
buffers. STAT reports that capacity and current raw queue fullness, plus existing
worker queues. The default additional maximum is 32 MiB of PCM; deque overhead
and decoder-private working memory are separate. The first lookahead window can
add startup buffering, especially for slow/live streams. Window caps shorten very
long requested transitions; they never silently allocate an unbounded period.

## Audible position and interruption

The incoming track boundary is at overlap START, matching squeezelite's moved
track_start and LMS's new-track notification. STMs waits until sink audibleFrames
passes that frame; incoming elapsed starts there at zero, and advances throughout
the overlap. At overlap completion it is already N/rate seconds into that track.
There is no elapsed reset at the outgoing track's nominal end. A sink that delays
or partially accepts output still controls STMs and elapsed; envelopes advance
only for accepted frames, and rejected/partial writes never mutate source PCM.

Pause freezes both transition progress and elapsed. Resume continues the same
window without restart. Full stop/disconnect/clear removes all transition state.
LMS seek is a full stop/flush followed by a new strm s at the desired file offset;
that is a new track instance, with only the newly supplied gain/transition fields.
The core cannot infer absolute file position from strm s alone. Streaming flush
(strm f) discards unaudible queued tracks. If overlap has started, incoming audio
is already audible: cancel the old branch and retain the incoming track from its
already-consumed head position. Its elapsed does not reset. An active standalone
fade is retained with the current audible track, matching streaming-flush's
retention contract. Skip-ahead cancels overlap before skipping the incoming track.

## Device checks and limits

The device script retains previous phases and adds two WAV plays of ALAC 47145
with replayGainMode 1 then 0. It compares aligned source-relative 2–8 second
windows, expecting the captured strm gain ratio within 0.1 dB. The crossfade phase
sets transitionType=1, transitionDuration=5, transitionSmart=0, loads FLAC 44436
then MP3 41965, and seeks to first duration minus 20 seconds. It checks the
captured request, overlap start/completion/boundary frames, five-second length,
WAV coverage, no silent ten-ms overlap blocks and no underrun/error lines.
Natural silence could fail the music-window check; it is a practical device
check, not a general proof that every silent PCM run is an inserted gap.

Every changed player preference is saved before mutation and restored in finally,
including failed setup and Ctrl+C. Restoration attempts every saved preference;
failures are reported, never treated as restored. SIGINT is temporarily ignored
while restoring, then its prior handler is reinstated. run-info.txt records the
server's `pref disabledformats ?` response. Capture is required to validate sent
ReplayGain; --no-capture reports a failed transition capture check. WAVs remain
in the existing tarball. No real-server run or deployment is part of make test.

Only requested types 0–4 are supported; type 5 (crossfade-immediate) is explicitly
unsupported and produces STMn. There is no audg volume fade, resampling, equal-power
crossfade, tag-driven gain, limiter or dithering. LMS can suppress/shorten transitions
through smart, sleep, rate restriction and short-track preferences; we obey the
resulting wire fields rather than duplicating server policy. Native ALAC requires
Apple Lossless -> Native enabled in LMS File Types; otherwise lossless FLAC
conversion can bypass our ALAC decoder even with ALAC first in capabilities.

## Pinned evidence

LMS commit f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1:

- Slim/Player/Squeezebox.pm:516–530 documents strm fields/type digits;
  :930–941 chooses transitionType/transitionDuration; :943–1018 applies smart,
  sleep, rate, start/reposition and short-track overrides; :1038–1039 sends fields.
- Slim/Player/StreamingController.pm:1092–1142 stops and restarts for seek;
  Slim/Player/Squeezebox.pm:206–215 sends strm q for stop.
- Slim/Player/Squeezebox2.pm:44–47 gives transition/replay preferences;
  :888–896 maps server ReplayGain dB to fixed-point, zero when absent.
- Slim/Player/ReplayGain.pm:27–39 reads replayGainMode and disables mode 0;
  :56–58 selects track gain for mode 1.
- Slim/Networking/Slimproto.pm:744 defines STMs as new-track start;
  :865–878 derives play points from reported elapsed and dispatches status.

Squeezelite evidence at 0e1667ead996834e355fc51f6a8eb2ea7e55f44b:

- slimproto.c:352–386 reads transition and gain fields.
- output.c:153–164 resets track time at track_start; :223–245 defines linear
  fades and separate ReplayGain in overlap; :291–342 defines half-period in/out,
  remaining-tail clipping, rate restriction and moved crossfade track_start.
- output.c:250–252 falls back when incoming PCM is unavailable;
  :438–465 distinguishes full reset from streaming flush.
- output_pack.c:33–43 establishes fixed-point scale/saturation and float-to-gain;
  :366–375 mixes branches. Our per-frame integer ramps avoid block-dependent
  quantization, and our crossfade sum saturates instead of allowing signed wrap.

No implementation expressions are imported from these evidence files.
