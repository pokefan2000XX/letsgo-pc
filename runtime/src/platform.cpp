// PC platform layer (M2): the SDL window, keyboard and gamepads. Every input
// source becomes the Switch's Handheld controller (hid::set_host_input) and
// the game's vibration drives gamepad rumble. Until the renderer exists (M3)
// the window shows a status screen: the game's frame rate, the Switch input it
// sees, and the files it is loading. SDL is only used on the process's main
// thread; guest threads talk to it through atomics.
#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "hle.h"

namespace rt {
namespace {

#ifdef LETSGO_DIST
constexpr bool kDistBuild = true;     // LetsGoPikachu.exe: the players' build
#else
constexpr bool kDistBuild = false;
#endif

enum Button { A, B, X, Y, LStick, RStick, L, R, ZL, ZR, Plus, Minus, Left, Up, Right, Down };
const char* const kButtonNames[] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-",
                                    "Left", "Up", "Right", "Down"};

// keyboard, laid out like the DS emulators (DeSmuME, melonDS): arrows move (the left stick, which
// this game walks and points menus with; Left Shift = half tilt), X/Z/S/A are A/B/X/Y, Q/W are
// L/R, Enter is Plus (Start), Backspace or Right Shift is Minus (Select). Beyond the DS: E/R are
// ZL/ZR, I/J/K/L the d-pad, numpad 8/4/2/6 the right stick, F/G the stick clicks
const struct { SDL_Scancode key; Button button; } kKeys[] = {
    {SDL_SCANCODE_X, A},      {SDL_SCANCODE_Z, B},         {SDL_SCANCODE_S, X},       {SDL_SCANCODE_A, Y},
    {SDL_SCANCODE_Q, L},      {SDL_SCANCODE_W, R},         {SDL_SCANCODE_E, ZL},      {SDL_SCANCODE_R, ZR},
    {SDL_SCANCODE_RETURN, Plus}, {SDL_SCANCODE_KP_ENTER, Plus}, {SDL_SCANCODE_BACKSPACE, Minus}, {SDL_SCANCODE_RSHIFT, Minus},
    {SDL_SCANCODE_I, Up},     {SDL_SCANCODE_K, Down},      {SDL_SCANCODE_J, Left},    {SDL_SCANCODE_L, Right},
    {SDL_SCANCODE_F, LStick}, {SDL_SCANCODE_G, RStick},
};

bool g_headless = true;
// --screenshot: in the order given; `frame` entries wait for that many presented frames
struct Shot { bool frame; double when; std::string path; };
std::vector<Shot> g_shots;
std::mutex g_shot_lock;

// the path of the next screenshot if it is due
bool next_shot(std::string& path) {
    std::lock_guard<std::mutex> lk(g_shot_lock);
    if (g_shots.empty()) return false;
    const Shot& s = g_shots.front();
    if (s.frame ? (double)nvn_frames() < s.when : uptime() < s.when) return false;
    path = s.path;
    g_shots.erase(g_shots.begin());
    return true;
}
SDL_Window* g_win;
SDL_Renderer* g_ren;
std::vector<SDL_Gamepad*> g_pads;
std::atomic<uint32_t> g_rumble_lo{0}, g_rumble_hi{0};    // 0..65535, from the game's vibration
uint32_t g_rumble_sent_lo, g_rumble_sent_hi;
uint64_t g_rumble_sent_at;

// a stick vector in [-1, 1]^2 (y up) with a radial dead zone, as Switch units
void put_stick(float x, float y, int32_t out[2]) {
    float m = std::sqrt(x * x + y * y);
    const float dz = 0.12f;
    if (m < dz) { out[0] = out[1] = 0; return; }
    float s = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
    out[0] = (int32_t)std::lround(std::clamp(x * s, -1.0f, 1.0f) * 32767.0f);
    out[1] = (int32_t)std::lround(std::clamp(y * s, -1.0f, 1.0f) * 32767.0f);
}
float mag(const int32_t v[2]) { return std::hypot((float)v[0], (float)v[1]); }
void take_larger(int32_t dst[2], const int32_t src[2]) {
    if (mag(src) > mag(dst)) { dst[0] = src[0]; dst[1] = src[1]; }
}

// face buttons follow the label printed on the pad, so the button marked A confirms on
// every controller; pads without letters (PlayStation) use the Xbox positions
Button face_button(SDL_Gamepad* pad, SDL_GamepadButton b) {
    switch (SDL_GetGamepadButtonLabel(pad, b)) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return A;
    case SDL_GAMEPAD_BUTTON_LABEL_B: return B;
    case SDL_GAMEPAD_BUTTON_LABEL_X: return X;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return Y;
    default: break;
    }
    return b == SDL_GAMEPAD_BUTTON_SOUTH ? A : b == SDL_GAMEPAD_BUTTON_EAST ? B : b == SDL_GAMEPAD_BUTTON_WEST ? X : Y;
}

