#!/usr/bin/env python3
import os, sys, time, subprocess, pathlib
FIFO="/tmp/motion_bus"; RING="/dev/shm/ring"
OUT=str(pathlib.Path.home()/ "tcc" / "clips"); pathlib.Path(OUT).mkdir(parents=True, exist_ok=True)
def segs(t0,t1):
  L=[]; 
  for s in range(int(t0),int(t1)+1):
    p=f"{RING}/{s}.ts"; 
    if os.path.exists(p): L.append(p)
  return L
def concat(paths,outmp4):
  with open("/tmp/files.txt","w") as f:
    for p in paths: f.write(f"file '{p}'\n")
  return subprocess.call(["ffmpeg","-y","-f","concat","-safe","0","-i","/tmp/files.txt","-c","copy","-movflags","+faststart",outmp4])
def now(): return int(time.time())
def main():
  on_ms=None
  while True:
    try:
      with open(FIFO,"r") as f:
        for line in f:
          parts=line.strip().split()
          if len(parts)<2: continue
          ev,ts=parts[0],parts[1]
          try: ts=int(ts)
          except: continue
          if ev=="MOTION_ON": on_ms=ts
          elif ev=="MOTION_OFF" and on_ms is not None:
            t0=max(0,(on_ms//1000)-5); t1=(ts//1000)+5
            while now()<t1+1: time.sleep(0.25)
            L=segs(t0,t1)
            if not L: print("[clip] nenhum segmento",file=sys.stderr,flush=True); on_ms=None; continue
            stamp=time.strftime("%Y%m%d-%H%M%S",time.localtime(t0))
            out=f"{OUT}/clip_{stamp}_{t0}-{t1}.mp4"
            rc=concat(L,out)
            print(("[clip] "+out if rc==0 else f"[clip][erro] rc={rc}"), flush=True)
            on_ms=None
    except FileNotFoundError:
      os.makedirs(os.path.dirname(FIFO),exist_ok=True); time.sleep(0.5)
    except Exception as e:
      print(f"[collector][ex] {e}",file=sys.stderr,flush=True); time.sleep(0.5)
if __name__=="__main__": main()
