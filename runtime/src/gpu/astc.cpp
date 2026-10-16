// ASTC LDR block decoder (2D), written from the Khronos Data Format Specification's
// ASTC chapter: block mode, integer sequence encoding (trits/quints), color endpoint
// modes, partition hashing and weight infill. HDR blocks decode to magenta.
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "texture.h"

namespace gpu {
namespace {

struct BitReader {
    const uint8_t* d;
    uint32_t get(int start, int count) const {
        uint32_t v = 0;
        for (int i = 0; i < count; i++) {
            int b = start + i;
            v |= (uint32_t)((d[b >> 3] >> (b & 7)) & 1) << i;
        }
        return v;
    }
};

// quantization levels: index -> (trits, quints, bits)
struct Q { int trits, quints, bits; };
const Q kQuant[21] = {{0, 0, 1}, {1, 0, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 1}, {0, 0, 3}, {0, 1, 1},
                      {1, 0, 2}, {0, 0, 4}, {0, 1, 2}, {1, 0, 3}, {0, 0, 5}, {0, 1, 3}, {1, 0, 4},
                      {0, 0, 6}, {0, 1, 4}, {1, 0, 5}, {0, 0, 7}, {0, 1, 5}, {1, 0, 6}, {0, 0, 8}};

int ise_bits(int n, int q) {
    const Q& z = kQuant[q];
    return n * z.bits + (z.trits ? (8 * n + 4) / 5 : 0) + (z.quints ? (7 * n + 2) / 3 : 0);
}

// decode `n` ISE values of quant level q starting at bit `start` (reading upward)
void ise_decode(const BitReader& br, int start, int n, int q, int* out) {
    const Q& z = kQuant[q];
    int pos = start;
    if (z.trits) {
        for (int i = 0; i < n; i += 5) {
            int m[5] = {0, 0, 0, 0, 0};
            uint32_t T = 0;
            int b = z.bits;
            m[0] = (int)br.get(pos, b); pos += b; T |= br.get(pos, 2); pos += 2;
            m[1] = (int)br.get(pos, b); pos += b; T |= br.get(pos, 2) << 2; pos += 2;
            m[2] = (int)br.get(pos, b); pos += b; T |= br.get(pos, 1) << 4; pos += 1;
            m[3] = (int)br.get(pos, b); pos += b; T |= br.get(pos, 2) << 5; pos += 2;
            m[4] = (int)br.get(pos, b); pos += b; T |= br.get(pos, 1) << 7; pos += 1;
            int t[5], C;
            if (((T >> 2) & 7) == 7) {
                C = (int)(((T >> 5) & 7) << 2 | (T & 3));
                t[4] = 2;
                t[3] = 2;
            } else {
                C = (int)(T & 31);
                if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = (int)((T >> 7) & 1); }
                else { t[4] = (int)((T >> 7) & 1); t[3] = (int)((T >> 5) & 3); }
            }
            if ((C & 3) == 3) {
                t[2] = 2; t[1] = (C >> 4) & 1; t[0] = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1));
            } else if (((C >> 2) & 3) == 3) {
                t[2] = 2; t[1] = 2; t[0] = C & 3;
            } else {
                t[2] = (C >> 4) & 1; t[1] = (C >> 2) & 3; t[0] = ((C >> 1) & 1) << 1 | ((C & 1) & ~((C >> 1) & 1));
            }
            for (int k = 0; k < 5 && i + k < n; k++) out[i + k] = (t[k] << b) | m[k];
        }
    } else if (z.quints) {
        for (int i = 0; i < n; i += 3) {
            int m[3] = {0, 0, 0};
            uint32_t Qv = 0;
            int b = z.bits;
            m[0] = (int)br.get(pos, b); pos += b; Qv |= br.get(pos, 3); pos += 3;
            m[1] = (int)br.get(pos, b); pos += b; Qv |= br.get(pos, 2) << 3; pos += 2;
            m[2] = (int)br.get(pos, b); pos += b; Qv |= br.get(pos, 2) << 5; pos += 2;
            int q[3], C;
            if (((Qv >> 1) & 3) == 3 && ((Qv >> 5) & 3) == 0) {
                int q0b = (int)(Qv & 1);
                q[2] = (q0b << 2) | ((((Qv >> 4) & 1) & ~q0b) << 1) | (((Qv >> 3) & 1) & ~q0b);
                q[1] = 4;
                q[0] = 4;
            } else {
                if (((Qv >> 1) & 3) == 3) {
                    q[2] = 4;
                    C = (int)((((Qv >> 3) & 3) << 3) | ((~(Qv >> 5) & 3) << 1) | (Qv & 1));
                } else {
                    q[2] = (int)((Qv >> 5) & 3);
                    C = (int)(Qv & 31);
                }
                if ((C & 7) == 5) { q[1] = 4; q[0] = (C >> 3) & 3; }
                else { q[1] = (C >> 3) & 3; q[0] = C & 7; }
            }
            for (int k = 0; k < 3 && i + k < n; k++) out[i + k] = (q[k] << b) | m[k];
        }
    } else {
        for (int i = 0; i < n; i++) { out[i] = (int)br.get(pos, z.bits); pos += z.bits; }
    }
}

