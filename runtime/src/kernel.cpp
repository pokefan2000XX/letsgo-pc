// Horizon kernel subset: handles, threads, mutex/condvar arbitration, address
// arbiter, events, waits, memory and info SVCs. Semantics follow the
// documented kernel behaviour (switchbrew, Atmosphere's mesosphere).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rt.h"

namespace rt {

enum : uint32_t {
    RC_OK = 0, RC_INVALID_SIZE = 0xCA01, RC_INVALID_ADDRESS = 0xCC01, RC_OUT_OF_MEMORY = 0xD201,
    RC_INVALID_HANDLE = 0xE401, RC_TIMEOUT = 0xEA01, RC_CANCELLED = 0xEC01, RC_OUT_OF_RANGE = 0xEE01,
    RC_INVALID_ENUM = 0xF001, RC_NOT_FOUND = 0xF201, RC_INVALID_STATE = 0xFA01, RC_NOT_SUPPORTED = 0xFE01,
    RC_SESSION_CLOSED = 0xF601,
};
constexpr uint32_t kWaiters = 0x40000000u;
constexpr uint32_t kCurThread = 0xFFFF8000u;            // pseudo-handle (kCurProcess is 0xFFFF8001)

struct KObject {
    enum Kind { Thread, Event, Transfer, Session } kind;
    explicit KObject(Kind k) : kind(k) {}
    virtual ~KObject() {}
};

struct KThread : KObject {
    KThread() : KObject(Thread) {}
    uint32_t handle = 0;
    uint64_t id = 0, entry = 0, arg = 0, stack_top = 0, tlr = 0;
    int prio = 44, core = 0;
    Ctx* ctx = nullptr;
    HANDLE host = nullptr;
    bool started = false, exited = false, cancel = false;
    std::condition_variable cv;
    bool woken = false;
    uint32_t result = 0;
    uint64_t mutex_addr = 0, cv_key = 0, arb_addr = 0;
    uint32_t tag = 0;
    uint64_t seq = 0;                    // FIFO order among equal priorities
    std::atomic<uint32_t> svc{0};        // SVC in progress (thread dumps)
    uint64_t svc_args[4] = {};
};

struct KEvent : KObject {
    KEvent() : KObject(Event) {}
    bool signaled = false;
};

static std::mutex g_k;
static std::condition_variable g_sync;   // any event/thread state change
static std::unordered_map<uint32_t, KObject*> g_handles;
static uint32_t g_next_handle = 0x101;
static uint64_t g_next_tid = 0x52, g_seq;
static std::multimap<uint64_t, KThread*> g_mutex_waiters, g_cv_waiters, g_arb_waiters;
static thread_local KThread* t_cur;
static KThread* g_main_thread;

uint64_t ticks() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)(((unsigned __int128)t.QuadPart * 19200000u) / (uint64_t)freq.QuadPart);
}

Ctx* current_ctx() { return t_cur ? t_cur->ctx : nullptr; }
KThread* current_thread() { return t_cur; }

static uint32_t new_handle(KObject* o) {
    uint32_t h = g_next_handle++;
    g_handles[h] = o;
    return h;
}
static KObject* get(uint32_t h) {
    if (h == kCurThread) return t_cur;
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}
// kernel events created by host services: the guest gets the readable handle,
// host threads signal through the writable one
uint32_t kevent_create(uint32_t* readable) {
    std::lock_guard<std::mutex> lk(g_k);
    KEvent* e = new KEvent;
    uint32_t w = new_handle(e);
    *readable = new_handle(e);
    return w;
}
void kevent_signal(uint32_t handle) {
    std::lock_guard<std::mutex> lk(g_k);
    KObject* o = get(handle);
    if (!o || o->kind != KObject::Event) return;
    static_cast<KEvent*>(o)->signaled = true;
    g_sync.notify_all();
}
static KThread* get_thread(uint32_t h) {
    KObject* o = get(h);
    return o && o->kind == KObject::Thread ? static_cast<KThread*>(o) : nullptr;
}

