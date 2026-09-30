# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
"""Independent generated signal/reference and malformed-container tests."""
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from fake_lms import Session

ROOT = Path(__file__).resolve().parents[1]
FFMPEG = shutil.which('ffmpeg')
FLAC = shutil.which('flac')
if '--fixtures' in sys.argv:
    sys.argv.remove('--fixtures')
    FFMPEG = FLAC = None
U32 = lambda n: struct.pack('!I', n)
U64 = lambda n: struct.pack('!Q', n)


def run(args, **kw):
    result = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if result.returncode:
        raise AssertionError(f'{args}: {result.returncode}\n{result.stdout.decode(errors="replace")}\n{result.stderr.decode(errors="replace")}')
    return result


def box(tag, data=b'', wide=False, end=False):
    return (U32(0 if end else 1 if wide else len(data) + 8) + tag.encode() + (U64(len(data) + 16) if wide else b'') + data)


def child_boxes(data, start=0):
    result = []
    while start < len(data):
        n, tag = struct.unpack_from('!I4s', data, start)
        header = 8
        if n == 1:
            n = struct.unpack_from('!Q', data, start + 8)[0]
            header = 16
        if n == 0:
            n = len(data) - start
        assert n >= header and start + n <= len(data)
        result.append((tag.decode(), data[start + header:start + n]))
        start += n
    return result


def transform(data, changes):
    containers = {'moov', 'trak', 'mdia', 'minf', 'stbl', 'edts', 'udta'}
    out = bytearray()
    for tag, payload in child_boxes(data):
        if tag in containers:
            payload = transform(payload, changes)
        if tag in changes:
            replacement = changes[tag](payload)
            if replacement is None:
                continue
            payload = replacement
        out.extend(box(tag, payload))
    return bytes(out)


def alac_parts(file):
    top = child_boxes(file)
    movie = next(p for t, p in top if t == 'moov')
    mdat = next(p for t, p in top if t == 'mdat')
    cookie = None
    sizes = None
    def visit(data):
        nonlocal cookie, sizes
        for tag, p in child_boxes(data):
            if tag in {'trak', 'mdia', 'minf', 'stbl'}:
                visit(p)
            if tag == 'stsd':
                entry = next(p for t, p in child_boxes(p, 8) if t == 'alac')
                cookie = next(p[4:28] for t, p in child_boxes(entry, 28) if t == 'alac')
            if tag == 'stsz':
                size, count = struct.unpack_from('!II', p, 4)
                sizes = [size] * count if size else list(struct.unpack_from('!' + 'I' * count, p, 12))
    visit(movie)
    packets = []
    at = 0
    for size in sizes:
        packets.append(mdat[at:at + size])
        at += size
    assert at == len(mdat)
    return cookie, packets