int replicate(int v, int from_bits, int to_bits) {
    if (from_bits == 0) return 0;
    int r = 0, shift = to_bits;
    while (shift > 0) {
        shift -= from_bits;
        r |= shift >= 0 ? v << shift : v >> -shift;
    }
    return r & ((1 << to_bits) - 1);
}

int unquant_color(int v, int q) {
    const Q& z = kQuant[q];
    if (!z.trits && !z.quints) return replicate(v, z.bits, 8);
    int m = v & ((1 << z.bits) - 1), D = v >> z.bits;
    int A = (m & 1) ? 0x1FF : 0, B = 0, C = 0;
    int b = z.bits;
    if (z.trits) {
        switch (b) {
        case 1: C = 204; break;
        case 2: { int x = (m >> 1) & 1; B = (x << 8) | (x << 4) | (x << 2) | (x << 1); C = 93; break; }
        case 3: { int x = (m >> 1) & 3; B = (x << 7) | (x << 2) | x; C = 44; break; }
        case 4: { int x = (m >> 1) & 7; B = (x << 6) | x; C = 22; break; }
        case 5: { int x = (m >> 1) & 15; B = (x << 5) | (x >> 2); C = 11; break; }
        case 6: { int x = (m >> 1) & 31; B = (x << 4) | (x >> 4); C = 5; break; }
        }
    } else {
        switch (b) {
        case 1: C = 113; break;
        case 2: { int x = (m >> 1) & 1; B = (x << 8) | (x << 3) | (x << 2); C = 54; break; }
        case 3: { int x = (m >> 1) & 3; B = (x << 7) | (x << 1) | (x >> 1); C = 26; break; }
        case 4: { int x = (m >> 1) & 7; B = (x << 6) | (x >> 1); C = 13; break; }
        case 5: { int x = (m >> 1) & 15; B = (x << 5) | (x >> 3); C = 6; break; }
        }
    }
    int T = D * C + B;
    T ^= A;
    return (A & 0x80) | (T >> 2);
}

int unquant_weight(int v, int q) {
    const Q& z = kQuant[q];
    int r;
    if (!z.trits && !z.quints) {
        r = replicate(v, z.bits, 6);
    } else if (z.bits == 0) {
        if (z.trits) { static const int t[3] = {0, 32, 64}; return t[v]; }
        static const int qv[5] = {0, 16, 32, 48, 64};
        return qv[v];
    } else {
        int m = v & ((1 << z.bits) - 1), D = v >> z.bits;
        int A = (m & 1) ? 0x7F : 0, B = 0, C = 0;
        if (z.trits) {
            if (z.bits == 1) C = 50;
            else if (z.bits == 2) { int x = (m >> 1) & 1; B = (x << 6) | (x << 2) | x; C = 23; }
            else { int x = (m >> 1) & 3; B = (x << 5) | x; C = 11; }
        } else {
            if (z.bits == 1) C = 28;
            else { int x = (m >> 1) & 1; B = (x << 6) | (x << 1); C = 13; }
        }
        int T = D * C + B;
        T ^= A;
        r = (A & 0x20) | (T >> 2);
    }
    return r > 32 ? r + 1 : r;
}

