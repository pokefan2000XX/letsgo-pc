// Maxwell (SM 5.x) machine code -> GLSL (Vulkan) translator (see maxwell.h).
//
// Instruction semantics and field layouts follow Ryujinx's Ryujinx.Graphics.Shader (MIT);
// maxwell_ops.inc, the opcode table, is generated from it.
//
// Model: R0..R254 are uint locals holding bit patterns (RZ reads as 0), P0..P6 are bools
// (PT is true) and cc_c is the carry flag of integer adds. Each instruction becomes one
// GLSL block. Straight-line programs translate in order; programs with branches become a
// dispatch loop (`for (;;) switch (pc)`) with a stack for SSY/SYNC reconvergence points.
#include "maxwell.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace maxwell {
namespace {

using nvasm::Stage;

struct Pattern {
    uint64_t mask, value;
    const char* name;
};
const Pattern kPatterns[] = {
#define X(m, v, n) {m, v, #n},
#include "maxwell_ops.inc"
#undef X
};

const char* decode(uint64_t w) {
    for (const Pattern& p : kPatterns)
        if ((w & p.mask) == p.value) return p.name;
    return "?";
}

// branch target of BRA / SSY: relative to the next instruction; a target at the start of a
// group (its scheduling word) means the group's first instruction
uint32_t target(uint32_t pc, uint64_t w) {
    uint32_t t = pc + 8 + (uint32_t)((int32_t)((uint32_t)(w >> 20) << 8) >> 8);
    return (t & 31) == 0 ? t + 8 : t;
}

// BRA to itself (or to the scheduling word before it): the padding after the last instruction
bool end_marker(uint64_t w) { return w == 0xE2400FFFFF87000Full || w == 0xE2400FFFFF07000Full; }

struct Fail {
    std::string msg;
};
[[noreturn]] void fail(const std::string& m) { throw Fail{m}; }

uint32_t bits(uint64_t w, int lo, int n) { return (uint32_t)((w >> lo) & ((1ull << n) - 1)); }
bool bit(uint64_t w, int b) { return (w >> b) & 1; }
int32_t sext(uint32_t v, int n) { return (int32_t)(v << (32 - n)) >> (32 - n); }
uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint64_t rd64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

std::string hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08Xu", v);
    return b;
}
std::string dec(int v) { return std::to_string(v); }
const char* comp(int c) { return c == 0 ? "x" : c == 1 ? "y" : c == 2 ? "z" : "w"; }

struct Ins {
    uint32_t pc;             // byte offset from the start of the instruction stream
    uint64_t w;
    std::string name;        // Ryujinx emitter name: FfmaR, FfmaC, Texs, ...
};

struct Cx {
    const nvasm::Options* opt = nullptr;
    Stage stage = Stage::Vertex;
    std::vector<uint32_t> consts;            // the compiler's constants (hardware c[1])
    uint8_t imap[32] = {};                   // fragment: interpolation of generic input N (SPH):
                                             // 0 unused, 1 constant, 2 perspective, 3 screen-linear
    uint32_t omap = 0;                       // fragment: render target components written (SPH)
    bool omap_depth = false;
    std::ostringstream body;
    int indent = 1;
    std::set<int> regs;
    uint32_t preds = 0;
    bool flow = false, ssy = false, cc = false, const_array = false, ldg = false, point_size = false;
    bool bfe = false, tex_array = false;
    bool uses_scale = false, uses_fc = false;   // the render scale's declarations; fc_ (nvasm.h)
    nvasm::Reflection refl;
    std::string where;                       // the instruction being translated, for errors

    void line(const std::string& s) { body << std::string((size_t)indent * 4, ' ') << s << "\n"; }
};

// ---- operands ------------------------------------------------------------------------------------
std::string R(Cx& cx, int r) {
    if (r >= 255) return "0u";
    cx.regs.insert(r);
    return "R" + dec(r);
}
void set(Cx& cx, int d, const std::string& v) {
    if (d >= 255) return;
    cx.line(R(cx, d) + " = " + v + ";");
}
std::string F(const std::string& u) { return "uintBitsToFloat(" + u + ")"; }
std::string U(const std::string& f) { return "floatBitsToUint(" + f + ")"; }
std::string P(Cx& cx, int p, bool inv) {
    std::string s = "true";
    if (p != 7) {
        cx.preds |= 1u << p;
        s = "P" + dec(p);
    }
    return inv ? "!" + s : s;
}
void set_pred(Cx& cx, int p, const std::string& v) {
    if (p != 7) cx.line(P(cx, p, false) + " = " + v + ";");
}
std::string absneg(const std::string& f, bool abs, bool neg) {
    std::string s = abs ? "abs(" + f + ")" : f;
    return neg ? "(-" + s + ")" : s;
}
std::string sat(const std::string& f, bool s) { return s ? "clamp(" + f + ", 0.0, 1.0)" : f; }
std::string bop(int op, const std::string& a, const std::string& b) {
    switch (op) {
    case 0: return "(" + a + " && " + b + ")";
    case 1: return "(" + a + " || " + b + ")";
    case 2: return "(" + a + " != " + b + ")";
    }
    fail("predicate operation " + dec(op));
}

// word index expression `w` of hardware constant buffer `slot`
std::string cbuf_dyn(Cx& cx, int slot, const std::string& w) {
    int b;
    if (slot == 1) {
        cx.const_array = true;
        return "c1w(" + w + ")";
    }
    if (slot == 0) b = 15;
    else if (slot == 2) {
        cx.refl.cbufs |= 1u << 14;
        return "cb14w((" + w + ") - 8u)";
    } else if (slot >= 3 && slot <= 16) b = slot - 3;
    else fail("constant buffer slot " + dec(slot));
    cx.refl.cbufs |= 1u << b;
    return "cb" + dec(b) + "w(" + w + ")";
}
// word `w` of constant buffer `slot` (the compiler's constants are inlined)
std::string cbuf(Cx& cx, int slot, uint32_t w) {
    if (slot == 1) return hex(w < cx.consts.size() ? cx.consts[w] : 0u);
    if (slot == 2 && w < 8) fail("constant buffer 2 below the texture handles");
    return cbuf_dyn(cx, slot, hex(w));
}

// 20-bit immediate (sign at bit 56): a float's top bits, or a sign-extended integer
uint32_t imm20(uint64_t w) { return bits(w, 20, 19) | (uint32_t)bit(w, 56) << 19; }
std::string imm20f(uint64_t w) { return hex(imm20(w) << 12); }
std::string imm20i(uint64_t w) { return hex((uint32_t)sext(imm20(w), 20)); }

// operand B of the R / C / I forms (bits)
std::string srcb(Cx& cx, uint64_t w, char form, bool float_imm) {
    switch (form) {
    case 'R': return R(cx, bits(w, 20, 8));
    case 'C': return cbuf(cx, bits(w, 34, 5), bits(w, 20, 14));
    case 'I': return float_imm ? imm20f(w) : imm20i(w);
    }
    fail("operand form");
}

