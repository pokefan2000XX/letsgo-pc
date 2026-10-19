// Switch container formats for reading the game from the player's NSPs (nx.h): PFS0, NCA (AES-XTS
// headers, AES-CTR sections), the update's BKTR patch (indirect storage + AES-CTR-EX), RomFS
// and NSO. The same steps as tools/nxextract.py, done on demand instead of extracting.
#include "nx.h"

#include <windows.h>
#include <wmmintrin.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace rt::nx {
namespace {

// ---- AES-128 with the CPU's AES instructions (every x86-64-v3 CPU has them) -----------------------
#define AES_FN __attribute__((target("aes,sse4.1")))

AES_FN __m128i expand_step(__m128i key, __m128i gen) {
    gen = _mm_shuffle_epi32(gen, 0xFF);
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    return _mm_xor_si128(key, gen);
}

struct Aes128 {
    __m128i enc[11], dec[11];

    AES_FN explicit Aes128(const uint8_t* key = nullptr) {
        uint8_t zero[16] = {};
        enc[0] = _mm_loadu_si128((const __m128i*)(key ? key : zero));
#define LETSGO_EXPAND(i, rcon) enc[i] = expand_step(enc[i - 1], _mm_aeskeygenassist_si128(enc[i - 1], rcon))
        LETSGO_EXPAND(1, 0x01); LETSGO_EXPAND(2, 0x02); LETSGO_EXPAND(3, 0x04); LETSGO_EXPAND(4, 0x08);
        LETSGO_EXPAND(5, 0x10); LETSGO_EXPAND(6, 0x20); LETSGO_EXPAND(7, 0x40); LETSGO_EXPAND(8, 0x80);
        LETSGO_EXPAND(9, 0x1B); LETSGO_EXPAND(10, 0x36);
#undef LETSGO_EXPAND
        dec[0] = enc[10];
        for (int i = 1; i < 10; i++) dec[i] = _mm_aesimc_si128(enc[10 - i]);
        dec[10] = enc[0];
    }
    AES_FN __m128i encrypt(__m128i x) const {
        x = _mm_xor_si128(x, enc[0]);
        for (int i = 1; i < 10; i++) x = _mm_aesenc_si128(x, enc[i]);
        return _mm_aesenclast_si128(x, enc[10]);
    }
    AES_FN __m128i decrypt(__m128i x) const {
        x = _mm_xor_si128(x, dec[0]);
        for (int i = 1; i < 10; i++) x = _mm_aesdec_si128(x, dec[i]);
        return _mm_aesdeclast_si128(x, dec[10]);
    }
    AES_FN void ecb_decrypt(uint8_t* data, size_t n) const {
        for (size_t i = 0; i + 16 <= n; i += 16)
            _mm_storeu_si128((__m128i*)(data + i), decrypt(_mm_loadu_si128((const __m128i*)(data + i))));
    }
    // XORs data (a multiple of 16 bytes) with the keystream; ctr is the big-endian counter of its first block
    AES_FN void ctr_xor(const uint8_t ctr[16], uint8_t* data, size_t n) const {
        uint64_t hi = 0, lo = 0;
        for (int i = 0; i < 8; i++) hi = hi << 8 | ctr[i];
        for (int i = 8; i < 16; i++) lo = lo << 8 | ctr[i];
        const __m128i swap = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        auto block = [&](uint64_t h, uint64_t l) { return _mm_shuffle_epi8(_mm_set_epi64x((long long)h, (long long)l), swap); };
        size_t i = 0;
        for (; i + 64 <= n; i += 64) {           // four blocks at a time
            __m128i c[4];
            for (int k = 0; k < 4; k++) {
                c[k] = _mm_xor_si128(block(hi, lo), enc[0]);
                if (++lo == 0) hi++;
            }
            for (int r = 1; r < 10; r++)
                for (int k = 0; k < 4; k++) c[k] = _mm_aesenc_si128(c[k], enc[r]);
            for (int k = 0; k < 4; k++) {
                c[k] = _mm_aesenclast_si128(c[k], enc[10]);
                __m128i* p = (__m128i*)(data + i + 16 * k);
                _mm_storeu_si128(p, _mm_xor_si128(_mm_loadu_si128(p), c[k]));
            }
        }
        for (; i + 16 <= n; i += 16) {
            __m128i k = encrypt(block(hi, lo));
            if (++lo == 0) hi++;
            __m128i* p = (__m128i*)(data + i);
            _mm_storeu_si128(p, _mm_xor_si128(_mm_loadu_si128(p), k));
        }
    }
};

// NCA headers: AES-XTS with 0x200-byte sectors and the sector number big-endian in the tweak
AES_FN void xts_decrypt_nintendo(const uint8_t key[32], uint8_t* data, size_t size) {
    Aes128 k1(key), k2(key + 16);
    for (size_t s = 0; s * 0x200 < size; s++) {
        uint8_t tw[16] = {};
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(s >> (8 * i));
        uint8_t t[16];
        _mm_storeu_si128((__m128i*)t, k2.encrypt(_mm_loadu_si128((const __m128i*)tw)));
        for (size_t b = s * 0x200; b < std::min(size, s * 0x200 + 0x200); b += 16) {
            __m128i tv = _mm_loadu_si128((const __m128i*)t);
            __m128i c = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(data + b)), tv);
            _mm_storeu_si128((__m128i*)(data + b), _mm_xor_si128(k1.decrypt(c), tv));
            uint8_t carry = t[15] >> 7;
            for (int i = 15; i > 0; i--) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (uint8_t)(t[0] << 1) ^ (carry ? 0x87 : 0);
        }
    }
}

