#!/usr/bin/env python3
# Copyright (c) 2026 Jaap van Vliet
# Original implementation for the YeneY project.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
"""core-device-test.py -- unattended real-LMS test of yeney-player.

Starts ./yeney-player as a silent test player ("Core test"), lets Lyrion play a
fixed set of tracks on it and checks, per track, via the LMS CLI and the
player's own event log:

  play    the track starts and LMS position advances at real-time speed
  seek    a jump to the middle lands there and keeps advancing
  pause   position freezes while paused
  resume  position advances again after resume
  clean   no STMn / error lines from the player for this track

Run on the bridge host from the yeney-core checkout, after `make`:

  python3 scripts/core-device-test.py [--lms <host>] [--keep-player]

Results go to /tmp/core-test-<timestamp>/ (report.txt, player.log,
lms-status.log). Exit status 0 only if every check passes.

Tracks: edit TRACKS below. A spec is either "id:<n>" or a title search; the
first match with the expected LMS content type (and, for hi-res, a sample rate
above 48 kHz) is used. Every match considered is written to the report.
"""
import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.parse

# (label, search text or "id:<n>", LMS content type, minimum sample rate)
TRACKS = [
    ("ALAC", "id:47145", "alc", 0),            # The Lady Is A Tramp (Duets II)
    ("MP3", "Crowd Control (Original Mix)", "mp3", 0),
    ("FLAC", "id:44436", "flc", 0),            # Just A Little Bit More (Extended)
    ("ALAC hi-res", "Sharp Dressed Man", "alc", 48001),
]

PLAYER_NAME = "Core test"
PLAYER_MAC = "02:00:00:00:be:03"
CLI_PORT = 9090

PLAY_SECS = 12      # measure normal playback over this many seconds
SEEK_TO = 60        # seconds; capped at half the track duration
PAUSE_SECS = 5
RATE_MIN, RATE_MAX = 0.85, 1.15   # accepted LMS-time / wall-time ratio


# ---------------------------------------------------------------- helpers --

def now():
    return time.strftime("%H:%M:%S")


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
    def records(cls, reply, start="id"):
        out, cur = [], None
        for t in cls.tokens(reply):
            k, _, v = t.partition(":")
            if k == start:
                cur = {}
                out.append(cur)
            if cur is not None and _:
                cur.setdefault(k, v)
        return out


def lms_host_from_config():
    try:
        for line in open("/etc/yeney/config"):
            if line.startswith("LMS_SERVER="):
                return line.split("=", 1)[1].strip() or None
    except OSError:
        pass
    return None


