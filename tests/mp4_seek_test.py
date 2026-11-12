# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Real LMS 9.1.1 loopback wire and in-memory PCM position oracle."""
import contextlib
import http.client
import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.parse import quote, unquote
import decoders_test
from decoders_test import alac_parts, mp4

ROOT = Path(__file__).resolve().parents[1]
LMS = Path(os.environ.get('YENEY_REAL_LMS_HOME', Path.home()/'.cache/yeney-test/real-lms-9.1.1'))
MAC = '02:00:00:ab:cd:01'

def wait(predicate):
    until = time.monotonic() + 45
    while time.monotonic() < until:
        try:
            result = predicate()
            if result:
                return result
        except OSError:
            pass
        time.sleep(.05)
    raise AssertionError('loopback LMS deadline')

def cli(command):
    with socket.create_connection(('127.0.0.1', 19090), timeout=10) as stream:
        stream.sendall((command+'\n').encode())
        data = b''
        while not data.endswith(b'\n'):
            block = stream.recv(65536)
            assert block, 'LMS CLI closed'
            data += block
    return unquote(data.decode()).strip()

def boxes(data, path=''):
    result = {}
    at = 0
    while at + 8 <= len(data):
        size, tag = struct.unpack_from('>I4s', data, at)
        header = 8
        if size == 1:
            size = struct.unpack_from('>Q', data, at+8)[0]
            header = 16
        if size == 0:
            size = len(data)-at
        if size < header or at+size > len(data):
            break
        tag = tag.decode('latin1')
        payload = data[at+header:at+size]
        key = path+'/'+tag
        result[key] = payload
        if tag in ('moov', 'trak', 'mdia', 'minf', 'stbl', 'edts', 'udta', 'ilst'):
            result.update(boxes(payload, key))
        if tag == 'meta':
            result.update(boxes(payload[4:], key))
        at += size
    return result

def origin(body):
    table = boxes(body)
    mdhd = table['/moov/trak/mdia/mdhd']
    scale, total = struct.unpack_from('>II', mdhd, 12)
    stts = table['/moov/trak/mdia/minf/stbl/stts']
    entries = struct.unpack_from('>I', stts, 4)[0]
    remaining = sum(a*b for a, b in struct.iter_unpack('>II', stts[8:8+entries*8]))
    return total-remaining, total, remaining, scale

def probe(body, mode='native', full=False):
    result = subprocess.run([str(ROOT/'mp4-position-test'), mode]+(['full'] if full else []),
                            input=body, capture_output=True, check=True, timeout=130)
    assert len(result.stdout) >= 32, result.stderr.decode()
    known, start, count, failed = struct.unpack('<QQQQ', result.stdout[:32])
    return known, start, count, failed, result.stdout[32:]

def reference(path):
    # Never persist decoded private PCM. FFmpeg is only an executable oracle.
    return subprocess.check_output(['ffmpeg', '-v', 'quiet', '-i', str(path), '-f', 's32le', '-acodec', 'pcm_s32le', '-'])

def generated(path, rate, bits):
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                    f'anoisesrc=r={rate}:d=61:seed=7123:a=0.4', '-ac', '2', '-c:a', 'alac',
                    '-sample_fmt', 's16p' if bits == 16 else 's32p', str(path)], check=True)
    cookie, packets = alac_parts(path.read_bytes())
    # Our existing public container writer makes packet tables easy to inspect.
    # This also avoids LMS's independent eight-byte stale free-box offset bug
    # on unmodified FFmpeg faststart files. Encoded packets are unchanged.
    path.write_bytes(mp4(cookie, packets, 61*rate, complete=True))
    return cookie, packets

def trimmed(cookie, packets, *, edit):
    saved = decoders_test.box
    def complete_meta(tag, data, **kwargs):
        if tag == 'meta':
            data = data[:4]+saved('hdlr', bytes(8)+b'mdir'+b'appl'+bytes(9))+data[4:]
        return saved(tag, data, **kwargs)
    # The older minimal writer omits meta's handler, which Audio::Scan requires.
    decoders_test.box = complete_meta
    try:
        total = 61*44100
        return mp4(cookie, packets, total, complete=True,
                   trim=(113, total-332) if edit else None, smpb=(113, 219, total-332))
    finally:
        decoders_test.box = saved

