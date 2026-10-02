#!/usr/bin/env python3
# Copyright (c) 2026 Jaap van Vliet
# Original implementation for the YeneY project.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
"""core-device-test.py -- unattended real-LMS test of yeney-player.

Starts ./yeney-player as a silent test player ("Core test"), lets Lyrion play a
fixed set of tracks on it, and checks per track, using the LMS CLI, the
player's event log and a capture of the Slimproto traffic (TCP 3483):

  play    the track starts and LMS position advances at real-time speed
  format  what LMS actually sent (strm format code: f=FLAC, l=ALAC, m=MP3,
          p=PCM/AIFF) matches the expectation for this file, and the audio
          arrives at <= the phase maximum (48 kHz, then 192 kHz native ALAC)
  seek    a jump to the middle lands there and keeps advancing
  pause   position freezes while paused
  resume  position advances again after resume
  clean   no STMn / error lines from the player for this track

Setup checks: the player connects, and its HELO advertises the expected
formats. Run as root on the bridge host from the yeney-core checkout after
`make` (tcpdump needs root; --no-capture skips it):

  python3 scripts/core-device-test.py [--lms <host>] [--no-capture] [--keep-player]

Everything is written to /tmp/core-test-<timestamp>/ and packed into
/tmp/core-test-<timestamp>.tar.gz: report.txt, player.log, lms-status.log,
lms-events.log, slimproto.pcap, slimproto.txt (decoded capture), run-info.txt,
shm-extension.txt (11 extension hex dumps over 10 seconds), and transition WAVs.
An SHM phase checks ABI, generation, rate and pacing. Final WAV phases compare
ReplayGain levels and verify a five-second crossfade, restoring changed LMS prefs.
Exit status 0 only if every check passes.
"""
import argparse
import array
import math
import mmap
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tarfile
import threading
import time
import urllib.parse

# (label, LMS track id, expected LMS content type)
TRACKS = [
    ("ALAC", "47145", "alc"),          # The Lady Is A Tramp (Ft. Lady Gaga) - Duets II
    ("MP3", "41965", "mp3"),           # Crowd Control (Original Mix)
    ("FLAC", "44436", "flc"),          # Just A Little Bit More (Extended)
    ("ALAC hi-res", "47797", "alc"),   # Sharp Dressed Man - ZZ Top - Eliminator
]
NATIVE_TRACK = ("ALAC 192k native", "47797", "alc")
NATIVE_MAX_RATE = 192000
EXPECTED_CAPS = ["alc", "flc", "mp3", "aif", "pcm"]
MAX_RATE = 48000

PLAYER_NAME = "Core test"
PLAYER_MAC = "02:00:00:00:be:03"
CLI_PORT = 9090
SLIM_PORT = 3483

PLAY_SECS = 12      # measure normal playback over this many seconds
SEEK_TO = 60        # seconds; capped at half the track duration
PAUSE_SECS = 5
RATE_MIN, RATE_MAX = 0.85, 1.15   # accepted LMS-time / wall-time ratio
FORMAT_NAMES = {"f": "FLAC", "l": "ALAC", "m": "MP3", "p": "PCM/AIFF", "o": "Ogg", "a": "AAC", "?": "unknown"}


def now():
    return time.strftime("%H:%M:%S")


# -------------------------------------------------------------- LMS CLI --