static uint32_t& word(uint64_t a) { return *(uint32_t*)(uintptr_t)a; }
static uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t*)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t*)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static bool cas32(uint64_t a, uint32_t& expected, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t*)(uintptr_t)a, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// highest priority (lowest value), FIFO among equals
static std::multimap<uint64_t, KThread*>::iterator best_waiter(std::multimap<uint64_t, KThread*>& m, uint64_t key) {
    auto r = m.equal_range(key);
    auto best = m.end();
    for (auto it = r.first; it != r.second; ++it)
        if (best == m.end() || it->second->prio < best->second->prio ||
            (it->second->prio == best->second->prio && it->second->seq < best->second->seq))
            best = it;
    return best;
}
static void wake(KThread* t, uint32_t result) {
    t->woken = true;
    t->result = result;
    t->cv.notify_one();
}
static void erase_waiter(std::multimap<uint64_t, KThread*>& m, uint64_t key, KThread* t) {
    auto r = m.equal_range(key);
    for (auto it = r.first; it != r.second; ++it)
        if (it->second == t) { m.erase(it); return; }
}
// wait on t->cv until woken or the timeout (ns, <0 = forever) expires; returns false on timeout
static bool block(std::unique_lock<std::mutex>& lk, KThread* t, int64_t timeout_ns) {
    if (timeout_ns < 0) {
        t->cv.wait(lk, [t] { return t->woken; });
        return true;
    }
    return t->cv.wait_for(lk, std::chrono::nanoseconds(timeout_ns), [t] { return t->woken; });
}

// ---- mutex / condition variable arbitration ---------------------------------------------
static void release_mutex(uint64_t addr) {
    auto it = best_waiter(g_mutex_waiters, addr);
    if (it == g_mutex_waiters.end()) { st32(addr, 0); return; }
    KThread* t = it->second;
    g_mutex_waiters.erase(it);
    bool more = g_mutex_waiters.count(addr) > 0;
    st32(addr, t->tag | (more ? kWaiters : 0));
    t->mutex_addr = 0;
    wake(t, RC_OK);
}

static uint32_t arbitrate_lock(uint32_t owner_handle, uint64_t addr, uint32_t tag) {
    std::unique_lock<std::mutex> lk(g_k);
    if (ld32(addr) != (owner_handle | kWaiters)) return RC_OK;
    if (!get_thread(owner_handle)) return RC_INVALID_HANDLE;
    KThread* cur = t_cur;
    cur->woken = false;
    cur->mutex_addr = addr;
    cur->tag = tag;
    cur->seq = ++g_seq;
    g_mutex_waiters.emplace(addr, cur);
    block(lk, cur, -1);
    return cur->result;
}

static uint32_t arbitrate_unlock(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_k);
    release_mutex(addr);
    return RC_OK;
}

// a signalled condvar waiter takes the mutex, or queues for it
static void cv_take_mutex(KThread* t) {
    t->cv_key = 0;
    uint32_t v = ld32(t->mutex_addr);
    for (;;) {
        if (v == 0) {
            if (cas32(t->mutex_addr, v, t->tag)) { t->mutex_addr = 0; wake(t, RC_OK); return; }
        } else {
            if ((v & kWaiters) || cas32(t->mutex_addr, v, v | kWaiters)) {
                t->seq = ++g_seq;
                g_mutex_waiters.emplace(t->mutex_addr, t);
                return;
            }
        }
    }
}

static uint32_t wait_key(uint64_t mutex_addr, uint64_t key, uint32_t tag, int64_t timeout) {
    std::unique_lock<std::mutex> lk(g_k);
    KThread* cur = t_cur;
    release_mutex(mutex_addr);
    st32(key, 1);
    cur->woken = false;
    cur->mutex_addr = mutex_addr;
    cur->cv_key = key;
    cur->tag = tag;
    cur->seq = ++g_seq;
    g_cv_waiters.emplace(key, cur);
    if (block(lk, cur, timeout)) return cur->result;
    // timed out: leave whatever queue we are in; user mode relocks the mutex itself
    if (cur->cv_key) { erase_waiter(g_cv_waiters, key, cur); cur->cv_key = 0; }
    if (cur->mutex_addr) { erase_waiter(g_mutex_waiters, cur->mutex_addr, cur); cur->mutex_addr = 0; }
    if (!g_cv_waiters.count(key)) st32(key, 0);
    return RC_TIMEOUT;
}

