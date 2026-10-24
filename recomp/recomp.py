#!/usr/bin/env python3
"""Static recompiler driver: translate every function of a module to C.

  python -P recomp/recomp.py main sdk [--out build/gen] [--per-file 40000]
  python -P recomp/recomp.py patch --patch-elf build/decomp/patch.elf --replaces build/decomp/replaces.json [--main-out DIR]

Writes <out>/<mod>/fNNN.c (functions, split by instruction count),
<out>/<mod>/table.c (guest address -> host function), and <out>/<mod>/report.json
(jump tables, unusual branches, trapping instructions).

Decompiled code replaces original functions at link time: main's and sdk's functions are
emitted as o<addr> with a weak alias f<addr>, the name every call, tail call and the dispatch
table use. The patch module (decompiled C++ built by decomp/tools/build.py) defines a strong
f<addr> for each function it replaces, which forwards to the decompiled version (or back to
o<addr> when LETSGO_NO_DECOMP turns it off), so main never has to be regenerated.
"""
import argparse
import collections
import csv
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import a64  # noqa: E402
import emit  # noqa: E402
import nso2elf  # noqa: E402

BASES = {"main": 0x7100000000, "sdk": 0x7180000000}
FLAG_SETTERS = {"bcond", "ccmp", "fcmp", "fccmp", "csel", "fcsel", "adc"}
# Originals by another address, for hooks: the decomp module calls ORIG_<address> (decomp/tools/build.py)
# at ORIGINALS + (address - main's base), and that call reaches o<address>, the original's body, even
# when the decomp module replaces it. The range is within BL reach of the patch module (0x7102000000), so
# a BL there stays a direct call; the guest toolchain's calls are mostly indirect (-fno-plt), and those
# reach the same body through rc_lookup (runtime/src/loader.cpp).
ORIGINALS, ORIGINALS_SPAN = 0x7104000000, 0x2000000


def fname(addr):
    return "f%x" % addr


def callee_name(addr):
    """The C function a call to addr reaches: an original's body for its ORIGINALS address, else f<addr>."""
    if ORIGINALS <= addr < ORIGINALS + ORIGINALS_SPAN:
        return bname(addr - ORIGINALS + BASES["main"])
    return fname(addr)


def dest_reg(ins):
    """GPR written by a simple data-processing / load instruction (or None)."""
    op = ins.op
    if op in ("addsub_imm", "addsub_reg", "addsub_ext", "logic_imm", "logic_reg", "movw", "bfm", "extr", "adr",
              "adrp", "csel", "dp1", "dp2", "dp3", "adc"):
        return ins.rd
    if op in ("ldst",) and not ins.fp and ins.kind in ("ld", "lds"):
        return ins.rt
    if op == "ldr_lit" and not ins.fp:
        return ins.rt
    return None


