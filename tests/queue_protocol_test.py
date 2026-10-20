# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Queue status discipline; all endpoints are loopback and clocks are explicit."""
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time
from fake_lms import LMS, HTTP, ROOT

with tempfile.TemporaryDirectory(prefix='yeney-protocol-') as folder:
    path = Path(folder)/'speaker'
    lms = LMS()
    source = HTTP(struct.pack('<hh', 100, -100)*8000*30)
    proc = subprocess.Popen([str(ROOT/'test-speaker'), str(lms.listener.getsockname()[1]), str(path)],
                            env={**os.environ, 'YENEY_TEST_REMOTE': '1'}, stdout=subprocess.DEVNULL)
    try:
        lms.accept(); lms.strm('s', source, rate=8000); lms.wait('DSCO')
        path.write_text('1 0 r\n'); start = lms.wait('STMs')
        for serial, ms in enumerate((0, 9999, 10000), 2):
            path.write_text(f'{serial} {ms} r\n'); time.sleep(.02); lms.timer()
            assert not any(p.get('event')=='STMd' for p in lms.packets), lms.packets
        path.write_text('5 10001 r\n'); done = lms.wait('STMd')
        assert done['jiffies'] > start['jiffies'], (start, done)
        assert len([p for p in lms.packets if p.get('event')=='STMd']) == 1
        assert len([p for p in lms.packets if p['op']=='HELO']) == 1
        print('PASS remote queue STMd: fully produced 30 s item held until remaining <20 s; speaker STMs first, one HELO, one STMd')
    finally:
        proc.send_signal(signal.SIGTERM); proc.wait(3); lms.close(); source.close()

for body in (b'', b'x', struct.pack('<hh', 100, -100)*2400+b'x'):
    with tempfile.TemporaryDirectory(prefix='yeney-protocol-fault-') as folder:
        lms=LMS(); source=HTTP(body)
        proc=subprocess.Popen([str(ROOT/'test-speaker'), str(lms.listener.getsockname()[1]), str(Path(folder)/'speaker')], stdout=subprocess.DEVNULL)
        try:
            lms.accept(); lms.strm('s', source, rate=8000)
            if not body: lms.wait('STMn')
            else:
                lms.wait('DSCO'); time.sleep(.05); lms.timer()
            assert sum(p.get('event')=='STMn' for p in lms.packets)==int(not body), lms.packets
        finally:
            proc.send_signal(signal.SIGTERM); proc.wait(3); lms.close(); source.close()
print('PASS queue STMn: only zero audio bytes; malformed and partially decoded received audio never emits StreamingFailed')

with tempfile.TemporaryDirectory(prefix='yeney-cached-protocol-') as folder:
    path=Path(folder)/'speaker';lms=LMS();source=HTTP(struct.pack('<hh',1,-1)*8000)
    cached_source=HTTP(source.body)
    log=path.with_suffix('.log').open('w')
    proc=subprocess.Popen([str(ROOT/'test-speaker'),str(lms.listener.getsockname()[1]),str(path)],
                          env={**os.environ,'YENEY_TEST_CACHED':'1'},stdout=log)
    try:
        lms.accept();lms.strm('s',source,rate=8000);lms.wait('DSCO')
        path.write_text('1 0 r\n');lms.wait('STMs');lms.wait('STMd')
        lms.strm('q');lms.wait('STMf');lms.strm('s',cached_source,rate=8000)
        ack=lms.wait('STMc');path.write_text('2 0 c\n');start=lms.wait('STMs');done=lms.wait('STMd')
        assert ack['jiffies']<=start['jiffies']<done['jiffies'],(ack,start,done)
        assert sum(p.get('event')=='STMc' for p in lms.packets)==2
        assert sum(p.get('event')=='STMs' for p in lms.packets)==2
        assert sum(p.get('event')=='STMe' for p in lms.packets)==2 # HTTP handshake, retained PCM
        assert sum(p['op']=='HELO' for p in lms.packets)==1
        print('PASS cached LMS run: STMc before fresh confirmed STMs, STMd next round, same retained PCM, HTTP response handshake, no decoder/HELO or stop echo')
    finally:
        proc.send_signal(signal.SIGTERM);proc.wait(3);lms.close();source.close();cached_source.close();log.close()
    assert path.with_suffix('.log').read_text().splitlines().count('TRACK')==1

