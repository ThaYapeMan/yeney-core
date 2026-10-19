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

# Refetching a progressive item is explicitly authorized by the queue host;
# default and subsequent unarmed starts retain destructive flush semantics.
with tempfile.TemporaryDirectory(prefix='yeney-refetch-') as folder:
    path = Path(folder)/'state'
    lms = LMS()
    body = struct.pack('<hh',100,-100)*8000*24
    sources = [HTTP(body,first=4*8000*4,delay=60),
               HTTP(body,first=4*8000*4,delay=60), HTTP(body[:8000*4])]
    with (Path(folder)/'core.log').open('w+') as log:
        proc = subprocess.Popen([str(ROOT/'test-speaker'),str(lms.listener.getsockname()[1]),str(path)],stdout=log)
        def output():
            log.flush()
            return Path(log.name).read_text()
        try:
            lms.accept(); lms.strm('s',sources[0],rate=8000)
            lms.wait('RESP'); time.sleep(.1)
            path.write_text('1 0 r\n'); lms.wait('STMs')
            initial = output().count('FLUSH')
            path.write_text('2 0 x\n'); lms.timer()
            lms.strm('s',sources[1],rate=8000)
            lms.wait('STMc'); lms.wait('RESP'); time.sleep(.1)
            assert output().count('REFETCH') == 1, output()
            assert output().count('FLUSH') == initial, output()
            assert len([p for p in lms.packets if p.get('event') == 'STMs']) == 1
            assert not any(p.get('event') == 'STMd' for p in lms.packets)
            lms.strm('s',sources[2],rate=8000)
            lms.wait('STMc'); lms.wait('RESP'); time.sleep(.1)
            assert output().count('FLUSH') == initial+1, output()
            assert output().count('REFETCH') == 1, output()
            print('PASS queue refetch core: one-shot host authorization cancels progressive producer without sink flush or fabricated STMs/STMd; unarmed start still flushes')
        finally:
            proc.send_signal(signal.SIGTERM); proc.wait(3)
            lms.close()
            for source in sources: source.close()
            assert proc.returncode == 0

# Opt-in generation handoff discards the old producer and advertises the stream
# capability before the room asks LMS for a continuation. Default fixtures above
# use generation zero and retain their existing protocol transcripts.
with tempfile.TemporaryDirectory(prefix='yeney-mode-') as folder:
    path = Path(folder)/'state'
    generation = Path(str(path)+'.generation')
    generation.write_text('1')
    lms = LMS()
    source = HTTP(struct.pack('<hh',100,-100)*8000*24, first=8000*4, delay=60)
    with (Path(folder)/'core.log').open('w+') as log:
        proc = subprocess.Popen([str(ROOT/'test-speaker'),str(lms.listener.getsockname()[1]),str(path),'modes'],stdout=log)
        try:
            lms.accept(); lms.strm('s',source,rate=8000); lms.wait('RESP')
            path.write_text('1 0 r\n'); lms.wait('STMs')
            before = Path(log.name).read_text().count('FLUSH')
            generation.write_text('2'); hello=lms.wait('HELO'); assert b'MaxSampleRate=48000' in hello['body'],hello
            end=time.monotonic()+3
            while 'MODE READY 2' not in Path(log.name).read_text() and time.monotonic()<end: time.sleep(.01)
            text=Path(log.name).read_text()
            assert 'MODE READY 2' in text,text
            assert text.count('FLUSH')==before+1,text
            assert text.rindex('FLUSH')<text.index('MODE READY 2'),text
            count=len([p for p in lms.packets if p.get('event') in ('STMs','STMd')])
            time.sleep(.1);lms.timer()
            assert len([p for p in lms.packets if p.get('event') in ('STMs','STMd')])==count
            continuation = HTTP(struct.pack('<hh',200,-200)*8000)
            try:
                lms.strm('s',continuation,rate=8000)
                lms.wait('STMs'); lms.wait('STMu')
                assert len([p for p in lms.packets if p.get('event') == 'STMs']) == 2
            finally: continuation.close()
            print('PASS mode generation core: producer/decoder/PCM reset before readiness, one capability HELO, no late queue start/advance; default generation-zero fixtures unchanged')
        finally:
            proc.send_signal(signal.SIGTERM);proc.wait(3);lms.close();source.close()
            assert proc.returncode==0