def find_jump_table(insns, k, fstart, fend, img, base):
    """Recognize clang's jump-table sequence ending at insns[k] (a BR).
    Returns (register holding the target, [targets]) or None."""
    br = insns[k]
    reg = br.rn
    j = k - 1
    while j >= 0 and k - j <= 24 and dest_reg(insns[j]) != reg:
        j -= 1
    if j < 0 or k - j > 24:
        return None
    add = insns[j]
    if add.op != "addsub_reg" or add.sub or add.s or add.amount != 0 or not add.sf:
        return None
    a, b = add.rn, add.rm
    # the 32-bit offset load: ldrsw entry, [table, index, lsl #2]
    m = j - 1
    ld = None
    while m >= 0 and j - m <= 12:
        x = insns[m]
        if x.op == "ldst" and not x.fp and x.kind == "lds" and x.nbytes == 4 and x.mode == "reg" and x.rt in (a, b):
            ld = x
            break
        m -= 1
    if ld is None:
        return None
    table_reg = b if ld.rt == a else a
    if ld.rn != table_reg or ld.amount != 2:
        return None
    # table address: adrp table_reg ; add table_reg, table_reg, #lo
    table = None
    lo = None
    p = m - 1
    while p >= 0 and m - p <= 24:
        x = insns[p]
        if lo is None and x.op == "addsub_imm" and not x.s and not x.sub and x.rd == table_reg and x.rn == table_reg:
            lo = x.imm
        elif x.op == "adrp" and x.rd == table_reg:
            table = x.value + (lo or 0)
            break
        elif (x.op == "logic_reg" and x.opc == 1 and x.rn == "zr" and not x.neg and x.amount == 0
              and x.rd == table_reg and lo is None):
            table_reg = x.rm            # mov table_reg, other: keep following the copy
        elif dest_reg(x) == table_reg and x.op != "addsub_imm":
            break
        p -= 1
    if table is None:
        # table address hoisted out of a loop: any adrp+add pair in the function that
        # builds table_reg and whose entries all land inside the function
        cands = []
        for p2 in range(len(insns) - 1):
            x = insns[p2]
            if x.op == "adrp" and x.rd == table_reg:
                for p3 in range(p2 + 1, min(p2 + 8, len(insns))):
                    y = insns[p3]
                    if y.op == "addsub_imm" and not y.s and not y.sub and y.rd == table_reg and y.rn == table_reg:
                        cands.append(x.value + y.imm)
                        break
        good = []
        for tb in set(cands):
            off = tb - base
            if 0 <= off < len(img) - 4:
                t = tb + struct.unpack_from("<i", img, off)[0]
                if fstart <= t < fend and not t & 3:
                    good.append(tb)
        if len(good) != 1:
            return None
        table = good[0]
    # bound: cmp idx, #K ; b.hi / b.hs default (nearest before the table load)
    count = None
    q = m - 1
    while q >= 0 and m - q <= 24:
        x = insns[q]
        if x.op == "bcond" and x.cond in (8, 2):
            for r in range(q - 1, max(q - 6, -1), -1):
                y = insns[r]
                if y.op == "addsub_imm" and y.s and y.sub and y.rd == "zr":
                    count = y.imm + 1 if x.cond == 8 else y.imm
                    break
            break
        q -= 1
    targets = []
    if count and count <= 4096:
        for e in range(count):
            off = table - base + 4 * e
            if not 0 <= off < len(img) - 4:
                return None
            t = table + struct.unpack_from("<i", img, off)[0]
            if not (fstart <= t < fend) or t & 3:
                return None
            targets.append(t)
    else:
        # bounds check lives in another block: read entries while they stay inside the
        # function (over-reading only adds unreachable cases, never drops real ones)
        for e in range(4096):
            off = table - base + 4 * e
            if not 0 <= off < len(img) - 4:
                break
            t = table + struct.unpack_from("<i", img, off)[0]
            if not (fstart <= t < fend) or t & 3:
                break
            targets.append(t)
        if not targets:
            return None
    return reg, targets


def write_if_changed(path, text):
    """Write `text` unless the file already holds it (unchanged files keep their timestamps)."""
    try:
        with open(path, "r", newline="\n") as fp:
            if fp.read() == text:
                return
    except OSError:
        pass
    with open(path, "w", newline="\n") as fp:
        fp.write(text)


def add_entry_points(funcs, img, base):
    """Branches (b/b.cond/cbz/tbz/bl) that leave a function and land somewhere other
    than a known entry make that address an extra entry: a function running to the end
    of whatever contains it (or to the next entry). Repeats until nothing new appears."""
    import bisect
    starts = sorted(f[0] for f in funcs)
    size = dict(funcs)
    todo = list(funcs)
    added = 0
    while todo:
        new = {}
        for fs, sz in todo:
            for a in range(fs, fs + sz, 4):
                w = struct.unpack_from("<I", img, a - base)[0]
                ins = a64.decode(w, a)
                if ins.op not in ("b", "bcond", "cbz", "cbnz", "tbz", "tbnz", "bl"):
                    continue
                t = ins.target
                if fs <= t < fs + sz or t in size or t in new:
                    continue
                if not 0 <= t - base < len(img):
                    continue
                k = bisect.bisect_right(starts, t) - 1
                if k >= 0 and t < starts[k] + size[starts[k]]:
                    end = starts[k] + size[starts[k]]
                else:
                    end = starts[k + 1] if k + 1 < len(starts) else t + 4
                new[t] = end - t
        for t, sz in new.items():
            size[t] = sz
            bisect.insort(starts, t)
        added += len(new)
        todo = list(new.items())
    return sorted(size.items()), added


