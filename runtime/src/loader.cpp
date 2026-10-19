// Module loader (replaces rtld): maps main and sdk at their fixed guest bases,
// resolves symbols with ELF interposition order (main first), routes
// overridden symbols to host functions, and serves rc_lookup().
// The decomp patch module (decompiled C++, recompiled with the rest) is part of the exe: its
// image is copied to its guest address at startup, and its functions replace the originals
// they were written for unless LETSGO_NO_DECOMP turns them off.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "nx.h"
#include "rt.h"

extern "C" {
struct RcEntry { uint64_t addr; rc_fn fn; };
extern const RcEntry rc_main_funcs[];
extern const uint32_t rc_main_nfuncs;
extern const RcEntry rc_sdk_funcs[];
extern const uint32_t rc_sdk_nfuncs;
extern const RcEntry rc_main_bodies[];                     // original bodies (o<addr>)
extern const RcEntry rc_sdk_bodies[];
extern const RcEntry rc_patch_funcs[];                     // decomp patch module
extern const uint32_t rc_patch_nfuncs;
extern const uint64_t rc_patch_base, rc_patch_size;
extern const uint8_t rc_patch_image[];
struct RcReplace { uint64_t orig, repl; };
extern const RcReplace rc_patch_replaces[];
extern const uint32_t rc_patch_nreplaces;
}

namespace rt {

std::vector<Module> g_modules;

struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
              uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Shdr { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; };
struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
struct Rela { uint64_t offset, info; int64_t addend; };
struct Dyn { int64_t tag; uint64_t val; };

struct Loaded {
    std::vector<uint8_t> elf;
    const Rela* rela[2] = {nullptr, nullptr};
    size_t nrela[2] = {0, 0};
    const Sym* syms = nullptr;
    size_t nsyms = 0;
    const char* strtab = nullptr;
};
static std::vector<Loaded> g_loaded;
nx::Game* g_game = nullptr;

// ---- host-function trampolines -------------------------------------------------------------
static std::vector<rc_fn> g_tramp_fns;
static std::vector<std::string> g_tramp_names;
static std::mutex g_tramp_lock;

uint64_t host_fn(const char* name, rc_fn fn) {
    std::lock_guard<std::mutex> lk(g_tramp_lock);
    g_tramp_fns.push_back(fn);
    g_tramp_names.push_back(name);
    return kTrampBase + 16 * (g_tramp_fns.size() - 1);
}

// imports nobody provides: log once, return 0
static thread_local std::string t_missing;
static std::vector<std::string> g_missing_names;
static void missing_import(Ctx* c) {
    // PLT stubs branch through x17, so it still holds this trampoline's address
    std::string what = describe(c->x[17]);
    // the movie (subsdk1) and glslc (subsdk0) libraries are replaced in later milestones; their
    // objects can't be faked by returning 0, so stop with a clear message instead of crashing later
    if (what.find("<host:_ZN5movie") != std::string::npos) {
        log("movie library call %s", what.c_str());
        log_backtrace(c);
        fatal("The game started a movie (the title screen's background video). Movie playback "
              "arrives in milestone M4, so the run ends here.");
    }
    if (what.find("<host:glslc") != std::string::npos) {
        log("shader compiler call %s", what.c_str());
        log_backtrace(c);
        fatal("The game compiled a shader at run time. The shader compiler arrives in milestone M3, "
              "so the run ends here.");
    }
    log("call to unresolved import %s (returning 0); caller %s", what.c_str(), describe(c->x[30]).c_str());
    c->x[0] = 0;
}

static void load_one(const std::string& path, const std::string& name, uint64_t base) {
    Loaded L;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) fatal("cannot open %s", path.c_str());
    fseek(f, 0, SEEK_END);
    L.elf.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(L.elf.data(), 1, L.elf.size(), f) != L.elf.size()) fatal("short read on %s", path.c_str());
    fclose(f);
    auto* eh = (const Ehdr*)L.elf.data();
    auto* ph = (const Phdr*)(L.elf.data() + eh->phoff);
    auto* sh = (const Shdr*)(L.elf.data() + eh->shoff);
    Module M;
    M.name = name;
    M.base = base;
    uint64_t end = 0;
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == 1) end = std::max(end, ph[i].vaddr + ph[i].memsz);
    end = (end + 0xFFFF) & ~0xFFFFull;
    void* p = VirtualAlloc((void*)base, end, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if ((uint64_t)p != base) fatal("cannot map %s at 0x%llx", name.c_str(), (unsigned long long)base);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != 1) continue;
        memcpy((void*)(base + ph[i].vaddr), L.elf.data() + ph[i].offset, ph[i].filesz);
        uint32_t perm = (ph[i].flags & 4 ? 1 : 0) | (ph[i].flags & 2 ? 2 : 0) | (ph[i].flags & 1 ? 4 : 0);
        mem_track(base + ph[i].vaddr, (ph[i].memsz + 0xFFF) & ~0xFFFull, ph[i].flags & 2 ? kMemCodeData : kMemCode, perm);
        if (ph[i].flags & 1) M.text_end = base + ph[i].vaddr + ph[i].memsz;
    }
    M.end = base + end;
    for (int i = 0; i < eh->shnum; i++) {
        if (sh[i].type == 11) {
            L.syms = (const Sym*)(L.elf.data() + sh[i].offset);
            L.nsyms = sh[i].size / sizeof(Sym);
            L.strtab = (const char*)(L.elf.data() + sh[sh[i].link].offset);
        } else if (sh[i].type == 4) {
            int k = L.rela[0] ? 1 : 0;
            L.rela[k] = (const Rela*)(L.elf.data() + sh[i].offset);
            L.nrela[k] = sh[i].size / sizeof(Rela);
        } else if (sh[i].type == 6) {
            for (auto* d = (const Dyn*)(L.elf.data() + sh[i].offset); d->tag; d++) {
                if (d->tag == 12) M.init = base + d->val;
                if (d->tag == 13) M.fini = base + d->val;
            }
        }
    }
    for (size_t i = 0; i < L.nsyms; i++)
        M.syms.push_back({L.strtab + L.syms[i].name, L.syms[i].shndx ? base + L.syms[i].value : 0, L.syms[i].shndx});
    log("loaded %s at 0x%llx-0x%llx (%zu symbols, %zu+%zu relocations)", name.c_str(), (unsigned long long)base,
        (unsigned long long)M.end, L.nsyms, L.nrela[0], L.nrela[1]);
    g_modules.push_back(std::move(M));
    g_loaded.push_back(std::move(L));
}

