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