// ---- files ------------------------------------------------------------------------------------------
class HostFile {
public:
    explicit HostFile(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_RANDOM_ACCESS, nullptr);
        LARGE_INTEGER sz{};
        if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
    }
    ~HostFile() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t size() const { return size_; }
    // positional read, safe from several threads at once
    bool read(uint64_t off, void* out, size_t n) const {
        uint8_t* p = (uint8_t*)out;
        while (n) {
            DWORD chunk = (DWORD)std::min<size_t>(n, 64u << 20), got = 0;
            OVERLAPPED ov{};
            ov.Offset = (DWORD)off;
            ov.OffsetHigh = (DWORD)(off >> 32);
            if (!ReadFile(h_, p, chunk, &got, &ov) || got != chunk) return false;
            p += chunk;
            off += chunk;
            n -= chunk;
        }
        return true;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

using Keys = std::unordered_map<std::string, std::vector<uint8_t>>;
void derive_keys(Keys& keys);

Keys load_keys(const std::wstring& path) {
    Keys keys;
    std::ifstream f{std::filesystem::path(path)};
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
            size_t i = 0;
            while (i < s.size() && isspace((unsigned char)s[i])) i++;
            return s.substr(i);
        };
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        std::vector<uint8_t> bytes;
        bool good = v.size() % 2 == 0 && !v.empty();
        for (size_t i = 0; good && i < v.size(); i += 2) {
            char* end;
            std::string h = v.substr(i, 2);
            unsigned long b = strtoul(h.c_str(), &end, 16);
            if (*end) good = false;
            bytes.push_back((uint8_t)b);
        }
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        if (good) keys[k] = bytes;
    }
    derive_keys(keys);
    return keys;
}

const std::vector<uint8_t>* key(const Keys& keys, const std::string& name, size_t size) {
    auto it = keys.find(name);
    return it != keys.end() && it->second.size() == size ? &it->second : nullptr;
}

std::vector<uint8_t> ecb_decrypt(const std::vector<uint8_t>& k, const uint8_t* in, size_t n) {
    std::vector<uint8_t> out(in, in + n);
    Aes128(k.data()).ecb_decrypt(out.data(), n);
    return out;
}