static uint32_t signal_key(uint64_t key, int32_t count) {
    std::lock_guard<std::mutex> lk(g_k);
    int n = count <= 0 ? INT_MAX : count;
    while (n-- > 0) {
        auto it = best_waiter(g_cv_waiters, key);
        if (it == g_cv_waiters.end()) break;
        KThread* t = it->second;
        g_cv_waiters.erase(it);
        cv_take_mutex(t);
    }
    if (!g_cv_waiters.count(key)) st32(key, 0);
    return RC_OK;
}

// ---- address arbiter -----------------------------------------------------------------
static uint32_t wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    std::unique_lock<std::mutex> lk(g_k);
    int32_t cur_v = (int32_t)ld32(addr);
    if (type == 0) {
        if (!(cur_v < value)) return RC_INVALID_STATE;
    } else if (type == 1) {
        if (!(cur_v < value)) return RC_INVALID_STATE;
        uint32_t e = (uint32_t)cur_v;
        if (!cas32(addr, e, (uint32_t)(cur_v - 1))) return RC_INVALID_STATE;
    } else if (type == 2) {
        if (cur_v != value) return RC_INVALID_STATE;
    } else {
        return RC_INVALID_ENUM;
    }
    if (timeout == 0) return RC_TIMEOUT;
    KThread* t = t_cur;
    t->woken = false;
    t->arb_addr = addr;
    t->seq = ++g_seq;
    g_arb_waiters.emplace(addr, t);
    if (block(lk, t, timeout)) return t->result;
    erase_waiter(g_arb_waiters, addr, t);
    t->arb_addr = 0;
    return RC_TIMEOUT;
}

static void arb_wake(uint64_t addr, int32_t count) {
    int n = count <= 0 ? INT_MAX : count;
    while (n-- > 0) {
        auto it = best_waiter(g_arb_waiters, addr);
        if (it == g_arb_waiters.end()) break;
        KThread* t = it->second;
        g_arb_waiters.erase(it);
        t->arb_addr = 0;
        wake(t, RC_OK);
    }
}

static uint32_t signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    std::lock_guard<std::mutex> lk(g_k);
    if (type == 0) {
        arb_wake(addr, count);
        return RC_OK;
    }
    if (type == 1) {
        uint32_t e = (uint32_t)value;
        if (!cas32(addr, e, (uint32_t)(value + 1))) return RC_INVALID_STATE;
        arb_wake(addr, count);
        return RC_OK;
    }
    if (type == 2) {
        size_t waiters = g_arb_waiters.count(addr);
        int32_t nv;
        if (count <= 0) nv = waiters ? value - 2 : value + 1;
        else if (waiters) nv = (int64_t)waiters - 1 < count ? value - 1 : value;
        else nv = value + 1;
        if (nv != value) {
            uint32_t e = (uint32_t)value;
            if (!cas32(addr, e, (uint32_t)nv)) return RC_INVALID_STATE;
        } else if ((int32_t)ld32(addr) != value) {
            return RC_INVALID_STATE;
        }
        arb_wake(addr, count);
        return RC_OK;
    }
    return RC_INVALID_ENUM;
}

// ---- synchronization objects --------------------------------------------------------------
static bool is_signaled(KObject* o) {
    if (o->kind == KObject::Event) return static_cast<KEvent*>(o)->signaled;
    if (o->kind == KObject::Thread) return static_cast<KThread*>(o)->exited;
    return false;
}

static uint32_t wait_sync(const uint32_t* handles, int32_t n, int64_t timeout, uint64_t* index) {
    std::unique_lock<std::mutex> lk(g_k);
    KThread* cur = t_cur;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout < 0 ? 0 : timeout);
    for (;;) {
        if (cur->cancel) { cur->cancel = false; return RC_CANCELLED; }
        for (int i = 0; i < n; i++) {
            KObject* o = get(handles[i]);
            if (!o) return RC_INVALID_HANDLE;
            if (is_signaled(o)) { *index = (uint64_t)i; return RC_OK; }
        }
        if (timeout == 0) return RC_TIMEOUT;
        if (timeout < 0) g_sync.wait(lk);
        else if (g_sync.wait_until(lk, deadline) == std::cv_status::timeout) {
            for (int i = 0; i < n; i++)
                if (KObject* o = get(handles[i]); o && is_signaled(o)) { *index = (uint64_t)i; return RC_OK; }
            return RC_TIMEOUT;
        }
    }
}

