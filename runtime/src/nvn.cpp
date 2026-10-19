// Logging NVN (milestone 1): every NVN entry point exists and returns a
// plausible value, so the game's nn::gfx layer can initialize and record
// frames headlessly. Object state lives inside the game-owned opaque NVN
// structs. nvnQueuePresentTexture marks a frame and is paced by a virtual 60 Hz
// display at the window's present interval.
#include <windows.h>
#include <dwmapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "hle.h"
#include "perf.h"

namespace gpu {
void register_nvn_vulkan();
}

namespace rt {
bool g_gpu = true;                              // --no-gpu: logging-only NVN
namespace nvn {

enum Kind { K_void, K_bool, K_int32, K_int64, K_ptr, K_float };
#define K_void K_void
struct Proc { const char* name; Kind kind; int nargs; rc_fn custom; std::atomic<uint64_t> calls; uint64_t tramp; };

// ---- generated stubs -----------------------------------------------------------------------
enum ProcId {
#define NVN_PROC(n, k, a) P_##n,
#include "nvn_procs.inc"
#undef NVN_PROC
    P_COUNT
};
static Proc g_procs[P_COUNT] = {
#define NVN_PROC(n, k, a) {#n, K_##k, a, nullptr, {0}, 0},
#include "nvn_procs.inc"
#undef NVN_PROC
};
static std::atomic<uint64_t> g_frames;

uint64_t g_trace_frame = ~0ull;                 // --nvn-trace N: log every call made during frame N
static void call(Ctx* c, int id) {
    Proc& p = g_procs[id];
    uint64_t n = p.calls.fetch_add(1);
    if (g_frames == g_trace_frame)
        log("nvn-trace: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)", p.name, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4]);
    else if (n < 2 || g_verbose > 1)
        log("nvn: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx)%s", p.name, (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
            n == 1 ? " [further calls not logged]" : "");
    if (p.custom) { p.custom(c); return; }
    switch (p.kind) {
    case K_void: break;
    case K_bool: c->x[0] = 1; break;
    case K_float: c->v[0] = (V128){{0}}; break;
    default: c->x[0] = 0; break;
    }
}
#define NVN_PROC(n, k, a) static void stub_##n(Ctx* c) { call(c, P_##n); }
#include "nvn_procs.inc"
#undef NVN_PROC
static rc_fn g_stubs[P_COUNT] = {
#define NVN_PROC(n, k, a) stub_##n,
#include "nvn_procs.inc"
#undef NVN_PROC
};

static std::unordered_map<std::string, int> g_by_name;
static uint64_t proc_address(const char* name) {
    auto it = g_by_name.find(name);
    if (it == g_by_name.end()) {
        log("nvn: unknown proc \"%s\" requested", name);
        return 0;
    }
    Proc& p = g_procs[it->second];
    if (!p.tramp) p.tramp = host_fn(p.name, g_stubs[it->second]);
    return p.tramp;
}

// ---- object state kept inside the opaque NVN structs -----------------------------------------
template <class T> static T* obj(uint64_t p) { return (T*)(uintptr_t)p; }
struct PoolB { uint64_t mem, size, flags; };
struct Pool { uint64_t mem, size, flags; };
struct BufB { uint64_t pool, offset, size; };
struct Buf { uint64_t addr, size; };
struct TexB { uint32_t w, h, d, levels, format, target, samples, pad; uint64_t pool, offset; };
struct Tex { uint32_t w, h, d, levels, format, target; uint64_t addr; };
struct WinB { uint32_t count; uint32_t pad; uint64_t textures; uint32_t present_interval; };
struct Win { uint32_t count, next; uint64_t textures; uint32_t present_interval; };

static void DeviceGetProcAddress(Ctx* c) { c->x[0] = proc_address((const char*)(uintptr_t)c->x[1]); }

// device info: NX constants (generated from nnsdk's nvn_DeviceConstantsNX.h) + API version
static const int32_t kApiMajor = 53, kApiMinor = 207;
static void DeviceGetInteger(Ctx* c) {
    uint32_t prop = (uint32_t)c->x[1];
    int32_t* out = (int32_t*)(uintptr_t)c->x[2];
    int32_t v = 0;
    int known = 0;
    const char* name = "?";
    switch (prop) {
#define NVN_DEVINFO(id, val, k, nm) case id: v = val; known = k; name = nm; break;
#include "nvn_devinfo.inc"
#undef NVN_DEVINFO
    default: break;
    }
    if (prop == 0) { v = kApiMajor; known = 1; }
    if (prop == 1) { v = kApiMinor; known = 1; }
    if (g_procs[P_nvnDeviceGetInteger].calls < 300)
        log("nvn:   DeviceGetInteger(%s) -> %d%s", name, v, known ? "" : " (no NX constant: 0)");
    if (out) *out = v;
}

static void MemoryPoolBuilderSetStorage(Ctx* c) {
    auto* b = obj<PoolB>(c->x[0]);
    b->mem = c->x[1];
    b->size = c->x[2];
}
static void MemoryPoolBuilderSetFlags(Ctx* c) { obj<PoolB>(c->x[0])->flags = c->x[1]; }
static void MemoryPoolInitialize(Ctx* c) {
    auto* p = obj<Pool>(c->x[0]);
    auto* b = obj<PoolB>(c->x[1]);
    p->mem = b->mem;
    p->size = b->size;
    p->flags = b->flags;
    c->x[0] = 1;
}
static void MemoryPoolGetAddress(Ctx* c) { c->x[0] = obj<Pool>(c->x[0])->mem; }
static void MemoryPoolGetSize(Ctx* c) { c->x[0] = obj<Pool>(c->x[0])->size; }

static void BufferBuilderSetStorage(Ctx* c) {
    auto* b = obj<BufB>(c->x[0]);
    b->pool = c->x[1];
    b->offset = c->x[2];
    b->size = c->x[3];
}
static void BufferInitialize(Ctx* c) {
    auto* buf = obj<Buf>(c->x[0]);
    auto* b = obj<BufB>(c->x[1]);
    buf->addr = (b->pool ? obj<Pool>(b->pool)->mem : 0) + b->offset;
    buf->size = b->size;
    c->x[0] = 1;
}
static void BufferGetAddress(Ctx* c) { c->x[0] = obj<Buf>(c->x[0])->addr; }
static void BufferGetSize(Ctx* c) { c->x[0] = obj<Buf>(c->x[0])->size; }

static void TextureBuilderSetDefaults(Ctx* c) {
    auto* b = obj<TexB>(c->x[0]);
    memset(b, 0, sizeof *b);
    b->w = b->h = b->d = b->levels = b->samples = 1;
}
static void TextureBuilderSetSize2D(Ctx* c) { auto* b = obj<TexB>(c->x[0]); b->w = (uint32_t)c->x[1]; b->h = (uint32_t)c->x[2]; b->d = 1; }
static void TextureBuilderSetSize3D(Ctx* c) { auto* b = obj<TexB>(c->x[0]); b->w = (uint32_t)c->x[1]; b->h = (uint32_t)c->x[2]; b->d = (uint32_t)c->x[3]; }
static void TextureBuilderSetSize1D(Ctx* c) { auto* b = obj<TexB>(c->x[0]); b->w = (uint32_t)c->x[1]; b->h = b->d = 1; }
static void TextureBuilderSetLevels(Ctx* c) { obj<TexB>(c->x[0])->levels = (uint32_t)c->x[1]; }
static void TextureBuilderSetFormat(Ctx* c) { obj<TexB>(c->x[0])->format = (uint32_t)c->x[1]; }
static void TextureBuilderSetTarget(Ctx* c) { obj<TexB>(c->x[0])->target = (uint32_t)c->x[1]; }
static void TextureBuilderSetSamples(Ctx* c) { obj<TexB>(c->x[0])->samples = (uint32_t)c->x[1] ? (uint32_t)c->x[1] : 1; }
static void TextureBuilderSetStorage(Ctx* c) { auto* b = obj<TexB>(c->x[0]); b->pool = c->x[1]; b->offset = c->x[2]; }
static uint64_t tex_size(const TexB* b) {
    // generous estimate: 16 bytes per texel (covers RGBA32F), full mip chain, 64 KiB aligned
    uint64_t total = 0, w = b->w ? b->w : 1, h = b->h ? b->h : 1, d = b->d ? b->d : 1;
    for (uint32_t l = 0; l < (b->levels ? b->levels : 1); l++) {
        total += w * h * d * 16 * (b->samples ? b->samples : 1);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return (total + 0xFFFF) & ~0xFFFFull;
}
static void TextureBuilderGetStorageSize(Ctx* c) { c->x[0] = tex_size(obj<TexB>(c->x[0])); }
static void TextureBuilderGetStorageAlignment(Ctx* c) { c->x[0] = 0x200; }
static void TextureInitialize(Ctx* c) {
    auto* t = obj<Tex>(c->x[0]);
    auto* b = obj<TexB>(c->x[1]);
    t->w = b->w; t->h = b->h; t->d = b->d; t->levels = b->levels; t->format = b->format; t->target = b->target;
    t->addr = (b->pool ? obj<Pool>(b->pool)->mem : 0) + b->offset;
    c->x[0] = 1;
}
static void TextureGetWidth(Ctx* c) { c->x[0] = obj<Tex>(c->x[0])->w; }
static void TextureGetHeight(Ctx* c) { c->x[0] = obj<Tex>(c->x[0])->h; }
static void TextureGetDepth(Ctx* c) { c->x[0] = obj<Tex>(c->x[0])->d; }
static void TextureGetLevels(Ctx* c) { c->x[0] = obj<Tex>(c->x[0])->levels; }
static void TextureGetFormat(Ctx* c) { c->x[0] = obj<Tex>(c->x[0])->format; }
static void TextureGetTarget(Ctx* c) { c->x[0] = obj<Tex>(c->x[0])->target; }
static void TextureGetTextureAddress(Ctx* c) { c->x[0] = obj<Tex>(c->x[0])->addr; }

static void WindowBuilderSetTextures(Ctx* c) {
    auto* b = obj<WinB>(c->x[0]);
    b->count = (uint32_t)c->x[1];
    b->textures = c->x[2];
}
static void WindowBuilderSetPresentInterval(Ctx* c) { obj<WinB>(c->x[0])->present_interval = (uint32_t)c->x[1]; }
static void WindowInitialize(Ctx* c) {
    auto* w = obj<Win>(c->x[0]);
    auto* b = obj<WinB>(c->x[1]);
    w->count = b->count ? b->count : 2;
    w->next = 0;
    w->textures = b->textures;
    w->present_interval = b->present_interval;
    log("nvn: window initialized with %u textures", w->count);
    c->x[0] = 1;
}
// AcquireTexture(window, sync, int* index) -> NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
static void WindowAcquireTexture(Ctx* c) {
    auto* w = obj<Win>(c->x[0]);
    *(int32_t*)(uintptr_t)c->x[2] = (int32_t)(w->next++ % (w->count ? w->count : 2));
    c->x[0] = 0;
}
static void WindowSetPresentInterval(Ctx* c) { obj<Win>(c->x[0])->present_interval = (uint32_t)c->x[1]; }
static void WindowGetPresentInterval(Ctx* c) { c->x[0] = obj<Win>(c->x[0])->present_interval; }
// a present shows at the next vblank at least `interval` refreshes after the previous one;
// interval 0 presents immediately
// LETSGO_TURBO[=S]: tests present as fast as possible (for the first S seconds); the control
// file can switch it (nvn_set_turbo)
static std::atomic<int> g_turbo{-1};                      // -1: as LETSGO_TURBO says, 0 off, 1 on
static bool turbo_now() {
    static const double until = [] {
        const char* t = getenv("LETSGO_TURBO");
        return !t ? -1.0 : atof(t) > 0 ? atof(t) : 1e30;
    }();
    int t = g_turbo.load();
    return t < 0 ? uptime() < until : t == 1;
}
// times in QueryPerformanceCounter ticks (the compositor's clock too)
static int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static int64_t qpc_freq() {
    static const int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return (int64_t)q.QuadPart;
    }();
    return f;
}
// sleeps until `t` to within a few microseconds: a high-resolution waitable timer for most of the
// wait (Sleep can oversleep by a millisecond or two), then a spin for the rest
static void sleep_precisely_until(int64_t t) {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t f = qpc_freq();
    for (;;) {
        int64_t left = t - qpc();
        if (left <= 0) return;
        if (left > f * 3 / 2000) {                                      // over 1.5 ms: sleep all but 1 ms
            LONGLONG wait_100ns = (LONGLONG)((left - f / 1000) * 10000000 / f);
            LARGE_INTEGER due;
            due.QuadPart = -wait_100ns;
            if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
            else Sleep(1);
        } else if (left > f / 5000) {                                   // over 0.2 ms
            SwitchToThread();
        } else {
            YieldProcessor();
        }
    }
}
// The display the window is on, from the compositor: the time of its last vblank and its refresh
// period. When that's a whole multiple of 60 Hz (59.94, 60, 120, 240 Hz...), frames are timed to the
// display itself, half a refresh after a vblank, so each stays on screen for exactly as many
// refreshes as the game asked for. Otherwise (144 Hz, variable refresh rate) the 60 Hz clock alone.
static bool display_refresh(int64_t& vblank, int64_t& refresh, int& per_60hz) {
    static const bool off = getenv("LETSGO_NO_VSYNC_LOCK") != nullptr;
    if (off || !platform_hwnd()) return false;
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti))) return false;
    const int64_t f = qpc_freq();
    vblank = (int64_t)ti.qpcVBlank;
    refresh = (int64_t)ti.qpcRefreshPeriod;
    if (vblank <= 0 || refresh < f / 500 || refresh > f / 24) return false;
    double hz = (double)f / (double)refresh;
    per_60hz = (int)(hz / 60.0 + 0.5);
    return per_60hz >= 1 && std::fabs(hz - 60.0 * per_60hz) < 0.5 * per_60hz;
}
// waits for the frame's place on the virtual 60 Hz display: frames go out evenly spaced, whatever
// each took to make
static void pace(uint32_t interval) {
    static std::mutex m;
    static int64_t next = 0;
    static int logged = 0;
    if (!interval || turbo_now()) return;
    std::lock_guard<std::mutex> lk(m);
    const int64_t f = qpc_freq();
    int64_t period = f * interval / 60;
    int64_t vblank = 0, refresh = 0;
    int per_60hz = 0;
    bool locked = display_refresh(vblank, refresh, per_60hz);
    if (locked) period = refresh * per_60hz * interval;
    if (locked != (logged == 1) || !logged) {
        logged = locked ? 1 : 2;
        if (locked) log("nvn: frames timed to the display (%.3f Hz)", (double)f / (double)refresh);
        else log("nvn: frames timed to a 60 Hz clock");
    }
    int64_t now = qpc();
    if (!next || now > next + period) next = now;                     // first frame, or a stall: resync
    if (locked) {
        // half a refresh after a vblank: clear of the refreshes on both sides. A quarter of the way
        // there each frame, so a correction never shows
        int64_t phase = ((next - vblank) % refresh + refresh) % refresh;
        next -= (phase - refresh / 2) / 4;
    }
    if (next > now) sleep_precisely_until(next);
    next += period;
}
// a frame's present: pacing at the window's interval comes first (frame_pace), then the present,
// then counting and a periodic log line (frame_done)
void frame_pace(uint64_t interval) { pace((uint32_t)interval); }
void frame_done() {
    perf::frame();
    uint64_t n = ++g_frames;
    if (n <= 5 || (n % 300) == 0) log("nvn: *** frame %llu presented ***", (unsigned long long)n);
}
void frame_presented(uint64_t interval) {
    frame_pace(interval);
    frame_done();
}
// QueuePresentTexture(queue, window, index)
static void QueuePresentTexture(Ctx* c) { frame_presented(obj<Win>(c->x[1])->present_interval); }
static uint64_t g_cmd_handles = 0x1000;
static void CommandBufferEndRecording(Ctx* c) { c->x[0] = g_cmd_handles++; }
static void SyncWait(Ctx* c) { c->x[0] = 0; }                      // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
static void DeviceGetTextureHandle(Ctx* c) { c->x[0] = (c->x[2] << 20) | (c->x[1] & 0xFFFFF); }
static void DeviceGetImageHandle(Ctx* c) { c->x[0] = c->x[1]; }