class Loopback:
    def __init__(self, base):
        self.base = base
        self.process = self.stream = self.thread = None
        self.closed = False
        self.requests = queue.Queue()
        self.errors = []
        for directory in ('prefs/plugin', 'library', 'cache', 'log', 'playlists'):
            (base/directory).mkdir(parents=True, exist_ok=True)
    def start(self):
        (self.base/'prefs/server.prefs').write_text(
            f'---\naudiodir: {self.base}/library\nplaylistdir: {self.base}/playlists\ncheckVersion: 0\nlanguage: EN\n')
        (self.base/'prefs/plugin/cli.prefs').write_text('---\ncliport: 19090\n_version: 1\n')
        (self.base/'prefs/custom-convert.conf').write_text('alcx alc * *\n\t-\naac aac * *\n\t-\n')
        self.log = (self.base/'lms.log').open('w')
        perl = Path(os.environ.get('YENEY_REAL_LMS_PERL5', Path.home()/'.cache/yeney-test/real-lms-perl5'))
        env = dict(os.environ, PERL5LIB=str(perl/'lib/perl5')+':'+os.environ.get('PERL5LIB', ''))
        self.process = subprocess.Popen(['perl', str(LMS/'slimserver.pl'), '--prefsdir', str(self.base/'prefs'),
            '--cachedir', str(self.base/'cache'), '--logdir', str(self.base/'log'), '--httpport', '9000',
            '--httpaddr', '127.0.0.1', '--cliport', '19090', '--cliaddr', '127.0.0.1', '--playeraddr',
            '127.0.0.1', '--streamaddr', '127.0.0.1', '--advertiseaddr', '127.0.0.1', '--norestart'],
            env=env, stdout=self.log, stderr=subprocess.STDOUT)
        def ready():
            assert self.process.poll() is None, (self.base/'lms.log').read_text()[-4000:]
            return '9.1.1' in cli('version ?')
        wait(ready)
        cli('rescan')
        wait(lambda: cli('rescan ?').endswith(' 0'))
        self.stream = socket.create_connection(('127.0.0.1', 3483), timeout=20)
        caps = b'Model=yeney,ModelName=YeneY,AccuratePlayPoints=1,MaxSampleRate=192000,alc,aac,flc,mp3,aif,pcm'
        hello = bytes([12, 0])+bytes.fromhex(MAC.replace(':', ''))+bytes(28)+b'EN'+caps
        self.send('HELO', hello)
        self.thread = threading.Thread(target=self.receive)
        self.thread.start()
        wait(lambda: cli(MAC+' connected ?').endswith(' 1'))
        cli('pref prioritizeNative 1')
        print('WIRE loopback LMS version=9.1.1; synthetic capture player advertises AAC only for investigation', flush=True)
    def send(self, op, body):
        self.stream.sendall(op.encode()+struct.pack('>I', len(body))+body)
    def receive(self):
        pending = b''
        try:
            while not self.closed:
                block = self.stream.recv(65536)
                if not block:
                    break
                pending += block
                while len(pending) >= 2:
                    size = struct.unpack_from('>H', pending)[0]
                    if len(pending) < size+2:
                        break
                    message, pending = pending[2:size+2], pending[size+2:]
                    if message[:5] == b'strms':
                        self.requests.put(message[4:])
        except OSError as error:
            if not self.closed:
                self.errors.append(str(error))
    def fetch(self, limit):
        message = self.requests.get(timeout=25)
        request = message[24:].decode()
        assert struct.unpack_from('>H', message, 18)[0] == 9000
        assert message[20:24] in (bytes(4), socket.inet_aton('127.0.0.1'))
        connection = http.client.HTTPConnection('127.0.0.1', 9000, timeout=30)
        try:
            connection.request('GET', request.split()[1])
            response = connection.getresponse()
            assert response.status == 200
            # Observe the real response bytes before passing them to the decoder.
            body = response.read(limit)
        finally:
            connection.close()
        return message[2:3], body
    def capture(self, path, limit=8*1024*1024):
        cli(MAC+' playlist play '+quote('file://'+str(path), safe=''))
        codec, first = self.fetch(limit)
        status = b'STMs'+bytes(3)+struct.pack('>IIIIHIIIIIHIIH', 1048576, 0, 0, len(first),
                                           65535, 1, 8192, 4096, 0, 0, 0, 0, 0, 0)
        self.send('STAT', status)
        cli(MAC+' time 19')
        seek_codec, seek = self.fetch(limit)
        cli(MAC+' stop')
        assert codec == seek_codec
        assert not self.errors, self.errors
        return codec, first, seek
    def close(self):
        self.closed = True
        if self.stream:
            with contextlib.suppress(OSError):
                self.stream.shutdown(socket.SHUT_RDWR)
            self.stream.close()
        if self.thread:
            self.thread.join(5)
            assert not self.thread.is_alive()
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            self.log.close()

