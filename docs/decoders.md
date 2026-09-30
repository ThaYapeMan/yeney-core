# Decoder contract and evidence

Round 2 advertises `alc,flc,mp3,aif,pcm`, in that exact order after
`MaxSampleRate=<sink maximum>`. Our demuxer, adapters, trimming, queues, and tests
are original code. No squeezelite implementation is incorporated. Library source
and licence details are in `THIRD_PARTY_NOTICES.md`.

## Capability order and test-player maximum

ALAC comes first because LMS tries conversion targets in advertised order unless
`prioritizeNative` is enabled. At pinned slimserver commit
`f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1`,
`Slim/Player/TranscodingHelper.pm:355–366` obtains the supported order and
optionally moves native first; `:368–389` builds and tries profiles in that order.
`convert.conf:350–353` provides alc->flc, so putting FLAC first can bypass our
native ALAC decoder; `:377–378` provides native alc->alc.

The same pinned `convert.conf:1–414` contains no active flc->alc, mp3->alc, mp3->flc,
or aac->alc rule. Native FLAC (`:295–296`) and MP3 (`:169–170`) therefore stay
native. AAC still selects the lossless FLAC target (`:342–344`). MP3 follows
ALAC and FLAC because alc->mp3 (`:117–120`) and flc->mp3 (`:137–140`) exist
and must not take precedence over lossless/native playback. The mp3->flc
example at `:292–293` is commented out. Pinned sources:
[TranscodingHelper.pm](https://github.com/LMS-Community/slimserver/blob/f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1/Slim/Player/TranscodingHelper.pm#L355-L389),
[convert.conf](https://github.com/LMS-Community/slimserver/blob/f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1/convert.conf#L1-L414).

`yeney-player --max-rate <Hz>` sets NullSink/WavSink maxSampleRate and HELO's
MaxSampleRate, default 48000, accepting integer Hz from 44100 through 384000.
It does not change core limits. The device test keeps its four 48 kHz tracks,
then restarts the same player identity with `--max-rate 192000` for
"ALAC 192k native". That phase requires wire `l`, exact songinfo rate 192000,
a second-phase HELO with the same format list and MaxSampleRate=192000, and
all existing playback, seek, pause, resume, and clean checks. Logs and the
original capture/tarball cover both phases. Localhost tests verify both sink
choices at 192 kHz (WAV samples bit-exact) and the default/argument bounds.

## Per-track pipeline

Each track owns one Decoder. `feed` accepts bytes without waiting and may accept
fewer bytes; `take` returns normalized signed 32-bit stereo frames. `format`
reports source rate, bit depth, and channels when known. `finish` marks input EOF;
`done` means the worker finished and its decoded output has been consumed.
A worker bridges blocking codec reads to the nonblocking player. Destruction
cancels input/output waits and joins it. Decode failures produce a clear log and
STMn, with the existing DSCO error path for an active HTTP connection.

Each worker has 64 KiB input and 8192 output frames. STAT capacities/fullness
include these queues and the player's configured queues. Codec-private working
memory and MP4 metadata/media staging are separate. STMd waits for decoder
completion, including gapless trimming; STMs and elapsed still use the sink's
audible counter. A completed track's decoder state is never reused for its next
track. Source format changes inside one track fail explicitly.

## FLAC

Wire code `f` selects system libFLAC's stream decoder, using read/write/error
callbacks. Integer 16/24-bit mono/stereo is supported and left-aligned in int32.
The first decoded frame determines format, including after a seek. Initial sync
scanning is allowed; CRC/header errors and sync loss after audio are errors.
MD5 checking is disabled because a seek stream is not the entire original file;
frame CRC checks remain enabled. Complete streams and frame-aligned headerless
streams are covered by bit-exact tests against the flac CLI reference.

LMS evidence, checkout `f0a77cdce73ef1d1cc53019967d845dfcd0f8fb1`:
`Slim/Formats/FLAC.pm:799–814` finds the frame at the requested seek time.
`Slim/Player/Protocols/File.pm:145–170,345–356` only prepends an initial audio
block when the format implements that method; native FLAC has no such method.
`Slim/Formats/FLAC.pm:883–946` explicitly distinguishes full-file STREAMINFO
from frame-aligned data. Thus a seek must not require the original `fLaC` header.
The library API contract is documented at
https://xiph.org/flac/api/group__flac__stream__decoder.html.

## MP3

Wire code `m` uses unmodified minimp3 at commit
`ea99364f61c14656440e8d77e9c233ccf3124633`. ID3v2's synchsafe length and optional
footer are skipped without buffering the whole tag. MP3 mono/stereo output is
minimp3 float synthesis (`MINIMP3_FLOAT_OUTPUT`), converted to normalised int32.
Each sample is multiplied by 2^31 in double precision and rounded to nearest,
with ties away from zero. Values at or above +1 saturate to INT32_MAX; values
at or below -1 saturate to INT32_MIN. This includes unclipped overs from hot
masters and infinities; defensive NaN handling produces silence. Format reports
32-bit frames, without an intermediate int16 quantisation. The SHM sink retains
its existing int16 rounding. ID3v1 at EOF is also accepted.

The first MPEG frame is inspected at its version/channel-dependent side-info
boundary for Xing/Info. Its flags determine optional fields; the encoder
extension stores the 12-bit delay and padding. The info frame is not audio.
Trim start is encoder delay + 529 samples (standard Layer III decoder delay).
Trim end is Xing audio-frame count times samples/frame, minus
max(encoder padding - 529, 0). Without a frame count, retain the padding tail
until EOF; without gapless metadata, emit all decoded audio rather than invent
encoder delay. Bounds and impossible trim ranges produce errors.

Interface evidence is the pinned upstream minimp3/minimp3_ex.h:193–265; that
file is read as documentation and is neither vendored nor compiled. Our parser
and trimming implementation are independent. The continuous-signal test splits
at a non-MPEG-frame boundary: 7013 + 8006 = 15019 source frames. It checks exact
output lengths, independent ffmpeg-reference alignment around the join, and
real fake-LMS playback with a sink boundary at frame 7013. Lossy independent
encodes cannot be bit-identical to a single continuous encode; this test proves
no timeline samples are inserted or omitted, not lossless MP3 compression.

## ALAC and MP4

Wire code `l` selects our Mp4Demuxer followed by Apple ALAC, pinned as submodule
`mikebrady/alac` at `5d8c5db0dfcadd5872f28e665cf4f4303447352a`. The magic cookie
is the 24-byte configuration inside the selected sample entry's inner `alac`
box; QuickTime `wave` nesting and audio entry versions 0/1 are supported.

Top-level headers are assembled incrementally, including bytewise fragments.
Box lengths support 32-bit, extended 64-bit, and zero (remaining parent/EOF).
Every child view stays within its parent. Table counts are checked against the
remaining payload before allocation or access. Tracks keep separate hdlr,
mdhd, stsd, stts, stsc, stsz, stco/co64, and elst data. Select the first audio
track with an ALAC entry; unrelated tracks cannot supply its tables or cookie.

stsc runs expand chunk-to-packet mapping using the selected description index.
Chunks can contain multiple packets and have gaps between them. Backwards or
overlapping offsets, missing packets, offsets outside mdat, truncated boxes,
and inconsistent sample/time tables are explicit errors. Extended mdhd/mvhd
and elst version 1 are supported. Fragmented MP4, compressed sample entries,
audio entry version 2, mixed sample descriptions, and discontinuous media edits
are unsupported.

With moov first, packets stream directly from mdat without retaining that box.
With mdat first, its payload is buffered until moov resolves offsets. Configure
`Config::earlyMediaBytes` or `DecoderConfig::earlyMediaCap`; default 256 MiB.
Exceeding it fails with `mdat-before-moov exceeds configured buffering cap`.
Additional limits: 32 MiB moov, one million entries per table/box list, nesting
32, 16 MiB compressed packet, and 65536 frames per ALAC packet. Arithmetic is
checked before offset addition and duration multiplication.

Gapless output is a sample interval. Prefer one nonempty elst media edit:
media time gives the first sample; movie-timescale segment duration gives the
length. Leading empty edits are ignored for audio trimming. Otherwise iTunSMPB
provides delay/padding. Both ends are capped by stts duration converted from the
selected media timescale; no other track's duration is used.

Apple decoding supports 16/20/24/32 bits, with mono duplicated to stereo.
Its 20-bit output is already left-aligned in a 24-bit storage word; normalize
that word to int32. More than two channels fails with a clear STMn log.
Cookie frame lengths and initial packet partial-frame counts are checked before
calling Apple. Standard SCE/CPE audio packet starts are supported; ancillary-first
packet layouts are rejected. Apple source is not modified or formatted.

Evidence: pinned `ALACMagicCookieDescription.txt`, `codec/ALACDecoder.h`,
`codec/ALACAudioTypes.h`, and `codec/matrix_dec.c` describe the cookie and sample
storage. LMS `Slim/Player/Squeezebox.pm:635–642,706–714` selects f/l with unknown
PCM fields; our own validated box parser supplies ALAC parameters.
Upstream ALAC has LICENSE and per-file notices but no NOTICE file. Its LICENSE
is retained in the submodule and reproduced in THIRD_PARTY_NOTICES; our
attribution notice is `third_party/ALAC_NOTICE`.

## AIFF, testing, and distribution

AIFF is advertised as `aif` but LMS sends it through wire code `p`
(`Slim/Player/Squeezebox.pm:611–633`). Reused round 1 container parsing detects
FORM/AIFF/AIFC even when strm provides known PCM fields. Integer uncompressed
AIFF and AIFC NONE/sowt are supported; WAV detection remains available too.

`make test` includes `make format-check`, all lifecycle regressions, bit-exact
FLAC/ALAC references, gapless MP3 playback, random 1-byte–64-KiB feeds, moov
orders, co64, chunk gaps/runs, unrelated tracks, both trimming sources, 64-bit
metadata, and malformed MP4 under AddressSanitizer plus UndefinedBehaviorSanitizer.
Third-party codec sources are excluded from formatting and the demux-only
sanitizer binary. The sanitizer tests include deterministic malformed cases,
ten chunking seeds, and sixty byte mutations; this is not a general security
claim about third-party compressed-bitstream decoders.

Use ffmpeg/flac for generated reference fixtures when available. Otherwise use
small committed synthetic/reference fixtures and print SKIP with the reason for
skipping generation. Missing clang-format is a test failure, not a silent pass.
All network tests stay on localhost; no production LMS or deployment is used.
Clone with `--recurse-submodules`, and distribute THIRD_PARTY_NOTICES plus the
ALAC licence/attribution alongside binaries. YeneY and LampaStream are unchanged.

Run `make decoder-benchmark && ./decoder-benchmark` for five trials over both
committed MP3 fixtures (100 repetitions per trial). It reports process CPU and
wall time for the public decoder pipeline, including worker and queue overhead;
no PCM file writing is timed. Gapless counts are checked in every trial.

The cross-repository SHM test normally finds a sibling LampaStream checkout.
Set `LAMPASTREAM_CHECKOUT=/path/to/LampaStream` to use another real checkout,
including a temporary clone; its consumer remains unmodified.