// prod.keys files differ by firmware and dumping tool: some carry the derived keys, some only the
// master keys and the key sources. Derive whatever is missing the way the console does (hactool,
// Ryujinx), so any prod.keys with the master keys for this game's key generation works.
void derive_keys(Keys& keys) {
    auto get = [&](const std::string& n, size_t size = 16) { return key(keys, n, size); };
    const auto* kek_seed = get("aes_kek_generation_source");
    const auto* key_seed = get("aes_key_generation_source");
    auto generate_kek = [&](const std::vector<uint8_t>& src, const std::vector<uint8_t>& master) {
        std::vector<uint8_t> kek = ecb_decrypt(master, kek_seed->data(), 16);
        std::vector<uint8_t> src_kek = ecb_decrypt(kek, src.data(), 16);
        return key_seed ? ecb_decrypt(src_kek, key_seed->data(), 16) : src_kek;
    };
    char name[64];
    for (int rev = 0; rev < 0x20; rev++) {
        snprintf(name, sizeof name, "master_key_%02x", rev);
        const auto* master = get(name);
        if (!master) continue;
        std::vector<uint8_t> mk = *master;
        snprintf(name, sizeof name, "titlekek_%02x", rev);
        if (!get(name) && get("titlekek_source")) keys[name] = ecb_decrypt(mk, get("titlekek_source")->data(), 16);
        for (const char* area : {"application", "ocean", "system"}) {
            snprintf(name, sizeof name, "key_area_key_%s_source", area);
            const auto* src = get(name);
            snprintf(name, sizeof name, "key_area_key_%s_%02x", area, rev);
            if (!get(name) && src && kek_seed) keys[name] = generate_kek(*src, mk);
        }
    }
    if (!get("header_key", 32) && get("master_key_00") && get("header_kek_source") && get("header_key_source", 32) && kek_seed) {
        std::vector<uint8_t> header_kek = generate_kek(*get("header_kek_source"), *get("master_key_00"));
        keys["header_key"] = ecb_decrypt(header_kek, get("header_key_source", 32)->data(), 32);
    }
}

struct Pfs0Entry {
    std::string name;
    uint64_t offset, size;
};

template <class Read>
bool parse_pfs0(Read read, uint64_t base, std::vector<Pfs0Entry>* out) {
    uint8_t hdr[0x10];
    if (!read(base, hdr, 0x10) || memcmp(hdr, "PFS0", 4)) return false;
    uint32_t count, strsz;
    memcpy(&count, hdr + 4, 4);
    memcpy(&strsz, hdr + 8, 4);
    if (count > 0x10000 || strsz > 0x100000) return false;
    std::vector<uint8_t> ent(count * 0x18), str(strsz + 1);
    if (!read(base + 0x10, ent.data(), ent.size()) || !read(base + 0x10 + ent.size(), str.data(), strsz)) return false;
    uint64_t data0 = base + 0x10 + ent.size() + strsz;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t off, size;
        uint32_t name;
        memcpy(&off, &ent[i * 0x18], 8);
        memcpy(&size, &ent[i * 0x18 + 8], 8);
        memcpy(&name, &ent[i * 0x18 + 16], 4);
        if (name >= strsz) return false;
        out->push_back({std::string((const char*)&str[name]), data0 + off, size});
    }
    return true;
}

// ---- NSP and NCA -----------------------------------------------------------------------------------
struct Nsp {
    HostFile file;
    std::vector<Pfs0Entry> entries;
    std::map<std::string, std::vector<uint8_t>> title_keys;   // rights ID (raw bytes) -> encrypted title key

    explicit Nsp(const std::wstring& path) : file(path) {
        if (!file.ok()) return;
        auto rd = [&](uint64_t o, void* p, size_t n) { return file.read(o, p, n); };
        if (!parse_pfs0(rd, 0, &entries)) { entries.clear(); return; }
        for (auto& e : entries) {
            if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".tik") || e.size < 0x2C0 || e.size > 0x10000) continue;
            std::vector<uint8_t> tik(e.size);
            if (!file.read(e.offset, tik.data(), tik.size())) continue;
            uint32_t sig;
            memcpy(&sig, tik.data(), 4);
            size_t body = sig == 0x10004 ? 0x140 : sig == 0x10005 || sig == 0x10002 ? 0x80 : 0x240;
            if (body + 0x170 > tik.size()) continue;
            title_keys[std::string((const char*)&tik[body + 0x160], 16)] =
                std::vector<uint8_t>(tik.begin() + body + 0x40, tik.begin() + body + 0x50);
        }
    }
    bool ok() const { return !entries.empty(); }
};

struct Section {
    uint64_t start = 0, size = 0;   // in the NCA
    uint8_t fsh[0x200] = {};        // filesystem header
    uint8_t ctr_hi[8] = {};
    bool encrypted = false;
};

