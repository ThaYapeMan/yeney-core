# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Queue-only mixer accounting and decode/audible protocol separation."""
import os
import struct
import io
import wave
import tempfile
import subprocess
from pathlib import Path
import unittest
from unittest.mock import patch
from fake_lms import Session

class QueueTests(unittest.TestCase):
    def pair(self, kind, rate=8000, short=False, smart=False):
        n = 1600 if short else 12000
        with patch.dict(os.environ, {'YENEY_TEST_SMART': '1'} if smart else {}, clear=False), Session(mode='queue') as s:
            for r in (8000, rate):
                s.lms.strm('s', s.source(struct.pack('<ii', 100000000, -100000000) * n),
                           rate=r, bits=32, transition=kind, period=1, autostart=0)
                s.lms.wait('STMd')
            self.assertFalse(any(p.get('event') == 'STMs' for p in s.lms.packets))
            s.lms.strm('u')
            s.lms.wait('STMu', timeout=8)
            lines = [x.split() for x in s.events() if x.startswith('item-end ')]
            self.assertEqual([int(x[1]) for x in lines], [1, 2])
            end, total = int(lines[0][3]), int(lines[1][3])
            overlap = min(n, 8000) if kind == 1 and rate == 8000 and not smart else 0
            self.assertEqual(int(lines[0][4]), overlap)
            self.assertEqual(end, n-overlap)
            self.assertEqual(total, 2*n-overlap)
            self.assertEqual(len(s.data()), total*8)
            self.assertEqual(len([p for p in s.lms.packets if p.get('event') == 'STMs']), 2)
            self.assertEqual(len([p for p in s.lms.packets if p.get('event') == 'STMd']), 2)
            if smart: self.assertEqual(lines[0][5], 'smart-same-album')
            if rate != 8000 and kind == 1: self.assertEqual(lines[0][5], 'different-rates')

    def test_types_rates_short_and_smart(self):
        for kind in range(5):
            for short in (False, True):
                with self.subTest(kind=kind, short=short): self.pair(kind, short=short)
        self.pair(1, rate=48000)
        self.pair(1, smart=True)
        print('PASS queue boundaries: types 0-4, short tracks, native rates and smart rule; item totals equal output')

    def test_insufficient_incoming(self):
        with Session(mode='queue') as s:
            data=struct.pack('<ii', 100000000,-100000000)*12000
            s.lms.strm('s',s.source(data),rate=8000,bits=32,transition=1,period=1,autostart=0)
            s.lms.wait('STMd')
            s.lms.strm('s',s.source(data,first=16,delay=.4),rate=8000,bits=32,transition=1,period=1)
            s.lms.wait('STMu',timeout=8)
            self.assertTrue(any('insufficient-incoming-pcm' in x for x in s.events()),s.events())
            self.assertEqual(len(s.data()),24000*8)
        print('PASS queue insufficient incoming PCM: unfaded join, no missing or duplicated frames')

    def test_pcm_exact_counts(self):
        n=1600
        audio=struct.pack('<hh',123,-456)*n
        buf=io.BytesIO()
        with wave.open(buf,'wb') as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(8000); w.writeframes(audio)
        comm=struct.pack('>HIH',2,n,16)+bytes.fromhex('400bfa00000000000000')
        ssnd=struct.pack('>II',0,0)+struct.pack('>hh',123,-456)*n
        body=b'AIFF'+b'COMM'+struct.pack('>I',len(comm))+comm+b'SSND'+struct.pack('>I',len(ssnd))+ssnd
        aiff=b'FORM'+struct.pack('>I',len(body))+body
        with tempfile.TemporaryDirectory() as d:
            for codec,data in [('p',buf.getvalue()),('a',aiff)]:
                src=Path(d)/'source'; dst=Path(d)/'pcm'; src.write_bytes(data)
                p=subprocess.run([str(Path(__file__).resolve().parents[1]/'decoder-test'),codec,str(src),str(dst)],capture_output=True,text=True,check=True)
                self.assertIn('EXACT 1600',p.stdout); self.assertEqual(len(dst.read_bytes()),n*8)
                with patch.dict(os.environ, {'YENEY_TEST_TRANSCODED':'1'}):
                    result=subprocess.run([str(Path(__file__).resolve().parents[1]/'decoder-test'),codec,str(src),str(dst)],capture_output=True,text=True,check=True)
                    self.assertIn('EXACT unknown',result.stdout)
        print('PASS exact WAV/AIFF source frame counts match decoded references; declared transcoding reports unknown')

    def test_decoder_ahead_audible_start(self):
        with Session(mode='queue') as s:
            for _ in range(2):
                s.lms.strm('s', s.source(struct.pack('<hh', 100, -100)*1600), rate=8000)
                s.lms.wait('STMd')
            self.assertFalse(any(p.get('event') == 'STMs' for p in s.lms.packets))
            s.lms.wait('STMu', timeout=6)
            events = s.events()
            self.assertLess(events.index('STMd 1 1600'), next(i for i,x in enumerate(events) if x.startswith('STMs 1 ')))
            self.assertEqual(len([x for x in events if x.startswith('strm ')]), 2)
            self.assertEqual(len([x for x in events if x.startswith('STMs ')]), 2)
            print('PASS queue SlimProto: STMd before audible, STMs only from explicit audible signals, no CLI or double advance')

if __name__ == '__main__': unittest.main(verbosity=2)
