#!/usr/bin/env python3
# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Independent SHM reader plus the unmodified LampaStream production consumer."""
import mmap
import os
import select
from pathlib import Path
import struct
import sys
import time
import unittest
from fake_lms import Session, pcm

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<IIB3xIq')
EXTENSION = struct.Struct('<IHHIQQQ4x')


def snapshot(path):
    # Reopen is intentional: validates backing file size independently of the C layout.
    with open(path, 'rb') as f:
        if os.fstat(f.fileno()).st_size != 32888:
            raise AssertionError('SHM size is not 32888')
        with mmap.mmap(f.fileno(), 32888, access=mmap.ACCESS_READ) as mm:
            for _ in range(100):
                first = struct.unpack_from('<I', mm, 32856)[0]
                if first & 1:
                    continue
                raw = mm[:]
                last = struct.unpack_from('<I', mm, 32856)[0]
                if first == last:
                    h = HEADER.unpack_from(raw, 56)
                    e = EXTENSION.unpack_from(raw, 32848)
                    assert e[:3] == (0x48555345, 1, 0), e
                    assert h[0] == 16384 and h[1] == (e[5] * 2) % 16384
                    return raw, h, e
    raise AssertionError('no stable SHM snapshot')


def converted(data):
    out = bytearray()
    for (sample,) in struct.iter_unpack('<i', data):
        value = (abs(sample) + 32768) // 65536
        if sample < 0:
            value = -value
        out += struct.pack('<h', max(-32768, min(32767, value)))
    return bytes(out)


