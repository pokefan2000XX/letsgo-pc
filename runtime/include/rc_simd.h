// AdvSIMD helpers for recompiled code. Vectors are passed and returned by
// value; `q` = 0 selects the 64-bit form, whose result has the upper half
// zeroed. `sz` is the element size: 0 = 8, 1 = 16, 2 = 32, 3 = 64 bits.
#pragma once

#define VN(sz, q) (((q) ? 16 : 8) >> (sz))
RC_INLINE V128 vzhi(V128 r, int q) {
    if (!q) r.d[1] = 0;
    return r;
}

// element access by size, as unsigned 64-bit
RC_INLINE uint64_t vget(V128 v, int sz, int i) {
    switch (sz) {
    case 0: return v.b[i];
    case 1: return v.h[i];
    case 2: return v.s[i];
    default: return v.d[i];
    }
}
RC_INLINE int64_t vgets(V128 v, int sz, int i) {
    switch (sz) {
    case 0: return v.sb[i];
    case 1: return v.sh[i];
    case 2: return v.ss[i];
    default: return v.sd[i];
    }
}
RC_INLINE void vset(V128* v, int sz, int i, uint64_t x) {
    switch (sz) {
    case 0: v->b[i] = (uint8_t)x; break;
    case 1: v->h[i] = (uint16_t)x; break;
    case 2: v->s[i] = (uint32_t)x; break;
    default: v->d[i] = x; break;
    }
}
RC_INLINE uint64_t emask(int sz) { return sz == 3 ? ~(uint64_t)0 : (((uint64_t)1 << (8 << sz)) - 1); }

// ---- integer lane-wise ---------------------------------------------------------
#define VMAP2(NAME, EXPR)                                                    \
    RC_INLINE V128 NAME(V128 a, V128 b, int sz, int q) {                     \
        V128 r = {{0}};                                                        \
        int n = VN(sz, q);                                                   \
        for (int i = 0; i < n; i++) {                                        \
            uint64_t x = vget(a, sz, i), y = vget(b, sz, i);                 \
            int64_t sx_ = vgets(a, sz, i), sy = vgets(b, sz, i);             \
            (void)sx_; (void)sy; (void)x; (void)y;                           \
            vset(&r, sz, i, (uint64_t)(EXPR));                               \
        }                                                                    \
        return r;                                                            \
    }
VMAP2(v_add, x + y)
VMAP2(v_sub, x - y)
VMAP2(v_mul, x * y)
VMAP2(v_cmeq, x == y ? ~(uint64_t)0 : 0)
VMAP2(v_cmtst, (x & y) ? ~(uint64_t)0 : 0)
VMAP2(v_cmgt, sx_ > sy ? ~(uint64_t)0 : 0)
VMAP2(v_cmge, sx_ >= sy ? ~(uint64_t)0 : 0)
VMAP2(v_cmhi, x > y ? ~(uint64_t)0 : 0)
VMAP2(v_cmhs, x >= y ? ~(uint64_t)0 : 0)
VMAP2(v_smax, sx_ > sy ? x : y)
VMAP2(v_smin, sx_ < sy ? x : y)
VMAP2(v_umax, x > y ? x : y)
VMAP2(v_umin, x < y ? x : y)

