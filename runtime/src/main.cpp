// letsgo-pc runtime entry: map the game, then start it the way the Switch's
// loader does - nn::init::Start(main thread handle, arg buffer, cbA, cbB).
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "hle.h"
#include "mods.h"
#include "nx.h"
#include "setup.h"

// The single-file build for players (LetsGoPikachu.exe) reads the game from their own NSPs only;
// the development build (letsgo.exe) uses orig/ unless --base is given.
#ifdef LETSGO_DIST
constexpr bool kDist = true;
#else
constexpr bool kDist = false;
#endif

namespace rt {

// rtld callback A: marks the loader as initialized; nothing to do here
static void cb_loader_ready(Ctx* c) { (void)c; }

// rtld callback B: run every module's DT_INIT in load order (main, then sdk),
// which runs the static constructors
static void cb_run_initializers(Ctx* c) {
    for (auto& M : g_modules) {
        if (!M.init) continue;
        log("running %s initializers (%s)", M.name.c_str(), describe(M.init).c_str());
        uint64_t lr = c->x[30];
        rc_lookup(M.init)(c);
        c->x[30] = lr;
    }
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_BREAKPOINT || er->ExceptionCode == 0x406D1388 /* thread name */)
        return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionCode < 0xC0000000u || er->ExceptionCode == 0xE06D7363u /* C++ throw */)
        return EXCEPTION_CONTINUE_SEARCH;
    static const uint64_t image = (uint64_t)GetModuleHandleW(nullptr);
    static const uint64_t image_end = image + ((IMAGE_NT_HEADERS*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew))
                                                  ->OptionalHeader.SizeOfImage;
    uint64_t rip = ep->ContextRecord->Rip;
    Ctx* c = current_ctx();
    if (rip < image || rip >= image_end) {
        // another module's: its own business (drivers handle some of theirs), unless a game thread
        // faults in it, as in the C runtime's memcpy called for the game
        if (!c || er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &module))
            GetModuleFileNameW(module, path, MAX_PATH);
        log("host exception 0x%08lx in %ls+0x%llx on a game thread (access 0x%llx %s)", er->ExceptionCode,
            std::filesystem::path(path).filename().c_str(), (unsigned long long)(rip - (uint64_t)module),
            (unsigned long long)(er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0),
            er->NumberParameters > 0 && er->ExceptionInformation[0] == 1 ? "write" : "read");
    } else {
        log("host exception 0x%08lx at letsgo.exe+0x%llx (access 0x%llx %s)", er->ExceptionCode,
            (unsigned long long)(rip - image),
            (unsigned long long)(er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0),
            er->NumberParameters > 0 && er->ExceptionInformation[0] == 1 ? "write" : "read");
    }
    if (uint64_t g = guest_of_host(ep->ContextRecord->Rip))
        log("  in guest function %s", describe(g).c_str());
    if (c) {
        log_backtrace(c);
        log("  guest sp 0x%llx", (unsigned long long)c->sp);
        for (int i = 0; i < 31; i += 4)
            log("  x%-2d %016llx x%-2d %016llx x%-2d %016llx x%-2d %016llx", i, (unsigned long long)c->x[i], i + 1,
                (unsigned long long)c->x[i + 1], i + 2, (unsigned long long)c->x[i + 2], i + 3,
                (unsigned long long)(i + 3 < 31 ? c->x[i + 3] : 0));
    }
    fatal("crashed");
}

}  // namespace rt

extern "C" void rc_callcheck_fail(Ctx* c, uint64_t ret, uint64_t target, const uint64_t* saved, uint64_t saved_sp) {
    rt::log("ABI violation: callee returning to %s (indirect target %s) changed callee-saved state", rt::describe(ret).c_str(),
            target ? rt::describe(target).c_str() : "direct");
    for (int i = 0; i < 11; i++)
        if (saved[i] != c->x[19 + i])
            rt::log("  x%d: 0x%llx -> 0x%llx", 19 + i, (unsigned long long)saved[i], (unsigned long long)c->x[19 + i]);
    if (saved_sp != c->sp) rt::log("  sp: 0x%llx -> 0x%llx", (unsigned long long)saved_sp, (unsigned long long)c->sp);
    rt::log_backtrace(c);
    rt::fatal("callee-saved register check failed");
}

