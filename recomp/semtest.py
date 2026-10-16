#!/usr/bin/env python3
"""Instruction-level differential test of the recompiler against Unicorn.

For every distinct instruction pattern in main+sdk, real instruction words are
translated with emit.py into one C test function each, compiled into a DLL with
the runtime helpers, and run against Unicorn (the reference AArch64 CPU) on
random register/flag/memory states. All GPRs, SP, V registers, NZCV, branch
outcome and a window of memory must match.

Usage: python -P recomp/semtest.py [--samples 6] [--cases 40] [--only OP]
"""
import argparse
import collections
import csv
import ctypes
import json
import os
import random
import struct
import subprocess
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL
from unicorn import arm64_const as A

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import a64  # noqa: E402
import emit  # noqa: E402
import nso2elf  # noqa: E402

BASES = {"main": 0x7100000000, "sdk": 0x7180000000}
SCRATCH, SCRATCH_SIZE = 0x7600000000, 0x100000
MID = SCRATCH + SCRATCH_SIZE // 2
SKIP = {"b", "bl", "br", "blr", "ret", "svc", "brk", "hlt", "udf", "unknown", "hint", "sys", "sysl", "mrs", "msr",
        "msr_imm", "clrex", "dmb", "dsb", "isb", "prfm", "ldxp", "stxp"}
BRANCHES = {"bcond", "cbz", "cbnz", "tbz", "tbnz"}
REGF = {"rd", "rn", "rm", "rt", "ra", "rs", "rt2", "target", "value", "addr", "imm", "imm16", "immr", "imms", "lsb",
        "index", "bit", "nzcv", "cond", "imm8", "immb", "imm4", "imm5", "amount", "scale", "l", "m", "h"}
MEMOPS = {"ldst", "ldstp", "ldxr", "stxr", "ldar", "stlr", "vldst_multi", "vldst_single"}


def key_of(ins):
    f = ins.f
    k = [ins.op] + ["%s=%s" % (n, f[n]) for n in sorted(f) if n not in REGFIELDS_FOR(ins)]
    if "immh" in f:
        k = [x for x in k if not x.startswith("immh=")] + ["esz=%d" % (f["immh"].bit_length() - 1)]
    if "imm5" in f:
        k.append("esz=%d" % ((f["imm5"] & -f["imm5"]).bit_length() - 1))
    if "imm4" in f and ins.op in ("v_copy", "s_copy"):
        k.append("imm4=%d" % f["imm4"])
    if ins.op in ("ldst", "ldstp"):
        k.append("negimm=%d" % (f.get("imm", 0) < 0))
    return tuple(k)


def REGFIELDS_FOR(ins):
    return REGF


def collect(samples):
    groups = collections.defaultdict(list)
    for mod, base in BASES.items():
        img = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", mod))["image"]
        for r in csv.DictReader(open(os.path.join(ROOT, "data", "%s_functions.csv" % mod))):
            f0 = int(r["Address"], 16)
            for a in range(f0, f0 + int(r["Size"]), 4):
                w = struct.unpack_from("<I", img, a - base)[0]
                ins = a64.decode(w, a)
                if ins.op in SKIP:
                    continue
                k = key_of(ins)
                g = groups[k]
                if len(g) < samples and all(x.word != w for x in g):
                    g.append(ins)
    return groups


def gen_c(tests):
    out = ['#include "rc.h"', '#pragma clang diagnostic ignored "-Wunused-label"',
           '#pragma clang diagnostic ignored "-Wparentheses-equality"']
    names = []
    for n, ins in enumerate(tests):
        if ins.op in BRANCHES:
            lo, hi = min(ins.pc, ins.target), max(ins.pc, ins.target) + 4
        else:
            lo, hi = ins.pc, ins.pc + 4
        fc = emit.FuncCtx(lo, hi, set(), set(), {}, lambda a: "f%x" % a)
        body = emit.emit(ins, fc)
        nm = "t%d" % n
        names.append(nm)
        out.append("static int %s(Ctx* restrict c) {" % nm)
        out.append("uint32_t N = (c->nzcv >> 31) & 1, Z = (c->nzcv >> 30) & 1, C = (c->nzcv >> 29) & 1, V = (c->nzcv >> 28) & 1;")
        out.extend(body)
        out.append("c->nzcv = (N << 31) | (Z << 30) | (C << 29) | (V << 28); return 0;")
        if ins.op in BRANCHES:
            out.append("%s: c->nzcv = (N << 31) | (Z << 30) | (C << 29) | (V << 28); return 1;" % emit.lbl(ins.target))
        out.append("}")
    out.append("__declspec(dllexport) int (*semtest_funcs[])(Ctx*) = { %s };" % ", ".join(names))
    out.append("__declspec(dllexport) int semtest_count = %d;" % len(names))
    out.append("""
rc_fn rc_lookup(uint64_t t) { (void)t; return 0; }
void rc_svc(Ctx* c, uint32_t imm) { (void)c; (void)imm; }
void rc_trap(Ctx* c, uint64_t pc, uint32_t w, const char* why) { (void)c; (void)pc; (void)w; (void)why; __builtin_trap(); }
uint64_t rc_mrs(Ctx* c, uint64_t pc, uint32_t r) { (void)c; (void)pc; (void)r; return 0; }
void rc_msr(Ctx* c, uint64_t pc, uint32_t r, uint64_t v) { (void)c; (void)pc; (void)r; (void)v; }
uint64_t rc_cntpct(void) { return 0; }
""")
    return "\n".join(out)