RC_INLINE int64_t sat_s(__int128 v, int sz) {
    int bits = 8 << sz;
    __int128 hi = ((__int128)1 << (bits - 1)) - 1, lo = -((__int128)1 << (bits - 1));
    return (int64_t)(v > hi ? hi : v < lo ? lo : v);
}
RC_INLINE uint64_t sat_u(__int128 v, int sz) {
    int bits = 8 << sz;
    __int128 hi = sz == 3 ? (__int128)UINT64_MAX : (((__int128)1 << bits) - 1);
    return (uint64_t)(v > hi ? hi : v < 0 ? 0 : v);
}
VMAP2(v_sqadd, sat_s((__int128)sx_ + sy, sz))
VMAP2(v_sqsub, sat_s((__int128)sx_ - sy, sz))
VMAP2(v_uqadd, sat_u((__int128)x + y, sz))
VMAP2(v_uqsub, sat_u((__int128)x - (__int128)y, sz))
// USHL/SSHL: shift by the signed low byte of each element of b
RC_INLINE uint64_t ushl_e(uint64_t x, int8_t s, int sz) {
    int bits = 8 << sz;
    if (s >= bits || s <= -bits) return 0;
    return s >= 0 ? (x << s) : (x >> -s);
}
RC_INLINE uint64_t sshl_e(int64_t x, int8_t s, int sz) {
    int bits = 8 << sz;
    if (s >= bits) return 0;
    if (s <= -bits) return x < 0 ? ~(uint64_t)0 : 0;
    return s >= 0 ? ((uint64_t)x << s) : (uint64_t)(x >> -s);
}
VMAP2(v_ushl, ushl_e(x, (int8_t)y, sz))
VMAP2(v_sshl, sshl_e(sx_, (int8_t)y, sz))

// logical (whole register)
RC_INLINE V128 v_and(V128 a, V128 b, int q) { V128 r; r.d[0] = a.d[0] & b.d[0]; r.d[1] = a.d[1] & b.d[1]; return vzhi(r, q); }
RC_INLINE V128 v_bic(V128 a, V128 b, int q) { V128 r; r.d[0] = a.d[0] & ~b.d[0]; r.d[1] = a.d[1] & ~b.d[1]; return vzhi(r, q); }
RC_INLINE V128 v_orr(V128 a, V128 b, int q) { V128 r; r.d[0] = a.d[0] | b.d[0]; r.d[1] = a.d[1] | b.d[1]; return vzhi(r, q); }
RC_INLINE V128 v_orn(V128 a, V128 b, int q) { V128 r; r.d[0] = a.d[0] | ~b.d[0]; r.d[1] = a.d[1] | ~b.d[1]; return vzhi(r, q); }
RC_INLINE V128 v_eor(V128 a, V128 b, int q) { V128 r; r.d[0] = a.d[0] ^ b.d[0]; r.d[1] = a.d[1] ^ b.d[1]; return vzhi(r, q); }
// BSL: d = (d & n) | (~d & m); BIT: d = (d & ~m) | (n & m); BIF: d = (d & m) | (n & ~m)
RC_INLINE V128 v_bsl(V128 d, V128 n, V128 m, int q) {
    V128 r;
    for (int i = 0; i < 2; i++) r.d[i] = (d.d[i] & n.d[i]) | (~d.d[i] & m.d[i]);
    return vzhi(r, q);
}
RC_INLINE V128 v_bit(V128 d, V128 n, V128 m, int q) {
    V128 r;
    for (int i = 0; i < 2; i++) r.d[i] = (d.d[i] & ~m.d[i]) | (n.d[i] & m.d[i]);
    return vzhi(r, q);
}
RC_INLINE V128 v_bif(V128 d, V128 n, V128 m, int q) {
    V128 r;
    for (int i = 0; i < 2; i++) r.d[i] = (d.d[i] & m.d[i]) | (n.d[i] & ~m.d[i]);
    return vzhi(r, q);
}

// unary integer
#define VMAP1(NAME, EXPR)                                                    \
    RC_INLINE V128 NAME(V128 a, int sz, int q) {                             \
        V128 r = {{0}};                                                        \
        int n = VN(sz, q);                                                   \
        for (int i = 0; i < n; i++) {                                        \
            uint64_t x = vget(a, sz, i);                                     \
            int64_t sx_ = vgets(a, sz, i);                                   \
            (void)x; (void)sx_;                                              \
            vset(&r, sz, i, (uint64_t)(EXPR));                               \
        }                                                                    \
        return r;                                                            \
    }
