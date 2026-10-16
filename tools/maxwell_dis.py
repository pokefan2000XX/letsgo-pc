"""Maxwell (SM 5.x) disassembler for NVN shader code blobs, driven by the opcode
table and field layouts tools/maxwell_ref.py recovers from Ryujinx
(orig/ryujinx_ref/insttable.txt, instfields.txt).

NVN code blob: 0x30-byte header (magic 0x12345678), the 0x50-byte shader program
header (SPH) at 0x30, instructions from 0x80 in groups of four 64-bit words (one
scheduling word, then three instructions).

  python -P tools/maxwell_dis.py <blob> [<blob>...]       disassemble
  python -P tools/maxwell_dis.py --census <dir>             opcode histogram over *.code
"""
import collections
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "orig", "ryujinx_ref")


def load_table():
    pats = []
    for line in open(os.path.join(REF, "insttable.txt")):
        p = line.split()
        if len(p) != 3 or len(p[0]) != 64:
            continue
        mask = int("".join("0" if c == "x" else "1" for c in p[0]), 2)
        val = int(p[0].replace("x", "0"), 2)
        pats.append((bin(mask).count("1"), mask, val, p[1], p[2]))
    pats.sort(reverse=True)               # most specific first
    return pats


def load_fields():
    """{struct: [(field, getter)]}; layouts are printed as name[mask] (bits 0..),
    name[shift, mask], name[bitmask, 0] (flag) or name[s1, m1, s2, m2] (two pieces)"""
    out = {}
    for line in open(os.path.join(REF, "instfields.txt")):
        name, _, rest = line.strip().partition(" ")
        fields = []
        for item in rest.replace(", ", ",").split():
            fname, _, args = item.partition("[")
            if not args.rstrip("]"):
                continue
            a = [int(x) for x in args.rstrip("]").split(",")]
            if fname == "Imm32":
                g = (lambda s: lambda w: w >> s)(a[0])
            elif len(a) == 1:
                g = (lambda m: lambda w: w & m)(a[0])
            elif len(a) == 2 and a[1] == 0:
                g = (lambda m: lambda w: int(w & m != 0))(a[0] & 0xFFFFFFFF if a[0] < 0 else a[0])
            elif len(a) == 2:
                g = (lambda s, m: lambda w: (w >> s) & m)(a[0], a[1])
            elif len(a) == 4:
                g = (lambda s1, m1, s2, m2: lambda w: ((w >> s1) & m1) | ((w >> s2) & m2))(*a)
            else:
                continue
            fields.append((fname, g))
        out[name] = fields
    return out


TABLE = load_table()
FIELDS = load_fields()
ALIAS = {"TexsF16": "Texs", "TldsF16": "Tlds", "Tld4sF16": "Tld4s"}


def decode(w):
    """(name, emitter) of the instruction word"""
    for _, mask, val, name, emitter in TABLE:
        if w & mask == val:
            return name, emitter
    return "???", "???"


def fields(emitter, w):
    return {f: g(w) for f, g in FIELDS.get("Inst" + ALIAS.get(emitter, emitter), [])}


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def reg(r):
    return "RZ" if r == 255 else "R%d" % r


def pred(p, inv=0):
    return ("!" if inv else "") + ("PT" if p == 7 else "P%d" % p)


FLOAT_OPS = {"Fadd", "Fadd32i", "Fmul", "Fmul32i", "Ffma", "Ffma32i", "Fset", "Fsetp", "Fmnmx", "Fswzadd"}


