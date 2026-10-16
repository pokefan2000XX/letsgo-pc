"""C emitter: one decoded A64 instruction -> C statements (see runtime/include/rc.h).

FuncCtx tells the emitter how branches leave or stay in the current function.
Instructions without implemented semantics emit an rc_trap() so they fail
loudly when executed instead of silently doing the wrong thing.
"""
import struct

import a64

CONDS = {0: "Z", 1: "!Z", 2: "C", 3: "!C", 4: "N", 5: "!N", 6: "V", 7: "!V", 8: "(C && !Z)", 9: "(!C || Z)",
         10: "(N == V)", 11: "(N != V)", 12: "(!Z && N == V)", 13: "(Z || N != V)", 14: "1", 15: "1"}


class FuncCtx:
    """What the emitter needs to know about the function being translated."""

    def __init__(self, start, end, labels, func_starts, jump_tables, name_of):
        self.start, self.end = start, end
        self.labels = labels                  # intra-function branch targets
        self.func_starts = func_starts        # set of all function entry addresses
        self.jump_tables = jump_tables        # br pc -> (index_reg, [targets]) or None
        self.name_of = name_of                # addr -> C function name
        self.bad_branches = []                # (pc, target) leaving the function into a non-entry
        self.callees = set()                  # functions called/tail-called directly

    def inside(self, a):
        return self.start <= a < self.end


def lbl(a):
    return "L_%x" % a


def r64(r):
    if r == "zr":
        return "0"
    if r == "sp":
        return "SP"
    return "X(%d)" % r


def r32(r):
    if r == "zr":
        return "0"
    if r == "sp":
        return "(uint32_t)SP"
    return "(uint32_t)X(%d)" % r


def rd(r, sf):
    return r64(r) if sf else r32(r)


def wr(r, expr, sf):
    """Assignment of `expr` to register r (32-bit writes zero-extend)."""
    if r == "zr":
        return "(void)(%s);" % expr
    tgt = "SP" if r == "sp" else "X(%d)" % r
    if sf:
        return "%s = (uint64_t)(%s);" % (tgt, expr)
    return "%s = W32(%s);" % (tgt, expr)


def hexc(v, bits=64):
    v &= (1 << bits) - 1
    return "0x%xull" % v if bits == 64 else "0x%xu" % v


def shifted(r, sf, shift, amount):
    if r == "zr":
        return "0"
    if sf:
        v = r64(r)
        if amount == 0:
            return v
        return {0: "(%s << %d)" % (v, amount), 1: "(%s >> %d)" % (v, amount),
                2: "(uint64_t)((int64_t)%s >> %d)" % (v, amount), 3: "ror64(%s, %d)" % (v, amount)}[shift]
    v = r32(r)
    if amount == 0:
        return v
    return {0: "(uint32_t)(%s << %d)" % (v, amount), 1: "(%s >> %d)" % (v, amount),
            2: "(uint32_t)((int32_t)%s >> %d)" % (v, amount), 3: "ror32(%s, %d)" % (v, amount)}[shift]


def extended(r, option, amount):
    """Extended-register operand (64-bit expression)."""
    v = r64(r)
    e = {0: "(uint64_t)(uint8_t)%s", 1: "(uint64_t)(uint16_t)%s", 2: "(uint64_t)(uint32_t)%s", 3: "%s",
         4: "(uint64_t)(int64_t)(int8_t)%s", 5: "(uint64_t)(int64_t)(int16_t)%s",
         6: "(uint64_t)(int64_t)(int32_t)%s", 7: "%s"}[option] % v
    return "(%s << %d)" % (e, amount) if amount else e


