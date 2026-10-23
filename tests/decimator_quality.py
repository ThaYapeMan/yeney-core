#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Own FIR versus external SoX -v; generated PCM is temporary, never committed."""
import argparse, json, math, os, re, subprocess, tempfile
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--sox',required=True);p.add_argument('--report',type=Path);a=p.parse_args()
rows=[];details=[];failures=[]
def db(v):return max(-300.,20*math.log10(max(float(v),1e-15)))
def fit(x,rate,freq):
    n=np.arange(len(x))/rate
    design=np.column_stack((np.sin(2*np.pi*freq*n),np.cos(2*np.pi*freq*n),np.ones(len(n))))
    weights=np.linalg.lstsq(design,x,rcond=None)[0]
    tone=design[:,:2]@weights[:2]
    return np.hypot(*weights[:2]),db(np.sqrt(np.mean((x-design@weights)**2))/np.sqrt(np.mean(tone**2)))
def quant24(x):return np.clip(np.where(x>=0,np.floor(x*2**23+.5),np.ceil(x*2**23-.5)),-2**23,2**23-1)/2**23
with tempfile.TemporaryDirectory(prefix='yeney-decimator-quality-') as directory:
    d=Path(directory)
    for source in (88200,96000,176400,192000):
        target=44100 if source%44100==0 else 48000;factor=source//target
        time=np.arange(source*2)/source
        rng=np.random.default_rng(20261023)
        signals={f'{freq}Hz':10**(-1/20)*np.sin(2*np.pi*freq*time) for freq in (1000,19000,25000,30000)}
        duration=len(time)/source;k=math.log(40000/10)/duration
        signals['sweep']=.5*np.sin(2*np.pi*10*np.expm1(k*time)/k)
        signals['noise']=np.clip(rng.normal(0,.08,len(time)),-.9,.9)
        signals['zero']=np.zeros(len(time))
        # A full-scale step demonstrates unavoidable low-pass Gibbs overshoot.
        signals['intersample']=np.where(np.sin(2*np.pi*1000*time)>=0,1.,-1.)
        signals['impulse']=np.zeros(len(time));signals['impulse'][source]=.5
        results={'YeneY':{},'SoX -v':{}};floating={'YeneY':{},'SoX -v':{}};clipcounts={};soxclips={};speed=0
        for name,signal in signals.items():
            pcm=(quant24(signal)*2**31).astype('<i4');np.column_stack((pcm,pcm)).tofile(d/'input.raw')
            command=[str(ROOT/'decimator-fixture'),str(source),str(d/'input.raw'),str(d/'yeney.raw')]
            line=subprocess.check_output(command,text=True).strip();fields=dict(v.split('=') for v in line.split())
            speed=max(speed,float(fields['speed'].rstrip('x')))
            clipcounts[name]=int(fields['clips'])
            ours=np.fromfile(d/'yeney.raw',dtype='<i4').reshape(-1,2)[:,0]/2**31
            sox_run=subprocess.run([a.sox,'-D','-t','raw','-r',str(source),'-c','2','-b','32','-e','signed-integer','-L',str(d/'input.raw'),
                            '-t','raw','-r',str(target),'-c','2','-b','64','-e','floating-point','-L',str(d/'sox.raw'),'rate','-v',str(target)],
                           check=True,capture_output=True)
            warnings=re.findall(r'clipped (\d+) samples',sox_run.stderr.decode())
            soxclips[name]=sum(map(int,warnings))//2
            sox=np.fromfile(d/'sox.raw',dtype='<f8').reshape(-1,2)[:,0].astype(float)
            assert len(ours)==math.ceil(len(signal)/factor) and len(sox)==len(ours)
            results['YeneY'][name]=quant24(ours);results['SoX -v'][name]=quant24(sox)
            floating['YeneY'][name]=ours;floating['SoX -v'][name]=sox
            if name in ('noise','sweep'):
                details.append(f'{source} {name}: output RMS YeneY {db(np.sqrt(np.mean(ours**2))):.3f}, SoX {db(np.sqrt(np.mean(sox**2))):.3f} dBFS')
        h=np.fromfile(d/'yeney.raw.taps',dtype='<f8');response=np.abs(np.fft.rfft(h,2**20));freq=np.fft.rfftfreq(2**20,1/source)
        ripple=float(np.max(np.abs(20*np.log10(response[freq<=20000]))));stop=db(np.max(response[freq>=target/2]))
        assert ripple<=.01 and stop<=-120,(source,ripple,stop)
        details.append(f'{source}: {len(h)} taps, delay {(len(h)-1)/2/source*1000:.6f} ms; unquantised FIR ripple {ripple:.8f} dB, stopband maximum {stop:.2f} dB, observed speed up to {speed:.2f}x')
        for method,values in results.items():
            trim=slice(target//4,-target//4)
            thdn=[fit(values[f'{f}Hz'][trim],target,f)[1] for f in (1000,19000)]
            alias=[db(fit(values[f'{f}Hz'][trim],target,abs(f-round(f/target)*target))[0]) for f in (25000,30000)]
            impulse=values['impulse'];resp=np.abs(np.fft.rfft(impulse,2**18))*factor/.5
            bins=np.fft.rfftfreq(2**18,1/target)
            measured_ripple=float(np.max(np.abs(20*np.log10(np.maximum(resp[bins<=20000],1e-30)))))
            floor=db(np.sqrt(np.mean((values['noise'][trim]-floating[method]['noise'][trim])**2)))
            clips=clipcounts['intersample'] if method=='YeneY' else soxclips['intersample']
            rows.append([source,target,method,measured_ripple,*alias,*thdn,floor,clips])
            if max(alias)>-120 or measured_ripple>.01:failures.append((source,method,'response'))
        own,ref=rows[-2],rows[-1]
        for column in (6,7):
            if own[column]>ref[column]+1:failures.append((source,'THD+N',column,own[column],ref[column]))
        if any(clipcounts[n] for n in signals if n!='intersample'):failures.append((source,'normal signal clipped',clipcounts))
    # Use a longer 192 kHz signal, median of five independent one-thread runs.
    signal=(np.sin(2*np.pi*1000*np.arange(192000*8)/192000)*.8*2**31).astype('<i4')
    np.column_stack((signal,signal)).tofile(d/'speed.raw')
    speeds=[]
    for _ in range(5):
        text=subprocess.check_output([str(ROOT/'decimator-fixture'),'192000',str(d/'speed.raw'),str(d/'speed-out.raw')],text=True)
        speeds.append(float(dict(v.split('=') for v in text.split())['speed'].rstrip('x')))
    median=float(np.median(speeds))
    if median<20:failures.append(('speed',median))
report='# Queue hi-res decimation: measured quality\n\n'
report+='Original symmetric Kaiser-windowed sinc, beta 18.5; unity DC gain. The cutoff is halfway between 20 kHz and output Nyquist. Only the retained phase is evaluated, with symmetric tap pairs and four independent double accumulators. No SoX/libsoxr source is incorporated.\n\n'
report+='| Source Hz | Target Hz | Converter | Ripple to 20k (±dB) | Alias 25k dBFS | Alias 30k dBFS | THD+N 1k dB | THD+N 19k dB | 24-bit quantisation noise dBFS | Full-scale step clipped frames |\n|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|\n'
for row in rows:report+='| '+' | '.join(f'{v:.6f}' if isinstance(v,float) else str(v) for v in row)+' |\n'
report+='\n'+ '\n'.join('- '+v for v in details)+'\n'
report+=f'\n192→48 kHz stereo, one FIR thread, median of five 8 s runs including raw I/O: **{median:.2f}× real time** ({speeds}).\n'
report+='\nSignals are generated as 24-bit PCM; tones are −1 dBFS. SoX uses `-D ... rate -v` with floating output, followed by the same deterministic 24-bit rounding for comparison. Tone fitting excludes 250 ms at either edge; no edge ringing is counted as harmonic distortion. Alias levels are fitted amplitudes; −300 dBFS denotes no measurable product. Noise floor is the white-noise requantisation residual; undithered digital silence stays exactly zero. The full-scale square/step deliberately exhibits intersample/Gibbs overshoot: YeneY guard frames and SoX diagnostic clipped samples/2 are reported, never hidden; normal tones/noise/sweep must not clip. Sweep and white noise are also converted and their RMS is reported above. Generated audio is temporary and is not committed.\n'
report+=f'\nVerdict: {"PASS" if not failures else "FAIL "+repr(failures)}\n'
if a.report:a.report.write_text(report)
print(report)
if failures:raise SystemExit(1)