// ---- threads ---------------------------------------------------------------------------
static Ctx* new_ctx() {
    Ctx* c = (Ctx*)_aligned_malloc(sizeof(Ctx), 64);
    memset(c, 0, sizeof *c);
    c->ex_addr = ~0ull;
    return c;
}

static void mark_exited(KThread* t) {
    std::lock_guard<std::mutex> lk(g_k);
    t->exited = true;
    g_sync.notify_all();
}

static DWORD WINAPI thread_main(void* p) {
    KThread* t = (KThread*)p;
    t_cur = t;
    rc_lookup(t->entry)(t->ctx);
    RT_TRACE("thread %llu returned from its entry", (unsigned long long)t->id);
    mark_exited(t);
    return 0;
}

static uint32_t create_thread(uint64_t entry, uint64_t arg, uint64_t stack_top, int prio, int core, uint32_t* out) {
    std::lock_guard<std::mutex> lk(g_k);
    KThread* t = new KThread;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->prio = prio;
    t->core = core < 0 ? 0 : core;
    t->id = g_next_tid++;
    t->tlr = mem_alloc_tlr();
    t->handle = new_handle(t);
    *out = t->handle;
    return RC_OK;
}

static uint32_t start_thread(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_k);
    KThread* t = get_thread(h);
    if (!t) return RC_INVALID_HANDLE;
    if (t->started) return RC_INVALID_STATE;
    t->started = true;
    t->ctx = new_ctx();
    t->ctx->x[0] = t->arg;
    t->ctx->sp = t->stack_top;
    t->ctx->tpidrro_el0 = t->tlr;
    t->ctx->host = t;
    DWORD tid;
    t->host = CreateThread(nullptr, 64u << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!t->host) fatal("CreateThread failed (%lu)", GetLastError());
    RT_TRACE("thread %llu started: entry %s arg 0x%llx", (unsigned long long)t->id, describe(t->entry).c_str(),
             (unsigned long long)t->arg);
    return RC_OK;
}

uint32_t create_main_thread(uint64_t stack_top) {
    uint32_t h;
    create_thread(0, 0, stack_top, 44, 0, &h);
    std::lock_guard<std::mutex> lk(g_k);
    g_main_thread = get_thread(h);
    return h;
}

void* start_guest_main(uint64_t entry, const std::vector<uint64_t>& args) {
    KThread* t;
    {
        std::lock_guard<std::mutex> lk(g_k);
        t = g_main_thread;
        t->entry = entry;
        t->started = true;
        t->ctx = new_ctx();
        for (size_t i = 0; i < args.size() && i < 8; i++) t->ctx->x[i] = args[i];
        t->ctx->sp = t->stack_top;
        t->ctx->tpidrro_el0 = t->tlr;
        t->ctx->host = t;
    }
    DWORD tid;
    t->host = CreateThread(nullptr, 64u << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!t->host) fatal("CreateThread failed (%lu)", GetLastError());
    return t->host;
}

// ---- SVC dispatch ------------------------------------------------------------------------
static uint64_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint32_t* rc) {
    *rc = RC_OK;
    switch (type) {
    case 0: return 0x7;                                       // core mask
    case 1: return 0xFFFFFFFFF0000000ull;                     // priority mask (28..59 allowed)
    case 2: return kAliasBase;
    case 3: return kAliasSize;
    case 4: return kHeapBase;
    case 5: return kHeapRegionSize;
    case 6: case 21: return kTotalMemory;                     // total (non-system) memory
    case 7: case 22: return mem_used();                       // used (non-system) memory
    case 20: {                                                // user exception context address
        static uint64_t ctx_page = [] { uint64_t a = mem_alloc_tlr(); mem_alloc_tlr(); mem_alloc_tlr(); return a; }();
        return ctx_page;
    }
    case 23: return 1;                                        // is application
    case 8: return 0;                                         // debugger attached
    case 9: return 0;                                         // resource limit handle
    case 10: return 0;                                        // idle tick count
    case 11: {                                                // random entropy
        static std::mt19937_64 rng{std::random_device{}()};
        return rng();
    }
    case 12: return kAslrBase;
    case 13: return kAslrSize;
    case 14: return kStackBase;
    case 15: return kStackSize;
    case 16: case 17: return 0;                               // system resource size
    case 18: return 0x010003F003A34000ull;                    // program id
    case 19: return 0;
    case 0xF0000002: return ticks();                          // thread tick count
    default:
        (void)handle; (void)sub;
        log("svcGetInfo: unhandled type %u (sub %llu)", type, (unsigned long long)sub);
        *rc = RC_INVALID_ENUM;
        return 0;
    }
}

