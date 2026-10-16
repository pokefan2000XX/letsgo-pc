"""AArch64 (A64) instruction decoder for the recompiler.

decode(word, pc) -> Insn with a canonical `op` name and decoded fields. Covers
the encodings used by the game's main and sdk modules (ARMv8.0 + AES/SHA/CRC32).
Register 31 is resolved here: fields named rd/rn/rm/rt/... hold 0..30, 'zr' or
'sp' according to each encoding's rules, so the emitter never has to know.
Anything not covered decodes as op='unknown'.
"""

COND = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]


class Insn:
    __slots__ = ("op", "f", "pc", "word")

    def __init__(self, op, pc=0, word=0, **f):
        self.op, self.pc, self.word, self.f = op, pc, word, f

    def __getattr__(self, k):
        try:
            return self.f[k]
        except KeyError:
            raise AttributeError(k)

    def __repr__(self):
        return "%s(%s)" % (self.op, ", ".join("%s=%r" % kv for kv in self.f.items()))


def B(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sx(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def zr(r):
    return "zr" if r == 31 else r


def sp(r):
    return "sp" if r == 31 else r


def decode_bitmask(n, imms, immr, datasize):
    """DecodeBitMasks for logical immediates; returns the wmask (or None if reserved)."""
    combined = (n << 6) | (~imms & 0x3F)
    if combined == 0:
        return None
    length = combined.bit_length() - 1
    if length < 1:
        return None
    levels = (1 << length) - 1
    s = imms & levels
    r = immr & levels
    if s == levels:
        return None
    esize = 1 << length
    welem = (1 << (s + 1)) - 1
    # rotate right by r within esize
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1) if r else welem
    out = 0
    for i in range(datasize // esize):
        out |= welem << (i * esize)
    return out


def vfp_expand_imm(imm8, n):
    """VFPExpandImm: 8-bit FP immediate -> bit pattern of an n-bit float (16/32/64)."""
    e = {16: 5, 32: 8, 64: 11}[n]
    f = n - e - 1
    sign = (imm8 >> 7) & 1
    b6 = (imm8 >> 6) & 1
    exp = ((b6 ^ 1) << (e - 1)) | ((((1 << (e - 3)) - 1) if b6 else 0) << 2) | ((imm8 >> 4) & 3)
    frac = (imm8 & 0xF) << (f - 4)
    return (sign << (n - 1)) | (exp << f) | frac


def adv_simd_expand_imm(op, cmode, imm8):
    """AdvSIMDExpandImm -> 64-bit pattern (None for reserved)."""
    c3 = cmode >> 1
    if c3 == 0:
        return (imm8 << 32) | imm8
    if c3 == 1:
        v = imm8 << 8
        return (v << 32) | v
    if c3 == 2:
        v = imm8 << 16
        return (v << 32) | v
    if c3 == 3:
        v = imm8 << 24
        return (v << 32) | v
    if c3 == 4:
        v = imm8
        v = (v << 16) | v
        return (v << 32) | v
    if c3 == 5:
        v = imm8 << 8
        v = (v << 16) | v
        return (v << 32) | v
    if c3 == 6:
        if cmode & 1 == 0:
            v = (imm8 << 8) | 0xFF
        else:
            v = (imm8 << 16) | 0xFFFF
        return (v << 32) | v
    # c3 == 7
    if cmode & 1 == 0 and op == 0:
        v = 0
        for i in range(8):
            v |= imm8 << (8 * i)
        return v
    if cmode & 1 == 0 and op == 1:
        v = 0
        for i in range(8):
            if (imm8 >> i) & 1:
                v |= 0xFF << (8 * i)
        return v
    if cmode & 1 == 1 and op == 0:
        v = vfp_expand_imm(imm8, 32)
        return (v << 32) | v
    if cmode & 1 == 1 and op == 1:
        return vfp_expand_imm(imm8, 64)
    return None


# ------------------------------------------------------------------ groups ---

def dec_dp_imm(w, pc):
    g = B(w, 25, 23)
    if g in (0, 1):
        imm = sx((B(w, 23, 5) << 2) | B(w, 30, 29), 21)
        if w >> 31:
            return Insn("adrp", rd=zr(B(w, 4, 0)), value=((pc & ~0xFFF) + (imm << 12)) & 0xFFFFFFFFFFFFFFFF)
        return Insn("adr", rd=zr(B(w, 4, 0)), value=(pc + imm) & 0xFFFFFFFFFFFFFFFF)
    sf = w >> 31
    rd, rn = B(w, 4, 0), B(w, 9, 5)
    if g == 2:
        sub, s = B(w, 30, 30), B(w, 29, 29)
        sh = B(w, 22, 22)
        imm = B(w, 21, 10) << (12 if sh else 0)
        return Insn("addsub_imm", sf=sf, sub=sub, s=s, rd=(zr(rd) if s else sp(rd)), rn=sp(rn), imm=imm)
    if g == 4:
        opc, n = B(w, 30, 29), B(w, 22, 22)
        if sf == 0 and n:
            return None
        imm = decode_bitmask(n, B(w, 15, 10), B(w, 21, 16), 64 if sf else 32)
        if imm is None:
            return None
        return Insn("logic_imm", sf=sf, opc=opc, rd=(zr(rd) if opc == 3 else sp(rd)), rn=zr(rn), imm=imm)
    if g == 5:
        opc, hw = B(w, 30, 29), B(w, 22, 21)
        if opc == 1 or (sf == 0 and hw >= 2):
            return None
        return Insn("movw", sf=sf, opc=opc, rd=zr(rd), imm16=B(w, 20, 5), shift=hw * 16)
    if g == 6:
        opc, n = B(w, 30, 29), B(w, 22, 22)
        if opc == 3 or n != sf:
            return None
        return Insn("bfm", sf=sf, opc=opc, rd=zr(rd), rn=zr(rn), immr=B(w, 21, 16), imms=B(w, 15, 10))
    if g == 7:
        if B(w, 30, 29) != 0 or B(w, 21, 21) != 0 or B(w, 22, 22) != sf:
            return None
        return Insn("extr", sf=sf, rd=zr(rd), rn=zr(rn), rm=zr(B(w, 20, 16)), lsb=B(w, 15, 10))
    return None


SYSREGS = {
    (3, 3, 13, 0, 2): "tpidr_el0", (3, 3, 13, 0, 3): "tpidrro_el0", (3, 3, 14, 0, 1): "cntpct_el0",
    (3, 3, 14, 0, 0): "cntfrq_el0", (3, 3, 0, 0, 1): "ctr_el0", (3, 3, 4, 4, 0): "fpcr",
    (3, 3, 4, 4, 1): "fpsr", (3, 3, 0, 0, 7): "dczid_el0", (3, 3, 4, 2, 0): "nzcv",
    (3, 3, 14, 0, 2): "cntvct_el0", (3, 0, 0, 0, 5): "mpidr_el1",
}


def dec_branch_sys(w, pc):
    if (w & 0x7C000000) == 0x14000000:
        tgt = (pc + sx(B(w, 25, 0), 26) * 4) & 0xFFFFFFFFFFFFFFFF
        return Insn("bl" if w >> 31 else "b", target=tgt)
    if (w & 0xFF000010) == 0x54000000:
        return Insn("bcond", cond=B(w, 3, 0), target=(pc + sx(B(w, 23, 5), 19) * 4) & 0xFFFFFFFFFFFFFFFF)
    if (w & 0x7E000000) == 0x34000000:
        return Insn("cbnz" if B(w, 24, 24) else "cbz", sf=w >> 31, rt=zr(B(w, 4, 0)),
                    target=(pc + sx(B(w, 23, 5), 19) * 4) & 0xFFFFFFFFFFFFFFFF)
    if (w & 0x7E000000) == 0x36000000:
        return Insn("tbnz" if B(w, 24, 24) else "tbz", bit=(B(w, 31, 31) << 5) | B(w, 23, 19), rt=zr(B(w, 4, 0)),
                    target=(pc + sx(B(w, 18, 5), 14) * 4) & 0xFFFFFFFFFFFFFFFF)
    if (w & 0xFE000000) == 0xD6000000:
        opc = B(w, 24, 21)
        if B(w, 20, 16) != 0x1F or B(w, 15, 10) != 0 or B(w, 4, 0) != 0:
            return None
        name = {0: "br", 1: "blr", 2: "ret"}.get(opc)
        return Insn(name, rn=zr(B(w, 9, 5))) if name else None
    if (w & 0xFF000000) == 0xD4000000:
        opc, ll = B(w, 23, 21), B(w, 1, 0)
        imm = B(w, 20, 5)
        if opc == 0 and ll == 1:
            return Insn("svc", imm=imm)
        if opc == 1 and ll == 0:
            return Insn("brk", imm=imm)
        if opc == 2 and ll == 0:
            return Insn("hlt", imm=imm)
        return None
    if (w & 0xFFC00000) == 0xD5000000:
        l, op0, op1, crn, crm, op2, rt = B(w, 21, 21), B(w, 20, 19), B(w, 18, 16), B(w, 15, 12), B(w, 11, 8), B(w, 7, 5), B(w, 4, 0)
        if op0 == 0 and l == 0:
            if crn == 2 and op1 == 3 and rt == 31:
                return Insn("hint", imm=(crm << 3) | op2)
            if crn == 3 and op1 == 3 and rt == 31:
                if op2 == 2:
                    return Insn("clrex")
                if op2 == 4:
                    return Insn("dsb", opt=crm)
                if op2 == 5:
                    return Insn("dmb", opt=crm)
                if op2 == 6:
                    return Insn("isb")
            if crn == 4:
                return Insn("msr_imm", op1=op1, op2=op2, crm=crm)
            return None
        if op0 == 1:
            return Insn("sysl" if l else "sys", op1=op1, crn=crn, crm=crm, op2=op2, rt=zr(rt))
        reg = SYSREGS.get((op0, op1, crn, crm, op2), "s%d_%d_c%d_c%d_%d" % (op0, op1, crn, crm, op2))
        return Insn("mrs" if l else "msr", sysreg=reg, rt=zr(rt))
    return None


def dec_ldst(w, pc):
    size = B(w, 31, 30)
    v = B(w, 26, 26)
    rt, rn = B(w, 4, 0), B(w, 9, 5)
    # load/store exclusive / ordered
    if (w & 0x3F000000) == 0x08000000:
        o2, l, o1, o0 = B(w, 23, 23), B(w, 22, 22), B(w, 21, 21), B(w, 15, 15)
        rs, rt2 = B(w, 20, 16), B(w, 14, 10)
        nbytes = 1 << size
        if o2 == 0 and o1 == 0:
            if l:
                return Insn("ldxr", size=nbytes, acq=o0, rt=zr(rt), rn=sp(rn))
            return Insn("stxr", size=nbytes, rel=o0, rs=zr(rs), rt=zr(rt), rn=sp(rn))
        if o2 == 0 and o1 == 1 and size >= 2:
            if l:
                return Insn("ldxp", size=nbytes, acq=o0, rt=zr(rt), rt2=zr(rt2), rn=sp(rn))
            return Insn("stxp", size=nbytes, rel=o0, rs=zr(rs), rt=zr(rt), rt2=zr(rt2), rn=sp(rn))
        if o2 == 1 and o1 == 0:
            if l:
                return Insn("ldar", size=nbytes, acq=o0, rt=zr(rt), rn=sp(rn))
            return Insn("stlr", size=nbytes, rel=o0, rt=zr(rt), rn=sp(rn))
        return None
    # SIMD load/store multiple structures
    if (w & 0xBFBF0000) == 0x0C000000 or (w & 0xBFA00000) == 0x0C800000:
        post = B(w, 23, 23)
        opcode = B(w, 15, 12)
        table = {0b0000: (4, 4), 0b0010: (1, 4), 0b0100: (3, 3), 0b0110: (1, 3), 0b0111: (1, 1),
                 0b1000: (2, 2), 0b1010: (1, 2)}
        if opcode not in table:
            return None
        selem, rpt_regs = table[opcode]
        nregs = rpt_regs if selem == 1 else selem
        q = B(w, 30, 30)
        if selem > 1 and B(w, 11, 10) == 3 and q == 0:
            return None
        rm = B(w, 20, 16)
        return Insn("vldst_multi", load=B(w, 22, 22), q=q, size=B(w, 11, 10), selem=selem, nregs=nregs,
                    rt=rt, rn=sp(rn), post=post, rm=(None if not post else ("imm" if rm == 31 else rm)))
    # SIMD load/store single structure (incl. replicate)
    if (w & 0xBF9F0000) == 0x0D000000 or (w & 0xBF800000) == 0x0D800000:
        post = B(w, 23, 23)
        l, r = B(w, 22, 22), B(w, 21, 21)
        opcode, s, sz = B(w, 15, 13), B(w, 12, 12), B(w, 11, 10)
        q = B(w, 30, 30)
        selem = ((opcode & 1) << 1 | r) + 1
        scale = opcode >> 1
        replicate = False
        index = 0
        if scale == 3:
            if not l or s:
                return None
            scale = sz
            replicate = True
        elif scale == 0:
            index = (q << 3) | (s << 2) | sz
        elif scale == 1:
            if sz & 1:
                return None
            index = (q << 2) | (s << 1) | (sz >> 1)
        elif scale == 2:
            if sz & 2:
                return None
            if sz & 1 == 0:
                index = (q << 1) | s
            else:
                if s:
                    return None
                index = q
                scale = 3
        rm = B(w, 20, 16)
        return Insn("vldst_single", load=l, q=q, scale=scale, selem=selem, index=index, replicate=replicate,
                    rt=rt, rn=sp(rn), post=post, rm=(None if not post else ("imm" if rm == 31 else rm)))
    # load register (literal)
    if (w & 0x3B000000) == 0x18000000:
        opc = size
        addr = (pc + sx(B(w, 23, 5), 19) * 4) & 0xFFFFFFFFFFFFFFFF
        if v:
            if opc == 3:
                return None
            return Insn("ldr_lit", fp=1, nbytes=[4, 8, 16][opc], signed=0, rt=rt, addr=addr)
        if opc == 3:
            return Insn("prfm", )
        return Insn("ldr_lit", fp=0, nbytes=[4, 8, 4][opc], signed=(opc == 2), rt=zr(rt), addr=addr)
    # load/store pair
    if (w & 0x3A000000) == 0x28000000:
        mode = B(w, 24, 23)
        l = B(w, 22, 22)
        opc = size
        imm7 = sx(B(w, 21, 15), 7)
        rt2 = B(w, 14, 10)
        if v:
            if opc == 3:
                return None
            nbytes = 4 << opc
            signed = 0
        else:
            if opc == 3 or (opc == 1 and not l):
                return None
            nbytes = 8 if opc == 2 else 4
            signed = (opc == 1)
        modes = {0: "off", 1: "post", 2: "off", 3: "pre"}  # 0 = non-temporal pair (same semantics)
        return Insn("ldstp", load=l, fp=v, nbytes=nbytes, signed=signed, rt=(rt if v else zr(rt)),
                    rt2=(rt2 if v else zr(rt2)), rn=sp(rn), mode=modes[mode], imm=imm7 * nbytes)
    # load/store register (all non-literal single-register forms)
    if (w & 0x3A000000) == 0x38000000:
        opc = B(w, 23, 22)
        if v:
            if size == 0 and opc & 2:
                nbytes = 16
            else:
                if opc & 2:
                    return None
                nbytes = 1 << size
            load = opc & 1
            kind = "ld" if load else "st"
            ext = 0
        else:
            nbytes = 1 << size
            if opc == 0:
                kind, ext = "st", 0
            elif opc == 1:
                kind, ext = "ld", 0
            elif opc == 2:
                if size == 3:
                    kind = "prfm"
                    ext = 0
                else:
                    kind, ext = "lds", 64
            else:
                if size >= 2:
                    return None
                kind, ext = "lds", 32
        rtv = rt if v else zr(rt)
        if B(w, 24, 24):
            imm = B(w, 21, 10) * nbytes
            if kind == "prfm":
                return Insn("prfm")
            return Insn("ldst", kind=kind, fp=v, nbytes=nbytes, ext=ext, rt=rtv, rn=sp(rn), mode="off", imm=imm)
        if B(w, 21, 21) == 0:
            imm9 = sx(B(w, 20, 12), 9)
            m = B(w, 11, 10)
            mode = {0: "off", 1: "post", 2: "off", 3: "pre"}[m]   # 2 = unprivileged (LDTR): same here
            if kind == "prfm":
                return Insn("prfm")
            return Insn("ldst", kind=kind, fp=v, nbytes=nbytes, ext=ext, rt=rtv, rn=sp(rn), mode=mode, imm=imm9)
        if B(w, 11, 10) == 2:
            option, s = B(w, 15, 13), B(w, 12, 12)
            if option & 2 == 0:
                return None
            amount = (4 if nbytes == 16 else size if not v else (4 if nbytes == 16 else size)) if s else 0
            if v:
                amount = ({1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[nbytes]) if s else 0
            else:
                amount = size if s else 0
            if kind == "prfm":
                return Insn("prfm")
            return Insn("ldst", kind=kind, fp=v, nbytes=nbytes, ext=ext, rt=rtv, rn=sp(rn), mode="reg",
                        rm=zr(B(w, 20, 16)), option=option, amount=amount)
        return None
    return None


def dec_dp_reg(w, pc):
    sf = w >> 31
    rd, rn, rm = B(w, 4, 0), B(w, 9, 5), B(w, 20, 16)
    if (w & 0x1F000000) == 0x0A000000:
        imm6 = B(w, 15, 10)
        if sf == 0 and imm6 >= 32:
            return None
        return Insn("logic_reg", sf=sf, opc=B(w, 30, 29), neg=B(w, 21, 21), shift=B(w, 23, 22), amount=imm6,
                    rd=zr(rd), rn=zr(rn), rm=zr(rm))
    if (w & 0x1F200000) == 0x0B000000:
        sh, imm6 = B(w, 23, 22), B(w, 15, 10)
        if sh == 3 or (sf == 0 and imm6 >= 32):
            return None
        return Insn("addsub_reg", sf=sf, sub=B(w, 30, 30), s=B(w, 29, 29), shift=sh, amount=imm6,
                    rd=zr(rd), rn=zr(rn), rm=zr(rm))
    if (w & 0x1F200000) == 0x0B200000:
        if B(w, 23, 22) != 0:
            return None
        s = B(w, 29, 29)
        imm3 = B(w, 12, 10)
        if imm3 > 4:
            return None
        return Insn("addsub_ext", sf=sf, sub=B(w, 30, 30), s=s, option=B(w, 15, 13), amount=imm3,
                    rd=(zr(rd) if s else sp(rd)), rn=sp(rn), rm=zr(rm))
    if (w & 0x1FE00000) == 0x1A000000:
        if B(w, 15, 10) != 0:
            return None
        return Insn("adc", sf=sf, sub=B(w, 30, 30), s=B(w, 29, 29), rd=zr(rd), rn=zr(rn), rm=zr(rm))
    if (w & 0x1FE00000) == 0x1A400000:
        if B(w, 29, 29) != 1 or B(w, 10, 10) or B(w, 4, 4):
            return None
        is_imm = B(w, 11, 11)
        return Insn("ccmp", sf=sf, sub=B(w, 30, 30), rn=zr(rn), rm=(rm if is_imm else zr(rm)), is_imm=is_imm,
                    cond=B(w, 15, 12), nzcv=B(w, 3, 0))
    if (w & 0x1FE00000) == 0x1A800000:
        if B(w, 29, 29) or B(w, 11, 11):
            return None
        return Insn("csel", sf=sf, inv=B(w, 30, 30), inc=B(w, 10, 10), rd=zr(rd), rn=zr(rn), rm=zr(rm),
                    cond=B(w, 15, 12))
    if (w & 0x5FE00000) == 0x1AC00000:
        if B(w, 29, 29):
            return None
        opcode = B(w, 15, 10)
        kinds = {2: "udiv", 3: "sdiv", 8: "lslv", 9: "lsrv", 10: "asrv", 11: "rorv"}
        if opcode in kinds:
            return Insn("dp2", sf=sf, kind=kinds[opcode], rd=zr(rd), rn=zr(rn), rm=zr(rm))
        if opcode & 0x38 == 0x10:   # CRC32
            sz, c = opcode & 3, (opcode >> 2) & 1
            if (sz == 3) != bool(sf):
                return None
            return Insn("crc32", size=sz, c=c, rd=zr(rd), rn=zr(rn), rm=zr(rm))
        return None
    if (w & 0x5FE00000) == 0x5AC00000:
        if B(w, 29, 29) or B(w, 20, 16):
            return None
        opcode = B(w, 15, 10)
        if sf:
            kinds = {0: "rbit", 1: "rev16", 2: "rev32", 3: "rev", 4: "clz", 5: "cls"}
        else:
            kinds = {0: "rbit", 1: "rev16", 2: "rev", 4: "clz", 5: "cls"}
        if opcode not in kinds:
            return None
        return Insn("dp1", sf=sf, kind=kinds[opcode], rd=zr(rd), rn=zr(rn))
    if (w & 0x1F000000) == 0x1B000000:
        op54, op31, o0 = B(w, 30, 29), B(w, 23, 21), B(w, 15, 15)
        if op54:
            return None
        kinds = {(0, 0): "madd", (0, 1): "msub", (1, 0): "smaddl", (1, 1): "smsubl", (2, 0): "smulh",
                 (5, 0): "umaddl", (5, 1): "umsubl", (6, 0): "umulh"}
        k = kinds.get((op31, o0))
        if k is None or (k not in ("madd", "msub") and not sf):
            return None
        return Insn("dp3", sf=sf, kind=k, rd=zr(rd), rn=zr(rn), rm=zr(rm), ra=zr(B(w, 14, 10)))
    return None


def dec_simd_fp(w, pc):
    rd, rn, rm = B(w, 4, 0), B(w, 9, 5), B(w, 20, 16)
    q, u, size = B(w, 30, 30), B(w, 29, 29), B(w, 23, 22)
    # crypto
    if (w & 0xFF3E0C00) == 0x4E280800:
        k = {4: "aese", 5: "aesd", 6: "aesmc", 7: "aesimc"}.get(B(w, 16, 12))
        return Insn("crypto_aes", kind=k, rd=rd, rn=rn) if k and size == 0 else None
    if (w & 0xFF208C00) == 0x5E000000:
        k = {0: "sha1c", 1: "sha1p", 2: "sha1m", 3: "sha1su0", 4: "sha256h", 5: "sha256h2", 6: "sha256su1"}.get(B(w, 14, 12))
        return Insn("crypto_sha3", kind=k, rd=rd, rn=rn, rm=rm) if k and size == 0 else None
    if (w & 0xFF3E0C00) == 0x5E280800:
        k = {0: "sha1h", 1: "sha1su1", 2: "sha256su0"}.get(B(w, 16, 12))
        return Insn("crypto_sha2", kind=k, rd=rd, rn=rn) if k and size == 0 else None
    # scalar groups (bits 31:30 = 01, bit 28 = 1)
    if (w & 0xDFE08400) == 0x5E000400:
        return Insn("s_copy", opbit=u, imm5=B(w, 20, 16), imm4=B(w, 14, 11), rd=rd, rn=rn)
    if (w & 0xDF3E0C00) == 0x5E200800:
        return Insn("s_misc", u=u, size=size, opcode=B(w, 16, 12), rd=rd, rn=rn)
    if (w & 0xDF3E0C00) == 0x5E300800:
        return Insn("s_pair", u=u, size=size, opcode=B(w, 16, 12), rd=rd, rn=rn)
    if (w & 0xDF200C00) == 0x5E200000:
        return Insn("s_diff", u=u, size=size, opcode=B(w, 15, 12), rd=rd, rn=rn, rm=rm)
    if (w & 0xDF200400) == 0x5E200400:
        return Insn("s_same", u=u, size=size, opcode=B(w, 15, 11), rd=rd, rn=rn, rm=rm)
    if (w & 0xDF800400) == 0x5F000400:
        if B(w, 22, 19) == 0:
            return None
        return Insn("s_shift", u=u, immh=B(w, 22, 19), immb=B(w, 18, 16), opcode=B(w, 15, 11), rd=rd, rn=rn)
    if (w & 0xDF000400) == 0x5F000000:
        return Insn("s_elem", u=u, size=size, l=B(w, 21, 21), m=B(w, 20, 20), rm=B(w, 19, 16), opcode=B(w, 15, 12),
                    h=B(w, 11, 11), rd=rd, rn=rn)
    # vector groups (bit 31 = 0, bit 28 = 0)
    if (w & 0xBF208C00) == 0x0E000000:
        return Insn("v_tbl", q=q, len=B(w, 14, 13) + 1, tbx=B(w, 12, 12), rd=rd, rn=rn, rm=rm) if size == 0 else None
    if (w & 0xBF208C00) == 0x0E000800:
        k = {1: "uzp1", 2: "trn1", 3: "zip1", 5: "uzp2", 6: "trn2", 7: "zip2"}.get(B(w, 14, 12))
        if not k or (size == 3 and not q):
            return None
        return Insn("v_perm", kind=k, q=q, size=size, rd=rd, rn=rn, rm=rm)
    if (w & 0xBF208400) == 0x2E000000:
        imm4 = B(w, 14, 11)
        if size != 0 or (not q and imm4 & 8):
            return None
        return Insn("v_ext", q=q, imm4=imm4, rd=rd, rn=rn, rm=rm)
    if (w & 0x9FE08400) == 0x0E000400:
        return Insn("v_copy", q=q, opbit=u, imm5=B(w, 20, 16), imm4=B(w, 14, 11), rd=rd, rn=rn)
    if (w & 0x9F200400) == 0x0E200400:
        return Insn("v_same", q=q, u=u, size=size, opcode=B(w, 15, 11), rd=rd, rn=rn, rm=rm)
    if (w & 0x9F200C00) == 0x0E200000:
        return Insn("v_diff", q=q, u=u, size=size, opcode=B(w, 15, 12), rd=rd, rn=rn, rm=rm)
    if (w & 0x9F3E0C00) == 0x0E200800:
        return Insn("v_misc", q=q, u=u, size=size, opcode=B(w, 16, 12), rd=rd, rn=rn)
    if (w & 0x9F3E0C00) == 0x0E300800:
        return Insn("v_across", q=q, u=u, size=size, opcode=B(w, 16, 12), rd=rd, rn=rn)
    if (w & 0x9FF80400) == 0x0F000400:
        cmode, op = B(w, 15, 12), u
        imm8 = (B(w, 18, 16) << 5) | B(w, 9, 5)
        imm = adv_simd_expand_imm(op, cmode, imm8)
        if imm is None:
            return None
        return Insn("v_modimm", q=q, opbit=op, cmode=cmode, o2=B(w, 11, 11), imm=imm, imm8=imm8, rd=rd)
    if (w & 0x9F800400) == 0x0F000400:
        return Insn("v_shift", q=q, u=u, immh=B(w, 22, 19), immb=B(w, 18, 16), opcode=B(w, 15, 11), rd=rd, rn=rn)
    if (w & 0x9F000400) == 0x0F000000:
        return Insn("v_elem", q=q, u=u, size=size, l=B(w, 21, 21), m=B(w, 20, 20), rm=B(w, 19, 16), opcode=B(w, 15, 12),
                    h=B(w, 11, 11), rd=rd, rn=rn)
    # scalar floating point (bit 30 = 0, bits 28:24 = 11110 / 11111)
    if (w & 0x5F000000) == 0x1F000000:
        return Insn("fp_dp3", ftype=size, o1=B(w, 21, 21), o0=B(w, 15, 15), rd=rd, rn=rn, rm=rm, ra=B(w, 14, 10))
    if (w & 0x5F200000) == 0x1E000000:
        sf = w >> 31
        return Insn("fcvt_fixed", sf=sf, ftype=size, rmode=B(w, 20, 19), opcode=B(w, 18, 16), scale=B(w, 15, 10),
                    rd=rd, rn=rn)
    if (w & 0x5F20FC00) == 0x1E200000:
        return Insn("fcvt_int", sf=w >> 31, ftype=size, rmode=B(w, 20, 19), opcode=B(w, 18, 16), rd=rd, rn=rn)
    if (w & 0x5F207C00) == 0x1E204000:
        return Insn("fp_dp1", ftype=size, opcode=B(w, 20, 15), rd=rd, rn=rn)
    if (w & 0x5F203C00) == 0x1E202000:
        return Insn("fcmp", ftype=size, rn=rn, rm=rm, opc=B(w, 4, 0))
    if (w & 0x5F201C00) == 0x1E201000:
        if B(w, 9, 5):
            return None
        return Insn("fmov_imm", ftype=size, rd=rd, imm8=B(w, 20, 13))
    if (w & 0x5F200C00) == 0x1E200400:
        return Insn("fccmp", ftype=size, rn=rn, rm=rm, cond=B(w, 15, 12), e=B(w, 4, 4), nzcv=B(w, 3, 0))
    if (w & 0x5F200C00) == 0x1E200800:
        return Insn("fp_dp2", ftype=size, opcode=B(w, 15, 12), rd=rd, rn=rn, rm=rm)
    if (w & 0x5F200C00) == 0x1E200C00:
        return Insn("fcsel", ftype=size, rd=rd, rn=rn, rm=rm, cond=B(w, 15, 12))
    return None


def decode(w, pc):
    op0 = B(w, 28, 25)
    try:
        if w >> 16 == 0 or w == 0xE7FFDEFE:
            ins = Insn("udf", imm=w & 0xFFFF)
        elif op0 in (0b1000, 0b1001):
            ins = dec_dp_imm(w, pc)
        elif op0 in (0b1010, 0b1011):
            ins = dec_branch_sys(w, pc)
        elif op0 & 0b0101 == 0b0100:
            ins = dec_ldst(w, pc)
        elif op0 & 0b0111 == 0b0101:
            ins = dec_dp_reg(w, pc)
        elif op0 & 0b0111 == 0b0111:
            ins = dec_simd_fp(w, pc)
        else:
            ins = None
    except (KeyError, IndexError):
        ins = None
    if ins is None:
        ins = Insn("unknown")
    ins.pc, ins.word = pc, w
    return ins