std::string fcmp(int c, const std::string& a, const std::string& b) {
    std::string nan = "(isnan(" + a + ") || isnan(" + b + "))";
    switch (c) {
    case 0: return "false";
    case 1: return "(" + a + " < " + b + ")";
    case 2: return "(" + a + " == " + b + ")";
    case 3: return "(" + a + " <= " + b + ")";
    case 4: return "(" + a + " > " + b + ")";
    case 5: return "(" + a + " != " + b + " && !" + nan + ")";
    case 6: return "(" + a + " >= " + b + ")";
    case 7: return "!" + nan;
    case 8: return nan;
    case 9: return "!(" + a + " >= " + b + ")";
    case 10: return "(" + a + " == " + b + " || " + nan + ")";
    case 11: return "!(" + a + " > " + b + ")";
    case 12: return "!(" + a + " <= " + b + ")";
    case 13: return "(" + a + " != " + b + " || " + nan + ")";
    case 14: return "!(" + a + " < " + b + ")";
    default: return "true";
    }
}
std::string icmp(int c, const std::string& a, const std::string& b, bool s) {
    std::string x = s ? "int(" + a + ")" : a, y = s ? "int(" + b + ")" : b;
    switch (c) {
    case 0: return "false";
    case 1: return "(" + x + " < " + y + ")";
    case 2: return "(" + x + " == " + y + ")";
    case 3: return "(" + x + " <= " + y + ")";
    case 4: return "(" + x + " > " + y + ")";
    case 5: return "(" + x + " != " + y + ")";
    case 6: return "(" + x + " >= " + y + ")";
    default: return "true";
    }
}

// integer source of I2F / I2I: format (bit 2 signed; 0 8-bit, 1 16-bit, 2 32-bit) and byte select
std::string int_src(const std::string& b, int fmt, int sel, bool as_int) {
    bool s = fmt & 4;
    switch (fmt & 3) {
    case 0: return s ? "bitfieldExtract(int(" + b + "), " + dec(8 * sel) + ", 8)"
                     : (as_int ? "int" : "") + std::string("((") + b + " >> " + dec(8 * sel) + "u) & 0xFFu)";
    case 1: return s ? "bitfieldExtract(int(" + b + "), " + dec(16 * (sel >> 1)) + ", 16)"
                     : (as_int ? "int" : "") + std::string("((") + b + " >> " + dec(16 * (sel >> 1)) + "u) & 0xFFFFu)";
    case 2: return s || as_int ? "int(" + b + ")" : b;
    }
    fail("64-bit integer conversion");
}

// ---- attributes ----------------------------------------------------------------------------------
int interp(Cx& cx, int i) {
    for (int c = 0; c < 4; c++)
        if (int m = (cx.imap[i] >> (2 * c)) & 3) return m;
    return 0;
}

std::string fragment_input(Cx& cx, uint32_t at, bool multiply) {
    if (cx.stage != Stage::Fragment) fail("IPA outside a fragment program");
    if (at >= 0x70 && at < 0x80) {
        cx.uses_scale = cx.uses_fc = true;
        return std::string("fc_.") + comp((int)(at - 0x70) / 4);
    }
    if (at >= 0x80 && at < 0x280) {
        int i = (int)(at - 0x80) >> 4;
        cx.refl.inputs |= 1u << i;
        std::string v = "fi_" + dec(i) + "." + comp((int)(at >> 2) & 3);
        // the hardware interpolates perspective inputs as attr/w and the program multiplies by w
        int m = interp(cx, i);
        if (m == 2 || (m == 0 && multiply)) v = "(" + v + " * gl_FragCoord.w)";
        return v;
    }
    if (at == 0x2E0 || at == 0x2E4) return std::string("gl_PointCoord.") + comp((int)(at - 0x2E0) / 4);
    if (at == 0x3FC) return "uintBitsToFloat(gl_FrontFacing ? 0xFFFFFFFFu : 0u)";
    char b[64];
    snprintf(b, sizeof b, "fragment input a[0x%x]", at);
    fail(b);
}

std::string vertex_input(Cx& cx, uint32_t at) {
    if (cx.stage != Stage::Vertex) fail("ALD outside a vertex program");
    if (at >= 0x80 && at < 0x280) {
        int i = (int)(at - 0x80) >> 4;
        cx.refl.inputs |= 1u << i;
        std::string v = "va_" + dec(i) + "." + comp((int)(at >> 2) & 3);
        if (i < 16 && (cx.opt->sint_inputs >> i & 1)) return "uint(" + v + ")";
        if (i < 16 && (cx.opt->uint_inputs >> i & 1)) return v;
        return U(v);
    }
    if (at == 0x2F8) return "uint(gl_InstanceIndex - gl_BaseInstance)";
    if (at == 0x2FC) return "uint(gl_VertexIndex)";
    char b[64];
    snprintf(b, sizeof b, "vertex input a[0x%x]", at);
    fail(b);
}

void vertex_output(Cx& cx, uint32_t at, const std::string& v) {
    if (cx.stage != Stage::Vertex) fail("AST outside a vertex program");
    if (at >= 0x70 && at < 0x80) {
        cx.line(std::string("gl_Position.") + comp((int)(at - 0x70) / 4) + " = " + F(v) + ";");
    } else if (at == 0x6C) {
        cx.point_size = true;
        cx.line("gl_PointSize = " + F(v) + ";");
    } else if (at >= 0x80 && at < 0x280) {
        int i = (int)(at - 0x80) >> 4;
        cx.refl.outputs |= 1u << i;
        cx.line("vo_" + dec(i) + "." + comp((int)(at >> 2) & 3) + " = " + F(v) + ";");
    } else if (at >= 0x2C0 && at < 0x2E0) {
        // clip distances: NVN enables none for these programs
    } else {
        char b[64];
        snprintf(b, sizeof b, "vertex output a[0x%x]", at);
        fail(b);
    }
}

void epilogue(Cx& cx) {
    if (cx.stage == Stage::Vertex) {
        if (cx.opt->gl_depth) cx.line("gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;");
        if (!cx.point_size) cx.line("gl_PointSize = 1.0;");
    } else if (cx.stage == Stage::Fragment) {
        // each enabled render target takes the next four registers, its components in order
        int base = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (cx.omap >> (4 * rt)) & 15;
            if (!m) continue;
            cx.refl.outputs |= 1u << rt;
            for (int c = 0; c < 4; c++)
                if (m & (1u << c)) cx.line("fo_" + dec(rt) + "." + comp(c) + " = " + F(R(cx, base + c)) + ";");
            base += 4;
        }
        if (cx.omap_depth) cx.line("gl_FragDepth = " + F(R(cx, __builtin_popcount(cx.omap) + 1)) + ";");
    }
}

