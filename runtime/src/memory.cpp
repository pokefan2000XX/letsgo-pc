// Guest address space: fixed host reservations that mirror a Horizon process
// (heap, alias, stack and TLS regions) plus a mapping list for svcQueryMemory.
#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

#include "rt.h"

namespace rt {

static std::mutex g_mem;
struct Region { uint64_t size; uint32_t type, perm; };
static std::map<uint64_t, Region> g_map;             // non-overlapping tracked ranges
static uint64_t g_heap_size;
static uint64_t g_tls_next = kTlsBase;
static uint64_t g_used;

// `watch`: track CPU writes (GetWriteWatch), so the GPU layer can refresh textures the game
// writes through a CPU mapping without telling NVN
static void reserve(uint64_t base, uint64_t size, const char* what, bool watch = false) {
    void* p = VirtualAlloc((void*)base, size, MEM_RESERVE | (watch ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS);
    if ((uint64_t)p == base) return;
    // name whatever already occupies the range (the exe opts out of high-entropy ASLR,
    // which keeps the loader's own allocations out of these fixed regions)
    for (uint64_t a = base; a < base + size;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void*)a, &mi, sizeof mi)) break;
        if (mi.State != MEM_FREE)
            log("  0x%llx +0x%llx is in use (state 0x%lx, type 0x%lx)", (unsigned long long)(uint64_t)mi.BaseAddress,
                (unsigned long long)mi.RegionSize, mi.State, mi.Type);
        a = (uint64_t)mi.BaseAddress + mi.RegionSize;
    }
    fatal("could not reserve the %s region at 0x%llx (+0x%llx)", what, (unsigned long long)base, (unsigned long long)size);
}

void mem_init() {
    reserve(kHeapBase, kHeapRegionSize, "heap", true);
    reserve(kAliasBase, kAliasSize, "alias", true);
    reserve(kStackBase, kStackSize, "stack");
    reserve(kTlsBase, kTlsSize, "TLS");
}

bool mem_written(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    bool in_heap = a >= kHeapBase && e <= kHeapBase + kHeapRegionSize, in_alias = a >= kAliasBase && e <= kAliasBase + kAliasSize;
    if (!in_heap && !in_alias) return true;              // not watched: assume written
    static thread_local std::vector<void*> pages;
    pages.resize((size_t)((e - a) >> 12));
    ULONG_PTR n = pages.size();
    ULONG gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void*)a, (SIZE_T)(e - a), pages.data(), &n, &gran) != 0) return true;
    return n != 0;
}

bool mem_commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return VirtualAlloc((void*)a, e - a, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

static void untrack_locked(uint64_t addr, uint64_t size) {
    uint64_t end = addr + size;
    auto it = g_map.upper_bound(addr);
    if (it != g_map.begin()) --it;
    while (it != g_map.end() && it->first < end) {
        uint64_t s = it->first, e = s + it->second.size;
        Region r = it->second;
        if (e <= addr) { ++it; continue; }
        it = g_map.erase(it);
        if (s < addr) g_map[s] = {addr - s, r.type, r.perm};
        if (e > end) g_map[end] = {e - end, r.type, r.perm};
    }
}

void mem_track(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    std::lock_guard<std::mutex> lk(g_mem);
    untrack_locked(addr, size);
    g_map[addr] = {size, type, perm};
}

void mem_untrack(uint64_t addr, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_mem);
    untrack_locked(addr, size);
}

MemInfo mem_query(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_mem);
    MemInfo mi{};
    auto it = g_map.upper_bound(addr);
    uint64_t free_end = it == g_map.end() ? (1ull << 39) : it->first;
    if (it != g_map.begin()) {
        auto p = std::prev(it);
        if (addr < p->first + p->second.size) {
            mi.addr = p->first;
            mi.size = p->second.size;
            mi.type = p->second.type;
            mi.perm = p->second.perm;
            return mi;
        }
        mi.addr = p->first + p->second.size;
    }
    mi.size = free_end - mi.addr;
    mi.type = kMemFree;
    return mi;
}

uint64_t mem_heap_set_size(uint64_t size) {
    std::lock_guard<std::mutex> lk(g_mem);
    if (size > kHeapRegionSize) return 0;
    if (size > g_heap_size) {
        if (!VirtualAlloc((void*)(kHeapBase + g_heap_size), size - g_heap_size, MEM_COMMIT, PAGE_READWRITE)) return 0;
    } else if (size < g_heap_size) {
        VirtualFree((void*)(kHeapBase + size), g_heap_size - size, MEM_DECOMMIT);
    }
    untrack_locked(kHeapBase, kHeapRegionSize);
    if (size) g_map[kHeapBase] = {size, kMemHeap, 3};
    g_heap_size = size;
    return kHeapBase;
}

uint64_t mem_heap_size() { return g_heap_size; }
// how many bytes from addr on can be read (0 if none): for debug scans of the heap, which has holes
uint64_t mem_readable(uint64_t addr) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void*)addr, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint64_t)mi.BaseAddress + mi.RegionSize - addr;
}
uint64_t mem_used() { return g_used + g_heap_size; }

// svcMapMemory: the source becomes inaccessible while the alias exists, so a
// move (copy to dst, copy back on unmap) is indistinguishable from aliasing.
uint32_t mem_map_alias(uint64_t dst, uint64_t src, uint64_t size) {
    if (!mem_commit(dst, size)) return 0xD401;
    memcpy((void*)dst, (const void*)src, size);
    DWORD old;
    VirtualProtect((void*)src, size, PAGE_NOACCESS, &old);
    MemInfo mi = mem_query(dst);
    (void)mi;
    mem_track(dst, size, dst >= kStackBase && dst < kStackBase + kStackSize ? kMemStack : kMemAlias, 3);
    return 0;
}

uint32_t mem_unmap_alias(uint64_t dst, uint64_t src, uint64_t size) {
    DWORD old;
    VirtualProtect((void*)src, size, PAGE_READWRITE, &old);
    memcpy((void*)src, (const void*)dst, size);
    VirtualFree((void*)dst, size, MEM_DECOMMIT);
    mem_untrack(dst, size);
    return 0;
}

uint64_t mem_alloc_tlr() {
    std::lock_guard<std::mutex> lk(g_mem);
    uint64_t a = g_tls_next;
    g_tls_next += 0x200;
    if ((a & 0xFFF) == 0) {
        VirtualAlloc((void*)a, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        untrack_locked(a, 0x1000);
        g_map[a] = {0x1000, kMemTls, 3};
    }
    memset((void*)a, 0, 0x200);
    return a;
}

}  // namespace rt
