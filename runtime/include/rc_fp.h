// Scalar floating point with AArch64 semantics (FPCR = 0: round-to-nearest,
// no flush-to-zero, NaNs propagate). Where x86 differs (float->int saturation,
// fmin/fmax NaN and signed-zero rules, estimates) these helpers follow ARM.
#pragma once
#include <math.h>

RC_INLINE uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
RC_INLINE float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
RC_INLINE uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
RC_INLINE double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

// scalar register views; writes clear the rest of the 128-bit register
#define FS(n) (c->v[n].f[0])
#define FD(n) (c->v[n].df[0])
#define SETS(rd_, val) (c->v[rd_] = (V128){ .f = { (val) } })
#define SETD(rd_, val) (c->v[rd_] = (V128){ .df = { (val) } })
#define SETSB(rd_, bits) (c->v[rd_] = (V128){ .s = { (uint32_t)(bits) } })
#define SETDB(rd_, bits) (c->v[rd_] = (V128){ .d = { (uint64_t)(bits) } })
#define SETHB(rd_, bits) (c->v[rd_] = (V128){ .h = { (uint16_t)(bits) } })
#define SETBB(rd_, bits) (c->v[rd_] = (V128){ .b = { (uint8_t)(bits) } })

RC_INLINE int isnan_s(float a) { return a != a; }
RC_INLINE int isnan_d(double a) { return a != a; }
RC_INLINE int issnan_s(float a) { return a != a && !(f2u(a) & 0x00400000u); }
RC_INLINE int issnan_d(double a) { return a != a && !(d2u(a) & 0x0008000000000000ull); }

RC_INLINE float fabs_s(float a) { return u2f(f2u(a) & 0x7FFFFFFFu); }
RC_INLINE double fabs_d(double a) { return u2d(d2u(a) & 0x7FFFFFFFFFFFFFFFull); }
RC_INLINE float fneg_s(float a) { return u2f(f2u(a) ^ 0x80000000u); }
RC_INLINE double fneg_d(double a) { return u2d(d2u(a) ^ 0x8000000000000000ull); }

// FMAX/FMIN: any NaN -> NaN; -0 < +0
RC_INLINE float fmax_s(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2f(f2u(a) & f2u(b));
    return a > b ? a : b;
}
RC_INLINE float fmin_s(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2f(f2u(a) | f2u(b));
    return a < b ? a : b;
}
RC_INLINE double fmax_d(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2d(d2u(a) & d2u(b));
    return a > b ? a : b;
}
RC_INLINE double fmin_d(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2d(d2u(a) | d2u(b));
    return a < b ? a : b;
}
// FMAXNM/FMINNM: a quiet NaN loses against a number
RC_INLINE float fmaxnm_s(float a, float b) {
    if (a != a && b == b && !issnan_s(a)) return b;
    if (b != b && a == a && !issnan_s(b)) return a;
    return fmax_s(a, b);
}
RC_INLINE float fminnm_s(float a, float b) {
    if (a != a && b == b && !issnan_s(a)) return b;
    if (b != b && a == a && !issnan_s(b)) return a;
    return fmin_s(a, b);
}
RC_INLINE double fmaxnm_d(double a, double b) {
    if (a != a && b == b && !issnan_d(a)) return b;
    if (b != b && a == a && !issnan_d(b)) return a;
    return fmax_d(a, b);
}
RC_INLINE double fminnm_d(double a, double b) {
    if (a != a && b == b && !issnan_d(a)) return b;
    if (b != b && a == a && !issnan_d(b)) return a;
    return fmin_d(a, b);
}

// FP compare -> NZCV
#define FCMP_FLAGS(a, b) do {                                             \
        if ((a) != (a) || (b) != (b)) { N = 0; Z = 0; C = 1; V = 1; }     \
        else if ((a) == (b)) { N = 0; Z = 1; C = 1; V = 0; }              \
        else if ((a) < (b)) { N = 1; Z = 0; C = 0; V = 0; }               \
        else { N = 0; Z = 0; C = 1; V = 0; }                              \
    } while (0)

