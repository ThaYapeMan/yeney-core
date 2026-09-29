# yeney-core

An independently written C++17 LMS player core for YeneY and, later, LampaStream.
Round 1 advertises **PCM only** and delivers signed 32-bit stereo frames to a
pluggable sink. No squeezelite implementation is included or linked.

Build with a C++17 compiler, GNU Make and pthreads:

```sh
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
set `Config::discoveryAddress` for an isolated network. The sink's maximum
sample rate controls HELO. Source bit depth/channels are described at boundaries;
frames passed to `write` are always 32-bit stereo. Sink callbacks must not block.

Tests use only Python's standard library and localhost TCP/UDP, including a
recording sink that partially accepts writes. Ports 3483 on loopback must be free
for discovery/server-switch tests. No frontend exists. See
[the protocol contract and evidence](docs/slimproto.md).

FLAC, MP3, DSP/fades, SHM, TLS and ICY metadata are outside this round. HTTP PCM
responses must use Content-Length or connection-close framing, without transfer
encoding. No changes to YeneY or LampaStream are required.

Licensed under [PolyForm Noncommercial 1.0.0](LICENSE).
Required Notice: Copyright (c) 2026 Jaap van Vliet.