class V128(ctypes.Union):
    _fields_ = [("b", ctypes.c_uint8 * 16), ("d", ctypes.c_uint64 * 2)]


class Ctx(ctypes.Structure):
    _fields_ = [("x", ctypes.c_uint64 * 32), ("sp", ctypes.c_uint64), ("v", V128 * 32),
                ("tpidr_el0", ctypes.c_uint64), ("tpidrro_el0", ctypes.c_uint64), ("ex_addr", ctypes.c_uint64),
                ("ex_val", ctypes.c_uint64), ("fpcr", ctypes.c_uint32), ("fpsr", ctypes.c_uint32),
                ("nzcv", ctypes.c_uint32), ("depth", ctypes.c_uint32), ("host", ctypes.c_void_p)]


SPECIAL_F32 = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x7F800000, 0xFF800000, 0x7FC00000,
               0x7FA00000, 0x00000001, 0x80400000, 0x7F7FFFFF, 0x00800000, 0x4F000000, 0xCF000000, 0x5F800000,
               0x3F7FFFFF, 0x47000000, 0x3EAAAAAB, 0x40490FDB]
SPECIAL_F64 = [0x0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000,
               0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0x7FF4000000000000, 0x1,
               0x0008000000000000, 0x7FEFFFFFFFFFFFFF, 0x41E0000000000000, 0xC3E0000000000000, 0x43F0000000000000,
               0x400921FB54442D18]
SPECIAL_X = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x8000000000000000,
             0x7FFFFFFFFFFFFFFF, 0x100000000, 31, 32, 63, 64, 0xFFFFFFFF80000000]


def rand_x(rng):
    r = rng.random()
    if r < 0.3:
        return rng.choice(SPECIAL_X)
    if r < 0.5:
        return rng.randrange(0, 256)
    if r < 0.6:
        return (-rng.randrange(1, 256)) & 0xFFFFFFFFFFFFFFFF
    return rng.getrandbits(64)


def rand_f32(rng):
    r = rng.random()
    if r < 0.3:
        return rng.choice(SPECIAL_F32)
    if r < 0.8:
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-1000, 1000) * 10 ** rng.randint(-6, 6)))[0]
    return rng.getrandbits(32)


def rand_f64(rng):
    r = rng.random()
    if r < 0.3:
        return rng.choice(SPECIAL_F64)
    if r < 0.8:
        return struct.unpack("<Q", struct.pack("<d", rng.uniform(-1000, 1000) * 10 ** rng.randint(-12, 12)))[0]
    return rng.getrandbits(64)


def rand_v(rng):
    r = rng.random()
    if r < 0.4:
        return sum(rand_f32(rng) << (32 * i) for i in range(4))
    if r < 0.7:
        return sum(rand_f64(rng) << (64 * i) for i in range(2))
    if r < 0.8:
        return sum(rng.randrange(0, 256) << (8 * i) for i in range(16))
    return rng.getrandbits(128)