def mp4(cookie, packets, frames, co64=False, last=False, trim=None, smpb=None, second=False, wide=False, end=False, gaps=False, backwards=False):
    # Our fixture writer deliberately uses a new chunk for every packet and
    # changing stsc runs for a separate variant, independent of the demuxer.
    rate = struct.unpack_from('!I', cookie, 20)[0]
    channels, bits = cookie[9], cookie[5]
    chunk_groups = [packets[:2]] + [[p] for p in packets[2:]] if len(packets) > 2 else [[p] for p in packets]
    media = bytearray()
    relative = []
    for group in chunk_groups:
        if gaps:
            media.extend(b'GAP!\0')
        relative.append(len(media))
        for packet in group:
            media.extend(packet)
    ftyp = box('ftyp', b'M4A \0\0\0\0M4A isom')
    def movie(base):
        sample_entry = bytes(6) + struct.pack('!HHHIHHHHI', 1, 0, 0, 0, channels, bits, 0, 0, rate << 16)
        assert len(sample_entry) == 28
        stsd = box('stsd', bytes(4) + U32(1) + box('alac', sample_entry + box('alac', bytes(4) + cookie)))
        stsz = box('stsz', bytes(4) + U32(0) + U32(len(packets)) + b''.join(U32(len(p)) for p in packets))
        per_packet = struct.unpack_from('!I', cookie)[0]
        durations = [per_packet] * (len(packets) - 1) + [frames - per_packet * (len(packets) - 1)]
        stts = box('stts', bytes(4) + U32(len(packets)) + b''.join(U32(1) + U32(n) for n in durations))
        runs = [(1, len(chunk_groups[0]), 1)]
        if len(chunk_groups) > 1 and len(chunk_groups[0]) != 1:
            runs.append((2, 1, 1))
        stsc = box('stsc', bytes(4) + U32(len(runs)) + b''.join(struct.pack('!III', *r) for r in runs))
        offsets = [base + pos for pos in relative]
        if backwards and len(offsets) > 1:
            offsets[1] = offsets[0]
        offset_box = box('co64' if co64 else 'stco', bytes(4) + U32(len(offsets)) + b''.join((U64 if co64 else U32)(p) for p in offsets))
        mdhd = box('mdhd', bytes(12) + U32(rate) + U32(frames) + bytes(4))
        hdlr = box('hdlr', bytes(8) + b'soun' + bytes(12))
        stbl = box('stbl', stsd + stts + stsc + stsz + offset_box)
        edts = b''
        if trim is not None:
            delay, count = trim
            edts = box('edts', box('elst', bytes(4) + U32(1) + struct.pack('!IiI', count, delay, 0x10000)))
        trak = box('trak', edts + box('mdia', mdhd + hdlr + box('minf', stbl)))
        # A preceding video track has intentionally unrelated durations/tables.
        video = box('trak', box('mdia', box('hdlr', bytes(8) + b'vide' + bytes(12)) + box('minf', box('stbl', box('stsz', bytes(4) + U32(7) + U32(1)))))) if second else b''
        metadata = b''
        if smpb:
            delay, padding = smpb
            text = f' 00000000 {delay:08X} {padding:08X} {frames:016X}'.encode()
            item = box('----', box('mean', bytes(4) + b'com.apple.iTunes') + box('name', bytes(4) + b'iTunSMPB') + box('data', bytes(8) + text))
            metadata = box('udta', box('meta', bytes(4) + box('ilst', item)))
        mvhd = box('mvhd', bytes(12) + U32(rate) + U32(frames) + bytes(80))
        return box('moov', mvhd + video + trak + metadata, wide=wide)
    if last:
        media_box = box('mdat', bytes(media), wide=wide)
        return ftyp + media_box + movie(len(ftyp) + (16 if wide else 8))
    provisional = movie(0)
    movie_box = movie(len(ftyp) + len(provisional) + (16 if wide else 8))
    return ftyp + movie_box + box('mdat', bytes(media), wide=wide, end=end)