class ShmTests(unittest.TestCase):
    counter = 0

    def session(self, **kw):
        ShmTests.counter += 1
        mac = '02:3a:' + ':'.join(f'{v:02x}' for v in os.getpid().to_bytes(3, 'big')) + f':{self.counter:02x}'
        path = Path('/dev/shm/squeezelite-' + mac)
        self.addCleanup(lambda: path.unlink(missing_ok=True))
        return Session(app=True, sink='shm', mac=mac, **kw), path, mac

    def test_01_drop_in_lifecycle_and_quiet(self):
        s, path, _ = self.session(app_args=['-v', '-o', 'hw:CARD=Dummy,DEV=0'])
        with s:
            _, _, initial = snapshot(path)
            body, _ = pcm(seconds=1.5, bits=24)
            s.lms.strm('s', s.source(body), bits=24)
            s.lms.wait('STMs')
            s.lms.strm('p')
            s.lms.wait('STMp')
            _, paused, ext = snapshot(path)
            self.assertEqual(paused[2], 0)
            time.sleep(.08)
            self.assertEqual(snapshot(path)[2][5], ext[5])
            s.lms.strm('u')
            s.lms.wait('STMr')
            time.sleep(.03)
            self.assertEqual(snapshot(path)[1][2], 1)
            s.lms.strm('q')
            s.lms.wait('STMf')
            _, stopped, end = snapshot(path)
            self.assertEqual(stopped[2], 0)
            self.assertEqual(end[4], initial[4])
            self.assertGreater(end[5], 0)
            s.stop()
            log = s.base.with_suffix('.log').read_text()
            self.assertEqual(len(log.splitlines()), 1, log)
            self.assertIn('ignoring output device hw:CARD=Dummy,DEV=0', log)
        self.assertTrue(path.exists(), 'producer must not unlink on exit')

    def test_real_shm_underrun_rebuffers_once(self):
        s, path, _ = self.session()
        with s:
            s.lms.rebuffer_delay = .7
            body, _ = pcm(seconds=.35)
            s.lms.strm('s', s.source(body, first=4410 * 4, delay=.5))
            s.lms.wait('STMs')
            s.lms.wait('STMo')
            s.lms.wait('STMp')
            self.assertEqual(snapshot(path)[1][2], 0)
            s.lms.wait('STMr')
            s.lms.wait('STMu')
            s.stop()
            self.assertEqual(sum(p.get('event') == 'STMo' for p in s.lms.packets), 1)
            self.assertEqual(s.lms.rebuffer_commands, ['p', 'u'])

    def exercise_tracks(self, consumer=None):
        s, path, mac = self.session()
        with s:
            if consumer:
                source = consumer.SqueezeliteShmStereoSource()
                source.open(mac, require_v1=True)
                self.addCleanup(source.close)
            tracks = [pcm(44100, 24, .35), pcm(44100, 24, .3), pcm(48000, 24, .3)]
            expected = converted(b''.join(data for _, data in tracks))
            boundaries = [len(tracks[0][1]) // 8, (len(tracks[0][1]) + len(tracks[1][1])) // 8]
            previous = 0
            generation = snapshot(path)[2][4]
            seen = set()
            production_seen = set()
            invalidations = 0
            s.lms.strm('s', s.source(tracks[0][0]), bits=24)
            next_track = 1
            deadline = time.monotonic() + 8
            while previous < len(expected) // 4 and time.monotonic() < deadline:
                raw, h, ext = snapshot(path)
                pos = ext[5]
                self.assertEqual(ext[4], generation)
                self.assertEqual(ext[6], 0)
                self.assertLess(pos - previous, 8192)
                if pos > previous:
                    indices = [i % 8192 for i in range(previous, pos)]
                    data = b''.join(raw[80 + i * 4:84 + i * 4] for i in indices)
                    self.assertEqual(data, expected[previous * 4:pos * 4])
                    previous = pos
                    seen.add(h[3])
                if consumer:
                    result = source.read()
                    if isinstance(result, consumer.DataResult):
                        f = result.frame
                        position = f.source_sample_pos
                        data = (f.samples * 32768).astype('<i2').tobytes()
                        self.assertEqual(data, expected[position * 4:(position + len(f.samples)) * 4])
                        production_seen.add(f.sample_rate)
                        # Same-rate gapless boundary must be received, not invalidated/lost.
                        if position <= boundaries[0] < position + len(f.samples):
                            production_seen.add('gapless')
                    elif isinstance(result, consumer.StreamInvalidated):
                        invalidations += 1
                # Dispatch next strm after STMd, while prior audio is still playing.
                packet = s.lms.read() if select.select([s.lms.connection], [], [], 0)[0] else None
                if packet and packet.get('event') == 'STMd' and next_track < len(tracks):
                    rate = 44100 if next_track == 1 else 48000
                    s.lms.strm('s', s.source(tracks[next_track][0]), bits=24, rate=rate)
                    next_track += 1
                time.sleep(.002)
            self.assertEqual(previous, len(expected) // 4)
            self.assertEqual(seen, {44100, 48000})
            self.assertEqual(next_track, 3)
            if consumer:
                self.assertEqual(production_seen, {44100, 48000, 'gapless'})
                self.assertGreaterEqual(invalidations, 2) # running start and rate change
                source.close()
            if not any(p.get("event") == "STMu" for p in s.lms.packets):
                s.lms.wait("STMu")
            self.assertEqual(snapshot(path)[1][2], 0)
            s.stop()

    def test_02_independent_reader_gapless_and_rate(self):
        self.exercise_tracks()

    def test_03_real_lampastream_consumer(self):
        checkout = Path(os.environ.get('LAMPASTREAM_CHECKOUT', ROOT.parent / 'LampaStream'))
        if 'LAMPASTREAM_CHECKOUT' in os.environ:
            self.assertTrue(checkout.is_dir(), 'explicit LampaStream checkout missing')
        if not checkout.exists():
            alias = ROOT.parent / 'SqueezeHue'
            if alias.exists():
                import subprocess
                remote = subprocess.run(['git', '-C', str(alias), 'remote', 'get-url', 'origin'],
                                        capture_output=True, text=True).stdout
                if 'ThaYapeMan/LampaStream' in remote:
                    checkout = alias
        if not checkout.exists():
            print('SKIP cross-repo LampaStream consumer: sibling checkout missing', flush=True)
            self.skipTest('LampaStream checkout missing')
        sys.path.insert(0, str(checkout / 'src'))
        try:
            from lampastream import pcm_source
        except ImportError as e:
            print(f'SKIP cross-repo LampaStream consumer: Python dependency unavailable: {e}', flush=True)
            self.skipTest(str(e))
        print(f'Cross-repo consumer: {checkout}/src/lampastream/pcm_source.py (require_v1=True)', flush=True)
        self.exercise_tracks(pcm_source)


if __name__ == '__main__':
    unittest.main(verbosity=2)