class Cli:
    def __init__(self, host):
        self.host = host

    def raw(self, command):
        with socket.create_connection((self.host, CLI_PORT), timeout=5) as s:
            s.sendall((command + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        return data.decode("utf-8", "replace").strip()

    def player(self, command):
        return self.raw(f"{urllib.parse.quote(PLAYER_MAC)} {command}")

    @staticmethod
    def tokens(reply):
        return [urllib.parse.unquote(t) for t in reply.split(" ")]

    @classmethod
    def field(cls, reply, key):
        for t in cls.tokens(reply):
            if t.startswith(key + ":"):
                return t[len(key) + 1:]
        return None

    @classmethod
    def fields(cls, reply):
        out = {}
        for t in cls.tokens(reply):
            k, sep, v = t.partition(":")
            if sep:
                out.setdefault(k, v)
        return out


class PreferenceGuard:
    """Restore every changed player preference, including partial setup failures."""
    def __init__(self, cli, report):
        self.cli, self.report, self.saved = cli, report, {}

    def apply(self, name, value):
        reply = self.cli.player(f"playerpref {name} {urllib.parse.quote(str(value), safe='')}")
        tokens = Cli.tokens(reply)
        try:
            accepted = tokens[1:3] == ['playerpref', name] and float(tokens[-1]) == float(value)
        except (ValueError, IndexError):
            accepted = False
        if not accepted: raise RuntimeError(f"playerpref {name} was not acknowledged: {reply}")

    def set(self, name, value):
        if name not in self.saved:
            reply = self.cli.player(f"playerpref {name} ?")
            tokens = Cli.tokens(reply)
            if len(tokens) < 4 or tokens[1:3] != ["playerpref", name] or tokens[-1] in ("?", "") :
                raise RuntimeError(f"cannot save playerpref {name}: {reply}")
            self.saved[name] = tokens[-1]
        self.apply(name, value)

    def restore(self):
        errors = []
        handler = signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            for name, value in reversed(list(self.saved.items())):
                try:
                    self.apply(name, value)
                except Exception as e:
                    errors.append(f"{name}: {e}")
        finally:
            signal.signal(signal.SIGINT, handler)
        if errors:
            self.report.check("prefs", "restore", False, "; ".join(errors))
        elif self.saved:
            self.report.check("prefs", "restore", True, f"restored {', '.join(self.saved)}")


def read_wav(path):
    """Our app's fixed integer-stereo WAV, checked before level/timeline analysis."""
    with open(path, "rb") as f:
        header, raw = f.read(44), f.read()
    if len(header) != 44 or header[:4] != b"RIFF" or header[8:16] != b"WAVEfmt " or header[36:40] != b"data":
        raise ValueError("expected yeney-player PCM WAV")
    if struct.unpack_from("<HH", header, 20) != (1, 2) or struct.unpack_from("<H", header, 34)[0] != 32:
        raise ValueError("expected 32-bit stereo PCM")
    rate = struct.unpack_from("<I", header, 24)[0]
    values = array.array("i")
    values.frombytes(raw)
    if sys.byteorder != "little": values.byteswap()
    return rate, values


class EventListener:
    """Keeps an LMS CLI 'listen 1' connection and logs every event line."""

    def __init__(self, host, path):
        self.log = open(path, "w")
        self.sock = None
        try:
            self.sock = socket.create_connection((host, CLI_PORT), timeout=5)
            self.sock.sendall(b"listen 1\n")
            self.sock.settimeout(1)
            threading.Thread(target=self._run, daemon=True).start()
        except OSError as e:
            self.log.write(f"[{now()}] listen failed: {e}\n")

    def _run(self):
        buf = b""
        while self.sock:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = " ".join(Cli.tokens(line.decode("utf-8", "replace").strip()))
                self.log.write(f"[{now()}] {text}\n")
                self.log.flush()

    def stop(self):
        s, self.sock = self.sock, None
        if s:
            s.close()
        self.log.close()


def lms_host_from_config():
    try:
        for line in open("/etc/yeney/config"):
            if line.startswith("LMS_SERVER="):
                return line.split("=", 1)[1].strip() or None
    except OSError:
        pass
    return None


# ------------------------------------------------------ player process --

class PlayerProcess:
    """Runs yeney-player and keeps its event lines with timestamps."""

    def __init__(self, lms, logpath, max_rate=MAX_RATE, append=False, shm=False, wav=None):
        self.lines = []
        self.lock = threading.Lock()
        self.log = open(logpath, "a" if append else "w")
        self.proc = subprocess.Popen(
            ["./yeney-player", "-n", PLAYER_NAME, "-m", PLAYER_MAC, "-s", lms,
             "--sink", "wav:" + str(wav) if wav else "null", "--max-rate", str(max_rate), "-d", "1"] + (["-v"] if shm else []),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            stamp = time.time()
            with self.lock:
                self.lines.append((stamp, line))
            self.log.write(f"[{now()}] {line}\n")
            self.log.flush()

    def since(self, t0, t1=None):
        with self.lock:
            return [l for (t, l) in self.lines if t >= t0 and (t1 is None or t <= t1)]

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.reader.join()
        self.log.close()


# ------------------------------------------------------------- capture --

class Capture:
    """tcpdump of the Slimproto connection between LMS and the test player."""

    def __init__(self, lms, path):
        self.path, self.proc, self.error = path, None, None
        if not shutil.which("tcpdump"):
            self.error = "tcpdump not installed (apt-get install -y tcpdump)"
            return
        dev = "any"
        try:
            route = subprocess.run(["ip", "route", "get", lms], capture_output=True, text=True).stdout
            m = re.search(r"\bdev (\S+)", route)
            dev = m.group(1) if m else "any"
        except OSError:
            pass
        self.err = open(path + ".err", "w")
        self.proc = subprocess.Popen(
            ["tcpdump", "-i", dev, "-s", "0", "-U", "-w", path, f"host {lms} and tcp port {SLIM_PORT}"],
            stdout=subprocess.DEVNULL, stderr=self.err)
        time.sleep(1.5)
        if self.proc.poll() is not None:
            self.error = f"tcpdump exited ({self.proc.returncode}); see {os.path.basename(path)}.err"
            self.proc = None

    def stop(self):
        if self.proc and self.proc.poll() is None:
            time.sleep(1)
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def read_pcap(path):
    """Yield (time, src, sport, dst, dport, seq, payload) for TCP/IPv4 packets."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        return
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        e, nano = "<", magic == b"\x4d\x3c\xb2\xa1"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        e, nano = ">", magic == b"\xa1\xb2\x3c\x4d"
    else:
        raise ValueError("not a pcap file")
    linktype = struct.unpack(e + "I", data[20:24])[0]
    pos = 24
    while pos + 16 <= len(data):
        sec, frac, incl, _ = struct.unpack(e + "IIII", data[pos:pos + 16])
        pkt = data[pos + 16:pos + 16 + incl]
        pos += 16 + incl
        ts = sec + frac / (1e9 if nano else 1e6)
        if linktype == 1:            # Ethernet
            off, proto = 14, pkt[12:14]
            if proto == b"\x81\x00":
                off, proto = 18, pkt[16:18]
        elif linktype == 113:        # Linux cooked (SLL)
            off, proto = 16, pkt[14:16]
        elif linktype == 276:        # Linux cooked v2 (SLL2)
            off, proto = 20, pkt[0:2]
        else:
            continue
        if proto != b"\x08\x00" or len(pkt) < off + 20:
            continue
        ip = pkt[off:]
        ihl = (ip[0] & 15) * 4
        total = struct.unpack(">H", ip[2:4])[0]
        if ip[9] != 6:
            continue
        src, dst = socket.inet_ntoa(ip[12:16]), socket.inet_ntoa(ip[16:20])
        tcp = ip[ihl:total]
        if len(tcp) < 20:
            continue
        sport, dport, seq = struct.unpack(">HHI", tcp[:8])
        doff = (tcp[12] >> 4) * 4
        payload = tcp[doff:]
        if payload:
            yield ts, src, sport, dst, dport, seq, payload


def slimproto_messages(path, lms):
    """Decode both directions of every Slimproto connection in the capture.

    Returns a list of (time, direction, opcode, payload), direction 'S>P' for
    LMS to player and 'P>S' for player to LMS, in time order.
    """
    flows = {}
    for ts, src, sport, dst, dport, seq, payload in read_pcap(path):
        key = (src, sport, dst, dport)
        flows.setdefault(key, {})
        flows[key].setdefault(seq, (ts, payload))
    messages = []
    for (src, sport, dst, dport), segs in flows.items():
        to_player = src == lms and sport == SLIM_PORT
        stream, times = b"", []
        expected = None
        for seq in sorted(segs):
            ts, payload = segs[seq]
            if expected is not None and seq < expected:
                payload = payload[expected - seq:]
                if not payload:
                    continue
            stream += payload
            times.extend([ts] * len(payload))
            expected = seq + len(segs[seq][1])
        pos = 0
        while True:
            if to_player:
                if pos + 6 > len(stream):
                    break
                length = struct.unpack(">H", stream[pos:pos + 2])[0]
                if length < 4 or pos + 2 + length > len(stream):
                    break
                op = stream[pos + 2:pos + 6].decode("latin-1")
                body = stream[pos + 6:pos + 2 + length]
                messages.append((times[pos], "S>P", op, body))
                pos += 2 + length
            else:
                if pos + 8 > len(stream):
                    break
                op = stream[pos:pos + 4].decode("latin-1")
                length = struct.unpack(">I", stream[pos + 4:pos + 8])[0]
                if pos + 8 + length > len(stream):
                    break
                body = stream[pos + 8:pos + 8 + length]
                messages.append((times[pos], "P>S", op, body))
                pos += 8 + length
    return sorted(messages, key=lambda m: m[0])


def describe(message):
    ts, direction, op, body = message
    stamp = time.strftime("%H:%M:%S", time.localtime(ts)) + f".{int(ts * 1000) % 1000:03d}"
    text = f"[{stamp}] {direction} {op}"
    if op == "strm" and len(body) >= 24:
        cmd, auto, fmt, size, rate, chans, endian = (chr(b) for b in body[:7])
        gain = struct.unpack(">I", body[14:18])[0]
        text += f" cmd={cmd} autostart={auto} format={fmt} pcm={size}/{rate}/{chans}/{endian} gain={gain}"
        request = body[24:].split(b"\r\n", 1)[0].decode("latin-1")
        if request:
            text += f" request='{request}'"
    elif op == "STAT" and len(body) >= 4:
        text += " " + body[:4].decode("latin-1")
    elif op == "HELO":
        caps = body[36:].decode("latin-1", "replace") if len(body) > 36 else ""
        text += f" caps={caps}"
    elif op == "RESP":
        text += " " + " | ".join(body.decode("latin-1", "replace").strip().split("\r\n")[:6])
    return text


def shm_snapshot(path):
    """Copy metadata/extension only after equal even sequence reads."""
    with open(path, "rb") as f:
        if os.fstat(f.fileno()).st_size != 32888:
            raise ValueError("SHM segment size is not 32888")
        with mmap.mmap(f.fileno(), 32888, access=mmap.ACCESS_READ) as mm:
            for _ in range(1000):
                before = struct.unpack_from("<I", mm, 32856)[0]
                if before & 1:
                    continue
                header, extension = mm[56:80], mm[32848:32888]
                after = struct.unpack_from("<I", mm, 32856)[0]
                if before == after:
                    return struct.unpack("<IIB3xIq", header), struct.unpack("<IHHIQQQ4x", extension), extension
    raise ValueError("SHM has no stable even snapshot")


# ----------------------------------------------------------------- test --

class Run:
    def __init__(self, cli, player, outdir):
        self.cli, self.player, self.outdir = cli, player, outdir
        self.results = []   # (track, check, ok, detail)
        self.phases = []    # (label, start, end, songinfo, max_rate, native_192k)
        self.native_start = None
        self.rg_phases = []
        self.cross_phase = None
        self.report = open(os.path.join(outdir, "report.txt"), "w")
        self.status_log = open(os.path.join(outdir, "lms-status.log"), "w")

    def say(self, text):
        line = f"[{now()}] {text}"
        print(line, flush=True)
        self.report.write(line + "\n")
        self.report.flush()

    def check(self, track, name, ok, detail):
        self.results.append((track, name, ok, detail))
        self.say(f"  {'PASS' if ok else 'FAIL'} {track} {name}: {detail}")

    def status(self):
        r = self.cli.player("status - 1 tags:o")
        mode = Cli.field(r, "mode")
        try:
            t = float(Cli.field(r, "time") or 0)
        except ValueError:
            t = 0.0
        tid = Cli.field(r, "id")
        self.status_log.write(f"[{now()}] mode={mode} time={t:.2f} id={tid}\n")
        self.status_log.flush()
        return mode, t, tid

    def measure(self, secs):
        _, t0, _ = self.status()
        w0 = time.time()
        for _ in range(int(secs)):
            time.sleep(1)
            self.status()
        _, t1, _ = self.status()
        return t1 - t0, time.time() - w0, t0, t1

    def wait_playing(self, timeout=20):
        end = time.time() + timeout
        last = None
        while time.time() < end:
            mode, t, _ = self.status()
            if mode == "play" and last is not None and t > last + 0.3:
                return True
            last = t if mode == "play" else None
            time.sleep(1)
        return False

    def track(self, label, track_id, ctype, max_rate=MAX_RATE, native_192k=False):
        self.say(f"=== {label} (id {track_id}); max_rate={max_rate} Hz")
        info = Cli.fields(self.cli.raw(f"songinfo 0 100 track_id:{track_id} tags:aloTd"))
        if not info.get("title"):
            self.check(label, "found", False, f"LMS has no track with id {track_id}")
            return
        rate = int(info.get("samplerate") or 0)
        self.say(f"  {info.get('title')} / {info.get('artist')} / {info.get('album')}; "
                 f"type={info.get('type')} rate={rate} duration={info.get('duration')}")
        if info.get("type") != ctype:
            self.say(f"  NOTE expected type {ctype}, LMS reports {info.get('type')}")
        duration = float(info.get("duration") or 0)
        t_start = time.time()

        self.cli.player(f"playlistcontrol cmd:load track_id:{track_id}")
        if not self.wait_playing():
            self.check(label, "play", False, "LMS never reached play with advancing time")
            self.finish_phase(label, t_start, info, max_rate, native_192k)
            return
        adv, wall, t0, t1 = self.measure(PLAY_SECS)
        ratio = adv / wall if wall else 0
        self.check(label, "play", RATE_MIN <= ratio <= RATE_MAX,
                   f"{t0:.1f}s -> {t1:.1f}s in {wall:.1f}s wall (ratio {ratio:.2f})")

        target = min(SEEK_TO, duration / 2) if duration else SEEK_TO
        self.cli.player(f"time {target:.0f}")
        time.sleep(4)
        mode, _, _ = self.status()
        adv, wall, t0, t1 = self.measure(5)
        self.check(label, "seek", mode == "play" and target - 1 <= t0 <= target + 8 and adv >= 3,
                   f"target {target:.0f}s, at {t0:.1f}s after 4s, then +{adv:.1f}s in {wall:.1f}s")

        self.cli.player("pause 1")
        time.sleep(2)
        _, p0, _ = self.status()
        time.sleep(PAUSE_SECS)
        mode, p1, _ = self.status()
        self.check(label, "pause", mode == "pause" and abs(p1 - p0) < 0.5,
                   f"mode={mode}, {p0:.1f}s -> {p1:.1f}s over {PAUSE_SECS}s")

        self.cli.player("pause 0")
        time.sleep(3)
        adv, wall, t0, t1 = self.measure(5)
        mode, _, _ = self.status()
        self.check(label, "resume", mode == "play" and adv >= 3.5 and t0 >= p1 - 0.5,
                   f"from {p1:.1f}s: {t0:.1f}s -> {t1:.1f}s (+{adv:.1f}s in {wall:.1f}s)")
        self.finish_phase(label, t_start, info, max_rate, native_192k)

    def shm_phase(self):
        label, track_id = "FLAC SHM", "44436"
        self.say(f"=== {label} (id {track_id}); max_rate={MAX_RATE} Hz; sink=shm")
        info = Cli.fields(self.cli.raw(f"songinfo 0 100 track_id:{track_id} tags:aloTd"))
        start = time.time()
        self.cli.player(f"playlistcontrol cmd:load track_id:{track_id}")
        if not self.wait_playing():
            self.check(label, "play", False, "LMS never reached play with advancing time")
            return
        samples = []
        path = "/dev/shm/squeeze" + f"lite-{PLAYER_MAC}"
        with open(os.path.join(self.outdir, "shm-extension.txt"), "w") as dump:
            for index in range(11):  # baseline plus ten one-second intervals
                if index:
                    time.sleep(1)
                try:
                    header, ext, raw = shm_snapshot(path)
                except (OSError, ValueError) as e:
                    self.check(label, "snapshot", False, str(e))
                    return
                stamp = time.monotonic()
                samples.append((stamp, header, ext))
                dump.write(f"sample={index} monotonic={stamp:.6f} offset=32848\n{raw.hex(' ')}\n")
                dump.flush()
        self.check(label, "abi", all(e[0] == 0x48555345 and e[1] == 1 and e[2] == 0
                   and e[3] % 2 == 0 for _, _, e in samples),
                   "magic/version/flags and stable even sequence checked on 11 samples")
        self.check(label, "generation", len({e[4] for _, _, e in samples}) == 1,
                   f"generation={samples[0][2][4]:016x}; constant across 10 seconds")
        lines = self.player.since(start)
        rates = [int(m.group(1)) for line in lines for m in [re.search(r"rate=(\d+)", line)] if m]
        rate = rates[-1] if rates else 0
        self.check(label, "rate", rate > 0 and rate <= MAX_RATE
                   and all(h[3] == rate for _, h, _ in samples),
                   f"stream rate={rate} Hz; SHM rates={sorted({h[3] for _, h, _ in samples})}")
        elapsed = samples[-1][0] - samples[0][0]
        advanced = samples[-1][2][5] - samples[0][2][5]
        ratio = advanced / (rate * elapsed) if rate and elapsed else 0
        self.check(label, "pace", 0.95 <= ratio <= 1.05
                   and all(e[5] > samples[i - 1][2][5] for i, (_, _, e) in enumerate(samples) if i),
                   f"+{advanced} frames in {elapsed:.3f}s at {rate} Hz; ratio={ratio:.4f}")
        self.finish_phase(label, start, info)

    def transition_device_phases(self, lms, prefs):
        logpath = os.path.join(self.outdir, "player.log")
        def restart(path):
            self.cli.player("stop")
            self.player.stop()
            self.player = PlayerProcess(lms, logpath, MAX_RATE, append=True, wav=path)
            for _ in range(20):
                if any("HELO" in line for line in self.player.since(0)): return
                time.sleep(.5)
            raise RuntimeError("WAV player sent no HELO")
        prefs.set("transitionType", 0)
        for mode, label in [(1, "ReplayGain on"), (0, "ReplayGain off")]:
            prefs.set("replayGainMode", mode)
            path = os.path.join(self.outdir, "replaygain-on.wav" if mode else "replaygain-off.wav")
            restart(path)
            start = time.time()
            self.cli.player("playlistcontrol cmd:load track_id:47145")
            if not self.wait_playing(): raise RuntimeError(f"{label}: playback never advanced")
            self.measure(PLAY_SECS)
            self.cli.player("stop")
            self.player.stop()
            self.rg_phases.append((label, start, time.time(), path))
        prefs.set("transitionSmart", 0)
        prefs.set("transitionType", 1)
        prefs.set("transitionDuration", 5)
        path = os.path.join(self.outdir, "crossfade.wav")
        restart(path)
        info = Cli.fields(self.cli.raw("songinfo 0 100 track_id:44436 tags:aloTd"))
        duration = float(info.get("duration") or 0)
        if duration <= 20: raise RuntimeError("crossfade first track duration must exceed 20 seconds")
        self.cli.player("playlistcontrol cmd:load track_id:44436")
        self.cli.player("playlistcontrol cmd:add track_id:41965")
        if not self.wait_playing(): raise RuntimeError("crossfade playlist never started")
        self.cli.player(f"time {duration - 20:.3f}")
        start = time.time()
        self.measure(28)
        self.cli.player("stop")
        self.player.stop()
        self.cross_phase = (start, time.time(), path)

    def analyse_transitions(self, messages):
        try:
            gains = []
            wavs = []
            for label, t0, t1, path in self.rg_phases:
                starts = [m for m in messages if m[2] == "strm" and len(m[3]) >= 24
                          and m[3][0] == ord('s') and t0 <= m[0] <= t1]
                if not starts: raise ValueError(f"{label}: no captured strm gain")
                gains.append(struct.unpack_from(">I", starts[0][3], 14)[0])
                wavs.append(read_wav(path))
            if len(wavs) == 2:
                rate, on = wavs[0]; other_rate, off = wavs[1]
                if rate != other_rate: raise ValueError("ReplayGain WAV rates differ")
                lo, hi = rate * 2 * 2, rate * 8 * 2
                if min(len(on), len(off)) < hi: raise ValueError("ReplayGain aligned 2–8s window unavailable")
                on_power = sum(float(v) ** 2 for v in on[lo:hi])
                off_power = sum(float(v) ** 2 for v in off[lo:hi])
                if not on_power or not off_power: raise ValueError("ReplayGain window is silent")
                measured = 10 * math.log10(on_power / off_power)
                expected = 20 * math.log10((gains[0] or 65536) / (gains[1] or 65536))
                self.check("ReplayGain", "level", gains[0] not in (0, 65536) and gains[1] in (0, 65536)
                           and abs(measured - expected) <= .1,
                           f"sent={gains}, expected={expected:.3f} dB, measured={measured:.3f} dB (aligned 2–8s)")
            if self.cross_phase:
                t0, t1, path = self.cross_phase
                lines = self.player.since(t0, t1)
                starts = [re.search(r"crossfade start frame=(\d+) length=(\d+) rate=(\d+)", l) for l in lines]
                starts = [m for m in starts if m]
                ends = [re.search(r"crossfade complete frame=(\d+)", l) for l in lines]
                ends = [m for m in ends if m]
                if not starts or not ends: raise ValueError("crossfade start/completion missing from player log")
                begin, length, rate = map(int, starts[0].groups())
                finish = int(ends[0].group(1))
                wav_rate, values = read_wav(path)
                frames = len(values) // 2
                boundaries = [re.search(r"boundary frame=(\d+) rate=(\d+)", line) for line in lines]
                boundaries = [int(m.group(1)) for m in boundaries if m]
                starts_wire = [m for m in messages if m[2] == "strm" and len(m[3]) >= 24
                               and m[3][0] == ord('s') and t0 <= m[0] <= t1]
                sent_cross = any(m[3][9] == 5 and m[3][10] == ord('1') for m in starts_wire)
                self.check("Crossfade", "window", abs(length / rate - 5) <= .1 and finish - begin == length
                           and begin in boundaries and sent_cross,
                           f"overlap starts {length/rate:.3f}s before old end; frames {begin}..{finish}; length={length}")
                interval = values[begin * 2:finish * 2]
                # A missing-output gap appears as a silent ten-ms block in this music window.
                chunk = max(2, rate // 100 * 2)
                no_gap = len(interval) == length * 2 and all(any(interval[i:i + chunk]) for i in range(0, len(interval), chunk))
                self.check("Crossfade", "wav", wav_rate == rate and frames > finish and no_gap,
                           f"WAV frames={frames}, rate={wav_rate}; complete overlap, no silent 10ms blocks")
                bad = [l for l in lines if re.search(r"STMo|STMn|error", l)]
                self.check("Crossfade", "clean", not bad, f"underrun/error lines={bad[:3]}")
        except (OSError, ValueError, ZeroDivisionError) as e:
            self.check("Transitions", "analysis", False, str(e))

    def finish_phase(self, label, t_start, info, max_rate=MAX_RATE, native_192k=False):
        t_end = time.time()
        self.phases.append((label, t_start, t_end, info, max_rate, native_192k))
        lines = self.player.since(t_start, t_end)
        bad = [l for l in lines if re.search(r"\bSTMn\b|error|unsupported", l, re.I)]
        rates = sorted({m.group(1) for l in lines for m in [re.search(r"rate=(\d+)", l)] if m})
        self.check(label, "clean", not bad,
                   ("no STMn/error lines" if not bad else f"{len(bad)} line(s), first: {bad[0]}")
                   + (f"; stream rate(s) {', '.join(rates)} Hz" if rates else ""))
        high = [r for r in rates if int(r) > max_rate]
        if native_192k:
            expected = int(info.get("samplerate") or 0)
            self.check(label, "rate", expected == NATIVE_MAX_RATE and bool(rates)
                       and all(int(r) == expected for r in rates),
                       f"audio rate(s) {', '.join(rates) or 'missing'} Hz; songinfo={expected} Hz; "
                       f"phase maximum={max_rate} Hz")
        elif high:
            self.check(label, "rate", False, f"audio arrived above {max_rate} Hz: {', '.join(high)}")

    def analyse_capture(self, pcap, lms):
        try:
            messages = slimproto_messages(pcap, lms)
        except (OSError, ValueError) as e:
            self.check("capture", "decode", False, str(e))
            return
        with open(os.path.join(self.outdir, "slimproto.txt"), "w") as f:
            for m in messages:
                f.write(describe(m) + "\n")
        helos = [m for m in messages if m[2] == "HELO"]
        if helos:
            caps = helos[0][3][36:].decode("latin-1", "replace")
            formats = [c for c in caps.split(",") if "=" not in c]
            self.check("setup", "caps", formats == EXPECTED_CAPS,
                       f"advertised {','.join(formats)} (expected {','.join(EXPECTED_CAPS)})")
        else:
            self.check("setup", "caps", False, "no HELO in the capture")
        if self.native_start is not None:
            native_helos = [m for m in helos if m[0] >= self.native_start]
            caps = native_helos[0][3][36:].decode("latin-1", "replace").split(",") if native_helos else []
            formats = [c for c in caps if "=" not in c]
            self.check("native setup", "caps", formats == EXPECTED_CAPS
                       and f"MaxSampleRate={NATIVE_MAX_RATE}" in caps,
                       f"second-phase HELO: {','.join(caps) or 'missing'}; "
                       f"expected MaxSampleRate={NATIVE_MAX_RATE}, {','.join(EXPECTED_CAPS)}")
        self.analyse_transitions(messages)
        for label, t0, t1, info, max_rate, native_192k in self.phases:
            starts = [m for m in messages if m[2] == "strm" and m[1] == "S>P"
                      and len(m[3]) >= 24 and chr(m[3][0]) == "s" and t0 <= m[0] <= t1]
            if not starts:
                self.check(label, "format", False, "no strm start seen in the capture")
                continue
            codes = [chr(m[3][2]) for m in starts]
            rate = int(info.get("samplerate") or 0)
            ctype = info.get("type")
            native = {"flc": "f", "alc": "l", "mp3": "m", "aif": "p", "wav": "p"}.get(ctype)
            if native_192k:
                ok = all(c == "l" for c in codes)
                expect = "l (native ALAC at 192000 Hz)"
            elif rate > max_rate:
                ok = all(c in "fp" for c in codes)
                expect = f"f or p (LMS converts {rate} Hz to <= {max_rate} Hz)"
            else:
                ok = native is not None and all(c == native for c in codes)
                expect = f"{native} (native {ctype})"
            names = ", ".join(f"{c}={FORMAT_NAMES.get(c, '?')}" for c in codes)
            self.check(label, "format", ok, f"LMS sent {names}; expected {expect}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lms", default=None, help="LMS host (default: /etc/yeney/config)")
    ap.add_argument("--no-capture", action="store_true", help="skip the tcpdump capture")
    ap.add_argument("--keep-player", action="store_true",
                    help="do not remove the test player from LMS afterwards")
    args = ap.parse_args()

    if not os.access("./yeney-player", os.X_OK):
        sys.exit("Run from the yeney-core checkout after `make` (./yeney-player not found).")
    lms = args.lms or lms_host_from_config()
    if not lms:
        sys.exit("No LMS host: pass --lms <host> or set LMS_SERVER in /etc/yeney/config.")
    cli = Cli(lms)
    try:
        cli.raw("version ?")
    except OSError as e:
        sys.exit(f"LMS CLI not reachable on {lms}:{CLI_PORT}: {e}")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = f"/tmp/core-test-{stamp}"
    os.makedirs(outdir)
    disabled_formats = cli.raw("pref disabledformats ?")
    build = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(outdir, "run-info.txt"), "w") as f:
        f.write(f"yeney-core={build or '?'}\nlms={lms}\ndisabledformats={disabled_formats}\nplayer={PLAYER_NAME} {PLAYER_MAC}\n"
                f"tracks={' '.join(t[1] for t in TRACKS + [NATIVE_TRACK])}\n"
                f"initial_max_rate={MAX_RATE}\nnative_max_rate={NATIVE_MAX_RATE}\nshm_max_rate={MAX_RATE}\nshm_track=44436\nstarted={time.strftime('%Y-%m-%d %H:%M:%S')}\n")

    pcap = os.path.join(outdir, "slimproto.pcap")
    capture = None if args.no_capture else Capture(lms, pcap)
    events = EventListener(lms, os.path.join(outdir, "lms-events.log"))
    player = PlayerProcess(lms, os.path.join(outdir, "player.log"))
    run = Run(cli, player, outdir)
    run.say(f"yeney-core {build or '?'}; LMS {lms}; player {PLAYER_NAME} ({PLAYER_MAC})")
    prefs = PreferenceGuard(cli, run)
    if capture and capture.error:
        run.say(f"capture disabled: {capture.error}")

    try:
        for _ in range(20):
            if PLAYER_MAC in cli.raw("players 0 100").replace("%3A", ":"):
                break
            time.sleep(0.5)
        else:
            run.check("setup", "connect", False, "player did not appear in LMS within 10 s")
            raise KeyboardInterrupt
        run.check("setup", "connect", True, "player registered with LMS")
        for spec in TRACKS:
            run.track(*spec)
        cli.player("stop")
        player.stop()
        run.native_start = time.time()
        player = PlayerProcess(lms, os.path.join(outdir, "player.log"), NATIVE_MAX_RATE, append=True)
        run.player = player
        run.say(f"restarted {PLAYER_NAME} ({PLAYER_MAC}); max_rate={NATIVE_MAX_RATE} Hz")
        for _ in range(20):
            if any("HELO" in line for line in player.since(run.native_start)):
                break
            time.sleep(0.5)
        else:
            run.check("native setup", "connect", False, "restarted player sent no HELO within 10 s")
            raise KeyboardInterrupt
        run.check("native setup", "connect", True, "restarted player sent HELO")
        run.track(*NATIVE_TRACK, max_rate=NATIVE_MAX_RATE, native_192k=True)
        cli.player("stop")
        player.stop()
        player = PlayerProcess(lms, os.path.join(outdir, "player.log"), MAX_RATE, append=True, shm=True)
        run.player = player
        for _ in range(20):
            if any("HELO" in line for line in player.since(0)):
                break
            time.sleep(0.5)
        else:
            run.check("SHM setup", "connect", False, "SHM player sent no HELO within 10 s")
            raise KeyboardInterrupt
        run.check("SHM setup", "connect", True, "SHM player sent HELO")
        run.shm_phase()
        run.transition_device_phases(lms, prefs)
        player = run.player
    except KeyboardInterrupt:
        run.check("run", "interrupted", False, "stopped early")
    except Exception as e:
        run.check("run", "error", False, str(e))
    finally:
        player = run.player
        prefs.restore()
        try:
            cli.player("stop")
        except OSError:
            pass
        time.sleep(1)
        player.stop()
        if capture:
            capture.stop()
        events.stop()
        if not args.keep_player:
            try:
                cli.player("client forget")
            except OSError:
                pass

    if capture and not capture.error and os.path.exists(pcap):
        run.analyse_capture(pcap, lms)
    elif run.rg_phases or run.cross_phase:
        run.check("Transitions", "capture", False, "capture is required to verify sent ReplayGain")

    failed = [r for r in run.results if not r[2]]
    run.say("")
    run.say("track        | check   | result")
    for track, name, ok, _ in run.results:
        run.say(f"{track:<12} | {name:<7} | {'PASS' if ok else 'FAIL'}")
    run.say(f"{len(run.results) - len(failed)}/{len(run.results)} checks passed")
    with open(os.path.join(outdir, "run-info.txt"), "a") as f:
        f.write(f"finished={time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    run.report.close()
    run.status_log.close()
    tarball = f"{outdir}.tar.gz"
    with tarfile.open(tarball, "w:gz") as tar:
        tar.add(outdir, arcname=os.path.basename(outdir))
    print(f"Tarball: {tarball}", flush=True)
    sys.exit(0 if run.results and not failed else 1)


if __name__ == "__main__":
    main()