def bname(addr):
    """The C name of an original function's body (f<addr> is a weak alias of it)."""
    return "o%x" % addr


def load_nso_module(mod):
    """main or sdk: image, base, functions (data/<mod>_functions.csv) and the code addresses
    its RELATIVE relocations store."""
    base = BASES[mod]
    nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", mod))
    rows = list(csv.DictReader(open(os.path.join(ROOT, "data", "%s_functions.csv" % mod))))
    funcs = sorted((int(r["Address"], 16), int(r["Size"])) for r in rows)
    info = nso2elf.analyze(nso)
    relative = {base + r["addend"] for r in info["relocs"] if r["type"] == 1027}
    return {"img": nso["image"], "base": base, "funcs": funcs, "relative": relative, "alias": True}


def load_patch_module(elf_path):
    """The decomp patch module: a static AArch64 ELF linked at its guest address. Functions
    come from its symbol table (it is our own build, so every function has a symbol). Returns
    None when there is no patch module yet."""
    if not elf_path or not os.path.exists(elf_path):
        return None
    data = open(elf_path, "rb").read()
    phoff, shoff = struct.unpack_from("<QQ", data, 0x20)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 0x36)
    segs = []
    for i in range(phnum):
        p_type, _flags, off, va, _pa, fsz, msz, _al = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
        if p_type == 1 and msz:
            segs.append((va, off, fsz, msz))
    if not segs:
        return None
    base = min(sg[0] for sg in segs)
    end = max(sg[0] + sg[3] for sg in segs)
    img = bytearray(end - base)
    for va, off, fsz, _msz in segs:
        img[va - base:va - base + fsz] = data[off:off + fsz]
    secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    funcs = {}
    for name, typ, _fl, _addr, off, size, link, _info, _al, entsize in secs:
        if typ != 2:                                    # SHT_SYMTAB
            continue
        strtab_off = secs[link][4]
        for k in range(size // entsize):
            st_name, st_info, _other, shndx, value, ssize = struct.unpack_from("<IBBHQQ", data, off + k * entsize)
            if st_info & 0xF == 2 and ssize and shndx and base <= value < end:    # STT_FUNC
                funcs[value] = max(funcs.get(value, 0), ssize)
    return {"img": bytes(img), "base": base, "funcs": sorted(funcs.items()), "relative": set(), "alias": False,
            "end": end}


def main_function_starts(out_dir):
    """Function entries of the recompiled main (its table.c), so decompiled code calls them directly."""
    path = os.path.join(out_dir, "main", "table.c")
    starts = set()
    if os.path.exists(path):
        for line in open(path):
            if line.startswith("{0x"):
                starts.add(int(line[1:line.index("ull")], 16))
    return starts


def translate(mod, out_dir, per_file, src, extern_starts=frozenset()):
    img, base, alias = src["img"], src["base"], src["alias"]
    funcs, n_extra = add_entry_points(src["funcs"], img, base)
    starts = {f[0] for f in funcs}
    # code addresses stored in data (RELATIVE relocations) that land inside a function rather than
    # at an entry: labels-as-values tables (computed goto, e.g. the Pawn AMX interpreter's opcode
    # handlers). A BR without a recognized jump table may jump to any of them.
    import bisect
    code_ptrs = sorted(src["relative"] - starts)
    os.makedirs(os.path.join(out_dir, mod), exist_ok=True)

    report = {"functions": len(funcs), "extra_entries": n_extra, "instructions": 0, "jump_tables": 0, "jump_table_failures": [],
              "bad_branches": [], "traps": collections.Counter(), "fallthrough": 0, "computed_goto": []}
    files = []
    cur, cur_n, cur_defined, cur_callees = [], 0, [], set()

    def flush():
        nonlocal cur, cur_n, cur_defined, cur_callees
        if not cur:
            return
        idx = len(files)
        path = os.path.join(out_dir, mod, "f%03d.c" % idx)
        decl = sorted(cur_callees | set(cur_defined))
        text = ('#include "rc.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
                '#pragma clang diagnostic ignored "-Wpedantic"\n' +
                "".join("void %s(Ctx* restrict c);\n" % callee_name(a) for a in decl) + "\n".join(cur) + "\n")
        write_if_changed(path, text)
        files.append(path)
        cur, cur_n, cur_defined, cur_callees = [], 0, [], set()

    for fi, (fstart, fsize) in enumerate(funcs):
        fend = fstart + fsize
        insns = [a64.decode(struct.unpack_from("<I", img, a - base)[0], a) for a in range(fstart, fend, 4)]
        labels = set()
        jts = {}
        lo, hi = bisect.bisect_right(code_ptrs, fstart), bisect.bisect_left(code_ptrs, fend)
        goto_targets = [t for t in code_ptrs[lo:hi] if not t & 3]
        has_free_br = False
        for k, ins in enumerate(insns):
            t = getattr(ins, "f", {}).get("target")
            if ins.op in ("b", "bcond", "cbz", "cbnz", "tbz", "tbnz") and fstart <= t < fend:
                labels.add(t)
            elif ins.op == "bl" and fstart < t < fend and t not in starts:
                labels.add(t)
            elif ins.op == "br":
                jt = find_jump_table(insns, k, fstart, fend, img, base)
                if jt:
                    jts[ins.pc] = jt
                    labels.update(jt[1])
                    report["jump_tables"] += 1
                else:
                    has_free_br = True
        if goto_targets and has_free_br:
            labels.update(goto_targets)
            report["computed_goto"].append(("0x%x" % fstart, len(goto_targets)))
        else:
            goto_targets = []
        fc = emit.FuncCtx(fstart, fend, labels, starts | extern_starts, jts, callee_name)
        fc.goto_targets = goto_targets
        body = []
        for ins in insns:
            if ins.pc in labels:
                body.append("%s: ;" % emit.lbl(ins.pc))
            lines = emit.emit(ins, fc)
            for ln in lines:
                if ln.startswith("rc_trap("):
                    report["traps"][ln.split('"')[1]] += 1
            body.extend(lines)
        last = insns[-1] if insns else None
        if last is None or last.op not in ("ret", "b", "br", "udf", "brk"):
            body.append('rc_trap(c, 0x%xull, 0, "fell off the end of a function");' % fend)
            report["fallthrough"] += 1
        for pc, t in fc.bad_branches:
            report["bad_branches"].append(("0x%x" % pc, "0x%x" % t))
        for ins in insns:
            if ins.op == "br" and ins.pc not in jts:
                pass
        text = ["void %s(Ctx* restrict c) {" % (bname(fstart) if alias else fname(fstart)),
                "uint32_t N = 0, Z = 0, C = 0, V = 0; (void)N; (void)Z; (void)C; (void)V;"]
        text.extend(body)
        text.append("}")
        if alias:   # the name everything calls; a decompiled replacement overrides it at link time
            text.append('void %s(Ctx* restrict c) __attribute__((weak, alias("%s")));' % (fname(fstart), bname(fstart)))
        cur.append("\n".join(text))
        cur_defined.append(fstart)
        cur_callees |= fc.callees
        cur_n += len(insns)
        report["instructions"] += len(insns)
        if cur_n >= per_file:
            flush()
    flush()
    if not files:
        write_if_changed(os.path.join(out_dir, mod, "f000.c"), '#include "rc.h"\n')
        files.append(os.path.join(out_dir, mod, "f000.c"))

    table = ['#include "rc.h"\n']
    for a, _ in funcs:
        table.append("void %s(Ctx* restrict c);\n" % fname(a))
    table.append("const struct { uint64_t addr; rc_fn fn; } rc_%s_funcs[] = {\n" % mod)
    for a, _ in funcs:
        table.append("{0x%xull, %s},\n" % (a, fname(a)))
    table.append("};\nconst uint32_t rc_%s_nfuncs = %d;\n" % (mod, len(funcs)))
    if alias:   # the original bodies, for mapping host code back to guest functions in backtraces
        for a, _ in funcs:
            table.append("void %s(Ctx* restrict c);\n" % bname(a))
        table.append("const struct { uint64_t addr; rc_fn fn; } rc_%s_bodies[] = {\n" % mod)
        for a, _ in funcs:
            table.append("{0x%xull, %s},\n" % (a, bname(a)))
        table.append("};\n")
    if "patch_extra" in src:
        table.append(src["patch_extra"](funcs))
    write_if_changed(os.path.join(out_dir, mod, "table.c"), "".join(table))
    report["traps"] = dict(report["traps"].most_common())
    report["files"] = len(files)
    report["bad_branch_count"] = len(report["bad_branches"])
    report["bad_branches"] = report["bad_branches"][:200]
    json.dump(report, open(os.path.join(out_dir, mod, "report.json"), "w"), indent=1)
    return report


def patch_extra(src, replaces, main_starts):
    """table.c additions for the patch module: its loaded image (copied to guest memory at
    startup), and a strong f<orig> for every original function it replaces."""
    def make(funcs):
        out = []
        img = src["img"] if src else b""
        out.append("const uint64_t rc_patch_base = 0x%xull, rc_patch_size = 0x%xull;\n" %
                   (src["base"] if src else 0, len(img)))
        out.append("const uint8_t rc_patch_image[%d] = {" % max(len(img), 1))
        if img:
            for i in range(0, len(img), 32):
                out.append("\n" + ",".join(str(b) for b in img[i:i + 32]) + ",")
        else:
            out.append("0")
        out.append("};\n")
        patch_starts = {a for a, _ in funcs}
        out.append("int rc_decomp_off(uint64_t addr, int* seen);\n")
        rows = []
        for orig, repl in sorted(replaces.items()):
            if orig not in main_starts:
                raise SystemExit("decomp: 0x%x is not the start of a recompiled main function" % orig)
            if repl not in patch_starts:
                raise SystemExit("decomp: replacement 0x%x for 0x%x is not a patch function" % (repl, orig))
            out.append("void %s(Ctx* restrict c);\nvoid %s(Ctx* restrict c);\n" % (bname(orig), fname(repl)))
            out.append("void %s(Ctx* restrict c) { static int seen; if (rc_decomp_off(0x%xull, &seen)) TAIL(%s); TAIL(%s); }\n" %
                       (fname(orig), orig, bname(orig), fname(repl)))
            rows.append("{0x%xull, 0x%xull}," % (orig, repl))
        out.append("const struct { uint64_t orig, repl; } rc_patch_replaces[%d] = {%s};\n" %
                   (max(len(rows), 1), "".join(rows) or "{0, 0}"))
        out.append("const uint32_t rc_patch_nreplaces = %d;\n" % len(rows))
        return "".join(out)
    return make


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("modules", nargs="+")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "gen"))
    ap.add_argument("--per-file", type=int, default=40000)
    ap.add_argument("--patch-elf", help="the decomp patch module (for the module 'patch')")
    ap.add_argument("--replaces", help="JSON {original address: replacement address} (for the module 'patch')")
    ap.add_argument("--main-out", help="where main was translated, when not --out (for the module 'patch')")
    a = ap.parse_args()
    for mod in a.modules:
        if mod == "patch":
            src = load_patch_module(a.patch_elf)
            replaces = {}
            if a.replaces and os.path.exists(a.replaces):
                replaces = {int(k, 16): int(v, 16) for k, v in json.load(open(a.replaces)).items()}
            main_starts = main_function_starts(a.main_out or a.out)
            if not main_starts:
                raise SystemExit("patch: translate main first (build/gen/main/table.c)")
            if src is None:
                if replaces:
                    raise SystemExit("patch: replacements given without a patch module")
                src = {"img": b"", "base": 0, "funcs": [], "relative": set(), "alias": False}
                src["patch_extra"] = patch_extra(None, {}, main_starts)
            else:
                src["patch_extra"] = patch_extra(src, replaces, main_starts)
            originals = {ORIGINALS + s - BASES["main"] for s in main_starts}
            r = translate(mod, a.out, 1 << 40, src, frozenset(main_starts) | originals)
        else:
            r = translate(mod, a.out, a.per_file, load_nso_module(mod))
        print("%s: %d functions (%d extra entries), %d instructions, %d files, %d jump tables, %d computed-goto functions, %d bad branches, %d fallthrough" % (
            mod, r["functions"], r["extra_entries"], r["instructions"], r["files"], r["jump_tables"], len(r["computed_goto"]),
            r["bad_branch_count"], r["fallthrough"]))
        print("   traps:", r["traps"])


if __name__ == "__main__":
    main()
