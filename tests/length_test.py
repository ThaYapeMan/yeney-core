# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Synthetic length fixtures and read-only owner media selected by the caller."""
import math,os,re,struct,subprocess,tempfile,unittest
from pathlib import Path
from decoders_test import mp4, alac_parts
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
def decode(path,output,decimate=False,codec='l'):
    env=dict(os.environ,YENEY_TEST_NATIVE_RATE='1')
    if decimate:env['YENEY_TEST_DECIMATE']='1'
    else:env.pop('YENEY_TEST_DECIMATE',None)
    return subprocess.run([str(ROOT/'decoder-test'),codec,str(path),str(output)],env=env,capture_output=True,text=True)
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
    def test_alac_bounds(self):
        with tempfile.TemporaryDirectory(prefix='yeney-length-bounds-') as d:
            d=Path(d);path=d/'test.m4a';out=d/'out.raw'
            for rate in (44100,88200,96000):
                values=signal(9216);path.write_bytes(alac(values,rate,declared=12288))
                result=decode(path,out);self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(out.read_bytes(),raw(values)+bytes(3072*8));self.assertIn('source-short padded 3072 frames',result.stdout);self.assertIn('EXACT 12288',result.stdout)
                result=decode(path,out,True);self.assertEqual(result.returncode,0,result.stderr)
                factor=1 if rate==44100 else 2;self.assertIn('source-short padded '+str(3072//factor)+' frames',result.stdout)
                full=alac(signal(16384),rate);cookie,packets=alac_parts(full);path.write_bytes(full[:-sum(map(len,packets[-2:]))])
                result=decode(path,out);self.assertNotEqual(result.returncode,0);self.assertIn('source-short 8192 frames',result.stderr)
                values=signal(10386);path.write_bytes(alac(values,rate,declared=10000));result=decode(path,out)
                self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(out.read_bytes(),raw(values[:10000]));self.assertIn('source-long truncated 386',result.stdout)
            path.write_bytes(alac(signal(11384),44100,declared=16384,packet_size=8192))
            result=decode(path,out);self.assertNotEqual(result.returncode,0);self.assertIn('source-short 5000 frames',result.stderr)
        print('PASS ALAC EOF length policy: one-packet silence repair before decimation, exact published count, >packet and >100 ms faults, source-long truncation',flush=True)
    def test_flac_bounds(self):
        with tempfile.TemporaryDirectory(prefix='yeney-length-flac-') as d:
            d=Path(d)
            for rate in (44100,96000):
                values=signal(12000);body=b''.join(int(v).to_bytes(3,'little',signed=True) for pair in values for v in pair)
                wave=b'RIFF'+struct.pack('<I',len(body)+36)+b'WAVEfmt '+struct.pack('<IHHIIHH',16,1,2,rate,rate*6,6,24)+b'data'+struct.pack('<I',len(body))+body
                (d/'in.wav').write_bytes(wave)
                subprocess.run(['flac','-s','-f','-o',str(d/'original.flac'),str(d/'in.wav')],check=True)
                original=(d/'original.flac').read_bytes()
                for delta in (1000,-1000,5000,0):
                    data=bytearray(original);total=len(values)+delta
                    data[21]=(data[21]&240)|((total>>32)&15);data[22:26]=(total&0xffffffff).to_bytes(4,'big')
                    path=d/'test.flac';path.write_bytes(data);out=d/'out.raw'
                    result=decode(path,out,codec='f')
                    if delta==5000:
                        self.assertNotEqual(result.returncode,0);self.assertIn('source-short 5000 frames',result.stderr);continue
                    self.assertEqual(result.returncode,0,result.stderr);self.assertIn('EXACT '+str(total),result.stdout)
                    expected=raw(values[:total])+bytes(max(0,delta)*8);self.assertEqual(out.read_bytes(),expected)
                    if delta>0:self.assertIn('source-short padded 1000 frames',result.stdout)
                    if delta<0:self.assertIn('source-long truncated 1000 frames',result.stdout)
                    if delta==0:self.assertNotIn('source-short',result.stdout);self.assertNotIn('source-long',result.stdout)
                    converted=decode(path,out,True,codec='f');self.assertEqual(converted.returncode,0,converted.stderr)
                    factor=1 if rate==44100 else 2;self.assertIn('FRAMES '+str((total+factor-1)//factor),converted.stdout)
                # A stream client keeps the original strict/default PCM path.
                data=bytearray(original);total=len(values)+1000;data[21]=(data[21]&240)|((total>>32)&15);data[22:26]=(total&0xffffffff).to_bytes(4,'big');path.write_bytes(data)
                result=subprocess.run([str(ROOT/'decoder-test'),'f',str(path),str(out)],env=dict(os.environ,YENEY_TEST_STRICT='1',YENEY_TEST_NATIVE_RATE='1'),capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(out.read_bytes(),raw(values));self.assertNotIn('padded',result.stdout)
        print('PASS FLAC EOF length policy: STREAMINFO vs actual output, block-sized bounded silence, large-short fault, long-source truncation, exact native/decimated counts; strict stream PCM unchanged',flush=True)
    def test_strict_edit_precedence(self):
        with tempfile.TemporaryDirectory(prefix='yeney-length-strict-') as d:
            d=Path(d);values=signal(10386)
            data=alac(values,44100,trim=(117,10036),smpb=(0,0,len(values)))
            pos=data.index(b' 00000000');data=data[:pos+1]+b'ZZZZZZZZ'+data[pos+9:]
            path=d/'test.m4a';path.write_bytes(data);out=d/'out.raw'
            result=subprocess.run([str(ROOT/'decoder-test'),'l',str(path),str(out)],env=dict(os.environ,YENEY_TEST_STRICT='1'),capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(out.read_bytes(),raw(values[117:10153]))
        print('PASS strict stream M4A edit precedence unchanged: ignored malformed gapless metadata cannot override an edit',flush=True)
    def test_private(self):
        configured=os.environ.get('YENEY_LENGTH_FIXTURE_DIR')
        if not configured:
            print('SKIP owner-file length check: YENEY_LENGTH_FIXTURE_DIR empty/unset (disabled or no default directory exists); synthetic fixtures only',flush=True);return
        directory=Path(configured)
        self.assertTrue(directory.is_dir(), 'YENEY_LENGTH_FIXTURE_DIR is not an existing directory')
        files=sorted(p for p in directory.glob('*') if p.is_file() and p.suffix.lower() in ('.m4a','.flac'))
        if not files:
            self.fail('YENEY_LENGTH_FIXTURE_DIR contains no M4A/FLAC files')
        for path in files:
            for converted in (False,True):
                if path.suffix.lower()=='.m4a':result=decode(path,Path('/dev/null'),converted)
                else:
                    env=dict(os.environ,YENEY_TEST_NATIVE_RATE='1')
                    if converted:env['YENEY_TEST_DECIMATE']='1'
                    result=subprocess.run([str(ROOT/'decoder-test'),'f',str(path),'/dev/null'],env=env,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,(path.name,result.returncode))
                exact=int(re.search(r'^EXACT (\d+)$',result.stdout,re.M)[1]);frames=int(re.search(r'^FRAMES (\d+)$',result.stdout,re.M)[1]);self.assertEqual(exact,frames,path.name)
                self.assertNotIn('source-short padded',result.stdout,path.name)
                print(f'PASS owner-file length check: {path.name} {"output-rate" if converted else "native"} derived={exact} decoded={frames}; output discarded to /dev/null, no copies/excerpts',flush=True)
if __name__=='__main__':unittest.main(verbosity=2)
