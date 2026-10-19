"""Makes the phase 1 test hack's files for the game (docs/PLAN.md, roadmap phase 1, step 4) from your
own copy of the game, all recolored blue: the rugs in the player's bedroom, which are inside the
bedroom's gfpak (bin/archive/field/t001r01.gfpak), and the hero's cap, a file of its own. They go in
hacks/phase1-test/romfs/, which stays on your PC (it's made from the game).

  python -P hacks/phase1-test/make_romfs.py
  build/pc/letsgo.exe --mods hacks/phase1-test

The game only sees the rugs if the runtime puts them into the archive (runtime/src/mods.cpp). Needs
orig/romfs (tools/prepare.sh) and Python's lz4.
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_mods  # noqa: E402  (gfpak reading)

ARCHIVE = "bin/archive/field/t001r01.gfpak"
IN_ARCHIVE = ["bin/field/model/t001r01/carpet%02d_t001r01.bntx" % i for i in (1, 2, 3)]
FILES = ["bin/chara/data/tr/tr0001_00_hero/mdl/tr0001_00_cap_col_00_00.bntx"]
# One block of each texture format, every texel pure blue: BC1 (color 0, 565), and BC7 mode 6 with both
# endpoints (0, 0, 255, 255) (7-bit blue and alpha, p-bits set) and every index 0
BLUE = {
    0x1A06: struct.pack("<HHI", 0x001F, 0x0000, 0),
    0x2006: ((1 << 6) | (127 << 35) | (127 << 42) | (127 << 49) | (127 << 56) | (1 << 63) | (1 << 64)).to_bytes(16, "little"),
}


def recolor(bntx):
    """A one-texture BNTX, blue: its image data (in the BRTD block) all one block."""
    d = bytearray(bntx)
    info, data = d.find(b"BRTI"), d.find(b"BRTD")
    fmt, = struct.unpack_from("<I", d, info + 0x1C)
    size, = struct.unpack_from("<I", d, info + 0x50)
    if d[:4] != b"BNTX" or d.count(b"BRTI") != 1 or fmt not in BLUE or size % len(BLUE[fmt]):
        raise SystemExit("make_romfs: not a one-texture BC1 or BC7 file, which this hack expects")
    start = data + 0x10
    d[start:start + size] = BLUE[fmt] * (size // len(BLUE[fmt]))
    return bytes(d)


def write(path, data):
    out = os.path.join(ROOT, "hacks", "phase1-test", "romfs", *path.split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(data)
    print("wrote", os.path.relpath(out, ROOT))


def main():
    romfs = os.path.join(ROOT, "orig", "romfs")
    entries, _ = check_mods.parse(open(os.path.join(romfs, *ARCHIVE.split("/")), "rb").read())
    by_path = {e["path"]: e for e in entries}
    for path in IN_ARCHIVE:
        e = by_path.get(check_mods.fnv(path.encode()))
        if e is None:
            raise SystemExit("make_romfs: %s isn't in %s" % (path, ARCHIVE))
        write(path, recolor(check_mods.content(e)))
    for path in FILES:
        write(path, recolor(open(os.path.join(romfs, *path.split("/")), "rb").read()))


if __name__ == "__main__":
    main()