def bit_masks(n, imms, immr, datasize):
    """DecodeBitMasks(immediate=False): (wmask, tmask)."""
    combined = (n << 6) | (~imms & 0x3F)
    length = combined.bit_length() - 1
    levels = (1 << length) - 1
    s, r = imms & levels, immr & levels
    d = (s - r) & levels
    esize = 1 << length
    welem = (1 << (s + 1)) - 1
    telem = (1 << (d + 1)) - 1
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1) if r else welem
    w = t = 0
    for i in range(datasize // esize):
        w |= welem << (i * esize)
        t |= telem << (i * esize)
    return w, t


def trap(ins, why):
    return ["rc_trap(c, 0x%xull, 0x%08xu, \"%s\");" % (ins.pc, ins.word, why)]


# ------------------------------------------------------------------------ integer ---

def e_addsub_imm(i, fc):
    sf = i.sf
    a = rd(i.rn, sf)
    if i.s:
        f = ("SUB64F" if i.sub else "ADD64F") if sf else ("SUB32F" if i.sub else "ADD32F")
        return [wr(i.rd, "%s(%s, %s)" % (f, a, hexc(i.imm, 64 if sf else 32)), sf)]
    op = "-" if i.sub else "+"
    if i.imm == 0:
        return [wr(i.rd, a, sf)]
    return [wr(i.rd, "%s %s %s" % (a, op, hexc(i.imm, 64 if sf else 32)), sf)]


def e_logic_imm(i, fc):
    sf = i.sf
    a = rd(i.rn, sf)
    imm = hexc(i.imm, 64 if sf else 32)
    op = {0: "&", 1: "|", 2: "^", 3: "&"}[i.opc]
    expr = "%s %s %s" % (a, op, imm) if a != "0" else (imm if i.opc in (1, 2) else "0")
    if i.opc == 3:
        return [wr(i.rd, ("LOGF64(%s)" if sf else "LOGF32(%s)") % expr, sf)]
    return [wr(i.rd, expr, sf)]


def e_movw(i, fc):
    sf, sh = i.sf, i.shift
    bits = 64 if sf else 32
    v = i.imm16 << sh
    if i.opc == 0:
        return [wr(i.rd, hexc(~v, bits), sf)]
    if i.opc == 2:
        return [wr(i.rd, hexc(v, bits), sf)]
    if i.rd == "zr":
        return []
    mask = hexc(~(0xFFFF << sh), bits)
    return [wr(i.rd, "(%s & %s) | %s" % (rd(i.rd, sf), mask, hexc(v, bits)), sf)]


def e_bfm(i, fc):
    sf = i.sf
    bits = 64 if sf else 32
    wmask, tmask = bit_masks(sf, i.imms, i.immr, bits)
    R, S = i.immr, i.imms
    src = rd(i.rn, sf)
    ror = ("ror64(%s, %d)" if sf else "ror32(%s, %d)") % (src, R) if R else src
    if i.opc == 2:      # UBFM
        return [wr(i.rd, "%s & %s" % (ror, hexc(wmask & tmask, bits)), sf)]
    if i.opc == 0:      # SBFM
        top = ("(uint64_t)((int64_t)(%s << %d) >> 63)" % (src, 63 - S)) if sf else \
              ("(uint32_t)((int32_t)(%s << %d) >> 31)" % (src, 31 - S))
        return [wr(i.rd, "(%s & %s) | (%s & %s)" % (top, hexc(~tmask, bits), ror, hexc(wmask & tmask, bits)), sf)]
    # BFM
    dst = rd(i.rd, sf)
    bot = "((%s & %s) | (%s & %s))" % (dst, hexc(~wmask, bits), ror, hexc(wmask, bits))
    return [wr(i.rd, "(%s & %s) | (%s & %s)" % (dst, hexc(~tmask, bits), bot, hexc(tmask, bits)), sf)]


def e_extr(i, fc):
    if i.sf:
        if i.lsb == 0:
            return [wr(i.rd, r64(i.rm), 1)]
        return [wr(i.rd, "(%s >> %d) | (%s << %d)" % (r64(i.rm), i.lsb, r64(i.rn), 64 - i.lsb), 1)]
    if i.lsb == 0:
        return [wr(i.rd, r32(i.rm), 0)]
    return [wr(i.rd, "(%s >> %d) | (uint32_t)(%s << %d)" % (r32(i.rm), i.lsb, r32(i.rn), 32 - i.lsb), 0)]


def e_logic_reg(i, fc):
    sf = i.sf
    a = rd(i.rn, sf)
    b = shifted(i.rm, sf, i.shift, i.amount)
    if i.neg:
        b = "~%s" % b if sf else "(uint32_t)~%s" % b
    op = {0: "&", 1: "|", 2: "^", 3: "&"}[i.opc]
    expr = "%s %s %s" % (a, op, b)
    if i.opc == 3:
        return [wr(i.rd, ("LOGF64(%s)" if sf else "LOGF32(%s)") % expr, sf)]
    return [wr(i.rd, expr, sf)]


def e_addsub_reg(i, fc):
    sf = i.sf
    a, b = rd(i.rn, sf), shifted(i.rm, sf, i.shift, i.amount)
    if i.s:
        f = ("SUB64F" if i.sub else "ADD64F") if sf else ("SUB32F" if i.sub else "ADD32F")
        return [wr(i.rd, "%s(%s, %s)" % (f, a, b), sf)]
    return [wr(i.rd, "%s %s %s" % (a, "-" if i.sub else "+", b), sf)]


def e_addsub_ext(i, fc):
    sf = i.sf
    a = r64(i.rn)
    b = extended(i.rm, i.option, i.amount)
    if i.s:
        f = ("SUB64F" if i.sub else "ADD64F") if sf else ("SUB32F" if i.sub else "ADD32F")
        return [wr(i.rd, "%s(%s, %s)" % (f, a, b), sf)]
    return [wr(i.rd, "%s %s %s" % (a, "-" if i.sub else "+", b), sf)]


def e_adc(i, fc):
    sf = i.sf
    a, b = rd(i.rn, sf), rd(i.rm, sf)
    if i.s:
        f = ("SBC64F" if i.sub else "ADC64F") if sf else ("SBC32F" if i.sub else "ADC32F")
        return [wr(i.rd, "%s(%s, %s)" % (f, a, b), sf)]
    if i.sub:
        return [wr(i.rd, "%s + ~%s + C" % (a, b) if sf else "%s + (uint32_t)~%s + C" % (a, b), sf)]
    return [wr(i.rd, "%s + %s + C" % (a, b), sf)]


def e_ccmp(i, fc):
    sf = i.sf
    a = rd(i.rn, sf)
    b = hexc(i.rm, 64 if sf else 32) if i.is_imm else rd(i.rm, sf)
    f = ("SUB64F" if i.sub else "ADD64F") if sf else ("SUB32F" if i.sub else "ADD32F")
    return ["if (%s) (void)%s(%s, %s); else SETNZCV(%d);" % (CONDS[i.cond], f, a, b, i.nzcv)]


def e_csel(i, fc):
    sf = i.sf
    n, m = rd(i.rn, sf), rd(i.rm, sf)
    alt = {(0, 0): m, (0, 1): "%s + 1" % m, (1, 0): "~%s" % m, (1, 1): "(%s)0 - %s" % ("uint64_t" if sf else "uint32_t", m)}[(i.inv, i.inc)]
    return [wr(i.rd, "%s ? %s : (%s)" % (CONDS[i.cond], n, alt), sf)]


def e_dp2(i, fc):
    sf = i.sf
    a, b = rd(i.rn, sf), rd(i.rm, sf)
    k = i.kind
    if k == "udiv":
        return [wr(i.rd, ("udiv64(%s, %s)" if sf else "udiv32(%s, %s)") % (a, b), sf)]
    if k == "sdiv":
        return [wr(i.rd, ("sdiv64((int64_t)%s, (int64_t)%s)" if sf else "sdiv32((int32_t)%s, (int32_t)%s)") % (a, b), sf)]
    m = 63 if sf else 31
    if k == "lslv":
        e = "%s << (%s & %d)" % (a, b, m)
        return [wr(i.rd, e if sf else "(uint32_t)(%s)" % e, sf)]
    if k == "lsrv":
        return [wr(i.rd, "%s >> (%s & %d)" % (a, b, m), sf)]
    if k == "asrv":
        return [wr(i.rd, ("(uint64_t)((int64_t)%s >> (%s & 63))" if sf else "(uint32_t)((int32_t)%s >> (%s & 31))") % (a, b), sf)]
    return [wr(i.rd, ("ror64(%s, (unsigned)%s)" if sf else "ror32(%s, (unsigned)%s)") % (a, b), sf)]


def e_crc32(i, fc):
    return [wr(i.rd, "rc_crc32(%s, %s, %d, %d)" % (r32(i.rn), r64(i.rm), 1 << i.size, i.c), 0)]


def e_dp1(i, fc):
    sf, k = i.sf, i.kind
    a = rd(i.rn, sf)
    if k == "rbit":
        return [wr(i.rd, ("__builtin_bitreverse64(%s)" if sf else "__builtin_bitreverse32(%s)") % a, sf)]
    if k == "rev16":
        return [wr(i.rd, ("rev16_64(%s)" if sf else "rev16_32(%s)") % a, sf)]
    if k == "rev32":
        return [wr(i.rd, "rev32_64(%s)" % a, 1)]
    if k == "rev":
        return [wr(i.rd, ("__builtin_bswap64(%s)" if sf else "__builtin_bswap32(%s)") % a, sf)]
    if k == "clz":
        return [wr(i.rd, ("clz64(%s)" if sf else "clz32(%s)") % a, sf)]
    return [wr(i.rd, ("cls64(%s)" if sf else "cls32(%s)") % a, sf)]


def e_dp3(i, fc):
    sf, k = i.sf, i.kind
    if k in ("madd", "msub"):
        n, m, ra = rd(i.rn, sf), rd(i.rm, sf), rd(i.ra, sf)
        prod = "%s * %s" % (n, m)
        e = ("%s + %s" if k == "madd" else "%s - %s") % (ra, prod)
        return [wr(i.rd, e if sf else "(uint32_t)(%s)" % e, sf)]
    if k in ("smaddl", "smsubl"):
        prod = "(uint64_t)((int64_t)(int32_t)%s * (int64_t)(int32_t)%s)" % (r64(i.rn), r64(i.rm))
        return [wr(i.rd, ("%s + %s" if k == "smaddl" else "%s - %s") % (r64(i.ra), prod), 1)]
    if k in ("umaddl", "umsubl"):
        prod = "((uint64_t)(uint32_t)%s * (uint64_t)(uint32_t)%s)" % (r64(i.rn), r64(i.rm))
        return [wr(i.rd, ("%s + %s" if k == "umaddl" else "%s - %s") % (r64(i.ra), prod), 1)]
    if k == "smulh":
        return [wr(i.rd, "smulh64((int64_t)%s, (int64_t)%s)" % (r64(i.rn), r64(i.rm)), 1)]
    return [wr(i.rd, "umulh64(%s, %s)" % (r64(i.rn), r64(i.rm)), 1)]


# ------------------------------------------------------------------- memory ---

MEM = {1: "M8", 2: "M16", 4: "M32", 8: "M64"}
TYP = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}


