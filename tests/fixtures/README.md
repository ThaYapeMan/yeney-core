# Decoder fixtures

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

Copyright (c) 2026 Jaap van Vliet. Independently generated synthetic signals
from `signal_data` in `tests/decoders_test.py`, encoded with ffmpeg 7.0.2
(ALAC and libmp3lame) and flac 1.5.0. Zlib files contain independent reference
decodes: normalized 32-bit stereo for FLAC/ALAC and signed 16-bit stereo for
MP3. These small fallbacks are used when fixture-generation tools are missing.
The JSON records sample counts and FLAC frame-aligned seek offsets.
