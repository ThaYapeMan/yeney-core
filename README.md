# yeney-core

An independently written C++17 LMS player core for YeneY and, later, LampaStream.
Advertises **alc,flc,mp3,aif,pcm**, in that order, and delivers signed 32-bit stereo frames to a
pluggable sink. No squeezelite implementation is included or linked.

Build with a C++17 compiler, GNU Make, pthreads and system libFLAC (`libflac-dev`).
Initialize the pinned Apple ALAC submodule; minimp3 is already vendored.
Tests also require clang-format; ffmpeg/flac generate reference fixtures when
available, otherwise committed fixtures are used with an explicit SKIP reason:

```sh
git submodule update --init --recursive
make
make test
./yeney-player -n YeneY -m 02:00:00:00:00:01 --sink null
./yeney-player -n YeneY -m 02:00:00:00:00:01 -s localhost:3483 --sink wav:listen.wav
```

For LampaStream, use the same player arguments with our paced SHM output:

```sh
./yeney-player -n "Core player" -m 02:00:00:00:00:01 -o hw:CARD=Dummy,DEV=0 -v -s localhost
# --sink shm is equivalent to -v; no ALSA device is opened.
```

The SHM v1 segment is `/dev/shm/yeney-<lowercase-mac>` and is left in place
on exit for LampaStream's lifecycle/orphan cleanup. Install/select yeney-player
as the executable in place of squeezelite externally; this repo does not alter
LampaStream. See [SHM v1 layout, lifecycle and compatibility](docs/shm-v1.md).
`-o` is accepted and ignored with one stderr info line. Output is quiet by default;
errors still go to stderr, and `-d 1` enables event lines.

Without `-s`, UDP discovery uses port 3483. `-d 0` silences event logging;
levels 1–5 print event lines. `--max-rate <Hz>` sets the null/WAV sink maximum
(default 48000; accepted range 44100..384000) and the HELO MaxSampleRate.
Use `--max-rate 192000` for native 192 kHz playback. SIGINT/SIGTERM stop the player cleanly.
WAV output is stereo 32-bit integer PCM; rate changes open `listen.wav.1.wav`,
then successive numbered segments. The null and WAV sinks run at real time.

`libyeneycore.a` exposes `core/player.h` and `core/sink.h`. Set
`Config::streamBytes` and `Config::outputFrames` to bound the two buffers;
set `Config::discoveryAddress` for an isolated network.
`Config::earlyMediaBytes` caps MP4 media buffered before moov (default 256 MiB).
Per-track workers add bounded 64 KiB input and 8192-frame output queues. The sink's maximum
sample rate controls HELO. Source bit depth/channels are described at boundaries;
frames passed to `write` are always 32-bit stereo. Sink callbacks must not block.

Tests use only Python's standard library and localhost TCP/UDP, including a
recording sink that partially accepts writes. Ports 3483 on loopback must be free
for discovery/server-switch tests. No frontend exists. See
[the protocol contract and evidence](docs/slimproto.md).

ReplayGain, linear fades and same-rate crossfade are supported; LMS volume never
scales PCM. See [transition arithmetic, timing and limits](docs/transitions.md).
Native ALAC requires **Apple Lossless -> Native** enabled in LMS File Types;
otherwise LMS can convert it losslessly to FLAC.

ALAC uses our streaming MP4 demuxer; more than two channels is unsupported.
TLS and ICY metadata are outside this round. HTTP PCM
responses must use Content-Length or connection-close framing, without transfer
encoding. No changes to YeneY or LampaStream are required.

See [decoder formats, gapless rules, limits and evidence](docs/decoders.md).
`make format` formats our C++ files; third-party sources stay unchanged.
See [queue mode inventory and internal hooks](docs/queue-mode.md) for the
capability-isolated playback and decoder policy, and before/after PCM proof.

Licensed under [PolyForm Noncommercial 1.0.0](LICENSE).
Required Notice: Copyright (c) 2026 Jaap van Vliet.

Third-party libraries retain their own licences; full texts and pins are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Include these notices when
distributing binaries.

Content positions use the original decoded source sample coordinates. For LMS
ALAC/M4A seeks, the selected `mdhd` duration stays original while `stts` covers
only delivered packets: their exact duration difference gives the first packet's
source frame. A known origin requires a finite media duration, complete consistent
packet tables, exact media-to-sample conversion and a whole ALAC-packet difference
(with ordinary packet durations and at most one short final packet). Original
edit-list/iTunSMPB ranges are intersected with the delivered range, so priming is
not applied twice. Chunk offsets are rebased by LMS and are not source indices.
Zero is known only when these tables prove that the response starts at the first
packet; unprovable origins remain unknown. From-start trimming keeps its existing
source coordinates. FLAC positions and the SHM v1/YNPT layouts are unchanged.

Sample-rate-converted content remains unknown in output coordinates: a fresh
filter at a nonzero landing lacks the from-start production's preceding samples.
Native AAC is not advertised or decoded by this core; incoming AAC/M4A is rejected
without publishing a known position. This does not add AAC playback support.

`make test` includes real LMS 9.1.1 loopback MP4 seek tests when its cached
distribution is present (`YENEY_REAL_LMS_HOME` overrides the default
`~/.cache/yeney-test/real-lms-9.1.1`). Tests use ports 3483, 9000 and 19090 on
loopback. FFmpeg generates public ALAC/AAC inputs and serves only as a PCM oracle.
Set `YENEY_LENGTH_FIXTURE_DIR` for the optional private ALAC checks, read in place;
unset it to see an explicit SKIP. Set `LAMPASTREAM_CHECKOUT` to the read-only
consumer checkout to run its SHM `require_v1` gate.