static void BootstrapLoader(Ctx* c) {
    const char* name = (const char*)(uintptr_t)c->x[0];
    log("nvnBootstrapLoader(\"%s\")", name);
    c->x[0] = proc_address(name);
}

static void set(int id, rc_fn fn) { g_procs[id].custom = fn; }
void set_proc(const char* name, rc_fn fn) {
    auto it = g_by_name.find(name);
    if (it == g_by_name.end()) fatal("nvn: no entry point named %s", name);
    g_procs[it->second].custom = fn;
}

}  // namespace nvn

uint64_t nvn_frames() { return nvn::g_frames; }
void nvn_set_turbo(bool on) { nvn::g_turbo = on ? 1 : 0; }

void register_nvn_overrides(void (*reg)(const char*, rc_fn)) {
    using namespace nvn;
    for (int i = 0; i < P_COUNT; i++) g_by_name[g_procs[i].name] = i;
    set(P_nvnDeviceGetProcAddress, DeviceGetProcAddress);
    set(P_nvnDeviceGetInteger, DeviceGetInteger);
    set(P_nvnMemoryPoolBuilderSetStorage, MemoryPoolBuilderSetStorage);
    set(P_nvnMemoryPoolBuilderSetFlags, MemoryPoolBuilderSetFlags);
    set(P_nvnMemoryPoolInitialize, MemoryPoolInitialize);
    set(P_nvnMemoryPoolGetBufferAddress, MemoryPoolGetAddress);
    set(P_nvnMemoryPoolMap, MemoryPoolGetAddress);
    set(P_nvnMemoryPoolGetSize, MemoryPoolGetSize);
    set(P_nvnBufferBuilderSetStorage, BufferBuilderSetStorage);
    set(P_nvnBufferInitialize, BufferInitialize);
    set(P_nvnBufferGetAddress, BufferGetAddress);
    set(P_nvnBufferMap, BufferGetAddress);
    set(P_nvnBufferGetSize, BufferGetSize);
    set(P_nvnTextureBuilderSetDefaults, TextureBuilderSetDefaults);
    set(P_nvnTextureBuilderSetSize1D, TextureBuilderSetSize1D);
    set(P_nvnTextureBuilderSetSize2D, TextureBuilderSetSize2D);
    set(P_nvnTextureBuilderSetSize3D, TextureBuilderSetSize3D);
    set(P_nvnTextureBuilderSetLevels, TextureBuilderSetLevels);
    set(P_nvnTextureBuilderSetFormat, TextureBuilderSetFormat);
    set(P_nvnTextureBuilderSetTarget, TextureBuilderSetTarget);
    set(P_nvnTextureBuilderSetSamples, TextureBuilderSetSamples);
    set(P_nvnTextureBuilderSetStorage, TextureBuilderSetStorage);
    set(P_nvnTextureBuilderGetStorageSize, TextureBuilderGetStorageSize);
    set(P_nvnTextureBuilderGetStorageAlignment, TextureBuilderGetStorageAlignment);
    set(P_nvnTextureInitialize, TextureInitialize);
    set(P_nvnTextureGetWidth, TextureGetWidth);
    set(P_nvnTextureGetHeight, TextureGetHeight);
    set(P_nvnTextureGetDepth, TextureGetDepth);
    set(P_nvnTextureGetLevels, TextureGetLevels);
    set(P_nvnTextureGetFormat, TextureGetFormat);
    set(P_nvnTextureGetTarget, TextureGetTarget);
    set(P_nvnTextureGetTextureAddress, TextureGetTextureAddress);
    set(P_nvnWindowBuilderSetTextures, WindowBuilderSetTextures);
    set(P_nvnWindowBuilderSetPresentInterval, WindowBuilderSetPresentInterval);
    set(P_nvnWindowInitialize, WindowInitialize);
    set(P_nvnWindowAcquireTexture, WindowAcquireTexture);
    set(P_nvnQueuePresentTexture, QueuePresentTexture);
    set(P_nvnWindowSetPresentInterval, WindowSetPresentInterval);
    set(P_nvnWindowGetPresentInterval, WindowGetPresentInterval);
    set(P_nvnCommandBufferEndRecording, CommandBufferEndRecording);
    set(P_nvnSyncWait, SyncWait);
    set(P_nvnDeviceGetTextureHandle, DeviceGetTextureHandle);
    set(P_nvnDeviceGetImageHandle, DeviceGetImageHandle);
    if (g_gpu) gpu::register_nvn_vulkan();
    reg("nvnBootstrapLoader", BootstrapLoader);
    // NVIDIA driver setup (transfer memory for the GPU driver, allocator hooks): nothing to do
    reg("_ZN2nv18InitializeGraphicsEPvm", hle_void);
    reg("_ZN2nv16FinalizeGraphicsEv", hle_void);
    reg("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", hle_void);
}

}  // namespace rt
