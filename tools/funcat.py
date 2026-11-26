#!/usr/bin/env python3
"""Names the main-binary function containing each game address.

    python tools/funcat.py 0203A225 0207A925 ...
"""
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(ROOT, "..", "pokeblack2", "asm", "main.s")
starts = []
for line in open(src, encoding="utf-8", errors="replace"):
    m = re.match(r"^(\w+): ; 0x([0-9A-Fa-f]{8})", line)
    if m:
        starts.append((int(m.group(2), 16), m.group(1)))
starts.sort()
keys = [s[0] for s in starts]
for a in sys.argv[1:]:
    v = int(a, 16) & ~1
    i = bisect.bisect_right(keys, v) - 1
    print(f"{a}: {starts[i][1]}+0x{v - starts[i][0]:X}" if i >= 0 else f"{a}: ?")
