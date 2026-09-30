#!/usr/bin/env python3
# Copyright (c) 2026 Jaap van Vliet
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Offline checks of the device script's two-phase capture/rate contract."""
import importlib.util
from pathlib import Path
import tempfile
import struct
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

    def test_shm_phase_reports_abi_rate_generation_and_pace(self):
        self.run.wait_playing = Mock(return_value=True)
        self.run.cli.raw.return_value = 'songinfo title:fixture samplerate:44100 type:flc'
        self.player.since.return_value = ['boundary rate=44100']
        for field, bad in [(None, None), ('generation', 99), ('rate', 48000), ('sequence', 3), ('pace', 40000)]:
            self.run.results.clear()
            samples = []
            for i in range(11):
                rate = bad if field == 'rate' else 44100
                generation = bad if field == 'generation' and i == 5 else 7
                sequence = bad if field == 'sequence' else i * 2
                position = i * (bad if field == 'pace' else 44100)
                extension = (0x48555345, 1, 0, sequence, generation, position, 0)
                raw = struct.pack('<IHHIQQQ4x', *extension)
                samples.append(((16384, position * 2 % 16384, 1, rate, 0), extension, raw))
            with patch.object(device, 'shm_snapshot', side_effect=samples), \
                    patch.object(device.time, 'sleep'), \
                    patch.object(device.time, 'monotonic', side_effect=range(11)):
                self.run.shm_phase()
            self.assertEqual(self.result('FLAC SHM', 'generation'), field != 'generation')
            self.assertEqual(self.result('FLAC SHM', 'rate'), field != 'rate')
            self.assertEqual(self.result('FLAC SHM', 'abi'), field != 'sequence')
            self.assertEqual(self.result('FLAC SHM', 'pace'), field != 'pace')
            dump = (Path(self.temp.name) / 'shm-extension.txt').read_text()
            self.assertEqual(dump.count('offset=32848'), 11)
            self.assertIn('45 53 55 48 01 00', dump)

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
