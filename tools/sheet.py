#!/usr/bin/env python3
"""Puts PNG screenshots (256x384, as tools/ctrl.py writes them) side by side.

    python tools/sheet.py out.png a.png b.png ...
"""
import struct
import sys
import zlib


def read_png(p):
    d = open(p, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(d):
        n, t = struct.unpack_from(">I4s", d, pos)
        b = d[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h = struct.unpack_from(">II", b)
        elif t == b"IDAT":
            idat += b
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(w * 3)
    for y in range(h):
        f, line = raw[y * (w * 3 + 1)], bytearray(raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)])
        for i in range(len(line)):
            a = line[i - 3] if i >= 3 else 0
            b, c = prev[i], prev[i - 3] if i >= 3 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows


out, files = sys.argv[1], sys.argv[2:]
imgs = [read_png(f) for f in files]
h = max(i[1] for i in imgs)
raw = bytearray()
for y in range(h):
    raw.append(0)
    for w, ih, rows in imgs:
        raw += rows[y] if y < ih else bytes(w * 3)


def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))


W = sum(i[0] for i in imgs)
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, h, 8, 2, 0, 0, 0))
                      + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