void update_input() {
    hid::InputState in;
    // keyboard
    const bool* k = SDL_GetKeyboardState(nullptr);
    for (auto& m : kKeys)
        if (k[m.key]) in.buttons |= 1ull << m.button;
    auto key_stick = [&](SDL_Scancode left, SDL_Scancode right, SDL_Scancode up, SDL_Scancode down, int32_t out[2]) {
        float x = (float)k[right] - (float)k[left], y = (float)k[up] - (float)k[down];
        if (x != 0 && y != 0) { x *= 0.70710678f; y *= 0.70710678f; }
        if (k[SDL_SCANCODE_LSHIFT]) { x *= 0.5f; y *= 0.5f; }
        out[0] = (int32_t)std::lround(x * 32767.0f);
        out[1] = (int32_t)std::lround(y * 32767.0f);
    };
    key_stick(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, in.stick_l);
    key_stick(SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_2, in.stick_r);
    // gamepads
    for (SDL_Gamepad* p : g_pads) {
        for (SDL_GamepadButton b : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                    SDL_GAMEPAD_BUTTON_NORTH})
            if (SDL_GetGamepadButton(p, b)) in.buttons |= 1ull << face_button(p, b);
        const struct { SDL_GamepadButton b; Button s; } map[] = {
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R},
            {SDL_GAMEPAD_BUTTON_START, Plus},      {SDL_GAMEPAD_BUTTON_BACK, Minus},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, Up},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Down},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Left},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Right},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, LStick}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RStick},
        };
        for (auto& m : map)
            if (SDL_GetGamepadButton(p, m.b)) in.buttons |= 1ull << m.s;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) in.buttons |= 1ull << ZL;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) in.buttons |= 1ull << ZR;
        auto axis = [&](SDL_GamepadAxis a) { return std::max(-1.0f, SDL_GetGamepadAxis(p, a) / 32767.0f); };
        int32_t sl[2], sr[2];
        put_stick(axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY), sl);    // SDL's y grows downward
        put_stick(axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY), sr);
        take_larger(in.stick_l, sl);
        take_larger(in.stick_r, sr);
    }
    hid::set_host_input(in);
}

// rumble updates arrive every few milliseconds; resend when they change, and refresh
// the duration before it runs out
void apply_rumble() {
    uint32_t lo = g_rumble_lo, hi = g_rumble_hi;
    uint64_t now = SDL_GetTicks();
    if (lo == g_rumble_sent_lo && hi == g_rumble_sent_hi && ((lo == 0 && hi == 0) || now - g_rumble_sent_at < 100)) return;
    for (SDL_Gamepad* p : g_pads) SDL_RumbleGamepad(p, (Uint16)lo, (Uint16)hi, 200);
    g_rumble_sent_lo = lo;
    g_rumble_sent_hi = hi;
    g_rumble_sent_at = now;
}

void open_pad(SDL_JoystickID id) {
    if (SDL_Gamepad* p = SDL_OpenGamepad(id)) {
        g_pads.push_back(p);
        activity("controller connected: %s", SDL_GetGamepadName(p));
        log("input: controller connected: %s", SDL_GetGamepadName(p));
    }
}
void close_pad(SDL_JoystickID id) {
    for (size_t i = 0; i < g_pads.size(); i++)
        if (SDL_GetGamepadID(g_pads[i]) == id) {
            activity("controller disconnected: %s", SDL_GetGamepadName(g_pads[i]));
            SDL_CloseGamepad(g_pads[i]);
            g_pads.erase(g_pads.begin() + (long)i);
            return;
        }
}