def signal_data(count, bits=16, channels=2, start=0):
    values = []
    raw = bytearray()
    for i in range(start, start + count):
        row = []
        for c in range(channels):
            value = round((math.sin(i * .093 + c * .4) * .31 + math.sin(i * .171) * .09) * (1 << (bits - 1)))
            row.append(value << (32 - bits))
            raw.extend(value.to_bytes(bits // 8, 'little', signed=True))
        values.extend([row[0], row[-1]])
    return bytes(raw), struct.pack('<' + 'i' * len(values), *values)


class DecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='yeney-decoders-')
        cls.dir = Path(cls.temp.name)
        cls.alac = {}
        if FFMPEG:
            for bits, channels in [(16, 2), (24, 2), (16, 1)]:
                raw, expected = signal_data(12017, bits, channels)
                source = cls.dir / f'{bits}-{channels}.raw'
                source.write_bytes(raw)
                dest = cls.dir / f'{bits}-{channels}.m4a'
                run([FFMPEG, '-v', 'error', '-f', f's{bits}le', '-ar', '44100', '-ac', channels, '-i', source, '-c:a', 'alac', '-y', dest])
                reference = cls.dir / f'{bits}-{channels}.ref'
                run([FFMPEG, '-v', 'error', '-i', dest, '-f', 's32le', '-af', 'pan=stereo|c0=c0|c1=c0' if channels == 1 else 'anull', '-ac', '2', '-y', reference])
                assert reference.read_bytes() == expected
                cls.alac[bits, channels] = (dest.read_bytes(), expected)
        else:
            for bits, channels in [(16, 2), (24, 2), (16, 1)]:
                file = ROOT / 'tests/fixtures' / f'alac-{bits}-{channels}.m4a'
                if file.exists():
                    cls.alac[bits, channels] = (file.read_bytes(), zlib.decompress(file.with_suffix('.reference.zlib').read_bytes()))
            print('SKIP generated ALAC/MP3 reference fixtures: ffmpeg unavailable or generation disabled; using committed fixtures where present', flush=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def decode(self, codec, data, expected=None, cap=None):
        file = self.dir / 'input.bin'
        output = self.dir / 'output.pcm'
        file.write_bytes(data)
        args = [ROOT / 'decoder-test', codec, file, output]
        if cap is not None:
            args.append(cap)
        result = run(args)
        pcm = output.read_bytes()
        if expected is not None:
            self.assertEqual(pcm, expected)
        return pcm, result.stdout.decode()

    def demux(self, data, error=None, cap=None, seed=42):
        file = self.dir / 'container.bin'
        file.write_bytes(data)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
        result = subprocess.run([str(ROOT / 'demux-sanitized'), str(file), str(cap or 256 * 1024 * 1024), str(seed)], capture_output=True, env=env)
        stderr = result.stderr.decode(errors='replace')
        self.assertNotIn('AddressSanitizer', stderr)
        self.assertNotIn('runtime error:', stderr)
        if error:
            self.assertEqual(result.returncode, 2, stderr)
            self.assertIn(error, stderr)
        else:
            self.assertEqual(result.returncode, 0, stderr)
        return result.stdout.decode()

    def test_01_flac_bit_exact_and_headerless_seek(self):
        if not FLAC:
            print('SKIP generated FLAC reference/seek fixtures: flac CLI unavailable or generation disabled; testing committed references', flush=True)
            manifest = json.loads((ROOT / 'tests/fixtures/manifest.json').read_text())
            for entry in manifest['flac']:
                file = ROOT / 'tests/fixtures' / entry['file']
                reference = zlib.decompress(file.with_suffix('.reference.zlib').read_bytes())
                self.decode('f', file.read_bytes(), reference)
                self.decode('f', file.read_bytes()[entry['seekOffset']:], reference[entry['seekFrames'] * 8:])
            return
        for bits, channels in [(16, 2), (24, 2), (16, 1)]:
            with self.subTest(bits=bits, channels=channels):
                raw, expected = signal_data(18000, bits, channels)
                source = self.dir / 'flac.raw'
                source.write_bytes(raw)
                dest = self.dir / 'signal.flac'
                run([FLAC, '--silent', '--force', '--force-raw-format', '--endian=little', '--sign=signed', f'--channels={channels}', f'--bps={bits}', '--sample-rate=44100', '-o', dest, source])
                reference = self.dir / 'flac-reference.raw'
                run([FLAC, '--silent', '--force', '--decode', '--force-raw-format', '--endian=little', '--sign=signed', '-o', reference, dest])
                self.assertEqual(reference.read_bytes(), raw)
                self.decode('f', dest.read_bytes(), expected)
                ana = self.dir / 'signal.ana'
                run([FLAC, '--silent', '--force', '--analyze', '-o', ana, dest])
                entries = re.findall(r'frame=(\d+)\s+offset=(\d+)\s+bits=\d+\s+blocksize=(\d+)', ana.read_text())
                self.assertGreater(len(entries), 1)
                offset = int(entries[1][1])
                start = int(entries[0][2])
                self.decode('f', dest.read_bytes()[offset:], expected[start * 8:])

    def test_02_alac_reference_random_feeds(self):
        if not self.alac:
            self.skipTest('SKIP: no ALAC reference fixture available')
        for (bits, channels), (data, expected) in self.alac.items():
            with self.subTest(bits=bits, channels=channels):
                self.decode('l', data, expected)
                cookie, packets = alac_parts(data)
                for options in [dict(), dict(last=True), dict(co64=True, second=True, gaps=True), dict(wide=True), dict(end=True)]:
                    variant = mp4(cookie, packets, len(expected) // 8, **options)
                    self.demux(variant)
                    self.decode('l', variant, expected)

    def test_03_alac_gapless_elst_and_smpb(self):
        if not self.alac:
            self.skipTest('SKIP: no ALAC fixture for trimming')
        data, expected = self.alac[16, 2]
        cookie, packets = alac_parts(data)
        total = len(expected) // 8
        for options, begin, end in [(dict(trim=(117, total - 350), smpb=(900, 900)), 117, total - 233), (dict(smpb=(113, 219)), 113, total - 219), (dict(trim=(total + 10, 999)), total, total)]:
            variant = mp4(cookie, packets, total, **options)
            text = self.demux(variant)
            self.assertIn(f'TRIM {total} {begin} {end}', text)
            self.decode('l', variant, expected[begin * 8:end * 8])

    def test_04_malformed_mp4_sanitizers(self):
        if self.alac:
            data, expected = self.alac[16, 2]
            cookie, packets = alac_parts(data)
        else:
            cookie = U32(64) + bytes([0, 16, 40, 10, 14, 1]) + bytes(10) + U32(44100)
            packets, expected = [bytes(4), bytes(4)], bytes(128 * 8)
        valid = mp4(cookie, packets, len(expected) // 8, co64=True)
        for n in [1, 7, 15, len(valid) - 1]:
            self.demux(valid[:n], 'truncated')
        self.demux(U32(4) + b'free', 'smaller')
        self.demux(U32(1) + b'free' + U64(12), 'smaller')
        self.demux(U32(1) + b'free' + U64(2 ** 64 - 1), 'truncated')
        self.demux(U32(1) + b'moov' + U64(2 ** 63), 'metadata cap')
        for tag in ['stts', 'stsc', 'stco', 'co64', 'stsz', 'stsd', 'elst']:
            def oversized(p, tag=tag):
                p = bytearray(p)
                struct.pack_into('!I', p, 8 if tag == 'stsz' else 4, 0xffffffff)
                return bytes(p)
            variant = mp4(cookie, packets, len(expected) // 8, co64=tag == 'co64', trim=(0, len(expected) // 8))
            changed = transform(variant, {tag: oversized})
            self.demux(changed, 'count')
        self.demux(mp4(cookie, packets, len(expected) // 8, backwards=True), 'backwards')
        self.demux(mp4(cookie, packets, len(expected) // 8, last=True), 'buffering cap', cap=1)
        # size==0 moov and parent-bound child sizes; 64-bit boxes arriving bytewise.
        top = child_boxes(mp4(cookie, packets, len(expected) // 8, last=True))
        to_end = b''.join(box(t, p, end=t == 'moov') for t, p in top)
        self.demux(to_end)
        for seed in range(10):
            self.demux(valid, seed=seed)
        rng = random.Random(11)
        for _ in range(60):
            mutated = bytearray(valid)
            pos = rng.randrange(len(mutated))
            mutated[pos] ^= 0xff
            file = self.dir / 'fuzz.bin'
            file.write_bytes(mutated)
            result = subprocess.run([str(ROOT / 'demux-sanitized'), str(file)], capture_output=True, env=dict(os.environ, ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
            self.assertIn(result.returncode, [0, 2], result.stderr)
            self.assertNotIn(b'AddressSanitizer', result.stderr)
            self.assertNotIn(b'runtime error:', result.stderr)

    def assert_mp3_precision(self, actual, expected, data):
        self.assertEqual(len(actual), len(expected))
        energy = sum(x*x for x in expected)
        error = sum((x-y)**2 for x,y in zip(actual,expected))
        snr = 10*math.log10(energy/error) if error else math.inf
        self.assertGreater(snr, 110)
        values = [v[0] for v in struct.iter_unpack('<i', data)]
        self.assertTrue(any(v & 0xffff for v in values))
        quantised = [max(-32768,min(32767,math.copysign(math.floor(abs(v)/65536+.5),v)))*65536 for v in values]
        self.assertNotEqual(values, quantised)
        print(f'PASS MP3 float reference: SNR={snr:.2f} dB (>110); low bits retained; int16 round trip changes samples', flush=True)

    def test_05_mp3_gapless_continuous_signal(self):
        if not FFMPEG:
            print('SKIP generated MP3 gapless reference: ffmpeg unavailable or generation disabled; testing both committed track references', flush=True)
            decoded, references = bytearray(), bytearray()
            for start, count in [(0, 7013), (7013, 8006)]:
                file = ROOT / 'tests/fixtures' / f'gapless-{start}.mp3'
                pcm, _ = self.decode('m', file.read_bytes())
                self.assertEqual(len(pcm), count * 8)
                decoded.extend(pcm)
                references.extend(zlib.decompress(file.with_suffix('.float-reference.zlib').read_bytes()))
            actual = [v[0] / 2147483648.0 for v in struct.iter_unpack('<i', decoded)]
            expected = [v[0] for v in struct.iter_unpack('<f', references)]
            self.assertEqual(len(decoded), 15019 * 8)
            self.assert_mp3_precision(actual, expected, decoded)
            print('PASS committed MP3 gapless: 7013 + 8006 = 15019 frames', flush=True)
            with Session() as s:
                first = (ROOT / 'tests/fixtures/gapless-0.mp3').read_bytes()
                second = (ROOT / 'tests/fixtures/gapless-7013.mp3').read_bytes()
                s.lms.strm('s', s.source(first), fmt='m')
                s.lms.wait('STMd')
                s.lms.strm('s', s.source(second), fmt='m')
                s.lms.wait('STMu')
                self.assertEqual(s.data(), bytes(decoded))
                self.assertIn('boundary 7013 44100 32 2 1', s.events())

            return
        split, total = 7013, 15019
        joined, reference = bytearray(), bytearray()
        encoded_tracks = []
        for start, count in [(0, split), (split, total - split)]:
            raw, _ = signal_data(count, start=start)
            source = self.dir / 'mp3.raw'
            source.write_bytes(raw)
            dest = self.dir / f'{start}.mp3'
            run([FFMPEG, '-v', 'error', '-f', 's16le', '-ar', '44100', '-ac', '2', '-i', source, '-c:a', 'libmp3lame', '-b:a', '192k', '-y', dest])
            ref = self.dir / 'mp3.ref'
            run([FFMPEG, '-v', 'error', '-c:a', 'mp3float', '-i', dest, '-f', 'f32le', '-y', ref])
            encoded_tracks.append(dest.read_bytes())
            decoded, _ = self.decode('m', dest.read_bytes())
            self.assertEqual(len(decoded), count * 8)
            joined.extend(decoded)
            reference.extend(ref.read_bytes())
        self.assertEqual(len(joined), total * 8)
        actual = [v[0] / 2147483648.0 for v in struct.iter_unpack('<i', joined)]
        expected = [v[0] for v in struct.iter_unpack('<f', reference)]
        self.assertEqual(len(actual), len(expected))
        self.assert_mp3_precision(actual, expected, joined)
        print(f'PASS MP3 gapless: {split} + {total - split} = {total} frames; float reference', flush=True)
        with Session() as s:
            s.lms.strm('s', s.source(encoded_tracks[0]), fmt='m')
            s.lms.wait('STMd')
            s.lms.strm('s', s.source(encoded_tracks[1]), fmt='m')
            s.lms.wait('STMu')
            self.assertEqual(s.data(), bytes(joined))
            boundaries = [e for e in s.events() if e.startswith('boundary')]
            self.assertEqual(boundaries, ['boundary 0 44100 32 2 0', f'boundary {split} 44100 32 2 1'])


    def test_10_committed_mp3_precision(self):
        for start, count in ((0, 7013), (7013, 8006)):
            file = ROOT / 'tests/fixtures' / f'gapless-{start}.mp3'
            decoded, description = self.decode('m', file.read_bytes())
            self.assertEqual(len(decoded), count * 8)
            self.assertIn('FORMAT 44100 32 2', description)
            actual = [v[0] / 2147483648.0 for v in struct.iter_unpack('<i', decoded)]
            reference = zlib.decompress(file.with_suffix('.float-reference.zlib').read_bytes())
            expected = [v[0] for v in struct.iter_unpack('<f', reference)]
            self.assert_mp3_precision(actual, expected, decoded)

    def test_06_fake_lms_decoder_dispatch(self):
        raw, expected = signal_data(3000)
        cases = [('p', raw, None)]
        if self.alac:
            data, ref = self.alac[16, 2]
            cases.append(('l', data, ref))
        if FLAC:
            source = self.dir / 'dispatch.raw'
            source.write_bytes(raw)
            dest = self.dir / 'dispatch.flac'
            run([FLAC, '--silent', '--force', '--force-raw-format', '--endian=little', '--sign=signed', '--channels=2', '--bps=16', '--sample-rate=44100', '-o', dest, source])
            cases.append(('f', dest.read_bytes(), expected))
        else:
            file = ROOT / 'tests/fixtures/flac-16-2.flac'
            cases.append(('f', file.read_bytes(), zlib.decompress(file.with_suffix('.reference.zlib').read_bytes())))
        if FFMPEG:
            source = self.dir / 'dispatch.raw'
            source.write_bytes(raw)
            dest = self.dir / 'dispatch.mp3'
            run([FFMPEG, '-v', 'error', '-f', 's16le', '-ar', '44100', '-ac', '2', '-i', source, '-c:a', 'libmp3lame', '-y', dest])
            ref, _ = self.decode('m', dest.read_bytes())
            cases.append(('m', dest.read_bytes(), ref))
        else:
            print('SKIP generated MP3 fake LMS dispatch: ffmpeg unavailable or generation disabled; testing committed stream', flush=True)
            file = ROOT / 'tests/fixtures/gapless-0.mp3'
            ref, _ = self.decode('m', file.read_bytes())
            cases.append(('m', file.read_bytes(), ref))
        for codec, data, expected in cases:
            with self.subTest(codec=codec), Session() as s:
                self.assertTrue(s.hello['body'].endswith(b'alc,flc,mp3,aif,pcm'))
                s.lms.strm('s', s.source(data), fmt=codec)
                s.lms.wait('STMu')
                if expected is not None:
                    self.assertEqual(s.data(), expected)
                else:
                    self.assertEqual(len(s.data()), len(data) * 2)

    def test_07_alac_20_32_and_channel_limit(self):
        # Independently authored ALAC escape packets: signed uncompressed sample
        # words provide exact extreme-value oracles for both unusual bit depths.
        for bits in [20, 32]:
            values = [-(1 << (bits - 1)), -1, 0, 1, (1 << (bits - 1)) - 1] * 13
            header = '000' + '0000' + '0' * 12 + '1001' + f'{len(values):032b}'
            words = ''.join(f'{v & ((1 << bits) - 1):0{bits}b}' for v in values)
            binary = header + words + '111'
            binary += '0' * ((-len(binary)) % 8)
            packet = int(binary, 2).to_bytes(len(binary) // 8, 'big')
            cookie = U32(4096) + bytes([0, bits, 40, 10, 14, 1]) + bytes(10) + U32(44100)
            expected = b''.join(struct.pack('<ii', v * (1 << (32 - bits)), v * (1 << (32 - bits))) for v in values)
            self.decode('l', mp4(cookie, [packet], len(values)), expected)
        data, expected = self.alac[16, 2]
        cookie, packets = alac_parts(data)
        cookie = bytearray(cookie)
        cookie[9] = 3
        invalid = mp4(cookie, packets, len(expected) // 8)
        file = self.dir / 'channels.m4a'
        file.write_bytes(invalid)
        result = subprocess.run([str(ROOT / 'decoder-test'), 'l', str(file), str(self.dir / 'channels.pcm')], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'more than 2 channels', result.stderr)
        with Session() as s:
            s.lms.strm('s', s.source(invalid), fmt='l')
            s.lms.wait('STMn')
            s.stop()
            self.assertIn('more than 2 channels', s.base.with_suffix('.log').read_text())

    def test_08_alac_moov_first_streams_before_eof(self):
        data, expected = self.alac[16, 2]
        cookie, packets = alac_parts(data)
        stream = mp4(cookie, packets, len(expected) // 8)
        with Session() as s:
            prefix = len(stream) - len(packets[-1])
            s.lms.strm('s', s.source(stream, first=prefix, delay=.35), fmt='l')
            s.lms.wait('STMs')
            self.assertFalse(any(p.get('event') == 'STMd' for p in s.lms.packets))
            s.lms.wait('STMu')
            self.assertEqual(s.data(), expected)

    def test_09_64bit_metadata_and_audio_entry_v1(self):
        data, expected = self.alac[16, 2]
        cookie, packets = alac_parts(data)
        total = len(expected) // 8
        original = mp4(cookie, packets, total, last=True, co64=True, trim=(83, total - 183))
        def version1_entry(payload):
            entries = []
            for tag, value in child_boxes(payload, 8):
                value = bytearray(value)
                struct.pack_into('!H', value, 8, 1)
                entries.append(box(tag, bytes(value[:28]) + bytes(16) + bytes(value[28:])))
            return payload[:8] + b''.join(entries)
        variant = transform(original, {
            'mvhd': lambda p: bytes([1, 0, 0, 0]) + bytes(16) + p[12:16] + U64(struct.unpack_from('!I', p, 16)[0]) + p[20:],
            'mdhd': lambda p: bytes([1, 0, 0, 0]) + bytes(16) + p[12:16] + U64(struct.unpack_from('!I', p, 16)[0]) + p[20:],
            'elst': lambda p: bytes([1, 0, 0, 0]) + U32(2) + U64(10) + U64(2 ** 64 - 1) + U32(0x10000) + U64(total - 183) + U64(83) + U32(0x10000),
            'stsd': version1_entry,
        })
        self.demux(variant)
        self.decode('l', variant, expected[83 * 8:(total - 100) * 8])


if __name__ == '__main__':
    unittest.main(verbosity=2)