// a module straight from its NSO (the game read from the player's NSPs): segments at their offsets,
// symbols and relocations found through MOD0 and the dynamic section, as the Switch's loader does
static void load_one_nso(const std::vector<uint8_t>& file, const std::string& name, uint64_t base,
                         const char* build_id) {
    nx::Nso nso;
    if (!nx::load_nso(file, &nso)) fatal("the update's %s can't be read", name.c_str());
    char id[33];
    for (int i = 0; i < 16; i++) snprintf(id + 2 * i, 3, "%02x", nso.build_id[i]);
    if (strcmp(id, build_id))
        fatal("This isn't the 1.0.2 update of PokÃ©mon: Let's Go, Pikachu! (%s build %s, expected %s).",
              name.c_str(), id, build_id);
    uint32_t mod0;
    memcpy(&mod0, &nso.image[4], 4);
    if (mod0 + 0x1C > nso.image.size() || memcmp(&nso.image[mod0], "MOD0", 4)) fatal("%s has no MOD0 header", name.c_str());
    int32_t rel[3];
    memcpy(rel, &nso.image[mod0 + 4], sizeof rel);       // dynamic, bss start, bss end (from MOD0)
    uint64_t end = std::max<uint64_t>((uint64_t)nso.data_off + nso.data_size + nso.bss_size, (uint64_t)((int64_t)mod0 + rel[2]));
    end = (end + 0xFFFF) & ~0xFFFFull;
    void* p = VirtualAlloc((void*)base, end, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if ((uint64_t)p != base) fatal("cannot map %s at 0x%llx", name.c_str(), (unsigned long long)base);
    memcpy(p, nso.image.data(), nso.image.size());
    mem_track(base + nso.text_off, (nso.text_size + 0xFFF) & ~0xFFFull, kMemCode, 5);
    mem_track(base + nso.ro_off, (nso.ro_size + 0xFFF) & ~0xFFFull, kMemCode, 1);
    mem_track(base + nso.data_off, (end - nso.data_off + 0xFFF) & ~0xFFFull, kMemCodeData, 3);
    Module M;
    M.name = name;
    M.base = base;
    M.end = base + end;
    M.text_end = base + nso.text_off + nso.text_size;
    Loaded L;
    uint64_t symtab = 0, strtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    for (auto* d = (const Dyn*)(base + mod0 + rel[0]); d->tag; d++) {
        switch (d->tag) {
        case 5: strtab = d->val; break;
        case 6: symtab = d->val; break;
        case 7: rela = d->val; break;
        case 8: relasz = d->val; break;
        case 23: jmprel = d->val; break;
        case 2: pltrelsz = d->val; break;
        case 12: M.init = base + d->val; break;
        case 13: M.fini = base + d->val; break;
        }
    }
    L.syms = (const Sym*)(base + symtab);
    L.nsyms = nso.dynsym_size / sizeof(Sym);
    L.strtab = (const char*)(base + strtab);
    if (rela) { L.rela[0] = (const Rela*)(base + rela); L.nrela[0] = relasz / sizeof(Rela); }
    if (jmprel) { L.rela[1] = (const Rela*)(base + jmprel); L.nrela[1] = pltrelsz / sizeof(Rela); }
    for (size_t i = 0; i < L.nsyms; i++)
        M.syms.push_back({L.strtab + L.syms[i].name, L.syms[i].shndx ? base + L.syms[i].value : 0, L.syms[i].shndx});
    log("loaded %s at 0x%llx-0x%llx from the update NSP (%zu symbols, %zu+%zu relocations)", name.c_str(),
        (unsigned long long)base, (unsigned long long)M.end, L.nsyms, L.nrela[0], L.nrela[1]);
    g_modules.push_back(std::move(M));
    g_loaded.push_back(std::move(L));
}

static std::unordered_map<std::string, uint64_t> g_defs;   // first definition wins (main, then sdk)

uint64_t find_symbol(const std::string& name) {
    auto it = g_defs.find(name);
    return it == g_defs.end() ? 0 : it->second;
}

static uint64_t resolve(const std::string& name, bool weak) {
    if (rc_fn f = lookup_override(name)) {
        static std::unordered_map<std::string, uint64_t> made;
        auto it = made.find(name);
        if (it != made.end()) return it->second;
        uint64_t a = host_fn(name.c_str(), f);
        made[name] = a;
        return a;
    }
    if (uint64_t a = find_symbol(name)) return a;
    if (weak) return 0;
    static std::unordered_map<std::string, uint64_t> stubs;
    auto it = stubs.find(name);
    if (it != stubs.end()) return it->second;
    g_missing_names.push_back(name);
    uint64_t a = host_fn(name.c_str(), missing_import);
    stubs[name] = a;
    return a;
}

void load_modules(const std::string& orig) {
    // trampoline region: readable, filled with BRK so stray reads see something sane
    void* tp = VirtualAlloc((void*)kTrampBase, kTrampSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if ((uint64_t)tp != kTrampBase) fatal("cannot map trampoline region");
    for (uint64_t a = 0; a < kTrampSize; a += 4) *(uint32_t*)(kTrampBase + a) = 0xD4200000u;
    if (g_game) {
        load_one_nso(g_game->exefs("main"), "main", kMainBase, "c208db6a4ef4361f28b8402be0f7cd9a");
        load_one_nso(g_game->exefs("sdk"), "sdk", kSdkBase, "1101700d4118f1fd1458391c671f81d5");
    } else {
        load_one(orig + "/elf/main.elf", "main", kMainBase);
        load_one(orig + "/elf/sdk.elf", "sdk", kSdkBase);
    }
    register_overrides();
    for (auto& M : g_modules)
        for (auto& s : M.syms)
            if (s.shndx && !s.name.empty() && !g_defs.count(s.name)) g_defs[s.name] = s.addr;
    {   // every override must name a real symbol (catches mangling typos)
        std::unordered_map<std::string, int> known;
        for (auto& M : g_modules) for (auto& s : M.syms) known[s.name] = 1;
        for (auto& n : override_names())
            if (!known.count(n)) log("warning: override %s matches no symbol in main or sdk", n.c_str());
    }
    size_t n_rel = 0, n_over = 0;
    for (size_t m = 0; m < g_modules.size(); m++) {
        Module& M = g_modules[m];
        Loaded& L = g_loaded[m];
        for (int k = 0; k < 2; k++) {
            for (size_t i = 0; i < L.nrela[k]; i++) {
                const Rela& r = L.rela[k][i];
                uint32_t type = (uint32_t)r.info, si = (uint32_t)(r.info >> 32);
                uint64_t* slot = (uint64_t*)(M.base + r.offset);
                n_rel++;
                if (type == 1027) { *slot = M.base + r.addend; continue; }
                if (type != 257 && type != 1025 && type != 1026) fatal("%s: relocation type %u", M.name.c_str(), type);
                const Sym& s = L.syms[si];
                std::string nm = L.strtab + s.name;
                bool weak = (s.info >> 4) == 2;
                uint64_t v = resolve(nm, weak);
                if (lookup_override(nm)) n_over++;
                *slot = v + (type == 257 ? (uint64_t)r.addend : 0);
            }
        }
    }
    log("relocated %zu entries (%zu routed to host overrides, %zu unresolved imports stubbed)", n_rel, n_over,
        g_missing_names.size());
    for (auto& n : g_missing_names) RT_TRACE("  unresolved: %s", n.c_str());
    if (rc_patch_size) {
        // a static image linked at its address: no relocations, .bss is part of the image
        uint64_t size = (rc_patch_size + 0xFFFF) & ~0xFFFFull;
        void* p = VirtualAlloc((void*)rc_patch_base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if ((uint64_t)p != rc_patch_base) fatal("cannot map the decomp module at 0x%llx", (unsigned long long)rc_patch_base);
        memcpy(p, rc_patch_image, rc_patch_size);
        mem_track(rc_patch_base, size, kMemCodeData, 7);
        Module M;
        M.name = "decomp";
        M.base = rc_patch_base;
        M.end = M.text_end = rc_patch_base + size;
        g_modules.push_back(std::move(M));
        log("loaded decomp at 0x%llx-0x%llx (%u functions)", (unsigned long long)rc_patch_base,
            (unsigned long long)(rc_patch_base + size), rc_patch_nfuncs);
    }
}

// ---- address -> description --------------------------------------------------------------
static std::vector<std::pair<uint64_t, const std::string*>> g_symidx;
std::string describe(uint64_t a) {
    char buf[512];
    if (a >= kTrampBase && a < kTrampBase + kTrampSize) {
        size_t i = (size_t)((a - kTrampBase) / 16);
        snprintf(buf, sizeof buf, "0x%llx <host:%s>", (unsigned long long)a,
                 i < g_tramp_names.size() ? g_tramp_names[i].c_str() : "?");
        return buf;
    }
    if (g_symidx.empty()) {
        for (auto& M : g_modules)
            for (auto& s : M.syms)
                if (s.shndx && !s.name.empty()) g_symidx.push_back({s.addr, &s.name});
        std::sort(g_symidx.begin(), g_symidx.end());
    }
    const Module* mod = nullptr;
    for (auto& M : g_modules)
        if (a >= M.base && a < M.end) mod = &M;
    if (!mod) {
        snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)a);
        return buf;
    }
    auto it = std::upper_bound(g_symidx.begin(), g_symidx.end(), std::make_pair(a, (const std::string*)nullptr),
                               [](auto& x, auto& y) { return x.first < y.first; });
    if (it != g_symidx.begin()) {
        --it;
        if (it->first >= mod->base && a - it->first < 0x10000) {
            snprintf(buf, sizeof buf, "0x%llx %s+0x%llx <%s+0x%llx>", (unsigned long long)a, mod->name.c_str(),
                     (unsigned long long)(a - mod->base), it->second->c_str(), (unsigned long long)(a - it->first));
            return buf;
        }
    }
    snprintf(buf, sizeof buf, "0x%llx %s+0x%llx", (unsigned long long)a, mod->name.c_str(),
             (unsigned long long)(a - mod->base));
    return buf;
}

void log_backtrace(Ctx* c) {
    if (!c) return;
    log("  backtrace (lr first):");
    log("    #0 %s", describe(c->x[30]).c_str());
    uint64_t fp = c->x[29];
    for (int i = 1; i < 40 && fp; i++) {
        MemInfo mi = mem_query(fp);
        if (mi.type != kMemStack || (fp & 7)) break;
        uint64_t next = *(uint64_t*)fp, lr = *(uint64_t*)(fp + 8);
        if (!lr) break;
        log("    #%d %s", i, describe(lr).c_str());
        if (next <= fp) break;
        fp = next;
    }
}

// ---- dispatch --------------------------------------------------------------------------------
static rc_fn* g_main_tab;
static rc_fn* g_sdk_tab;
static rc_fn* g_patch_tab;
static uint64_t g_main_len, g_sdk_len, g_patch_len;
static thread_local uint64_t t_bad_target;

static void bad_target(Ctx* c) {
    log_backtrace(c);
    fatal("indirect branch to 0x%llx (%s), which is not a recompiled function entry; caller %s",
          (unsigned long long)t_bad_target, describe(t_bad_target).c_str(), describe(c->x[30]).c_str());
}

// host code address -> guest function containing it (nearest preceding recompiled function)
static std::vector<std::pair<uint64_t, uint64_t>> g_hostmap;
uint64_t guest_of_host(uint64_t rip) {
    if (g_hostmap.empty()) {
        for (uint32_t i = 0; i < rc_main_nfuncs; i++) g_hostmap.push_back({(uint64_t)rc_main_bodies[i].fn, rc_main_bodies[i].addr});
        for (uint32_t i = 0; i < rc_sdk_nfuncs; i++) g_hostmap.push_back({(uint64_t)rc_sdk_bodies[i].fn, rc_sdk_bodies[i].addr});
        for (uint32_t i = 0; i < rc_patch_nfuncs; i++) g_hostmap.push_back({(uint64_t)rc_patch_funcs[i].fn, rc_patch_funcs[i].addr});
        std::sort(g_hostmap.begin(), g_hostmap.end());
    }
    // the unwind table gives the start of the host function containing rip; it is guest
    // code only if that start is one of the recompiled functions
    DWORD64 base = 0;
    if (PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(rip, &base, nullptr)) {
        uint64_t start = base + rf->BeginAddress;
        auto it = std::lower_bound(g_hostmap.begin(), g_hostmap.end(), std::make_pair(start, 0ull));
        return it != g_hostmap.end() && it->first == start ? it->second : 0;
    }
    auto it = std::upper_bound(g_hostmap.begin(), g_hostmap.end(), std::make_pair(rip, ~0ull));
    if (it == g_hostmap.begin()) return 0;
    return std::prev(it)->second;                         // leaf function without unwind data: nearest start
}

void dispatch_init() {
    g_main_len = (g_modules[0].text_end - kMainBase) / 4;
    g_sdk_len = (g_modules[1].text_end - kSdkBase) / 4;
    g_main_tab = (rc_fn*)calloc(g_main_len, sizeof(rc_fn));
    g_sdk_tab = (rc_fn*)calloc(g_sdk_len, sizeof(rc_fn));
    for (uint32_t i = 0; i < rc_main_nfuncs; i++) g_main_tab[(rc_main_funcs[i].addr - kMainBase) / 4] = rc_main_funcs[i].fn;
    for (uint32_t i = 0; i < rc_sdk_nfuncs; i++) g_sdk_tab[(rc_sdk_funcs[i].addr - kSdkBase) / 4] = rc_sdk_funcs[i].fn;
    if (rc_patch_size) {
        g_patch_len = rc_patch_size / 4;
        g_patch_tab = (rc_fn*)calloc(g_patch_len, sizeof(rc_fn));
        for (uint32_t i = 0; i < rc_patch_nfuncs; i++) g_patch_tab[(rc_patch_funcs[i].addr - rc_patch_base) / 4] = rc_patch_funcs[i].fn;
    }
    if (rc_patch_nreplaces)
        log("decomp: %u original functions have decompiled replacements%s", rc_patch_nreplaces,
            getenv("LETSGO_NO_DECOMP") ? " (LETSGO_NO_DECOMP: some or all run the originals)" : "");
    // overrides also replace the function itself for indirect calls (vtables, callbacks),
    // which never go through a named relocation
    int n_hooked = 0;
    for (auto& name : override_names()) {
        uint64_t a = find_symbol(name);
        rc_fn f = lookup_override(name);
        if (a >= kMainBase && a < g_modules[0].text_end) { g_main_tab[(a - kMainBase) / 4] = f; n_hooked++; }
        else if (a >= kSdkBase && a < g_modules[1].text_end) { g_sdk_tab[(a - kSdkBase) / 4] = f; n_hooked++; }
    }
    log("dispatch: %u main + %u sdk functions, %d replaced by overrides", rc_main_nfuncs, rc_sdk_nfuncs, n_hooked);
}

}  // namespace rt

