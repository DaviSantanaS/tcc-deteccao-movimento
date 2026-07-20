import argparse,signal,sys,time
p=argparse.ArgumentParser();p.add_argument("--exit",type=int);p.add_argument("--ignore",action="store_true");p.add_argument("--out",default="");p.add_argument("--err",default="");a=p.parse_args()
if a.ignore:signal.signal(signal.SIGINT,signal.SIG_IGN);signal.signal(signal.SIGTERM,signal.SIG_IGN)
if a.out:print(a.out,flush=True)
if a.err:print(a.err,file=sys.stderr,flush=True)
if a.exit is not None:raise SystemExit(a.exit)
while True:time.sleep(.05)