struct Nca {
    const Nsp* nsp = nullptr;
    uint64_t offset = 0, size = 0;   // in the NSP
    uint8_t header[0xC00];
    uint8_t content_type = 0xFF;
    uint64_t title_id = 0;
    Aes128 ctr_key;
    Section sections[4];
    bool has[4] = {};

    // decrypts [off, off + n) of a section; generation replaces the low half of the counter's top 8 bytes (BKTR)
    bool read(int s, uint64_t off, void* out, size_t n, const uint32_t* generation = nullptr) const {
        const Section& sec = sections[s];
        uint64_t a = off & ~0xFull, b = (off + n + 0xF) & ~0xFull;
        if (b > sec.size) return false;
        std::vector<uint8_t> buf(b - a);
        if (!nsp->file.read(offset + sec.start + a, buf.data(), buf.size())) return false;
        if (sec.encrypted) {
            uint8_t ctr[16];
            memcpy(ctr, sec.ctr_hi, 8);
            if (generation)
                for (int i = 0; i < 4; i++) ctr[4 + i] = (uint8_t)(*generation >> (24 - 8 * i));
            uint64_t blk = (sec.start + a) >> 4;
            for (int i = 0; i < 8; i++) ctr[15 - i] = (uint8_t)(blk >> (8 * i));
            ctr_key.ctr_xor(ctr, buf.data(), buf.size());
        }
        memcpy(out, buf.data() + (off - a), n);
        return true;
    }
};

bool decrypt_header(const Keys& keys, const Nsp& nsp, const Pfs0Entry& e, uint8_t header[0xC00]) {
    const auto* hk = key(keys, "header_key", 32);
    if (!hk || e.size < 0xC00 || !nsp.file.read(e.offset, header, 0xC00)) return false;
    xts_decrypt_nintendo(hk->data(), header, 0xC00);
    return !memcmp(header + 0x200, "NCA3", 4);
}

bool open_nca(const Keys& keys, const Nsp& nsp, const Pfs0Entry& e, Nca* nca, std::string* error) {
    nca->nsp = &nsp;
    nca->offset = e.offset;
    nca->size = e.size;
    if (!decrypt_header(keys, nsp, e, nca->header)) {
        *error = "the keys don't decrypt this NSP (prod.keys is missing header_key or is from another console)";
        return false;
    }
    const uint8_t* h = nca->header;
    nca->content_type = h[0x205];
    memcpy(&nca->title_id, h + 0x210, 8);
    int keygen = std::max(h[0x206], h[0x220]);
    int rev = std::max(keygen - 1, 0);
    char name[64];
    static const uint8_t zero[16] = {};
    if (memcmp(h + 0x230, zero, 16)) {        // rights ID: the title key from the NSP's ticket
        auto it = nsp.title_keys.find(std::string((const char*)h + 0x230, 16));
        if (it == nsp.title_keys.end()) { *error = "the NSP has no ticket for its content"; return false; }
        snprintf(name, sizeof name, "titlekek_%02x", rev);
        const auto* kek = key(keys, name, 16);
        if (!kek) { *error = std::string("prod.keys has no ") + name + " (the keys are too old for this game)"; return false; }
        uint8_t tk[16];
        memcpy(tk, it->second.data(), 16);
        Aes128(kek->data()).ecb_decrypt(tk, 16);
        nca->ctr_key = Aes128(tk);
    } else {
        static const char* areas[] = {"application", "ocean", "system"};
        snprintf(name, sizeof name, "key_area_key_%s_%02x", areas[h[0x207] % 3], rev);
        const auto* kak = key(keys, name, 16);
        if (!kak) { *error = std::string("prod.keys has no ") + name + " (the keys are too old for this game)"; return false; }
        uint8_t area[0x40];
        memcpy(area, h + 0x300, 0x40);
        Aes128(kak->data()).ecb_decrypt(area, 0x40);
        nca->ctr_key = Aes128(area + 0x20);
    }
    for (int i = 0; i < 4; i++) {
        uint32_t st, en;
        memcpy(&st, h + 0x240 + i * 0x10, 4);
        memcpy(&en, h + 0x244 + i * 0x10, 4);
        if (!en) continue;
        Section& s = nca->sections[i];
        s.start = (uint64_t)st * 0x200;
        s.size = (uint64_t)(en - st) * 0x200;
        memcpy(s.fsh, h + 0x400 + i * 0x200, 0x200);
        s.encrypted = s.fsh[4] != 1;
        for (int k = 0; k < 8; k++) s.ctr_hi[k] = s.fsh[0x147 - k];
        nca->has[i] = true;
    }
    return true;
}