extern "C" rc_fn rc_lookup(uint64_t t) {
    using namespace rt;
    uint64_t o = (t - kMainBase) >> 2;
    if (o < g_main_len && !(t & 3) && g_main_tab[o]) return g_main_tab[o];
    o = (t - kSdkBase) >> 2;
    if (o < g_sdk_len && !(t & 3) && g_sdk_tab[o]) return g_sdk_tab[o];
    o = (t - rc_patch_base) >> 2;
    if (o < g_patch_len && !(t & 3) && g_patch_tab[o]) return g_patch_tab[o];
    o = t - kTrampBase;
    if (o < kTrampSize && !(o & 15) && o / 16 < g_tramp_fns.size()) return g_tramp_fns[o / 16];
    // a hook calling the original it replaces: the original's recompiled body (o<address>), which
    // runs even when the decomp module replaces that function
    o = t - kOriginalsBase;
    if (o < kOriginalsSize && !(t & 3)) {
        const RcEntry* end = rc_main_bodies + rc_main_nfuncs;
        const RcEntry* it = std::lower_bound(rc_main_bodies, end, kMainBase + o,
                                             [](const RcEntry& e, uint64_t a) { return e.addr < a; });
        if (it != end && it->addr == kMainBase + o) return it->fn;
    }
    t_bad_target = t;
    return bad_target;
}