// ---- textures -----------------------------------------------------------------------------------
// The sample of bound texture `tid` (TidB: its handle's word in constant buffer 2, binding 0 at word 8),
// as a vec4 expression: 2D or 2D array (`layer` is the integer layer register), optional depth
// compare `ref`, LOD 0 implicit / 1 zero / 2 bias / 3 level (`lod`), optional packed texel `offs`.
std::string sample(Cx& cx, int tid, bool array, const std::string& x, const std::string& y, const std::string& layer, int lod_mode,
                   const std::string& lod, const std::string& ref, const std::string& offs) {
    if (tid < 8 || tid >= 8 + 64) fail("texture handle offset " + dec(tid));
    cx.refl.cbufs |= 1u << 14;
    cx.refl.textures = true;
    if (array) cx.tex_array = true;
    cx.line("uint h_ = cb14w(" + dec(tid - 8) + "u);");
    const std::string ti = "nonuniformEXT(h_ & 0xFFFFFu)", si = "nonuniformEXT(h_ >> 20u)";
    std::string tex = array ? "g_texa[" + ti + "]" : "g_tex[" + ti + "]";
    bool shadow = !ref.empty();
    std::string smp = std::string(shadow ? (array ? "sampler2DArrayShadow(" : "sampler2DShadow(") : (array ? "sampler2DArray(" : "sampler2D(")) +
                      tex + ", g_smp[" + si + "])";
    std::string uv = "vec2(" + x + ", " + y + ")";
    if (!offs.empty()) {
        // a texel offset moves the sample point by whole texels of level 0 (the game's, for a scaled texture)
        cx.uses_scale = true;
        cx.line("vec2 o_ = vec2(bitfieldExtract(int(" + offs + "), 0, 4), bitfieldExtract(int(" + offs + "), 4, 4)) / vec2(max(textureSize(" +
                tex + ", 0)" + (array ? ".xy" : "") + " / sk_(h_ & 0xFFFFFu), ivec2(1)));");
        uv = "(" + uv + " + o_)";
    }
    std::string l = array ? "float(" + layer + " & 0xFFFFu)" : "";
    std::string coord = array ? (shadow ? "vec4(" + uv + ", " + l + ", " + ref + ")" : "vec3(" + uv + ", " + l + ")")
                              : (shadow ? "vec3(" + uv + ", " + ref + ")" : uv);
    std::string r;
    if (shadow && array && lod_mode != 0) {
        if (lod_mode == 2) fail("depth-compare array sample with a LOD bias");
        r = "textureGrad(" + smp + ", " + coord + ", vec2(0.0), vec2(0.0))";      // no textureLod for these
    } else {
        switch (lod_mode) {
        case 0: r = "texture(" + smp + ", " + coord + ")"; break;
        case 1: r = "textureLod(" + smp + ", " + coord + ", 0.0)"; break;
        case 2: r = "texture(" + smp + ", " + coord + ", " + lod + ")"; break;
        default: r = "textureLod(" + smp + ", " + coord + ", " + lod + ")"; break;
        }
    }
    return shadow ? "vec4(" + r + ")" : r;
}

// ---- instructions --------------------------------------------------------------------------------
void check_ccc(uint64_t w) {
    if (bits(w, 0, 5) != 15) fail("condition-code test");
}

