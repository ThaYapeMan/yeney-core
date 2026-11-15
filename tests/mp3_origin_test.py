# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Generated LAME gapless metadata is trusted only with proven full-file origin."""
import hashlib,json,os,pathlib,struct,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
work=tempfile.TemporaryDirectory(prefix='yeney-mp3-origin-',dir=os.environ.get('TMPDIR','/var/tmp'))
base=pathlib.Path(os.environ.get('YENEY_MP3_ORIGIN_WORK',work.name));base.mkdir(parents=True,exist_ok=True)
valid=base/'gapless.mp3';plain=base/'no-xing.mp3'
for path,xing in [(valid,'1'),(plain,'0')]:
 if not path.exists():subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=997:sample_rate=44100:duration=0.4','-ac','2','-c:a','libmp3lame','-b:a','192k','-write_xing',xing,str(path)],check=True)
data=valid.read_bytes();mark=max(data.find(b'Info'),data.find(b'Xing'));assert mark>=0
flags=struct.unpack_from('>I',data,mark+4)[0];pos=mark+8;count=0
for flag,length in [(1,4),(2,4),(4,100),(8,4)]:
 if flags&flag:
  if flag==1:count=struct.unpack_from('>I',data,pos)[0]
  pos+=length
assert data[pos:pos+4] in (b'LAME',b'Lavc')
delay=data[pos+21]*16+(data[pos+22]>>4);padding=((data[pos+22]&15)<<8)+data[pos+23]
expected=count*1152-delay-padding
no_lame=bytearray(data);no_lame[pos:pos+9]=bytes(9)
start=0
if data[:3]==b'ID3':start=10+sum(data[6+i]<<(7*(3-i)) for i in range(4))
# Remove whole MPEG-1 layer-III frames to model an actual seek suffix.
for _ in range(3):
 header=data[start:start+4];assert header[0]==255 and header[1]&224==224
 bitrate=[0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0][header[2]>>4]*1000
 rate=[44100,48000,32000][(header[2]>>2)&3]
 start+=144*bitrate//rate+((header[2]>>1)&1)
cases=[('lame-from-start',data,'from-start',True,expected),('seek-with-original-header',data,'seek',False,None),('seek-suffix',data[start:],'seek',False,None),('endless',data*4,'endless',False,None),('no-lame-info',bytes(no_lame),'from-start',False,None),('no-xing',plain.read_bytes(),'from-start',False,None)]
records={};failures=[]
baseline=json.loads(pathlib.Path(os.environ['YENEY_MP3_ORIGIN_BASELINE']).read_text()) if os.environ.get('YENEY_MP3_ORIGIN_BASELINE') else {}
for label,body,mode,known,exact in cases:
 output=subprocess.check_output([os.environ.get('YENEY_MP3_ORIGIN_BINARY',str(ROOT/'mp3-origin-test')),mode],input=body)
 values=list(struct.unpack_from('<QQQQQ',output));pcm=output[40:]
 if mode=='endless':pcm=pcm[:4096*8];values[2]=4096
 digest=hashlib.sha256(pcm).hexdigest()
 actual=dict(known=values[0],origin=values[1],frames=values[2],exact=None if values[3]==2**64-1 else values[3],error=values[4],pcm_sha256=digest)
 records[label]=actual
 assert not actual['error'] and len(pcm)==actual['frames']*8,(label,actual)
 if actual['known']!=known or (known and actual['origin']!=0) or actual['exact']!=exact:failures.append((label,actual,known,exact))
 if label in baseline:assert baseline[label]['pcm_sha256']==digest,(label,'decoded audio changed')
 print('C4 '+label+' '+json.dumps(actual)+(' PCM=identical-to-pre-item' if label in baseline else ''),flush=True)
if os.environ.get('YENEY_MP3_ORIGIN_REPORT'):pathlib.Path(os.environ['YENEY_MP3_ORIGIN_REPORT']).write_text(json.dumps(records,indent=2))
assert not failures,failures
print(f'PASS C4 LAME delay={delay} padding={padding} content_frames={expected}; seek/no-header/endless unknown',flush=True)
work.cleanup()