// ---- status screen (640x360 logical, 8x8 glyphs: 80 columns, 45 rows) --------------------
struct Text {
    float y = 6;
    void line(const std::string& s, Uint8 r = 220, Uint8 g = 220, Uint8 b = 220, float x = 8) {
        SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
        SDL_RenderDebugText(g_ren, x, y, s.c_str());
        y += 10;
    }
    void gap(float h = 6) { y += h; }
};

std::string stick_text(const int32_t v[2]) {
    char b[32];
    snprintf(b, sizeof b, "(%+.2f, %+.2f)", v[0] / 32767.0, v[1] / 32767.0);
    return b;
}

void draw_status() {
    static uint64_t last_frames, last_t;
    static double fps;
    uint64_t frames = nvn_frames(), t = SDL_GetTicks();
    if (t - last_t >= 1000) {
        fps = (double)(frames - last_frames) * 1000.0 / (double)(t - last_t);
        last_frames = frames;
        last_t = t;
    }
    SDL_SetRenderDrawColor(g_ren, 14, 16, 22, 255);
    SDL_RenderClear(g_ren);
    Text tx;
    char buf[160];
    tx.line("POKEMON: LET'S GO, PIKACHU!  -  letsgo-pc", 255, 214, 64);
    tx.line("No graphics until milestone M3: this screen shows what the game is doing.", 150, 150, 160);
    tx.gap();
    snprintf(buf, sizeof buf, "Game: frame %llu, %.1f fps, running %.0f s", (unsigned long long)frames, fps, uptime());
    tx.line(buf);
    std::string pads = "keyboard";
    for (SDL_Gamepad* p : g_pads) pads += std::string(" + ") + SDL_GetGamepadName(p);
    tx.line("Controls: " + pads.substr(0, 68));
    tx.gap();
    hid::InputState in = hid::current_input();
    std::string held;
    for (int i = 0; i < 16; i++)
        if (in.buttons & (1ull << i)) held += std::string(kButtonNames[i]) + " ";
    tx.line("Switch controller (Handheld), as the game sees it:", 120, 200, 255);
    tx.line("  Buttons:  " + (held.empty() ? std::string("-") : held));
    tx.line("  L stick:  " + stick_text(in.stick_l) + "    R stick: " + stick_text(in.stick_r));
    uint32_t lo = g_rumble_lo, hi = g_rumble_hi;
    snprintf(buf, sizeof buf, "  Rumble:   low %3.0f%%  high %3.0f%%", lo / 655.35, hi / 655.35);
    tx.line(buf);
    tx.gap();
    tx.line("Recent activity:", 120, 200, 255);
    for (auto& s : recent_activity(14)) tx.line("  " + s.substr(0, 76), 190, 190, 190);
    tx.y = 360 - 6 - 4 * 10;
    tx.line("Keyboard: arrows stick (Left Shift: half)  X=A  Z=B  S=X  A=Y  Q/W=L/R  E/R=ZL/ZR", 140, 140, 150);
    tx.line("          Enter=Plus  Backspace/Right Shift=Minus  I/J/K/L d-pad", 140, 140, 150);
    tx.line("          numpad 8/4/2/6 right stick   F/G stick clicks   F11 fullscreen", 140, 140, 150);
}

// --screenshot FILE.png@SECONDS[,...]: save the window's contents (works with the dummy driver)
void maybe_screenshot() {
    std::string path;
    if (!next_shot(path)) return;
    SDL_Surface* s = SDL_RenderReadPixels(g_ren, nullptr);
    if (s && SDL_SavePNG(s, path.c_str())) log("screenshot saved to %s", path.c_str());
    else log("screenshot failed: %s", SDL_GetError());
    SDL_DestroySurface(s);
}

void show_fatal(const char* msg) {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && !strcmp(driver, "dummy")) return;                // invisible test runs: log only
#ifdef LETSGO_DIST
    std::string text = std::string(msg) + "\n\nDetails are in %APPDATA%\\LetsGoPikachu\\letsgo.log.";
