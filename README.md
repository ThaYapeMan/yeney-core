# yeney-core

An independently written C++17 LMS player core for YeneY and, later, LampaStream.
Advertises **flc,alc,mp3,aif,pcm**, in that order, and delivers signed 32-bit stereo frames to a
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

Without `-s`, UDP discovery uses port 3483. `-d 0` silences event logging;
levels 1–5 print event lines. SIGINT/SIGTERM stop the player cleanly.
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

ALAC uses our streaming MP4 demuxer; more than two channels is unsupported.
DSP/fades, SHM, TLS and ICY metadata are outside this round. HTTP PCM
responses must use Content-Length or connection-close framing, without transfer
encoding. No changes to YeneY or LampaStream are required.

See [decoder formats, gapless rules, limits and evidence](docs/decoders.md).
`make format` formats our C++ files; third-party sources stay unchanged.

Licensed under [PolyForm Noncommercial 1.0.0](LICENSE).
Required Notice: Copyright (c) 2026 Jaap van Vliet.

Third-party libraries retain their own licences; full texts and pins are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Include these notices when
distributing binaries.