void emit_body(Cx& cx, const Ins& in, const std::string& base, char form) {
    const uint64_t w = in.w;
    const std::string& n = in.name;
    const int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);

    if (base == "Mov") {
        set(cx, d, form == 'R' ? R(cx, bits(w, 20, 8)) : srcb(cx, w, form, false));
    } else if (n == "Mov32i") {
        set(cx, d, hex(bits(w, 20, 32)));
    } else if (base == "Fadd") {
        std::string fa = absneg(F(R(cx, a)), bit(w, 46), bit(w, 48));
        std::string fb = absneg(F(srcb(cx, w, form, true)), bit(w, 49), bit(w, 45));
        set(cx, d, U(sat(fa + " + " + fb, bit(w, 50))));
    } else if (n == "Fadd32i") {
        std::string fa = absneg(F(R(cx, a)), bit(w, 54), bit(w, 56));
        std::string fb = absneg(F(hex(bits(w, 20, 32))), bit(w, 57), bit(w, 53));
        set(cx, d, U(fa + " + " + fb));
    } else if (base == "Fmul") {
        static const char* scale[8] = {"", " * 0.5", " * 0.25", " * 0.125", " * 8.0", " * 4.0", " * 2.0", ""};
        std::string m = F(R(cx, a)) + " * " + F(srcb(cx, w, form, true)) + scale[bits(w, 41, 3)];
        if (bit(w, 48)) m = "(-(" + m + "))";
        set(cx, d, U(sat(m, bit(w, 50))));
    } else if (n == "Fmul32i") {
        set(cx, d, U(sat(F(R(cx, a)) + " * " + F(hex(bits(w, 20, 32))), bit(w, 55))));
    } else if (base == "Ffma" || n == "Ffma32i") {
        std::string b, c;
        bool na = bit(w, 48), nc = bit(w, 49), s = bit(w, 50);
        if (form == 'R') b = R(cx, bits(w, 20, 8)), c = R(cx, bits(w, 39, 8));
        else if (form == 'C') b = cbuf(cx, bits(w, 34, 5), bits(w, 20, 14)), c = R(cx, bits(w, 39, 8));
        else if (form == 'I') b = imm20f(w), c = R(cx, bits(w, 39, 8));
        else if (form == 'r') b = R(cx, bits(w, 39, 8)), c = cbuf(cx, bits(w, 34, 5), bits(w, 20, 14));
        else {                                          // FFMA32I: C is the destination register
            b = hex(bits(w, 20, 32)), c = R(cx, d);
            na = bit(w, 56), nc = bit(w, 57), s = bit(w, 55);
        }
        std::string r = "fma(" + absneg(F(R(cx, a)), false, na) + ", " + F(b) + ", " + absneg(F(c), false, nc) + ")";
        set(cx, d, U(sat(r, s)));
    } else if (n == "Mufu") {
        std::string x = absneg(F(R(cx, a)), bit(w, 46), bit(w, 48)), r;
        switch (bits(w, 20, 4)) {
        case 0: r = "cos(" + x + ")"; break;           // RRO already turned the angle into radians
        case 1: r = "sin(" + x + ")"; break;
        case 2: r = "exp2(" + x + ")"; break;
        case 3: r = "log2(" + x + ")"; break;
        case 4: r = "(1.0 / " + x + ")"; break;
        case 5: r = "inversesqrt(" + x + ")"; break;
        case 8: r = "sqrt(" + x + ")"; break;
        default: fail("MUFU operation " + dec((int)bits(w, 20, 4)));
        }
        set(cx, d, U(sat(r, bit(w, 50))));
    } else if (base == "Rro") {
        // range reduction ahead of MUFU: a move here (MUFU's sin/cos take radians)
        set(cx, d, U(absneg(F(srcb(cx, w, form, true)), bit(w, 49), bit(w, 45))));
    } else if (base == "Fsetp") {
        std::string fa = absneg(F(R(cx, a)), bit(w, 7), bit(w, 43));
        std::string fb = absneg(F(srcb(cx, w, form, true)), bit(w, 44), bit(w, 6));
        cx.line("float a_ = " + fa + ", b_ = " + fb + ";");
        cx.line("bool r_ = " + fcmp((int)bits(w, 48, 4), "a_", "b_") + ", s_ = " + P(cx, bits(w, 39, 3), bit(w, 42)) + ";");
        int op = (int)bits(w, 45, 2);
        set_pred(cx, bits(w, 3, 3), bop(op, "r_", "s_"));
        set_pred(cx, bits(w, 0, 3), bop(op, "!r_", "s_"));
    } else if (base == "Fset") {
        std::string fa = absneg(F(R(cx, a)), bit(w, 54), bit(w, 43));
        std::string fb = absneg(F(srcb(cx, w, form, true)), bit(w, 44), bit(w, 53));
        cx.line("float a_ = " + fa + ", b_ = " + fb + ";");
        std::string r = bop((int)bits(w, 45, 2), fcmp((int)bits(w, 48, 4), "a_", "b_"), P(cx, bits(w, 39, 3), bit(w, 42)));
        set(cx, d, "(" + r + " ? " + (bit(w, 52) ? "0x3F800000u" : "0xFFFFFFFFu") + " : 0u)");
    } else if (base == "Fmnmx") {
        std::string fa = absneg(F(R(cx, a)), bit(w, 46), bit(w, 48));
        std::string fb = absneg(F(srcb(cx, w, form, true)), bit(w, 49), bit(w, 45));
        cx.line("float a_ = " + fa + ", b_ = " + fb + ";");
        set(cx, d, U("(" + P(cx, bits(w, 39, 3), bit(w, 42)) + " ? min(a_, b_) : max(a_, b_))"));
    } else if (n == "Ipa") {
        if (bit(w, 38)) fail("indexed IPA");
        bool mul = bits(w, 54, 2) == 1;
        std::string v = fragment_input(cx, bits(w, 28, 10), mul);
        if (mul) v = "(" + v + " * " + F(R(cx, bits(w, 20, 8))) + ")";
        set(cx, d, U(sat(v, bit(w, 51))));
    } else if (n == "Ald") {
        if (bits(w, 8, 8) != 255) fail("indexed attribute load");
        if (bit(w, 31) || bit(w, 32)) fail("patch or output attribute load");
        uint32_t at = bits(w, 20, 11);
        int cnt = (int)bits(w, 47, 2) + 1;
        std::vector<std::string> v;
        for (int k = 0; k < cnt; k++) v.push_back(vertex_input(cx, at + 4 * (uint32_t)k));
        for (int k = 0; k < cnt; k++) set(cx, d == 255 ? 255 : d + k, v[(size_t)k]);
    } else if (n == "Ast") {
        if (bits(w, 8, 8) != 255) fail("indexed attribute store");
        if (bit(w, 31)) fail("patch attribute store");
        uint32_t at = bits(w, 20, 11);
        int cnt = (int)bits(w, 47, 2) + 1, v = (int)bits(w, 0, 8);
        for (int k = 0; k < cnt; k++) vertex_output(cx, at + 4 * (uint32_t)k, R(cx, v == 255 ? 255 : v + k));
    } else if (base == "Isetp") {
        if (bit(w, 43)) fail("ISETP.X");
        cx.line("bool r_ = " + icmp((int)bits(w, 49, 3), R(cx, a), srcb(cx, w, form, false), bit(w, 48)) +
                ", s_ = " + P(cx, bits(w, 39, 3), bit(w, 42)) + ";");
        int op = (int)bits(w, 45, 2);
        set_pred(cx, bits(w, 3, 3), bop(op, "r_", "s_"));
        set_pred(cx, bits(w, 0, 3), bop(op, "!r_", "s_"));
    } else if (base == "Iset") {
        if (bit(w, 43)) fail("ISET.X");
        std::string r = bop((int)bits(w, 45, 2), icmp((int)bits(w, 49, 3), R(cx, a), srcb(cx, w, form, false), bit(w, 48)),
                            P(cx, bits(w, 39, 3), bit(w, 42)));
        set(cx, d, "(" + r + " ? " + (bit(w, 44) ? "0x3F800000u" : "0xFFFFFFFFu") + " : 0u)");
    } else if (base == "Psetp" || n == "Psetp") {
        cx.line("bool ab_ = " + bop((int)bits(w, 24, 2), P(cx, bits(w, 29, 3), bit(w, 32)), P(cx, bits(w, 12, 3), bit(w, 15))) +
                ", c_ = " + P(cx, bits(w, 39, 3), bit(w, 42)) + ";");
        int op = (int)bits(w, 45, 2);
        set_pred(cx, bits(w, 3, 3), bop(op, "ab_", "c_"));
        set_pred(cx, bits(w, 0, 3), bop(op, "!ab_", "c_"));
    } else if (base == "Lop" || n == "Lop32i") {
        std::string x = R(cx, a), y;
        int op, dp = 7, pop = 0;
        if (n == "Lop32i") {
            if (bit(w, 57)) fail("LOP32I.X");
            y = hex(bits(w, 20, 32));
            op = (int)bits(w, 53, 2);
            if (bit(w, 55)) x = "~" + x;
            if (bit(w, 56)) y = "~" + y;
        } else {
            if (bit(w, 43)) fail("LOP.X");
            y = srcb(cx, w, form, false);
            op = (int)bits(w, 41, 2);
            dp = (int)bits(w, 48, 3);
            pop = (int)bits(w, 44, 2);
            if (bit(w, 39)) x = "~" + x;
            if (bit(w, 40)) y = "~" + y;
        }
        std::string r = op == 0 ? "(" + x + " & " + y + ")" : op == 1 ? "(" + x + " | " + y + ")" : op == 2 ? "(" + x + " ^ " + y + ")" : y;
        cx.line("uint t_ = " + r + ";");
        if (dp != 7) set_pred(cx, dp, pop == 0 ? "false" : pop == 1 ? "true" : pop == 2 ? "t_ == 0u" : "t_ != 0u");
        set(cx, d, "t_");
    } else if (base == "Shl") {
        if (bit(w, 43)) fail("SHL.X");
        cx.line("uint s_ = " + srcb(cx, w, form, false) + ";");
        set(cx, d, bit(w, 39) ? "(" + R(cx, a) + " << (s_ & 31u))" : "(s_ >= 32u ? 0u : " + R(cx, a) + " << s_)");
    } else if (base == "Shr") {
        if (bit(w, 40)) fail("SHR.BREV");
        if (bits(w, 43, 2)) fail("SHR.X");
        cx.line("uint s_ = " + srcb(cx, w, form, false) + (bit(w, 39) ? " & 31u" : "") + ";");
        if (bit(w, 48)) set(cx, d, "uint(int(" + R(cx, a) + ") >> min(s_, 31u))");
        else set(cx, d, "(s_ >= 32u ? 0u : " + R(cx, a) + " >> s_)");
    } else if (base == "Bfe") {
        if (bit(w, 40)) fail("BFE.BREV");
        cx.bfe = true;
        set(cx, d, std::string(bit(w, 48) ? "bfe_s(" : "bfe_u(") + R(cx, a) + ", " + srcb(cx, w, form, false) + ")");
    } else if (base == "Xmad") {
        std::string b, c;
        bool hb = false, psl = false, mrg = false, x = false;
        int cop = 0;
        if (form == 'I') {
            b = hex(bits(w, 20, 16)), c = R(cx, bits(w, 39, 8));
            psl = bit(w, 36), mrg = bit(w, 37), x = bit(w, 38), cop = (int)bits(w, 50, 3);
        } else if (form == 'R') {
            b = R(cx, bits(w, 20, 8)), c = R(cx, bits(w, 39, 8));
            hb = bit(w, 35), psl = bit(w, 36), mrg = bit(w, 37), x = bit(w, 38), cop = (int)bits(w, 50, 3);
        } else if (form == 'C') {
            b = cbuf(cx, bits(w, 34, 5), bits(w, 20, 14)), c = R(cx, bits(w, 39, 8));
            hb = bit(w, 52), psl = bit(w, 55), mrg = bit(w, 56), x = bit(w, 54), cop = (int)bits(w, 50, 2);
        } else {
            b = R(cx, bits(w, 39, 8)), c = cbuf(cx, bits(w, 34, 5), bits(w, 20, 14));
            hb = bit(w, 52), x = bit(w, 54), cop = (int)bits(w, 50, 2);
        }
        auto ext = [](const std::string& v, bool hi, bool s) {
            return s && hi ? "uint(int(" + v + ") >> 16)" : s ? "uint(bitfieldExtract(int(" + v + "), 0, 16))"
                           : hi ? "(" + v + " >> 16u)" : "(" + v + " & 0xFFFFu)";
        };
        cx.line("uint a_ = " + ext(R(cx, a), bit(w, 53), bit(w, 48)) + ", b_ = " + ext(b, hb, bit(w, 49)) + ", c_ = " + c + ";");
        cx.line(std::string("uint p_ = a_ * b_") + (psl ? " << 16u" : "") + ";");
        switch (cop) {
        case 1: cx.line("c_ &= 0xFFFFu;"); break;
        case 2: cx.line("c_ >>= 16u;"); break;
        case 3: cx.line("c_ -= ((a_ >> 31u) << 16u) + ((b_ >> 31u) << 16u);"); break;
        case 4: cx.line("c_ += b_ << 16u;"); break;
        }
        if (x) {
            cx.cc = true;
            cx.line("uint t_ = p_ + c_ + cc_c;");
        } else {
            cx.line("uint t_ = p_ + c_;");
        }
        if (bit(w, 47)) {
            cx.cc = true;
            cx.line(x ? "cc_c = (t_ == p_ && cc_c != 0u) || t_ < p_ ? 1u : 0u;" : "cc_c = t_ < p_ ? 1u : 0u;");
        }
        if (mrg) cx.line("t_ = (t_ & 0xFFFFu) | (b_ << 16u);");
        set(cx, d, "t_");
    } else if (base == "Iscadd") {
        int avg = (int)bits(w, 48, 2);
        if (avg == 3) fail("ISCADD.PO");
        std::string x = "(" + R(cx, a) + " << " + dec((int)bits(w, 39, 5)) + "u)", y = srcb(cx, w, form, false);
        if (avg == 2) x = "(0u - " + x + ")";
        if (avg == 1) y = "(0u - " + y + ")";
        cx.line("uint a_ = " + x + ", t_ = a_ + " + y + ";");
        if (bit(w, 47)) {
            cx.cc = true;
            cx.line("cc_c = t_ < a_ ? 1u : 0u;");
        }
        set(cx, d, "t_");
    } else if (base == "Iadd" || n == "Iadd32i") {
        std::string b;
        int avg;
        bool x, wcc, s;
        if (n == "Iadd32i") {
            b = hex(bits(w, 20, 32)), avg = (int)bits(w, 55, 2), x = bit(w, 53), wcc = bit(w, 52), s = bit(w, 54);
        } else {
            b = srcb(cx, w, form, false), avg = (int)bits(w, 48, 2), x = bit(w, 43), wcc = bit(w, 47), s = bit(w, 50);
        }
        if (s) fail("IADD.SAT");
        std::string ea = R(cx, a), eb = b;
        if (avg == 2) ea = "(0u - " + ea + ")";
        else if (avg == 1) eb = "(0u - " + eb + ")";
        cx.line("uint a_ = " + ea + ", t_ = a_ + " + eb + (avg == 3 ? " + 1u" : "") + (x ? " + cc_c" : "") + ";");
        if (x) cx.cc = true;
        if (wcc) {
            cx.cc = true;
            cx.line(x ? "cc_c = (t_ == a_ && cc_c != 0u) || t_ < a_ ? 1u : 0u;" : "cc_c = t_ < a_ ? 1u : 0u;");
        }
        set(cx, d, "t_");
    } else if (base == "Imnmx") {
        if (bits(w, 43, 2)) fail("IMNMX.X");
        bool s = bit(w, 48);
        std::string x = R(cx, a), y = srcb(cx, w, form, false);
        if (s) x = "int(" + x + ")", y = "int(" + y + ")";
        std::string r = "(" + P(cx, bits(w, 39, 3), bit(w, 42)) + " ? min(" + x + ", " + y + ") : max(" + x + ", " + y + "))";
        set(cx, d, s ? "uint" + r : r);
    } else if (base == "Sel") {
        set(cx, d, "(" + P(cx, bits(w, 39, 3), bit(w, 42)) + " ? " + R(cx, a) + " : " + srcb(cx, w, form, false) + ")");
    } else if (base == "I2f") {
        int sf = (int)(((w >> 11) & 4) | ((w >> 10) & 3)), sel = (int)bits(w, 41, 2);
        if (bits(w, 8, 2) != 2) fail("I2F to a format other than F32");
        bool abs = bit(w, 49), neg = bit(w, 45);
        if (!(sf & 4) && (abs || neg)) fail("I2F.U with abs/neg");
        std::string v = int_src(srcb(cx, w, form, false), sf, sel, false);
        if (abs) v = "abs(" + v + ")";
        if (neg) v = "(-" + v + ")";
        set(cx, d, U("float(" + v + ")"));
    } else if (base == "F2i") {
        int df = (int)(((w >> 10) & 4) | ((w >> 8) & 3)), rm = (int)bits(w, 39, 2);
        if (bits(w, 10, 2) != 2) fail("F2I from a format other than F32");
        std::string f = absneg(F(srcb(cx, w, form, true)), bit(w, 49), bit(w, 45));
        static const char* round[4] = {"roundEven", "floor", "ceil", "trunc"};
        cx.line(std::string("float f_ = ") + round[rm] + "(" + f + ");");
        std::string r;
        switch (df) {
        case 0: r = "uint(clamp(f_, 0.0, 255.0))"; break;
        case 1: r = "uint(clamp(f_, 0.0, 65535.0))"; break;
        case 2: r = "(f_ >= 4294967296.0 ? 0xFFFFFFFFu : uint(max(f_, 0.0)))"; break;
        case 4: r = "uint(int(clamp(f_, -128.0, 127.0)))"; break;
        case 5: r = "uint(int(clamp(f_, -32768.0, 32767.0)))"; break;
        case 6: r = "(f_ >= 2147483648.0 ? 0x7FFFFFFFu : uint(int(max(f_, -2147483648.0))))"; break;
        default: fail("F2I to a 64-bit integer");
        }
        set(cx, d, "(isnan(f_) ? 0u : " + r + ")");
    } else if (base == "I2i") {
        int df = (int)(((w >> 10) & 4) | ((w >> 8) & 3)), sf = (int)(((w >> 11) & 4) | ((w >> 10) & 3));
        if ((df & 3) == 3) fail("I2I to a 64-bit integer");
        std::string v = int_src(srcb(cx, w, form, false), sf, (int)bits(w, 41, 2), true);
        if (bit(w, 49)) v = "abs(" + v + ")";
        if (bit(w, 45)) v = "(-" + v + ")";
        cx.line("int v_ = " + v + ";");
        bool s = df & 4, satf = bit(w, 50);
        std::string r;
        switch (df & 3) {
        case 0: r = satf ? (s ? "uint(clamp(v_, -128, 127))" : "uint(clamp(v_, 0, 255))") : (s ? "uint(bitfieldExtract(v_, 0, 8))" : "(uint(v_) & 0xFFu)"); break;
        case 1: r = satf ? (s ? "uint(clamp(v_, -32768, 32767))" : "uint(clamp(v_, 0, 65535))") : (s ? "uint(bitfieldExtract(v_, 0, 16))" : "(uint(v_) & 0xFFFFu)"); break;
        default: r = satf && !s ? "uint(max(v_, 0))" : "uint(v_)"; break;
        }
        set(cx, d, r);
    } else if (n == "Ldc") {
        int sz = (int)bits(w, 48, 3), slot = (int)bits(w, 36, 5);
        if (bits(w, 44, 2)) fail("LDC address mode");
        if (sz > 5) fail("LDC.128");
        int32_t off = sext(bits(w, 20, 16), 16);
        int cnt = sz == 5 ? 2 : 1;
        static const char* ext_u[4] = {"0xFFu", "", "0xFFFFu", ""};
        std::vector<std::string> v;
        if (a == 255) {
            uint32_t byte = (uint32_t)off;
            for (int k = 0; k < cnt; k++) {
                std::string x = cbuf(cx, slot, byte / 4 + (uint32_t)k);
                if (sz < 4) {
                    int sh = sz < 2 ? (int)(byte & 3) * 8 : (int)(byte & 2) * 8;
                    x = sz & 1 ? "uint(bitfieldExtract(int(" + x + "), " + dec(sh) + ", " + (sz < 2 ? "8" : "16") + "))"
                               : "((" + x + " >> " + dec(sh) + "u) & " + ext_u[sz] + ")";
                }
                v.push_back(x);
            }
        } else {
            cx.line("uint o_ = " + R(cx, a) + " + " + hex((uint32_t)off) + ";");
            for (int k = 0; k < cnt; k++) {
                std::string x = cbuf_dyn(cx, slot, "(o_ >> 2u) + " + dec(k) + "u");
                if (sz < 4) {
                    std::string sh = sz < 2 ? "int((o_ & 3u) * 8u)" : "int((o_ & 2u) * 8u)";
                    x = sz & 1 ? "uint(bitfieldExtract(int(" + x + "), " + sh + ", " + (sz < 2 ? "8" : "16") + "))"
                               : "((" + x + " >> uint(" + sh + ")) & " + ext_u[sz] + ")";
                }
                v.push_back(x);
            }
        }
        for (int k = 0; k < cnt; k++) set(cx, d == 255 ? 255 : d + k, v[(size_t)k]);
    } else if (n == "Ldg") {
        int sz = (int)bits(w, 48, 3);
        int32_t off = sext(bits(w, 20, 24), 24);
        cx.ldg = true;
        cx.line("uvec2 ga_ = uvec2(" + R(cx, a) + ", " + (bit(w, 45) && a != 255 ? R(cx, a + 1) : "0u") + ");");
        if (off) {
            cx.line("uint k_; ga_.x = uaddCarry(ga_.x, " + hex((uint32_t)off) + ", k_);");
            cx.line("ga_.y += k_" + std::string(off < 0 ? " + 0xFFFFFFFFu" : "") + ";");
        }
        if (sz < 4) {
            cx.line("uint g_ = GW(uvec2(ga_.x & ~3u, ga_.y)).w[0];");
            std::string sh = sz < 2 ? "int((ga_.x & 3u) * 8u)" : "int((ga_.x & 2u) * 8u)";
            std::string len = sz < 2 ? "8" : "16";
            set(cx, d, sz & 1 ? "uint(bitfieldExtract(int(g_), " + sh + ", " + len + "))" : "bitfieldExtract(g_, " + sh + ", " + len + ")");
        } else {
            int cnt = sz == 4 ? 1 : sz == 5 ? 2 : 4;
            cx.line("GW g_ = GW(ga_);");
            std::vector<std::string> v;
            for (int k = 0; k < cnt; k++) cx.line("uint v" + dec(k) + "_ = g_.w[" + dec(k) + "];");
            for (int k = 0; k < cnt; k++) set(cx, d == 255 ? 255 : d + k, "v" + dec(k) + "_");
        }
    } else if (n == "Texs") {
        int tid = (int)bits(w, 36, 13), rb = (int)bits(w, 20, 8), target = (int)bits(w, 53, 4);
        auto ra = [&](int k) { return R(cx, a == 255 ? 255 : a + k); };
        auto rbk = [&](int k) { return R(cx, rb == 255 ? 255 : rb + k); };
        std::string r;
        switch (target) {
        case 1: r = sample(cx, tid, false, F(ra(0)), F(rbk(0)), "", 0, "", "", ""); break;
        case 2: r = sample(cx, tid, false, F(ra(0)), F(rbk(0)), "", 1, "", "", ""); break;
        case 3: r = sample(cx, tid, false, F(ra(0)), F(ra(1)), "", 3, F(rbk(0)), "", ""); break;
        case 4: r = sample(cx, tid, false, F(ra(0)), F(ra(1)), "", 0, "", F(rbk(0)), ""); break;
        case 6: r = sample(cx, tid, false, F(ra(0)), F(ra(1)), "", 1, "", F(rbk(0)), ""); break;
        // arrays: Ra is the layer (an integer), Ra + 1 and Rb the coordinates
        case 7: r = sample(cx, tid, true, F(ra(1)), F(rbk(0)), ra(0), 0, "", "", ""); break;
        case 8: r = sample(cx, tid, true, F(ra(1)), F(rbk(0)), ra(0), 1, "", "", ""); break;
        case 9: r = sample(cx, tid, true, F(ra(1)), F(rbk(0)), ra(0), 1, "", F(rbk(1)), ""); break;
        default: fail("TEXS target " + dec(target));
        }
        cx.line("vec4 t_ = " + r + ";");
        // write mask: components go to Dest, Dest + 1, Dest2, Dest2 + 1 in order
        static const int lut[2][8] = {{1, 2, 4, 8, 3, 9, 10, 12}, {7, 11, 13, 14, 15, 0, 0, 0}};
        int d2 = (int)bits(w, 28, 8), mask = lut[d2 == 255 ? 0 : 1][bits(w, 50, 3)];
        if (!mask) fail("TEXS write mask");
        for (int c = 0, k = 0; c < 4; c++) {
            if (!(mask & (1 << c))) continue;
            int r0 = k < 2 ? d : d2;
            set(cx, r0 == 255 ? 255 : r0 + (k & 1), U(std::string("t_.") + comp(c)));
            k++;
        }
    } else if (n == "Tex") {
        // Ra: [layer,] x, y; Rb: [lod or bias,] [packed offsets,] [depth reference]
        int dim = (int)bits(w, 28, 3), lod = (int)bits(w, 55, 3), mask = (int)bits(w, 31, 4);
        if (dim != 2 && dim != 3) fail("TEX dimension " + dec(dim));
        if (bit(w, 58)) fail("TEX.LC");
        if (lod == 4 || lod == 5) fail("TEX LOD mode " + dec(lod));
        int ra = a, rb = (int)bits(w, 20, 8);
        auto RA = [&]() { std::string v = R(cx, ra); if (ra != 255) ra++; return v; };
        auto RB = [&]() { std::string v = R(cx, rb); if (rb != 255) rb++; return v; };
        bool array = dim == 3;
        std::string layer = array ? RA() : "";
        std::string x = F(RA()), y = F(RA());
        int mode = lod == 0 ? 0 : lod == 1 ? 1 : (lod == 2 || lod == 6) ? 2 : 3;
        std::string lodv = lod > 1 ? F(RB()) : "";
        std::string offs = bit(w, 54) ? RB() : "";
        std::string ref = bit(w, 50) ? F(RB()) : "";
        cx.line("vec4 t_ = " + sample(cx, (int)bits(w, 36, 13), array, x, y, layer, mode, lodv, ref, offs) + ";");
        for (int c = 0, k = 0; c < 4; c++) {
            if (!(mask & (1 << c))) continue;
            set(cx, d == 255 ? 255 : d + k, U(std::string("t_.") + comp(c)));
            k++;
        }
    } else if (n == "Kil") {
        check_ccc(w);
        cx.line("discard;");
    } else if (n == "Exit") {
        check_ccc(w);
        epilogue(cx);
        cx.line("return;");
    } else if (n == "Bra") {
        check_ccc(w);
        if (bit(w, 5)) fail("BRA through a constant buffer");
        cx.line("pc = " + hex(target(in.pc, w)) + "; continue;");
    } else if (n == "Ssy") {
        if (bit(w, 5)) fail("SSY through a constant buffer");
        cx.line("ssy_[ssp_++] = " + hex(target(in.pc, w)) + ";");
    } else if (n == "Sync") {
        check_ccc(w);
        cx.line("pc = ssy_[--ssp_]; continue;");
    } else if (n == "Depbar" || n == "Nop") {
        // scheduling only
    } else {
        fail("unsupported instruction " + n);
    }
}

