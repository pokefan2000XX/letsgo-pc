// nn::hid overrides: one Handheld controller fed by the PC's keyboard and
// gamepads (platform.cpp), no other controllers, no Poke Ball Plus (Palma),
// motion sensors at rest, vibration forwarded to gamepad rumble.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle.h"

namespace rt {

namespace hid {
constexpr uint32_t kHandheld = 0x20;
constexpr uint32_t kStyleHandheld = 1u << 1;
constexpr uint32_t kResultPalmaNotConnected = 0x1A80CA;   // hid: generic error, Palma unavailable

static std::atomic<uint64_t> g_sampling{0};
static InputState g_host;                                 // keyboard + gamepads
static uint64_t g_script_buttons;                         // --press
static int32_t g_script_stick[2][2];                      // --press SL_* / SR_*: left, right stick
static std::mutex g_input_lock;

void set_host_input(const InputState& s) {
    std::lock_guard<std::mutex> lk(g_input_lock);
    g_host = s;
}
InputState current_input() {
    std::lock_guard<std::mutex> lk(g_input_lock);
    InputState s = g_host;
    s.buttons |= g_script_buttons;
    for (int k = 0; k < 2; k++) {
        int32_t* st = k ? s.stick_r : s.stick_l;
        if (g_script_stick[k][0] || g_script_stick[k][1]) st[0] = g_script_stick[k][0], st[1] = g_script_stick[k][1];
    }
    return s;
}

// hid also reports each stick pushed past half way as a direction button
// (NpadButton bits 16-23: StickL left/up/right/down, then StickR)
static uint64_t stick_buttons(const int32_t l[2], const int32_t r[2]) {
    const int32_t t = 0x4000;
    uint64_t b = 0;
    if (l[0] < -t) b |= 1ull << 16;
    if (l[1] > t) b |= 1ull << 17;
    if (l[0] > t) b |= 1ull << 18;
    if (l[1] < -t) b |= 1ull << 19;
    if (r[0] < -t) b |= 1ull << 20;
    if (r[1] > t) b |= 1ull << 21;
    if (r[0] > t) b |= 1ull << 22;
    if (r[1] < -t) b |= 1ull << 23;
    return b;
}

static uint32_t npad_id(uint64_t ref) { return *(const uint32_t*)(uintptr_t)ref; }

// GetNpadStyleSet(uint32 const& id) -> NpadStyleSet
static void trace_query(const char* what, uint32_t id);
static void GetNpadStyleSet(Ctx* c) {
    trace_query("style set", npad_id(c->x[0]));
    c->x[0] = npad_id(c->x[0]) == kHandheld ? kStyleHandheld : 0;
}

// Npad states come from a history of samples, as hid keeps one per controller: a new sample when
// the sampling period (about 4 ms) has passed since the last one, taken when the game asks.
// GetNpadStates hands out the newest ones first; games find presses by comparing successive
// samples, so the history must hold the input as it was at each sample.
struct Sample {
    uint64_t number = 0, buttons = 0;
    int32_t l[2] = {0, 0}, r[2] = {0, 0};
};
constexpr int kHistory = 32;                              // hid keeps 17 per npad; NpadStateCountMax is 16
static Sample g_hist[kHistory];
static uint64_t g_samples = 0;                            // samples taken
static std::chrono::steady_clock::time_point g_last_sample;
static std::mutex g_hist_lock;

// takes a sample if the period has passed (g_hist_lock held); returns how many are kept. At 60 frames a second, on the
// frames the game's every-other-frame counts skip (framerate.cpp) the input stays as it was, so a press starts (and ends) on
// a frame they run in and none of them misses it
static int sample_now() {
    auto now = std::chrono::steady_clock::now();
    if (!g_samples || now - g_last_sample >= std::chrono::milliseconds(4)) {
        InputState in = current_input();
        Sample& s = g_hist[g_samples % kHistory];
        if (g_samples && fps60_odd_frame()) {
            const Sample& was = g_hist[(g_samples - 1) % kHistory];
            s.buttons = was.buttons;
            s.l[0] = was.l[0], s.l[1] = was.l[1], s.r[0] = was.r[0], s.r[1] = was.r[1];
        } else {
            s.buttons = in.buttons | stick_buttons(in.stick_l, in.stick_r);
            s.l[0] = in.stick_l[0], s.l[1] = in.stick_l[1], s.r[0] = in.stick_r[0], s.r[1] = in.stick_r[1];
        }
        s.number = ++g_samples;
        g_last_sample = now;
    }
    return (int)std::min<uint64_t>(g_samples, kHistory);
}
// NpadXxxState (0x28 bytes): sampling number, buttons, left stick, right stick, attributes
static void write_state(uint8_t* out, const Sample& s, bool connected) {
    memset(out, 0, 40);
    memcpy(out, &s.number, 8);
    if (!connected) return;
    memcpy(out + 8, &s.buttons, 8);
    memcpy(out + 16, s.l, 8);
    memcpy(out + 24, s.r, 8);
    uint32_t attr = 1 | 4 | 16;                           // connected, left and right Joy-Con attached
    memcpy(out + 32, &attr, 4);
}
// GetNpadState(NpadXxxState* out, uint32 const& id); only the Handheld style reports a controller
static void fill_state(uint64_t out, bool connected) {
    std::lock_guard<std::mutex> lk(g_hist_lock);
    sample_now();
    write_state((uint8_t*)(uintptr_t)out, g_hist[(g_samples - 1) % kHistory], connected);
}
// GetNpadStates(NpadXxxState* out, int count, uint32 const& id) -> states written, newest first
static void fill_states(Ctx* c, bool connected) {
    std::lock_guard<std::mutex> lk(g_hist_lock);
    int n = std::min((int)c->x[1], sample_now());
    uint8_t* out = (uint8_t*)(uintptr_t)c->x[0];
    for (int i = 0; i < n; i++) write_state(out + 40 * i, g_hist[(g_samples - 1 - (uint64_t)i) % kHistory], connected);
    c->x[0] = (uint64_t)std::max(n, 0);
}
static void trace_query(const char* what, uint32_t id) {
    static std::mutex m;
    static std::vector<std::pair<const char*, uint32_t>> seen;
    if (!g_verbose) return;
    std::lock_guard<std::mutex> lk(m);
    for (auto& e : seen)
        if (e.first == what && e.second == id) return;
    seen.push_back({what, id});
    log("hid: first %s query for npad 0x%x", what, id);
}
static void GetNpadStateHandheld(Ctx* c) { trace_query("Handheld state", npad_id(c->x[1])); fill_state(c->x[0], npad_id(c->x[1]) == kHandheld); }
static void GetNpadStateOther(Ctx* c) { trace_query("non-handheld state", npad_id(c->x[1])); fill_state(c->x[0], false); }
static void GetNpadStatesHandheld(Ctx* c) { trace_query("Handheld states", npad_id(c->x[2])); fill_states(c, npad_id(c->x[2]) == kHandheld); }
static void GetNpadStatesOther(Ctx* c) { trace_query("non-handheld states", npad_id(c->x[2])); fill_states(c, false); }

// BindNpadStyleSetUpdateEvent(uint32 const& id, SystemEventType*, EventClearMode). hid signals
// the event as soon as it is bound: games read their initial controller set when it fires
static std::unordered_map<uint32_t, uint32_t> g_style_events;     // npad id -> writable handle
static void BindNpadStyleSetUpdateEvent(Ctx* c) {
    uint32_t id = npad_id(c->x[0]);
    uint32_t w = attach_new_system_event(c, c->x[1], (int)c->x[2]);
    {
        std::lock_guard<std::mutex> lk(g_input_lock);
        g_style_events[id] = w;
    }
    kevent_signal(w);
    RT_TRACE("hid: style set update event bound for npad 0x%x", id);
}

// six-axis: handles 0..n-1, sensors at rest with zero motion
static void GetSixAxisSensorHandles(Ctx* c) {
    uint32_t* h = (uint32_t*)(uintptr_t)c->x[0];
    int n = (int)c->x[1];
    for (int i = 0; i < n; i++) h[i] = 0x100 + (uint32_t)i;
    c->x[0] = (uint64_t)n;
}
static void GetSixAxisSensorState(Ctx* c) {
    uint8_t* s = (uint8_t*)(uintptr_t)c->x[0];
    memset(s, 0, 0x60);
    uint64_t n = ++g_sampling, dt = 5000000;              // 5 ms
    memcpy(s, &dt, 8);
    memcpy(s + 8, &n, 8);
    float one = 1.0f;                                     // identity direction matrix
    memcpy(s + 0x30, &one, 4);
    memcpy(s + 0x40, &one, 4);
    memcpy(s + 0x50, &one, 4);
}
static void GetSixAxisSensorStates(Ctx* c) { c->x[0] = 0; }   // returns number of states written
static void IsSixAxisSensorAtRest(Ctx* c) { c->x[0] = 1; }
static void GetSixAxisSensorFusionParameters(Ctx* c) {
    *(float*)(uintptr_t)c->x[0] = 0.03f;
    *(float*)(uintptr_t)c->x[1] = 0.4f;
}
static void GetVibrationDeviceHandles(Ctx* c) {
    uint32_t* h = (uint32_t*)(uintptr_t)c->x[0];
    int n = (int)c->x[1];
    for (int i = 0; i < n; i++) h[i] = 0x200 + (uint32_t)i;
    c->x[0] = (uint64_t)n;
}
static void GetGestureStates(Ctx* c) { c->x[0] = 0; }
// GetTouchScreenStates<16>(TouchScreenState<16>* out, int count) -> states written; no touches
static void GetTouchScreenStates16(Ctx* c) {
    if ((int)c->x[1] < 1) { c->x[0] = 0; return; }
    uint8_t* s = (uint8_t*)(uintptr_t)c->x[0];
    memset(s, 0, 0x10 + 16 * 0x28);
    uint64_t n = ++g_sampling;
    memcpy(s, &n, 8);
    c->x[0] = 1;
}

// scripted input for headless runs: "--press A@8,PLUS@12.5" taps each button for 150 ms
// at the given number of seconds after start
// --press BUTTON@WHEN[:HOLD][,...]: WHEN is seconds since start, or fN for "once frame N is
// presented" (robust against slow starts); HOLD is how long to hold it (seconds, default 0.15).
// SL_UP/SL_DOWN/SL_LEFT/SL_RIGHT (and SR_*) push the left (right) stick all the way.
// Entries run in the order given.
static const struct { const char* name; int bit; } kButtons[] = {
    {"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"LSTICK", 4}, {"RSTICK", 5}, {"L", 6}, {"R", 7}, {"ZL", 8}, {"ZR", 9},
    {"PLUS", 10}, {"MINUS", 11}, {"LEFT", 12}, {"UP", 13}, {"RIGHT", 14}, {"DOWN", 15},
    // sticks: 100 + stick * 4 + direction (left, up, right, down)
    {"SL_LEFT", 100}, {"SL_UP", 101}, {"SL_RIGHT", 102}, {"SL_DOWN", 103},
    {"SR_LEFT", 104}, {"SR_UP", 105}, {"SR_RIGHT", 106}, {"SR_DOWN", 107}};
static int button_bit(const std::string& name) {
    for (auto& b : kButtons)
        if (name == b.name) return b.bit;
    return -1;
}
static const char* button_name(int bit) {
    for (auto& b : kButtons)
        if (b.bit == bit) return b.name;
    return "?";
}
// holds a button (or pushes a stick) for `hold` seconds
static void tap(int bit, double hold) {
    log("input: press %s", button_name(bit));
    auto set = [&](bool down) {
        std::lock_guard<std::mutex> lk(g_input_lock);
        if (bit < 100) {
            if (down) g_script_buttons |= 1ull << bit;
            else g_script_buttons &= ~(1ull << bit);
            return;
        }
        static const int32_t dir[4][2] = {{-32767, 0}, {0, 32767}, {32767, 0}, {0, -32767}};
        int32_t* st = g_script_stick[(bit - 100) / 4];
        st[0] = down ? dir[(bit - 100) % 4][0] : 0;
        st[1] = down ? dir[(bit - 100) % 4][1] : 0;
    };
    set(true);
    std::this_thread::sleep_for(std::chrono::duration<double>(hold));
    set(false);
}
void start_input_script(const std::string& spec) {
    struct Tap { bool frame; double when, hold; int bit; };
    std::vector<Tap> taps;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? spec.size() : end + 1;
        size_t at = item.find('@');
        int bit = button_bit(item.substr(0, at));
        if (bit < 0 || at == std::string::npos) fatal("--press: bad entry \"%s\" (use BUTTON@SECONDS or BUTTON@fFRAME)", item.c_str());
        bool frame = item[at + 1] == 'f';
        size_t colon = item.find(':', at);
        double hold = colon == std::string::npos ? 0.15 : atof(item.c_str() + colon + 1);
        taps.push_back({frame, atof(item.c_str() + at + 1 + frame), hold, bit});
    }
    std::thread([taps] {
        auto t0 = std::chrono::steady_clock::now();
        for (auto& t : taps) {
            if (t.frame)
                while ((double)nvn_frames() < t.when) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            else
                std::this_thread::sleep_until(t0 + std::chrono::duration<double>(t.when));
            tap(t.bit, t.hold);
        }
    }).detach();
}

// LETSGO_CONTROL=file: drives a running game from lines appended to `file`, in order:
//   A, SL_UP:1.5      tap a button / push a stick (for HOLD seconds)
//   wait 2            pause the script
//   shot out.png      save the next presented frame
//   burst 8 out       save the next 8 presented frames, one after another (out_01.png ...)
//   scan 1930 0 920   log where the heap holds those three floats in a row (a position, say)
//   trace 0x...,0x... 120   log the three floats at each address once a presented frame, for 120 frames (alongside
//                     what comes next, so a stick pushed after it is traced)
//   watch 0x... 3     for 3 seconds (alongside what comes next), which guest functions write the 4 bytes at an address
//   turbo on|off      present as fast as possible, or at the normal rate
//   calls out.tsv     with LETSGO_CALLS=1, each main function's indirect calls since the last "calls"
//   peek 0x...,0x... 64   log that many bytes at each address as 64-bit words (up to 512)
//   slow 30           every present takes 30 ms longer, as on a slow GPU (0 ends it)
//   fps 60, fps 30    switch the frame rate, as Tab does
//   frames            log how many frames have been presented and how many the game has begun
void start_control_file(const std::string& path) {
    std::thread([path] {
        size_t done = 0;
        std::string pending;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            if (size < (long)done) done = 0, pending.clear();          // the file was replaced
            std::string add((size_t)std::max<long>(size - (long)done, 0), '\0');
            fseek(f, (long)done, SEEK_SET);
            add.resize(fread(add.data(), 1, add.size(), f));
            fclose(f);
            done += add.size();
            pending += add;
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                if (line.rfind("shot ", 0) == 0) {
                    char when[32];
                    snprintf(when, sizeof when, "@%.3f", uptime());
                    platform_screenshot(line.substr(5) + when);
                } else if (line.rfind("burst ", 0) == 0 && line.find(' ', 6) != std::string::npos) {
                    int n = atoi(line.c_str() + 6);
                    std::string prefix = line.substr(line.find(' ', 6) + 1), spec;
                    uint64_t from = nvn_frames();
                    for (int k = 1; k <= n; k++) {
                        char one[32];
                        snprintf(one, sizeof one, "_%02d.png@f%llu", k, (unsigned long long)(from + k));
                        spec += (k > 1 ? "," : "") + prefix + one;
                    }
                    platform_screenshot(spec);
                } else if (line.rfind("scan ", 0) == 0) {
                    // three floats in a row; a * matches any value there ("scan 6657.6 * 188.64")
                    float want[3] = {};
                    bool any[3] = {};
                    char word[3][32] = {};
                    sscanf(line.c_str() + 5, "%31s %31s %31s", word[0], word[1], word[2]);
                    for (int k = 0; k < 3; k++) any[k] = word[k][0] == '*', want[k] = (float)atof(word[k]);
                    auto near = [&](const float* q) {
                        for (int k = 0; k < 3; k++)
                            if (!any[k] && !(std::fabs(q[k] - want[k]) < 0.01f)) return false;
                        return true;
                    };
                    size_t found = 0;
                    // the heap's readable stretches only (it has holes the game unmapped)
                    for (uint64_t a = kHeapBase, end = kHeapBase + mem_heap_size(); a < end && found < 64;) {
                        uint64_t span = mem_readable(a);
                        if (!span) { a = (a | 0xfff) + 1; continue; }
                        span = std::min(span, end - a);
                        const float* p = (const float*)(uintptr_t)a;
                        for (size_t i = 0, n = (size_t)(span / 4); i + 2 < n && found < 64; i++)
                            if (near(p + i))
                                log("control: scan %s at 0x%llx", line.c_str() + 5, (unsigned long long)(uintptr_t)(p + i)), found++;
                        a += span;
                    }
                    log("control: scan found %zu", found);
                } else if (line.rfind("trace ", 0) == 0) {
                    std::vector<const float*> at;
                    for (const char* s = line.c_str() + 6; *s && *s != ' ';) {
                        char* end;
                        at.push_back((const float*)(uintptr_t)strtoull(s, &end, 0));
                        s = *end == ',' ? end + 1 : end;
                    }
                    int frames = atoi(line.c_str() + line.rfind(' ') + 1);
                    std::thread([at, frames] {
                        for (uint64_t f = nvn_frames(), end = f + (uint64_t)frames; f < end;) {
                            uint64_t now = nvn_frames();
                            if (now == f) { std::this_thread::sleep_for(std::chrono::microseconds(500)); continue; }
                            f = now;
                            std::string row;
                            for (const float* p : at) {
                                char one[64];
                                snprintf(one, sizeof one, " | %.3f %.3f %.3f", p[0], p[1], p[2]);
                                row += one;
                            }
                            log("control: trace frame %llu%s", (unsigned long long)f, row.c_str());
                        }
                    }).detach();
                } else if (line.rfind("watch ", 0) == 0) {
                    uint64_t addr = strtoull(line.c_str() + 6, nullptr, 0);
                    double seconds = atof(line.c_str() + line.rfind(' ') + 1);
                    std::thread([addr, seconds] { watch_writes(addr, seconds); }).detach();
                } else if (line.rfind("wait ", 0) == 0) {
                    std::this_thread::sleep_for(std::chrono::duration<double>(atof(line.c_str() + 5)));
                } else if (line.rfind("calls ", 0) == 0) {
                    dump_call_counts(line.substr(6));
                } else if (line == "turbo on" || line == "turbo off") {
                    nvn_set_turbo(line == "turbo on");
                    log("control: %s", line.c_str());
                } else if (line.rfind("peek ", 0) == 0) {
                    size_t bytes = std::min<size_t>(strtoull(line.c_str() + line.rfind(' ') + 1, nullptr, 0), 512);
                    for (const char* s = line.c_str() + 5; *s && *s != ' ';) {
                        char* end;
                        uint64_t a = strtoull(s, &end, 0) & ~7ull;
                        s = *end == ',' ? end + 1 : end;
                        std::string row;
                        for (size_t o = 0; o < bytes && mem_readable(a + o) >= 8; o += 8) {
                            char one[24];
                            snprintf(one, sizeof one, " %016llx", (unsigned long long)*(const uint64_t*)(uintptr_t)(a + o));
                            row += one;
                        }
                        log("control: peek 0x%llx:%s", (unsigned long long)a, row.c_str());
                    }
                } else if (line.rfind("slow ", 0) == 0) {
                    nvn_set_slow(atoi(line.c_str() + 5));
                    log("control: %s", line.c_str());
                } else if (line == "fps 60" || line == "fps 30") {
                    if (fps60() != (line == "fps 60")) toggle_fps60();
                } else if (line == "frames") {
                    log("control: frames presented %llu, begun %llu", (unsigned long long)nvn_frames(),
                        (unsigned long long)fps60_frame());
                } else {
                    size_t colon = line.find(':');
                    int bit = button_bit(line.substr(0, colon));
                    if (bit < 0) log("control: unknown command \"%s\"", line.c_str());
                    else tap(bit, colon == std::string::npos ? 0.15 : atof(line.c_str() + colon + 1));
                }
            }
        }
    }).detach();
}
static void palma_error(Ctx* c) { c->x[0] = kResultPalmaNotConnected; }

