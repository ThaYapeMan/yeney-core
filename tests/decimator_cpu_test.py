# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import os,pathlib,subprocess,tempfile,threading,time,json,hashlib
import numpy as np
root=pathlib.Path(__file__).resolve().parents[1];fixture=os.environ.get('YENEY_BENCH_FIXTURE',str(root/'decimator-fixture'))
with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR','/var/tmp'),prefix='fir-cpu-verdict-') as directory:
 d=pathlib.Path(directory);signal=(np.sin(2*np.pi*1000*np.arange(192000*8)/192000)*.8*2**31).astype('<i4');body=np.column_stack((signal,signal)).tobytes();(d/'regular').write_bytes(body)
 regular=subprocess.check_output([fixture,'192000',str(d/'regular'),str(d/'reference')],text=True)
 os.mkfifo(d/'slow-input')
 def writer():
  with (d/'slow-input').open('wb',buffering=0) as stream:
   for at in range(0,len(body),32768):stream.write(body[at:at+32768]);time.sleep(.002)
 thread=threading.Thread(target=writer,daemon=True);thread.start()
 line=subprocess.check_output([fixture,'192000',str(d/'slow-input'),str(d/'slow-output')],text=True);thread.join()
 fields=dict(v.split('=') for v in line.split());wall=float(fields['speed'].rstrip('x'));cpu=float(fields.get('cpu_speed','0x').rstrip('x'))
 same=(d/'reference').read_bytes()==(d/'slow-output').read_bytes()
 print('C5 regular '+regular.strip());print('C5 slow-input '+line.strip());print(f'C5 PCM_identical={same} wall_below_20={wall<20} CPU_verdict={cpu>=20}',flush=True)
 assert same
 assert wall<20,wall
 assert 'cpu_speed' in fields,'conversion CPU time is missing; wall-clock scheduling/I/O delay controls the verdict'
 assert cpu>=20,cpu
 print('PASS C5 conversion CPU clock excludes I/O stalls; unchanged 20x threshold; PCM identical',flush=True)