VMAP1(v_neg, (uint64_t)0 - x)
VMAP1(v_abs, sx_ < 0 ? (uint64_t)0 - x : x)
VMAP1(v_cmeqz, x == 0 ? ~(uint64_t)0 : 0)
VMAP1(v_cmgez, sx_ >= 0 ? ~(uint64_t)0 : 0)
VMAP1(v_cmgtz, sx_ > 0 ? ~(uint64_t)0 : 0)
VMAP1(v_cmlez, sx_ <= 0 ? ~(uint64_t)0 : 0)
VMAP1(v_cmltz, sx_ < 0 ? ~(uint64_t)0 : 0)
VMAP1(v_cnt, (uint64_t)__builtin_popcountll(x))
VMAP1(v_not, ~x)
RC_INLINE V128 v_rbit8(V128 a, int q) {
    V128 r = {{0}};
    for (int i = 0; i < (q ? 16 : 8); i++) r.b[i] = (uint8_t)(__builtin_bitreverse8(a.b[i]));
    return r;
}
// REV16/REV32/REV64: reverse elements of size sz within each container of 2^c bytes
RC_INLINE V128 v_rev(V128 a, int sz, int container_log2, int q) {
    V128 r = {{0}};
    int esz = 1 << sz, cbytes = 1 << container_log2, n = q ? 16 : 8;
    for (int base = 0; base < n; base += cbytes)
        for (int e = 0; e < cbytes / esz; e++)
            memcpy(&r.b[base + e * esz], &a.b[base + cbytes - (e + 1) * esz], (size_t)esz);
    return r;
}
// XTN/SQXTN/UQXTN (narrow from 2*sz to sz); `part` = 1 writes the upper half (XTN2)
RC_INLINE V128 v_xtn(V128 d, V128 a, int sz, int part, int mode) {
    V128 r = part ? d : (V128){{0}};
    int n = 8 >> sz;
    for (int i = 0; i < n; i++) {
        uint64_t x = vget(a, sz + 1, i);
        int64_t sx_ = vgets(a, sz + 1, i);
        uint64_t v = mode == 0 ? x : mode == 1 ? (uint64_t)sat_s(sx_, sz) : sat_u(x, sz);
        vset(&r, sz, i + (part ? n : 0), v);
    }
    if (!part) r.d[1] = 0;
    return r;
}

// widening: SSHLL/USHLL (shift), SXTL/UXTL; part selects the source half
RC_INLINE V128 v_shll(V128 a, int sz, int shift, int is_signed, int part) {
    V128 r = {{0}};
    int n = 8 >> sz;
    for (int i = 0; i < n; i++) {
        uint64_t x = is_signed ? (uint64_t)vgets(a, sz, i + (part ? n : 0)) : vget(a, sz, i + (part ? n : 0));
        vset(&r, sz + 1, i, x << shift);
    }
    return r;
}
// SMULL/UMULL/SMLAL/UMLAL/SADDW/UADDW... (three different), part = source half
RC_INLINE V128 v_mull(V128 acc, V128 a, V128 b, int sz, int is_signed, int part, int mode) {
    // mode 0 = mull, 1 = mlal, 2 = mlsl
    V128 r = mode ? acc : (V128){{0}};
    int n = 8 >> sz;
    for (int i = 0; i < n; i++) {
        int k = i + (part ? n : 0);
        uint64_t p = is_signed ? (uint64_t)(vgets(a, sz, k) * vgets(b, sz, k)) : vget(a, sz, k) * vget(b, sz, k);
        uint64_t prev = mode ? vget(r, sz + 1, i) : 0;
        vset(&r, sz + 1, i, mode == 2 ? prev - p : prev + p);
    }
    return r;
}
RC_INLINE V128 v_addw(V128 a, V128 b, int sz, int is_signed, int part, int sub) {
    V128 r = {{0}};
    int n = 8 >> sz;
    for (int i = 0; i < n; i++) {
        int k = i + (part ? n : 0);
        uint64_t e = is_signed ? (uint64_t)vgets(b, sz, k) : vget(b, sz, k);
        vset(&r, sz + 1, i, sub ? vget(a, sz + 1, i) - e : vget(a, sz + 1, i) + e);
    }
    return r;
}
RC_INLINE V128 v_addl(V128 a, V128 b, int sz, int is_signed, int part, int sub) {
    V128 r = {{0}};
    int n = 8 >> sz;
    for (int i = 0; i < n; i++) {
        int k = i + (part ? n : 0);
        uint64_t x = is_signed ? (uint64_t)vgets(a, sz, k) : vget(a, sz, k);
        uint64_t y = is_signed ? (uint64_t)vgets(b, sz, k) : vget(b, sz, k);
        vset(&r, sz + 1, i, sub ? x - y : x + y);
    }
    return r;
}
V128 rc_pmull(V128 a, V128 b, int sz, int part);   // polynomial multiply (runtime)