#else
    std::string text = std::string(msg) + "\n\nDetails are in letsgo.log.";
#endif
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
    MessageBoxW(nullptr, w.c_str(), kDistBuild ? L"Pokémon: Let's Go, Pikachu!" : L"letsgo-pc stopped",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

}  // namespace

std::atomic<void*> g_hwnd{nullptr};

// audio output becomes available once SDL is up (the game starts first): 0 pending, 1 ready, -1 none
std::mutex g_audio_lock;
std::condition_variable g_audio_cv;
int g_audio_state = 0;
void set_audio_state(int s) {
    std::lock_guard<std::mutex> lk(g_audio_lock);
    g_audio_state = s;
    g_audio_cv.notify_all();
}

void platform_init(bool headless) {
    g_headless = headless;
    if (headless) {
        // silent, unless a test names an SDL audio driver (SDL_AUDIO_DRIVER=disk writes the output to a file)
        bool audio = getenv("SDL_AUDIO_DRIVER") && !getenv("LETSGO_MUTE") && SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (audio) log("audio: SDL driver %s", SDL_GetCurrentAudioDriver());
        set_audio_state(audio ? 1 : -1);
        return;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) fatal("SDL_Init failed: %s", SDL_GetError());
    if (getenv("LETSGO_MUTE") || !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (!getenv("LETSGO_MUTE")) log("audio: no output device (%s)", SDL_GetError());
        set_audio_state(-1);
    } else {
        log("audio: SDL driver %s", SDL_GetCurrentAudioDriver());
        set_audio_state(1);
    }
    const char* title = kDistBuild ? "Pok\xC3\xA9mon: Let's Go, Pikachu!" : "Pok\xC3\xA9mon: Let's Go, Pikachu! (letsgo-pc)";
    if (g_gpu) {
        // the game's frames are presented with Vulkan (gpu/device.cpp) from the render thread
        g_win = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_RESIZABLE);
        if (!g_win) fatal("could not create the window: %s", SDL_GetError());
        g_hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(g_win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else {
        if (!SDL_CreateWindowAndRenderer(title, 1280, 720, SDL_WINDOW_RESIZABLE, &g_win, &g_ren))
            fatal("could not create the window: %s", SDL_GetError());
        SDL_SetRenderVSync(g_ren, 1);
        SDL_SetRenderLogicalPresentation(g_ren, 640, 360, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
    set_fatal_hook(show_fatal);
    log("window: SDL %d.%d.%d, %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MICRO(SDL_GetVersion()), g_ren ? SDL_GetRendererName(g_ren) : "Vulkan presentation");
}

void* platform_hwnd() { return g_hwnd; }

bool platform_screenshot_due(std::string& path) { return next_shot(path); }

bool platform_save_png(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
    SDL_Surface* s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void*)rgba, (int)w * 4);
    bool ok = s && SDL_SavePNG(s, path.c_str());
    if (s) SDL_DestroySurface(s);
    log(ok ? "screenshot saved to %s" : "screenshot to %s failed", path.c_str());
    return ok;
}

void platform_screenshot(const std::string& spec) {
    std::lock_guard<std::mutex> lk(g_shot_lock);
    for (size_t pos = 0; pos <= spec.size();) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        std::string one = spec.substr(pos, end - pos);
        size_t at = one.rfind('@');
        bool frame = at != std::string::npos && one[at + 1] == 'f';
        if (!one.empty())
            g_shots.push_back({frame, at == std::string::npos ? 5.0 : atof(one.c_str() + at + 1 + frame), one.substr(0, at)});
        pos = end + 1;
    }
}

void* platform_audio_open(uint32_t rate, uint32_t channels) {
    {
        std::unique_lock<std::mutex> lk(g_audio_lock);
        g_audio_cv.wait_for(lk, std::chrono::seconds(10), [] { return g_audio_state != 0; });
        if (g_audio_state != 1) return nullptr;
    }
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, (int)channels, (int)rate};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s) {
        log("audio: could not open the output device: %s", SDL_GetError());
        return nullptr;
    }
    SDL_ResumeAudioStreamDevice(s);
    return s;
}
void platform_audio_put(void* stream, const void* data, uint32_t bytes) {
    SDL_PutAudioStreamData((SDL_AudioStream*)stream, data, (int)bytes);
}
uint32_t platform_audio_queued(void* stream) {
    int n = SDL_GetAudioStreamQueued((SDL_AudioStream*)stream);
    return n < 0 ? 0 : (uint32_t)n;
}
void platform_audio_pause(void* stream, bool paused) {
    if (paused) SDL_PauseAudioStreamDevice((SDL_AudioStream*)stream);
    else SDL_ResumeAudioStreamDevice((SDL_AudioStream*)stream);
}
void platform_audio_close(void* stream) { SDL_DestroyAudioStream((SDL_AudioStream*)stream); }