// An update's program: its RomFS section is a BKTR patch over the base game's. (It keeps the base
// game's title ID; the update's own ID is only in its ticket and its metadata.)
bool is_patch(const Nca& nca) {
    return nca.has[1] && !memcmp(nca.sections[1].fsh + 0x110, "BKTR", 4);
}

// the program NCA of an NSP (content type 0)
bool program_nca(const Keys& keys, const Nsp& nsp, Nca* out, std::string* error) {
    std::string err = "the NSP has no game program in it";
    for (auto& e : nsp.entries) {
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".nca")) continue;
        uint8_t hdr[0xC00];
        if (!decrypt_header(keys, nsp, e, hdr)) {
            err = "the keys don't decrypt this NSP (prod.keys is missing header_key or is from another console)";
            continue;
        }
        if (hdr[0x205] != 0) continue;
        return open_nca(keys, nsp, e, out, error);
    }
    *error = err;
    return false;
}

// ---- BKTR: the update's RomFS section, patched over the base game's ------------------------------------
struct BucketEntry {
    uint64_t a, b;
    int32_t c;
};

bool parse_bucket_tree(const std::vector<uint8_t>& blob, size_t entry_size, bool relocation, uint64_t* end,
                       std::vector<BucketEntry>* out) {
    if (blob.size() < 0x4000) return false;
    uint32_t sets;
    memcpy(&sets, &blob[4], 4);
    memcpy(end, &blob[8], 8);
    if (!sets || sets > 0x7FE) return false;
    for (uint32_t s = 0; s < sets; s++) {
        size_t base = 0x4000 + (size_t)s * 0x4000;
        if (base + 0x4000 > blob.size()) return false;
        uint32_t n;
        memcpy(&n, &blob[base + 4], 4);
        if (n > (0x4000 - 0x10) / entry_size) return false;
        for (uint32_t e = 0; e < n; e++) {
            const uint8_t* p = &blob[base + 0x10 + e * entry_size];
            BucketEntry be{};
            memcpy(&be.a, p, 8);
            if (relocation) {            // virtual offset, physical offset, storage (0 base, 1 patch)
                memcpy(&be.b, p + 8, 8);
                memcpy(&be.c, p + 16, 4);
            } else {                     // offset, size, CTR generation
                uint32_t gen;
                memcpy(&gen, p + 12, 4);
                be.c = (int32_t)gen;
            }
            out->push_back(be);
        }
    }
    return true;
}

class Bktr {
public:
    bool open(const Nca* base, const Nca* patch, std::string* error) {
        base_ = base;
        patch_ = patch;
        const uint8_t* fsh = patch->sections[1].fsh;
        uint64_t ind_off, ind_size, ctr_off, ctr_size;
        memcpy(&ind_off, fsh + 0x100, 8);
        memcpy(&ind_size, fsh + 0x108, 8);
        memcpy(&ctr_off, fsh + 0x120, 8);
        memcpy(&ctr_size, fsh + 0x128, 8);
        *error = "the update NSP isn't a patch for this base game";
        if (memcmp(fsh + 0x110, "BKTR", 4) || memcmp(fsh + 0x130, "BKTR", 4) || ind_size > (64u << 20) || ctr_size > (64u << 20))
            return false;
        std::vector<uint8_t> blob(ctr_size);
        uint64_t sub_end;
        std::vector<BucketEntry> subs;
        if (!patch_->read(1, ctr_off, blob.data(), blob.size()) || !parse_bucket_tree(blob, 0x10, false, &sub_end, &subs))
            return false;
        for (auto& s : subs) {
            sub_starts_.push_back(s.a);
            sub_gens_.push_back((uint32_t)s.c);
        }
        sub_end_ = ctr_off;
        blob.assign(ind_size, 0);
        std::vector<BucketEntry> rel;
        if (!read_patch(ind_off, blob.data(), blob.size()) || !parse_bucket_tree(blob, 0x14, true, &size_, &rel)) {
            rel.clear();      // older layouts encrypt the indirect table with the section's own counter
            if (!patch_->read(1, ind_off, blob.data(), blob.size()) || !parse_bucket_tree(blob, 0x14, true, &size_, &rel))
                return false;
        }
        for (auto& r : rel) {
            v_starts_.push_back(r.a);
            phys_.push_back(r.b);
            storage_.push_back(r.c);
        }
        return !rel.empty();
    }
    uint64_t size() const { return size_; }