bool block_mode(uint32_t mode, int& N, int& M, bool& dual, int& q) {
    int bq = (mode >> 4) & 1, H = (mode >> 9) & 1, D = (mode >> 10) & 1, A = (mode >> 5) & 3;
    if (mode & 3) {
        bq |= (int)(mode & 3) << 1;
        int B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
        case 0: N = B + 4; M = A + 2; break;
        case 1: N = B + 8; M = A + 2; break;
        case 2: N = A + 2; M = B + 8; break;
        default:
            B &= 1;
            if (mode & 0x100) { N = B + 2; M = A + 2; }
            else { N = A + 2; M = B + 6; }
        }
    } else {
        bq |= (int)((mode >> 2) & 3) << 1;
        if (((mode >> 2) & 3) == 0) return false;
        int B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
        case 0: N = 12; M = A + 2; break;
        case 1: N = A + 2; M = 12; break;
        case 2: N = A + 6; M = B + 6; D = 0; H = 0; break;
        default:
            switch ((mode >> 5) & 3) {
            case 0: N = 6; M = 10; break;
            case 1: N = 10; M = 6; break;
            default: return false;
            }
        }
    }
    dual = D != 0;
    q = (bq - 2) + 6 * H;
    if (q < 0 || q > 11) return false;
    int count = N * M * (dual ? 2 : 1);
    int bits = ise_bits(count, q);
    return count <= 64 && bits >= 24 && bits <= 96;
}

uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p *= 0xEEDE0891u; p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17;
    return p;
}