def operands(name, emitter, f, off):
    """readable operand text for the common forms; anything left over is printed as name=value"""
    used = {"Pred", "PredInv"}
    ops = []
    for k in ("Dest", "DestPred", "DestPredInv"):
        if k in f:
            used.add(k)
            ops.append(reg(f[k]) if k == "Dest" else pred(f[k]))
    if "SrcA" in f:
        used.add("SrcA")
        ops.append(("-" if f.get("NegA") else "") + ("|%s|" if f.get("AbsA") else "%s") % reg(f["SrcA"]))
        used |= {"NegA", "AbsA"}
    if "CbufSlot" in f:
        used |= {"CbufSlot", "CbufOffset"}
        scale = 1 if name == "Ldc" else 4
        ops.append("%sc[%d][0x%x]" % ("-" if f.get("NegB") else "", f["CbufSlot"], f["CbufOffset"] * scale))
        used.add("NegB")
    if "SrcB" in f:
        used |= {"SrcB", "NegB", "AbsB"}
        ops.append(("-" if f.get("NegB") else "") + ("|%s|" if f.get("AbsB") else "%s") % reg(f["SrcB"]))
    if "Imm20" in f:
        used.add("Imm20")
        if name in FLOAT_OPS:
            ops.append("%g" % f32(f["Imm20"] << 12))
        else:
            ops.append("0x%x" % (sext(f["Imm20"], 20) & 0xFFFFFFFF))
    if "Imm32" in f:
        used.add("Imm32")
        v = f["Imm32"] & 0xFFFFFFFF
        ops.append("%g" % f32(v) if name in FLOAT_OPS else "0x%x" % v)
    if "Imm16" in f:
        used.add("Imm16")
        ops.append("0x%x" % f["Imm16"])
    if "SrcC" in f:
        used |= {"SrcC", "NegC"}
        ops.append(("-" if f.get("NegC") else "") + reg(f["SrcC"]))
    if "SrcPred" in f:
        used |= {"SrcPred", "SrcPredInv"}
        ops.append(pred(f["SrcPred"], f.get("SrcPredInv")))
    if "Imm24" in f:
        used.add("Imm24")
        if name in ("Bra", "Ssy", "Pbk", "Cal", "Brk", "Cont"):
            ops.append("-> %04x" % (off + 8 + sext(f["Imm24"], 24)))
        else:
            ops.append("%+d" % sext(f["Imm24"], 24))
    if "Imm10" in f:
        used.add("Imm10")
        ops.append("a[0x%x]" % f["Imm10"])
    if "Imm11" in f:
        used.add("Imm11")
        ops.append("a[0x%x]" % f["Imm11"])
    rest = []
    for k, v in f.items():
        if k in used or k == "Phys":
            continue
        if k in ("Ccc",) and v == 15:
            continue
        if v:
            rest.append(k if v == 1 and k[0].isupper() and k not in ("FComp", "IComp", "MufuOp", "Bop", "LogicOp", "Lop", "IpaOp", "AlSize", "LsSize", "Target", "WMask", "TidB") else "%s=%d" % (k, v))
    return ", ".join(ops) + ("  " + " ".join(rest) if rest else "")


def instructions(blob, start=0x80):
    """(offset from start of code, word) for each instruction, skipping scheduling words"""
    end = len(blob)
    i = start
    n = 0
    while i + 8 <= end:
        if n % 4:
            w = struct.unpack_from("<Q", blob, i)[0]
            yield i - start, w
            if w == 0xE2400FFFFF87000F or w == 0xE2400FFFFF07000F:   # BRA to itself: end of program
                return
        i += 8
        n += 1


STAGES = {1: "vertex", 2: "tess-ctrl", 3: "tess-eval", 4: "geometry", 5: "fragment"}


def sph(blob):
    w = struct.unpack_from("<20I", blob, 0x30)
    kind = (w[0] >> 10) & 15
    info = {"stage": STAGES.get(kind, "?%d" % kind), "words": w}
    if kind == 5:
        imap = []
        for i in range(32):
            b = (w[6 + (i >> 2)] >> ((i & 3) * 8)) & 0xFF
            if b:
                imap.append("%d:%s" % (i, "".join(".CPL"[(b >> (2 * c)) & 3] for c in range(4))))
        info["imap"] = imap
        info["omap"] = "%08x" % w[18]
        info["depth"] = (w[19] >> 1) & 1
        info["kills"] = (w[0] >> 15) & 1
    return info


INC_HEADER = """\
// Maxwell (SM 5.x) opcode patterns, most specific first: X(mask, value, emitter name).
// Generated by tools/maxwell_dis.py --gen-inc from the instruction table of Ryujinx's
// Ryujinx.Graphics.Shader (Decoders/InstTable.cs).
//
// Copyright (c) Ryujinx Team and Contributors. MIT License:
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions: The above copyright notice and this
// permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""


def gen_inc(path):
    with open(path, "w", newline="\n") as f:
        f.write(INC_HEADER)
        for _, mask, val, _, emitter in TABLE:
            f.write("X(0x%016xull, 0x%016xull, %s)\n" % (mask, val, emitter))


def main():
    if sys.argv[1] == "--gen-inc":
        gen_inc(sys.argv[2])
        return
    if sys.argv[1] == "--census":
        hist = collections.Counter()
        files = sorted(glob.glob(os.path.join(sys.argv[2], "*.code")))
        for f in files:
            b = open(f, "rb").read()
            for _, w in instructions(b):
                hist[decode(w)[1]] += 1
        print(len(files), "programs")
        for k, v in hist.most_common():
            print("%6d %s" % (v, k))
        return
    for f in sys.argv[1:]:
        b = open(f, "rb").read()
        h = sph(b)
        print("== %s (%s)" % (f, h["stage"]))
        print("   sph " + " ".join("%08x" % x for x in h["words"]))
        if "imap" in h:
            print("   imap %s  omap %s depth %d kills %d" % (" ".join(h["imap"]), h["omap"], h["depth"], h["kills"]))
        for off, w in instructions(b):
            name, emitter = decode(w)
            f = fields(emitter, w)
            p = "" if f.get("Pred", 7) == 7 and not f.get("PredInv") else "@%s " % pred(f["Pred"], f.get("PredInv"))
            print("  %04x  %016x  %s%s %s" % (off, w, p, emitter, operands(name, emitter, f, off)))


if __name__ == "__main__":
    main()