// shifts by immediate
RC_INLINE V128 v_shl(V128 a, int sz, int sh, int q) {
    V128 r = {{0}};
    for (int i = 0; i < VN(sz, q); i++) vset(&r, sz, i, vget(a, sz, i) << sh);
    return r;
}
RC_INLINE V128 v_ushr(V128 a, int sz, int sh, int q, int round) {
    V128 r = {{0}};
    for (int i = 0; i < VN(sz, q); i++) {
        unsigned __int128 x = vget(a, sz, i);
        if (round) x += (unsigned __int128)1 << (sh - 1);
        vset(&r, sz, i, (uint64_t)(x >> sh));
    }
    return r;
}
RC_INLINE V128 v_sshr(V128 a, int sz, int sh, int q, int round) {
    V128 r = {{0}};
    for (int i = 0; i < VN(sz, q); i++) {
        __int128 x = vgets(a, sz, i);
        if (round) x += (__int128)1 << (sh - 1);
        vset(&r, sz, i, (uint64_t)(x >> sh));
    }
    return r;
}
// SHRN/RSHRN/SQSHRN/SQRSHRN/UQSHRN...: narrow (2*sz -> sz) after a right shift
RC_INLINE V128 v_shrn(V128 d, V128 a, int sz, int sh, int part, int round, int mode) {
    // mode 0: plain truncation, 1: signed saturate, 2: unsigned saturate (from unsigned), 3: unsigned sat from signed
    V128 r = part ? d : (V128){{0}};
    int n = 8 >> sz;
    for (int i = 0; i < n; i++) {
        __int128 x = (mode == 1 || mode == 3) ? (__int128)vgets(a, sz + 1, i) : (__int128)vget(a, sz + 1, i);
        if (round) x += (__int128)1 << (sh - 1);
        x >>= sh;
        uint64_t v = mode == 0 ? (uint64_t)x : mode == 1 ? (uint64_t)sat_s(x, sz) : sat_u(x, sz);
        vset(&r, sz, i + (part ? n : 0), v);
    }
    if (!part) r.d[1] = 0;
    return r;
}

// across lanes
RC_INLINE uint64_t v_addv(V128 a, int sz, int q) {
    uint64_t s = 0;
    for (int i = 0; i < VN(sz, q); i++) s += vget(a, sz, i);
    return s & emask(sz);
}
RC_INLINE uint64_t v_addlv(V128 a, int sz, int q, int is_signed) {
    uint64_t s = 0;
    for (int i = 0; i < VN(sz, q); i++) s += is_signed ? (uint64_t)vgets(a, sz, i) : vget(a, sz, i);
    return s & emask(sz + 1);
}
RC_INLINE uint64_t v_maxv(V128 a, int sz, int q, int is_signed, int is_min) {
    uint64_t best = vget(a, sz, 0);
    int64_t sbest = vgets(a, sz, 0);
    for (int i = 1; i < VN(sz, q); i++) {
        if (is_signed) {
            int64_t v = vgets(a, sz, i);
            if (is_min ? v < sbest : v > sbest) { sbest = v; best = vget(a, sz, i); }
        } else {
            uint64_t v = vget(a, sz, i);
            if (is_min ? v < best : v > best) best = v;
        }
    }
    return best;
}