class PlayerProcess:
    """Runs yeney-player and keeps its event lines with timestamps."""

    def __init__(self, lms, logpath):
        self.lines = []
        self.lock = threading.Lock()
        self.log = open(logpath, "w")
        self.proc = subprocess.Popen(
            ["./yeney-player", "-n", PLAYER_NAME, "-m", PLAYER_MAC, "-s", lms,
             "--sink", "null", "-d", "1"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            stamp = time.time()
            with self.lock:
                self.lines.append((stamp, line))
            self.log.write(f"[{now()}] {line}\n")
            self.log.flush()

    def since(self, t0):
        with self.lock:
            return [l for (t, l) in self.lines if t >= t0]

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()


# ------------------------------------------------------------------- test --

class Run:
    def __init__(self, cli, player, outdir):
        self.cli, self.player, self.outdir = cli, player, outdir
        self.results = []   # (track, check, ok, detail)
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
        """LMS time advance over `secs` wall seconds (sampled every second)."""
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

    def resolve(self, label, spec, ctype, min_rate):
        if spec.startswith("id:"):
            r = self.cli.raw(f"songinfo 0 100 track_id:{spec[3:]} tags:aloTd")
            rec = {k: v for k, _, v in (t.partition(":") for t in Cli.tokens(r)) if _}
            rec["id"] = spec[3:]
            self.say(f"{label}: using id {rec['id']} ({rec.get('title')}, {rec.get('type')})")
            return rec
        r = self.cli.raw(f"titles 0 50 search:{urllib.parse.quote(spec)} tags:aloTd")
        matches = Cli.records(r)
        self.say(f"{label}: search '{spec}' -> {len(matches)} match(es)")
        chosen = None
        for m in matches:
            rate = int(m.get("samplerate") or 0)
            fits = m.get("type") == ctype and rate >= min_rate
            self.say(f"    id {m.get('id')}: {m.get('title')} / {m.get('artist')} / "
                     f"{m.get('album')} type={m.get('type')} rate={rate}"
                     f"{'  <- chosen' if fits and not chosen else ''}")
            if fits and not chosen:
                chosen = m
        return chosen

    def track(self, label, spec, ctype, min_rate):
        self.say(f"=== {label}")
        rec = self.resolve(label, spec, ctype, min_rate)
        if not rec:
            self.check(label, "found", False,
                       f"no match with type={ctype}" + (f" and rate>={min_rate}" if min_rate else ""))
            return
        duration = float(rec.get("duration") or 0)
        t_start = time.time()

        # play
        self.cli.player(f"playlistcontrol cmd:load track_id:{rec['id']}")
        started = self.wait_playing()
        if not started:
            self.check(label, "play", False, "LMS never reached play with advancing time")
            self.clean(label, t_start)
            return
        adv, wall, t0, t1 = self.measure(PLAY_SECS)
        ratio = adv / wall if wall else 0
        self.check(label, "play", RATE_MIN <= ratio <= RATE_MAX,
                   f"{t0:.1f}s -> {t1:.1f}s in {wall:.1f}s wall (ratio {ratio:.2f})")

        # seek
        target = min(SEEK_TO, duration / 2) if duration else SEEK_TO
        self.cli.player(f"time {target:.0f}")
        time.sleep(4)
        mode, t_after, _ = self.status()
        adv, wall, t0, t1 = self.measure(5)
        ok = mode == "play" and target - 1 <= t0 <= target + 8 and adv >= 3
        self.check(label, "seek", ok,
                   f"target {target:.0f}s, at {t0:.1f}s after 4s, then +{adv:.1f}s in {wall:.1f}s")

        # pause
        self.cli.player("pause 1")
        time.sleep(2)
        _, p0, _ = self.status()
        time.sleep(PAUSE_SECS)
        mode, p1, _ = self.status()
        self.check(label, "pause", mode == "pause" and abs(p1 - p0) < 0.5,
                   f"mode={mode}, {p0:.1f}s -> {p1:.1f}s over {PAUSE_SECS}s")

        # resume
        self.cli.player("pause 0")
        time.sleep(3)
        adv, wall, t0, t1 = self.measure(5)
        mode, _, _ = self.status()
        self.check(label, "resume", mode == "play" and adv >= 3.5 and t0 >= p1 - 0.5,
                   f"from {p1:.1f}s: {t0:.1f}s -> {t1:.1f}s (+{adv:.1f}s in {wall:.1f}s)")

        self.clean(label, t_start)

    def clean(self, label, t_start):
        lines = self.player.since(t_start)
        bad = [l for l in lines if re.search(r"\bSTMn\b|error|unsupported", l, re.I)]
        rates = sorted({m.group(1) for l in lines for m in [re.search(r"rate=(\d+)", l)] if m})
        self.check(label, "clean", not bad,
                   ("no STMn/error lines" if not bad else f"{len(bad)} line(s), first: {bad[0]}")
                   + (f"; stream rate(s) {', '.join(rates)} Hz" if rates else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lms", default=None, help="LMS host (default: /etc/yeney/config)")
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

    outdir = time.strftime("/tmp/core-test-%Y%m%d-%H%M%S")
    os.makedirs(outdir)
    player = PlayerProcess(lms, os.path.join(outdir, "player.log"))
    run = Run(cli, player, outdir)
    build = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True).stdout.strip()
    run.say(f"yeney-core {build or '?'}; LMS {lms}; player {PLAYER_NAME} ({PLAYER_MAC})")

    try:
        for _ in range(20):
            if PLAYER_MAC in cli.raw("players 0 100").replace("%3A", ":"):
                break
            time.sleep(0.5)
        else:
            run.check("setup", "connect", False, "player did not appear in LMS within 10 s")
            raise SystemExit
        run.say("player connected to LMS")
        for spec in TRACKS:
            run.track(*spec)
    except KeyboardInterrupt:
        run.say("interrupted")
    finally:
        try:
            cli.player("stop")
        except OSError:
            pass
        player.stop()
        if not args.keep_player:
            try:
                cli.player("client forget")
            except OSError:
                pass

    failed = [r for r in run.results if not r[2]]
    run.say("")
    run.say("track        | check  | result")
    for track, name, ok, detail in run.results:
        run.say(f"{track:<12} | {name:<6} | {'PASS' if ok else 'FAIL'}")
    run.say(f"{len(run.results) - len(failed)}/{len(run.results)} checks passed; results in {outdir}")
    sys.exit(0 if run.results and not failed else 1)


if __name__ == "__main__":
    main()
