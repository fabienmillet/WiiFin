#!/usr/bin/env python3
"""Picture/sound offset of a tour.sh run, from Dolphin's frame and audio dumps.

    ONLY=sync tools/test/server.sh
    MARKER=1 tools/test/tour.sh "<play Sync Test>" "" 60
    tools/test/measure_av.py

Sync Test flashes white with a 1 kHz beep every 2 s.  The marker (one white
frame and the Start sound together, 12 s in) gives the offset between the
two dumps; each flash is then paired with the nearest beep.  Positive: the
picture comes after the sound.
"""
import glob
import struct
import subprocess
from pathlib import Path

D = str(Path(__file__).resolve().parent / 'out' / 'dolphin' / 'Dump') + '/'
V=glob.glob(D+'Frames/*_1.avi')[0]; A=glob.glob(D+'Audio/*_dspdump1.wav')[0]
raw=subprocess.run(['ffmpeg','-loglevel','error','-i',V,'-fps_mode','passthrough','-vf','scale=32:24,format=gray','-f','rawvideo','-'],capture_output=True).stdout
pts=[float(x) for x in subprocess.run(['ffprobe','-v','error','-select_streams','v','-show_entries','frame=pts_time','-of','csv=p=0',V],capture_output=True,text=True).stdout.split()]
lum=[sum(raw[i*768:(i+1)*768])/768 for i in range(min(len(raw)//768,len(pts)))]
vev=[pts[i] for i in range(1,len(lum)) if lum[i]>200 and lum[i-1]<=200]
a=subprocess.run(['ffmpeg','-loglevel','error','-i',A,'-ac','1','-ar','48000','-f','s16le','-'],capture_output=True).stdout
smp=struct.unpack('<%dh'%(len(a)//2),a); W=96
env=[max(abs(x) for x in smp[i:i+W]) for i in range(0,len(smp)-W,W)]
aev=[]
for k in range(1,len(env)):
    t=k*W/48000
    if env[k]>2000 and env[k-1]<=2000 and (not aev or t-aev[-1]>0.5): aev.append(t)
# marker: video white onset near 12 s after boot = first video event; audio: Start sound right before
vm=vev[0]; am=max(t for t in aev if t < vm+0.5 and t > vm-1.0)
off=vm-am
pairs=[]
for v in vev[1:]:
    best=min(aev, key=lambda t: abs((v-off)-t))
    pairs.append((v-off)-best)
print('calibration offset %.3f s; picture minus sound (s):'%off, ' '.join('%+.3f'%p for p in pairs))
print('median picture - sound: %+.3f s'%sorted(pairs)[len(pairs)//2])