// detail::SendVibrationValue(s): VibrationValue {amp low, freq low, amp high, freq high} per
// device; the Handheld's two motors (handles 0x200, 0x201) combine into the gamepad's
// low- and high-frequency rumble
struct VibrationValue { float amp_low, freq_low, amp_high, freq_high; };
static void vibrate(uint32_t handle, const VibrationValue& v) {
    static std::mutex m;
    static float amp[2][2];
    std::lock_guard<std::mutex> lk(m);
    amp[handle & 1][0] = v.amp_low;
    amp[handle & 1][1] = v.amp_high;
    platform_rumble(std::max(amp[0][0], amp[1][0]), std::max(amp[0][1], amp[1][1]));
}
static void SendVibrationValue(Ctx* c) {
    vibrate(*(const uint32_t*)(uintptr_t)c->x[0], *(const VibrationValue*)(uintptr_t)c->x[1]);
    c->x[0] = 0;
}
static void SendVibrationValues(Ctx* c) {
    const uint32_t* h = (const uint32_t*)(uintptr_t)c->x[0];
    const VibrationValue* v = (const VibrationValue*)(uintptr_t)c->x[1];
    for (int i = 0; i < (int)c->x[2]; i++) vibrate(h[i], v[i]);
    c->x[0] = 0;
}
}  // namespace hid

