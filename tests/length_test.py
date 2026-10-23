# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Independent synthetic M4A length fixtures; private media decoded only to /dev/null."""
import math,os,re,struct,subprocess,tempfile,unittest
from pathlib import Path
from decoders_test import mp4
ROOT=Path(__file__).resolve().parents[1]
def alac(values,rate,*,declared=None,trim=None,smpb=None,zero_tail=False,packet_size=4096,movie_scale=None,media_scale=None):
    packets=[]
    for at in range(0,len(values),packet_size):
        block=values[at:at+packet_size]
        payload=b''.join(int(v).to_bytes(3,'big',signed=True) for frame in block for v in frame)
        header=int('001'+'0000'+'0'*12+'1001'+f'{len(block):032b}',2)
        size=55+len(payload)*8+3;padding=(-size)%8
        bits=((header<<(len(payload)*8+3))|(int.from_bytes(payload,'big')<<3)|7)<<padding
        packets.append(bits.to_bytes((size+padding)//8,'big'))
    if zero_tail:
        header=int('001'+'0000'+'0'*12+'1001'+'0'*32,2)
        packets.append((((header<<3)|7)<<6).to_bytes(8,'big'))
    cookie=struct.pack('!I',packet_size)+bytes([0,24,40,10,14,2])+bytes(10)+struct.pack('!I',rate)
    return mp4(cookie,packets,len(values) if declared is None else declared,trim=trim,smpb=smpb,complete=True,movie_scale=movie_scale,media_scale=media_scale)
def signal(frames,start=0):return [((i*1237)%1000000-500000,(i*4217)%1000000-500000) for i in range(start,start+frames)]
def raw(values):return b''.join(struct.pack('<ii',l*256,r*256) for l,r in values)
def decode(path,output,decimate=False):
    env=dict(os.environ,YENEY_TEST_NATIVE_RATE='1')
    if decimate:env['YENEY_TEST_DECIMATE']='1'
    else:env.pop('YENEY_TEST_DECIMATE',None)
    return subprocess.run([str(ROOT/'decoder-test'),'l',str(path),str(output)],env=env,capture_output=True,text=True)
class Lengths(unittest.TestCase):
    def test_structures(self):
        with tempfile.TemporaryDirectory(prefix='yeney-length-') as d:
            d=Path(d)
            for rate in (44100,88200,96000):
                values=signal(10386)
                cases=[('short-stts',{},0,len(values)),
                       ('padding-already-excluded',dict(smpb=(0,1902,len(values))),0,len(values)),
                       ('padded-stts',dict(declared=12288,smpb=(0,1902,len(values))),0,len(values)),
                       ('edit',dict(trim=(117,10036)),117,10153),
                       ('gapless',dict(smpb=(113,219,10054)),113,10167),
                       ('coarse-edit-gapless',dict(trim=(113,10054),smpb=(113,219,10054),movie_scale=1000),113,10167),
                       ('media-timescale',dict(trim=(117,10036),media_scale=rate*2),117,10153),
                       ('cookie-frameLength',dict(packet_size=2048),0,len(values))]
                for name,options,begin,end in cases:
                    path=d/'test.m4a';path.write_bytes(alac(values,rate,**options));out=d/'out.raw'
                    result=decode(path,out);self.assertEqual(result.returncode,0,(rate,name,result.stdout,result.stderr))
                    self.assertIn('EXACT '+str(end-begin),result.stdout);self.assertEqual(out.read_bytes(),raw(values[begin:end]),(rate,name))
                values=signal(8192);path.write_bytes(alac(values,rate,declared=12288,smpb=(0,4096,8192),zero_tail=True))
                result=decode(path,out);self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(out.read_bytes(),raw(values));self.assertIn('EXACT 8192',result.stdout)
        print('PASS queue M4A lengths: 44.1/88.2/96 kHz, elst media_time/duration/scales, iTunSMPB original/priming/padding, cookie frameLength, short/zero final packets; exact PCM counts',flush=True)
    def test_private(self):
        directory=Path('/mnt/c/HueSyncLXC/private-fixtures')
        files=sorted(p for p in directory.glob('*') if p.suffix.lower() in ('.m4a','.flac')) if directory.is_dir() else []
        if not files:
            print('SKIP owner-file length check: private-fixtures folder absent or contains no M4A/FLAC files',flush=True);return
        for path in files:
            for converted in (False,True):
                if path.suffix.lower()=='.m4a':result=decode(path,Path('/dev/null'),converted)
                else:
                    env=dict(os.environ,YENEY_TEST_NATIVE_RATE='1')
                    if converted:env['YENEY_TEST_DECIMATE']='1'
                    result=subprocess.run([str(ROOT/'decoder-test'),'f',str(path),'/dev/null'],env=env,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,(str(path),result.stdout,result.stderr))
                exact=int(re.search(r'^EXACT (\d+)$',result.stdout,re.M)[1]);frames=int(re.search(r'^FRAMES (\d+)$',result.stdout,re.M)[1]);self.assertEqual(exact,frames,str(path))
                self.assertNotIn('source-short padded',result.stdout,str(path))
                print(f'PASS owner-file length check: {path} {"output-rate" if converted else "native"} derived={exact} decoded={frames}; output discarded to /dev/null, no copies/excerpts',flush=True)
if __name__=='__main__':unittest.main(verbosity=2)
