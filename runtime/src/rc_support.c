// Out-of-line helpers for recompiled code: CRC32, ARM reciprocal estimates,
// polynomial multiply, AES and SHA instructions. All follow the Arm ARM
// pseudocode so results match the hardware bit for bit.
#include "rc.h"

// ---- CRC32 / CRC32C (no pre/post inversion, like the instructions) --------------
uint32_t rc_crc32(uint32_t acc, uint64_t val, int bytes, int castagnoli) {
    uint32_t poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (int i = 0; i < bytes; i++) {
        acc ^= (uint8_t)(val >> (8 * i));
        for (int k = 0; k < 8; k++) acc = (acc >> 1) ^ (poly & (0u - (acc & 1)));
    }
    return acc;
}

// ---- FRECPE / FRSQRTE ---------------------------------------------------------------
static int recip_estimate(int a) {          // a in [256, 511]
    a = a * 2 + 1;
    int b = (1 << 19) / a;
    return (b + 1) / 2;                     // in [256, 511]
}
static int rsqrt_estimate(int a) {          // a in [128, 511]
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    int b = 512;
    while ((int64_t)a * (b + 1) * (b + 1) < ((int64_t)1 << 28)) b++;
    return (b + 1) / 2;                     // in [256, 511]
}
static uint16_t g_recip[256], g_rsqrt[512];
static int g_tables_ready;
static void tables(void) {
    if (g_tables_ready) return;
    for (int i = 256; i < 512; i++) g_recip[i - 256] = (uint16_t)recip_estimate(i);
    for (int i = 128; i < 512; i++) g_rsqrt[i] = (uint16_t)rsqrt_estimate(i);
    __atomic_store_n(&g_tables_ready, 1, __ATOMIC_RELEASE);
}

static uint32_t qnan32(uint32_t u) { return u | 0x00400000u; }
static uint64_t qnan64(uint64_t u) { return u | 0x0008000000000000ull; }

float rc_frecpe_s(float x) {
    tables();
    uint32_t u = f2u(x), sign = u & 0x80000000u;
    uint32_t exp = (u >> 23) & 0xFF, frac = u & 0x7FFFFF;
    if (exp == 0xFF && frac) return u2f(qnan32(u));
    if (exp == 0xFF) return u2f(sign);                         // +-inf -> +-0
    if (exp == 0 && frac == 0) return u2f(sign | 0x7F800000u); // +-0 -> +-inf
    if (exp == 0 && !(frac & 0x600000)) return u2f(sign | 0x7F800000u);  // |x| < 2^-128: overflow -> inf (RN)
    int e = (int)exp;
    uint64_t f = (uint64_t)frac << 29;                         // as a 52-bit fraction
    if (e == 0) {
        if (!(f & (1ull << 51))) { e = -1; f = (f << 2) & ((1ull << 52) - 1); }
        else f = (f << 1) & ((1ull << 52) - 1);
    }
    int scaled = (int)(0x100 | (f >> 44));
    int result_exp = 253 - e;
    int est = g_recip[scaled - 256];
    uint32_t rfrac = (uint32_t)(est & 0xFF) << 15;
    if (result_exp == 0) rfrac = (1u << 22) | (rfrac >> 1);
    else if (result_exp == -1) { rfrac = (1u << 21) | (rfrac >> 2); result_exp = 0; }
    return u2f(sign | ((uint32_t)result_exp << 23) | rfrac);
}

double rc_frecpe_d(double x) {
    tables();
    uint64_t u = d2u(x), sign = u & 0x8000000000000000ull;
    uint64_t exp = (u >> 52) & 0x7FF, frac = u & 0xFFFFFFFFFFFFFull;
    if (exp == 0x7FF && frac) return u2d(qnan64(u));
    if (exp == 0x7FF) return u2d(sign);
    if (exp == 0 && frac == 0) return u2d(sign | 0x7FF0000000000000ull);
    if (exp == 0 && !(frac & 0xC000000000000ull)) return u2d(sign | 0x7FF0000000000000ull);  // |x| < 2^-1024
    int e = (int)exp;
    uint64_t f = frac;
    if (e == 0) {
        if (!(f & (1ull << 51))) { e = -1; f = (f << 2) & ((1ull << 52) - 1); }
        else f = (f << 1) & ((1ull << 52) - 1);
    }
    int scaled = (int)(0x100 | (f >> 44));
    int result_exp = 2045 - e;
    int est = g_recip[scaled - 256];
    uint64_t rfrac = (uint64_t)(est & 0xFF) << 44;
    if (result_exp == 0) rfrac = (1ull << 51) | (rfrac >> 1);
    else if (result_exp == -1) { rfrac = (1ull << 50) | (rfrac >> 2); result_exp = 0; }
    return u2d(sign | ((uint64_t)result_exp << 52) | rfrac);
}