// permutes
RC_INLINE V128 v_zip(V128 a, V128 b, int sz, int q, int part) {
    V128 r = {{0}};
    int n = VN(sz, q), half = n / 2;
    for (int i = 0; i < half; i++) {
        vset(&r, sz, 2 * i, vget(a, sz, i + (part ? half : 0)));
        vset(&r, sz, 2 * i + 1, vget(b, sz, i + (part ? half : 0)));
    }
    return r;
}
RC_INLINE V128 v_uzp(V128 a, V128 b, int sz, int q, int part) {
    V128 r = {{0}};
    int n = VN(sz, q), half = n / 2;
    for (int i = 0; i < half; i++) {
        vset(&r, sz, i, vget(a, sz, 2 * i + part));
        vset(&r, sz, i + half, vget(b, sz, 2 * i + part));
    }
    return r;
}
RC_INLINE V128 v_trn(V128 a, V128 b, int sz, int q, int part) {
    V128 r = {{0}};
    int n = VN(sz, q);
    for (int i = 0; i < n / 2; i++) {
        vset(&r, sz, 2 * i, vget(a, sz, 2 * i + part));
        vset(&r, sz, 2 * i + 1, vget(b, sz, 2 * i + part));
    }
    return r;
}
RC_INLINE V128 v_ext(V128 a, V128 b, int imm, int q) {
    uint8_t cat[32];
    int n = q ? 16 : 8;
    memcpy(cat, a.b, (size_t)n);
    memcpy(cat + n, b.b, (size_t)n);
    V128 r = {{0}};
    memcpy(r.b, cat + imm, (size_t)n);
    return r;
}
// TBL/TBX: table = `len` consecutive registers starting at `first` (mod 32)
RC_INLINE V128 v_tbl(const V128* regs, int first, int len, V128 idx, V128 d, int q, int tbx) {
    uint8_t tab[64];
    for (int i = 0; i < len; i++) memcpy(tab + 16 * i, regs[(first + i) & 31].b, 16);
    V128 r = tbx ? d : (V128){{0}};
    for (int i = 0; i < (q ? 16 : 8); i++) {
        unsigned k = idx.b[i];
        if (k < 16u * (unsigned)len) r.b[i] = tab[k];
        else if (!tbx) r.b[i] = 0;
    }
    return vzhi(r, q);
}
// DUP/INS/UMOV/SMOV
RC_INLINE V128 v_dup(uint64_t x, int sz, int q) {
    V128 r = {{0}};
    for (int i = 0; i < VN(sz, q); i++) vset(&r, sz, i, x);
    return r;
}
RC_INLINE V128 v_ins(V128 d, int sz, int i, uint64_t x) {
    vset(&d, sz, i, x);
    return d;
}

// ---- floating point lane-wise ---------------------------------------------------
// fsz: 0 = f32 lanes, 1 = f64 lanes
#define VFMAP2(NAME, EXPRS, EXPRD)                                           \
    RC_INLINE V128 NAME(V128 a, V128 b, int fsz, int q) {                    \
        V128 r = {{0}};                                                        \
        if (fsz == 0) {                                                      \
            for (int i = 0; i < (q ? 4 : 2); i++) {                          \
                float x = a.f[i], y = b.f[i];                                \
                r.f[i] = (EXPRS);                                            \
            }                                                                \
        } else {                                                             \
            for (int i = 0; i < 2; i++) {                                    \
                double x = a.df[i], y = b.df[i];                             \
                r.df[i] = (EXPRD);                                           \
            }                                                                \
        }                                                                    \
        return r;                                                            \
    }