// ---- text entry for the software keyboard: a small modal dialog over the game window ------------
namespace {
struct TextDialog {
    std::wstring prompt, text;
    int max_len;
};
// LETSGO_DIALOG_TEST=prefix: each text box saves itself to prefix-N.png after a moment, then
// answers itself (its initial text, or "Test"), so a windowed test run needs no hands
std::atomic<int> g_dialog_count{0};
void dialog_self_test(HWND h) {
    const char* prefix = getenv("LETSGO_DIALOG_TEST");
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(h, mem, 2 /* PW_RENDERFULLCONTENT */);
    std::vector<uint8_t> rgba((size_t)w * ht * 4);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)w * ht; i++)
        rgba[i * 4] = src[i * 4 + 2], rgba[i * 4 + 1] = src[i * 4 + 1], rgba[i * 4 + 2] = src[i * 4], rgba[i * 4 + 3] = 255;
    platform_save_png(std::string(prefix) + "-" + std::to_string(++g_dialog_count) + ".png", (uint32_t)w, (uint32_t)ht, rgba.data());
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (GetWindowTextLengthW(GetDlgItem(h, 101)) == 0) SetDlgItemTextW(h, 101, L"Test");
    PostMessageW(h, WM_COMMAND, IDOK, 0);
}
INT_PTR CALLBACK text_dialog_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (TextDialog*)GetWindowLongPtrW(h, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        d = (TextDialog*)lp;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)d);
        SetDlgItemTextW(h, 100, d->prompt.c_str());
        SetDlgItemTextW(h, 101, d->text.c_str());
        SendDlgItemMessageW(h, 101, EM_SETLIMITTEXT, (WPARAM)d->max_len, 0);
        SendDlgItemMessageW(h, 101, EM_SETSEL, 0, -1);
        SetForegroundWindow(h);
        SetFocus(GetDlgItem(h, 101));
        if (getenv("LETSGO_DIALOG_TEST")) SetTimer(h, 1, 1500, nullptr);
        return FALSE;
    case WM_TIMER:
        KillTimer(h, 1);
        dialog_self_test(h);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = GetWindowTextLengthW(GetDlgItem(h, 101));
            std::wstring t((size_t)n + 1, L'\0');
            GetDlgItemTextW(h, 101, t.data(), n + 1);
            t.resize((size_t)n);
            d->text = t;
            EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(h, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
// an in-memory dialog template (DLGTEMPLATE + DLGITEMTEMPLATEs, WORD stream, items DWORD-aligned)
struct DialogTemplate {
    std::vector<WORD> v;
    void words(const void* p, size_t bytes) { v.insert(v.end(), (const WORD*)p, (const WORD*)p + bytes / 2); }
    void str(const wchar_t* s) {
        while (*s) v.push_back((WORD)*s++);
        v.push_back(0);
    }
    void dialog(int items, short cx, short cy, const wchar_t* title) {
        DLGTEMPLATE t{DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, (WORD)items, 0, 0, cx, cy};
        words(&t, sizeof t);
        v.push_back(0);                                // no menu
        v.push_back(0);                                // default class
        str(title);
        v.push_back(9);                                // DS_SETFONT: 9 pt Segoe UI
        str(L"Segoe UI");
    }
    void item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        if (v.size() & 1) v.push_back(0);
        DLGITEMTEMPLATE t{style | WS_CHILD | WS_VISIBLE, 0, x, y, cx, cy, id};
        words(&t, sizeof t);
        v.push_back(0xFFFF);
        v.push_back(cls);                              // 0x80 button, 0x81 edit, 0x82 static
        str(text);
        v.push_back(0);                                // no creation data
    }
};
const wchar_t* kTitle = L"Pokémon: Let's Go, Pikachu!";
}  // namespace

