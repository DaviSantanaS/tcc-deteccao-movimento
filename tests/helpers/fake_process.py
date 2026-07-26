import argparse
import pathlib
import signal
import sys
import time


parser = argparse.ArgumentParser()
parser.add_argument("--exit", type=int)
parser.add_argument("--ignore", action="store_true")
parser.add_argument("--out", default="")
parser.add_argument("--err", default="")
parser.add_argument("--ready-file", type=pathlib.Path)
arguments = parser.parse_args()

if arguments.ignore:
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

if arguments.out:
    print(arguments.out, flush=True)
if arguments.err:
    print(arguments.err, file=sys.stderr, flush=True)
if arguments.ready_file is not None:
    arguments.ready_file.touch()
if arguments.exit is not None:
    raise SystemExit(arguments.exit)

while True:
    time.sleep(0.05)
