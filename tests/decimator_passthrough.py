#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Compare old decoder PCM with converted-path bypass, including WAV payload."""
import argparse, os, subprocess, tempfile, wave, struct, hashlib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--before',required=True,type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='yeney-hires-passthrough-') as tmp:
 d=Path(tmp)
 for rate in (22050,44100,48000):
  body=b''.join(struct.pack('<hh',(n*1237)%60000-30000,(n*4217)%60000-30000) for n in range(rate))
  with wave.open(str(d/'in.wav'),'wb') as w:w.setnchannels(2);w.setsampwidth(2);w.setframerate(rate);w.writeframes(body)
  subprocess.run([str(a.before),'a',str(d/'in.wav'),str(d/'old.raw')],check=True,capture_output=True)
  subprocess.run([str(root/'decoder-test'),'a',str(d/'in.wav'),str(d/'new.raw')],env=dict(os.environ,YENEY_TEST_DECIMATE='1'),check=True,capture_output=True)
  old=(d/'old.raw').read_bytes();new=(d/'new.raw').read_bytes();assert old==new
  # 24-bit queue WAV payload: the original 16-bit fixture occupies high bits.
  packed=b''.join(struct.pack('<i',x[0]>>8)[:3] for x in struct.iter_unpack('<i',new))
  assert packed==b''.join(b'\0'+body[i:i+2] for i in range(0,len(body),2))
  print(f'PASS before/after {rate} Hz PCM and 24-bit queue WAV payload byte-identical: SHA256 {hashlib.sha256(packed).hexdigest()}')