@unittest.skipUnless((LMS/'slimserver.pl').is_file(), 'SKIP real LMS 9.1.1 cache missing; set YENEY_REAL_LMS_HOME')
class Positions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='yeney-mp4-seek-', dir=os.environ.get('TMPDIR', '/var/tmp'))
        cls.base = Path(cls.work.name)
        cls.server = Loopback(cls.base)
        cls.addClassCleanup(cls.work.cleanup)
        cls.addClassCleanup(cls.server.close)
        cookie, packets = generated(cls.base/'library/public-44100.m4a', 44100, 16)
        generated(cls.base/'library/public-96000.m4a', 96000, 24)
        for rate in (44100, 88200, 96000, 176400, 192000):
            path = cls.base/f'library/matrix-{rate}.m4a'
            generated(path, rate, 24)
            subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-c:a', 'flac',
                            '-sample_fmt', 's32', str(path.with_suffix('.flac'))], check=True)
        for edit in (True, False):
            (cls.base/f'library/trim-{edit}.m4a').write_bytes(trimmed(cookie, packets, edit=edit))
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'anoisesrc=r=44100:d=61:seed=7123:a=0.4',
                        '-ac', '2', '-c:a', 'aac', '-movflags', '+faststart', str(cls.base/'library/aac.m4a')], check=True)
        cls.server.start()
    def check_alac(self, label, path, rate, trim=0):
        codec, first, seek = self.server.capture(path)
        self.assertEqual(codec, b'l')
        table = boxes(seek)
        source = boxes(path.read_bytes())
        for tag in ('mvhd', 'trak/mdia/mdhd'):
            self.assertEqual(table['/moov/'+tag], source['/moov/'+tag])
        landed, total, remaining, scale = origin(seek)
        self.assertEqual(scale, rate)
        self.assertGreater(landed, 0)
        self.assertLess(landed, 19*rate)
        ref = reference(self.base/'library/public-44100.m4a' if trim else path)
        errors = []
        for kind, body, expected in (('start', first, trim), ('seek', seek, landed)):
            known, start, count, failed, pcm = probe(body)
            self.assertFalse(failed)
            self.assertEqual(count, 4096)
            match = ref.find(pcm)
            self.assertGreaterEqual(match, 0)
            self.assertEqual(match % 8, 0)
            true = match//8
            print(f'{label} {path.name} {kind}: requested={19*rate if kind=="seek" else 0} '
                  f'mdhd={total} stts={remaining} header_landing={landed} '
                  f'known={known} reported={start} PCM_match={true} frames=4096 exact={true==expected}', flush=True)
            if known != 1 or start != expected or true != expected:
                errors.append((kind, known, start, true, expected))
        self.assertEqual(errors, [], 'reported and PCM coordinates must both equal the header-derived origin')
        return first, seek, landed
    def test_K1_public_16_44100(self):
        self.check_alac('K1', self.base/'library/public-44100.m4a', 44100)
    def test_K2_public_24_96000(self):
        self.check_alac('K2', self.base/'library/public-96000.m4a', 96000)
    def test_K3_trim(self):
        for edit in (True, False):
            with self.subTest(edit=edit):
                self.check_alac('K3', self.base/f'library/trim-{edit}.m4a', 44100, 113)
    def test_K4_aac_unknown(self):
        codec, first, seek = self.server.capture(self.base/'library/aac.m4a')
        self.assertEqual(codec, b'a')
        landed, total, remaining, scale = origin(seek)
        self.assertEqual((landed, scale), (837632, 44100))
        # Native AAC is deliberately not advertised by yeney-core. Its existing
        # container path rejects MP4/AAC without ever publishing a known origin.
        known, start, count, failed, pcm = probe(seek, 'aac')
        self.assertEqual((known, count, failed, pcm), (0, 0, 1, b''))
        print(f'K4 AAC: requested=837900 packet_origin={landed} mdhd={total} stts={remaining} '
              'known=0 frames=0 unsupported native codec; never 0 known', flush=True)
    def test_K5_private(self):
        directory = os.environ.get('YENEY_LENGTH_FIXTURE_DIR')
        if not directory:
            self.skipTest('SKIP private ALAC: YENEY_LENGTH_FIXTURE_DIR unset; media read in place only')
        for name in ('04 I Put A Spell On You.m4a', '01 Mira.m4a'):
            with self.subTest(name=name):
                self.check_alac('K5', Path(directory)/name, 96000)
    def test_K6_from_start_and_flac(self):
        codec, first, seek = self.server.capture(self.base/'library/public-44100.m4a')
        known, start, count, failed, pcm = probe(first)
        self.assertEqual((known, start, failed), (1, 0, 0))
        manifest = json.loads((ROOT/'tests/fixtures/manifest.json').read_text())['flac'][0]
        original = (ROOT/'tests/fixtures'/manifest['file']).read_bytes()
        # The committed headerless FLAC seek oracle carries its own sample index.
        known, start, count, failed, pcm = probe(original[manifest['seekOffset']:], 'flac')
        self.assertEqual((known, start, failed), (1, manifest['seekFrames'], 0))
        print(f'K6 from-start M4A=0 known; FLAC landing={start} known unchanged', flush=True)
    def test_K7_unprovable_duration(self):
        path = self.base/'library/public-44100.m4a'
        body = bytearray(path.read_bytes())
        at = body.index(b'mdhd')+4+16
        for value in (0xffffffff, 61*44100-1, 61*44100+1):
            struct.pack_into('>I', body, at, value)
            known, start, count, failed, pcm = probe(body)
            self.assertEqual((known, failed, count), (0, 0, 4096))
        print('K7 missing/short/non-packet-aligned duration: unknown, decoded PCM retained', flush=True)
    def test_KC1_converted_matrix(self):
        for rate in (44100, 88200, 96000, 176400, 192000):
            target = 44100 if rate in (44100, 88200, 176400) else 48000
            factor = rate // target
            for codec, extension in ((b'l', 'm4a'), (b'f', 'flac')):
                path = self.base/f'library/matrix-{rate}.{extension}'
                wire, first, seek = self.server.capture(path, limit=64*1024*1024)
                self.assertEqual(wire, codec)
                source = probe(seek, "flac" if codec == b"f" else "native")
                self.assertEqual(source[0], 1, (rate, extension, source[:4], len(seek), seek[:24]))
                self.assertEqual(source[1] % factor, 0)
                mode = 'convert-flac' if codec == b'f' else 'convert'
                natural = probe(first, mode, full=True)
                sought = probe(seek, mode, full=True)
                self.assertEqual((natural[0], sought[0], natural[3], sought[3]), (1, 1, 0, 0))
                self.assertEqual(sought[1], source[1] // factor)
                result = subprocess.run([str(ROOT/'mp4-position-test'), mode], input=seek,
                                        capture_output=True, check=True)
                bound = int(result.stderr.decode().split('warmup=')[1].split()[0])
                self.assertEqual(bound, 0 if factor == 1 else 160 if target == 44100 else 128)
                aligned = natural[4][sought[1]*8:sought[1]*8+len(sought[4])]
                self.assertEqual(len(aligned), len(sought[4]))
                differences = [i for i in range(len(aligned)//8)
                               if aligned[i*8:i*8+8] != sought[4][i*8:i*8+8]]
                first_identical = max(differences, default=-1)+1
                self.assertLessEqual(first_identical, bound)
                print(f'KC1 {rate}->{target} {extension}: landed_source={source[1]} '
                      f'landed_out={sought[1]} measured={first_identical} bound={bound}', flush=True)
                # The content-position fixture checks converted known bits and v1 byte identity.
                fixture = self.base/'matrix-seek.bin'
                fixture.write_bytes(seek)
                from shm_name import SEGMENT_PREFIX
                subprocess.run([str(ROOT/'content-position-test'), 'c'+codec.decode(), str(fixture),
                                str(sought[1]), '1', SEGMENT_PREFIX], check=True)
    def test_K8_conversion_landing(self):
        codec, first, seek = self.server.capture(self.base/'library/public-96000.m4a', limit=32*1024*1024)
        landed, total, remaining, scale = origin(seek)
        natural = probe(first, 'convert', full=True)
        sought = probe(seek, 'convert', full=True)
        self.assertEqual((natural[0], sought[0], natural[3], sought[3]), (1, 1, 0, 0))
        self.assertEqual((natural[1], sought[1]), (0, landed//2))
        self.assertEqual((natural[2], sought[2]), ((total+1)//2, (remaining+1)//2))
        aligned = natural[4][landed//2*8:landed//2*8+len(sought[4])]
        self.assertEqual(len(aligned), len(sought[4]))
        differences = [i for i in range(len(aligned)//8) if aligned[i*8:i*8+8] != sought[4][i*8:i*8+8]]
        self.assertGreater(len(differences), 0)
        self.assertLess(max(differences), 128)
        print(f'K8 96000->48000: source_total={total} source_landing={landed} source_remaining={remaining} '
              f'output_total={natural[2]} output_remaining={sought[2]} known=1/1 '
              f'warmup_different_frames={len(differences)} last_difference={max(differences)}', flush=True)

if __name__ == '__main__':
    unittest.main(verbosity=2)
