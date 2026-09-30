# Copyright (c) 2026 Jaap van Vliet
# Original implementation for the YeneY project.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
# THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

"""Localhost-only wire oracle. No third-party Python packages or LMS required."""
import contextlib
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EVENTS = {'STMc', 'STMe', 'STMh', 'STMs', 'STMd', 'STMf', 'STMp', 'STMr', 'STMt', 'STMu', 'STMo', 'STMn', 'STMl'}
REQUEST = b'GET /pcm HTTP/1.0\r\nX-Exact: unmodified bytes\r\n\r\n'


def listener(host='127.0.0.1', port=0, kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    if kind == socket.SOCK_STREAM:
        s.listen(5)
    s.settimeout(5)
    return s


def pcm(rate=44100, bits=16, seconds=.2, channels=2, big=False):
    data, expected = bytearray(), bytearray()
    for i in range(round(rate * seconds)):
        pair = [((i * 137 + 17) % (1 << bits)) - (1 << (bits - 1)), ((i * 59) % (1 << bits)) - (1 << (bits - 1))]
        for v in pair[:channels]:
            data.extend(v.to_bytes(bits // 8, 'big' if big else 'little', signed=True))
        if channels == 1:
            pair[1] = pair[0]
        expected.extend(struct.pack('<ii', *(v * (1 << (32 - bits)) for v in pair)))
    return bytes(data), bytes(expected)


class HTTP:
    def __init__(self, body, delay=0, first=None, hold=False, extra_headers=b''):
        self.socket = listener()
        self.port = self.socket.getsockname()[1]
        self.body, self.delay, self.first, self.hold = body, delay, first, hold
        self.extra_headers = extra_headers
        self.request = None
        self.errors = []
        self.done = threading.Event()
        self.connection = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            conn, _ = self.socket.accept()
            self.connection = conn
            conn.settimeout(5)
            request = bytearray()
            while not request.endswith(b'\r\n\r\n'):
                request.extend(conn.recv(1))
            self.request = bytes(request)
            response = b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(self.body)).encode() + b'\r\n' + self.extra_headers + b'\r\n'
            # Fragment the header and PCM at inconvenient byte boundaries.
            for part in [response[:7], response[7:23], response[23:]]:
                conn.sendall(part)
            if self.first is not None:
                conn.sendall(self.body[:self.first])
                if self.done.wait(self.delay):
                    return
                body = self.body[self.first:]
            else:
                body = self.body
            for pos in range(0, len(body), 7919):
                conn.sendall(body[pos:pos + 7919])
            if self.hold:
                self.done.wait(3)
        except (BrokenPipeError, ConnectionResetError):
            pass  # q/f is permitted to cancel a response
        except Exception as e:
            if not self.done.is_set():
                self.errors.append(e)
        finally:
            if self.connection:
                self.connection.close()

    def close(self):
        self.done.set()
        self.socket.close()
        self.thread.join(1)
        if self.errors:
            raise self.errors[0]


class LMS:
    def __init__(self, sock=None):
        self.listener = sock or listener()
        self.connection = None
        self.packets = []

    def accept(self):
        if self.connection:
            self.connection.close()
        self.connection, _ = self.listener.accept()
        self.connection.settimeout(5)
        return self.read()

    def exact(self, n, eof_ok=False):
        data = bytearray()
        while len(data) < n:
            part = self.connection.recv(n - len(data))
            if not part:
                if eof_ok and not data:
                    return None
                raise AssertionError('unexpected player disconnect')
            data.extend(part)
        return bytes(data)

    def read(self, eof_ok=False):
        head = self.exact(8, eof_ok)
        if head is None:
            return None
        opcode = head[:4].decode()
        length = struct.unpack('!I', head[4:])[0]
        assert length <= 65536, (opcode, length)
        body = self.exact(length)
        p = dict(op=opcode, body=body, time=time.monotonic())
        if opcode == 'STAT':
            assert length == 53, length
            fields = struct.unpack('!4sBBBIIIIHIIIIHIIH', body)
            p.update(event=fields[0].decode(), size=fields[4], full=fields[5], received=(fields[6] << 32) | fields[7],
                     jiffies=fields[9], output_size=fields[10], output_full=fields[11], seconds=fields[12], elapsed=fields[14], stamp=fields[15])
            assert p['event'] in EVENTS
            assert p['full'] <= p['size'] and p['output_full'] <= p['output_size']
            assert p['output_full'] % 8 == 0 and p['seconds'] == p['elapsed'] // 1000
            assert fields[1:4] == (0, 0, 0) and fields[8] == 65535 and fields[13] == fields[16] == 0
        elif opcode == 'HELO':
            assert length >= 36
        elif opcode == 'DSCO':
            assert length == 1 and body[0] in (0, 1, 2, 3, 4)
        elif opcode == 'RESP':
            assert body.startswith(b'HTTP/') and body.endswith(b'\r\n\r\n')
        elif opcode == 'SETD':
            assert body[0] == 0 and body[-1] == 0
        else:
            raise AssertionError('unexpected opcode ' + opcode)
        self.packets.append(p)
        return p

    def wait(self, event, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.connection.settimeout(max(.01, deadline - time.monotonic()))
            p = self.read()
            if p.get('event', p['op']) == event:
                return p
        raise AssertionError('missing ' + event)

    def send(self, opcode, body=b'', fragment=False):
        frame = struct.pack('!H', 4 + len(body)) + opcode.encode() + body
        if fragment:
            for byte in frame:
                self.connection.sendall(bytes([byte]))
        else:
            self.connection.sendall(frame)

    def strm(self, command, http=None, rate=44100, bits=16, channels=2, big=False, autostart=1, value=0, fmt='p', threshold=0, out_threshold=0, unknown=False, gain=0, transition=0, period=0):
        rates = {44100: '3', 48000: '4', 8000: '5', 192000: '<'}
        params = b'????' if unknown else (str(bits // 8 - 1) + rates[rate] + str(channels) + ('0' if big else '1')).encode()
        body = command.encode() + str(autostart).encode() + fmt.encode() + params + bytes([threshold, 0, period, ord('0') + transition, 0, out_threshold, 0])
        body += struct.pack('!IHI', gain if command == 's' else value, http.port if http else 0, 0)
        assert len(body) == 24
        self.send('strm', body + (REQUEST if http else b''))

    def timer(self, stamp=0x1234abcd):
        self.strm('t', value=stamp)
        while True:
            p = self.wait('STMt')
            if p['stamp'] == stamp:
                return p

    def close(self):
        if self.connection:
            self.connection.close()
        self.listener.close()


class Session:
    def __init__(self, app=False, discover=False, sock=None, mode="normal", host="127.0.0.1", max_rate=None, sink="wav", app_args=(), mac="02:01:02:03:04:05"):
        self.temp = tempfile.TemporaryDirectory(prefix='yeney-test-')
        self.base = Path(self.temp.name) / 'record'
        self.lms = LMS(sock)
        self.log = open(self.base.with_suffix('.log'), 'w+')
        port = self.lms.listener.getsockname()[1]
        args = [str(ROOT / 'test-player'), 'discover' if discover else host, str(port), str(self.base), mode]
        if app:
            args = [str(ROOT / 'yeney-player'), '-n', 'Fixture', '-m', mac, '-s', host + ':' + str(port), '--sink', 'wav:' + str(self.base) + '.wav' if sink == 'wav' else sink]
            args += list(app_args)
            if max_rate is not None:
                args += ['--max-rate', str(max_rate)]
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        self.hello = self.lms.accept()
        self.http = []
        self.stopped = False

    def source(self, body, **kw):
        h = HTTP(body, **kw)
        self.http.append(h)
        return h

    def stop(self):
        if not self.stopped:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(3)
            self.stopped = True
            self.log.flush()
            if self.proc.returncode:
                raise AssertionError(self.base.with_suffix('.log').read_text())
            while self.lms.read(eof_ok=True) is not None:
                pass  # Validate every remaining packet through connection EOF.
        for h in self.http:
            assert h.request == REQUEST, h.request

    def data(self):
        self.stop()
        return self.base.with_suffix('.pcm').read_bytes()

    def events(self):
        self.stop()
        return self.base.with_suffix('.events').read_text().splitlines()

    def close(self):
        self.stop()
        for h in self.http:
            h.close()
        self.lms.close()
        self.log.close()
        self.temp.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class ProtocolTests(unittest.TestCase):
    def check_hello(self, p, reconnect=False):
        self.assertEqual(p['op'], 'HELO')
        body = p['body']
        self.assertEqual(body[:8], b'\x0c\x00\x02\x01\x02\x03\x04\x05')
        self.assertEqual(len(body[8:24]), 16)
        self.assertEqual(body[14] >> 4, 4)
        self.assertEqual(body[16] >> 6, 2)
        self.assertEqual(struct.unpack('!H', body[24:26])[0], 0x4000 if reconnect else 0)
        self.assertEqual(body[34:36], b'EN')
        self.assertEqual(body[36:].decode().split(','), ['Model=yeney', 'ModelName=YeneY', 'AccuratePlayPoints=1', 'MaxSampleRate=48000', 'alc', 'flc', 'mp3', 'aif', 'pcm'])

    def test_01_helo_pcm_clock_pause_timer_gain_name(self):
        with Session() as s:
            self.check_hello(s.hello)
            self.assertEqual(s.hello['body'][26:34], bytes(8))
            lms = s.lms
            lms.send('setd', b'\0', fragment=True)
            self.assertEqual(lms.wait('SETD')['body'], b'\0Fixture\0')
            lms.send('setd', b'\0Renamed\0')
            self.assertEqual(lms.wait('SETD')['body'], b'\0Renamed\0')
            body, expected = pcm(seconds=1.6)
            h = s.source(body, hold=True)
            lms.strm('s', h, threshold=8, out_threshold=1)
            lms.wait('STMs')
            lms.send('audg', struct.pack('!IIBBII', 0, 0, 1, 0, 12345, 23456))
            lms.send('aude', bytes([0, 1]))
            one = lms.timer()
            time.sleep(.45)
            two = lms.timer(0x99887766)
            self.assertLess(abs((two['elapsed'] - one['elapsed']) / 1000 - (two['time'] - one['time'])), (two['time'] - one['time']) * .05)
            self.assertEqual(two['stamp'], 0x99887766)
            lms.strm('p')
            p = lms.wait('STMp')
            time.sleep(.15)
            self.assertEqual(lms.timer()['elapsed'], p['elapsed'])
            lms.strm('u')
            lms.wait('STMr')
            lms.wait('STMu')
            self.assertEqual(s.data(), expected)
            self.assertIn('volume 12345 23456', s.events())
            self.assertIn('power 1', s.events())
            events = [p.get('event', p['op']) for p in lms.packets[1:]]
            order = [events.index(v) for v in ['STMc', 'STMe', 'STMh', 'RESP']]
            self.assertEqual(order, sorted(order))
            done = next(p for p in lms.packets if p.get('event') == 'STMd')
            self.assertEqual(done['received'], len(body))
            self.assertEqual(done['size'], 32768 + 65536)
            self.assertEqual(done['output_size'], (96000 + 8192) * 8)

    def test_02_pcm_formats_bit_exact(self):
        for bits, channels, big in [(24, 2, False), (24, 1, True), (8, 1, False), (32, 2, True)]:
            with self.subTest(bits=bits, channels=channels, big=big), Session() as s:
                body, expected = pcm(48000, bits, .08, channels, big)
                h = s.source(body)
                s.lms.strm('s', h, rate=48000, bits=bits, channels=channels, big=big, threshold=32)
                s.lms.wait('STMu')
                self.assertEqual(s.data(), expected)

    def test_03_gapless_and_rate_change(self):
        for second_rate in [44100, 48000]:
            with self.subTest(rate=second_rate), Session() as s:
                body, expected = pcm(seconds=.5)
                first = s.source(body)
                s.lms.strm('s', first)
                done = s.lms.wait('STMd')
                body2, expected2 = pcm(second_rate, 24, .2)
                second = s.source(body2)
                s.lms.strm('s', second, rate=second_rate, bits=24)
                end = s.lms.wait('STMu')
                starts = [p for p in s.lms.packets if p.get('event') == 'STMs']
                self.assertEqual(len(starts), 2)
                self.assertGreater(((starts[1]['jiffies'] - starts[0]['jiffies']) & 0xffffffff) / 1000, .48)
                self.assertLess(((starts[1]['jiffies'] - starts[0]['jiffies']) & 0xffffffff) / 1000, .53)
                self.assertLess(starts[1]['elapsed'], 12)
                self.assertGreater(end['elapsed'], 195)
                self.assertLess(done['time'], starts[1]['time'])
                self.assertEqual(s.data(), expected + expected2)
                boundaries = [v for v in s.events() if v.startswith('boundary')]
                self.assertEqual(boundaries, ['boundary 0 44100 16 2 0', f'boundary 22050 {second_rate} 24 2 1'])

    def test_04_timed_pause_and_future_start(self):
        with Session() as s:
            body, _ = pcm(seconds=1)
            s.lms.strm('s', s.source(body))
            s.lms.wait('STMs')
            s.lms.strm('p', value=200)
            p = s.lms.timer()
            time.sleep(.1)
            self.assertEqual(s.lms.timer()['elapsed'], p['elapsed'])
            time.sleep(.2)
            self.assertGreater(s.lms.timer()['elapsed'], p['elapsed'] + 80)
            self.assertFalse(any(p.get('event') == 'STMp' for p in s.lms.packets))
            s.lms.strm('p')
            frozen = s.lms.wait('STMp')
            s.lms.strm('u', value=(frozen['jiffies'] + 250) & 0xffffffff)
            s.lms.wait('STMr')
            time.sleep(.1)
            self.assertEqual(s.lms.timer()['elapsed'], frozen['elapsed'])
            time.sleep(.25)
            self.assertGreater(s.lms.timer()['elapsed'], frozen['elapsed'] + 80)
            s.lms.wait('STMu')

    def test_05_stop_flush(self):
        for command in ['q', 'f']:
            with self.subTest(command=command), Session() as s:
                body, _ = pcm(seconds=1)
                s.lms.strm('s', s.source(body))
                s.lms.wait('STMs')
                time.sleep(.1)
                s.lms.strm(command)
                stopped = s.lms.wait('STMf')
                time.sleep(.08)
                timer = s.lms.timer()
                if command == 'q':
                    self.assertEqual(timer['elapsed'], stopped['elapsed'])
                    self.assertEqual(stopped['elapsed'], 0)
                else:
                    self.assertGreater(stopped['elapsed'], 80)
                    self.assertGreater(timer['elapsed'], stopped['elapsed'] + 50)
                    s.lms.wait('STMu')
                events = s.events()
                if command == 'q':
                    self.assertIn('flush', events)

    def test_06_skip(self):
        with Session() as s:
            body, expected = pcm(seconds=.5)
            s.lms.strm('s', s.source(body))
            s.lms.wait('STMs')
            s.lms.strm('p')
            paused = s.lms.wait('STMp')
            s.lms.strm('a', value=100)
            s.lms.strm('u')
            s.lms.wait('STMr')
            end = s.lms.wait('STMu')
            data = s.data()
            self.assertEqual(len(data), len(expected) - 4410 * 8)
            frames = [expected[i:i + 8] for i in range(0, len(expected), 8)]
            actual = [data[i:i + 8] for i in range(0, len(data), 8)]
            # Locate the only discontinuity; samples on either side remain exact.
            prefix = next(i for i in range(len(actual)) if actual[i] != frames[i])
            self.assertEqual(actual[prefix:], frames[prefix + 4410:])
            self.assertGreater(end['elapsed'], 495)
            self.assertLess(paused['elapsed'], 100)

    def test_07_autostart_cont_codc(self):
        for auto in [0, 2, 3]:
            with self.subTest(auto=auto), Session() as s:
                body, expected = pcm(seconds=.15)
                s.lms.strm('s', s.source(body), autostart=auto, fmt='?' if auto >= 2 else 'p')
                s.lms.wait('RESP')
                if auto >= 2:
                    before = s.lms.timer()
                    self.assertEqual(before['elapsed'], 0)
                    self.assertFalse(any(p.get('event') in ('STMd', 'STMs') for p in s.lms.packets))
                    s.lms.send('codc', b'p1321')
                    s.lms.send('cont', struct.pack('!IB', 0, 0))
                if auto in [0, 2]:
                    s.lms.wait('STMl')
                    self.assertFalse(any(p.get('event') == 'STMs' for p in s.lms.packets))
                    s.lms.strm('u')
                    s.lms.wait('STMr')
                s.lms.wait('STMu')
                self.assertEqual(s.data(), expected)

    def test_08_wav_header_and_app_sink(self):
        body, expected = pcm(seconds=.08)
        wav = b'RIFF' + struct.pack('<I', len(body) + 36) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 176400, 4, 16) + b'data' + struct.pack('<I', len(body)) + body
        with Session() as s:
            s.lms.strm('s', s.source(wav), unknown=True)
            s.lms.wait('STMu')
            self.assertEqual(s.data(), expected)
        with Session(app=True) as s:
            self.check_hello(s.hello)
            s.lms.strm('s', s.source(body))
            s.lms.wait('STMu')
            s.stop()
            data = s.base.with_suffix('.wav').read_bytes()
            self.assertEqual(data[:4], b'RIFF')
            self.assertEqual(struct.unpack('<I', data[4:8])[0], len(expected) + 36)
            self.assertEqual(struct.unpack('<I', data[24:28])[0], 44100)
            self.assertEqual(data[44:], expected)

    def test_09_unsupported_and_bad_pcm(self):
        with Session() as s:
            s.lms.strm('s', fmt='z')
            s.lms.wait('STMn')
            self.assertEqual([p.get('event', p['op']) for p in s.lms.packets], ['HELO', 'STMf', 'STMc', 'STMn'])
        with Session() as s:
            s.lms.strm('s', s.source(bytes(7)))
            s.lms.wait('STMn')

    def test_10_underrun_recovers(self):
        with Session() as s:
            body, expected = pcm(seconds=.35)
            s.lms.strm('s', s.source(body, first=4410 * 4, delay=.3))
            s.lms.wait('STMo')
            self.assertFalse(any(p.get('event') == 'STMu' for p in s.lms.packets))
            s.lms.wait('STMu')
            self.assertEqual(s.data(), expected)

    def test_11_reconnect_backoff(self):
        with Session() as s:
            uuid = s.hello['body'][8:24]
            times = []
            for _ in range(3):
                start = time.monotonic()
                s.lms.connection.shutdown(socket.SHUT_RDWR)
                s.lms.connection.close()
                h = s.lms.accept()
                times.append(time.monotonic() - start)
                self.check_hello(h, True)
                self.assertEqual(h['body'][8:24], uuid)
            for measured, expected in zip(times, [.1, .2, .4]):
                self.assertGreaterEqual(measured, expected * .9)
                self.assertLess(measured, expected + .15)

    def test_12_discovery(self):
        udp = listener(port=3483, kind=socket.SOCK_DGRAM)
        requests = []
        def responder():
            data, addr = udp.recvfrom(1024)
            requests.append(data)
            udp.sendto(b'E' + b'NAME' + bytes([4]) + b'Test', addr)
        thread = threading.Thread(target=responder, daemon=True)
        thread.start()
        try:
            with Session(discover=True) as s:
                self.check_hello(s.hello)
                self.assertEqual(requests, [b'eNAME\0JSON\0UUID\0VERS\0'])
        finally:
            udp.close()
            thread.join(1)

    def test_13_serv(self):
        target = LMS(listener('127.0.0.2', 3483))
        try:
            with Session() as s:
                s.lms.send('serv', socket.inet_aton('127.0.0.2'))
                h = target.accept()
                self.check_hello(h, True)
                self.assertEqual(h['body'][8:24], s.hello['body'][8:24])
        finally:
            target.close()

    def test_14_clean_shutdown_and_malformed_frame(self):
        with Session() as s:
            s.lms.connection.sendall(bytes([0, 3, 0, 0, 0]))
            self.check_hello(s.lms.accept(), True)
            start = time.monotonic()
            s.stop()
            self.assertLess(time.monotonic() - start, .25)

    def test_15_sink_delay_and_zero_acceptance(self):
        for mode in ['delayed', 'blocked']:
            with self.subTest(mode=mode), Session(mode=mode) as s:
                body, expected = pcm(seconds=.2)
                s.lms.strm('s', s.source(body))
                end = s.lms.wait('STMu')
                starts = [p for p in s.lms.packets if p.get('event') == 'STMs']
                self.assertEqual(len(starts), 1)
                connected = next(p for p in s.lms.packets if p.get('event') == 'STMc')
                self.assertGreater((starts[0]['jiffies'] - connected['jiffies']) & 0xffffffff, 55 if mode == 'delayed' else 110)
                self.assertGreaterEqual(end['elapsed'], 199)
                self.assertEqual(s.data(), expected)

    def test_16_bounded_pipeline_backpressure(self):
        with Session() as s:
            body, expected = pcm(seconds=2.6)
            s.lms.strm('s', s.source(body))
            s.lms.wait('STMs')
            s.lms.strm('p')
            s.lms.wait('STMp')
            deadline = time.monotonic() + 1
            while True:
                status = s.lms.timer()
                if status['output_full'] == status['output_size'] or time.monotonic() > deadline:
                    break
                time.sleep(.02)
            self.assertEqual(status['output_full'], status['output_size'])
            self.assertGreater(status['full'], 0)
            s.lms.strm('u')
            s.lms.wait('STMu', timeout=5)
            self.assertEqual(s.data(), expected)

    def test_17_flush_only_queued_track(self):
        with Session() as s:
            body, expected = pcm(seconds=.4)
            s.lms.strm('s', s.source(body))
            s.lms.wait('STMd')
            second, _ = pcm(48000, 24, .15)
            s.lms.strm('s', s.source(second), rate=48000, bits=24)
            s.lms.wait('STMd')
            s.lms.strm('f')
            s.lms.wait('STMf')
            s.lms.wait('STMu')
            self.assertEqual(s.data(), expected)
            self.assertEqual(len([e for e in s.events() if e.startswith('boundary')]), 1)

    def test_18_aiff_and_unsigned_wav(self):
        with Session() as s:
            body, expected = pcm(seconds=.08, big=True)
            comm = struct.pack('!HIH', 2, len(body) // 4, 16) + bytes.fromhex('400eac44000000000000')
            chunks = b'COMM' + struct.pack('!I', len(comm)) + comm + b'SSND' + struct.pack('!III', len(body) + 8, 0, 0) + body
            aiff = b'FORM' + struct.pack('!I', len(chunks) + 4) + b'AIFF' + chunks
            s.lms.strm('s', s.source(aiff))
            s.lms.wait('STMu')
            self.assertEqual(s.data(), expected)
        with Session() as s:
            body = bytes(range(256)) * 4
            wav = b'RIFF' + struct.pack('<I', len(body) + 36) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 8000, 8000, 1, 8) + b'data' + struct.pack('<I', len(body)) + body
            expected = b''.join(struct.pack('<ii', (v - 128) * (1 << 24), (v - 128) * (1 << 24)) for v in body)
            s.lms.strm('s', s.source(wav), unknown=True)
            s.lms.wait('STMu')
            self.assertEqual(s.data(), expected)

    def test_19_hostname_wav_segments_and_unity_gain(self):
        with Session(app=True, host='localhost') as s:
            first, expected = pcm(seconds=.25)
            s.lms.strm('s', s.source(first))
            s.lms.wait('STMd')
            second, expected2 = pcm(48000, 24, .1)
            s.lms.strm('s', s.source(second), rate=48000, bits=24)
            s.lms.wait('STMu')
            s.stop()
            self.assertEqual(s.base.with_suffix('.wav').read_bytes()[44:], expected)
            segmented = Path(str(s.base) + '.wav.1.wav').read_bytes()
            self.assertEqual(struct.unpack('<I', segmented[24:28])[0], 48000)
            self.assertEqual(segmented[44:], expected2)
        with Session() as s:
            s.lms.send('audg', struct.pack('!IIBBII', 0, 0, 0, 0, 1, 2))
            s.lms.send('aude', bytes([0, 0]))
            s.lms.timer()  # Commands are serialized before this reply.
            self.assertIn('volume 65536 65536', s.events())
            self.assertIn('power 0', s.events())

    def test_20_nonzero_metadata_rejected(self):
        with Session() as s:
            body, _ = pcm(seconds=.08)
            s.lms.strm('s', s.source(body), autostart=3)
            s.lms.wait('RESP')
            s.lms.send('cont', struct.pack('!IB', 16000, 0))
            s.lms.wait('STMn')
            self.assertFalse(any(p.get('event') == 'STMs' for p in s.lms.packets))

    def test_21_app_max_rate(self):
        for sink in ['null', 'wav']:
            with self.subTest(sink=sink), Session(app=True, max_rate=192000, sink=sink) as s:
                self.assertIn('MaxSampleRate=192000', s.hello['body'][36:].decode().split(','))
                body, expected = pcm(192000, 24, .15)
                s.lms.strm('s', s.source(body), rate=192000, bits=24)
                s.lms.wait('STMu')
                s.stop()
                self.assertFalse(any(p.get('event') == 'STMn' for p in s.lms.packets))
                if sink == 'wav':
                    wav = s.base.with_suffix('.wav').read_bytes()
                    self.assertEqual(struct.unpack('<I', wav[24:28])[0], 192000)
                    self.assertEqual(wav[44:], expected)
        with Session(app=True) as s:
            self.check_hello(s.hello)
        for value in ['0', '44099', '384001', '-1', '192000Hz', 'abc', '999999999999999999999']:
            result = subprocess.run([str(ROOT / 'yeney-player'), '--max-rate', value],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('--max-rate must be an integer in 44100..384000 Hz', result.stderr)
        for value in ['44100', '384000']:
            with Session(app=True, max_rate=value) as s:
                self.assertIn('MaxSampleRate=' + value, s.hello['body'][36:].decode().split(','))


if __name__ == '__main__':
    unittest.main(verbosity=2)
