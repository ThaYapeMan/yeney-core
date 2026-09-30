# Slimproto: PCM round 1

Historical round 1 contract. Round 2 extends the codec list and adds decoder
queue sizes to STAT; see [decoders.md](decoders.md) for the current decoder contract.

## Evidence and independence

The implementation is original C++17: explicit byte serialization, one socket
poll loop, bounded input FIFO, per-track PCM queues, and sink-owned audible
positions. No squeezelite code was copied, translated, linked, or vendored.
Evidence below supplies interface facts and observed server expectations only.

LMS evidence is pinned to `LMS-Community/slimserver` commit
`f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1` (read-only checkout):

- `Slim/Networking/Slimproto.pm:600–675`: DSCO disconnect reason codes.
- `Slim/Networking/Slimproto.pm:730–780`: events and 53-byte STAT unpacking.
- `Slim/Networking/Slimproto.pm:940–989`: HELO, UUID, reconnect bit, capabilities.
- `Slim/Player/Squeezebox2.pm:140–179`: STMc establishes the lifecycle;
  STMd requests another stream; STMl is synchronization readiness; STMs starts
  the audible track; STMo is output starvation; STMu stops the player.
- `Slim/Player/Squeezebox2.pm:1052–1072`: PCM sample-rate wire codes.
- `Slim/Player/Squeezebox2.pm:710–724`: cont contains metaint and loop.
- `Slim/Player/StreamingController.pm:2223–2308`: stopped, ready-to-stream,
  output-underrun, and streaming-failure controller actions.

Local evidence is `../YeneY`, HEAD
`6670a476279899db1c2e565a78cc86cfc043f93f`, as inspected for this round:

- `squeezelite/slimproto.h:31–64,108–178`: packet fields and fixed sizes.
- `squeezelite/slimproto.c:130–205`: HELO flags and STAT field meanings.
- `squeezelite/slimproto.c:284–380`: timer, stop, flush, pause, skip,
  scheduled unpause, start, and autostart behavior.
- `squeezelite/slimproto.c:399–464`: cont, codc, gain, power, and name messages.
- `squeezelite/slimproto.c:644–761`: RESP, readiness, decoder completion,
  audible start, starvation, final drain, and periodic timer conditions.
- `squeezelite/stream.c:618–635,695–735,878–940`: header termination,
  HTTP body buffering, threshold transition, and exact request transport.
- `squeezelite/pcm.c:65–67,84–181,276–390,440–448`: rate codes,
  PCM container detection, signed raw samples, and size/channel/endian codes.
- `squeezelite/output.c:70–109,126–168,455–466`: skip/pause/start-at,
  track boundary, and flush preserving the already-playing track's tail.
- `yeney.cpp:707–758`: discovery query and length-delimited response tags.

The references are observations, not an assertion that every emulator emits
all historical events. In particular yeney-core explicitly emits STMe and STMh
at their LMS-defined phases; the inspected emulator primarily uses RESP there.

## Framing and identity

All protocol integers are big-endian. Server-to-player frames use a two-byte
length including the four-byte opcode. Player-to-server frames use a four-byte
opcode and four-byte payload length. Fragmentation and concatenation are legal.
Lengths below four in server frames cause a disconnect and backoff.

UDP discovery sends `eNAME\0JSON\0UUID\0VERS\0` to port 3483. Valid replies start
with `E`, followed by four-byte tags, one-byte lengths, and values. Use the
reply sender's IPv4 address. Production probes broadcast and loopback; tests
set `Config::discoveryAddress` to loopback and send no external network traffic.

TCP HELO has device 12, revision 0, the configured MAC, a stable 16-byte identity
derived from that MAC, flags `0x4000` on subsequent connections, a 64-bit byte
counter, language EN, and precisely these capabilities:

`Model=yeney,ModelName=YeneY,AccuratePlayPoints=1,MaxSampleRate=<sink maximum>,pcm`

Backoff is 100, 200, 400, 800, 1600, 3200, then 5000 ms; five seconds of stable
connection resets it. Socket connects time out after three seconds for control
and five for HTTP. Hostname resolution runs independently of shutdown. A `serv`
IPv4 address switches to TCP 3483, preserves identity, and sets reconnect.
No cloud-address sentinel or synchronization-group extension is implemented.

`setd` ID zero queries/sets a NUL-terminated name; SETD confirms ID and name.
`audg` passes modern 16.16 left/right gains to the sink, or unity when adjust is
zero. `aude` passes whether either output is enabled. Neither modifies PCM.

## Start, fetch, and decode

A `strm` fixed payload is 24 bytes after its opcode. The request following it
is sent byte-for-byte, without constructing or rewriting HTTP headers. The
stream address is its supplied IPv4/port; zero IPv4 means the current LMS host.
HTTP supports Content-Length or connection-close framing. Full response headers,
including the terminating CRLF pair, go to RESP. Transfer encoding, TLS, and
nonzero ICY metaint fail explicitly rather than being decoded as samples.

For each accepted start, ordering is:

1. STMf acknowledges replacement of the input stream; prior decoded PCM is
   retained when continuing after STMd.
2. STMc establishes the stream lifecycle, including for unsupported formats.
3. STMe follows successful HTTP connection establishment.
4. STMh then RESP follow receipt of the complete HTTP headers.
5. Read/decode readiness follows the input threshold (KiB), capped to ring
   capacity; EOF permits a short track below threshold.