bool platform_headless() { return g_headless; }

bool platform_text_input(const std::u16string& prompt, const std::u16string& guide, std::u16string& text, int max_len) {
    TextDialog d{std::wstring(prompt.begin(), prompt.end()), std::wstring(text.begin(), text.end()), max_len};
    if (d.prompt.empty()) d.prompt = std::wstring(guide.begin(), guide.end());
    DialogTemplate t;
    t.dialog(4, 260, 78, kTitle);
    t.item(SS_LEFT, 7, 7, 246, 26, 100, 0x82, L"");
    t.item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 7, 36, 246, 14, 101, 0x81, L"");
    t.item(BS_DEFPUSHBUTTON | WS_TABSTOP, 149, 57, 50, 14, IDOK, 0x80, L"OK");
    t.item(BS_PUSHBUTTON | WS_TABSTOP, 203, 57, 50, 14, IDCANCEL, 0x80, L"Cancel");
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (const DLGTEMPLATE*)t.v.data(), (HWND)g_hwnd.load(),
                                        text_dialog_proc, (LPARAM)&d);
    if (r != 1) return false;
    text.assign(d.text.begin(), d.text.end());
    return true;
}

bool platform_message(const std::u16string& msg, bool yes_no) {
    std::wstring w(msg.begin(), msg.end());
    int r = MessageBoxW((HWND)g_hwnd.load(), w.c_str(), kTitle, (yes_no ? MB_YESNO : MB_OK) | MB_ICONINFORMATION);
    return r == IDOK || r == IDYES;
}

void platform_rumble(float low, float high) {
    g_rumble_lo = (uint32_t)std::lround(std::clamp(low, 0.0f, 1.0f) * 65535.0f);
    g_rumble_hi = (uint32_t)std::lround(std::clamp(high, 0.0f, 1.0f) * 65535.0f);
}

[[noreturn]] void platform_run(void* guest_thread) {
    if (g_headless) {
        WaitForSingleObject((HANDLE)guest_thread, INFINITE);
        log("main guest thread finished");
        ExitProcess(0);
    }
    for (;;) {
        uint64_t t0 = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                log("window closed");
                for (SDL_Gamepad* p : g_pads) SDL_RumbleGamepad(p, 0, 0, 0);
                // no SDL_Quit: it frees the audio streams while the audio thread may still be feeding
                // one (a crash in SDL_LockMutex on the way out); ending the process frees everything
                TerminateProcess(GetCurrentProcess(), 0);
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) open_pad(e.gdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED) close_pad(e.gdevice.which);
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
        }
        update_input();
        apply_rumble();
        if (WaitForSingleObject((HANDLE)guest_thread, 0) == WAIT_OBJECT_0) {
            log("main guest thread finished");
            for (SDL_Gamepad* p : g_pads) SDL_RumbleGamepad(p, 0, 0, 0);
            TerminateProcess(GetCurrentProcess(), 0);   // as above: other threads may still be in SDL
        }
        if (g_ren) {
            draw_status();
            maybe_screenshot();
            SDL_RenderPresent(g_ren);
        } else {
            char title[160];
            snprintf(title, sizeof title, "Pok\xC3\xA9mon: Let's Go, Pikachu! (letsgo-pc) - %llu frames", (unsigned long long)nvn_frames());
            static uint64_t last_title;
            if (SDL_GetTicks() - last_title > 1000) {
                SDL_SetWindowTitle(g_win, title);
                last_title = SDL_GetTicks();
            }
        }
        uint64_t dt = SDL_GetTicks() - t0;                   // no vsync (e.g. dummy video driver): cap at ~60 Hz
        if (dt < 15) SDL_Delay((Uint32)(16 - dt));
    }
}

}  // namespace rt