def addr_and_wb(i):
    """(pre statements, address expression, post statements) for ldst/ldstp."""
    base = r64(i.rn)
    if i.mode == "reg":
        return [], "%s + %s" % (base, extended(i.rm, i.option, i.amount)), []
    if i.mode == "off":
        return [], ("%s + %s" % (base, i.imm) if i.imm else base) if i.imm >= 0 else "%s - %d" % (base, -i.imm), []
    tgt = "SP" if i.rn == "sp" else base
    if i.mode == "pre":
        return [], "%s %s %d" % (base, "+" if i.imm >= 0 else "-", abs(i.imm)), ["%s = a_;" % tgt]
    return [], base, ["%s = a_ %s %d;" % (tgt, "+" if i.imm >= 0 else "-", abs(i.imm))]


def load_expr(kind, n, ext, a):
    if kind == "ld":
        return "%s(%s)" % (MEM[n], a)
    if ext == 64:
        return {1: "LDS8", 2: "LDS16", 4: "LDS32"}[n] + "(%s)" % a
    return {1: "LDS8W", 2: "LDS16W"}[n] + "(%s)" % a


def e_ldst(i, fc):
    pre, a, post = addr_and_wb(i)
    out = ["{ uint64_t a_ = %s;" % a]
    if i.fp:
        if i.kind == "ld":
            out.append("VR(%d) = vld(a_, %d);" % (i.rt, i.nbytes))
        else:
            out.append("vst(a_, VR(%d), %d);" % (i.rt, i.nbytes))
    elif i.kind == "st":
        val = r64(i.rt)
        out.append("%s(a_) = (%s)%s;" % (MEM[i.nbytes], TYP[i.nbytes], val))
    else:
        e = load_expr(i.kind, i.nbytes, i.ext, "a_")
        if i.rt == "zr":
            out.append("(void)%s;" % e)
        else:
            out.append("X(%d) = %s;" % (i.rt, e))
    out += post
    out.append("}")
    return [" ".join(out)]


def e_ldstp(i, fc):
    pre, a, post = addr_and_wb(i)
    n = i.nbytes
    out = ["{ uint64_t a_ = %s;" % a]
    if i.fp:
        if i.load:
            out.append("V128 t0_ = vld(a_, %d), t1_ = vld(a_ + %d, %d); VR(%d) = t0_; VR(%d) = t1_;" % (n, n, n, i.rt, i.rt2))
        else:
            out.append("vst(a_, VR(%d), %d); vst(a_ + %d, VR(%d), %d);" % (i.rt, n, n, i.rt2, n))
    elif i.load:
        e1 = (load_expr("lds", 4, 64, "a_") if i.signed else "%s(a_)" % MEM[n])
        e2 = (load_expr("lds", 4, 64, "a_ + 4") if i.signed else "%s(a_ + %d)" % (MEM[n], n))
        out.append("uint64_t t0_ = %s, t1_ = %s;" % (e1, e2))
        if i.rt != "zr":
            out.append("X(%d) = t0_;" % i.rt)
        if i.rt2 != "zr":
            out.append("X(%d) = t1_;" % i.rt2)
    else:
        out.append("%s(a_) = (%s)%s; %s(a_ + %d) = (%s)%s;" % (MEM[n], TYP[n], r64(i.rt), MEM[n], n, TYP[n], r64(i.rt2)))
    out += post
    out.append("}")
    return [" ".join(out)]


def e_ldr_lit(i, fc):
    a = "0x%xull" % i.addr
    if i.fp:
        return ["VR(%d) = vld(%s, %d);" % (i.rt, a, i.nbytes)]
    if i.rt == "zr":
        return []
    if i.signed:
        return ["X(%d) = LDS32(%s);" % (i.rt, a)]
    return ["X(%d) = %s(%s);" % (i.rt, MEM[i.nbytes], a)]


def e_ldxr(i, fc):
    T = TYP[i.size]
    s = "LDX(%s, %s);" % (T, r64(i.rn))
    return [s if i.rt == "zr" else "X(%d) = (%s);" % (i.rt, s.rstrip(";"))]


def e_stxr(i, fc):
    f = {1: "stx8", 2: "stx16", 4: "stx32", 8: "stx64"}[i.size]
    e = "%s(c, %s, (%s)%s)" % (f, r64(i.rn), TYP[i.size], r64(i.rt))
    return [wr(i.rs, e, 0)]


def e_ldar(i, fc):
    e = "LDA(%s, %s)" % (TYP[i.size], r64(i.rn))
    return ["(void)%s;" % e if i.rt == "zr" else "X(%d) = %s;" % (i.rt, e)]


def e_stlr(i, fc):
    return ["STL(%s, %s, %s);" % (TYP[i.size], r64(i.rn), r64(i.rt))]


def e_vldst_multi(i, fc):
    esz = 1 << i.size
    nlanes = (16 if i.q else 8) // esz
    T = TYP[esz]
    lane = {1: "b", 2: "h", 4: "s", 8: "d"}[esz]
    out = ["{ uint64_t a_ = %s; uint64_t o_ = 0;" % r64(i.rn)]
    regs = [(i.rt + k) % 32 for k in range(i.nregs)]
    if i.selem == 1:
        # LD1/ST1 with several registers: whole registers, consecutive
        for r in regs:
            if i.load:
                out.append("VR(%d) = vld(a_ + o_, %d); o_ += %d;" % (r, 16 if i.q else 8, 16 if i.q else 8))
            else:
                out.append("vst(a_ + o_, VR(%d), %d); o_ += %d;" % (r, 16 if i.q else 8, 16 if i.q else 8))
    else:
        if i.load:
            out.append(" ".join("V128 t%d_ = {0};" % k for k in range(i.selem)))
        out.append("for (int e_ = 0; e_ < %d; e_++) {" % nlanes)
        for k in range(i.selem):
            if i.load:
                out.append("t%d_.%s[e_] = M%d(a_ + o_); o_ += %d;" % (k, lane, esz * 8, esz))
            else:
                out.append("M%d(a_ + o_) = VR(%d).%s[e_]; o_ += %d;" % (esz * 8, regs[k], lane, esz))
        out.append("}")
        if i.load:
            out.append(" ".join("VR(%d) = t%d_;" % (regs[k], k) for k in range(i.selem)))
    if i.post:
        total = (16 if i.q else 8) * i.nregs
        tgt = "SP" if i.rn == "sp" else r64(i.rn)
        out.append("%s = a_ + %s;" % (tgt, str(total) if i.rm == "imm" else r64(a64.zr(i.rm))))
    out.append("}")
    return [" ".join(out)]