    bool read(uint64_t off, void* out, size_t n) const {
        uint8_t* p = (uint8_t*)out;
        while (n) {
            size_t i = (size_t)(std::upper_bound(v_starts_.begin(), v_starts_.end(), off) - v_starts_.begin());
            if (!i) return false;
            i--;
            uint64_t next = i + 1 < v_starts_.size() ? v_starts_[i + 1] : size_;
            size_t k = (size_t)std::min<uint64_t>(n, next - off);
            uint64_t phys = phys_[i] + (off - v_starts_[i]);
            bool ok = storage_[i] == 1 ? read_patch(phys, p, k) : base_->read(1, phys, p, k);
            if (!ok) return false;
            p += k;
            off += k;
            n -= k;
        }
        return true;
    }

private:
    // the patch's own data: each subsection has its own CTR generation
    bool read_patch(uint64_t off, uint8_t* p, size_t n) const {
        while (n) {
            size_t i = (size_t)(std::upper_bound(sub_starts_.begin(), sub_starts_.end(), off) - sub_starts_.begin());
            if (!i) return false;
            i--;
            uint64_t next = i + 1 < sub_starts_.size() ? sub_starts_[i + 1] : sub_end_;
            size_t k = (size_t)std::min<uint64_t>(n, next - off);
            if (!k || !patch_->read(1, off, p, k, &sub_gens_[i])) return false;
            p += k;
            off += k;
            n -= k;
        }
        return true;
    }

    const Nca* base_ = nullptr;
    const Nca* patch_ = nullptr;
    std::vector<uint64_t> sub_starts_;
    std::vector<uint32_t> sub_gens_;
    uint64_t sub_end_ = 0;
    std::vector<uint64_t> v_starts_, phys_;
    std::vector<int32_t> storage_;
    uint64_t size_ = 0;
};

// ---- the game: v1.0.2 RomFS and ExeFS ---------------------------------------------------------------------
std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

class GameImpl : public Game {
public:
    bool open(const std::wstring& base_path, const std::wstring& update_path, const std::wstring& keys_path,
              std::string* error) {
        keys_ = load_keys(keys_path);
        if (!key(keys_, "header_key", 32)) { *error = "prod.keys has no header_key"; return false; }
        base_nsp_ = std::make_unique<Nsp>(base_path);
        update_nsp_ = std::make_unique<Nsp>(update_path);
        if (!base_nsp_->ok()) { *error = "the base game NSP can't be read"; return false; }
        if (!update_nsp_->ok()) { *error = "the update NSP can't be read"; return false; }
        if (!program_nca(keys_, *base_nsp_, &base_, error)) { *error = "base game NSP: " + *error; return false; }
        if (!program_nca(keys_, *update_nsp_, &update_, error)) { *error = "update NSP: " + *error; return false; }
        if (base_.title_id != 0x010003F003A34000ull || is_patch(base_)) {
            *error = "the base game NSP isn't Pok\xC3\xA9mon: Let's Go, Pikachu!";
            return false;
        }
        if (update_.title_id != 0x010003F003A34000ull || !is_patch(update_)) {
            *error = "the update NSP isn't the update for Pok\xC3\xA9mon: Let's Go, Pikachu!";
            return false;
        }
        if (!base_.has[1] || !update_.has[0] || !update_.has[1]) { *error = "the NSPs are missing sections"; return false; }
        // ExeFS (complete in the update)
        uint64_t pfs_off;
        memcpy(&pfs_off, update_.sections[0].fsh + 0x8 + 0x38, 8);
        auto rd0 = [&](uint64_t o, void* p, size_t n) { return update_.read(0, o, p, n); };
        if (!parse_pfs0(rd0, pfs_off, &exefs_)) { *error = "the update's program files can't be read"; return false; }
        if (!bktr_.open(&base_, &update_, error)) return false;
        // RomFS: IVFC level 6 of the patched section
        const uint8_t* ivfc = update_.sections[1].fsh + 0x8;
        if (memcmp(ivfc, "IVFC", 4)) { *error = "the update has no RomFS"; return false; }
        memcpy(&data_off_, ivfc + 0x10 + 5 * 0x18, 8);
        return index_romfs(error);
    }