with tempfile.TemporaryDirectory(prefix='yeney-autostart-zero-') as folder:
    path=Path(folder)/'speaker';lms=LMS();source=HTTP(struct.pack('<hh',1,-1)*8000)
    proc=subprocess.Popen([str(ROOT/'test-speaker'),str(lms.listener.getsockname()[1]),str(path)],
                          env={**os.environ,'YENEY_TEST_HOST_CONTROL':'1'},stdout=subprocess.DEVNULL)
    try:
        lms.accept();lms.strm('s',source,rate=8000,autostart=0);lms.wait('STMl')
        assert not any(p.get('event') in ('STMs','STMr') for p in lms.packets)
        lms.strm('u');path.write_text('1 0 r\n');lms.wait('STMs');lms.wait('STMd')
        assert not any(p.get('event')=='STMr' for p in lms.packets)
        print('PASS queue autostart=0: LMS u releases production; STMs/STMd still require speaker confirmation; no fabricated STMr')
        lms.strm('p');path.write_text('2 100 p\n');lms.wait('STMp')
        lms.strm('u');lms.timer()
        assert not any(p.get('event')=='STMr' for p in lms.packets)
        path.write_text('3 100 r\n');lms.wait('STMr')
        print('PASS consumed queue resume: confirmed pause preserved until speaker resumes, exactly one STMr, no premature acknowledgement')
    finally:
        proc.send_signal(signal.SIGTERM);proc.wait(3);lms.close();source.close()

with tempfile.TemporaryDirectory(prefix='yeney-relative-clock-') as folder:
    path=Path(folder)/'speaker';lms=LMS();source=HTTP(struct.pack('<hh',1,-1)*8000)
    proc=subprocess.Popen([str(ROOT/'test-speaker'),str(lms.listener.getsockname()[1]),str(path)],
                          env={**os.environ,'YENEY_TEST_RELATIVE':'1'},stdout=subprocess.DEVNULL)
    try:
        lms.accept();lms.strm('s',source,rate=8000);lms.wait('DSCO')
        path.write_text('1 20000 r\n');assert lms.wait('STMs')['elapsed']==1;lms.wait('STMd')
        path.write_text('2 20100 r\n');time.sleep(.03);assert lms.timer()['elapsed']==100
        print('PASS LMS seek clock: STAT is relative to the new run, source origin remains speaker-confirmed; no double seek offset')
    finally:
        proc.send_signal(signal.SIGTERM);proc.wait(3);lms.close();source.close()

with tempfile.TemporaryDirectory(prefix='yeney-deferred-start-') as folder:
    path=Path(folder)/'speaker';lms=LMS();source=HTTP(struct.pack('<hh',1,-1)*8000)
    proc=subprocess.Popen([str(ROOT/'test-speaker'),str(lms.listener.getsockname()[1]),str(path)],
                          env={**os.environ,'YENEY_TEST_DEFER':'1'},stdout=subprocess.DEVNULL)
    try:
        lms.accept();lms.strm('s',source,rate=8000);time.sleep(.05);lms.timer()
        assert source.request is None and not any(p.get('event')=='STMc' for p in lms.packets)
        path.write_text('1 0 d\n');lms.wait('STMs');lms.wait('STMd')
        assert sum(p.get('event')=='STMc' for p in lms.packets)==1
        assert sum(p['op']=='HELO' for p in lms.packets)==1
        print('PASS deferred queue start: host readiness resumes original request once; no CLI/server resend, early stream output or extra HELO')
    finally:
        proc.send_signal(signal.SIGTERM);proc.wait(3);lms.close();source.close()
