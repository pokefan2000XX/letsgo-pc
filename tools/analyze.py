#!/usr/bin/env python3
"""Build the function and import database for an NSO-derived ELF and take an
instruction census of every function (the recompiler's to-do list).

Writes:
  data/<mod>_functions.csv   Address,Size,Name,Source,LSDA   (addresses use the module base)
  data/<mod>_imports.csv     Name,Demangled,Kind,PLT,GOT,CallSites
  data/<mod>_census.json     instruction mix, system registers, indirect branches, gaps

Function boundaries come from .eh_frame (every compiled function has an FDE),
cross-checked against direct BL targets, exports and PLT stubs.

Usage: python -P tools/analyze.py orig/exefs/main --module main --base 0x7100000000
"""
import argparse
import bisect
import collections
import csv
import json
import os
import re
import struct
import subprocess
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso2elf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CXXFILT = os.path.join(ROOT, "toolchain", "llvm-mingw", "bin", "llvm-cxxfilt.exe")


def uleb(b, o):
    r = s = 0
    while True:
        x = b[o]
        o += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, o


def sleb(b, o):
    r = s = 0
    while True:
        x = b[o]
        o += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            if x & 0x40:
                r -= 1 << s
            return r, o


def read_enc(b, o, enc, field_addr):
    """DWARF EH pointer encoding; returns (value, new offset). Only what clang emits."""
    if enc == 0xFF:
        return None, o
    fmt = enc & 0x0F
    if fmt == 0x0B:
        v, o2 = struct.unpack_from("<i", b, o)[0], o + 4
    elif fmt == 0x03:
        v, o2 = struct.unpack_from("<I", b, o)[0], o + 4
    elif fmt in (0x00, 0x04):
        v, o2 = struct.unpack_from("<Q", b, o)[0], o + 8
    elif fmt == 0x0C:
        v, o2 = struct.unpack_from("<q", b, o)[0], o + 8
    elif fmt == 0x01:
        v, o2 = uleb(b, o)
    elif fmt == 0x09:
        v, o2 = sleb(b, o)
    else:
        raise ValueError("pointer encoding 0x%x" % enc)
    if enc & 0x70 == 0x10:
        v += field_addr
    return v, o2


def parse_eh_frame(img, start, size):
    cies, fdes = {}, []
    p, end = start, start + size
    while p < end:
        length = struct.unpack_from("<I", img, p)[0]
        if length == 0:
            break
        hdr = p + 4
        cie_id = struct.unpack_from("<I", img, hdr)[0]
        nxt = hdr + length
        if cie_id == 0:
            q = hdr + 4
            ver = img[q]
            q += 1
            aug_end = img.index(b"\0", q)
            aug = img[q:aug_end].decode()
            q = aug_end + 1
            _, q = uleb(img, q)
            _, q = sleb(img, q)
            if ver == 1:
                q += 1
            else:
                _, q = uleb(img, q)
            info = {"fde_enc": 0, "lsda_enc": 0xFF, "personality": None}
            if aug.startswith("z"):
                _, q = uleb(img, q)
                for ch in aug[1:]:
                    if ch == "R":
                        info["fde_enc"] = img[q]
                        q += 1
                    elif ch == "L":
                        info["lsda_enc"] = img[q]
                        q += 1
                    elif ch == "P":
                        penc = img[q]
                        q += 1
                        info["personality"], q = read_enc(img, q, penc, q)
            cies[p] = info
        else:
            cie = cies[hdr - cie_id]
            q = hdr + 4
            pc, q = read_enc(img, q, cie["fde_enc"], q)
            rng, q = read_enc(img, q, cie["fde_enc"] & 0x0F, q)
            lsda = None
            if cie["lsda_enc"] != 0xFF:
                _, q2 = uleb(img, q)
                lsda, _ = read_enc(img, q2, cie["lsda_enc"], q2)
                if lsda == 0 or (cie["lsda_enc"] & 0x70 == 0x10 and lsda == q2):
                    lsda = None
            fdes.append((pc & 0xFFFFFFFFFFFF, rng, lsda))
        p = nxt
    return fdes