def e_vldst_single(i, fc):
    esz = 1 << i.scale
    lane = {1: "b", 2: "h", 4: "s", 8: "d"}[esz]
    out = ["{ uint64_t a_ = %s;" % r64(i.rn)]
    for k in range(i.selem):
        r = (i.rt + k) % 32
        off = k * esz
        if i.replicate:
            out.append("VR(%d) = v_dup(M%d(a_ + %d), %d, %d);" % (r, esz * 8, off, i.scale, i.q))
        elif i.load:
            out.append("VR(%d).%s[%d] = M%d(a_ + %d);" % (r, lane, i.index, esz * 8, off))
        else:
            out.append("M%d(a_ + %d) = VR(%d).%s[%d];" % (esz * 8, off, r, lane, i.index))
    if i.post:
        tgt = "SP" if i.rn == "sp" else r64(i.rn)
        out.append("%s = a_ + %s;" % (tgt, str(esz * i.selem) if i.rm == "imm" else r64(a64.zr(i.rm))))
    out.append("}")
    return [" ".join(out)]


# ------------------------------------------------------------------ control ---

def branch_to(fc, pc, target):
    if fc.inside(target):
        return "goto %s;" % lbl(target)
    if target in fc.func_starts:
        fc.callees.add(target)
        return "TAIL(%s);" % fc.name_of(target)
    fc.bad_branches.append((pc, target))
    return "TAILI(0x%xull);" % target


def e_b(i, fc):
    return [branch_to(fc, i.pc, i.target)]


def e_bcond(i, fc):
    return ["if (%s) %s" % (CONDS[i.cond], branch_to(fc, i.pc, i.target))]


def e_cbz(i, fc):
    v = r64(i.rt) if i.sf else r32(i.rt)
    return ["if (%s %s 0) %s" % (v, "==" if i.op == "cbz" else "!=", branch_to(fc, i.pc, i.target))]


def e_tbz(i, fc):
    v = r64(i.rt)
    return ["if (((%s >> %d) & 1) %s 0) %s" % (v, i.bit, "==" if i.op == "tbz" else "!=", branch_to(fc, i.pc, i.target))]


def e_bl(i, fc):
    if i.target in fc.func_starts:          # includes recursion into the function's own entry
        fc.callees.add(i.target)
        return ["CALL(%s, 0x%xull);" % (fc.name_of(i.target), i.pc + 4)]
    if fc.inside(i.target):                 # "bl next; .word x; ldr [x30]": get-PC idiom
        return ["X(30) = 0x%xull; goto %s;" % (i.pc + 4, lbl(i.target))]
    fc.bad_branches.append((i.pc, i.target))
    return ["CALLI(0x%xull, 0x%xull);" % (i.target, i.pc + 4)]


def e_blr(i, fc):
    return ["CALLI(%s, 0x%xull);" % (r64(i.rn), i.pc + 4)]


def e_br(i, fc):
    jt = fc.jump_tables.get(i.pc)
    if jt:
        reg, targets = jt
        out = ["switch (%s) {" % r64(reg)]
        for t in sorted(set(targets)):
            out.append("case 0x%xull: goto %s;" % (t, lbl(t)))
        out.append("default: TAILI(%s); }" % r64(reg))
        return [" ".join(out)]
    if getattr(fc, "goto_targets", None):
        # computed goto: the target may be one of this function's address-taken labels
        out = ["switch (%s) {" % r64(i.rn)]
        for t in fc.goto_targets:
            out.append("case 0x%xull: goto %s;" % (t, lbl(t)))
        out.append("default: TAILI(%s); }" % r64(i.rn))
        return [" ".join(out)]
    return ["TAILI(%s);" % r64(i.rn)]


def e_ret(i, fc):
    return ["return;"]


# ------------------------------------------------------------------- system ---

def e_sys(i, fc):
    if i.op1 == 3 and i.crn == 7 and i.crm == 4 and i.op2 == 1:   # DC ZVA
        return ["memset((void*)(uintptr_t)(%s & ~(uint64_t)63), 0, 64);" % r64(i.rt)]
    return []


MRS_FIXED = {"ctr_el0": "0x8444c004ull", "dczid_el0": "4ull", "cntfrq_el0": "19200000ull", "mpidr_el1": "0x80000000ull"}


def e_mrs(i, fc):
    r = i.sysreg
    if r in ("tpidr_el0", "tpidrro_el0"):
        e = "c->%s" % r
    elif r == "cntpct_el0" or r == "cntvct_el0":
        e = "rc_cntpct()"
    elif r in ("fpcr", "fpsr"):
        e = "(uint64_t)c->%s" % r
    elif r == "nzcv":
        e = "((uint64_t)N << 31 | (uint64_t)Z << 30 | (uint64_t)C << 29 | (uint64_t)V << 28)"
    elif r in MRS_FIXED:
        e = MRS_FIXED[r]
    else:
        e = "rc_mrs(c, 0x%xull, 0x%08xu)" % (i.pc, i.word)
    return [wr(i.rt, e, 1)]


def e_msr(i, fc):
    r = i.sysreg
    if r in ("tpidr_el0",):
        return ["c->%s = %s;" % (r, r64(i.rt))]
    if r in ("fpcr", "fpsr"):
        return ["c->%s = (uint32_t)%s;" % (r, r64(i.rt))]
    if r == "nzcv":
        return ["{ uint64_t f_ = %s; N = (f_ >> 31) & 1; Z = (f_ >> 30) & 1; C = (f_ >> 29) & 1; V = (f_ >> 28) & 1; }" % r64(i.rt)]
    return ["rc_msr(c, 0x%xull, 0x%08xu, %s);" % (i.pc, i.word, r64(i.rt))]


# ------------------------------------------------------------- scalar float ---

def fsel(ftype):
    return {0: ("FS", "SETS", "float"), 1: ("FD", "SETD", "double")}.get(ftype)


def e_fp_dp1(i, fc):
    s = fsel(i.ftype)
    if not s:
        return trap(i, "fp16")
    G, S, T = s
    n = "%s(%d)" % (G, i.rn)
    op = i.opcode
    sfx = "s" if i.ftype == 0 else "d"
    if op == 0:
        return ["%s(%d, %s);" % (S, i.rd, n)]
    if op == 1:
        return ["%s(%d, fabs_%s(%s));" % (S, i.rd, sfx, n)]
    if op == 2:
        return ["%s(%d, fneg_%s(%s));" % (S, i.rd, sfx, n)]
    if op == 3:
        return ["%s(%d, %s(%s));" % (S, i.rd, "sqrt_s" if i.ftype == 0 else "sqrt_d", n)]
    if op == 4 and i.ftype == 1:
        return ["SETS(%d, (float)%s);" % (i.rd, n)]
    if op == 5 and i.ftype == 0:
        return ["SETD(%d, (double)%s);" % (i.rd, n)]
    modes = {8: 0, 9: 1, 10: 2, 11: 3, 12: 4, 14: 0, 15: 0}
    if op in modes:
        return ["%s(%d, rnd_%s(%s, %d));" % (S, i.rd, sfx, n, modes[op])]
    return trap(i, "fp_dp1")


def e_fp_dp2(i, fc):
    s = fsel(i.ftype)
    if not s:
        return trap(i, "fp16")
    G, S, T = s
    sfx = "s" if i.ftype == 0 else "d"
    n, m = "%s(%d)" % (G, i.rn), "%s(%d)" % (G, i.rm)
    e = {0: "%s * %s", 1: "%s / %s", 2: "%s + %s", 3: "%s - %s", 4: "fmax_" + sfx + "(%s, %s)",
         5: "fmin_" + sfx + "(%s, %s)", 6: "fmaxnm_" + sfx + "(%s, %s)", 7: "fminnm_" + sfx + "(%s, %s)",
         8: "-(%s * %s)"}.get(i.opcode)
    if e is None:
        return trap(i, "fp_dp2")
    return ["%s(%d, %s);" % (S, i.rd, e % (n, m))]