    bool file_size(const std::string& path, uint64_t* size) const override {
        auto it = files_.find(lower(path));
        if (it == files_.end()) return false;
        *size = it->second.size;
        return true;
    }
    bool is_dir(const std::string& path) const override {
        std::string p = lower(path);
        while (!p.empty() && p.back() == '/') p.pop_back();
        return p.empty() || dirs_.count(p);
    }
    std::vector<std::string> list(const std::string& dir) const override {
        std::string prefix = lower(dir);
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        if (!prefix.empty()) prefix += '/';
        std::vector<std::string> out;
        for (auto& [path, entry] : files_)
            if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path.find('/', prefix.size()) == std::string::npos)
                out.push_back(path);
        std::sort(out.begin(), out.end());
        return out;
    }
    bool read(const std::string& path, uint64_t offset, void* out, uint64_t size) const override {
        auto it = files_.find(lower(path));
        if (it == files_.end() || offset > it->second.size || size > it->second.size - offset) return false;
        return bktr_.read(data_off_ + it->second.offset + offset, out, (size_t)size);
    }
    std::vector<uint8_t> exefs(const std::string& name) const override {
        for (auto& e : exefs_)
            if (e.name == name) {
                std::vector<uint8_t> v(e.size);
                if (update_.read(0, e.offset, v.data(), v.size())) return v;
            }
        return {};
    }

private:
    bool index_romfs(std::string* error) {
        uint64_t hdr[10];
        *error = "the game's RomFS can't be read (wrong keys or a damaged dump)";
        if (!bktr_.read(data_off_, hdr, sizeof hdr) || hdr[0] != 0x50) return false;
        uint64_t dm_o = hdr[3], dm_s = hdr[4], fm_o = hdr[7], fm_s = hdr[8], data = hdr[9];
        if (dm_s > (64u << 20) || fm_s > (64u << 20)) return false;
        std::vector<uint8_t> dmeta(dm_s), fmeta(fm_s);
        if (!bktr_.read(data_off_ + dm_o, dmeta.data(), dm_s) || !bktr_.read(data_off_ + fm_o, fmeta.data(), fm_s)) return false;
        std::unordered_map<uint32_t, std::string> dir_path{{0, ""}};
        std::function<std::string(uint32_t)> dirpath = [&](uint32_t o) -> std::string {
            auto it = dir_path.find(o);
            if (it != dir_path.end()) return it->second;
            if ((size_t)o + 0x18 > dmeta.size()) return "";
            uint32_t parent, name_len;
            memcpy(&parent, &dmeta[o], 4);
            memcpy(&name_len, &dmeta[o + 0x14], 4);
            std::string name((const char*)&dmeta[o + 0x18], std::min<size_t>(name_len, dmeta.size() - o - 0x18));
            std::string p = parent == o ? name : dirpath(parent);
            p = p.empty() ? name : p + "/" + name;
            dir_path[o] = p;
            return p;
        };
        for (size_t o = 0; o + 0x20 <= fmeta.size();) {
            uint32_t parent, name_len;
            uint64_t foff, fsize;
            memcpy(&parent, &fmeta[o], 4);
            memcpy(&foff, &fmeta[o + 8], 8);
            memcpy(&fsize, &fmeta[o + 16], 8);
            memcpy(&name_len, &fmeta[o + 0x1C], 4);
            if (o + 0x20 + name_len > fmeta.size()) return false;
            std::string name((const char*)&fmeta[o + 0x20], name_len);
            std::string d = dirpath(parent);
            files_[lower(d.empty() ? name : d + "/" + name)] = {data + foff, fsize};
            for (std::string p = d; !p.empty(); p = p.substr(0, p.find_last_of('/') == std::string::npos ? 0 : p.find_last_of('/')))
                if (!dirs_.insert(lower(p)).second) break;
            o += (0x20 + name_len + 3) & ~(size_t)3;
        }
        return !files_.empty();
    }

    struct FileEntry {
        uint64_t offset, size;
    };
    Keys keys_;
    std::unique_ptr<Nsp> base_nsp_, update_nsp_;
    Nca base_, update_;
    Bktr bktr_;
    std::vector<Pfs0Entry> exefs_;
    uint64_t data_off_ = 0;
    std::unordered_map<std::string, FileEntry> files_;
    std::unordered_set<std::string> dirs_;
};

