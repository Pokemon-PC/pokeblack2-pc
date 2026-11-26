#!/usr/bin/env python3
"""Drives a game started with PB2_CTRL=<dir> (see src/video/screen.c).

    python tools/ctrl.py <dir> "m A 600 40" "k D 60" "s here"

Writes the commands, waits until the game is idle again, converts each
screenshot taken to <dir>/<name>.png and prints the frame reached; input
commands are appended to <dir>/history.txt (to replay a session). Exits 1
if the game process is gone (crash): look at its log.
"""
import os
import struct
import sys
import time
import zlib


def bmp_to_png(src, dst):
    d = open(src, "rb").read()
    off, w, h = struct.unpack_from("<I", d, 10)[0], *struct.unpack_from("<ii", d, 18)
    stride = (w * 3 + 3) & ~3
    raw = bytearray()
    for y in range(h):
        row = d[off + (h - 1 - y) * stride:][:w * 3]
        raw.append(0)
        for x in range(w):
            b, g, r = row[x * 3:x * 3 + 3]
            raw += bytes((r, g, b))

    def chunk(t, b):
        c = t + b
        return struct.pack(">I", len(b)) + c + struct.pack(">I", zlib.crc32(c))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


def main():
    d, cmds = sys.argv[1], sys.argv[2:]
    status = os.path.join(d, "status.txt")
    if os.path.exists(status):
        os.remove(status)
    with open(os.path.join(d, "history.txt"), "a") as f:    # replayable: cat history.txt
        f.write("".join(c + "\n" for c in cmds if c[:1] in "kmt"))
    tmp = os.path.join(d, "cmd.tmp")
    with open(tmp, "w") as f:
        f.write("\n".join(cmds) + "\n")
    os.replace(tmp, os.path.join(d, "cmd.txt"))
    shots = [c.split()[1] for c in cmds if c.startswith("s ")]
    pid = os.path.join(d, "pid.txt")
    t0 = time.time()
    while not os.path.exists(status):
        time.sleep(0.1)
        if os.path.exists(pid) and time.time() - t0 > 2:
            import subprocess
            p = open(pid).read().strip()
            out = subprocess.run(["tasklist", "/FI", f"PID eq {p}"], capture_output=True, text=True).stdout
            if p not in out:
                print("game process is gone")
                sys.exit(1)
            t0 = time.time()
    time.sleep(0.05)
    for s in shots:
        b = os.path.join(d, s + ".bmp")
        if os.path.exists(b):
            bmp_to_png(b, os.path.join(d, s + ".png"))
            os.remove(b)
    print(open(status).read().strip())


if __name__ == "__main__":
    main()
