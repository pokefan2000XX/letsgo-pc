#!/usr/bin/env python3
"""Who references an address in main: code (ADRP + ADD/LDR/STR pairs, GOT loads) and data
(pointers made by relocations: vtables, tables of names and callbacks).

  python -P tools/refs.py build                      index main (about a minute)
  python -P tools/refs.py 0x7100f01632 [...]         references to these addresses
  python -P tools/refs.py --str PokePartyGetParam    the C string with this text, then its references
  python -P tools/refs.py --range 0x7101600000 0x7101600100   references into a range

The index is orig/analysis/main_refs.tsv (target, from, function, kind). Kinds:
  adr  address taken (ADRP + ADD)     ld/st  load/store at the address (ADRP + LDR/STR)
  got  address loaded from the GOT     ptr  a pointer in data (RELATIVE or ABS64 relocation)
"""
import argparse
import bisect
import collections
import csv
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nso2elf  # noqa: E402

BASE = 0x7100000000
INDEX = os.path.join(ROOT, "orig", "analysis", "main_refs.tsv")


def load_funcs():
    return sorted(int(r["Address"], 16) for r in csv.DictReader(open(os.path.join(ROOT, "data", "main_functions.csv"))))


def load_names():
    """Names for addresses: decomp/symbols.txt, then main's exports."""
    names = {}
    p = os.path.join(ROOT, "decomp", "symbols.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            parts = line.split("#", 1)[0].split(None, 1)
            if len(parts) == 2:
                names.setdefault(int(parts[0], 16), parts[1].strip())
    for r in csv.DictReader(open(os.path.join(ROOT, "data", "main_functions.csv"))):
        if r["Name"]:
            names.setdefault(int(r["Address"], 16), r["Name"])
    return names


def build():
    nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", "main"))
    info = nso2elf.analyze(nso)
    img = nso["image"]
    t0, tsz = nso["text"]
    funcs = load_funcs()
    got = {r["offset"]: r["addend"] for r in info["relocs"] if r["type"] == 1027}
    rows = []
    for r in info["relocs"]:
        if r["type"] == 1027:                       # RELATIVE
            rows.append((BASE + r["addend"], BASE + r["offset"], 0, "ptr"))
        elif r["type"] == 257 and r["sym"]:         # ABS64 to a symbol defined in main
            s = info["symbols"][r["sym"]]
            if s["shndx"]:
                rows.append((BASE + s["value"] + r["addend"], BASE + r["offset"], 0, "ptr"))
    words = struct.unpack_from("<%dI" % (tsz // 4), img, t0)
    for i, w in enumerate(words):
        if (w & 0x9F000000) != 0x90000000:          # ADRP
            continue
        rd = w & 0x1F
        imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        pc = t0 + i * 4
        page = (pc & ~0xFFF) + (imm << 12)
        for j in range(i + 1, min(i + 16, len(words))):
            v = words[j]
            if (v & 0xFF800000) == 0x91000000 and ((v >> 5) & 0x1F) == rd:        # ADD Xd, Xn, #imm
                rows.append((BASE + page + (((v >> 10) & 0xFFF) << (12 if v & (1 << 22) else 0)), BASE + t0 + j * 4, 0, "adr"))
            elif (v & 0x3B000000) == 0x39000000 and ((v >> 5) & 0x1F) == rd:      # LDR/STR (unsigned offset)
                size, vec, opc = v >> 30, (v >> 26) & 1, (v >> 22) & 3
                scale = 4 if vec and opc >= 2 else size
                tgt = page + (((v >> 10) & 0xFFF) << scale)
                if not vec and size == 3 and opc == 1 and tgt in got:            # LDR Xt from a GOT slot
                    rows.append((BASE + got[tgt], BASE + t0 + j * 4, 0, "got"))
                else:
                    rows.append((BASE + tgt, BASE + t0 + j * 4, 0, "st" if opc == 0 else "ld"))
            else:
                if (v & 0x1F) == rd and (v & 0x9F000000) != 0x90000000:
                    pass                    # keep looking: compilers interleave
                continue
            break
    out = []
    for tgt, frm, _f, kind in rows:
        fi = bisect.bisect_right(funcs, frm) - 1 if kind != "ptr" else -1
        out.append((tgt, frm, funcs[fi] if fi >= 0 else 0, kind))
    out.sort()
    os.makedirs(os.path.dirname(INDEX), exist_ok=True)
    with open(INDEX, "w", newline="\n") as f:
        for tgt, frm, fn, kind in out:
            f.write("%x\t%x\t%x\t%s\n" % (tgt, frm, fn, kind))
    print("%d references (%s) -> %s" % (len(out), ", ".join("%s %d" % kv for kv in collections.Counter(
        r[3] for r in out).most_common()), os.path.relpath(INDEX, ROOT)))


def load_index():
    if not os.path.exists(INDEX):
        raise SystemExit("no index: run python -P tools/refs.py build")
    tg, rows = [], []
    for line in open(INDEX):
        t, frm, fn, kind = line.split()
        tg.append(int(t, 16))
        rows.append((int(frm, 16), int(fn, 16), kind))
    return tg, rows


def find_string(text):
    nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", "main"))
    img = nso["image"]
    hits, start = [], 0
    needle = b"\0" + text.encode("utf-8") + b"\0"
    while True:
        i = img.find(needle, start)
        if i < 0:
            return hits
        hits.append(BASE + i + 1)
        start = i + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addrs", nargs="*")
    ap.add_argument("--str", action="append", default=[])
    ap.add_argument("--range", nargs=2)
    a = ap.parse_args()
    if a.addrs == ["build"]:
        build()
        return
    tg, rows = load_index()
    names = load_names()

    def show(lo, hi, label):
        i = bisect.bisect_left(tg, lo)
        n = 0
        print("%s:" % label)
        while i < len(tg) and tg[i] < hi:
            frm, fn, kind = rows[i]
            where = ("data 0x%x" % frm) if kind == "ptr" else (
                "0x%x in %s" % (frm, names.get(fn, "FUN_%x" % fn)))
            extra = "" if hi - lo == 1 else "  -> 0x%x" % tg[i]
            print("  %-4s %s%s" % (kind, where, extra))
            i += 1
            n += 1
        if not n:
            print("  (no references)")

    for s in a.str:
        hits = find_string(s)
        if not hits:
            print("%r: not found" % s)
        for h in hits:
            show(h, h + 1, "%r at 0x%x" % (s, h))
    for x in a.addrs:
        v = int(x, 16)
        show(v, v + 1, "0x%x %s" % (v, names.get(v, "")))
    if a.range:
        lo, hi = int(a.range[0], 16), int(a.range[1], 16)
        show(lo, hi, "0x%x-0x%x" % (lo, hi))


if __name__ == "__main__":
    main()
