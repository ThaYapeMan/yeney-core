# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Out-of-band moov preserves exact samples and validates its in-band copy."""
import pathlib, subprocess, struct
from decoders_test import alac_parts, mp4
ROOT=pathlib.Path(__file__).resolve().parents[1]
source=(ROOT/'tests/fixtures/alac-16-2.m4a').read_bytes()
cookie,packets=alac_parts(source)
# The committed fixture's packet table gives the authoritative sample count.
reference=subprocess.check_output([str(ROOT/'mp4-position-test'),'native','full'],input=source)
frames=struct.unpack_from('<Q',reference,16)[0]
for last in (False,True):
 for wide in (False,True):
  stream=mp4(cookie,packets,frames,last=last,wide=wide,co64=wide,complete=True)
  plain=subprocess.check_output([str(ROOT/'mp4-position-test'),'native','full'],input=stream)
  prefetched=subprocess.check_output([str(ROOT/'mp4-position-test'),'prefetch','full'],input=stream)
  assert plain==prefetched,(last,wide)
  assert struct.unpack_from('<Q',prefetched,24)[0]==0
  print(f'PASS C2 prefetched moov last={last} wide={wide}: exact decoded bytes and coordinates',flush=True)
stream=mp4(cookie,packets,frames,last=True,trim=(113,frames-219),complete=True)
plain=subprocess.check_output([str(ROOT/'mp4-position-test'),'native','full'],input=stream)
prefetched=subprocess.check_output([str(ROOT/'mp4-position-test'),'prefetch','full'],input=stream)
assert plain==prefetched
print('PASS C2 prefetched trimmed ALAC: exact samples and origin',flush=True)
stream=mp4(cookie,packets,frames,last=True,complete=True)
for mode in ('prefetch-mismatch','prefetch-truncated'):
 result=subprocess.check_output([str(ROOT/'mp4-position-test'),mode,'full'],input=stream)
 assert struct.unpack_from('<Q',result,24)[0]==1,mode
 print('PASS C2 '+mode+' rejected',flush=True)
