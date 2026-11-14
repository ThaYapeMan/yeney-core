# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Host requests use the current speaker clock without changing heartbeat cadence."""
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import tempfile
import time
from fake_lms import LMS, HTTP, ROOT

with tempfile.TemporaryDirectory(prefix='status-request-') as folder:
    path = Path(folder) / 'state'
    lms = LMS()
    source = HTTP(struct.pack('<hh', 100, -100) * 8000)
    proc = subprocess.Popen([os.environ.get('YENEY_STATUS_BINARY', str(ROOT / 'test-speaker')), str(lms.listener.getsockname()[1]), str(path)],
                            stdin=subprocess.PIPE, env={**os.environ, 'YENEY_TEST_STATUS_REQUEST': '1'})
    try:
        lms.accept()
        lms.strm('s', source, rate=8000)
        lms.wait('DSCO')
        path.write_text('1 12345 r\n')
        lms.wait('STMs')
        lms.wait('STMd')
        # Synchronize to the periodic heartbeat, leaving a full second for requests.
        lms.wait('STMt')
        assert not select.select([lms.connection], [], [], .08)[0], 'unsolicited extra STAT'
        path.write_text('2 23456 r\n')
        time.sleep(.01)
        start = time.monotonic()
        proc.stdin.write(b'r' * 1000)
        proc.stdin.flush()
        first = lms.wait('STMt')
        delay = first['time'] - start
        assert delay < .020, delay
        assert first['elapsed'] == 23456, first
        # A burst may race the first exchange, but any follow-up is rate limited.
        extra = []
        while select.select([lms.connection], [], [], .08)[0]:
            extra.append(lms.read())
        assert len(extra) <= 1, extra
        if extra:
            assert extra[0]['event'] == 'STMt', extra
            assert extra[0]['jiffies'] - first['jiffies'] >= 50, extra
        assert not select.select([lms.connection], [], [], .08)[0]
        print(f'PASS B1 request STMt delay_ms={delay*1000:.3f} elapsed=23456; 1000 requests coalesce; no request no extra STAT')
    finally:
        proc.stdin.close()
        proc.send_signal(signal.SIGTERM)
        proc.wait(3)
        lms.close()
        source.close()
        assert proc.returncode == 0
