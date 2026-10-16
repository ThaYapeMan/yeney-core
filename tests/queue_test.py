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
from fake_lms import Session, SongQueue

class QueueTests(unittest.TestCase):
    def pair(self, kind, rate=8000, short=False, smart=False):
        n = 1600 if short else 12000
        with patch.dict(os.environ, {'YENEY_TEST_SMART': '1'} if smart else {}, clear=False), Session(mode='queue') as s:
            for r in (8000, rate):
                s.lms.strm('s', s.source(struct.pack('<ii', 100000000, -100000000) * n),
                           rate=r, bits=32, transition=kind, period=1)
                s.lms.wait('STMd')
            s.lms.wait('STMu', timeout=8)
            lines = [x.split() for x in s.events() if x.startswith('item-end ')]
            self.assertEqual([int(x[1]) for x in lines], [1, 2])
            end, total = int(lines[0][3]), int(lines[1][3])
            overlap = min(n - 1, 8000) if kind == 1 and rate == 8000 and not smart else 0
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
            s.lms.strm('s',s.source(data),rate=8000,bits=32,transition=1,period=1)
            s.lms.wait('STMd')
            s.lms.strm('s',s.source(data,first=16,delay=2),rate=8000,bits=32,transition=1,period=1)
            s.lms.wait('STMu',timeout=8)
            self.assertTrue(any('insufficient-incoming-pcm' in x for x in s.events()),s.events())
            self.assertEqual(len(s.data()),24000*8)
        print('PASS queue insufficient incoming PCM: unfaded join, no missing or duplicated frames')

    def test_lms_song_queue_two_ahead_and_unpause(self):
        with Session(mode='queue') as s:
            sources = [(s.source(struct.pack('<hh', i, -i)*2400), dict(rate=8000))
                       for i in range(1, 6)]
            model = SongQueue(s.lms, sources)
            s.lms.wait('STMu', timeout=12)
            starts = [p['playing_occurrence'] for p in s.lms.packets if p.get('event') == 'STMs']
            self.assertEqual(starts, [1, 2, 3, 4, 5])
            self.assertEqual(model.playing, 5)
            self.assertFalse(any(p.get('event') == 'STMr' for p in s.lms.packets))
            s.lms.strm('p'); s.lms.wait('STMp')
            s.lms.strm('u'); s.lms.wait('STMr')
            events = s.events()
            for i in range(1, 6):
                a = next(j for j,x in enumerate(events) if x.startswith(f'STMs {i} '))
                b = next(j for j,x in enumerate(events) if x.startswith(f'STMd {i} '))
                self.assertLess(a, b)
        print('PASS LMS song queue: playing equals audible across startup and five joins; two-ahead ceiling; STMr only on unpause')

    def test_alac_tail_seek_short_and_fact_e(self):
        root = Path(__file__).resolve().parent/'fixtures'
        import zlib
        reference = zlib.decompress((root/'alac-16-2.reference.zlib').read_bytes())
        for name, expected in [('regular', reference), ('seek', reference[4096*8:]),
                               ('short', reference[4096*8:8192*8] + bytes(3825*8))]:
            with self.subTest(name=name), Session(mode='queue') as s:
                SongQueue(s.lms, [(s.source((root/f'queue-alac-{name}.m4a').read_bytes()), dict(fmt='l'))])
                s.lms.wait('STMu', timeout=6)
                self.assertFalse(any(p.get('event') == 'STMn' for p in s.lms.packets))
                self.assertEqual(s.data(), expected)
                if name == 'short':
                    self.assertTrue(any('source-short 3825 frames' in x for x in s.events()))
        # A valid rewritten header with no decodable samples has no last
        # decoded frame after which to pad: it must never manufacture STMs.
        stream = (root/'queue-alac-seek.m4a').read_bytes()
        empty = stream[:stream.index(b'mdat') + 4]
        with Session(mode='queue') as s:
            SongQueue(s.lms, [(s.source(empty), dict(fmt='l')),
                              (s.source(struct.pack('<hh', 31, -31)*1600), dict(rate=8000))])
            s.lms.wait('STMu', timeout=6)
            self.assertEqual([p['playing_occurrence'] for p in s.lms.packets if p.get('event') == 'STMs'], [2])
            self.assertEqual(sum(p.get('event') == 'STMn' for p in s.lms.packets), 1)
            self.assertEqual(len(s.data()), 1600*8)
        print('PASS empty MP4 source: no silence-only promise or STMs; successor is the first audible occurrence')
        # Start immediately after HELO, fault with retained decoded PCM, then
        # successor / owner flush / replacement. A fault must drain before STMn.
        with Session(mode='queue') as s:
            raw = struct.pack('<hh', 17, -17)*2400 + b'x'
            model = SongQueue(s.lms, [(s.source(raw), dict(rate=8000)),
                                      (s.source(struct.pack('<hh', 23, -23)*2400), dict(rate=8000))])
            failed = s.lms.wait('STMn', timeout=6)
            starts = [p for p in s.lms.packets if p.get('event') == 'STMs']
            self.assertEqual([p['playing_occurrence'] for p in starts], [1])
            self.assertGreater(failed['time'] - starts[0]['time'], .25)
            s.lms.song_queue = None
            s.lms.wait('RESP')
            s.lms.strm('f'); s.lms.wait('STMf')
            model = SongQueue(s.lms, [(s.source(struct.pack('<hh', 29, -29)*2400), dict(rate=8000))], first=3)
            s.lms.wait('STMu', timeout=6)
            self.assertEqual([p['playing_occurrence'] for p in s.lms.packets if p.get('event') == 'STMs'], [1,3])
            self.assertEqual(model.playing, 3)
        print('PASS queue ALAC: regular trailing box, rewritten seek mdat, exact remaining count and bounded source-short padding')
        print('PASS fact-E recovery: HELO start, drain old PCM before STMn, successor, flush, replacement; no empty or stale STMs')

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
            for codec,data in [('p',buf.getvalue()),('a',aiff),('p',buf.getvalue()+b'LIST'+struct.pack('<I',3)+b'abc\0'),('a',aiff+b'ANNO'+struct.pack('>I',3)+b'abc\0')]:
                data=data[:4]+struct.pack('<I' if codec=='p' else '>I',len(data)-8)+data[8:]
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
            self.assertTrue(any(p.get('event') == 'STMs' for p in s.lms.packets))
            s.lms.wait('STMu', timeout=6)
            events = s.events()
            self.assertLess(next(i for i,x in enumerate(events) if x.startswith('STMs 1 ')), events.index('STMd 1 1600'))
            self.assertEqual(len([x for x in events if x.startswith('strm ')]), 2)
            self.assertEqual(len([x for x in events if x.startswith('STMs ')]), 2)
            print('PASS queue SlimProto: STMs before STMd, STMs only from explicit audible signals, no CLI or double advance')

if __name__ == '__main__': unittest.main(verbosity=2)
