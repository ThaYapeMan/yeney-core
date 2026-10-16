# Decoder fixtures

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

Copyright (c) 2026 Jaap van Vliet. Independently generated synthetic signals
from `signal_data` in `tests/decoders_test.py`, encoded with ffmpeg 7.0.2
(ALAC and libmp3lame) and flac 1.5.0. Zlib files contain independent reference
decodes: normalized 32-bit stereo for FLAC/ALAC and signed 16-bit stereo for
MP3. These small fallbacks are used when fixture-generation tools are missing.
The JSON records sample counts and FLAC frame-aligned seek offsets.

The `.float-reference.zlib` MP3 references contain stereo IEEE float32 PCM
decoded independently with FFmpeg 7.0.2 (`-c:a mp3float -f f32le`). They retain
the same 7,013 and 8,006 gapless frames as the older int16 references. Decoder
precision assertions use these float references, without quantising core output.

Queue field regressions are independently rewritten from `alac-16-2.m4a`:
`queue-alac-regular.m4a` has 12,017 exact frames and a trailing free box;
`queue-alac-seek.m4a` has a rewritten sample table for the last 7,921 frames,
with the original (longer) mdat length retained. `queue-alac-short.m4a` has the
same 7,921-frame promise but only its first 4,096 frames of complete samples.
These model LMS header rewriting; no owner media or raw field logs are included.