// rounding modes for conversions/FRINT: 0 = nearest-even, 1 = +inf, 2 = -inf, 3 = zero, 4 = nearest-away
RC_INLINE double rnd_d(double x, int m) {
    switch (m) {
    case 0: return nearbyint(x);
    case 1: return ceil(x);
    case 2: return floor(x);
    case 3: return trunc(x);
    default: return round(x);
    }
}
RC_INLINE float rnd_s(float x, int m) {
    switch (m) {
    case 0: return nearbyintf(x);
    case 1: return ceilf(x);
    case 2: return floorf(x);
    case 3: return truncf(x);
    default: return roundf(x);
    }
}
// float -> integer with ARM saturation (NaN -> 0)
RC_INLINE int64_t cvt_d_s64(double x, int m) {
    if (x != x) return 0;
    double r = rnd_d(x, m);
    if (r >= 9223372036854775808.0) return INT64_MAX;
    if (r < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)r;
}
RC_INLINE uint64_t cvt_d_u64(double x, int m) {
    if (x != x) return 0;
    double r = rnd_d(x, m);
    if (r >= 18446744073709551616.0) return UINT64_MAX;
    if (r <= 0.0) return 0;
    return (uint64_t)r;
}
RC_INLINE int32_t cvt_d_s32(double x, int m) {
    if (x != x) return 0;
    double r = rnd_d(x, m);
    if (r >= 2147483648.0) return INT32_MAX;
    if (r < -2147483648.0) return INT32_MIN;
    return (int32_t)r;
}
RC_INLINE uint32_t cvt_d_u32(double x, int m) {
    if (x != x) return 0;
    double r = rnd_d(x, m);
    if (r >= 4294967296.0) return UINT32_MAX;
    if (r <= 0.0) return 0;
    return (uint32_t)r;
}
// floats convert exactly to double, so the double versions serve both widths
#define CVT_S64(x, m) ((uint64_t)cvt_d_s64((double)(x), (m)))
#define CVT_U64(x, m) (cvt_d_u64((double)(x), (m)))
#define CVT_S32(x, m) ((uint64_t)(uint32_t)cvt_d_s32((double)(x), (m)))
#define CVT_U32(x, m) ((uint64_t)cvt_d_u32((double)(x), (m)))

// reciprocal / reciprocal-sqrt estimates and Newton steps (exact ARM algorithms, in the runtime)
float rc_frecpe_s(float x);
double rc_frecpe_d(double x);
float rc_frsqrte_s(float x);
double rc_frsqrte_d(double x);
RC_INLINE float frecps_s(float a, float b) {
    if ((isinf(a) && b == 0.0f) || (a == 0.0f && isinf(b))) return 2.0f;
    return fmaf(-a, b, 2.0f);
}
RC_INLINE double frecps_d(double a, double b) {
    if ((isinf(a) && b == 0.0) || (a == 0.0 && isinf(b))) return 2.0;
    return fma(-a, b, 2.0);
}
// (3 - a*b) / 2 with one rounding: 1.5 - a*(b/2), halving whichever operand halves exactly
RC_INLINE float frsqrts_s(float a, float b) {
    if ((isinf(a) && b == 0.0f) || (a == 0.0f && isinf(b))) return 1.5f;
    if (fabsf(b) >= 0x1p-125f || isinf(b) || isnan(b)) return fmaf(-a, b * 0.5f, 1.5f);
    if (fabsf(a) >= 0x1p-125f || isinf(a) || isnan(a)) return fmaf(-(a * 0.5f), b, 1.5f);
    return fmaf(-a, b, 3.0f) * 0.5f;
}
RC_INLINE double frsqrts_d(double a, double b) {
    if ((isinf(a) && b == 0.0) || (a == 0.0 && isinf(b))) return 1.5;
    if (fabs(b) >= 0x1p-1021 || isinf(b) || isnan(b)) return fma(-a, b * 0.5, 1.5);
    if (fabs(a) >= 0x1p-1021 || isinf(a) || isnan(a)) return fma(-(a * 0.5), b, 1.5);
    return fma(-a, b, 3.0) * 0.5;
}
// square root: invalid operands give ARM's default NaN (positive), not x86's negative one
RC_INLINE float sqrt_s(float a) { return a < 0.0f ? u2f(0x7FC00000u) : sqrtf(a); }
RC_INLINE double sqrt_d(double a) { return a < 0.0 ? u2d(0x7FF8000000000000ull) : sqrt(a); }
