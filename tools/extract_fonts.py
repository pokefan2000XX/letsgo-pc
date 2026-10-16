#!/usr/bin/env python3
"""Extract the Switch system shared fonts (BFTTF -> TTF) from installed firmware.

The game loads them through nn::pl (shared fonts). They come from the user's own
firmware, here the copy Ryujinx installed: one NCA per system title, titles
0100000000000810-814. Output: <out>/<type>.ttf, decrypted, as nn::pl hands
them to the game.

Usage: python -P tools/extract_fonts.py --keys prod.keys --nca-dir <...>/bis/system/Contents/registered --out orig/fonts
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nxextract  # noqa: E402

# nn::pl::SharedFontType -> (title id, file)
FONTS = {
    0: (0x0100000000000811, "nintendo_udsg-r_std_003.bfttf"),          # Standard
    1: (0x0100000000000814, "nintendo_udsg-r_org_zh-cn_003.bfttf"),    # ChineseSimple
    2: (0x0100000000000814, "nintendo_udsg-r_ext_zh-cn_003.bfttf"),    # ChineseSimpleExtension
    3: (0x0100000000000813, "nintendo_udjxh-db_zh-tw_003.bfttf"),      # ChineseTraditional
    4: (0x0100000000000812, "nintendo_udsg-r_ko_003.bfttf"),           # Korean
    5: (0x0100000000000810, "nintendo_ext_003.bfttf"),                 # NintendoExtension
}
KEY = 0x06186249


class OneFile:
    """Looks enough like an Nsp for nxextract.Nca: a file object and no tickets."""
    def __init__(self, path):
        self.f = open(path, "rb")
        self.title_keys = {}


def decrypt_bfttf(data):
    magic, size = struct.unpack_from("<II", data, 0)
    if magic ^ KEY != 0x18029A7F:
        raise ValueError("not a BFTTF (magic 0x%08x)" % magic)
    # the size word is stored big-endian
    size = int.from_bytes((size ^ KEY).to_bytes(4, "little"), "big")
    if size > len(data) - 8:
        raise ValueError("bad BFTTF size %d (file %d)" % (size, len(data)))
    n = (size + 3) // 4
    words = struct.unpack_from("<%dI" % n, data + bytes(4), 8)
    return struct.pack("<%dI" % n, *(w ^ KEY for w in words))[:size]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", required=True)
    ap.add_argument("--nca-dir", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    keys = nxextract.load_keys(a.keys)
    os.makedirs(a.out, exist_ok=True)
    wanted = {tid for tid, _ in FONTS.values()}
    files = {}
    for entry in sorted(os.listdir(a.nca_dir)):
        path = os.path.join(a.nca_dir, entry)
        if os.path.isdir(path):
            path = os.path.join(path, "00")
        if not os.path.isfile(path):
            continue
        src = OneFile(path)
        try:
            nca = nxextract.Nca(src, entry, 0, os.path.getsize(path), keys)
        except Exception:
            continue
        if nca.title_id not in wanted or nca.content_type != "Data" or 0 not in nca.sections:
            continue
        sec = nca.sections[0]
        master, levels = nxextract.ivfc_levels(sec.fsh)
        l6 = levels[5][0]
        for name, off, size in nxextract.romfs_listing(lambda o, n: sec.read(l6 + o, n)):
            files[(nca.title_id, os.path.basename(name))] = sec.read(l6 + off, size)
    ok = 0
    for t, (tid, fname) in sorted(FONTS.items()):
        blob = files.get((tid, fname))
        if blob is None:
            print("type %d: %s not found in title %016x" % (t, fname, tid))
            continue
        ttf = decrypt_bfttf(blob)
        open(os.path.join(a.out, "%d.ttf" % t), "wb").write(ttf)
        print("type %d: %s -> %d.ttf (%d bytes, sfnt %s)" % (t, fname, t, len(ttf), ttf[:4].hex()))
        ok += 1
    return 0 if ok == len(FONTS) else 1


if __name__ == "__main__":
    sys.exit(main())