def demangle(names):
    names = list(names)
    if not names or not os.path.exists(CXXFILT):
        return {n: n for n in names}
    out = subprocess.run([CXXFILT], input="\n".join(names) + "\n", capture_output=True, text=True).stdout
    return dict(zip(names, out.split("\n")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("--module", required=True)
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x7100000000)
    ap.add_argument("--out", default=os.path.join(ROOT, "data"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    B = a.base

    nso = nso2elf.load_nso(a.nso)
    info = nso2elf.analyze(nso)
    img = nso["image"]
    t0, tsz = nso["text"]
    t1 = t0 + tsz

    # ---------------------------------------------------------- functions --
    fdes = parse_eh_frame(img, *info["eh_frame"])
    funcs = {}
    for pc, rng, lsda in fdes:
        if t0 <= pc < t1:
            funcs[pc] = {"size": rng, "src": "eh_frame", "lsda": lsda}
    syms = info["symbols"]
    exports = {s["value"]: s["name"] for s in syms if s["shndx"] and s["name"] and s["type"] == 2}
    for addr, name in exports.items():
        f = funcs.setdefault(addr, {"size": 0, "src": "export", "lsda": None})
        f["name"] = name
    sym_size = {s["value"]: s["size"] for s in syms if s["shndx"] and s["type"] == 2}
    for addr, f in funcs.items():
        if f["size"] == 0 and sym_size.get(addr):
            f["size"] = sym_size[addr]
    plt = info["plt"]
    for addr, name in plt.items():
        funcs[addr] = {"size": 16, "src": "plt", "lsda": None, "name": name + "@plt"}

    # direct BL/B targets (a cross-check for functions without unwind info)
    bl_targets, b_targets = collections.Counter(), collections.Counter()
    for addr in range(t0, t1, 4):
        w = struct.unpack_from("<I", img, addr)[0]
        top = w & 0xFC000000
        if top in (0x94000000, 0x14000000):
            imm = w & 0x3FFFFFF
            if imm & 0x2000000:
                imm -= 1 << 26
            tgt = addr + imm * 4
            if t0 <= tgt < t1:
                (bl_targets if top == 0x94000000 else b_targets)[tgt] += 1

    starts = sorted(funcs)
    ends = [s + funcs[s]["size"] for s in starts]

    def containing(x):
        i = bisect.bisect_right(starts, x) - 1
        return starts[i] if i >= 0 and x < ends[i] else None

    bl_mid = [t for t in bl_targets if t not in funcs]
    bl_unknown = [t for t in bl_mid if containing(t) is None]
    for t in bl_unknown:
        funcs[t] = {"size": 0, "src": "bl_target", "lsda": None}
    # sizes for functions found only by BL: run to the next known start
    starts = sorted(funcs)
    for i, s in enumerate(starts):
        if funcs[s]["size"] == 0:
            nxt = starts[i + 1] if i + 1 < len(starts) else t1
            funcs[s]["size"] = nxt - s
            funcs[s]["size_guessed"] = True
    ends = [s + funcs[s]["size"] for s in starts]

    # gaps in .text not covered by any function
    gaps, cur = [], t0
    for s, e in zip(starts, ends):
        if s > cur:
            gaps.append((cur, s - cur))
        cur = max(cur, e)
    if cur < t1:
        gaps.append((cur, t1 - cur))
    gap_nonpad = []
    for g0, gs in gaps:
        words = struct.unpack_from("<%dI" % (gs // 4), img, g0)
        if any(w not in (0, 0xD503201F) for w in words):  # zero / NOP padding
            gap_nonpad.append((g0, gs))

    # ------------------------------------------------------------- census --
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    mnem = collections.Counter()
    sysregs = collections.Counter()
    special = collections.Counter()
    vector_ops = scalar_fp = total = invalid = 0
    br_kinds = collections.Counter()
    blr = 0
    VEC = re.compile(r"\bv\d+\.(\d+)?[bhsdq]\b|\bv\d+\.[bhsd]\[")
    FPREG = re.compile(r"\b[bhsdq]\d+\b")
    for s in sorted(funcs):
        f = funcs[s]
        if f["src"] == "plt":
            continue
        code = bytes(img[s:s + f["size"]])
        window = collections.deque(maxlen=8)
        pos = 0
        while pos < len(code):
            got_any = False
            for (addr, size, m, ops) in md.disasm_lite(code[pos:], s + pos):
                got_any = True
                pos = addr - s + size
                total += 1
                mnem[m] += 1
                if VEC.search(ops):
                    vector_ops += 1
                elif m.startswith("f") or FPREG.search(ops):
                    scalar_fp += 1
                if m in ("mrs", "msr"):
                    reg = ops.split(",")[1 if m == "mrs" else 0].strip()
                    sysregs[m + " " + reg] += 1
                elif m in ("svc", "hvc", "smc", "brk", "hlt", "udf", "dcps1"):
                    special[m + " " + ops] += 1
                elif m in ("dmb", "dsb", "isb", "clrex", "wfe", "wfi", "sev", "sevl", "yield", "dc", "ic", "tlbi", "sys", "sysl", "hint"):
                    special[m] += 1
                elif re.match(r"(ld|st)(a)?(x|ax|lx)[rp]|ld(a|ar)r|stlr|cas|swp|ldadd|ldset|ldclr|ldeor", m):
                    special["atomic " + m] += 1
                elif m.startswith(("aes", "sha1", "sha256", "pmull", "crc32")):
                    special["crypto " + m] += 1
                if m == "blr":
                    blr += 1
                elif m == "br":
                    ctx = list(window)
                    regoff = any(re.match(r"ldr(b|h|sw)?$", pm) and re.search(r"\[x\d+, [xw]\d+", po) for pm, po in ctx)
                    addlsl = any(pm == "add" and "lsl #2" in po for pm, po in ctx)
                    br_kinds["jump table" if (regoff and addlsl) or regoff else "computed / tail call"] += 1
                window.append((m, ops))
            if pos < len(code) and (not got_any or True):
                # stopped on an undecodable word: record it and step over
                if pos < len(code):
                    invalid += 1
                    pos += 4

    # -------------------------------------------------------------- imports --
    callsites = collections.Counter()
    for t, n in bl_targets.items():
        if t in plt:
            callsites[plt[t]] += n
    for t, n in b_targets.items():
        if t in plt:
            callsites[plt[t]] += n
    got_of = {}
    for r in info["relocs"]:
        if r["type"] in (1025, 1026, 257) and r["sym"]:
            got_of.setdefault(syms[r["sym"]]["name"], r["offset"])
    import_names = sorted({s["name"] for s in syms if s["shndx"] == 0 and s["name"]})
    dm = demangle(import_names + [f.get("name") for f in funcs.values() if f.get("name")])
    plt_by_name = {n: a for a, n in plt.items()}
    with open(os.path.join(a.out, "%s_imports.csv" % a.module), "w", newline="", encoding="utf-8") as fp:
        w = csv.writer(fp)
        w.writerow(["Name", "Demangled", "Kind", "PLT", "GOT", "CallSites"])
        for n in import_names:
            kind = "func" if n in plt_by_name else "data"
            w.writerow([n, dm.get(n, n), kind,
                        "0x%x" % (B + plt_by_name[n]) if n in plt_by_name else "",
                        "0x%x" % (B + got_of[n]) if n in got_of else "", callsites.get(n, 0)])

    with open(os.path.join(a.out, "%s_functions.csv" % a.module), "w", newline="", encoding="utf-8") as fp:
        w = csv.writer(fp)
        w.writerow(["Address", "Size", "Name", "Source", "LSDA"])
        for s in sorted(funcs):
            f = funcs[s]
            w.writerow(["0x%x" % (B + s), f["size"], f.get("name", ""), f["src"] + ("?" if f.get("size_guessed") else ""),
                        "0x%x" % (B + f["lsda"]) if f["lsda"] else ""])

    census = {
        "module": a.module, "base": "0x%x" % B, "build_id": nso["build_id"],
        "text_bytes": tsz, "instructions": total, "undecodable_words": invalid,
        "functions": {"total": len(funcs), "eh_frame": sum(1 for f in funcs.values() if f["src"] == "eh_frame"),
                      "export_only": sum(1 for f in funcs.values() if f["src"] == "export"),
                      "plt": len(plt), "bl_only": len(bl_unknown),
                      "with_lsda": sum(1 for f in funcs.values() if f["lsda"]),
                      "bl_into_middle_of_function": len([t for t in bl_mid if containing(t) not in (None, t)])},
        "text_gaps": {"count": len(gaps), "bytes": sum(g[1] for g in gaps),
                      "non_padding": [("0x%x" % (B + g0), gs) for g0, gs in gap_nonpad[:50]],
                      "non_padding_count": len(gap_nonpad), "non_padding_bytes": sum(g[1] for g in gap_nonpad)},
        "mnemonics_distinct": len(mnem), "mnemonics_top": mnem.most_common(60),
        "mnemonics_all": dict(sorted(mnem.items())),
        "vector_simd_instructions": vector_ops, "scalar_fp_instructions": scalar_fp,
        "system_registers": dict(sysregs.most_common()), "special": dict(special.most_common()),
        "indirect": {"blr": blr, "br": dict(br_kinds), "ret": mnem.get("ret", 0)},
    }
    json.dump(census, open(os.path.join(a.out, "%s_census.json" % a.module), "w"), indent=1)
    show = {k: census[k] for k in ("instructions", "undecodable_words", "functions", "mnemonics_distinct",
                                   "vector_simd_instructions", "scalar_fp_instructions", "indirect")}
    show["text_gaps"] = {k: census["text_gaps"][k] for k in ("count", "bytes", "non_padding_count", "non_padding_bytes")}
    print(json.dumps(show, indent=1))
    print("system registers:", census["system_registers"])
    print("special:", census["special"])


if __name__ == "__main__":
    sys.exit(main())
