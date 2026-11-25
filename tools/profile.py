#!/usr/bin/env python3
"""Sums the samples of PB2_PROFILE=file per function.

    python tools/profile.py build/pc/pokeblack2.exe file [top]
"""
import collections
import subprocess
import sys

exe, path = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
rows = [l.split() for l in open(path) if l.strip()]
addrs = [r[0] for r in rows]
out = subprocess.run(["llvm-addr2line", "-f", "-C", "-e", exe], input="\n".join(addrs) + "\n",
                     capture_output=True, text=True).stdout.split("\n")
names = out[0::2][:len(addrs)]
per = collections.Counter()
for (a, n), name in zip(rows, names):
    per[name] += int(n)
total = sum(per.values())
print(f"{total} samples")
for name, n in per.most_common(top):
    print(f"{100.0 * n / total:6.2f}%  {n:7d}  {name}")

# python tools/profile.py exe file top FUNCTION : samples of that function per source line
if len(sys.argv) > 4:
    want = sys.argv[4]
    sel = [(a, int(n)) for (a, n), name in zip(rows, names) if name == want]
    out = subprocess.run(["llvm-addr2line", "-e", exe], input="\n".join(a for a, _ in sel) + "\n",
                         capture_output=True, text=True).stdout.split("\n")
    per_line = collections.Counter()
    for (a, n), where in zip(sel, out):
        per_line[where.split("\\")[-1].split("/")[-1]] += n
    print(f"--- {want} by line")
    for where, n in per_line.most_common(25):
        print(f"{n:7d}  {where}")
