# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Remote queue evidence is authoritative even at internal frame zero."""
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time
import threading
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
        lms.strm('s',source,rate=8000)
        lms.timer()
        assert sum(p.get('event')=='STMc' for p in lms.packets)==1,lms.packets
        assert sum(p['op']=='HELO' for p in lms.packets)==1,lms.packets
        print('PASS repeated queue start while preparing: identical request retains one producer, no duplicate STMc, no premature STMs/STMd or HELO')
        assert not any(p.get('event') in ('STMs','STMd') for p in lms.packets)
        state(0,'r')
        assert lms.wait('STMs')['elapsed'] == 1
        lms.wait('STMd')
        # Real LMS can serve every item from an identical player stream URL.
        # Reuse this listening endpoint and the exact strm request for item two.
        source.thread.join(1);assert not source.thread.is_alive()
        source.thread=threading.Thread(target=source.run,daemon=True);source.thread.start()
        lms.strm('s',source,rate=8000);lms.wait('RESP');lms.wait('DSCO')
        assert sum(p.get('event')=='STMc' for p in lms.packets)==2,lms.packets
        print('PASS queue same-URL successor: STMd authorizes a new producer with identical strm/HTTP bytes; no false duplicate suppression')
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

# Internal generation handoff discards the old producer without announcing a
# reconnect to LMS. Capabilities belong to the connection. Default fixtures above
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
            generation.write_text('2')
            end=time.monotonic()+3
            while 'MODE READY 2' not in Path(log.name).read_text() and time.monotonic()<end: time.sleep(.01)
            text=Path(log.name).read_text()
            assert 'MODE READY 2' in text,text
            assert text.count('FLUSH')==before+1,text
            assert text.rindex('FLUSH')<text.index('MODE READY 2'),text
            count=len([p for p in lms.packets if p.get('event') in ('STMs','STMd')])
            time.sleep(.1);lms.timer()
            assert len([p for p in lms.packets if p.get('event') in ('STMs','STMd')])==count
            assert sum(p['op']=='HELO' for p in lms.packets)==1,lms.packets
            continuation = HTTP(struct.pack('<hh',200,-200)*8000)
            try:
                lms.strm('s',continuation,rate=8000)
                lms.wait('STMs'); lms.wait('STMu')
                assert len([p for p in lms.packets if p.get('event') == 'STMs']) == 2
            finally: continuation.close()
            print('PASS mode generation core: producer/decoder/PCM reset before readiness, one connection HELO, no generation HELO, no late queue start/advance; default generation-zero fixtures unchanged')
        finally:
            proc.send_signal(signal.SIGTERM);proc.wait(3);lms.close();source.close()
            assert proc.returncode==0

# The common fake must reproduce LMS's same-connection HELO restart policy.
import socket
from types import SimpleNamespace
from fake_lms import SongQueue
lms=LMS()
peer=socket.create_connection(lms.listener.getsockname(),timeout=3)
hello=b'HELO'+struct.pack('!I',36)+bytes(36)
def command():
    def exact(n):
        data=b''
        while len(data)<n:
            part=peer.recv(n-len(data))
            assert part,'fake peer closed before reconnect response'
            data+=part
        return data
    n=struct.unpack('!H',exact(2))[0]
    return exact(n)[4:5]
try:
    peer.sendall(hello);lms.accept()
    songs=SongQueue(lms,[(SimpleNamespace(port=9,body=bytes(64)),dict(rate=8000)),(SimpleNamespace(port=9,body=b'x'*64),dict(rate=8000))])
    assert command()==b's'
    songs.react(dict(event='STMs'));songs.react(dict(event='STMd'))
    assert command()==b's' # source two is decoding ahead of audible item one
    peer.sendall(hello);lms.read()
    assert command()==b'q' and command()==b's'
    assert lms.reconnect_replays==1
    assert lms.reconnect_sources[0].body==bytes(64)
    songs.react(dict(event='STMd'))
    assert command()==b's'
    assert lms.reconnect_sources[1].body==b'x'*64
    for fresh in lms.reconnect_sources:
        with socket.create_connection(('127.0.0.1',fresh.port),timeout=3) as http:
            http.sendall(b'GET /audio HTTP/1.0\r\n\r\n')
            while http.recv(4096):pass
    print('PASS fake LMS reconnect oracle: repeated connected-player HELO resends q/current-audible s, not decoder-ahead item; cached successor replayed with fresh audio')
finally:
    peer.close();lms.close()