VFMAP2(vf_add, x + y, x + y)
VFMAP2(vf_sub, x - y, x - y)
VFMAP2(vf_mul, x * y, x * y)
VFMAP2(vf_div, x / y, x / y)
VFMAP2(vf_max, fmax_s(x, y), fmax_d(x, y))
VFMAP2(vf_min, fmin_s(x, y), fmin_d(x, y))
VFMAP2(vf_maxnm, fmaxnm_s(x, y), fmaxnm_d(x, y))
VFMAP2(vf_minnm, fminnm_s(x, y), fminnm_d(x, y))
VFMAP2(vf_recps, frecps_s(x, y), frecps_d(x, y))
VFMAP2(vf_rsqrts, frsqrts_s(x, y), frsqrts_d(x, y))
VFMAP2(vf_abd, fabs_s(x - y), fabs_d(x - y))
#define VFCMP(NAME, COND)                                                    \
    RC_INLINE V128 NAME(V128 a, V128 b, int fsz, int q) {                    \
        V128 r = {{0}};                                                        \
        if (fsz == 0) {                                                      \
            for (int i = 0; i < (q ? 4 : 2); i++) {                          \
                float x = a.f[i], y = b.f[i];                                \
                r.s[i] = (COND) ? 0xFFFFFFFFu : 0;                           \
            }                                                                \
        } else {                                                             \
            for (int i = 0; i < 2; i++) {                                    \
                double x = a.df[i], y = b.df[i];                             \
                r.d[i] = (COND) ? ~(uint64_t)0 : 0;                          \
            }                                                                \
        }                                                                    \
        return r;                                                            \
    }
VFCMP(vf_cmeq, x == y)
VFCMP(vf_cmge, x >= y)
VFCMP(vf_cmgt, x > y)
VFCMP(vf_acge, fabs(x) >= fabs(y))
VFCMP(vf_acgt, fabs(x) > fabs(y))
// fused multiply-accumulate: d + a*b (sub: d - a*b)
RC_INLINE V128 vf_mla(V128 d, V128 a, V128 b, int fsz, int q, int sub) {
    V128 r = {{0}};
    if (fsz == 0) {
        for (int i = 0; i < (q ? 4 : 2); i++) r.f[i] = fmaf(sub ? -a.f[i] : a.f[i], b.f[i], d.f[i]);
    } else {
        for (int i = 0; i < 2; i++) r.df[i] = fma(sub ? -a.df[i] : a.df[i], b.df[i], d.df[i]);
    }
    return r;
}
// pairwise add: concatenate a:b, add adjacent pairs
RC_INLINE V128 vf_addp(V128 a, V128 b, int fsz, int q) {
    V128 r = {{0}};
    if (fsz == 0) {
        int n = q ? 4 : 2;
        float t[8];
        memcpy(t, a.f, sizeof(float) * (size_t)n);
        memcpy(t + n, b.f, sizeof(float) * (size_t)n);
        for (int i = 0; i < n; i++) r.f[i] = t[2 * i] + t[2 * i + 1];
    } else {
        r.df[0] = a.df[0] + a.df[1];
        r.df[1] = b.df[0] + b.df[1];
    }
    return r;
}
#define VFMAP1(NAME, EXPRS, EXPRD)                                           \
    RC_INLINE V128 NAME(V128 a, int fsz, int q) {                            \
        V128 r = {{0}};                                                        \
        if (fsz == 0) {                                                      \
            for (int i = 0; i < (q ? 4 : 2); i++) {                          \
                float x = a.f[i];                                            \
                r.f[i] = (EXPRS);                                            \
            }                                                                \
        } else {                                                             \
            for (int i = 0; i < 2; i++) {                                    \
                double x = a.df[i];                                          \
                r.df[i] = (EXPRD);                                           \
            }                                                                \
        }                                                                    \
        return r;                                                            \
    }
VFMAP1(vf_abs, fabs_s(x), fabs_d(x))
VFMAP1(vf_neg, fneg_s(x), fneg_d(x))
VFMAP1(vf_sqrt, sqrt_s(x), sqrt_d(x))
VFMAP1(vf_recpe, rc_frecpe_s(x), rc_frecpe_d(x))
VFMAP1(vf_rsqrte, rc_frsqrte_s(x), rc_frsqrte_d(x))
#define VFCMPZ(NAME, COND)                                                   \
    RC_INLINE V128 NAME(V128 a, int fsz, int q) {                            \
        V128 r = {{0}};                                                        \
        if (fsz == 0) {                                                      \
            for (int i = 0; i < (q ? 4 : 2); i++) {                          \
                float x = a.f[i];                                            \
                r.s[i] = (COND) ? 0xFFFFFFFFu : 0;                           \
            }                                                                \
        } else {                                                             \
            for (int i = 0; i < 2; i++) {                                    \
                double x = a.df[i];                                          \
                r.d[i] = (COND) ? ~(uint64_t)0 : 0;                          \
            }                                                                \
        }                                                                    \
        return r;                                                            \
    }