def e_fp_dp3(i, fc):
    s = fsel(i.ftype)
    if not s:
        return trap(i, "fp16")
    G, S, T = s
    f = "fmaf" if i.ftype == 0 else "fma"
    n, m, a = "%s(%d)" % (G, i.rn), "%s(%d)" % (G, i.rm), "%s(%d)" % (G, i.ra)
    e = {(0, 0): "%s(%s, %s, %s)" % (f, n, m, a), (0, 1): "%s(-%s, %s, %s)" % (f, n, m, a),
         (1, 0): "%s(-%s, %s, -%s)" % (f, n, m, a), (1, 1): "%s(%s, %s, -%s)" % (f, n, m, a)}[(i.o1, i.o0)]
    return ["%s(%d, %s);" % (S, i.rd, e)]


def e_fcmp(i, fc):
    s = fsel(i.ftype)
    if not s:
        return trap(i, "fp16")
    G, S, T = s
    n = "%s(%d)" % (G, i.rn)
    m = "(%s)0" % T if i.opc & 8 else "%s(%d)" % (G, i.rm)
    return ["{ %s a_ = %s, b_ = %s; FCMP_FLAGS(a_, b_); }" % (T, n, m)]


def e_fccmp(i, fc):
    s = fsel(i.ftype)
    if not s:
        return trap(i, "fp16")
    G, S, T = s
    return ["if (%s) { %s a_ = %s(%d), b_ = %s(%d); FCMP_FLAGS(a_, b_); } else SETNZCV(%d);" % (
        CONDS[i.cond], T, G, i.rn, G, i.rm, i.nzcv)]


def e_fcsel(i, fc):
    if i.ftype == 0:
        return ["SETSB(%d, %s ? VR(%d).s[0] : VR(%d).s[0]);" % (i.rd, CONDS[i.cond], i.rn, i.rm)]
    if i.ftype == 1:
        return ["SETDB(%d, %s ? VR(%d).d[0] : VR(%d).d[0]);" % (i.rd, CONDS[i.cond], i.rn, i.rm)]
    return trap(i, "fp16")


def e_fmov_imm(i, fc):
    if i.ftype == 0:
        return ["SETSB(%d, 0x%xu);" % (i.rd, a64.vfp_expand_imm(i.imm8, 32))]
    if i.ftype == 1:
        return ["SETDB(%d, 0x%xull);" % (i.rd, a64.vfp_expand_imm(i.imm8, 64))]
    return trap(i, "fp16")


def e_fcvt_int(i, fc):
    sf, ft, rm, op = i.sf, i.ftype, i.rmode, i.opcode
    if op in (6, 7):
        if rm == 0 and ft in (0, 1):
            if op == 6:
                return [wr(i.rd, "VR(%d).%s[0]" % (i.rn, "s" if ft == 0 else "d"), sf)]
            return ["%s(%d, %s);" % ("SETSB" if ft == 0 else "SETDB", i.rd, rd(i.rn, sf))]
        if rm == 1 and ft == 2 and sf:
            if op == 6:
                return [wr(i.rd, "VR(%d).d[1]" % i.rn, 1)]
            return ["VR(%d).d[1] = %s;" % (i.rd, r64(i.rn))]
        return trap(i, "fmov")
    s = fsel(ft)
    if not s:
        return trap(i, "fp16")
    G, S, T = s
    if op in (2, 3) and rm == 0:
        src = rd(i.rn, sf)
        cast = ("(int64_t)%s" if sf else "(int32_t)%s") % src if op == 2 else src
        return ["%s(%d, (%s)%s);" % (S, i.rd, T, cast)]
    if op in (0, 1, 4, 5):
        mode = 4 if op in (4, 5) else {0: 0, 1: 1, 2: 2, 3: 3}[rm]
        if op in (4, 5) and rm != 0:
            return trap(i, "fcvt")
        uns = op in (1, 5)
        f = ("CVT_U64" if uns else "CVT_S64") if sf else ("CVT_U32" if uns else "CVT_S32")
        return [wr(i.rd, "%s(%s(%d), %d)" % (f, G, i.rn, mode), sf)]
    return trap(i, "fcvt_int")


def e_fcvt_fixed(i, fc):
    s = fsel(i.ftype)
    if not s:
        return trap(i, "fp16")
    G, S, T = s
    fbits = 64 - i.scale
    if i.rmode == 3 and i.opcode in (0, 1):
        uns = i.opcode == 1
        f = ("CVT_U64" if uns else "CVT_S64") if i.sf else ("CVT_U32" if uns else "CVT_S32")
        return [wr(i.rd, "%s(ldexp((double)%s(%d), %d), 3)" % (f, G, i.rn, fbits), i.sf)]
    if i.rmode == 0 and i.opcode in (2, 3):
        src = rd(i.rn, i.sf)
        if i.opcode == 2:
            src = ("(int64_t)%s" if i.sf else "(int32_t)%s") % src
        fn = "ldexpf" if i.ftype == 0 else "ldexp"
        return ["%s(%d, %s((%s)%s, -%d));" % (S, i.rd, fn, T, src, fbits)]
    return trap(i, "fcvt_fixed")


# ------------------------------------------------------------------- SIMD ---

def vr(n):
    return "VR(%d)" % n


def e_v_same(i, fc):
    q, u, sz, op = i.q, i.u, i.size, i.opcode
    d, n, m = i.rd, vr(i.rn), vr(i.rm)
    if op == 3:
        if u == 0:
            f = {0: "v_and", 1: "v_bic", 2: "v_orr", 3: "v_orn"}[sz]
            return ["%s = %s(%s, %s, %d);" % (vr(d), f, n, m, q)]
        f = {0: "v_eor", 1: "v_bsl", 2: "v_bit", 3: "v_bif"}[sz]
        if sz == 0:
            return ["%s = v_eor(%s, %s, %d);" % (vr(d), n, m, q)]
        return ["%s = %s(%s, %s, %s, %d);" % (vr(d), f, vr(d), n, m, q)]
    if op < 24:
        f = {(16, 0): "v_add", (16, 1): "v_sub", (17, 0): "v_cmtst", (17, 1): "v_cmeq", (6, 0): "v_cmgt",
             (6, 1): "v_cmhi", (7, 0): "v_cmge", (7, 1): "v_cmhs", (12, 0): "v_smax", (12, 1): "v_umax",
             (13, 0): "v_smin", (13, 1): "v_umin", (1, 0): "v_sqadd", (1, 1): "v_uqadd", (5, 0): "v_sqsub",
             (5, 1): "v_uqsub", (8, 0): "v_sshl", (8, 1): "v_ushl", (19, 0): "v_mul"}.get((op, u))
        if f is None:
            return trap(i, "v_same")
        return ["%s = %s(%s, %s, %d, %d);" % (vr(d), f, n, m, sz, q)]
    fsz, hi = sz & 1, sz >> 1
    if fsz == 1 and not q:
        return trap(i, "v_same 1d")
    if op == 25 and u == 0:
        return ["%s = vf_mla(%s, %s, %s, %d, %d, %d);" % (vr(d), vr(d), n, m, fsz, q, hi)]
    f = {(24, 0, 0): "vf_maxnm", (24, 0, 1): "vf_minnm", (26, 0, 0): "vf_add", (26, 0, 1): "vf_sub",
         (26, 1, 0): "vf_addp", (26, 1, 1): "vf_abd", (27, 1, 0): "vf_mul", (28, 0, 0): "vf_cmeq",
         (28, 1, 0): "vf_cmge", (28, 1, 1): "vf_cmgt", (29, 1, 0): "vf_acge", (29, 1, 1): "vf_acgt",
         (30, 0, 0): "vf_max", (30, 0, 1): "vf_min", (31, 0, 0): "vf_recps", (31, 0, 1): "vf_rsqrts",
         (31, 1, 0): "vf_div"}.get((op, u, hi))
    if f is None:
        return trap(i, "v_same fp")
    return ["%s = %s(%s, %s, %d, %d);" % (vr(d), f, n, m, fsz, q)]


