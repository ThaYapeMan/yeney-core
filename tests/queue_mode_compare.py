#!/usr/bin/env python3
# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Capture with --binary /tmp/before/test-player --output /tmp/before-pcm,
then capture the rebuilt test-player and compare the two directories.
Only loopback fake LMS is used; existing helpers/tests are not modified.
PCM is compared directly, plus structural callbacks without wall timestamps.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import wave
from unittest.mock import patch
import fake_lms
from fake_lms import Session, SongQueue


def body(rate, seconds, seed):
    pcm = b''.join(struct.pack('<ii', ((i*137+seed)%1000001)*1024,
                              -((i*59+seed)%1000001)*1024)
                   for i in range(int(rate*seconds)))
    out = io.BytesIO()
    with wave.open(out, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(4); w.setframerate(rate); w.writeframes(pcm)
    return out.getvalue()


def capture(binary, directory):
    directory.mkdir(parents=True, exist_ok=True)
    cases = [(f'{mode}-type-{kind}', mode, [8000,8000], kind)
             for mode in ('normal','queue') for kind in range(5)]
    cases += [('stream-gapless','normal',[8000]*3,0),
              ('stream-rate-change','normal',[44100,48000,44100],0),
              ('queue-boundaries','queue',[8000]*3,0),
              ('queue-rate-change','queue',[44100,48000,44100],0)]
    manifest = {}
    with patch.object(fake_lms, 'ROOT', binary.resolve().parent):
        for name, mode, rates, kind in cases:
            with Session(mode=mode) as s:
                sources = [(s.source(body(rate, 2 if kind else .2, i+17)),
                            dict(rate=rate,bits=32,fmt='p',transition=kind,period=1))
                           for i,rate in enumerate(rates)]
                SongQueue(s.lms,sources); s.lms.wait('STMu',timeout=15)
                pcm=s.data(); assert pcm,name
                boundaries=[line for line in s.events() if line.startswith(('boundary ','item-end '))]
                (directory/(name+'.pcm')).write_bytes(pcm)
                manifest[name]=dict(bytes=len(pcm),sha256=hashlib.sha256(pcm).hexdigest(),boundaries=boundaries)
                print(f'CAPTURE {name}: bytes={len(pcm)} sha256={manifest[name]["sha256"]}',flush=True)
    (directory/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


def compare(before,after):
    a=json.loads((before/'manifest.json').read_text()); b=json.loads((after/'manifest.json').read_text())
    assert a.keys()==b.keys()
    for name in a:
        assert (before/(name+'.pcm')).read_bytes()==(after/(name+'.pcm')).read_bytes(),name
        assert a[name]==b[name],(name,a[name],b[name])
        print(f'PASS before/after {name}: byte-identical PCM and boundaries; {a[name]["bytes"]} bytes')
    print(f'PASS before/after queue extraction: {len(a)} deterministic fixtures identical')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    cap=commands.add_parser('capture');cap.add_argument('--binary',type=Path,required=True);cap.add_argument('--output',type=Path,required=True)
    cmp=commands.add_parser('compare');cmp.add_argument('before',type=Path);cmp.add_argument('after',type=Path)
    args=parser.parse_args()
    if args.command=='capture':capture(args.binary,args.output)
    else:compare(args.before,args.after)