6. Autostart 0 emits STMl after output readiness and waits for `u`; autostart 1
   starts after the output threshold (tenths of a second), or EOF. Autostart
   2/3 waits for `cont`, then becomes 0/1. Unknown codec `?` also waits for
   `codc`; only PCM is accepted. `cont` metaint zero releases the gate.
7. STMs is emitted when `sink.audibleFrames()` first crosses the exact track
   boundary. Accepting/decoding frames alone cannot trigger it.
8. HTTP completion sends DSCO reason zero. STMd follows consumption of all
   complete PCM frames into the output queue; a partial final frame emits STMn.
9. STMu is emitted once after all queued and sink-buffered output has drained,
   provided no next stream remains. STMo instead reports starvation while the
   current stream is incomplete, once per starvation episode.

STMs and STMd have no unconditional ordering relative to one another: a short
or unpaced track can finish decoding before its first frame becomes audible.
LMS can send the next start after STMd while the preceding track still plays.
The new queue is appended without flushing that preceding queue. Boundaries are
cumulative accepted stereo-frame positions. `trackBoundary(frame, format,
gaplessCandidate)` occurs immediately before offering that track's first frames;
`gaplessCandidate` is true for continuation before final drain. A changed sample
rate takes effect at that boundary, without resampling. A sink must honor the
boundary even when the following write accepts zero frames.

PCM size codes 0/1/2/3 mean 8/16/24/32 bits; channel codes 1/2 mean mono/stereo;
endian codes 0/1 mean big/little. Rate codes 0–8 cover 11025, 22050, 32000,
44100, 48000, 8000, 12000, 16000, 24000 Hz; higher known codes are accepted
only within the sink maximum. Raw samples are signed. Every decoded sample is
left-aligned in signed 32 bits; mono is duplicated, with no gain/DSP applied.
Unknown PCM parameters `?` parse integer RIFF/WAVE or AIFF/uncompressed AIFC
headers. WAV 8-bit samples are unsigned; AIFF/raw 8-bit samples are signed.
Header chunks must fit the configured ring; transport EOF governs stream length.

## Transport commands and clock

- `p`, interval zero: call sink.pause, freeze audible progress, emit STMp.
- `p`, nonzero: pause for that many milliseconds and resume automatically;
  emit neither the user-pause STMp nor an unsolicited STMr.
- `u`, zero: resume immediately and emit STMr. Nonzero is an absolute 32-bit
  jiffies deadline: acknowledge STMr immediately, resume at the deadline.
- `q`: cancel input/output, call sink.flush and sink.stop, reset track elapsed,
  emit STMf. Sink frame counters remain cumulative.
- `f`: cancel input and discard an unstarted upcoming track; preserve the
  already-playing track's decoded tail and elapsed position; emit STMf.
- `a`: discard the requested duration in source frames without writing samples;
  add those skipped frames to that track's elapsed position. Beyond available
  data, continue discarding as data arrives, until the requested skip is met.
- `t`: emit STMt immediately, echoing the four raw timestamp bytes carried in
  the replay-gain field. Also send STMt about once per second during playback.

Jiffies uses steady_clock milliseconds modulo 2^32; deadlines compare using
signed modular differences. The pacer uses the same clock and per-format frame
rates, retains fractional time, honors partial/zero writes, and caps scheduling
debt at 20 ms. Pause/starvation does not accumulate a burst of delayed audio.
For `paced()==false`, the sink controls playback timing and the core supplies
available PCM without a real-time delay. Sink methods must be nonblocking.

STAT payload offsets (from its event field) are:

| Offset | Field | Width |
| --- | --- | --- |
| 0 | Event | 4 |
| 4 | CRLF, initialized, mode (zero) | 3 |
| 7, 11 | Input buffer capacity, fullness (bytes) | 4 each |
| 15, 19 | Received bytes high, low | 4 each |
| 23 | Signal strength (0xffff) | 2 |
| 25 | Jiffies | 4 |
| 29, 33 | Output capacity, fullness (bytes; 8/frame) | 4 each |
| 37 | Elapsed whole seconds | 4 |
| 41 | Voltage (zero) | 2 |
| 43 | Total elapsed milliseconds | 4 |
| 47 | Echoed server timestamp, otherwise zero | 4 |
| 51 | Error code (zero) | 2 |

Received bytes count the HTTP body for the latest input track, resetting at
start; they exclude response headers. Elapsed comes exclusively from the sink's
cumulative audible position minus the audible track boundary, plus explicit
skip duration, divided by that track's rate. It never comes from bytes received,
decoder position, write acceptance, or wall-clock extrapolation. Pausing requires
the sink to freeze its audible counter. Flush/stop discard in-flight frames;
future accepted positions restart from the sink's retained cumulative counter.

## Validation and limits

`make test` checks every received packet's framing/fields against an independent
Python wire oracle. Scenarios cover discovery, identity, lifecycle ordering,
bit-exact PCM, both endian orders, mono, rate changes, gapless boundaries,
real-time elapsed within 5%, timers, both pause modes, scheduled starts, skip,
stop/flush, volume/power forwarding, naming, server switching, reconnect/backoff,
unsupported formats, output starvation, final drain, container parameters,
partial/zero writes, delayed audible position, and full-buffer backpressure.
Unit tests cover randomized ring wraparound, packet fragmentation, invalid
lengths, byte offsets, signed sample extremes, and jiffies wraparound.

Validation uses fake LMS on localhost, not a running production LMS. FLAC/MP3,
ReplayGain, fades, crossfade, SHM, and integration into other repositories remain
outside round 1. The WAV sink splits files on rate changes because a WAV header
has one sample rate; the core itself preserves a continuous frame boundary.