// Called by each replaced function's stub (recomp.py) to pick the decompiled version or the
// original. LETSGO_NO_DECOMP=all (or 1) runs every original, LETSGO_NO_DECOMP=7100101b30,...
// only the listed ones: bisecting which replacement breaks something. LETSGO_DECOMP_TRACE=1
// logs the first call of each replaced function.
extern "C" int rc_decomp_off(uint64_t addr, int* seen) {
    static const struct Off {
        bool all = false;
        std::vector<uint64_t> list;
        bool trace = getenv("LETSGO_DECOMP_TRACE") != nullptr;
        Off() {
            const char* e = getenv("LETSGO_NO_DECOMP");
            if (!e) return;
            if (!*e || !strcmp(e, "1") || !strcmp(e, "all")) {
                all = true;
                return;
            }
            for (const char* q = e; *q;) {
                char* end;
                uint64_t a = strtoull(q, &end, 16);
                if (end == q) break;
                list.push_back(a);
                q = *end ? end + 1 : end;
            }
        }
    } off;
    bool o = off.all || std::find(off.list.begin(), off.list.end(), addr) != off.list.end();
    if (!*seen) {
        *seen = 1;
        if (off.trace) rt::log("decomp: first call of 0x%llx runs the %s", (unsigned long long)addr, o ? "original" : "decompiled version");
    }
    return o;
}
