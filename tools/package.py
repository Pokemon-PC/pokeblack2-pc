#!/usr/bin/env python3
"""Builds the player package: dist/PokemonNoire2-PC/ and a .zip.

    python tools/package.py [build dir, default build/release]

The release build (cmake -B build/release -DPB2_RELEASE=ON) must exist.
Contents: pokeblack2.exe (stripped, no console), config.ini (empty rom_dir),
LISEZMOI.txt. Never any game data: the script refuses to package a ROM, a
save or a log, and checks the exe's imports (system DLLs only).
"""
import datetime
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "release")
exe = os.path.join(build, "pokeblack2.exe")
if not os.path.isfile(exe):
    sys.exit(f"no {exe}: build the release first (cmake -B build/release -DPB2_RELEASE=ON)")

name = "PokemonNoire2-PC"
dist = os.path.join(ROOT, "dist")
out = os.path.join(dist, name)
shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)

shutil.copy2(exe, os.path.join(out, "pokeblack2.exe"))
shutil.copy2(os.path.join(ROOT, "tools", "package", "LISEZMOI.txt"), os.path.join(out, "LISEZMOI.txt"))
with open(os.path.join(out, "config.ini"), "w", newline="\r\n") as f:
    f.write("; Pokemon Black 2 (PC) - settings\n\n"
            "; Folder holding your Pokemon Black 2 ROM (English, USA/Europe). Empty: the\n"
            "; ROM is looked for next to this exe. The file is recognised by its header,\n"
            "; whatever its name. Example: rom_dir = D:\\Games\\roms\n"
            "rom_dir =\n")

# nothing from the game, ever
for dirpath, _, files in os.walk(out):
    for fn in files:
        low = fn.lower()
        if low.endswith((".nds", ".sav", ".log", ".bmp", ".wav", ".raw")):
            sys.exit(f"refused: {fn} must not be packaged")
        with open(os.path.join(dirpath, fn), "rb") as f:
            head = f.read(0x10)
        if head[:10] == b"POKEMON B2":
            sys.exit(f"refused: {fn} is a ROM")

# imports: system DLLs only (SDL2 is linked in)
try:
    dump = subprocess.run(["llvm-objdump", "-p", exe], capture_output=True, text=True).stdout
    dlls = sorted({l.split("DLL Name:")[1].strip() for l in dump.splitlines() if "DLL Name:" in l})
    allowed = ("KERNEL32", "USER32", "GDI32", "SHELL32", "SETUPAPI", "OLE32", "ADVAPI32", "IMM32", "VERSION",
               "OLEAUT32", "WINMM", "API-MS-WIN-CRT-")
    odd = [d for d in dlls if not d.upper().startswith(allowed)]
    if odd:
        sys.exit(f"refused: the exe needs non-system DLLs {odd}")
except FileNotFoundError:
    print("warning: llvm-objdump not found, imports not checked")

stamp = datetime.date.today().isoformat()
zpath = os.path.join(dist, f"{name}-{stamp}.zip")
with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for fn in sorted(os.listdir(out)):
        z.write(os.path.join(out, fn), f"{name}/{fn}")
size = os.path.getsize(zpath)
print(f"{zpath}: {size / 1048576:.1f} MiB")
for fn in sorted(os.listdir(out)):
    print(f"  {fn}  {os.path.getsize(os.path.join(out, fn)) / 1048576:.2f} MiB")