float rc_frsqrte_s(float x) {
    tables();
    uint32_t u = f2u(x), sign = u & 0x80000000u;
    uint32_t exp = (u >> 23) & 0xFF, frac = u & 0x7FFFFF;
    if (exp == 0xFF && frac) return u2f(qnan32(u));
    if (exp == 0 && frac == 0) return u2f(sign | 0x7F800000u);
    if (sign) return u2f(0x7FC00000u);                         // negative -> default NaN
    if (exp == 0xFF) return 0.0f;
    int e = (int)exp;
    uint64_t f = (uint64_t)frac << 29;
    if (e == 0) {
        while (!(f & (1ull << 51))) { f = (f << 1) & ((1ull << 52) - 1); e--; }
        f = (f << 1) & ((1ull << 52) - 1);
    }
    int scaled = (e & 1) == 0 ? (int)(0x100 | (f >> 44)) : (int)(0x80 | (f >> 45));
    int result_exp = (380 - e) / 2;
    int est = g_rsqrt[scaled];
    return u2f(((uint32_t)(result_exp & 0xFF) << 23) | ((uint32_t)(est & 0xFF) << 15));
}

double rc_frsqrte_d(double x) {
    tables();
    uint64_t u = d2u(x), sign = u & 0x8000000000000000ull;
    uint64_t exp = (u >> 52) & 0x7FF, frac = u & 0xFFFFFFFFFFFFFull;
    if (exp == 0x7FF && frac) return u2d(qnan64(u));
    if (exp == 0 && frac == 0) return u2d(sign | 0x7FF0000000000000ull);
    if (sign) return u2d(0x7FF8000000000000ull);
    if (exp == 0x7FF) return 0.0;
    int e = (int)exp;
    uint64_t f = frac;
    if (e == 0) {
        while (!(f & (1ull << 51))) { f = (f << 1) & ((1ull << 52) - 1); e--; }
        f = (f << 1) & ((1ull << 52) - 1);
    }
    int scaled = (e & 1) == 0 ? (int)(0x100 | (f >> 44)) : (int)(0x80 | (f >> 45));
    int result_exp = (3068 - e) / 2;
    int est = g_rsqrt[scaled];
    return u2d(((uint64_t)(result_exp & 0x7FF) << 52) | ((uint64_t)(est & 0xFF) << 44));
}

// ---- PMULL ------------------------------------------------------------------------------
static uint64_t clmul8(uint8_t a, uint8_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) if (b >> i & 1) r ^= (uint64_t)a << i;
    return r;
}
V128 rc_pmull(V128 a, V128 b, int sz, int part) {
    V128 r = {0};
    if (sz == 0) {
        for (int i = 0; i < 8; i++) r.h[i] = (uint16_t)clmul8(a.b[i + 8 * part], b.b[i + 8 * part]);
    } else {
        uint64_t x = a.d[part], y = b.d[part], lo = 0, hi = 0;
        for (int i = 0; i < 64; i++) {
            if (y >> i & 1) {
                lo ^= x << i;
                if (i) hi ^= x >> (64 - i);
            }
        }
        r.d[0] = lo;
        r.d[1] = hi;
    }
    return r;
}

// ---- AES ------------------------------------------------------------------------------
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static uint8_t inv_sbox[256];
static void aes_init(void) {
    static int ready;
    if (ready) return;
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
}
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}
V128 rc_aese(V128 d, V128 n) {
    aes_init();
    V128 s, r;
    for (int i = 0; i < 16; i++) s.b[i] = d.b[i] ^ n.b[i];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) r.b[row + 4 * c] = sbox[s.b[row + 4 * ((c + row) & 3)]];
    return r;
}
V128 rc_aesd(V128 d, V128 n) {
    aes_init();
    V128 s, r;
    for (int i = 0; i < 16; i++) s.b[i] = d.b[i] ^ n.b[i];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) r.b[row + 4 * c] = inv_sbox[s.b[row + 4 * ((c - row) & 3)]];
    return r;
}
V128 rc_aesmc(V128 n) {
    V128 r;
    for (int c = 0; c < 4; c++) {
        uint8_t* a = &n.b[4 * c];
        r.b[4 * c + 0] = (uint8_t)(xt(a[0]) ^ (xt(a[1]) ^ a[1]) ^ a[2] ^ a[3]);
        r.b[4 * c + 1] = (uint8_t)(a[0] ^ xt(a[1]) ^ (xt(a[2]) ^ a[2]) ^ a[3]);
        r.b[4 * c + 2] = (uint8_t)(a[0] ^ a[1] ^ xt(a[2]) ^ (xt(a[3]) ^ a[3]));
        r.b[4 * c + 3] = (uint8_t)((xt(a[0]) ^ a[0]) ^ a[1] ^ a[2] ^ xt(a[3]));
    }
    return r;
}
V128 rc_aesimc(V128 n) {
    V128 r;
    for (int c = 0; c < 4; c++) {
        uint8_t* a = &n.b[4 * c];
        r.b[4 * c + 0] = gmul(a[0], 14) ^ gmul(a[1], 11) ^ gmul(a[2], 13) ^ gmul(a[3], 9);
        r.b[4 * c + 1] = gmul(a[0], 9) ^ gmul(a[1], 14) ^ gmul(a[2], 11) ^ gmul(a[3], 13);
        r.b[4 * c + 2] = gmul(a[0], 13) ^ gmul(a[1], 9) ^ gmul(a[2], 14) ^ gmul(a[3], 11);
        r.b[4 * c + 3] = gmul(a[0], 11) ^ gmul(a[1], 13) ^ gmul(a[2], 9) ^ gmul(a[3], 14);
    }
    return r;
}

