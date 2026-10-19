#!/usr/bin/env python3
"""Disassemble functions of main with names and targets filled in, for decomp work.

  python -P tools/disasm.py 0x7100197770 [...]       whole functions (any address inside one)
  python -P tools/disasm.py 0x7100197770 -n 20        only the first 20 instructions

Calls show the callee's name (decomp/symbols.txt, main's exports, imports) or FUN_<address>.
ADRP pairs show their target: a string's text, a known name or DAT_<address>. The output is
derived from the game: read it in the terminal, never commit or publish it.
"""
import argparse
import bisect
import csv
import os
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nso2elf  # noqa: E402
from refs import load_names  # noqa: E402

BASE = 0x7100000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addrs", nargs="+")
    ap.add_argument("-n", type=int, default=0, help="at most this many instructions per function")
    a = ap.parse_args()
    nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", "main"))
    info = nso2elf.analyze(nso)
    img = nso["image"]
    r0, rsz = nso["ro"]
    d0, dsz = nso["data"]
    rows = [(int(r["Address"], 16), int(r["Size"])) for r in csv.DictReader(open(os.path.join(ROOT, "data", "main_functions.csv")))]
    rows.sort()
    starts = [r[0] for r in rows]
    names = load_names()
    for off, imp in info["plt"].items():
        names.setdefault(BASE + off, imp)
    got = {r["offset"]: r["addend"] for r in info["relocs"] if r["type"] == 1027}
    got_sym = {r["offset"]: info["symbols"][r["sym"]]["name"] for r in info["relocs"] if r["type"] in (1025, 257) and r["sym"]}

    def name(v):
        return names.get(v) or ("FUN_%x" % v if v in names or bisect.bisect_left(starts, v) < len(starts)
                                and starts[bisect.bisect_left(starts, v)] == v else "DAT_%x" % v)

    def describe(v):
        o = v - BASE
        if r0 <= o < r0 + rsz or d0 <= o < d0 + dsz:
            end = img.find(b"\0", o, o + 200)
            s = img[o:end] if end > o else b""
            if len(s) >= 3 and all(0x20 <= c < 0x7F for c in s):
                return '"%s"' % s.decode("ascii")
        return name(v)

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    for x in a.addrs:
        v = int(x, 16)
        i = bisect.bisect_right(starts, v) - 1
        f0, fsz = rows[i]
        print("%s (0x%x, %d bytes)" % (name(f0), f0, fsz))
        code = bytes(img[f0 - BASE:f0 - BASE + fsz])
        pages = {}
        for n, insn in enumerate(md.disasm(code, f0)):
            if a.n and n >= a.n:
                break
            addr, mn, ops = insn.address, insn.mnemonic, insn.op_str
            note = ""
            w = struct.unpack_from("<I", code, addr - f0)[0]
            if mn == "adrp":
                imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
                if imm & (1 << 20):
                    imm -= 1 << 21
                pages[w & 0x1F] = (addr & ~0xFFF) + (imm << 12)
            elif mn in ("bl", "b") and ops.startswith("#"):
                t = int(ops[1:], 16)
                if not f0 <= t < f0 + fsz:
                    note = name(t)
            elif (w & 0xFF800000) == 0x91000000 and ((w >> 5) & 0x1F) in pages:      # ADD after ADRP
                t = pages[(w >> 5) & 0x1F] + (((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0))
                note = describe(t)
            elif (w & 0x3B000000) == 0x39000000 and ((w >> 5) & 0x1F) in pages:      # LDR/STR after ADRP
                size, vec, opc = w >> 30, (w >> 26) & 1, (w >> 22) & 3
                t = pages[(w >> 5) & 0x1F] + (((w >> 10) & 0xFFF) << (4 if vec and opc >= 2 else size))
                if t - BASE in got_sym:
                    note = "&" + got_sym[t - BASE]
                elif t - BASE in got:
                    note = "&" + describe(BASE + got[t - BASE])
                else:
                    note = ("store " if opc == 0 else "") + name(t)
            if mn != "adrp":                 # a register an ADRP set is overwritten
                for r in insn.regs_access()[1]:
                    rn = insn.reg_name(r)
                    if rn[0] in "xw" and rn[1:].isdigit():
                        pages.pop(int(rn[1:]), None)
            print("  %x  %-8s %-40s%s" % (addr, mn, ops, ("; " + note) if note else ""))
        print()


if __name__ == "__main__":
    main()