void emit(Cx& cx, const Ins& in) {
    const std::string& n = in.name;
    std::string base = n;
    char form = 0;
    size_t len = n.size();
    if (len > 3 && n.compare(len - 3, 3, "32i") == 0) form = '3';
    else if (len > 2 && n.compare(len - 2, 2, "Rc") == 0) form = 'r', base = n.substr(0, len - 2);
    else if (len > 1 && (n[len - 1] == 'R' || n[len - 1] == 'C' || n[len - 1] == 'I') && islower((unsigned char)n[len - 2]))
        form = n[len - 1], base = n.substr(0, len - 1);
    int pg = (int)bits(in.w, 16, 3);
    bool pinv = bit(in.w, 19), has_pred = n != "Ssy";
    if (has_pred && pg == 7 && pinv) return;                // @!PT: never executes
    char c[64];
    snprintf(c, sizeof c, "// %04x %s", in.pc, n.c_str());
    cx.line(c);
    bool guarded = has_pred && pg != 7;
    cx.line(guarded ? "if (" + P(cx, pg, pinv) + ") {" : "{");
    cx.indent++;
    emit_body(cx, in, base, form);
    cx.indent--;
    cx.line("}");
}

}  // namespace

size_t data_size(const uint8_t* control, size_t control_size) {
    if (control_size < 0x710 || rd32(control) != 0x98761234u) return 0;
    uint32_t total = rd32(control + 0x704);
    return total >= 0x80 && total <= (16u << 20) ? total : 0;
}