def e_v_misc(i, fc):
    q, u, sz, op = i.q, i.u, i.size, i.opcode
    d, n = vr(i.rd), vr(i.rn)
    if op == 0:
        return ["%s = v_rev(%s, %d, %d, %d);" % (d, n, sz, 2 if u else 3, q)]
    if op == 1 and u == 0:
        return ["%s = v_rev(%s, %d, 1, %d);" % (d, n, sz, q)]
    if op == 5:
        if u == 0 and sz == 0:
            return ["%s = v_cnt(%s, 0, %d);" % (d, n, q)]
        if u == 1 and sz == 0:
            return ["%s = v_not(%s, 0, %d);" % (d, n, q)]
        if u == 1 and sz == 1:
            return ["%s = v_rbit8(%s, %d);" % (d, n, q)]
    int_ops = {(8, 0): "v_cmgtz", (8, 1): "v_cmgez", (9, 0): "v_cmeqz", (9, 1): "v_cmlez", (10, 0): "v_cmltz",
               (11, 0): "v_abs", (11, 1): "v_neg"}
    if (op, u) in int_ops:
        return ["%s = %s(%s, %d, %d);" % (d, int_ops[(op, u)], n, sz, q)]
    if op == 18 and u == 0:
        return ["%s = v_xtn(%s, %s, %d, %d, 0);" % (d, d, n, sz, q)]
    if op == 20:
        return ["%s = v_xtn(%s, %s, %d, %d, %d);" % (d, d, n, sz, q, 1 if u == 0 else 2)]
    if op == 22 and u == 0 and sz & 1:
        return ["%s = vf_cvtn(%s, %s, %d);" % (d, d, n, q)]
    if op == 23 and u == 0 and sz & 1:
        return ["%s = vf_cvtl(%s, %d);" % (d, n, q)]
    fsz, hi = sz & 1, sz >> 1
    if fsz == 1 and not q:
        return trap(i, "v_misc 1d")
    fops = {(12, 0, 1): "vf_cmgtz", (12, 1, 1): "vf_cmgez", (13, 0, 1): "vf_cmeqz", (13, 1, 1): "vf_cmlez",
            (14, 0, 1): "vf_cmltz", (15, 0, 1): "vf_abs", (15, 1, 1): "vf_neg", (29, 0, 1): "vf_recpe",
            (29, 1, 1): "vf_rsqrte", (31, 1, 1): "vf_sqrt"}
    if (op, u, hi) in fops:
        return ["%s = %s(%s, %d, %d);" % (d, fops[(op, u, hi)], n, fsz, q)]
    if op == 29 and hi == 0:
        return ["%s = vf_cvt_fromint(%s, %d, %d, %d, 0);" % (d, n, fsz, q, u)]
    cvt = {(26, 0): 0, (26, 1): 1, (27, 0): 2, (27, 1): 3, (28, 0): 4}
    if (op, hi) in cvt:
        return ["%s = vf_cvt_toint(%s, %d, %d, %d, %d, 0);" % (d, n, fsz, q, u, cvt[(op, hi)])]
    rint = {(24, 0, 0): 0, (24, 0, 1): 1, (25, 0, 0): 2, (25, 0, 1): 3, (24, 1, 0): 4, (25, 1, 0): 0, (25, 1, 1): 0}
    if (op, u, hi) in rint:
        return ["%s = vf_rint(%s, %d, %d, %d);" % (d, n, fsz, q, rint[(op, u, hi)])]
    return trap(i, "v_misc")


def e_v_across(i, fc):
    q, u, sz, op = i.q, i.u, i.size, i.opcode
    S = {0: "SETBB", 1: "SETHB", 2: "SETSB", 3: "SETDB"}
    if op == 27 and u == 0:
        return ["%s(%d, v_addv(%s, %d, %d));" % (S[sz], i.rd, vr(i.rn), sz, q)]
    if op == 3:
        return ["%s(%d, v_addlv(%s, %d, %d, %d));" % (S[sz + 1], i.rd, vr(i.rn), sz, q, 1 - u)]
    if op in (10, 26):
        return ["%s(%d, v_maxv(%s, %d, %d, %d, %d));" % (S[sz], i.rd, vr(i.rn), sz, q, 1 - u, op == 26)]
    return trap(i, "v_across")


def e_v_diff(i, fc):
    q, u, sz, op = i.q, i.u, i.size, i.opcode
    d, n, m = vr(i.rd), vr(i.rn), vr(i.rm)
    if op == 14 and u == 0:
        return ["%s = rc_pmull(%s, %s, %d, %d);" % (d, n, m, sz, q)]
    if op in (12, 8, 10):
        mode = {12: 0, 8: 1, 10: 2}[op]
        return ["%s = v_mull(%s, %s, %s, %d, %d, %d, %d);" % (d, d, n, m, sz, 1 - u, q, mode)]
    if op in (1, 3):
        return ["%s = v_addw(%s, %s, %d, %d, %d, %d);" % (d, n, m, sz, 1 - u, q, op == 3)]
    if op in (0, 2):
        return ["%s = v_addl(%s, %s, %d, %d, %d, %d);" % (d, n, m, sz, 1 - u, q, op == 2)]
    return trap(i, "v_diff")


def elem_index(i):
    """(element size log2, index, rm) for by-element forms."""
    if i.size == 1:
        return 1, (i.h << 2) | (i.l << 1) | i.m, i.rm
    if i.size == 2:
        return 2, (i.h << 1) | i.l, (i.m << 4) | i.rm
    return 3, i.h, (i.m << 4) | i.rm


def e_v_elem(i, fc):
    q, u, op = i.q, i.u, i.opcode
    esz, idx, rmn = elem_index(i)
    d, n = vr(i.rd), vr(i.rn)
    if op in (1, 5, 9) and u == 0 and esz in (2, 3):
        fsz = esz - 2
        dup = "v_dup(VR(%d).%s[%d], %d, 1)" % (rmn, "s" if esz == 2 else "d", idx, esz)
        if op == 9:
            return ["%s = vf_mul(%s, %s, %d, %d);" % (d, n, dup, fsz, q)]
        return ["%s = vf_mla(%s, %s, %s, %d, %d, %d);" % (d, d, n, dup, fsz, q, op == 5)]
    if op == 8 and u == 0:
        return ["%s = v_mul(%s, v_dup(vget(VR(%d), %d, %d), %d, 1), %d, %d);" % (d, n, rmn, esz, idx, esz, esz, q)]
    if op in (2, 6, 10):
        mode = {10: 0, 2: 1, 6: 2}[op]
        dup = "v_dup(vget(VR(%d), %d, %d), %d, 1)" % (rmn, esz, idx, esz)
        return ["%s = v_mull(%s, %s, %s, %d, %d, %d, %d);" % (d, d, n, dup, esz, 1 - u, q, mode)]
    return trap(i, "v_elem")