extern "C" void rc_trap(Ctx* c, uint64_t pc, uint32_t word, const char* why) {
    rt::log_backtrace(c);
    rt::fatal("guest trap (%s) at %s, insn 0x%08x, lr %s", why, rt::describe(pc).c_str(), word,
              rt::describe(c ? c->x[30] : 0).c_str());
}

// orig/ when --orig isn't given: next to the exe or up to two levels above it (build/pc/),
// then the working directory, so double-clicking the exe works
static std::string find_orig() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    for (auto d : {dir, dir.parent_path(), dir.parent_path().parent_path(), std::filesystem::current_path()})
        if (std::filesystem::exists(d / "orig" / "elf" / "main.elf")) return (d / "orig").string();
    return "orig";
}

namespace rt::nvn { extern uint64_t g_trace_frame; }
namespace gpu { void shaders_set_cache(const std::string& path); }

int main(int argc, char** argv) {
    using namespace rt;
    std::string orig, press, shot;
    bool headless = false;
    std::vector<double> dump_at;                          // --dump-threads SECONDS (repeatable)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") g_verbose = 1;
        else if (a == "--orig" && i + 1 < argc) orig = argv[++i];
        else if (a == "--headless") headless = true;
        else if (a == "--no-gpu") g_gpu = false;
        else if (a == "--keyboard" && i + 1 < argc) keyboard_script(argv[++i]);
        else if (a == "--nvn-trace" && i + 1 < argc) nvn::g_trace_frame = strtoull(argv[++i], nullptr, 10);
        else if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--press" && i + 1 < argc) press = argv[++i];
        else if (a == "--dump-threads" && i + 1 < argc) dump_at.push_back(atof(argv[++i]));
    }
    GameFiles files;                                      // --base / --update / --keys (any characters)
    std::wstring mods_dir;                                // --mods
    {
        int wargc = 0;
        wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        for (int i = 1; wargv && i + 1 < wargc; i++) {
            if (!wcscmp(wargv[i], L"--base")) files.base = wargv[++i];
            else if (!wcscmp(wargv[i], L"--update")) files.update = wargv[++i];
            else if (!wcscmp(wargv[i], L"--keys")) files.keys = wargv[++i];
            else if (!wcscmp(wargv[i], L"--mods")) mods_dir = wargv[++i];
        }
        LocalFree(wargv);
    }
    bool from_nsp = !files.base.empty() || (kDist && orig.empty());
    std::wstring data_dir = from_nsp ? app_data_dir() : L"";
    if (!from_nsp && orig.empty()) orig = find_orig();
    log_init(kDist && from_nsp ? narrow(data_dir + L"\\letsgo.log") : "letsgo.log");
    if (kDist && !headless)                               // no console: stop with a message box
        set_fatal_hook([](const char* msg) {
            std::string text = std::string(msg) + "\n\nDetails are in %APPDATA%\\LetsGoPikachu\\letsgo.log.";
            int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            std::wstring w((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
            MessageBoxW(nullptr, w.c_str(), L"Pok\u00e9mon: Let's Go, Pikachu!", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        });
    AddVectoredExceptionHandler(1, on_exception);
    static std::unique_ptr<nx::Game> game;
    if (from_nsp) {
        game = open_player_game(files, data_dir, !headless);
        if (!game) return 0;                              // the player cancelled
        g_game = game.get();
        fsx::g_save = narrow(data_dir + L"\\save");
    } else {
        fsx::g_orig = orig;
        fsx::g_save = (std::filesystem::absolute(orig).parent_path() / "save").string();   // beside orig/
    }
    // translated shaders kept between runs (gpu/shaders.cpp): beside the saves, or LETSGO_SHADER_CACHE
    // (a file, or "off")
    std::string shader_cache = from_nsp ? narrow(data_dir + L"\\shader_cache.bin")
                                        : narrow((std::filesystem::absolute(orig).parent_path() / "cache" / "shader_cache.bin").wstring());
    if (const char* sc = getenv("LETSGO_SHADER_CACHE")) shader_cache = !strcmp(sc, "off") ? "" : sc;
    gpu::shaders_set_cache(shader_cache);
    if (const char* d = getenv("LETSGO_SAVE_DIR")) {                                    // tests: a save of their own
        std::filesystem::create_directories(d);
        fsx::g_save = std::filesystem::absolute(d).string();
    }
    log("letsgo-pc runtime starting (game: %s, saves: %s)", from_nsp ? "the player's NSPs" : orig.c_str(),
        fsx::g_save.c_str());
    if (!from_nsp && !std::filesystem::exists(orig + "/elf/main.elf"))
        fatal("game files not found in \"%s\": run tools/prepare.sh, or pass --orig <dir>", orig.c_str());
    // the mods folder (mods.h): --mods DIR or LETSGO_MODS ("off": none), else mods/ beside the exe, then
    // beside orig/ (or, with the NSPs, in %APPDATA%\LetsGoPikachu)
    if (mods_dir.empty())
        if (const wchar_t* m = _wgetenv(L"LETSGO_MODS")) mods_dir = m;
    if (mods_dir.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path beside = std::filesystem::path(exe).parent_path() / L"mods";
        mods_dir = std::filesystem::exists(beside) ? beside.wstring()
                   : from_nsp                      ? data_dir + L"\\mods"
                                                   : (std::filesystem::absolute(orig).parent_path() / L"mods").wstring();
    }
    if (mods_dir != L"off") mods::init(mods_dir);
    mem_init();
    timeBeginPeriod(1);                                   // 1 ms sleep granularity for frame and audio pacing
    load_modules(orig);
    dispatch_init();
    if (!press.empty()) hid::start_input_script(press);
    if (const char* ctl = getenv("LETSGO_CONTROL")) hid::start_control_file(ctl);
    for (double t : dump_at)
        std::thread([t] {
            std::this_thread::sleep_for(std::chrono::duration<double>(t));
            dump_threads();
        }).detach();

    // main thread: 1 MiB stack (main.npdm) at the top of the stack region
    const uint64_t stack_size = 0x100000;
    uint64_t stack = kStackBase + kStackSize - stack_size - 0x10000;
    mem_commit(stack, stack_size);
    mem_track(stack, stack_size, kMemStack, 3);
    uint32_t main_handle = create_main_thread(stack + stack_size);

    // program arguments, in the loader's format: u32 allocated size, u32 string size,
    // 0x18 reserved, then the string. nnosInitialize tokenizes it into argc/argv
    // (and checks the region's memory type is 3, as the loader maps it).
    uint64_t argbuf = kAliasBase;
    mem_commit(argbuf, 0x10000);
    mem_track(argbuf, 0x10000, kMemCode, 3);
    const char* args = "letsgo";
    *(uint32_t*)argbuf = 0x9000;
    *(uint32_t*)(argbuf + 4) = (uint32_t)strlen(args);
    memcpy((void*)(argbuf + 0x20), args, strlen(args));

    uint64_t start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) fatal("nn::init::Start not found in the sdk");
    uint64_t cb_a = host_fn("rtld:loader_ready", cb_loader_ready);
    uint64_t cb_b = host_fn("rtld:run_initializers", cb_run_initializers);
    log("starting guest: nn::init::Start at %s", describe(start).c_str());
    void* guest = start_guest_main(start, {main_handle, argbuf, cb_a, cb_b});
    if (!shot.empty()) platform_screenshot(shot);
    platform_init(headless);                              // the game boots while SDL starts (~1 s)
    platform_run(guest);
}
