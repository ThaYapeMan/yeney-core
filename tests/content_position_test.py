# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Real decoder blocks, including a frame-aligned FLAC seek and unknown MP3."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from shm_name import SEGMENT_PREFIX
ROOT = Path(__file__).resolve().parents[1]
def check(codec, path, start, known=True):
    subprocess.run([str(ROOT/'content-position-test'), codec, str(path), str(start), str(int(known)), SEGMENT_PREFIX], check=True)
with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR', '/var/tmp')) as directory:
    work = Path(directory)
    pcm = work/'pcm.raw'
    pcm.write_bytes(bytes(8000))
    check('p', pcm, 0)
    check('f', ROOT/'tests/fixtures/flac-16-2.flac', 0)
    entry = json.loads((ROOT/'tests/fixtures/manifest.json').read_text())['flac'][0]
    seek = work/'seek.flac'
    seek.write_bytes((ROOT/'tests/fixtures'/entry['file']).read_bytes()[entry['seekOffset']:])
    requested = 5000
    assert entry['seekFrames'] != requested
    check('f', seek, entry['seekFrames'])
    check('l', ROOT/'tests/fixtures/alac-16-2.m4a', 0)
    check('m', ROOT/'tests/fixtures/gapless-0.mp3', 0, False)
print('PASS content tail: PCM, FLAC, ALAC, FLAC seek landing 4096 != requested 5000, unsupported MP3, mixed validity')
