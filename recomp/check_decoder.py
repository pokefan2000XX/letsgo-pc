#!/usr/bin/env python3
"""Decode every instruction of a module with a64.py and cross-check the result
against capstone's independent disassembly (mnemonic level).

Usage: python -P recomp/check_decoder.py main sdk
"""
import collections
import csv
import os
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import a64  # noqa: E402
import nso2elf  # noqa: E402

BASES = {"main": 0x7100000000, "sdk": 0x7180000000}
CONDN = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]

EXPECT = {
    "addsub_imm": {"add", "adds", "sub", "subs", "cmp", "cmn", "mov"},
    "logic_imm": {"and", "orr", "eor", "ands", "tst", "mov"},
    "movw": {"movz", "movn", "movk", "mov"},
    "bfm": {"sbfm", "bfm", "ubfm", "asr", "lsl", "lsr", "sxtb", "sxth", "sxtw", "uxtb", "uxth", "sbfiz", "sbfx",
            "ubfiz", "ubfx", "bfi", "bfxil", "bfc"},
    "extr": {"extr", "ror"},
    "logic_reg": {"and", "bic", "orr", "orn", "eor", "eon", "ands", "bics", "mov", "mvn", "tst"},
    "addsub_reg": {"add", "adds", "sub", "subs", "cmp", "cmn", "neg", "negs", "mov"},
    "addsub_ext": {"add", "adds", "sub", "subs", "cmp", "cmn", "mov"},
    "adc": {"adc", "adcs", "sbc", "sbcs", "ngc", "ngcs"},
    "ccmp": {"ccmp", "ccmn"},
    "csel": {"csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg"},
    "dp2": {"udiv", "sdiv", "lsl", "lsr", "asr", "ror", "lslv", "lsrv", "asrv", "rorv"},
    "crc32": {"crc32b", "crc32h", "crc32w", "crc32x", "crc32cb", "crc32ch", "crc32cw", "crc32cx"},
    "dp1": {"rbit", "rev16", "rev32", "rev", "clz", "cls", "rev64"},
    "dp3": {"madd", "msub", "mul", "mneg", "smaddl", "smsubl", "smull", "smnegl", "umaddl", "umsubl", "umull",
            "umnegl", "smulh", "umulh"},
    "ldxr": {"ldxr", "ldxrb", "ldxrh", "ldaxr", "ldaxrb", "ldaxrh"},
    "stxr": {"stxr", "stxrb", "stxrh", "stlxr", "stlxrb", "stlxrh"},
    "ldxp": {"ldxp", "ldaxp"}, "stxp": {"stxp", "stlxp"},
    "ldar": {"ldar", "ldarb", "ldarh", "ldlar", "ldlarb", "ldlarh"},
    "stlr": {"stlr", "stlrb", "stlrh", "stllr", "stllrb", "stllrh"},
    "vldst_multi": {"ld1", "ld2", "ld3", "ld4", "st1", "st2", "st3", "st4"},
    "vldst_single": {"ld1", "ld2", "ld3", "ld4", "st1", "st2", "st3", "st4", "ld1r", "ld2r", "ld3r", "ld4r"},
    "ldr_lit": {"ldr", "ldrsw"}, "prfm": {"prfm", "prfum"},
    "ldstp": {"ldp", "stp", "ldpsw", "ldnp", "stnp"},
    "ldst": {"ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh", "ldrsw", "ldur", "stur", "ldurb", "sturb",
             "ldurh", "sturh", "ldursb", "ldursh", "ldursw", "ldtr", "sttr", "ldtrb", "sttrb", "ldtrh", "sttrh",
             "ldtrsb", "ldtrsh", "ldtrsw"},
    "hint": {"nop", "yield", "wfe", "wfi", "sev", "sevl", "hint", "esb", "psb", "csdb", "bti"},
    "sys": {"dc", "ic", "at", "tlbi", "sys"}, "sysl": {"sysl"}, "msr_imm": {"msr"},
    "crypto_aes": {"aese", "aesd", "aesmc", "aesimc"},
    "crypto_sha3": {"sha1c", "sha1p", "sha1m", "sha1su0", "sha256h", "sha256h2", "sha256su1"},
    "crypto_sha2": {"sha1h", "sha1su1", "sha256su0"},
    "fcmp": {"fcmp", "fcmpe"}, "fccmp": {"fccmp", "fccmpe"}, "fcsel": {"fcsel"}, "fmov_imm": {"fmov"},
    "fp_dp3": {"fmadd", "fmsub", "fnmadd", "fnmsub"},
    "fp_dp2": {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"},
    "fp_dp1": {"fmov", "fabs", "fneg", "fsqrt", "fcvt", "frintn", "frintp", "frintm", "frintz", "frinta", "frintx",
               "frinti"},
    "fcvt_int": {"fcvtns", "fcvtnu", "scvtf", "ucvtf", "fcvtas", "fcvtau", "fmov", "fcvtps", "fcvtpu", "fcvtms",
                 "fcvtmu", "fcvtzs", "fcvtzu"},
    "fcvt_fixed": {"fcvtzs", "fcvtzu", "scvtf", "ucvtf"},
}


def expected(ins):
    op = ins.op
    if op in ("b", "bl", "cbz", "cbnz", "tbz", "tbnz", "br", "blr", "ret", "svc", "brk", "hlt", "clrex", "dsb",
              "dmb", "isb", "adr", "adrp", "mrs", "msr"):
        return {op}
    if op == "bcond":
        return {"b." + CONDN[ins.cond]}
    if op == "udf":
        return {"udf", None}
    return EXPECT.get(op)


def main():
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for mod in sys.argv[1:]:
        base = BASES[mod]
        nso = nso2elf.load_nso(os.path.join(ROOT, "orig", "exefs", mod))
        img = nso["image"]
        funcs = [(int(r["Address"], 16) - base, int(r["Size"])) for r in
                 csv.DictReader(open(os.path.join(ROOT, "data", "%s_functions.csv" % mod)))]
        ops = collections.Counter()
        unknown = collections.Counter()
        unknown_ex = {}
        mismatch = collections.Counter()
        mismatch_ex = {}
        total = 0
        for f0, fsz in funcs:
            for a in range(f0, f0 + fsz, 4):
                w = struct.unpack_from("<I", img, a)[0]
                ins = a64.decode(w, base + a)
                total += 1
                ops[ins.op] += 1
                cs = next(md.disasm_lite(struct.pack("<I", w), base + a), None)
                mn = cs[2] if cs else None
                if ins.op == "unknown":
                    unknown[mn] += 1
                    unknown_ex.setdefault(mn, "%08x  %s %s" % (w, mn, cs[3] if cs else ""))
                    continue
                exp = expected(ins)
                if exp is not None and mn not in exp:
                    k = (ins.op, mn)
                    mismatch[k] += 1
                    mismatch_ex.setdefault(k, "0x%x %08x  capstone: %s %s  ours: %r" % (
                        base + a, w, mn, cs[3] if cs else "", ins))
        print("== %s: %d instructions, %d unknown, %d mismatches" % (mod, total, sum(unknown.values()), sum(mismatch.values())))
        for mn, n in unknown.most_common(40):
            print("   unknown %6d  %s" % (n, unknown_ex[mn]))
        for k, n in mismatch.most_common(40):
            print("   mismatch %6d  %s" % (n, mismatch_ex[k]))
        print("   ops:", ", ".join("%s %d" % kv for kv in ops.most_common()))


if __name__ == "__main__":
    main()
