# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Generated stereo ramps identify actual frames in LMS WAV/AIFF seek responses."""
import sys,os,pathlib,struct,http.client,json,subprocess,re,tempfile
sys.dont_write_bytecode=True
ROOT=pathlib.Path(__file__).resolve().parents[1]
from mp4_seek_test import Loopback,cli,MAC,quote
import numpy as np
work=tempfile.TemporaryDirectory(prefix='yeney-pcm-origin-',dir=os.environ.get('TMPDIR','/var/tmp'))
base=pathlib.Path(work.name)
server=Loopback(base)
def chunk(tag,data,big=False):return tag+struct.pack('>I' if big else '<I',len(data))+data+(b'\0' if len(data)&1 else b'')
for bits,rate in ([] if len(list((base/'library').glob('ramp-*'))) == 4 else [(16,44100),(24,96000)]):
 frames=rate*40; width=bits//8; i=np.arange(frames,dtype=np.uint32)
 v=np.column_stack((i & ((1<<bits)-1),i>>bits)).reshape(-1)
 for kind in ('wav','aiff'):
  cells=v.astype('<u4').view(np.uint8).reshape(-1,4)[:,:width]
  pcm=(cells if kind=='wav' else cells[:,::-1]).tobytes()
  if kind=='wav':
   content=b'WAVE'+chunk(b'fmt ',struct.pack('<HHIIHH',1,2,rate,rate*width*2,width*2,bits))+chunk(b'data',pcm)
   body=b'RIFF'+struct.pack('<I',len(content))+content
  else:
   exponent=rate.bit_length()-1
   extended=struct.pack('>HQ',16383+exponent,rate<<(63-exponent))
   content=b'AIFF'+chunk(b'COMM',struct.pack('>HIH',2,frames,bits)+extended,True)+chunk(b'SSND',bytes(8)+pcm,True)
   body=b'FORM'+struct.pack('>I',len(content))+content
  (base/f'library/ramp-{bits}-{rate}.{kind}').write_bytes(body)

def fetch():
 message=server.requests.get(timeout=25)
 request=message[24:].decode(); c=http.client.HTTPConnection('127.0.0.1',9000,timeout=30)
 c.request('GET',request.split()[1]);r=c.getresponse();headers=dict(r.getheaders());data=r.read();c.close()
 return message,headers,data

def payload(data):
 if data[:4]==b'RIFF':
  at=12
  while at+8<=len(data):
   n=struct.unpack_from('<I',data,at+4)[0]
   if data[at:at+4]==b'data':return data[at+8:],at+8,'RIFF'
   at+=8+n+(n&1)
 elif data[:4]==b'FORM':
  at=12
  while at+8<=len(data):
   n=struct.unpack_from('>I',data,at+4)[0]
   if data[at:at+4]==b'SSND':
    off=struct.unpack_from('>I',data,at+8)[0];return data[at+16+off:],at+16+off,'FORM'
   at+=8+n+(n&1)
 return data,0,'raw'
try:
 server.start()
 failures=[]
 with (base/'capture.jsonl').open('w') as out:
  for path in sorted((base/'library').iterdir()):
   bits,rate=map(int,path.stem.split('-')[1:]);width=bits//8
   cli(MAC+' playlist play '+quote('file://'+str(path),safe=''))
   for seconds in (None,.229443,5,30,39.25):
    if seconds is not None:cli(MAC+' time '+str(seconds))
    msg,headers,data=fetch();pcm,offset,shape=payload(data)
    endian='big' if (shape=='FORM' or (shape=='raw' and msg[6:7]==b'0')) else 'little'
    first=int.from_bytes(pcm[:width],endian)|(int.from_bytes(pcm[width:2*width],endian)<<bits)
    result=dict(file=path.name,seek=seconds,wire=msg[:24].hex(),pcm_fields=msg[2:7].decode('latin1'),headers=headers,body_prefix=data[:80].hex(),shape=shape,header_bytes=offset,body_bytes=len(data),first_frame=first)
    probe=json.loads(subprocess.check_output([os.environ.get('YENEY_PCM_ORIGIN_BINARY',str(ROOT/'pcm-origin-test'))]+[chr(v) for v in msg[3:7]],input=data))
    result['decoder_origin']=probe
    if seconds is None:
     track=re.search(r'\bid:(\d+)',cli(MAC+' status 0 1 tags:u')).group(1)
     c=http.client.HTTPConnection('127.0.0.1',9000,timeout=30)
     c.request('GET','/music/'+track+'/download');response=c.getresponse()
     download=response.read();download_headers=dict(response.getheaders());c.close()
     assert response.status==200 and download==path.read_bytes()
     original,_,_=payload(download)
     assert original==data
     proven=json.loads(subprocess.check_output([str(ROOT/'pcm-origin-test')]+[chr(v) for v in msg[3:7]]+['proven'],input=data))
     assert proven==dict(known=1,frame=0),proven
     result['immutable_download']=dict(status=response.status,headers=download_headers,prefix=download[:80].hex(),bytes=len(download),origin=proven)
    if probe['known']:failures.append(('unprovable response',path.name,seconds,probe))
    if probe['known'] and probe['frame']!=first:failures.append((path.name,seconds,first,probe))
    out.write(json.dumps(result)+'\n');out.flush();print(json.dumps(result),flush=True)
    status=b'STMs'+bytes(3)+struct.pack('>IIIIHIIIIIHIIH',1048576,0,0,len(data),65535,1,8192,4096,0,0,0,0,0,0)
    server.send('STAT',status)
   cli(MAC+' stop')
 print('C1 failures='+repr(failures),flush=True)
 assert not failures, failures
finally:
 server.close()
 work.cleanup()