void register_hid_overrides(void (*reg)(const char*, rc_fn)) {
    using namespace hid;
    for (const char* s : {"_ZN2nn3hid14InitializeNpadEv", "_ZN2nn3hid18InitializeDebugPadEv", "_ZN2nn3hid17InitializeGestureEv",
                          "_ZN2nn3hid21InitializeTouchScreenEv",
                          "_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE",
                          "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
                          "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",
                          "_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",
                          "_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj",
                          "_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE",
                          "_ZN2nn3hid14DisconnectNpadERKj", "_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE",
                          "_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE",
                          "_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb",
                          "_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff",
                          "_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",
                          "_ZN2nn3hid24EnableAnyPalmaConnectionEv", "_ZN2nn3hid25DisableAnyPalmaConnectionEv",
                          "_ZN2nn3hid17SetPalmaBoostModeEb"})
        reg(s, hle_ok);
    reg("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
    reg("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadStateHandheld);
    reg("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadStateOther);
    reg("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadStateOther);
    reg("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", GetNpadStateOther);
    reg("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", GetNpadStateOther);
    reg("_ZN2nn3hid12GetNpadStateEPNS0_14NpadPalmaStateERKj", GetNpadStateOther);
    reg("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", GetNpadStatesHandheld);
    reg("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", GetNpadStatesOther);
    reg("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", GetNpadStatesOther);
    reg("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", GetNpadStatesOther);
    reg("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", GetNpadStatesOther);
    reg("_ZN2nn3hid13GetNpadStatesEPNS0_14NpadPalmaStateEiRKj", GetNpadStatesOther);
    reg("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
    reg("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetSixAxisSensorHandles);
    reg("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", GetSixAxisSensorState);
    reg("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", GetSixAxisSensorStates);
    reg("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", IsSixAxisSensorAtRest);
    reg("_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", hle_ok);
    reg("_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", GetSixAxisSensorFusionParameters);
    reg("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", hle_ok);
    reg("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetVibrationDeviceHandles);
    reg("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", GetGestureStates);
    reg("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", GetTouchScreenStates16);
    for (const char* s : {"_ZN2nn3hid24GetPalmaConnectionHandleEPNS0_21PalmaConnectionHandleERKj",
                          "_ZN2nn3hid15InitializePalmaERKNS0_21PalmaConnectionHandleE",
                          "_ZN2nn3hid9PairPalmaERKNS0_21PalmaConnectionHandleE"})
        reg(s, palma_error);
    reg("_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", SendVibrationValue);
    reg("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", SendVibrationValues);
}

}  // namespace rt
