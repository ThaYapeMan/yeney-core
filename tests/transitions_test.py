#!/usr/bin/env python3
# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Independent integer oracle, exercised through the localhost player pipeline."""
import struct
import time
import unittest
from fake_lms import Session

RATE = 8000


def signal(frames, left=100000001, right=-200000003):
    data = struct.pack('<ii', left, right) * frames
    return data, [(left, right)] * frames


def scale(sample, gain, envelope=65536):
    combined = (gain or 65536) * envelope // 65536
    return max(-2147483648, min(2147483647, sample * combined // 65536))


def pack(frames):
    return b''.join(struct.pack('<ii', *frame) for frame in frames)


def gain_frame(frame, gain=0, envelope=65536):
    return tuple(scale(sample, gain, envelope) for sample in frame)


class TransitionTests(unittest.TestCase):
    def play(self, transition, frames=12000, period=1, gain=0):
        body, source = signal(frames)
        with Session() as s:
            s.lms.strm('s', s.source(body), rate=RATE, bits=32, transition=transition, period=period, gain=gain)
            done = s.lms.wait('STMu')
            self.assertEqual(done['elapsed'], frames * 1000 // RATE)
            data = s.data()
            starts = [p for p in s.lms.packets if p.get('event') == 'STMs']
            self.assertEqual(len(starts), 1)
            self.assertLess(starts[0]['elapsed'], 30)
            return data, source

    def test_01_replay_gain_saturation_and_exact_boundary(self):
        for gain in [0, 65536, 26112, 32768, 131072, 0xffffffff]:
            body, source = signal(1200, 2000000001, -2000000003)
            with Session() as s:
                s.lms.strm('s', s.source(body), rate=RATE, bits=32, gain=gain)
                s.lms.wait('STMu')
                self.assertEqual(s.data(), pack([gain_frame(f, gain) for f in source]))
        with Session() as s:
            a, first = signal(1600)
            b, second = signal(1600)
            s.lms.strm('s', s.source(a), rate=RATE, bits=32, gain=26112, autostart=0)
            s.lms.wait('STMd')
            s.lms.strm('s', s.source(b), rate=RATE, bits=32, gain=131072, autostart=0)
            s.lms.wait('STMd')
            s.lms.send('audg', struct.pack('!IIBBII', 0, 0, 1, 0, 1, 1))
            s.lms.strm('u')
            s.lms.wait('STMu')
            self.assertEqual(s.data(), pack([gain_frame(f, 26112) for f in first] +
                                          [gain_frame(f, 131072) for f in second]))
            self.assertIn('boundary 1600 8000 32 2 1', s.events())

    def test_02_fade_curves_and_short_tracks(self):
        for kind in [0, 2, 3, 4]:
            for length in [12000, 1600]:
                gain = 26112 if length == 12000 else 0
                actual, source = self.play(kind, length, gain=gain)
                window = RATE // 2 if kind == 4 else RATE
                out_length = min(window, length)
                start = length - out_length
                expected = []
                for i, frame in enumerate(source):
                    envelope = 65536
                    if kind in [3, 4] and i >= start:
                        envelope = 65536 - (i - start) * 65536 // out_length
                    elif kind in [2, 4]:
                        envelope = min(65536, i * 65536 // min(window, length))
                    expected.append(gain_frame(frame, gain, envelope))
                self.assertEqual(actual, pack(expected), (kind, length))

    def cross(self, next_rate=RATE, next_frames=12000, mode='normal'):
        old, a = signal(12000)
        new, b = signal(next_frames, 300000007, -100000009)
        with Session(mode=mode) as s:
            s.lms.strm('s', s.source(old), rate=RATE, bits=32, transition=1, period=1, gain=32768, autostart=0)
            s.lms.wait('STMd')
            s.lms.strm('s', s.source(new), rate=next_rate, bits=32, transition=1, period=1, gain=131072, autostart=0)
            s.lms.wait('STMd')
            s.lms.strm('u')
            done = s.lms.wait('STMu')
            self.assertEqual(done['elapsed'], next_frames * 1000 // next_rate)
            actual = s.data()
            if next_rate != RATE:
                expected = [gain_frame(f, 32768) for f in a] + [gain_frame(f, 131072) for f in b]
                boundary = len(a)
            else:
                window = min(RATE, len(a), len(b))
                boundary = len(a) - window
                expected = [gain_frame(f, 32768) for f in a[:boundary]]
                for i in range(window):
                    up = i * 65536 // window
                    fa, fb = gain_frame(a[boundary + i], 32768, 65536 - up), gain_frame(b[i], 131072, up)
                    expected.append(tuple(max(-2147483648, min(2147483647, x + y)) for x, y in zip(fa, fb)))
                expected += [gain_frame(f, 131072) for f in b[window:]]
            self.assertEqual(actual, pack(expected))
            self.assertTrue(any(line.startswith(f'boundary {boundary} {next_rate} ') for line in s.events()))
            starts = [p for p in s.lms.packets if p.get('event') == 'STMs']
            self.assertEqual(len(starts), 2)
            self.assertLess(starts[1]['elapsed'], 30)
            self.assertGreaterEqual(starts[1]['time'] - starts[0]['time'], boundary / RATE - .12)

    def test_03_crossfade_sum_lengths_rates_and_audible_clock(self):
        self.cross()
        self.cross(next_frames=1600)
        self.cross(next_rate=44100)
        self.cross(mode='delayed')

    def test_04_pause_seek_stop_flush_during_overlap(self):
        for action in ['pause', 'seek', 'stop', 'flush']:
            with Session() as s:
                a, _ = signal(12000)
                b, _ = signal(12000, 300000007, -100000009)
                for body in [a, b]:
                    s.lms.strm('s', s.source(body), rate=RATE, bits=32, transition=1, period=1, autostart=0)
                    s.lms.wait('STMd')
                s.lms.strm('u')
                s.lms.wait('STMs')
                s.lms.wait('STMs')
                time.sleep(.1)
                if action == 'pause':
                    s.lms.strm('p'); s.lms.wait('STMp')
                    before = s.lms.timer()['elapsed']
                    time.sleep(.08)
                    self.assertEqual(s.lms.timer()['elapsed'], before)
                    s.lms.strm('u'); s.lms.wait('STMr'); s.lms.wait('STMu')
                    self.assertEqual(len(s.data()), 16000 * 8)
                elif action == 'flush':
                    s.lms.strm('f'); s.lms.wait('STMf'); s.lms.wait('STMu')
                    captured = s.data()
                    self.assertTrue(captured.endswith(b[-800 * 8:]))
                    self.assertEqual(len(captured), 16000 * 8)
                    self.assertEqual(captured[-9000 * 8:], b[-9000 * 8:])
                else:
                    s.lms.strm('q'); s.lms.wait('STMf')
                    if action == 'seek':
                        c, _ = signal(800, 123456789, -123456789)
                        s.lms.strm('s', s.source(c), rate=RATE, bits=32, gain=32768)
                        s.lms.wait('STMu')
                        self.assertTrue(s.data().endswith(pack([gain_frame((123456789, -123456789), 32768)] * 800)))
                    else:
                        first = s.lms.timer()['elapsed']; time.sleep(.05)
                        self.assertEqual(s.lms.timer()['elapsed'], first)


    def test_05_fade_interruptions(self):
        for kind in [2, 3, 4]:
            for action in ['pause', 'flush', 'stop', 'seek']:
                body, source = signal(12000)
                window = RATE // 2 if kind == 4 else RATE
                expected = []
                for i, frame in enumerate(source):
                    envelope = 65536
                    if kind in [3, 4] and i >= len(source) - window:
                        envelope -= (i - (len(source) - window)) * 65536 // window
                    elif kind in [2, 4]:
                        envelope = min(65536, i * 65536 // window)
                    expected.append(gain_frame(frame, 0, envelope))
                expected = pack(expected)
                with self.subTest(kind=kind, action=action), Session() as s:
                    s.lms.strm('s', s.source(body), rate=RATE, bits=32, transition=kind, period=1)
                    s.lms.wait('STMs')
                    time.sleep(.55 if kind == 3 else .1)
                    if action == 'pause':
                        s.lms.strm('p'); s.lms.wait('STMp')
                        elapsed = s.lms.timer()['elapsed']; time.sleep(.03)
                        self.assertEqual(s.lms.timer()['elapsed'], elapsed)
                        s.lms.strm('u'); s.lms.wait('STMr'); s.lms.wait('STMu')
                        self.assertEqual(s.data(), expected)
                    elif action == 'flush':
                        s.lms.strm('f'); s.lms.wait('STMf'); s.lms.wait('STMu')
                        self.assertEqual(s.data(), expected)
                    else:
                        s.lms.strm('q'); s.lms.wait('STMf')
                        if action == 'stop':
                            actual = s.data()
                            self.assertGreater(len(actual), 0)
                            self.assertLess(len(actual), len(expected))
                            self.assertTrue(expected.startswith(actual))
                        else:
                            next_body, next_frames = signal(800, 55555555, -55555555)
                            s.lms.strm('s', s.source(next_body), rate=RATE, bits=32, gain=32768)
                            s.lms.wait('STMu')
                            self.assertTrue(s.data().endswith(pack([gain_frame(f, 32768) for f in next_frames])))


if __name__ == '__main__':
    unittest.main(verbosity=2)