def mem_regs(ins):
    """(base registers, index registers) of a memory instruction."""
    f = ins.f
    bases, idx = [f.get("rn")], []
    if ins.op == "ldst" and f.get("mode") == "reg":
        idx.append(f["rm"])
    if ins.op in ("vldst_multi", "vldst_single") and f.get("post") and f.get("rm") not in (None, "imm"):
        idx.append(f["rm"])
    return bases, idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=6)
    ap.add_argument("--cases", type=int, default=40)
    ap.add_argument("--only")
    ap.add_argument("--seed", type=int, default=1234)
    a = ap.parse_args()
    out_dir = os.path.join(ROOT, "build", "semtest")
    os.makedirs(out_dir, exist_ok=True)

    groups = collect(a.samples)
    if a.only:
        groups = {k: v for k, v in groups.items() if k[0] == a.only}
    tests = [ins for k in sorted(groups) for ins in groups[k]]
    keys = [k for k in sorted(groups) for _ in groups[k]]
    print("%d patterns, %d test instructions" % (len(groups), len(tests)), flush=True)
    src = os.path.join(out_dir, "tests.c")
    open(src, "w", newline="\n").write(gen_c(tests))
    dll = os.path.join(out_dir, "semtest.dll")
    clang = os.path.join(ROOT, "toolchain", "llvm-mingw", "bin", "clang.exe")
    r = subprocess.run([clang, "-shared", "-O1", "-std=gnu11", "-march=x86-64-v3", "-static",
                        "-Wno-parentheses-equality", "-I", os.path.join(ROOT, "runtime", "include"), src,
                        os.path.join(ROOT, "runtime", "src", "rc_support.c"), "-o", dll], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-4000:])
        return 1

    # memory: scratch + module images at their guest addresses, in this process and in Unicorn
    k32 = ctypes.windll.kernel32
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    images = {}
    for mod, base in BASES.items():
        img = bytes(nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", mod))["image"])
        size = (len(img) + 0xFFFF) & ~0xFFFF
        p = k32.VirtualAlloc(base, size, 0x3000, 0x04)
        assert p == base, "could not map %s at 0x%x" % (mod, base)
        ctypes.memmove(base, img, len(img))
        images[mod] = (base, size, img)
    assert k32.VirtualAlloc(SCRATCH, SCRATCH_SIZE, 0x3000, 0x04) == SCRATCH

    lib = ctypes.CDLL(dll)
    count = ctypes.c_int.in_dll(lib, "semtest_count").value
    funcs = (ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(Ctx)) * count).in_dll(lib, "semtest_funcs")

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for mod, (base, size, img) in images.items():
        uc.mem_map(base, size, UC_PROT_ALL)
        uc.mem_write(base, img)
    uc.mem_map(SCRATCH, SCRATCH_SIZE, UC_PROT_ALL)
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
    XREGS = [getattr(A, "UC_ARM64_REG_X%d" % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
    VREGS = [getattr(A, "UC_ARM64_REG_V%d" % i) for i in range(32)]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    rng = random.Random(a.seed)
    ctx = Ctx()
    fails = collections.defaultdict(list)
    nanonly = collections.Counter()
    ran = 0
    window = 0x11000
    for t, (ins, key) in enumerate(zip(tests, keys)):
        is_mem = ins.op in MEMOPS
        bases, idx = mem_regs(ins) if is_mem else ([], [])
        for case in range(a.cases):
            xs = [rand_x(rng) for _ in range(31)]
            sp = MID - rng.randrange(0, 64) * 16
            for b in bases:
                if isinstance(b, int):
                    xs[b] = MID + rng.randrange(-64, 64) * 8
            for ix in idx:
                if isinstance(ix, int):
                    xs[ix] = rng.randrange(0, 32)
            vs = [rand_v(rng) for _ in range(32)]
            nzcv = rng.randrange(16) << 28
            mem = bytes(rng.getrandbits(8) for _ in range(256)) * (2 * window // 256)
            # unicorn
            for i in range(31):
                uc.reg_write(XREGS[i], xs[i])
            uc.reg_write(A.UC_ARM64_REG_SP, sp)
            for i in range(32):
                uc.reg_write(VREGS[i], vs[i])
            uc.reg_write(A.UC_ARM64_REG_NZCV, nzcv)
            uc.reg_write(A.UC_ARM64_REG_FPCR, 0)
            uc.reg_write(A.UC_ARM64_REG_FPSR, 0)
            uc.mem_write(MID - window, mem)
            try:
                uc.emu_start(ins.pc, ins.pc + 4, count=1)
                u_err = None
            except UcError as e:
                u_err = str(e)
            u_x = [uc.reg_read(XREGS[i]) for i in range(31)]
            u_sp = uc.reg_read(A.UC_ARM64_REG_SP)
            u_v = [uc.reg_read(VREGS[i]) for i in range(32)]
            u_nzcv = uc.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000
            u_pc = uc.reg_read(A.UC_ARM64_REG_PC)
            u_mem = bytes(uc.mem_read(MID - window, 2 * window))
            # host
            for i in range(31):
                ctx.x[i] = xs[i]
            ctx.sp = sp
            for i in range(32):
                ctx.v[i].d[0] = vs[i] & 0xFFFFFFFFFFFFFFFF
                ctx.v[i].d[1] = vs[i] >> 64
            ctx.nzcv = nzcv
            ctx.fpcr = ctx.fpsr = 0
            ctx.ex_addr = 0xFFFFFFFFFFFFFFFF
            ctypes.memmove(MID - window, mem, len(mem))
            taken = funcs[t](ctypes.byref(ctx))
            h_x = [ctx.x[i] for i in range(31)]
            h_v = [ctx.v[i].d[0] | (ctx.v[i].d[1] << 64) for i in range(32)]
            h_mem = ctypes.string_at(MID - window, 2 * window)
            ran += 1
            diffs = []
            if u_err:
                diffs.append("unicorn error " + u_err)
            for i in range(31):
                if u_x[i] != h_x[i]:
                    diffs.append("x%d u=%x h=%x" % (i, u_x[i], h_x[i]))
            if u_sp != ctx.sp:
                diffs.append("sp u=%x h=%x" % (u_sp, ctx.sp))
            only_nan = True
            for i in range(32):
                if u_v[i] != h_v[i]:
                    if not nan_equiv(u_v[i], h_v[i]):
                        only_nan = False
                    diffs.append("v%d u=%032x h=%032x" % (i, u_v[i], h_v[i]))
            if u_nzcv != (ctx.nzcv & 0xF0000000):
                only_nan = False
                diffs.append("nzcv u=%x h=%x" % (u_nzcv >> 28, ctx.nzcv >> 28))
            if ins.op in BRANCHES:
                u_taken = int(u_pc == ins.target)
                if u_taken != taken:
                    only_nan = False
                    diffs.append("taken u=%d h=%d" % (u_taken, taken))
            if u_mem != h_mem:
                only_nan = False
                k = next(j for j in range(len(u_mem)) if u_mem[j] != h_mem[j])
                diffs.append("mem @%x u=%s h=%s" % (MID - window + k, u_mem[k:k + 8].hex(), h_mem[k:k + 8].hex()))
            if any(not d.startswith("v") for d in diffs):
                only_nan = False
            if diffs:
                if only_nan:
                    nanonly[key] += 1
                    continue
                if len(fails[key]) < 3:
                    cs = next(md.disasm_lite(struct.pack("<I", ins.word), ins.pc), None)
                    fails[key].append({"insn": "%08x %s %s" % (ins.word, cs[2], cs[3]) if cs else "%08x" % ins.word,
                                       "pc": "0x%x" % ins.pc, "diffs": diffs[:6],
                                       "inputs": {"nzcv": nzcv >> 28, **{("x%d" % i): "%x" % xs[i] for i in range(31)
                                                                         if ("x%d " % i) in " ".join(diffs) + " "}}})
        if t % 500 == 0:
            print("  %d/%d instructions, %d failing patterns" % (t, len(tests), len(fails)), flush=True)
    report = {"patterns": len(groups), "instructions": len(tests), "cases": ran,
              "failing_patterns": len(fails), "nan_payload_only": {" ".join(k): v for k, v in nanonly.items()},
              "fails": {" ".join(k): v for k, v in fails.items()}}
    json.dump(report, open(os.path.join(out_dir, "report.json"), "w"), indent=1)
    print("%d cases run: %d of %d patterns fail, %d patterns differ only in NaN payload/sign" % (
        ran, len(fails), len(groups), len(nanonly)))
    for k, v in list(fails.items())[:40]:
        print(" FAIL", " ".join(k))
        for e in v[:1]:
            print("     ", e["insn"], "|", "; ".join(e["diffs"][:3]))
    return 1 if fails else 0


def nan_equiv(u, h):
    """True if every differing 32/64-bit lane is a NaN in both."""
    for bits in (32, 64):
        ok = True
        for i in range(128 // bits):
            a_ = (u >> (bits * i)) & ((1 << bits) - 1)
            b_ = (h >> (bits * i)) & ((1 << bits) - 1)
            if a_ == b_:
                continue
            if bits == 32:
                na = (a_ & 0x7F800000) == 0x7F800000 and a_ & 0x7FFFFF
                nb = (b_ & 0x7F800000) == 0x7F800000 and b_ & 0x7FFFFF
            else:
                na = (a_ >> 52 & 0x7FF) == 0x7FF and a_ & 0xFFFFFFFFFFFFF
                nb = (b_ >> 52 & 0x7FF) == 0x7FF and b_ & 0xFFFFFFFFFFFFF
            if not (na and nb):
                ok = False
                break
        if ok:
            return True
    return False


if __name__ == "__main__":
    sys.exit(main())
