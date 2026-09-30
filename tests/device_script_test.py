#!/usr/bin/env python3
# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Offline checks of the device script's two-phase capture/rate contract."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('device', Path(__file__).resolve().parents[1] / 'scripts/core-device-test.py')
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)


class DeviceScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.player = Mock()
        self.run = device.Run(Mock(), self.player, self.temp.name)
        self.run.say = Mock()
        self.addCleanup(self.run.report.close)
        self.addCleanup(self.run.status_log.close)

    def result(self, track, check):
        return next(ok for label, name, ok, _ in self.run.results if (label, name) == (track, check))

    def test_native_rate_must_equal_songinfo(self):
        for rates, expected, ok in [(['192000'], 192000, True), (['48000'], 192000, False),
                                    ([], 192000, False), (['192000', '48000'], 192000, False),
                                    (['96000'], 96000, False)]:
            self.run.results.clear()
            self.player.since.return_value = ['STMs rate=' + r for r in rates]
            self.run.finish_phase('native', 1, {'samplerate': str(expected)}, 192000, True)
            self.assertEqual(self.result('native', 'rate'), ok)
        self.run.results.clear()
        self.player.since.return_value = ['STMs rate=192000']
        self.run.finish_phase('default', 1, {'samplerate': '192000'})
        self.assertFalse(self.result('default', 'rate'))

    def test_capture_requires_new_helo_and_native_wire_code(self):
        def hello(t, rate):
            return (t, 'P>S', 'HELO', bytes(36) +
                    f'MaxSampleRate={rate},alc,flc,mp3,aif,pcm'.encode())
        def start(t, code):
            return (t, 'S>P', 'strm', b's1' + code.encode() + bytes(21))
        self.run.native_start = 10
        self.run.phases = [('default', 2, 4, {'samplerate': '192000', 'type': 'alc'}, 48000, False),
                           ('native', 11, 14, {'samplerate': '192000', 'type': 'alc'}, 192000, True)]
        for new_hello, wire, caps_ok, format_ok in [(hello(10, 192000), 'l', True, True),
                (hello(10, 48000), 'l', False, True), (None, 'l', False, True),
                (hello(10, 192000), 'f', True, False)]:
            messages = [hello(1, 48000), start(3, 'f'), start(12, wire)]
            if new_hello:
                messages.append(new_hello)
            self.run.results.clear()
            with patch.object(device, 'slimproto_messages', return_value=sorted(messages)):
                self.run.analyse_capture('unused', '127.0.0.1')
            self.assertTrue(self.result('setup', 'caps'))
            self.assertTrue(self.result('default', 'format'))
            self.assertEqual(self.result('native setup', 'caps'), caps_ok)
            self.assertEqual(self.result('native', 'format'), format_ok)

    def test_restart_preserves_identity_and_appends_log(self):
        path = Path(self.temp.name) / 'player.log'
        for rate, append in [(48000, False), (192000, True)]:
            proc = Mock()
            proc.stdout = iter(['HELO reconnect=0\n'])
            proc.poll.return_value = 0
            with patch.object(device.subprocess, 'Popen', return_value=proc) as spawn:
                player = device.PlayerProcess('127.0.0.1', path, rate, append)
                player.stop()
            args = spawn.call_args.args[0]
            self.assertEqual(args[args.index('--max-rate') + 1], str(rate))
            self.assertEqual(args[args.index('-n') + 1], device.PLAYER_NAME)
            self.assertEqual(args[args.index('-m') + 1], device.PLAYER_MAC)
        self.assertEqual(path.read_text().count('HELO reconnect=0'), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
