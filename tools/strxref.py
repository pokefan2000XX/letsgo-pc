#!/usr/bin/env python3
"""Index which function references which C string (ADRP + ADD/LDR pairs into
.rodata). Assert messages, format strings and the source paths left in the
binary are the main naming aid for decomp work.

Writes (game text, so under orig/ which is never committed):
  orig/analysis/<mod>_string_xrefs.tsv   function, instruction, string address, string
  orig/analysis/<mod>_source_files.tsv   source path, function count, functions

Usage: python -P tools/strxref.py orig/exefs/main --module main --base 0x7100000000
"""
import argparse
import bisect
import collections
import csv
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso2elf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH_RE = re.compile(r"[\w.-]+\.(cpp|cc|c|h|hpp|inl)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("--module", required=True)
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x7100000000)
    a = ap.parse_args()
    B = a.base
    nso = nso2elf.load_nso(a.nso)
    img = nso["image"]
    t0, tsz = nso["text"]
    r0, rsz = nso["ro"]
    d0, dsz = nso["data"]

    funcs = sorted(int(r["Address"], 16) - B for r in csv.DictReader(
        open(os.path.join(ROOT, "data", "%s_functions.csv" % a.module))))

    def func_of(x):
        i = bisect.bisect_right(funcs, x) - 1
        return funcs[i] if i >= 0 else None

    def cstring(addr):
        if not (r0 <= addr < r0 + rsz or d0 <= addr < d0 + dsz):
            return None
        end = img.find(b"\0", addr, addr + 4096)
        if end < 0 or end - addr < 4:
            return None
        s = img[addr:end]
        if any(c < 0x20 and c not in (9, 10, 13) or c > 0x7E for c in s):
            try:
                txt = s.decode("utf-8")
            except UnicodeDecodeError:
                return None
            return txt if txt.isprintable() else None
        return s.decode("ascii")

    # GOT slots filled by RELATIVE relocations: slot -> target
    got = {r["offset"]: r["addend"] for r in nso2elf.analyze(nso)["relocs"] if r["type"] == 1027}
    words = struct.unpack_from("<%dI" % (tsz // 4), img, t0)
    xrefs = []
    for i, w in enumerate(words):
        if (w & 0x9F000000) != 0x90000000:   # ADRP
            continue
        rd = w & 0x1F
        imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        pc = t0 + i * 4
        page = (pc & ~0xFFF) + (imm << 12)
        for j in range(i + 1, min(i + 12, len(words))):
            v = words[j]
            if (v & 0xFF800000) == 0x91000000 and ((v >> 5) & 0x1F) == rd:      # ADD Xd, Xn, #imm
                tgt = page + (((v >> 10) & 0xFFF) << (12 if v & (1 << 22) else 0))
            elif (v & 0xFFC00000) == 0xF9400000 and ((v >> 5) & 0x1F) == rd:    # LDR Xt, [Xn, #imm] (GOT)
                tgt = got.get(page + ((v >> 10) & 0xFFF) * 8)
                if tgt is None:
                    break
            else:
                continue
            s = cstring(tgt)
            if s is not None:
                xrefs.append((func_of(pc), t0 + j * 4, tgt, s))
            break

    out_dir = os.path.join(ROOT, "orig", "analysis")
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "%s_string_xrefs.tsv" % a.module), "w", encoding="utf-8") as fp:
        for f, ins, tgt, s in xrefs:
            fp.write("0x%x\t0x%x\t0x%x\t%s\n" % (B + f, B + ins, B + tgt, s.replace("\t", "\\t").replace("\n", "\\n")))
    by_path = collections.defaultdict(set)
    for f, ins, tgt, s in xrefs:
        if PATH_RE.search(s.strip()):
            by_path[s.strip().replace("\\", "/")].add(f)
    with open(os.path.join(out_dir, "%s_source_files.tsv" % a.module), "w", encoding="utf-8") as fp:
        for p in sorted(by_path):
            fp.write("%s\t%d\t%s\n" % (p, len(by_path[p]), " ".join("0x%x" % (B + f) for f in sorted(by_path[p]))))
    funcs_with_str = len({x[0] for x in xrefs})
    print("string references: %d in %d functions (%.1f%% of %d)" %
          (len(xrefs), funcs_with_str, 100 * funcs_with_str / len(funcs), len(funcs)))
    print("source paths referenced: %d, attributed functions: %d" %
          (len(by_path), len(set().union(*by_path.values())) if by_path else 0))
    libs = collections.Counter()
    for p, fs in by_path.items():
        q = re.sub(r"^.*?/program/+", "", p)
        key = "/".join([x for x in q.split("/") if x not in ("..", ".")][:2])
        libs[key] += len(fs)
    for k, v in libs.most_common(25):
        print("  %5d  %s" % (v, k))


if __name__ == "__main__":
    sys.exit(main())