def shift_params(immh, immb):
    esz = immh.bit_length() - 1        # 0..3
    v = (immh << 3) | immb
    return esz, (16 << esz) - v, v - (8 << esz)   # (size, right shift, left shift)


def e_v_shift(i, fc):
    q, u, op = i.q, i.u, i.opcode
    esz, rsh, lsh = shift_params(i.immh, i.immb)
    d, n = vr(i.rd), vr(i.rn)
    if op in (0, 4):
        f = "v_ushr" if u else "v_sshr"
        return ["%s = %s(%s, %d, %d, %d, %d);" % (d, f, n, esz, rsh, q, op == 4)]
    if op == 10 and u == 0:
        return ["%s = v_shl(%s, %d, %d, %d);" % (d, n, esz, lsh, q)]
    if op == 20:
        return ["%s = v_shll(%s, %d, %d, %d, %d);" % (d, n, esz, lsh, 1 - u, q)]
    if op in (16, 17, 18, 19):
        # narrowing: immh gives the destination element size; the source is twice as wide
        mode = {(16, 0): 0, (17, 0): 0, (18, 0): 1, (19, 0): 1, (18, 1): 2, (19, 1): 2, (16, 1): 3, (17, 1): 3}[(op, u)]
        rnd = op in (17, 19)
        return ["%s = v_shrn(%s, %s, %d, %d, %d, %d, %d);" % (d, d, n, esz, rsh, q, rnd, mode)]
    if op in (28, 31) and esz in (2, 3):
        fsz = esz - 2
        if op == 28:
            return ["%s = vf_cvt_fromint(%s, %d, %d, %d, %d);" % (d, n, fsz, q, u, rsh)]
        return ["%s = vf_cvt_toint(%s, %d, %d, %d, 3, %d);" % (d, n, fsz, q, u, rsh)]
    return trap(i, "v_shift")


def e_v_copy(i, fc):
    q = i.q
    sz = (i.imm5 & -i.imm5).bit_length() - 1
    if sz > 3:
        return trap(i, "v_copy")
    idx = i.imm5 >> (sz + 1)
    d, n = i.rd, i.rn
    if i.opbit == 1:
        src_idx = i.imm4 >> sz
        return ["%s = v_ins(%s, %d, %d, vget(%s, %d, %d));" % (vr(d), vr(d), sz, idx, vr(n), sz, src_idx)]
    if i.imm4 == 0:
        return ["%s = v_dup(vget(%s, %d, %d), %d, %d);" % (vr(d), vr(n), sz, idx, sz, q)]
    if i.imm4 == 1:
        return ["%s = v_dup(%s, %d, %d);" % (vr(d), r64(a64.zr(n)), sz, q)]
    if i.imm4 == 3:
        return ["%s = v_ins(%s, %d, %d, %s);" % (vr(d), vr(d), sz, idx, r64(a64.zr(n)))]
    if i.imm4 == 7:
        return [wr(a64.zr(d), "vget(%s, %d, %d)" % (vr(n), sz, idx), q)]
    if i.imm4 == 5:
        e = "(uint64_t)vgets(%s, %d, %d)" % (vr(n), sz, idx)
        return [wr(a64.zr(d), e, q)]
    return trap(i, "v_copy")


def e_v_modimm(i, fc):
    q, op, cmode = i.q, i.opbit, i.cmode
    imm = i.imm
    d = vr(i.rd)
    lo, hi = imm, (imm if q else 0)
    is_orr = op == 0 and ((cmode & 0b1001) == 0b0001 or (cmode & 0b1101) == 0b1001)
    is_bic = op == 1 and ((cmode & 0b1001) == 0b0001 or (cmode & 0b1101) == 0b1001)
    is_mvni = op == 1 and ((cmode & 0b1001) == 0b0000 or (cmode & 0b1101) == 0b1000 or (cmode & 0b1110) == 0b1100)
    if is_orr:
        return ["%s.d[0] |= 0x%xull; %s.d[1] = %s;" % (d, lo, d, ("%s.d[1] | 0x%xull" % (d, hi)) if q else "0")]
    if is_bic:
        return ["%s.d[0] &= 0x%xull; %s.d[1] = %s;" % (d, ~lo & ((1 << 64) - 1), d,
                                                        ("%s.d[1] & 0x%xull" % (d, ~hi & ((1 << 64) - 1))) if q else "0")]
    if is_mvni:
        lo = ~lo & ((1 << 64) - 1)
        hi = (~hi & ((1 << 64) - 1)) if q else 0
    return ["%s = (V128){ .d = { 0x%xull, 0x%xull } };" % (d, lo, hi)]


def e_v_perm(i, fc):
    f = {"zip1": ("v_zip", 0), "zip2": ("v_zip", 1), "uzp1": ("v_uzp", 0), "uzp2": ("v_uzp", 1),
         "trn1": ("v_trn", 0), "trn2": ("v_trn", 1)}[i.kind]
    return ["%s = %s(%s, %s, %d, %d, %d);" % (vr(i.rd), f[0], vr(i.rn), vr(i.rm), i.size, i.q, f[1])]


def e_v_ext(i, fc):
    return ["%s = v_ext(%s, %s, %d, %d);" % (vr(i.rd), vr(i.rn), vr(i.rm), i.imm4, i.q)]


def e_v_tbl(i, fc):
    return ["%s = v_tbl(c->v, %d, %d, %s, %s, %d, %d);" % (vr(i.rd), i.rn, i.len, vr(i.rm), vr(i.rd), i.q, i.tbx)]


def e_s_copy(i, fc):
    if i.opbit or i.imm4:
        return trap(i, "s_copy")
    sz = (i.imm5 & -i.imm5).bit_length() - 1
    idx = i.imm5 >> (sz + 1)
    S = {0: "SETBB", 1: "SETHB", 2: "SETSB", 3: "SETDB"}[sz]
    return ["%s(%d, vget(%s, %d, %d));" % (S, i.rd, vr(i.rn), sz, idx)]