// ---- LZ4 (NSO segments) ----------------------------------------------------------------------------------
bool lz4_decompress(const uint8_t* src, size_t n, uint8_t* dst, size_t out_size) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15)
            for (uint8_t b = 255; b == 255 && i < n;) lit += (b = src[i++]);
        if (i + lit > n || o + lit > out_size) return false;
        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= n) break;                    // the last sequence has only literals
        if (i + 2 > n) return false;
        size_t off = src[i] | src[i + 1] << 8;
        i += 2;
        size_t len = (token & 15) + 4;
        if ((token & 15) == 15)
            for (uint8_t b = 255; b == 255 && i < n;) len += (b = src[i++]);
        if (!off || off > o || o + len > out_size) return false;
        for (size_t k = 0; k < len; k++, o++) dst[o] = dst[o - off];
    }
    return o == out_size;
}

}  // namespace

std::unique_ptr<Game> open_game(const std::wstring& base_nsp, const std::wstring& update_nsp, const std::wstring& keys,
                                std::string* error) {
    auto g = std::make_unique<GameImpl>();
    if (!g->open(base_nsp, update_nsp, keys, error)) return nullptr;
    return g;
}

NspKind identify_nsp(const std::wstring& path, const std::wstring& keys_path) {
    Nsp nsp(path);
    if (!nsp.ok()) return NspKind::Unreadable;
    Keys keys = load_keys(keys_path);
    Nca nca;
    std::string err;
    if (!program_nca(keys, nsp, &nca, &err)) return NspKind::Unknown;
    if (nca.title_id != 0x010003F003A34000ull) return NspKind::OtherGame;
    return is_patch(nca) ? NspKind::Update : NspKind::Base;
}

bool keys_work(const std::wstring& keys_path, const std::wstring& nsp_path) {
    Keys keys = load_keys(keys_path);
    Nsp nsp(nsp_path);
    if (!nsp.ok() || !key(keys, "header_key", 32)) return false;
    for (auto& e : nsp.entries) {
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".nca")) continue;
        uint8_t hdr[0xC00];
        return decrypt_header(keys, nsp, e, hdr);
    }
    return false;
}

bool load_nso(const std::vector<uint8_t>& file, Nso* out) {
    if (file.size() < 0x100 || memcmp(file.data(), "NSO0", 4)) return false;
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &file[o], 4); return v; };
    uint32_t flags = u32(0xC);
    uint32_t foff[3], moff[3], dsize[3], csize[3];
    for (int i = 0; i < 3; i++) {
        foff[i] = u32(0x10 + 0x10 * i);
        moff[i] = u32(0x14 + 0x10 * i);
        dsize[i] = u32(0x18 + 0x10 * i);
        csize[i] = u32(0x60 + 4 * i);
    }
    out->bss_size = u32(0x3C);
    out->dynsym_size = u32(0x9C);
    memcpy(out->build_id, &file[0x40], 32);
    size_t end = 0;
    for (int i = 0; i < 3; i++) end = std::max<size_t>(end, (size_t)moff[i] + dsize[i]);
    out->image.assign(end, 0);
    for (int i = 0; i < 3; i++) {
        size_t n = flags & (1u << i) ? csize[i] : dsize[i];
        if ((size_t)foff[i] + n > file.size()) return false;
        if (flags & (1u << i)) {
            if (!lz4_decompress(&file[foff[i]], n, &out->image[moff[i]], dsize[i])) return false;
        } else {
            memcpy(&out->image[moff[i]], &file[foff[i]], dsize[i]);
        }
    }
    out->text_off = moff[0];
    out->text_size = dsize[0];
    out->ro_off = moff[1];
    out->ro_size = dsize[1];
    out->data_off = moff[2];
    out->data_size = dsize[2];
    return true;
}

}  // namespace rt::nx
