#!/usr/bin/env python3
"""Where each function of the disassembly really is in White 2.

The pokeblack2 disassembly names its labels after their Black 2 addresses;
its "#if defined(WHITE2)" blocks change the size of a few functions, so in
White 2 the code after them is shifted. This tool measures the shifts on the
binaries: for each module (main ARM9, each overlay) it decompresses the Black 2
and White 2 images, and finds every function of Black 2 (its first bytes) in
White 2 near its Black 2 address.

    python tools/recomp/w2addr.py <Black 2 ROM> <White 2 ROM> ../pokeblack2 build/w2addr.json

Output: {"main": {"FUN_02018968": 33654224, ...}, "overlay_162": {...}} with
the White 2 address of every function whose address differs. recomp.py
--version white2 --addr-map build/w2addr.json starts each function there and
counts the rest of it from its instructions.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp  # noqa: E402


def r32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def blz(buf, out_len):
    """Nintendo backward LZ, in place at the end of buf (as rom.c)"""
    length = len(buf)
    buf = bytearray(buf) + bytearray(max(0, out_len - length))
    top, inc = r32(buf, length - 8), r32(buf, length - 4)
    enc_len, hdr_len = top & 0xFFFFFF, top >> 24
    src, src_end, dst = length - hdr_len, length - enc_len, out_len
    while src > src_end:
        src -= 1
        flags = buf[src]
        for _ in range(8):
            if src <= src_end:
                break
            if flags & 0x80:
                src -= 1
                hi = buf[src]
                src -= 1
                lo = buf[src]
                disp, n = (((hi & 0x0F) << 8) | lo) + 3, (hi >> 4) + 3
                for _ in range(n):
                    dst -= 1
                    buf[dst] = buf[dst + disp]
            else:
                src -= 1
                dst -= 1
                buf[dst] = buf[src]
            flags = (flags << 1) & 0xFF
    return bytes(buf[:out_len])


class Rom:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        a9off, _, self.a9ram, a9size = struct.unpack_from("<IIII", d, 0x20)
        a9 = d[a9off:a9off + a9size]
        # module params end with the nitrocode 21 06 C0 DE DE C0 06 21, after
        # {..., compressed_static_end, sdk_version}
        k = a9.find(bytes.fromhex("2106c0dedec00621"))
        comp_end = r32(a9, k - 8) if k >= 8 else 0
        self.bss = (r32(a9, k - 16), r32(a9, k - 12)) if k >= 16 else (0, 0)   # static bss start, end
        if comp_end:
            end = comp_end - self.a9ram
            inc = r32(a9, end - 4)
            self.main = blz(a9[:end], end + inc) + a9[end:]
        else:
            self.main = a9
        ovt, ovsz = struct.unpack_from("<II", d, 0x50)
        fat = r32(d, 0x48)
        self.ov = {}
        for i in range(ovsz // 32):
            oid, ram, size, bss, _, _, fid, flags = struct.unpack_from("<8I", d, ovt + 32 * i)
            s, e = r32(d, fat + 8 * fid), r32(d, fat + 8 * fid + 4)
            img = d[s:e]
            if flags & 0x01000000:
                img = blz(img[:flags & 0xFFFFFF], size)
            self.ov[oid] = (ram, img)

    def module(self, fid):
        if fid == "main":
            return self.a9ram, self.main
        m = re.match(r"overlay_(\d+)$", fid)
        return self.ov[int(m.group(1))] if m else (None, None)


def main():
    b2, w2, repo, out = sys.argv[1:5]
    B, W = Rom(b2), Rom(w2)
    asm = os.path.join(repo, "asm")
    result, report = {}, []
    for n in sorted(os.listdir(asm)):
        if not n.endswith(".s"):
            continue
        fid = n[:-2]
        bbase, bimg = B.module(fid)
        wbase, wimg = W.module(fid)
        if bimg is None:
            continue                     # itcm, dtcm, ltdmain: no version differences
        text = open(os.path.join(asm, n), encoding="utf-8", errors="replace").read()
        recomp.DEFINES.clear()
        recomp.DEFINES.add("BLACK2")
        funcs = [f for f in recomp.parse_file(os.path.join(asm, n)) if f["addr"] is not None]
        addrs, delta, missing = {}, 0, []
        search = True                    # also same-size modules: another language shifts code inside
        for f in funcs:
            a = f["addr"] - bbase
            found = 0 if not search else None
            if found is None and 0 <= a < len(bimg):
                # instruction bytes only: up to the first data (literal pool)
                ln = 0
                for it in f["items"]:
                    if it[0] == "data" and not it[1].startswith((".balign", ".align")):
                        break
                    if it[0] == "insn":
                        ln += it[1].size
                    if ln >= 32:
                        break
                ln = min(ln, 32, len(bimg) - a)
                key = bimg[a:a + ln]
                # relative branches (bl/blx/b) change when their target moved: masked
                care = [True] * ln
                if f["thumb"]:
                    for o in range(0, ln - 3, 2):
                        h1, h2 = key[o] | key[o + 1] << 8, key[o + 2] | key[o + 3] << 8
                        if (h1 & 0xF800) == 0xF000 and (h2 & 0xE800) == 0xE800:
                            care[o:o + 4] = [False] * 4
                else:
                    for o in range(0, ln - 3, 4):
                        if (r32(key, o) & 0x0E000000) == 0x0A000000:
                            care[o:o + 4] = [False] * 4
                idx = [i for i in range(ln) if care[i]]
                if len(idx) >= 2:
                    for d in sorted(range(-0x100, 0x101, 2), key=lambda x: (abs(x - delta), abs(x))):
                        o = a + d
                        if 0 <= o and o + ln <= len(wimg) and all(wimg[o + i] == key[i] for i in idx):
                            found = d
                            break
            if found is None:
                missing.append(f["name"])
                found = delta             # its own code changed: it starts where the previous one ended
            delta = found
            addrs[f["name"]] = wbase + a + found
        result[fid] = addrs
        moved = sum(1 for f in funcs if addrs.get(f["name"], f["addr"]) != f["addr"])
        report.append(f"{fid}: base {bbase:08X} -> {wbase:08X}, {len(funcs)} functions, {moved} moved, "
                      f"{len(missing)} inferred {missing[:4]}")
    result["_bss"] = [B.bss[0], B.bss[1], W.bss[0] - B.bss[0]]
    report.append(f"static bss {B.bss[0]:08X}-{B.bss[1]:08X} -> {W.bss[0]:08X}-{W.bss[1]:08X}")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    json.dump(result, open(out, "w"), indent=0)
    print("\n".join(r for r in report if " 0 moved" not in r or "inferred []" not in r))


if __name__ == "__main__":
    main()
