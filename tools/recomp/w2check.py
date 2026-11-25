#!/usr/bin/env python3
"""Checks the White 2 addresses the translator uses: every numeric literal
(.word 0x...) inside a function must be found in the White 2 binary at the
address computed for it.

    python tools/recomp/w2check.py <White 2 ROM> ../pokeblack2 build/w2addr.json
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp  # noqa: E402
import w2addr  # noqa: E402

rom, repo, amap = sys.argv[1:4]
W = w2addr.Rom(rom)
recomp.ADDR_MAP = json.load(open(amap))
recomp.DEFINES.clear()
recomp.DEFINES.add("WHITE2")
asm = os.path.join(repo, "asm")
ok = bad = 0
worst = []
for n in sorted(os.listdir(asm)):
    if not n.endswith(".s"):
        continue
    fid = n[:-2]
    base, img = W.module(fid)
    if img is None:
        continue
    fbad = 0
    for f in recomp.parse_file(os.path.join(asm, n)):
        addr = None
        for it in f["items"]:
            if it[0] == "label":
                addr = it[2]
            elif it[0] == "insn":
                addr = it[1].addr + it[1].size if it[1].addr is not None else None
            elif it[0] == "data":
                m = re.match(r"^\.word\s+(0x[0-9A-Fa-f]+)\s*$", it[1])
                if m and addr is not None:
                    o = addr - base
                    if 0 <= o and o + 4 <= len(img):
                        if struct.unpack_from("<I", img, o)[0] == int(m.group(1), 16) & 0xFFFFFFFF:
                            ok += 1
                        else:
                            bad += 1
                            fbad += 1
                if it[1].startswith((".balign", ".align")) and addr is not None:
                    k = int(it[1].split()[1].split(",")[0], 0)
                    k = k if it[1].startswith(".balign") else 1 << k
                    addr = (addr + k - 1) & ~(k - 1)
                elif addr is not None:
                    addr += recomp.directive_size(it[1], addr)
    if fbad:
        worst.append((fbad, fid))
print(f"literals found at their White 2 address: {ok}, not found: {bad}")
for c, fid in sorted(worst, reverse=True)[:15]:
    print(f"  {fid}: {c}")