// ---- SHA ------------------------------------------------------------------------------
static uint32_t rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static uint32_t sha_choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static uint32_t sha_major(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }

static V128 sha1_round(V128 x, uint32_t y, V128 w, int kind) {
    for (int e = 0; e < 4; e++) {
        uint32_t t = kind == 0 ? sha_choose(x.s[1], x.s[2], x.s[3])
                   : kind == 1 ? sha_parity(x.s[1], x.s[2], x.s[3])
                               : sha_major(x.s[1], x.s[2], x.s[3]);
        y = y + rol32(x.s[0], 5) + t + w.s[e];
        x.s[1] = rol32(x.s[1], 30);
        uint32_t ny = x.s[3];
        x.s[3] = x.s[2]; x.s[2] = x.s[1]; x.s[1] = x.s[0]; x.s[0] = y;
        y = ny;
    }
    return x;
}
V128 rc_sha1c(V128 d, uint32_t n, V128 m) { return sha1_round(d, n, m, 0); }
V128 rc_sha1p(V128 d, uint32_t n, V128 m) { return sha1_round(d, n, m, 1); }
V128 rc_sha1m(V128 d, uint32_t n, V128 m) { return sha1_round(d, n, m, 2); }
uint32_t rc_sha1h(uint32_t n) { return rol32(n, 30); }
V128 rc_sha1su0(V128 d, V128 n, V128 m) {
    V128 r;
    r.d[0] = d.d[1];
    r.d[1] = n.d[0];
    for (int i = 0; i < 2; i++) r.d[i] ^= d.d[i] ^ m.d[i];
    return r;
}
V128 rc_sha1su1(V128 d, V128 n) {
    V128 t, r;
    uint32_t y[4] = {n.s[1], n.s[2], n.s[3], 0};
    for (int i = 0; i < 4; i++) t.s[i] = d.s[i] ^ y[i];
    r.s[0] = rol32(t.s[0], 1);
    r.s[1] = rol32(t.s[1], 1);
    r.s[2] = rol32(t.s[2], 1);
    r.s[3] = rol32(t.s[3], 1) ^ rol32(t.s[0], 2);
    return r;
}
static uint32_t ror32c(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }
static V128 sha256_hash(V128 x, V128 y, V128 w, int part1) {
    for (int e = 0; e < 4; e++) {
        uint32_t chs = sha_choose(y.s[0], y.s[1], y.s[2]);
        uint32_t maj = sha_major(x.s[0], x.s[1], x.s[2]);
        uint32_t t1 = y.s[3] + (ror32c(y.s[0], 6) ^ ror32c(y.s[0], 11) ^ ror32c(y.s[0], 25)) + chs + w.s[e];
        x.s[3] = t1 + x.s[3];
        y.s[3] = t1 + (ror32c(x.s[0], 2) ^ ror32c(x.s[0], 13) ^ ror32c(x.s[0], 22)) + maj;
        uint32_t y3 = y.s[3], x3 = x.s[3];
        y.s[3] = y.s[2]; y.s[2] = y.s[1]; y.s[1] = y.s[0]; y.s[0] = x3;
        x.s[3] = x.s[2]; x.s[2] = x.s[1]; x.s[1] = x.s[0]; x.s[0] = y3;
    }
    return part1 ? x : y;
}
V128 rc_sha256h(V128 d, V128 n, V128 m) { return sha256_hash(d, n, m, 1); }
V128 rc_sha256h2(V128 d, V128 n, V128 m) { return sha256_hash(n, d, m, 0); }
V128 rc_sha256su0(V128 d, V128 n) {
    uint32_t t[4] = {d.s[1], d.s[2], d.s[3], n.s[0]};
    V128 r;
    for (int e = 0; e < 4; e++) r.s[e] = (ror32c(t[e], 7) ^ ror32c(t[e], 18) ^ (t[e] >> 3)) + d.s[e];
    return r;
}
V128 rc_sha256su1(V128 d, V128 n, V128 m) {
    uint32_t t0[4] = {n.s[1], n.s[2], n.s[3], m.s[0]};
    V128 r;
    uint32_t t1[2] = {m.s[2], m.s[3]};
    for (int e = 0; e < 2; e++) {
        uint32_t elt = ror32c(t1[e], 17) ^ ror32c(t1[e], 19) ^ (t1[e] >> 10);
        r.s[e] = elt + d.s[e] + t0[e];
    }
    for (int e = 2; e < 4; e++) {
        uint32_t v = r.s[e - 2];
        uint32_t elt = ror32c(v, 17) ^ ror32c(v, 19) ^ (v >> 10);
        r.s[e] = elt + d.s[e] + t0[e];
    }
    return r;
}
