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

The SHM v1 segment is `/dev/shm/squeezelite-<lowercase-mac>` and is left in place
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

Licensed under [PolyForm Noncommercial 1.0.0](LICENSE).
Required Notice: Copyright (c) 2026 Jaap van Vliet.

Third-party libraries retain their own licences; full texts and pins are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Include these notices when
distributing binaries.