VFCMPZ(vf_cmeqz, x == 0)
VFCMPZ(vf_cmgez, x >= 0)
VFCMPZ(vf_cmgtz, x > 0)
VFCMPZ(vf_cmlez, x <= 0)
VFCMPZ(vf_cmltz, x < 0)
RC_INLINE V128 vf_rint(V128 a, int fsz, int q, int mode) {
    V128 r = {{0}};
    if (fsz == 0) for (int i = 0; i < (q ? 4 : 2); i++) r.f[i] = rnd_s(a.f[i], mode);
    else for (int i = 0; i < 2; i++) r.df[i] = rnd_d(a.df[i], mode);
    return r;
}
// float <-> int vector conversions, with optional fixed-point fraction bits
RC_INLINE V128 vf_cvt_toint(V128 a, int fsz, int q, int is_unsigned, int mode, int fbits) {
    V128 r = {{0}};
    if (fsz == 0) {
        for (int i = 0; i < (q ? 4 : 2); i++) {
            double x = ldexp((double)a.f[i], fbits);
            r.s[i] = is_unsigned ? cvt_d_u32(x, mode) : (uint32_t)cvt_d_s32(x, mode);
        }
    } else {
        for (int i = 0; i < 2; i++) {
            double x = ldexp(a.df[i], fbits);
            r.d[i] = is_unsigned ? cvt_d_u64(x, mode) : (uint64_t)cvt_d_s64(x, mode);
        }
    }
    return r;
}
RC_INLINE V128 vf_cvt_fromint(V128 a, int fsz, int q, int is_unsigned, int fbits) {
    V128 r = {{0}};
    if (fsz == 0) {
        for (int i = 0; i < (q ? 4 : 2); i++)
            r.f[i] = ldexpf(is_unsigned ? (float)a.s[i] : (float)a.ss[i], -fbits);
    } else {
        for (int i = 0; i < 2; i++)
            r.df[i] = ldexp(is_unsigned ? (double)a.d[i] : (double)a.sd[i], -fbits);
    }
    return r;
}
// FCVTN/FCVTL: double <-> single narrowing/widening
RC_INLINE V128 vf_cvtn(V128 d, V128 a, int part) {
    V128 r = part ? d : (V128){{0}};
    r.f[part * 2 + 0] = (float)a.df[0];
    r.f[part * 2 + 1] = (float)a.df[1];
    return r;
}
RC_INLINE V128 vf_cvtl(V128 a, int part) {
    V128 r;
    r.df[0] = (double)a.f[part * 2 + 0];
    r.df[1] = (double)a.f[part * 2 + 1];
    return r;
}

// ---- crypto (runtime) -----------------------------------------------------------
V128 rc_aese(V128 d, V128 n);
V128 rc_aesd(V128 d, V128 n);
V128 rc_aesmc(V128 n);
V128 rc_aesimc(V128 n);
V128 rc_sha1c(V128 d, uint32_t n, V128 m);
V128 rc_sha1p(V128 d, uint32_t n, V128 m);
V128 rc_sha1m(V128 d, uint32_t n, V128 m);
V128 rc_sha1su0(V128 d, V128 n, V128 m);
V128 rc_sha1su1(V128 d, V128 n);
uint32_t rc_sha1h(uint32_t n);
V128 rc_sha256h(V128 d, V128 n, V128 m);
V128 rc_sha256h2(V128 d, V128 n, V128 m);
V128 rc_sha256su0(V128 d, V128 n);
V128 rc_sha256su1(V128 d, V128 n, V128 m);