int select_partition(int seed, int x, int y, int count, bool small) {
    if (small) { x <<= 1; y <<= 1; }
    seed += (count - 1) * 1024;
    uint32_t r = hash52((uint32_t)seed);
    uint32_t s[13];
    s[1] = r & 15; s[2] = (r >> 4) & 15; s[3] = (r >> 8) & 15; s[4] = (r >> 12) & 15; s[5] = (r >> 16) & 15;
    s[6] = (r >> 20) & 15; s[7] = (r >> 24) & 15; s[8] = (r >> 28) & 15; s[9] = (r >> 18) & 15; s[10] = (r >> 22) & 15;
    s[11] = (r >> 26) & 15; s[12] = ((r >> 30) | (r << 2)) & 15;
    for (int i = 1; i <= 12; i++) s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    int sh3 = (seed & 0x10) ? sh1 : sh2;
    for (int i = 1; i <= 8; i++) s[i] >>= (i & 1) ? sh1 : sh2;
    for (int i = 9; i <= 12; i++) s[i] >>= sh3;
    int a = (int)((s[1] * x + s[2] * y + (r >> 14)) & 0x3F);
    int b = (int)((s[3] * x + s[4] * y + (r >> 10)) & 0x3F);
    int c = (int)((s[5] * x + s[6] * y + (r >> 6)) & 0x3F);
    int d = (int)((s[7] * x + s[8] * y + (r >> 2)) & 0x3F);
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

void bit_transfer_signed(int& a, int& b) {
    b >>= 1;
    b |= a & 0x80;
    a >>= 1;
    a &= 0x3F;
    if (a & 0x20) a -= 0x40;
}
int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
void blue_contract(int* c) { c[0] = (c[0] + c[2]) >> 1; c[1] = (c[1] + c[2]) >> 1; }

// endpoints e0/e1 (RGBA 0..255) of one color endpoint mode; false for HDR modes
bool endpoints(int mode, int* v, int* e0, int* e1) {
    auto set = [](int* e, int r, int g, int b, int a) { e[0] = r; e[1] = g; e[2] = b; e[3] = a; };
    switch (mode) {
    case 0: set(e0, v[0], v[0], v[0], 255); set(e1, v[1], v[1], v[1], 255); return true;
    case 1: {
        int L0 = (v[0] >> 2) | (v[1] & 0xC0), L1 = std::min(L0 + (v[1] & 0x3F), 255);
        set(e0, L0, L0, L0, 255); set(e1, L1, L1, L1, 255);
        return true;
    }
    case 4: set(e0, v[0], v[0], v[0], v[2]); set(e1, v[1], v[1], v[1], v[3]); return true;
    case 5:
        bit_transfer_signed(v[1], v[0]);
        bit_transfer_signed(v[3], v[2]);
        set(e0, v[0], v[0], v[0], v[2]);
        set(e1, clamp255(v[0] + v[1]), clamp255(v[0] + v[1]), clamp255(v[0] + v[1]), clamp255(v[2] + v[3]));
        return true;
    case 6:
        set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255);
        set(e1, v[0], v[1], v[2], 255);
        return true;
    case 8:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) { set(e0, v[0], v[2], v[4], 255); set(e1, v[1], v[3], v[5], 255); }
        else { set(e0, v[1], v[3], v[5], 255); set(e1, v[0], v[2], v[4], 255); blue_contract(e0); blue_contract(e1); }
        return true;
    case 9:
        bit_transfer_signed(v[1], v[0]);
        bit_transfer_signed(v[3], v[2]);
        bit_transfer_signed(v[5], v[4]);
        if (v[1] + v[3] + v[5] >= 0) {
            set(e0, v[0], v[2], v[4], 255);
            set(e1, v[0] + v[1], v[2] + v[3], v[4] + v[5], 255);
        } else {
            set(e0, v[0] + v[1], v[2] + v[3], v[4] + v[5], 255);
            set(e1, v[0], v[2], v[4], 255);
            blue_contract(e0);
            blue_contract(e1);
        }
        for (int i = 0; i < 4; i++) { e0[i] = clamp255(e0[i]); e1[i] = clamp255(e1[i]); }
        return true;
    case 10:
        set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]);
        set(e1, v[0], v[1], v[2], v[5]);
        return true;
    case 12:
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) { set(e0, v[0], v[2], v[4], v[6]); set(e1, v[1], v[3], v[5], v[7]); }
        else { set(e0, v[1], v[3], v[5], v[7]); set(e1, v[0], v[2], v[4], v[6]); blue_contract(e0); blue_contract(e1); }
        return true;
    case 13:
        bit_transfer_signed(v[1], v[0]);
        bit_transfer_signed(v[3], v[2]);
        bit_transfer_signed(v[5], v[4]);
        bit_transfer_signed(v[7], v[6]);
        if (v[1] + v[3] + v[5] >= 0) {
            set(e0, v[0], v[2], v[4], v[6]);
            set(e1, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]);
        } else {
            set(e0, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]);
            set(e1, v[0], v[2], v[4], v[6]);
            blue_contract(e0);
            blue_contract(e1);
        }
        for (int i = 0; i < 4; i++) { e0[i] = clamp255(e0[i]); e1[i] = clamp255(e1[i]); }
        return true;
    default: return false;     // HDR modes
    }
}

void fill(uint8_t* out, int bw, int bh, int stride, const uint8_t c[4]) {
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) memcpy(out + y * stride + x * 4, c, 4);
}

}  // namespace

