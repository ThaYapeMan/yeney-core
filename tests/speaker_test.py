# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Remote queue evidence is authoritative even at internal frame zero."""
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time
from fake_lms import LMS, HTTP, ROOT
with tempfile.TemporaryDirectory(prefix='yeney-speaker-') as folder:
    path = Path(folder)/'state'
    lms = LMS()
    source = HTTP(struct.pack('<hh', 100, -100)*8000)
    proc = subprocess.Popen([str(ROOT/'test-speaker'), str(lms.listener.getsockname()[1]), str(path)])
    serial = 0
    def state(ms, value):
        global serial
        serial += 1
        temp = path.with_suffix('.next'); temp.write_text(f'{serial} {ms} {value}\n'); os.replace(temp,path)
    try:
        lms.accept()
        lms.strm('s', source, rate=8000)
        lms.wait('DSCO')
        assert not any(p.get('event') in ('STMs','STMd') for p in lms.packets)
        state(0,'r')
        assert lms.wait('STMs')['elapsed'] == 1
        lms.wait('STMd')
        assert lms.timer()['elapsed'] == 1
        state(100,'r'); time.sleep(.04)
        assert lms.timer()['elapsed'] == 100
        state(100,'p'); assert lms.wait('STMp')['elapsed'] == 100
        state(100,'r'); lms.wait('STMr')
        state(100,'s'); lms.wait('STMu')
        assert len([p for p in lms.packets if p.get('event') == 'STMs']) == 1
        assert len([p for p in lms.packets if p.get('event') == 'STMu']) == 1
        print('PASS speaker-driven queue core: confirmed internal zero starts once, STMs before STMd, protocol-only 1 ms sentinel, authoritative elapsed and pause/resume/stop')
    finally:
        proc.send_signal(signal.SIGTERM); proc.wait(3)
        lms.close(); source.close()
        assert proc.returncode == 0