static void svc_dispatch_impl(Ctx* c, uint32_t imm);

void svc_dispatch(Ctx* c, uint32_t imm) {
    KThread* t = t_cur;
    if (!t) return svc_dispatch_impl(c, imm);
    memcpy(t->svc_args, c->x, sizeof t->svc_args);
    t->svc = imm;
    svc_dispatch_impl(c, imm);
    t->svc = 0;
}

static const char* svc_name(uint32_t imm) {
    switch (imm) {
    case 0x0B: return "SleepThread";
    case 0x18: return "WaitSynchronization";
    case 0x1A: return "ArbitrateLock";
    case 0x1C: return "WaitProcessWideKeyAtomic";
    case 0x34: return "WaitForAddress";
    default: return "svc";
    }
}

// every guest thread: name, priority, and the wait it is blocked in with its guest call chain
void dump_threads() {
    std::vector<KThread*> threads;
    {
        std::lock_guard<std::mutex> lk(g_k);
        for (auto& kv : g_handles)
            if (kv.second->kind == KObject::Thread &&
                std::find(threads.begin(), threads.end(), kv.second) == threads.end())
                threads.push_back(static_cast<KThread*>(kv.second));
    }
    std::sort(threads.begin(), threads.end(), [](KThread* a, KThread* b) { return a->id < b->id; });
    log("---- thread dump (%zu threads) ----", threads.size());
    for (KThread* t : threads) {
        const char* name = "?";
        if (t->tlr) {
            uint64_t tt = *(uint64_t*)(uintptr_t)(t->tlr + 0x1F8);        // nn::os::ThreadType*
            uint64_t np = tt ? *(uint64_t*)(uintptr_t)(tt + 0x1A8) : 0;    // name pointer
            if (np && mem_query(np).type != kMemFree) name = (const char*)(uintptr_t)np;
        }
        uint32_t svc = t->svc;
        if (!t->started || t->exited || !svc) {
            log("thread %llu \"%s\" prio %d: %s", (unsigned long long)t->id, name, t->prio,
                !t->started ? "not started" : t->exited ? "exited" : "running");
            continue;
        }
        log("thread %llu \"%s\" prio %d: in %s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", (unsigned long long)t->id, name, t->prio,
            svc_name(svc), (unsigned long long)t->svc_args[0], (unsigned long long)t->svc_args[1],
            (unsigned long long)t->svc_args[2], (unsigned long long)t->svc_args[3]);
        Ctx* c = t->ctx;
        log("    #0 %s", describe(c->x[30]).c_str());
        uint64_t fp = c->x[29];
        for (int i = 1; i < 24 && fp && !(fp & 7); i++) {
            if (mem_query(fp).type == kMemFree) break;
            uint64_t next = *(uint64_t*)(uintptr_t)fp, lr = *(uint64_t*)(uintptr_t)(fp + 8);
            if (!lr) break;
            log("    #%d %s", i, describe(lr).c_str());
            if (next <= fp) break;
            fp = next;
        }
    }
    log("---- end of thread dump ----");
}

