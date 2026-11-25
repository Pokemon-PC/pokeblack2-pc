#!/usr/bin/env python3
"""Turns the host addresses of a log ([pb2:stack] lines) into game functions.

    python tools/symbolize.py build/pc/pokeblack2.exe run.log
"""
import re
import subprocess
import sys

exe, log = sys.argv[1], sys.argv[2]
lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
addrs = [m.group(1) for l in lines for m in [re.search(r"\[pb2:stack\] #\d+ (0x[0-9A-Fa-f]+)", l)] if m]
if not addrs:
    sys.exit("no [pb2:stack] lines")
out = subprocess.run(["llvm-addr2line", "-f", "-C", "-e", exe] + addrs, capture_output=True, text=True).stdout.split("\n")
names = out[0::2]
it = iter(names)
for l in lines:
    m = re.search(r"\[pb2:stack\] (#\d+) (0x[0-9A-Fa-f]+)", l)
    print(f"{l}  {next(it, '?')}" if m else l)