def e_s_misc(i, fc):
    u, sz, op = i.u, i.size, i.opcode
    fsz, hi = sz & 1, sz >> 1
    G = "s" if fsz == 0 else "d"
    S = "SETS" if fsz == 0 else "SETD"
    if op == 29 and hi == 0:
        src = ("(float)" if fsz == 0 else "(double)") + (
            ("VR(%d).%s[0]" % (i.rn, "s" if fsz == 0 else "d")) if u else ("VR(%d).%s[0]" % (i.rn, "ss" if fsz == 0 else "sd")))
        return ["%s(%d, %s);" % (S, i.rd, src)]
    if op == 29 and hi == 1:
        f = "rc_frsqrte_%s" % G if u else "rc_frecpe_%s" % G
        return ["%s(%d, %s(%s(%d)));" % (S, i.rd, f, "FS" if fsz == 0 else "FD", i.rn)]
    cvt = {(26, 0): 0, (26, 1): 1, (27, 0): 2, (27, 1): 3, (28, 0): 4}
    if (op, hi) in cvt:
        f = ("CVT_U32" if u else "CVT_S32") if fsz == 0 else ("CVT_U64" if u else "CVT_S64")
        SB = "SETSB" if fsz == 0 else "SETDB"
        return ["%s(%d, %s(%s(%d), %d));" % (SB, i.rd, f, "FS" if fsz == 0 else "FD", i.rn, cvt[(op, hi)])]
    if op == 9 and u == 0 and sz == 3:
        return ["SETDB(%d, VR(%d).d[0] == 0 ? ~(uint64_t)0 : 0);" % (i.rd, i.rn)]
    return trap(i, "s_misc")


def e_s_pair(i, fc):
    if i.opcode == 13 and i.u == 1:
        if i.size & 1:
            return ["SETD(%d, VR(%d).df[0] + VR(%d).df[1]);" % (i.rd, i.rn, i.rn)]
        return ["SETS(%d, VR(%d).f[0] + VR(%d).f[1]);" % (i.rd, i.rn, i.rn)]
    if i.opcode == 27 and i.u == 0 and i.size == 3:
        return ["SETDB(%d, VR(%d).d[0] + VR(%d).d[1]);" % (i.rd, i.rn, i.rn)]
    if i.opcode in (12, 15) and i.u == 1:
        f = {(12, 0): "fmaxnm", (12, 1): "fminnm", (15, 0): "fmax", (15, 1): "fmin"}[(i.opcode, i.size >> 1)]
        if i.size & 1:
            return ["SETD(%d, %s_d(VR(%d).df[0], VR(%d).df[1]));" % (i.rd, f, i.rn, i.rn)]
        return ["SETS(%d, %s_s(VR(%d).f[0], VR(%d).f[1]));" % (i.rd, f, i.rn, i.rn)]
    return trap(i, "s_pair")


def e_s_elem(i, fc):
    esz, idx, rmn = elem_index(i)
    if esz not in (2, 3) or i.u:
        return trap(i, "s_elem")
    G, S, f = ("FS", "SETS", "fmaf") if esz == 2 else ("FD", "SETD", "fma")
    e = "VR(%d).%s[%d]" % (rmn, "f" if esz == 2 else "df", idx)
    if i.opcode == 9:
        return ["%s(%d, %s(%d) * %s);" % (S, i.rd, G, i.rn, e)]
    if i.opcode == 1:
        return ["%s(%d, %s(%s(%d), %s, %s(%d)));" % (S, i.rd, f, G, i.rn, e, G, i.rd)]
    if i.opcode == 5:
        return ["%s(%d, %s(-%s(%d), %s, %s(%d)));" % (S, i.rd, f, G, i.rn, e, G, i.rd)]
    return trap(i, "s_elem")


def e_crypto_aes(i, fc):
    k = i.kind
    if k in ("aese", "aesd"):
        return ["%s = rc_%s(%s, %s);" % (vr(i.rd), k, vr(i.rd), vr(i.rn))]
    return ["%s = rc_%s(%s);" % (vr(i.rd), k, vr(i.rn))]


def e_crypto_sha3(i, fc):
    k = i.kind
    if k in ("sha1c", "sha1p", "sha1m"):
        return ["%s = rc_%s(%s, VR(%d).s[0], %s);" % (vr(i.rd), k, vr(i.rd), i.rn, vr(i.rm))]
    return ["%s = rc_%s(%s, %s, %s);" % (vr(i.rd), k, vr(i.rd), vr(i.rn), vr(i.rm))]


def e_crypto_sha2(i, fc):
    k = i.kind
    if k == "sha1h":
        return ["SETSB(%d, rc_sha1h(VR(%d).s[0]));" % (i.rd, i.rn)]
    return ["%s = rc_%s(%s, %s);" % (vr(i.rd), k, vr(i.rd), vr(i.rn))]


HANDLERS = {
    "addsub_imm": e_addsub_imm, "logic_imm": e_logic_imm, "movw": e_movw, "bfm": e_bfm, "extr": e_extr,
    "adr": lambda i, fc: [wr(i.rd, "0x%xull" % i.value, 1)], "adrp": lambda i, fc: [wr(i.rd, "0x%xull" % i.value, 1)],
    "logic_reg": e_logic_reg, "addsub_reg": e_addsub_reg, "addsub_ext": e_addsub_ext, "adc": e_adc, "ccmp": e_ccmp,
    "csel": e_csel, "dp2": e_dp2, "crc32": e_crc32, "dp1": e_dp1, "dp3": e_dp3,
    "ldst": e_ldst, "ldstp": e_ldstp, "ldr_lit": e_ldr_lit, "ldxr": e_ldxr, "stxr": e_stxr, "ldar": e_ldar,
    "stlr": e_stlr, "vldst_multi": e_vldst_multi, "vldst_single": e_vldst_single, "prfm": lambda i, fc: [],
    "b": e_b, "bcond": e_bcond, "cbz": e_cbz, "cbnz": e_cbz, "tbz": e_tbz, "tbnz": e_tbz, "bl": e_bl,
    "blr": e_blr, "br": e_br, "ret": e_ret,
    "svc": lambda i, fc: ["rc_svc(c, %d);" % i.imm],
    "brk": lambda i, fc: trap(i, "brk"), "hlt": lambda i, fc: trap(i, "hlt"), "udf": lambda i, fc: trap(i, "udf"),
    "unknown": lambda i, fc: trap(i, "undecoded"),
    "hint": lambda i, fc: ["RC_YIELD();"] if i.imm == 1 else [],
    "clrex": lambda i, fc: ["c->ex_addr = ~(uint64_t)0;"],
    "dmb": lambda i, fc: ["FENCE();"], "dsb": lambda i, fc: ["FENCE();"], "isb": lambda i, fc: [],
    "sys": e_sys, "sysl": lambda i, fc: trap(i, "sysl"), "mrs": e_mrs, "msr": e_msr,
    "msr_imm": lambda i, fc: [],
    "fp_dp1": e_fp_dp1, "fp_dp2": e_fp_dp2, "fp_dp3": e_fp_dp3, "fcmp": e_fcmp, "fccmp": e_fccmp, "fcsel": e_fcsel,
    "fmov_imm": e_fmov_imm, "fcvt_int": e_fcvt_int, "fcvt_fixed": e_fcvt_fixed,
    "v_same": e_v_same, "v_misc": e_v_misc, "v_across": e_v_across, "v_diff": e_v_diff, "v_elem": e_v_elem,
    "v_shift": e_v_shift, "v_copy": e_v_copy, "v_modimm": e_v_modimm, "v_perm": e_v_perm, "v_ext": e_v_ext,
    "v_tbl": e_v_tbl, "s_copy": e_s_copy, "s_misc": e_s_misc, "s_pair": e_s_pair, "s_elem": e_s_elem,
    "crypto_aes": e_crypto_aes, "crypto_sha3": e_crypto_sha3, "crypto_sha2": e_crypto_sha2,
}


def emit(ins, fc):
    h = HANDLERS.get(ins.op)
    if h is None:
        return trap(ins, ins.op)
    return h(ins, fc)