void astc_decode_block(const uint8_t block[16], int bw, int bh, uint8_t* out, int stride) {
    static const uint8_t kError[4] = {255, 0, 255, 255};
    BitReader br{block};
    uint32_t mode = br.get(0, 11);
    if ((mode & 0x1FF) == 0x1FC) {                  // void extent: one color
        if (mode & 0x200) { fill(out, bw, bh, stride, kError); return; }
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = (uint8_t)(br.get(64 + 16 * i, 16) >> 8);
        fill(out, bw, bh, stride, c);
        return;
    }
    int N, M, q;
    bool dual;
    if (!block_mode(mode, N, M, dual, q)) { fill(out, bw, bh, stride, kError); return; }
    int parts = (int)br.get(11, 2) + 1;
    if (dual && parts == 4) { fill(out, bw, bh, stride, kError); return; }
    int weight_count = N * M * (dual ? 2 : 1);
    int weight_bits = ise_bits(weight_count, q);
    int cem[4] = {0, 0, 0, 0};
    int seed = 0, color_start, extra_bits = 0;
    if (parts == 1) {
        cem[0] = (int)br.get(13, 4);
        color_start = 17;
    } else {
        seed = (int)br.get(13, 10);
        uint32_t cembits = br.get(23, 6);
        color_start = 29;
        if ((cembits & 3) == 0) {
            for (int i = 0; i < parts; i++) cem[i] = (int)(cembits >> 2);
        } else {
            extra_bits = 3 * parts - 4;
            uint32_t encoded = (cembits >> 2) | (br.get(128 - weight_bits - extra_bits, extra_bits) << 4);
            int base = (int)(cembits & 3) - 1;
            for (int i = 0; i < parts; i++) {
                int C = (int)((encoded >> i) & 1);
                int Mv = (int)((encoded >> (parts + 2 * i)) & 3);
                cem[i] = ((base + C) << 2) | Mv;
            }
        }
    }
    int ccs = 0;
    if (dual) ccs = (int)br.get(128 - weight_bits - extra_bits - 2, 2);
    int color_count = 0;
    for (int i = 0; i < parts; i++) color_count += ((cem[i] >> 2) + 1) * 2;
    if (color_count > 18) { fill(out, bw, bh, stride, kError); return; }
    int color_bits_avail = 128 - weight_bits - extra_bits - (dual ? 2 : 0) - color_start;
    int cq = -1;
    for (int k = 20; k >= 0; k--)
        if (ise_bits(color_count, k) <= color_bits_avail) { cq = k; break; }
    if (cq < 4) { fill(out, bw, bh, stride, kError); return; }   // not enough bits for a valid encoding
    int cv[18];
    ise_decode(br, color_start, color_count, cq, cv);
    for (int i = 0; i < color_count; i++) cv[i] = unquant_color(cv[i], cq);
    int e0[4][4], e1[4][4];
    for (int i = 0, k = 0; i < parts; i++) {
        if (!endpoints(cem[i], cv + k, e0[i], e1[i])) { fill(out, bw, bh, stride, kError); return; }
        k += ((cem[i] >> 2) + 1) * 2;
    }
    // weights are stored bit-reversed from the top of the block
    uint8_t rev[16];
    for (int i = 0; i < 16; i++) {
        uint8_t b = block[15 - i], r = 0;
        for (int j = 0; j < 8; j++) r |= (uint8_t)(((b >> j) & 1) << (7 - j));
        rev[i] = r;
    }
    int wq[64 + 32] = {0};
    ise_decode(BitReader{rev}, 0, weight_count, q, wq);
    int planes = dual ? 2 : 1;
    int wg[2][64 + 16] = {{0}};
    for (int i = 0; i < N * M; i++)
        for (int p = 0; p < planes; p++) wg[p][i] = unquant_weight(wq[i * planes + p], q);
    bool small = bw * bh < 31;
    int Ds = (1024 + bw / 2) / std::max(bw - 1, 1), Dt = (1024 + bh / 2) / std::max(bh - 1, 1);
    for (int t = 0; t < bh; t++)
        for (int s = 0; s < bw; s++) {
            int gs = (Ds * s * (N - 1) + 32) >> 6, gt = (Dt * t * (M - 1) + 32) >> 6;
            int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int v0 = js + jt * N;
            int w[2];
            for (int p = 0; p < planes; p++) {
                auto at = [&](int i) { return i < N * M ? wg[p][i] : 0; };
                w[p] = (at(v0) * w00 + at(v0 + 1) * w01 + at(v0 + N) * w10 + at(v0 + N + 1) * w11 + 8) >> 4;
            }
            int part = parts > 1 ? select_partition(seed, s, t, parts, small) : 0;
            uint8_t* o = out + t * stride + s * 4;
            for (int c = 0; c < 4; c++) {
                int wt = dual && c == ccs ? w[1] : w[0];
                int c0 = e0[part][c] * 257, c1 = e1[part][c] * 257;
                int v = (c0 * (64 - wt) + c1 * wt + 32) >> 6;
                o[c] = (uint8_t)(v >> 8);
            }
        }
}

}  // namespace gpu
