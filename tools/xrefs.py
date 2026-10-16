#!/usr/bin/env python3
"""Cross-reference database for main: vtables, call graph, and which functions
and imports are statically reachable from the boot entry points.

Writes:
  data/main_vtables.csv          AddressPoint,Kind,Slots,CodeRefs,Targets
                                 (Kind: vtable = preceded by the Itanium offset-to-top/RTTI
                                 words; table = other function-pointer array)
  data/main_reachability.json    reachable functions/imports from nninitStartup,
                                 the static constructors and nnMain
  orig/analysis/main_callgraph.tsv   caller  callee  kind(call/tail/ref/vtable)  count

Reachability follows direct calls, tail calls, function addresses taken in
reachable code, and every slot of a vtable whose address point reachable code
uses (constructors store it). Indirect calls through other data are not
followed, so this is a lower bound.

Usage: python -P tools/xrefs.py orig/exefs/main --module main --base 0x7100000000
"""
import argparse
import bisect
import collections
import csv
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso2elf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("--module", required=True)
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x7100000000)
    a = ap.parse_args()
    B = a.base
    nso = nso2elf.load_nso(a.nso)
    info = nso2elf.analyze(nso)
    img = nso["image"]
    t0, tsz = nso["text"]
    d0, dsz = nso["data"]
    syms = info["symbols"]

    rows = list(csv.DictReader(open(os.path.join(ROOT, "data", "%s_functions.csv" % a.module))))
    funcs = sorted((int(r["Address"], 16) - B, int(r["Size"]), r["Source"]) for r in rows)
    fstart = {f[0] for f in funcs}
    starts = [f[0] for f in funcs]
    plt = info["plt"]                                   # stub -> import name

    def func_of(x):
        i = bisect.bisect_right(starts, x) - 1
        return starts[i] if i >= 0 and x < starts[i] + funcs[i][1] else None

    rel = {r["offset"]: r["addend"] for r in info["relocs"] if r["type"] == 1027}
    symptr = {r["offset"]: syms[r["sym"]]["name"] for r in info["relocs"]
              if r["type"] in (257, 1025) and r["sym"] and syms[r["sym"]]["shndx"] == 0}

    # ------------------------------------------------------------ vtables ---
    def is_slot(off):
        t = rel.get(off)
        return (t is not None and t in fstart) or symptr.get(off) in ("__cxa_pure_virtual", "__cxa_deleted_virtual")

    slots = sorted(o for o in set(rel) | set(symptr) if o % 8 == 0 and is_slot(o))
    slot_set = set(slots)

    # code references to data addresses (ADRP+ADD, ADRP+LDR via GOT, ADR)
    words = struct.unpack_from("<%dI" % (tsz // 4), img, t0)
    code_refs = collections.defaultdict(list)          # target -> [insn addr]
    for i, w in enumerate(words):
        pc = t0 + i * 4
        if (w & 0x9F000000) == 0x10000000:               # ADR
            tgt = pc + sx((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21)
            code_refs[tgt].append(pc)
            continue
        if (w & 0x9F000000) != 0x90000000:               # ADRP
            continue
        rd = w & 0x1F
        page = (pc & ~0xFFF) + (sx((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3), 21) << 12)
        for j in range(i + 1, min(i + 10, len(words))):
            v = words[j]
            if (v & 0xFF800000) == 0x91000000 and ((v >> 5) & 0x1F) == rd:
                code_refs[page + (((v >> 10) & 0xFFF) << (12 if v & (1 << 22) else 0))].append(pc)
                break
            if (v & 0xFFC00000) == 0xF9400000 and ((v >> 5) & 0x1F) == rd:
                slot = page + ((v >> 10) & 0xFFF) * 8
                if slot in rel:
                    code_refs[rel[slot]].append(pc)
                elif slot in symptr:
                    code_refs[("import", symptr[slot])].append(pc)
                break

    # split slot runs at code-referenced address points
    tables = []
    i = 0
    while i < len(slots):
        j = i
        while j + 1 < len(slots) and slots[j + 1] == slots[j] + 8:
            j += 1
        run = slots[i:j + 1]
        cuts = [k for k, o in enumerate(run) if o in code_refs] or [0]
        if cuts[0] != 0:
            cuts = [0] + cuts
        for c, k in enumerate(cuts):
            end = cuts[c + 1] if c + 1 < len(cuts) else len(run)
            apnt = run[k]
            hdr = img[apnt - 16:apnt] if apnt >= 16 else b""
            o2t = struct.unpack_from("<q", hdr, 0)[0] if len(hdr) == 16 else None
            rtti = struct.unpack_from("<Q", hdr, 8)[0] if len(hdr) == 16 else None
            is_vt = o2t is not None and -0x10000 < o2t <= 0 and (rtti == 0 or (apnt - 8) in rel or (apnt - 8) in symptr) \
                and (apnt - 16) not in slot_set and (apnt - 8) not in slot_set
            tables.append((apnt, "vtable" if is_vt else "table", run[k:end]))
        i = j + 1

    with open(os.path.join(ROOT, "data", "%s_vtables.csv" % a.module), "w", newline="") as fp:
        w = csv.writer(fp)
        w.writerow(["AddressPoint", "Kind", "Slots", "CodeRefs", "Targets"])
        for apnt, kind, run in tables:
            tg = []
            for o in run:
                t = rel.get(o)
                tg.append("0x%x" % (B + t) if t is not None else symptr[o])
            refs = len(code_refs.get(apnt, [])) + len(code_refs.get(apnt - 16, []))
            w.writerow(["0x%x" % (B + apnt), kind, len(run), refs, " ".join(tg)])
    vt_at = {apnt: run for apnt, kind, run in tables}

    # ---------------------------------------------------------- call graph --
    edges = collections.Counter()
    for f0, fsz, src in funcs:
        if src == "plt":
            continue
        for k in range(f0, f0 + fsz, 4):
            w = struct.unpack_from("<I", img, k)[0]
            top = w & 0xFC000000
            if top in (0x94000000, 0x14000000):
                tgt = k + sx(w & 0x3FFFFFF, 26) * 4
                if top == 0x14000000 and f0 <= tgt < f0 + fsz:
                    continue                             # branch inside the function
                kind = "call" if top == 0x94000000 else "tail"
                callee = ("import", plt[tgt]) if tgt in plt else (func_of(tgt) if tgt not in fstart else tgt)
                if callee is not None:
                    edges[(f0, callee, kind)] += 1
    for tgt, pcs in code_refs.items():
        for pc in pcs:
            f = func_of(pc)
            if f is None:
                continue
            if isinstance(tgt, tuple):
                edges[(f, tgt, "ref")] += 1
            elif tgt in plt:
                edges[(f, ("import", plt[tgt]), "ref")] += 1
            elif tgt in fstart:
                edges[(f, tgt, "ref")] += 1
            elif tgt in vt_at:
                edges[(f, ("vtable", tgt), "vtable")] += 1
            elif tgt + 16 in vt_at:
                # PIC code loads the vtable symbol (offset-to-top word) and adds 16
                edges[(f, ("vtable", tgt + 16), "vtable")] += 1

    out_dir = os.path.join(ROOT, "orig", "analysis")
    os.makedirs(out_dir, exist_ok=True)

    def name(x):
        if isinstance(x, tuple):
            return x[1] if x[0] == "import" else "vtable:0x%x" % (B + x[1])
        return "0x%x" % (B + x)

    with open(os.path.join(out_dir, "%s_callgraph.tsv" % a.module), "w") as fp:
        for (c, e, k), n in sorted(edges.items(), key=lambda kv: (kv[0][0], str(kv[0][1]))):
            fp.write("0x%x\t%s\t%s\t%d\n" % (B + c, name(e), k, n))

    # -------------------------------------------------------- reachability --
    out = collections.defaultdict(set)
    for (c, e, k), n in edges.items():
        out[c].add(e)
    exports = {s["name"]: s["value"] for s in syms if s["shndx"] and s["name"]}
    init_arr = info["dyn"].get("INIT_ARRAY"), info["dyn"].get("INIT_ARRAYSZ", 0)
    ctors = [rel[init_arr[0] + 8 * k] for k in range(init_arr[1] // 8) if init_arr[0] + 8 * k in rel]
    roots = [exports["nninitStartup"], exports["nnMain"]] + ctors
    seen, imports_hit, stack = set(), collections.Counter(), list(roots)
    while stack:
        f = stack.pop()
        if isinstance(f, tuple):
            if f[0] == "import":
                imports_hit[f[1]] += 1
            elif f[0] == "vtable":
                for o in vt_at.get(f[1], []):
                    t = rel.get(o)
                    if t is not None and t not in seen:
                        stack.append(t)
            continue
        if f in seen:
            continue
        seen.add(f)
        stack.extend(out.get(f, ()))
    all_imports = sorted({s["name"] for s in syms if s["shndx"] == 0 and s["name"]})
    res = {
        "roots": {"nninitStartup": "0x%x" % (B + exports["nninitStartup"]), "nnMain": "0x%x" % (B + exports["nnMain"]),
                  "static_constructors": len(ctors)},
        "functions_total": len([f for f in funcs if f[2] != "plt"]),
        "functions_reachable": len(seen),
        "imports_total": len(all_imports),
        "imports_reachable": len(imports_hit),
        "imports_not_reached": sorted(set(all_imports) - set(imports_hit)),
        "vtables": sum(1 for t in tables if t[1] == "vtable"), "fnptr_tables": sum(1 for t in tables if t[1] == "table"),
        "vtable_slots": sum(len(t[2]) for t in tables if t[1] == "vtable"),
        "edges": {k: sum(n for (c, e, kk), n in edges.items() if kk == k) for k in ("call", "tail", "ref", "vtable")},
    }
    json.dump(res, open(os.path.join(ROOT, "data", "%s_reachability.json" % a.module), "w"), indent=1)
    show = {k: v for k, v in res.items() if k != "imports_not_reached"}
    print(json.dumps(show, indent=1))
    print("imports never reached statically (%d): %s" % (len(res["imports_not_reached"]),
                                                          ", ".join(res["imports_not_reached"][:25])))


if __name__ == "__main__":
    sys.exit(main())
