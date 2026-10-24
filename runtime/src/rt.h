// Runtime-internal declarations (host side). Guest code only sees rc.h.
#pragma once
extern "C" {
#include "rc.h"
}
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rt {

// ---- logging ------------------------------------------------------------------
void log_init(const std::string& path);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_verbose;
#define RT_TRACE(...) do { if (rt::g_verbose) rt::log(__VA_ARGS__); } while (0)
double uptime();                                      // seconds since the runtime started
// notable events (files loaded, controllers) kept for the status screen; not logged
void activity(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> recent_activity(size_t n);
void set_fatal_hook(void (*hook)(const char* message)); // e.g. a message box, before exiting

// ---- guest address space -----------------------------------------------------------
constexpr uint64_t kMainBase = 0x7100000000ull;
constexpr uint64_t kSdkBase = 0x7180000000ull;
constexpr uint64_t kTrampBase = 0x7200000000ull;
constexpr uint64_t kTrampSize = 0x100000ull;          // 64k host functions
// a hook's call to the original it replaces (ORIG_<address>): main's functions again, at the same
// offsets (recomp/recomp.py's ORIGINALS)
constexpr uint64_t kOriginalsBase = 0x7104000000ull, kOriginalsSize = 0x2000000ull;
constexpr uint64_t kAslrBase = 0x0008000000ull, kAslrSize = 0x7FF8000000ull;
constexpr uint64_t kHeapBase = 0x2000000000ull, kHeapRegionSize = 0x180000000ull;
constexpr uint64_t kAliasBase = 0x0800000000ull, kAliasSize = 0x1000000000ull;
constexpr uint64_t kStackBase = 0x1C00000000ull, kStackSize = 0x80000000ull;
constexpr uint64_t kTlsBase = 0x1E00000000ull, kTlsSize = 0x1000000ull;
constexpr uint64_t kTotalMemory = 0xCD500000ull;       // application memory pool

enum MemState : uint32_t {
    kMemFree = 0x00, kMemCode = 0x03, kMemCodeData = 0x04, kMemHeap = 0x05, kMemShared = 0x06,
    kMemAlias = 0x0A, kMemStack = 0x0B, kMemTls = 0x0C, kMemTransfer = 0x0E, kMemIpc = 0x10,
};
void mem_init();
// record a mapping for svcQueryMemory (type, permission bits 1=R 2=W 4=X)
void mem_track(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm);
void mem_untrack(uint64_t addr, uint64_t size);
struct MemInfo { uint64_t addr, size; uint32_t type, attr, perm, ipc_ref, dev_ref, pad; };
MemInfo mem_query(uint64_t addr);
bool mem_commit(uint64_t addr, uint64_t size);       // within a reserved region
bool mem_written(uint64_t addr, uint64_t size);      // CPU writes since the last call (heap/alias); resets
uint64_t mem_heap_set_size(uint64_t size);            // svcSetHeapSize; returns base or 0
uint64_t mem_heap_size();
uint64_t mem_readable(uint64_t addr);                 // bytes readable from addr on (0: none); for debug scans
uint64_t mem_used();
uint32_t mem_map_alias(uint64_t dst, uint64_t src, uint64_t size);    // svcMapMemory (move semantics)
uint32_t mem_unmap_alias(uint64_t dst, uint64_t src, uint64_t size);
uint64_t mem_alloc_tlr();                             // 0x200-byte thread local region

// ---- modules / symbols / host functions -------------------------------------------------
struct Module {
    std::string name;
    uint64_t base = 0, end = 0, text_end = 0;
    uint64_t init = 0, fini = 0;                      // DT_INIT / DT_FINI (absolute)
    struct Sym { std::string name; uint64_t addr; uint16_t shndx; };
    std::vector<Sym> syms;                            // dynamic symbols
};
extern std::vector<Module> g_modules;
namespace nx { class Game; }
extern nx::Game* g_game;                             // the game read from the player's NSPs (null: from orig/)
void load_modules(const std::string& orig_dir);
uint64_t find_symbol(const std::string& name);       // 0 if absent
std::string describe(uint64_t guest_addr);           // "module+off <symbol+off>"
void log_backtrace(Ctx* c);                           // walks the guest frame-pointer chain
uint64_t guest_of_host(uint64_t host_rip);            // guest function compiled at that host address

// host functions callable from guest code get a trampoline address
uint64_t host_fn(const char* name, rc_fn fn);
void register_overrides();                            // nn:: functions replaced by host code
rc_fn lookup_override(const std::string& sym);       // nullptr if none
std::vector<std::string> override_names();
void dispatch_init();
void override_main(uint64_t addr, rc_fn fn);          // a main function replaced by host code (before dispatch_init)
void dump_call_counts(const std::string& path);       // LETSGO_CALLS: indirect calls per main function since the last dump
void watch_writes(uint64_t addr, double seconds);      // LETSGO_CONTROL watch: the guest functions writing 4 bytes there
// ---- settings.ini (settings.cpp) -----------------------------------------------------------
void settings_init(const std::string& file);
std::string setting(const std::string& key);           // "" when the file doesn't have it
void set_setting(const std::string& key, const std::string& value);   // written to the file
void render_scale_init(bool headless);                 // LETSGO_SCALE, scale=, or the monitor's height
int render_scale();                                    // screen-sized render targets are this many times the game's
// ---- frame rate (framerate.cpp) ----------------------------------------------------------
void framerate_init();                                // reads the choice; replaces the game's two period setters
bool fps60();                                         // the game at 60 frames a second, not its 30
bool fps60_odd_frame();                               // at 60, a frame the every-other-frame counts skip (input waits too)
void toggle_fps60();                                  // Tab

// ---- kernel (threads, sync, handles) -----------------------------------------------------
struct KThread;
Ctx* current_ctx();
KThread* current_thread();
uint32_t create_main_thread(uint64_t stack_top);
void* start_guest_main(uint64_t entry, const std::vector<uint64_t>& args);   // its host thread HANDLE
void svc_dispatch(Ctx* c, uint32_t imm);
void dump_threads();                                  // log every guest thread's wait and call chain
uint64_t ticks();                                     // 19.2 MHz
uint32_t kevent_create(uint32_t* readable);          // returns the writable handle
void kevent_signal(uint32_t handle);


// ---- platform (SDL window, keyboard, gamepads; platform.cpp) -------------------------------
void platform_init(bool headless);
[[noreturn]] void platform_run(void* guest_thread);   // event loop until the game or window ends
void platform_rumble(float low, float high);         // amplitudes 0..1 for the gamepad motors
// audio output (PCM16): a stream on the default device, nullptr when there is none (headless,
// LETSGO_MUTE, no device). Opening waits for the platform layer to start.
void* platform_audio_open(uint32_t rate, uint32_t channels);
void platform_audio_put(void* stream, const void* data, uint32_t bytes);
uint32_t platform_audio_queued(void* stream);          // bytes not yet played
void platform_audio_pause(void* stream, bool paused);
void platform_audio_close(void* stream);
bool platform_headless();
// the software keyboard: a text-entry dialog (false if canceled) and a message (true for OK/Yes)
bool platform_text_input(const std::u16string& prompt, const std::u16string& guide, std::u16string& text, int max_len);
bool platform_message(const std::u16string& msg, bool yes_no);
void keyboard_script(const std::string& spec);         // --keyboard: answers for the next keyboards, ';'-separated
void platform_screenshot(const std::string& spec);  // --screenshot FILE.png@SECONDS
uint64_t nvn_frames();                                // frames the game has presented
void nvn_set_turbo(bool on);                          // present as fast as possible (tests)
extern bool g_gpu;                                    // Vulkan renderer (default) or logging-only NVN
void* platform_hwnd();                                // the game window (HWND), null when headless
// a --screenshot that is due now: its path (and the request is consumed)
bool platform_screenshot_due(std::string& path);
bool platform_save_png(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba);

}  // namespace rt