nvasm::Result translate(const uint8_t* control, size_t control_size, const uint8_t* code, size_t code_size,
                        const nvasm::Options& opt) {
    nvasm::Result res;
    Cx cx;
    cx.opt = &opt;
    try {
        if (!data_size(control, control_size)) fail("not an NVN control section");
        uint32_t const_size = rd32(control + 0x6FC), const_off = rd32(control + 0x700), sph = rd32(control + 0x708);
        if ((uint64_t)sph + 0x50 > code_size) fail("shader program header out of range");
        if (const_size && (uint64_t)const_off + const_size > code_size) fail("constants out of range");
        for (uint32_t i = 0; i + 4 <= const_size; i += 4) cx.consts.push_back(rd32(code + const_off + i));
        // shader program header
        uint32_t h[20];
        for (int i = 0; i < 20; i++) h[i] = rd32(code + sph + 4 * i);
        switch ((h[0] >> 10) & 15) {
        case 1: cx.stage = Stage::Vertex; break;
        case 5: cx.stage = Stage::Fragment; break;
        default: fail("shader type " + dec((int)((h[0] >> 10) & 15)));
        }
        if (cx.stage == Stage::Fragment) {
            for (int i = 0; i < 32; i++) cx.imap[i] = (uint8_t)(h[6 + (i >> 2)] >> ((i & 3) * 8));
            cx.omap = h[18];
            cx.omap_depth = (h[19] >> 1) & 1;
        }
        cx.refl.stage = cx.stage;
        // instructions, up to the padding branch (or the constants)
        std::vector<Ins> prog;
        size_t start = sph + 0x50, end = const_off > start ? std::min<size_t>(const_off, code_size) : code_size;
        bool ended = false;
        for (size_t i = start, k = 0; i + 8 <= end; i += 8, k++) {
            if (k % 4 == 0) continue;                       // scheduling word
            uint64_t w = rd64(code + i);
            if (end_marker(w)) {
                ended = true;
                break;
            }
            prog.push_back(Ins{(uint32_t)(i - start), w, decode(w)});
        }
        if (!ended) fail("no end of program found");
        // branch targets start the cases of the dispatch loop
        std::set<uint32_t> pcs, leaders = {prog.empty() ? 0u : prog[0].pc};
        for (auto& in : prog) pcs.insert(in.pc);
        for (auto& in : prog) {
            if (in.name == "Bra" || in.name == "Ssy" || in.name == "Sync") cx.flow = true;
            if (in.name == "Ssy") cx.ssy = true;
            if (in.name == "Bra" || in.name == "Ssy") {
                uint32_t t = target(in.pc, in.w);
                if (!pcs.count(t)) fail("branch target outside the program");
                leaders.insert(t);
            }
        }
        // two passes: the first finds what later instructions affect (point size written, ...)
        for (int pass = 0; pass < 2; pass++) {
            cx.body.str("");
            cx.indent = cx.flow ? 3 : 1;
            for (auto& in : prog) {
                cx.where = in.name;
                if (cx.flow && leaders.count(in.pc)) {
                    cx.indent--;
                    cx.line("case " + hex(in.pc) + ":");
                    cx.indent++;
                }
                emit(cx, in);
            }
        }
        cx.where.clear();
        // ---- assemble the GLSL ----
        std::ostringstream g;
        bool vs = cx.stage == Stage::Vertex;
        g << "#version 460\n#extension GL_EXT_nonuniform_qualifier : require\n#extension GL_EXT_samplerless_texture_functions : require\n";
        if (cx.ldg) g << "#extension GL_EXT_buffer_reference : require\n#extension GL_EXT_buffer_reference_uvec2 : require\n";
        g << "// " << (vs ? "vertex" : "fragment") << " program translated from Maxwell machine code by letsgo-pc\n";
        if (cx.refl.textures) {
            g << "layout(set = 0, binding = 0) uniform texture2D g_tex[];\n"
                 "layout(set = 0, binding = 3) uniform sampler g_smp[];\n";
            if (cx.tex_array) g << "layout(set = 0, binding = 4) uniform texture2DArray g_texa[];\n";
        }
        int bbase = nvasm::kStageBase[(int)cx.stage];
        for (int i = 0; i < nvasm::kMaxCbufs; i++)
            if (cx.refl.cbufs & (1u << i))
                g << "layout(std140, set = 1, binding = " << bbase + i << ") uniform CB" << i << " { uvec4 cb" << i
                  << "[4096]; };\nuint cb" << i << "w(uint i) { return cb" << i << "[i >> 2u][i & 3u]; }\n";
        if (cx.ldg) g << "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GW { uint w[]; };\n";
        for (int i = 0; i < 32; i++) {
            if (!(cx.refl.inputs & (1u << i))) continue;
            if (vs) {
                const char* t = i < 16 && (opt.sint_inputs >> i & 1) ? "ivec4" : i < 16 && (opt.uint_inputs >> i & 1) ? "uvec4" : "vec4";
                g << "layout(location = " << i << ") in " << t << " va_" << i << ";\n";
            } else {
                int m = interp(cx, i);
                g << "layout(location = " << i << ") " << (m == 1 ? "flat " : m == 3 ? "noperspective " : "") << "in vec4 fi_" << i << ";\n";
            }
        }
        for (int i = 0; i < 32; i++)
            if (cx.refl.outputs & (1u << i)) g << "layout(location = " << i << ") out vec4 " << (vs ? "vo_" : "fo_") << i << ";\n";
        if (cx.const_array) {
            size_t nc = std::max<size_t>(cx.consts.size(), 1);
            g << "const uint c1[" << nc << "] = uint[" << nc << "](";
            for (size_t i = 0; i < nc; i++) g << hex(i < cx.consts.size() ? cx.consts[i] : 0u) << (i + 1 < nc ? ", " : ");\n");
            g << "uint c1w(uint i) { return i < " << nc << "u ? c1[i] : 0u; }\n";
        }
        if (cx.bfe)
            g << "uint bfe_u(uint v, uint f) { uint p = f & 0xFFu, l = (f >> 8u) & 0xFFu; if (l == 0u || p >= 32u) return 0u;"
                 " return bitfieldExtract(v, int(p), int(min(l, 32u - p))); }\n"
                 "uint bfe_s(uint v, uint f) { uint p = f & 0xFFu, l = (f >> 8u) & 0xFFu; if (l == 0u) return 0u;"
                 " if (p >= 32u) return uint(int(v) >> 31); return uint(bitfieldExtract(int(v), int(p), int(min(l, 32u - p)))); }\n";
        if (cx.uses_scale) g << nvasm::scale_decls(cx.stage);
        g << "void main() {\n";
        if (cx.uses_fc) g << nvasm::kFragCoordLine;
        for (int r : cx.regs) g << "    uint R" << r << " = 0u;\n";
        for (int p = 0; p < 7; p++)
            if (cx.preds & (1u << p)) g << "    bool P" << p << " = false;\n";
        if (cx.cc) g << "    uint cc_c = 0u;\n";
        if (cx.flow) {
            g << "    uint pc = " << hex(prog.empty() ? 0u : prog[0].pc) << ";\n";
            if (cx.ssy) g << "    uint ssy_[16];\n    int ssp_ = 0;\n";
            g << "    for (;;) {\n        switch (pc) {\n" << cx.body.str() << "        }\n        break;\n    }\n";
        } else {
            g << cx.body.str();
        }
        g << "}\n";
        res.glsl = g.str();
        res.refl = cx.refl;
        res.ok = true;
    } catch (const Fail& f) {
        res.error = f.msg + (cx.where.empty() ? "" : "  [in: " + cx.where + "]");
    }
    return res;
}

}  // namespace maxwell
