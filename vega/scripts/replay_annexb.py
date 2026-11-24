# replay_annexb.py file.h264 [fps] [tv-ip] : send access units to TV :7100 in "SCRV" framing at real-time pace
import socket,sys,time,struct,re
d=open(sys.argv[1],'rb').read(); fps=float(sys.argv[2]) if len(sys.argv)>2 else 30
starts=[];saw=False
for m in re.finditer(b'\x00\x00\x01',d):
    i=m.start(); sc=i-1 if i>0 and d[i-1]==0 else i; t=d[i+3]&31
    first=t in(1,5) and d[i+4]&0x80
    if not starts or (t in(7,9) or first) and saw: starts.append(sc); saw=False
    if t in(1,5): saw=True
starts.append(len(d)); print('aus',len(starts)-1,flush=True)
s=socket.create_connection((sys.argv[3] if len(sys.argv)>3 else "192.168.0.57",7100)); s.sendall(b'SCRV'); t0=time.time()
for k in range(len(starts)-1):
    pts=int(k*1e6/fps); dl=t0+pts/1e6-time.time()
    if dl>0: time.sleep(dl)
    au=d[starts[k]:starts[k+1]]; s.sendall(struct.pack('>Iq',len(au),pts)+au)
time.sleep(3); s.close(); print('done',flush=True)
