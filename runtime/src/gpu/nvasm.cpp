// NV_gpu_program5 -> GLSL translator (see nvasm.h for the model and layout).
// Semantics follow the NV_gpu_program4/5, NV_parameter_buffer_object2,
// NV_bindless_texture and ARB_fragment_program_shadow specifications.
#include "nvasm.h"

#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace nvasm {
namespace {

enum class T { F, S, U };
const char* const kComp = "xyzw";

struct Fail {
    std::string msg;
};
[[noreturn]] void fail(const std::string& m) { throw Fail{m}; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

// split on commas that are not inside {} [] ()
std::vector<std::string> split_operands(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char ch : s) {
        if (ch == '{' || ch == '[' || ch == '(') depth++;
        if (ch == '}' || ch == ']' || ch == ')') depth--;
        if (ch == ',' && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

std::string float_lit(double v) {
    char b[64];
    snprintf(b, sizeof b, "%.9g", v);
    std::string s = b;
    if (s == "inf") return "uintBitsToFloat(0x7F800000u)";
    if (s == "-inf") return "uintBitsToFloat(0xFF800000u)";
    if (s == "nan" || s == "-nan") return "uintBitsToFloat(0x7FC00000u)";
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

// ---- operands ---------------------------------------------------------------------------------
enum class Base { Temp, Long, Float, Uint, Int, Lit, CBuf, Storage, Param, Handle };

struct Src {
    Base base = Base::Temp;
    std::string expr;          // GLSL for the base vector (Temp: "R0"; Float: "fi_1"; Param: "c")
    int slot = 0;              // cbuf / storage slot, attrib index
    bool idx_reg = false;      // array index [reg.c + k] / [k]
    std::string idx_expr;      // reg component GLSL ("R0.x")
    long long idx_const = 0;
    std::string lit[4];
    int swz[4] = {0, 1, 2, 3};
    bool scalar = false;       // one-component swizzle (replicated)
    bool neg = false, abs = false;
    std::string text;
};

struct Dst {
    enum Kind { Temp, Long, Out, Position, Color } kind = Temp;
    std::string name;          // GLSL lvalue base
    int mask = 0xF;
    int comp = 0;              // Long: component
    int loc = 0;
};

struct Ctx {
    Stage stage = Stage::Vertex;
    const Options* opt = nullptr;
    Reflection refl;
    std::map<std::string, int> cbuf_alias;        // "buf2" -> 2
    std::map<std::string, int> storage_alias;     // "sbo_buf0" -> 0
    struct Range { int base; };
    std::map<std::string, int> in_alias, out_alias;   // array name -> first attrib
    std::map<std::string, int> color_alias;           // "result_color0" -> 0
    std::set<std::string> temps, longs;
    int param_count = 0;
    std::vector<uint32_t> param_words;
    std::map<int, T> vertex_input_type;               // how each vertex attribute is read
    std::ostringstream body;
    int indent = 1;
    int rep_depth = 0, rep_serial = 0;
    bool uses_pow = false;
    std::string stmt;                                 // current statement, for errors

    void line(const std::string& s) { body << std::string((size_t)indent * 4, ' ') << s << "\n"; }
};

int parse_swizzle(const std::string& s, int out[4]) {
    if (s.empty() || s.size() > 4) return 0;
    for (size_t i = 0; i < s.size(); i++) {
        const char* p = strchr(kComp, s[i]);
        if (!p || !s[i]) return 0;
        out[i] = (int)(p - kComp);
    }
    return (int)s.size();
}

int parse_mask(const std::string& s) {
    int m = 0;
    for (char ch : s) {
        const char* p = strchr(kComp, ch);
        if (!p || !ch) fail("bad write mask ." + s);
        m |= 1 << (p - kComp);
    }
    return m;
}

std::string mask_str(int m) {
    std::string s;
    for (int i = 0; i < 4; i++)
        if (m & (1 << i)) s += kComp[i];
    return s;
}

// "R0.x + 16" / "R1.w" / "80"
void parse_index(Ctx& cx, const std::string& in, Src& s) {
    std::string e = trim(in);
    size_t plus = e.find('+');
    std::string reg = plus == std::string::npos ? e : trim(e.substr(0, plus));
    std::string k = plus == std::string::npos ? "" : trim(e.substr(plus + 1));
    if (!reg.empty() && (isdigit((unsigned char)reg[0]) || reg[0] == '-')) {
        s.idx_const = strtoll(reg.c_str(), nullptr, 10);
        return;
    }
    size_t dot = reg.find('.');
    if (dot == std::string::npos) fail("index without component: " + e);
    std::string r = reg.substr(0, dot), c = reg.substr(dot + 1);
    if (!cx.temps.count(r) || c.size() != 1 || !strchr(kComp, c[0])) fail("unsupported index " + e);
    s.idx_reg = true;
    s.idx_expr = r + "." + c;
    if (!k.empty()) s.idx_const = strtoll(k.c_str(), nullptr, 10);
}

// split "name[idx].swz" / "name.swz" into its parts (the name may contain dots: fragment.attrib)
void split_ref(const std::string& t, std::string& name, std::string& idx, std::string& swz, bool& has_idx) {
    has_idx = false;
    size_t lb = t.find('[');
    if (lb != std::string::npos) {
        size_t rb = t.find(']', lb);
        if (rb == std::string::npos) fail("unbalanced [ in " + t);
        name = t.substr(0, lb);
        idx = t.substr(lb + 1, rb - lb - 1);
        has_idx = true;
        swz = rb + 1 < t.size() && t[rb + 1] == '.' ? t.substr(rb + 2) : "";
        return;
    }
    // builtins with dots: keep the known prefix whole
    static const char* const kDotted[] = {"fragment.position", "vertex.id", "invocation.groupid", "invocation.localindex",
                                          "result.position", "result.color", "fragment.facing"};
    for (const char* d : kDotted)
        if (starts(t, d)) {
            name = d;
            swz = t.size() > strlen(d) && t[strlen(d)] == '.' ? t.substr(strlen(d) + 1) : "";
            return;
        }
    size_t dot = t.find('.');
    name = dot == std::string::npos ? t : t.substr(0, dot);
    swz = dot == std::string::npos ? "" : t.substr(dot + 1);
}

Src parse_src(Ctx& cx, std::string t, T use_type) {
    Src s;
    s.text = t;
    t = trim(t);
    if (!t.empty() && t[0] == '-') { s.neg = true; t = trim(t.substr(1)); }
    if (!t.empty() && t[0] == '|') {
        size_t e = t.find('|', 1);
        if (e == std::string::npos) fail("unbalanced |");
        s.abs = true;
        t = t.substr(1, e - 1) + t.substr(e + 1);
    }
    std::string swz;
    if (t[0] == '{') {
        size_t e = t.find('}');
        if (e == std::string::npos) fail("unbalanced {");
        auto parts = split_operands(t.substr(1, e - 1));
        if (parts.empty() || parts.size() > 4) fail("bad literal " + t);
        for (int i = 0; i < 4; i++) s.lit[i] = i < (int)parts.size() ? parts[(size_t)i] : "0";
        s.base = Base::Lit;
        swz = e + 1 < t.size() && t[e + 1] == '.' ? t.substr(e + 2) : "";
    } else if (starts(t, "handle(")) {
        size_t e = t.find(')');
        std::string in = t.substr(7, e - 7);           // D0.x
        size_t dot = in.find('.');
        std::string r = in.substr(0, dot);
        if (!cx.longs.count(r) || dot == std::string::npos) fail("bad handle operand " + t);
        s.base = Base::Handle;
        s.expr = r;
        s.slot = (int)(strchr(kComp, in[dot + 1]) - kComp);
        return s;
    } else {
        std::string name, idx;
        bool has_idx;
        split_ref(t, name, idx, swz, has_idx);
        if (cx.temps.count(name)) {
            s.base = Base::Temp;
            s.expr = name;
        } else if (cx.longs.count(name)) {
            s.base = Base::Long;
            s.expr = name;
        } else if (name == "fragment.attrib" || name == "vertex.attrib" || cx.in_alias.count(name)) {
            int base = cx.in_alias.count(name) ? cx.in_alias[name] : 0;
            if (!has_idx) fail("attribute without index: " + t);
            int n = base + atoi(idx.c_str());
            if (n < 0 || n > 31) fail("attribute index out of range: " + t);
            cx.refl.inputs |= 1u << n;
            s.slot = n;
            if (cx.stage == Stage::Vertex) {
                // remember how the program reads it: integer reads need an integer vertex format
                if (use_type != T::F) cx.vertex_input_type[n] = use_type;
                else if (!cx.vertex_input_type.count(n)) cx.vertex_input_type[n] = T::F;
                s.base = Base::Float;                    // fixed up after parsing (see finish_inputs)
                s.expr = "va_" + std::to_string(n);
            } else {
                s.base = Base::Float;
                s.expr = "fi_" + std::to_string(n);
            }
        } else if (name == "fragment.position") {
            s.base = Base::Float;
            s.expr = "gl_FragCoord";
        } else if (name == "fragment.facing") {
            s.base = Base::Float;
            s.expr = "vec4(gl_FrontFacing ? 1.0 : -1.0)";
        } else if (name == "vertex.id") {
            s.base = Base::Int;
            s.expr = "ivec4(gl_VertexIndex)";
        } else if (name == "invocation.groupid") {
            s.base = Base::Uint;
            s.expr = "uvec4(gl_WorkGroupID, 0u)";
        } else if (name == "invocation.localindex") {
            s.base = Base::Uint;
            s.expr = "uvec4(gl_LocalInvocationIndex)";
        } else if (cx.cbuf_alias.count(name)) {
            s.base = Base::CBuf;
            s.slot = cx.cbuf_alias[name];
            if (!has_idx) fail("constant buffer without offset: " + t);
            parse_index(cx, idx, s);
        } else if (cx.storage_alias.count(name)) {
            s.base = Base::Storage;
            s.slot = cx.storage_alias[name];
            if (!has_idx) fail("storage buffer without offset: " + t);
            parse_index(cx, idx, s);
        } else if (name == "c" && cx.param_count) {
            s.base = Base::Param;
            if (!has_idx) fail("PARAM without index: " + t);
            parse_index(cx, idx, s);
        } else {
            fail("unknown operand " + t);
        }
    }
    if (!swz.empty()) {
        int n = parse_swizzle(swz, s.swz);
        if (n == 1) {
            s.scalar = true;
            s.swz[1] = s.swz[2] = s.swz[3] = s.swz[0];
        } else if (n != 4) {
            fail("unsupported swizzle ." + swz + " in " + t);
        }
    }
    return s;
}

Dst parse_dst(Ctx& cx, const std::string& t0) {
    Dst d;
    std::string t = trim(t0), name, idx, swz;
    bool has_idx;
    split_ref(t, name, idx, swz, has_idx);
    if (!swz.empty()) d.mask = parse_mask(swz);
    if (cx.temps.count(name)) {
        d.kind = Dst::Temp;
        d.name = name;
    } else if (cx.longs.count(name)) {
        d.kind = Dst::Long;
        d.name = name;
        if (swz.size() != 1) fail("64-bit destination needs one component: " + t);
        d.comp = (int)(strchr(kComp, swz[0]) - kComp);
    } else if (name == "result.position") {
        d.kind = Dst::Position;
        d.name = "gl_Position";
    } else if (name == "result.attrib" || cx.out_alias.count(name)) {
        int base = cx.out_alias.count(name) ? cx.out_alias[name] : 0;
        if (!has_idx) fail("output without index: " + t);
        d.loc = base + atoi(idx.c_str());
        d.kind = Dst::Out;
        d.name = "vo_" + std::to_string(d.loc);
        cx.refl.outputs |= 1u << d.loc;
    } else if (name == "result.color" || cx.color_alias.count(name)) {
        d.loc = cx.color_alias.count(name) ? cx.color_alias[name] : (has_idx ? atoi(idx.c_str()) : 0);
        d.kind = Dst::Color;
        d.name = "fo_" + std::to_string(d.loc);
        cx.refl.outputs |= 1u << d.loc;
    } else {
        fail("unknown destination " + t);
    }
    return d;
}

const char* vec_of(T t) { return t == T::F ? "vec4" : t == T::S ? "ivec4" : "uvec4"; }

std::string swz_str(const Src& s) {
    if (s.swz[0] == 0 && s.swz[1] == 1 && s.swz[2] == 2 && s.swz[3] == 3) return "";
    std::string r = ".";
    for (int i = 0; i < 4; i++) r += kComp[s.swz[i]];
    return r;
}

std::string index_expr(const Src& s) {
    if (!s.idx_reg) return std::to_string((unsigned long long)(uint32_t)s.idx_const) + "u";
    if (!s.idx_const) return s.idx_expr;
    return "(" + s.idx_expr + " + " + std::to_string((uint32_t)s.idx_const) + "u)";
}

// the operand as a vec4 / ivec4 / uvec4 of type t (swizzle, abs and negate applied)
std::string load(Ctx& cx, const Src& s, T t) {
    std::string e;
    std::string sw = swz_str(s);
    auto from_bits = [&](const std::string& u) {     // u: uvec4 expression
        return t == T::F ? "uintBitsToFloat(" + u + ")" : t == T::S ? "ivec4(" + u + ")" : u;
    };
    switch (s.base) {
    case Base::Temp: e = from_bits(s.expr + sw); break;
    case Base::Uint: e = from_bits("(" + s.expr + ")" + sw); break;
    case Base::Int: e = from_bits("uvec4(" + s.expr + ")" + sw); break;
    case Base::Float:
        if (cx.stage == Stage::Vertex && starts(s.expr, "va_") && cx.vertex_input_type[s.slot] != T::F) {
            e = from_bits("uvec4(" + s.expr + ")" + sw);       // integer vertex input
            break;
        }
        e = t == T::F ? s.expr + sw : t == T::S ? "floatBitsToInt(" + s.expr + sw + ")" : "floatBitsToUint(" + s.expr + sw + ")";
        break;
    case Base::Param: e = from_bits("c[" + index_expr(s) + "]" + sw); break;
    case Base::Lit: {
        std::string v[4];
        for (int i = 0; i < 4; i++) {
            const std::string& l = s.lit[i];
            if (t == T::F) {
                v[i] = float_lit(strtod(l.c_str(), nullptr));
            } else {
                bool is_float = l.find_first_of(".eE") != std::string::npos && l.find("0x") == std::string::npos;
                long long x = is_float ? (long long)strtod(l.c_str(), nullptr) : strtoll(l.c_str(), nullptr, starts(l, "0x") ? 16 : 10);
                v[i] = t == T::S ? std::to_string((int32_t)x) : std::to_string((uint32_t)x) + "u";
                if (t == T::S && (int32_t)x == INT32_MIN) v[i] = "(-2147483647 - 1)";
            }
        }
        e = std::string(vec_of(t)) + "(" + v[0] + ", " + v[1] + ", " + v[2] + ", " + v[3] + ")" + sw;
        break;
    }
    default: fail("operand cannot be read as a vector: " + s.text);
    }
    if (s.abs && t != T::U) e = "abs(" + e + ")";
    if (s.neg) e = "-(" + e + ")";
    return e;
}

// scalar operand (component 0 of the swizzled vector), replicated
std::string load_scalar(Ctx& cx, const Src& s, T t) { return "(" + load(cx, s, t) + ").xxxx"; }

std::string to_bits(T t, const std::string& r) {
    return t == T::F ? "floatBitsToUint(" + r + ")" : t == T::S ? "uvec4(" + r + ")" : r;
}
std::string to_float(T t, const std::string& r) {
    return t == T::F ? r : t == T::S ? "intBitsToFloat(" + r + ")" : "uintBitsToFloat(" + r + ")";
}

void store(Ctx& cx, const Dst& d, T t, const std::string& r, bool cc) {
    std::string m = mask_str(d.mask);
    std::string msw = d.mask == 0xF ? "" : "." + m;
    switch (d.kind) {
    case Dst::Temp: cx.line(d.name + msw + " = " + to_bits(t, r) + msw + ";"); break;
    case Dst::Out: case Dst::Position: case Dst::Color: cx.line(d.name + msw + " = " + to_float(t, r) + msw + ";"); break;
    case Dst::Long: fail("64-bit destination for a vector result");
    }
    if (cc) {
        if (t == T::F) cx.line("ccNE" + msw + " = notEqual(" + r + ", vec4(0.0))" + msw + ";");
        else cx.line("ccNE" + msw + " = notEqual(" + to_bits(t, r) + ", uvec4(0u))" + msw + ";");
    }
}

std::string cond_expr(const std::string& c0) {
    std::string c = trim(c0);
    size_t dot = c.find('.');
    std::string op = c.substr(0, dot);
    char comp = dot == std::string::npos ? 'x' : c[dot + 1];
    if (op == "NE") return std::string("ccNE.") + comp;
    if (op == "EQ") return std::string("!ccNE.") + comp;
    if (op == "TR") return "true";
    if (op == "FL") return "false";
    fail("unsupported condition " + c);
}

// one 32-bit word of constant buffer `slot` at word index `w`
std::string cb_word(int slot, const std::string& w) { return "cb" + std::to_string(slot) + "w(" + w + ")"; }

struct Op {
    std::string name;
    std::vector<std::string> mods;
    bool has(const char* m) const {
        for (auto& x : mods)
            if (x == m) return true;
        return false;
    }
};

T type_of(const Op& op, T def = T::F) {
    for (auto& m : op.mods) {
        if (m == "F" || m == "F32") return T::F;
        if (m == "S" || m == "S32") return T::S;
        if (m == "U" || m == "U32") return T::U;
    }
    return def;
}

void emit_texture(Ctx& cx, const Op& op, const std::vector<std::string>& ops) {
    cx.refl.textures = true;
    std::string target = ops.back();
    if (target != "2D" && target != "SHADOW2D") fail("unsupported texture target " + target);
    bool shadow = target == "SHADOW2D";
    Src h = parse_src(cx, ops[ops.size() - 2], T::U);
    if (h.base != Base::Handle) fail("texture access without handle(): " + cx.stmt);
    std::string hx = h.expr + "[" + std::to_string(h.slot) + "].x";
    std::string ti = "nonuniformEXT(" + hx + " & 0xFFFFFu)", si = "nonuniformEXT(" + hx + " >> 20u)";
    T rt = type_of(op, T::F);
    std::string tex = rt == T::F ? "g_tex[" + ti + "]" : rt == T::U ? "g_utex[" + ti + "]" : "g_itex[" + ti + "]";
    std::string s2d = shadow ? "sampler2DShadow(g_tex[" + ti + "], g_smp[" + si + "])"
                             : (rt == T::F ? "sampler2D(" : rt == T::U ? "usampler2D(" : "isampler2D(") + tex + ", g_smp[" + si + "])";
    Dst d = parse_dst(cx, ops[0]);
    const std::string& name = op.name;
    std::string r;
    if (name == "TXQ") {
        cx.line("ivec4 l_ = " + load(cx, parse_src(cx, ops[1], T::S), T::S) + ";");
        r = "uvec4(uvec2(textureSize(g_tex[" + ti + "], l_.x)), 0u, uint(textureQueryLevels(g_tex[" + ti + "])))";
        store(cx, d, T::U, r, op.has("CC"));
        return;
    }
    if (name == "TXF") {
        cx.line("ivec4 t_ = " + load(cx, parse_src(cx, ops[1], T::S), T::S) + ";");
        r = "texelFetch(" + tex + ", t_.xy, t_.w)";
        store(cx, d, rt, r, op.has("CC"));
        return;
    }
    cx.line("vec4 t_ = " + load(cx, parse_src(cx, ops[1], T::F), T::F) + ";");
    if (name == "TEX") r = shadow ? "vec4(texture(" + s2d + ", t_.xyz))" : "texture(" + s2d + ", t_.xy)";
    else if (name == "TXB") r = shadow ? "vec4(texture(" + s2d + ", t_.xyz, t_.w))" : "texture(" + s2d + ", t_.xy, t_.w)";
    else if (name == "TXL") r = shadow ? "vec4(textureLod(" + s2d + ", t_.xyz, t_.w))" : "textureLod(" + s2d + ", t_.xy, t_.w)";
    else if (name == "TXP") r = shadow ? "vec4(textureProj(" + s2d + ", t_))" : "textureProj(" + s2d + ", t_)";
    else if (name == "LOD") r = "vec4(textureQueryLod(" + s2d + ", t_.xy), 0.0, 0.0)";
    else if (name == "TXGO") {
        // gather with four constant offsets: x offsets in operand 2, y offsets in operand 3
        Src ox = parse_src(cx, ops[2], T::S), oy = parse_src(cx, ops[3], T::S);
        if (ox.base != Base::Lit || oy.base != Base::Lit) fail("TXGO with non-constant offsets");
        std::string offs = "ivec2[4](";
        for (int i = 0; i < 4; i++) {
            offs += "ivec2(" + std::to_string(atoi(ox.lit[ox.swz[i]].c_str())) + ", " + std::to_string(atoi(oy.lit[oy.swz[i]].c_str())) + ")";
            offs += i < 3 ? ", " : ")";
        }
        r = shadow ? "textureGatherOffsets(" + s2d + ", t_.xy, t_.z, " + offs + ")" : "textureGatherOffsets(" + s2d + ", t_.xy, " + offs + ", 0)";
    } else {
        fail("unsupported texture instruction " + name);
    }
    store(cx, d, rt, r, op.has("CC"));
}

int ldc_words(const Op& op, bool& is64) {
    is64 = false;
    for (auto& m : op.mods) {
        if (m == "F32" || m == "S32" || m == "U32") return 1;
        if (m == "F32X2" || m == "S32X2" || m == "U32X2") return 2;
        if (m == "F32X4" || m == "S32X4" || m == "U32X4") return 4;
        if (m == "U64" || m == "S64") { is64 = true; return 2; }
    }
    fail("unsupported load format");
}

void emit_instruction(Ctx& cx, const std::string& stmt) {
    cx.stmt = stmt;
    size_t sp = stmt.find_first_of(" \t");
    std::string head = sp == std::string::npos ? stmt : stmt.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : trim(stmt.substr(sp + 1));
    Op op;
    {
        std::stringstream ss(head);
        std::string part;
        std::getline(ss, op.name, '.');
        while (std::getline(ss, part, '.')) op.mods.push_back(part);
    }
    const std::string& n = op.name;
    auto ops = split_operands(rest);
    cx.line("// " + stmt);

    // ---- flow control ----
    if (n == "IF") { cx.line("if (" + cond_expr(rest) + ") {"); cx.indent++; return; }
    if (n == "ELSE") { cx.indent--; cx.line("} else {"); cx.indent++; return; }
    if (n == "ENDIF" || n == "ENDREP") {
        cx.indent--;
        cx.line("}");
        if (n == "ENDREP") cx.rep_depth--;
        return;
    }
    if (n == "REP") {
        std::string count;
        if (rest.empty()) count = "0x7FFFFFFF";
        else count = "int((" + load(cx, parse_src(cx, ops[0], T::S), T::S) + ").x)";
        std::string v = "rep" + std::to_string(cx.rep_serial++);
        cx.line("for (int " + v + " = 0; " + v + " < " + count + "; " + v + "++) {");
        cx.indent++;
        cx.rep_depth++;
        return;
    }
    if (n == "BRK" || n == "CONT") {
        const char* kw = n == "BRK" ? "break;" : "continue;";
        if (rest.empty()) cx.line(kw);
        else cx.line("if (" + cond_expr(rest.substr(rest.find('(') + 1, rest.find(')') - rest.find('(') - 1)) + ") " + kw);
        return;
    }
    if (n == "KIL") {
        if (cx.stage != Stage::Fragment) fail("KIL outside a fragment program");
        if (!rest.empty() && isupper((unsigned char)rest[0]) && rest.size() >= 2 && (starts(rest, "NE") || starts(rest, "EQ") || starts(rest, "TR")))
            cx.line("if (" + cond_expr(rest) + ") discard;");
        else
            cx.line("if (any(lessThan(" + load(cx, parse_src(cx, rest, T::F), T::F) + ", vec4(0.0)))) discard;");
        return;
    }

    struct Scope {
        Ctx& c;
        explicit Scope(Ctx& x) : c(x) { c.line("{"); c.indent++; }
        ~Scope() { c.indent--; c.line("}"); }
    } scope(cx);

    // ---- loads and stores ----
    if (n == "LDC" || n == "LDB") {
        bool is64;
        int words = ldc_words(op, is64);
        Src s = parse_src(cx, ops[1], T::U);
        bool cb = n == "LDC";
        if (cb ? s.base != Base::CBuf : s.base != Base::Storage) fail("bad " + n + " source");
        if (cb) cx.refl.cbufs |= 1u << s.slot;
        else cx.refl.storage |= 1u << s.slot;
        cx.line("uint o_ = " + index_expr(s) + " >> 2u;");
        auto word = [&](int i) {
            std::string w = i ? "o_ + " + std::to_string(i) + "u" : "o_";
            return cb ? cb_word(s.slot, w) : "sb" + std::to_string(s.slot) + "[" + w + "]";
        };
        Dst d = parse_dst(cx, ops[0]);
        if (is64) {
            if (d.kind != Dst::Long) fail("64-bit load into a 32-bit register");
            cx.line(d.name + "[" + std::to_string(d.comp) + "] = uvec2(" + word(0) + ", " + word(1) + ");");
            return;
        }
        std::string v = "uvec4(";
        for (int i = 0; i < 4; i++) v += (i < words ? word(i) : std::string("0u")) + (i < 3 ? ", " : ")");
        store(cx, d, T::U, v, op.has("CC"));
        return;
    }
    if (n == "STB") {
        // STB.F32X4 value, sbo_buf[offset]
        bool is64;
        int words = ldc_words(op, is64);
        Src dst = parse_src(cx, ops[1], T::U);
        if (dst.base != Base::Storage) fail("bad STB destination");
        cx.refl.storage |= 1u << dst.slot;
        cx.line("uvec4 v_ = " + load(cx, parse_src(cx, ops[0], T::U), T::U) + ";");
        cx.line("uint o_ = " + index_expr(dst) + " >> 2u;");
        for (int i = 0; i < words; i++)
            cx.line("sb" + std::to_string(dst.slot) + "[o_ + " + std::to_string(i) + "u] = v_." + kComp[i] + ";");
        return;
    }

    // ---- textures ----
    if (n == "TEX" || n == "TXB" || n == "TXL" || n == "TXP" || n == "TXF" || n == "TXQ" || n == "LOD" || n == "TXGO") {
        emit_texture(cx, op, ops);
        return;
    }

    // ---- ALU ----
    T t = type_of(op, T::F);
    bool sat = op.has("SAT"), cc = op.has("CC");
    if (ops.empty()) fail("instruction without operands");
    Dst d = parse_dst(cx, ops[0]);
    auto src = [&](int i, T ty) {
        if ((int)ops.size() <= i) fail("missing operand");
        return load(cx, parse_src(cx, ops[(size_t)i], ty), ty);
    };
    auto scalar = [&](int i, T ty) {
        if ((int)ops.size() <= i) fail("missing operand");
        return load_scalar(cx, parse_src(cx, ops[(size_t)i], ty), ty);
    };
    std::string r;
    T rt = t;
    const char* V = vec_of(t);
    if (n == "MOV") r = src(1, t);
    else if (n == "ADD") r = src(1, t) + " + " + src(2, t);
    else if (n == "SUB") r = src(1, t) + " - " + src(2, t);
    else if (n == "MUL") r = src(1, t) + " * " + src(2, t);
    else if (n == "MAD") r = t == T::F ? "fma(" + src(1, t) + ", " + src(2, t) + ", " + src(3, t) + ")"
                                       : src(1, t) + " * " + src(2, t) + " + " + src(3, t);
    else if (n == "MIN") r = "min(" + src(1, t) + ", " + src(2, t) + ")";
    else if (n == "MAX") r = "max(" + src(1, t) + ", " + src(2, t) + ")";
    else if (n == "AND" || n == "OR" || n == "XOR") {
        const char* o = n == "AND" ? " & " : n == "OR" ? " | " : " ^ ";
        if (t == T::F) t = rt = T::U;
        r = src(1, t) + o + src(2, t);
    } else if (n == "SHL" || n == "SHR") {
        if (t == T::F) t = rt = T::U;
        std::string amount = src(2, T::U) + " & uvec4(31u)";
        r = src(1, t) + (n == "SHL" ? " << " : " >> ") + (t == T::S ? "ivec4(" + amount + ")" : "(" + amount + ")");
    } else if (n == "DIV") r = src(1, t) + " / " + scalar(2, t);
    else if (n == "MOD") {
        if (t == T::F) fail("MOD.F");
        cx.line(std::string(V) + " a_ = " + src(1, t) + ", b_ = " + scalar(2, t) + ";");
        r = "a_ - b_ * (a_ / b_)";
    } else if (n == "DP2" || n == "DP3" || n == "DP4") {
        const char* sw = n == "DP2" ? ".xy" : n == "DP3" ? ".xyz" : "";
        r = std::string("vec4(dot((") + src(1, T::F) + ")" + sw + ", (" + src(2, T::F) + ")" + sw + "))";
        rt = T::F;
    } else if (n == "RCP") { r = "vec4(1.0 / (" + scalar(1, T::F) + ").x)"; rt = T::F; }
    else if (n == "RSQ") { r = "vec4(inversesqrt(abs((" + scalar(1, T::F) + ").x)))"; rt = T::F; }
    else if (n == "EX2") { r = "vec4(exp2((" + scalar(1, T::F) + ").x))"; rt = T::F; }
    else if (n == "LG2") { r = "vec4(log2((" + scalar(1, T::F) + ").x))"; rt = T::F; }
    else if (n == "SIN") { r = "vec4(sin((" + scalar(1, T::F) + ").x))"; rt = T::F; }
    else if (n == "COS") { r = "vec4(cos((" + scalar(1, T::F) + ").x))"; rt = T::F; }
    else if (n == "POW") {
        cx.uses_pow = true;
        r = "vec4(nv_pow((" + scalar(1, T::F) + ").x, (" + scalar(2, T::F) + ").x))";
        rt = T::F;
    } else if (n == "FLR" || n == "TRUNC" || n == "ROUND" || n == "CEIL" || n == "FRC") {
        std::string f = n == "FLR" ? "floor" : n == "TRUNC" ? "trunc" : n == "ROUND" ? "roundEven" : n == "CEIL" ? "ceil" : "fract";
        std::string x = f + "(" + src(1, T::F) + ")";
        if (t == T::F) r = x;
        else if (t == T::S) r = "ivec4(clamp(" + x + ", vec4(-2147483648.0), vec4(2147483520.0)))";
        else r = "uvec4(clamp(" + x + ", vec4(0.0), vec4(4294967040.0)))";
    } else if (n == "I2F") {
        r = "vec4(" + src(1, t == T::F ? T::S : t) + ")";
        rt = T::F;
    } else if (n == "F2I") {
        std::string x = "trunc(" + src(1, T::F) + ")";
        r = t == T::U ? "uvec4(clamp(" + x + ", vec4(0.0), vec4(4294967040.0)))" : "ivec4(clamp(" + x + ", vec4(-2147483648.0), vec4(2147483520.0)))";
    } else if (n == "SEQ" || n == "SNE" || n == "SLT" || n == "SGT" || n == "SLE" || n == "SGE") {
        std::string f = n == "SEQ" ? "equal" : n == "SNE" ? "notEqual" : n == "SLT" ? "lessThan" : n == "SGT" ? "greaterThan"
                      : n == "SLE" ? "lessThanEqual" : "greaterThanEqual";
        std::string c = f + "(" + src(1, t) + ", " + src(2, t) + ")";
        r = t == T::F ? "vec4(" + c + ")" : t == T::S ? "mix(ivec4(0), ivec4(-1), " + c + ")" : "mix(uvec4(0u), uvec4(0xFFFFFFFFu), " + c + ")";
    } else {
        fail("unsupported instruction " + n);
    }
    if (sat) {
        if (rt != T::F) fail(".SAT on an integer result");
        r = "clamp(" + r + ", vec4(0.0), vec4(1.0))";
    }
    cx.line(std::string(vec_of(rt)) + " r_ = " + r + ";");
    store(cx, d, rt, "r_", cc);
}

void declare(Ctx& cx, const std::string& stmt) {
    std::string s = trim(stmt);
    auto rhs_index = [&](const char* key) {
        size_t p = s.find(key);
        if (p == std::string::npos) fail("bad declaration " + s);
        return atoi(s.c_str() + p + strlen(key));
    };
    auto lhs_name = [&](size_t start) {
        size_t e = s.find_first_of("[ =", start);
        return trim(s.substr(start, e - start));
    };
    if (starts(s, "CBUFFER ")) {
        cx.cbuf_alias[lhs_name(8)] = rhs_index("program.buffer[");
    } else if (starts(s, "STORAGE ")) {
        cx.storage_alias[lhs_name(8)] = rhs_index("program.storage[");
    } else if (starts(s, "ATTRIB ")) {
        std::string nm = lhs_name(7);
        size_t p = s.find("attrib[");
        cx.in_alias[nm] = p == std::string::npos ? 0 : atoi(s.c_str() + p + 7);
    } else if (starts(s, "OUTPUT ")) {
        std::string nm = lhs_name(7);
        if (s.find("result.attrib[") != std::string::npos) cx.out_alias[nm] = rhs_index("result.attrib[");
        else if (s.find("result.color[") != std::string::npos) cx.color_alias[nm] = rhs_index("result.color[");
        else if (s.find("result.color") != std::string::npos) cx.color_alias[nm] = 0;
        else if (s.find("result.position") != std::string::npos) fail("aliased result.position");
        else fail("unsupported output " + s);
    } else if (starts(s, "TEMP ") || starts(s, "SHORT TEMP ") || starts(s, "LONG TEMP ")) {
        bool is_long = starts(s, "LONG ");
        std::string list = s.substr(s.find("TEMP ") + 5);
        for (auto& r : split_operands(list)) (is_long ? cx.longs : cx.temps).insert(r);
    } else if (starts(s, "PARAM ")) {
        // PARAM c[N] = { program.local[0..N-1] };
        if (s.find("program.local[") == std::string::npos || lhs_name(6) != "c") fail("unsupported PARAM " + s);
        cx.param_count = rhs_index("c[");
    } else if (starts(s, "GROUP_SIZE ")) {
        auto v = split_operands(s.substr(11));
        for (size_t i = 0; i < v.size() && i < 3; i++) cx.refl.group_size[i] = (uint32_t)atoi(v[i].c_str());
    } else {
        fail("unsupported declaration " + s);
    }
}

bool is_declaration(const std::string& s) {
    return starts(s, "CBUFFER ") || starts(s, "STORAGE ") || starts(s, "ATTRIB ") || starts(s, "OUTPUT ") ||
           starts(s, "TEMP ") || starts(s, "SHORT TEMP ") || starts(s, "LONG TEMP ") || starts(s, "PARAM ") ||
           starts(s, "GROUP_SIZE ");
}

}  // namespace

Result translate(const uint8_t* control, size_t size, const Options& opt) {
    Result res;
    if (size < 0x24 || *(const uint32_t*)control != 0x98761234u) {
        res.error = "not an NVN control section";
        return res;
    }
    uint32_t asm_off = *(const uint32_t*)(control + 0x14), asm_size = *(const uint32_t*)(control + 0x18);
    uint32_t const_off = *(const uint32_t*)(control + 0x20);
    if ((uint64_t)asm_off + asm_size > size || asm_size < 8 || memcmp(control + asm_off, "!!NV", 4) != 0) {
        res.error = "control section has no NV assembly";
        return res;
    }
    std::string text((const char*)control + asm_off, asm_size);
    text = text.substr(0, text.find('\0'));
    res.asm_text = text;
    Ctx cx;
    cx.opt = &opt;
    try {
        std::string header = trim(text.substr(0, text.find('\n')));
        if (starts(header, "!!NVvp")) cx.stage = Stage::Vertex;
        else if (starts(header, "!!NVfp")) cx.stage = Stage::Fragment;
        else if (starts(header, "!!NVcp")) cx.stage = Stage::Compute;
        else fail("unsupported program type " + header);
        cx.refl.stage = cx.stage;
        // statements end with ';' (a line may hold one; the header line has none)
        std::vector<std::string> stmts;
        std::string cur;
        for (char ch : text.substr(text.find('\n') + 1)) {
            if (ch == ';') {
                stmts.push_back(trim(cur));
                cur.clear();
            } else if (ch != '\r') {
                cur += ch == '\n' ? ' ' : ch;
            }
        }
        // declarations first (they may appear anywhere before use)
        for (auto& s : stmts)
            if (is_declaration(s)) declare(cx, s);
        if (cx.param_count) {
            if ((uint64_t)const_off + 16ull * (uint64_t)cx.param_count > size) fail("program.local constants out of range");
            for (int i = 0; i < cx.param_count * 4; i++) cx.param_words.push_back(*(const uint32_t*)(control + const_off + 4 * i));
        }
        for (int pass = 0; pass < 2; pass++) {
            cx.body.str("");
            cx.indent = 1;
            cx.rep_depth = cx.rep_serial = 0;
            for (auto& s : stmts) {
                if (s.empty() || is_declaration(s) || starts(s, "OPTION ") || s == "END") continue;
                emit_instruction(cx, s);
            }
            if (cx.indent != 1) fail("unbalanced IF/REP blocks");
        }
        // ---- assemble the GLSL ----
        std::ostringstream g;
        const char* stage_name = cx.stage == Stage::Vertex ? "vertex" : cx.stage == Stage::Fragment ? "fragment" : "compute";
        g << "#version 460\n#extension GL_EXT_nonuniform_qualifier : require\n"
             "#extension GL_EXT_samplerless_texture_functions : require\n"
          << "// " << stage_name << " program translated from NV_gpu_program5 by letsgo-pc\n";
        if (cx.stage == Stage::Compute)
            g << "layout(local_size_x = " << cx.refl.group_size[0] << ", local_size_y = " << cx.refl.group_size[1]
              << ", local_size_z = " << cx.refl.group_size[2] << ") in;\n";
        if (cx.refl.textures)
            g << "layout(set = 0, binding = 0) uniform texture2D g_tex[];\n"
                 "layout(set = 0, binding = 1) uniform utexture2D g_utex[];\n"
                 "layout(set = 0, binding = 2) uniform itexture2D g_itex[];\n"
                 "layout(set = 0, binding = 3) uniform sampler g_smp[];\n";
        int base = kStageBase[(int)cx.stage];
        for (int i = 0; i < kMaxCbufs; i++)
            if (cx.refl.cbufs & (1u << i))
                g << "layout(std140, set = 1, binding = " << base + i << ") uniform CB" << i << " { uvec4 cb" << i
                  << "[4096]; };\nuint cb" << i << "w(uint i) { return cb" << i << "[i >> 2u][i & 3u]; }\n";
        for (int i = 0; i < 16; i++)
            if (cx.refl.storage & (1u << i))
                g << "layout(std430, set = 1, binding = " << kStorageBase + i << ") buffer SB" << i << " { uint sb" << i << "[]; };\n";
        for (int i = 0; i < 32; i++) {
            if (!(cx.refl.inputs & (1u << i))) continue;
            if (cx.stage == Stage::Vertex) {
                T ty = cx.vertex_input_type.count(i) ? cx.vertex_input_type[i] : T::F;
                if (ty != T::F) cx.refl.int_inputs |= 1u << i;
                const char* decl = ty == T::F ? "vec4" : (opt.sint_inputs & (1u << i)) ? "ivec4" : "uvec4";
                g << "layout(location = " << i << ") in " << decl << " va_" << i << ";\n";
            } else if (cx.stage == Stage::Fragment) {
                g << "layout(location = " << i << ") in vec4 fi_" << i << ";\n";
            }
        }
        for (int i = 0; i < 32; i++)
            if (cx.refl.outputs & (1u << i))
                g << "layout(location = " << i << ") out vec4 " << (cx.stage == Stage::Vertex ? "vo_" : "fo_") << i << ";\n";
        if (cx.param_count) {
            g << "const uvec4 c[" << cx.param_count << "] = uvec4[" << cx.param_count << "](";
            for (int i = 0; i < cx.param_count; i++) {
                char b[96];
                snprintf(b, sizeof b, "uvec4(0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu)", cx.param_words[(size_t)i * 4],
                         cx.param_words[(size_t)i * 4 + 1], cx.param_words[(size_t)i * 4 + 2], cx.param_words[(size_t)i * 4 + 3]);
                g << b << (i + 1 < cx.param_count ? ", " : ");\n");
            }
        }
        if (cx.uses_pow) g << "float nv_pow(float x, float y) { return y == 0.0 ? 1.0 : exp2(y * log2(x)); }\n";
        g << "void main() {\n";
        for (auto& r : cx.temps) g << "    uvec4 " << r << " = uvec4(0u);\n";
        for (auto& r : cx.longs) g << "    uvec2 " << r << "[4] = uvec2[4](uvec2(0u), uvec2(0u), uvec2(0u), uvec2(0u));\n";
        g << "    bvec4 ccNE = bvec4(false);\n";
        g << cx.body.str();
        if (cx.stage == Stage::Vertex) {
            if (opt.gl_depth) g << "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
            g << "    gl_PointSize = 1.0;\n";
        }
        g << "}\n";
        res.glsl = g.str();
        res.refl = cx.refl;
        res.ok = true;
    } catch (const Fail& f) {
        res.error = f.msg + (cx.stmt.empty() ? "" : "  [in: " + cx.stmt + "]");
    }
    return res;
}

}  // namespace nvasm
