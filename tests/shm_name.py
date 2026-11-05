# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Read the producer's single SHM prefix constant for Python verification."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SEGMENT_PREFIX = re.search(
    r'kSegmentPrefix\[\] = "([^"]+)";',
    (ROOT / 'sinks/shm_v1/sink.h').read_text(),
).group(1)


def segment_path(mac):
    return Path('/dev/shm') / (SEGMENT_PREFIX.lstrip('/') + mac)