static void svc_dispatch_impl(Ctx* c, uint32_t imm) {
    uint64_t* x = c->x;
    uint32_t rc = RC_OK;
    switch (imm) {
    case 0x01: {                                              // SetHeapSize(size) -> addr
        uint64_t base = mem_heap_set_size(x[1]);
        if (!base) rc = RC_OUT_OF_MEMORY;
        x[1] = base;
        RT_TRACE("svcSetHeapSize(0x%llx) -> 0x%llx", (unsigned long long)x[1], (unsigned long long)base);
        break;
    }
    case 0x02: case 0x03: break;                              // SetMemoryPermission / SetMemoryAttribute
    case 0x04: rc = mem_map_alias(x[0], x[1], x[2]); break;   // MapMemory(dst, src, size)
    case 0x05: rc = mem_unmap_alias(x[0], x[1], x[2]); break; // UnmapMemory
    case 0x06: {                                              // QueryMemory(info*, _, addr)
        MemInfo mi = mem_query(x[2]);
        memcpy((void*)(uintptr_t)x[0], &mi, sizeof mi);
        x[1] = 0;
        break;
    }
    case 0x07: log("svcExitProcess"); ExitProcess(0);
    case 0x08: {                                              // CreateThread(entry, arg, sp, prio, core)
        uint32_t h = 0;
        rc = create_thread(x[1], x[2], x[3], (int)x[4], (int)(int32_t)x[5], &h);
        x[1] = h;
        break;
    }
    case 0x09: rc = start_thread((uint32_t)x[0]); break;
    case 0x0A: mark_exited(t_cur); ExitThread(0);             // ExitThread (guest frames own no host state)
    case 0x0B: {                                              // SleepThread(ns)
        int64_t ns = (int64_t)x[0];
        if (ns <= 0) SwitchToThread();
        else std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        break;
    }
    case 0x0C: { std::lock_guard<std::mutex> lk(g_k); KThread* t = get_thread((uint32_t)x[1]); if (t) x[1] = (uint64_t)t->prio; else rc = RC_INVALID_HANDLE; break; }
    case 0x0D: { std::lock_guard<std::mutex> lk(g_k); KThread* t = get_thread((uint32_t)x[0]); if (t) t->prio = (int)x[1]; else rc = RC_INVALID_HANDLE; break; }
    case 0x0E: { std::lock_guard<std::mutex> lk(g_k); KThread* t = get_thread((uint32_t)x[2]); if (t) { x[1] = (uint64_t)t->core; x[2] = 0x7; } else rc = RC_INVALID_HANDLE; break; }
    case 0x0F: { std::lock_guard<std::mutex> lk(g_k); KThread* t = get_thread((uint32_t)x[0]); if (t) { if ((int32_t)x[1] >= 0) t->core = (int)x[1]; } else rc = RC_INVALID_HANDLE; break; }
    case 0x10: x[0] = (uint64_t)(t_cur ? t_cur->core : 0); return;
    case 0x11: case 0x12: case 0x17: {                        // SignalEvent / ClearEvent / ResetSignal
        std::lock_guard<std::mutex> lk(g_k);
        KObject* o = get((uint32_t)x[0]);
        if (!o || o->kind != KObject::Event) { rc = RC_INVALID_HANDLE; break; }
        KEvent* e = static_cast<KEvent*>(o);
        if (imm == 0x11) { e->signaled = true; g_sync.notify_all(); }
        else if (imm == 0x17 && !e->signaled) rc = RC_INVALID_STATE;
        else e->signaled = false;
        break;
    }
    case 0x15: {                                              // CreateTransferMemory(_, addr, size, perm)
        std::lock_guard<std::mutex> lk(g_k);
        x[1] = new_handle(new KObject(KObject::Transfer));
        break;
    }
    case 0x16: {                                              // CloseHandle
        std::lock_guard<std::mutex> lk(g_k);
        auto it = g_handles.find((uint32_t)x[0]);
        if (it == g_handles.end()) rc = RC_INVALID_HANDLE;
        else g_handles.erase(it);
        break;
    }
    case 0x18: {                                              // WaitSynchronization(_, handles, n, timeout)
        uint64_t idx = 0;
        rc = wait_sync((const uint32_t*)(uintptr_t)x[1], (int32_t)x[2], (int64_t)x[3], &idx);
        x[1] = idx;
        break;
    }
    case 0x19: {                                              // CancelSynchronization(thread)
        std::lock_guard<std::mutex> lk(g_k);
        KThread* t = get_thread((uint32_t)x[0]);
        if (!t) { rc = RC_INVALID_HANDLE; break; }
        t->cancel = true;
        g_sync.notify_all();
        break;
    }
    case 0x1A: rc = arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]); break;
    case 0x1B: rc = arbitrate_unlock(x[0]); break;
    case 0x1C: rc = wait_key(x[0], x[1], (uint32_t)x[2], (int64_t)x[3]); break;
    case 0x1D: rc = signal_key(x[0], (int32_t)x[1]); break;
    case 0x1E: x[0] = ticks(); return;                        // GetSystemTick
    case 0x1F: {                                              // ConnectToNamedPort(_, name)
        log("IPC: svcConnectToNamedPort(\"%s\") from %s", (const char*)(uintptr_t)x[1], describe(c->x[30]).c_str());
        std::lock_guard<std::mutex> lk(g_k);
        x[1] = new_handle(new KObject(KObject::Session));
        break;
    }
    case 0x21: case 0x22: case 0x23:                          // SendSyncRequest*
        log_backtrace(c);
        fatal("IPC: svcSendSyncRequest (svc 0x%x) on handle 0x%x, caller %s", imm, (uint32_t)x[0],
              describe(c->x[30]).c_str());
    case 0x25: {                                              // GetThreadId(_, handle)
        std::lock_guard<std::mutex> lk(g_k);
        KThread* t = get_thread((uint32_t)x[1]);
        if (t) x[1] = t->id; else rc = RC_INVALID_HANDLE;
        break;
    }
    case 0x26: {                                              // Break(reason, addr, size)
        log("svcBreak(reason 0x%llx, 0x%llx, 0x%llx) from %s", (unsigned long long)x[0], (unsigned long long)x[1],
            (unsigned long long)x[2], describe(c->x[30]).c_str());
        if (!(x[0] & 0x80000000)) { log_backtrace(c); fatal("guest called svcBreak"); }
        break;
    }
    case 0x27: {                                              // OutputDebugString(str, size)
        std::string s((const char*)(uintptr_t)x[0], (size_t)x[1]);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        log("[guest] %s", s.c_str());
        break;
    }
    case 0x29: {                                              // GetInfo(_, type, handle, sub)
        x[1] = get_info((uint32_t)x[1], (uint32_t)x[2], x[3], &rc);
        break;
    }
    case 0x2A: case 0x2B: case 0x2C: case 0x2D:              // cache flush / (Un)MapPhysicalMemory
        if (imm == 0x2C) { if (!mem_commit(x[0], x[1])) rc = RC_OUT_OF_MEMORY; else mem_track(x[0], x[1], kMemAlias, 3); }
        if (imm == 0x2D) mem_untrack(x[0], x[1]);
        break;
    case 0x32: break;                                         // SetThreadActivity
    case 0x34: rc = wait_for_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int64_t)x[3]); break;
    case 0x35: rc = signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]); break;
    case 0x45: {                                              // CreateEvent -> (write, read)
        std::lock_guard<std::mutex> lk(g_k);
        KEvent* e = new KEvent;
        x[1] = new_handle(e);
        x[2] = new_handle(e);
        break;
    }
    default:
        fatal("unimplemented svc 0x%x (x0=0x%llx x1=0x%llx x2=0x%llx) from %s", imm, (unsigned long long)x[0],
              (unsigned long long)x[1], (unsigned long long)x[2], describe(c->x[30]).c_str());
    }
    x[0] = rc;
}

}  // namespace rt

// ---- C entry points used by recompiled code ---------------------------------------------------
extern "C" {
void rc_svc(Ctx* c, uint32_t imm) { rt::svc_dispatch(c, imm); }
uint64_t rc_cntpct(void) { return rt::ticks(); }
uint64_t rc_mrs(Ctx* c, uint64_t pc, uint32_t w) {
    (void)c;
    rt::log("mrs of unhandled system register (insn 0x%08x) at %s: returning 0", w, rt::describe(pc).c_str());
    return 0;
}
void rc_msr(Ctx* c, uint64_t pc, uint32_t w, uint64_t v) {
    (void)c;
    rt::log("msr to unhandled system register (insn 0x%08x, value 0x%llx) at %s: ignored", w, (unsigned long long)v,
            rt::describe(pc).c_str());
}
}
