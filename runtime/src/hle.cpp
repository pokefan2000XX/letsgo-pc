// Host overrides: nn:: (and other) functions that are replaced by host code
// instead of running the recompiled sdk implementation (anything that would
// talk to a Horizon service over IPC). Registered by mangled symbol name.
#include <unordered_map>

#include "hle.h"

namespace rt {

static std::unordered_map<std::string, rc_fn> g_over;

static void reg(const char* sym, rc_fn fn) { g_over[sym] = fn; }

std::vector<std::string> override_names() {
    std::vector<std::string> v;
    for (auto& kv : g_over) v.push_back(kv.first);
    return v;
}

uint64_t guest_call(Ctx* c, uint64_t fn, std::initializer_list<uint64_t> args) {
    if (!fn) fatal("guest_call: function not found");
    uint64_t lr = c->x[30];
    int i = 0;
    for (uint64_t a : args) c->x[i++] = a;
    c->x[30] = 0;
    rc_lookup(fn)(c);
    c->x[30] = lr;
    return c->x[0];
}

uint32_t attach_new_system_event(Ctx* c, uint64_t event, int clear_mode) {
    static uint64_t attach = find_symbol("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    uint32_t readable = 0, writable = kevent_create(&readable);
    guest_call(c, attach, {event, readable, 1, (uint64_t)clear_mode});
    return writable;
}

rc_fn lookup_override(const std::string& sym) {
    auto it = g_over.find(sym);
    return it == g_over.end() ? nullptr : it->second;
}

// ---- generic -----------------------------------------------------------------------------
static void ret_void(Ctx* c) { (void)c; }
static void ret_success(Ctx* c) { c->x[0] = 0; }

void register_overrides() {
    // SDK module initialization (nninitInitializeSdkModule): service sessions we don't need
    reg("_ZN2nn2oe10InitializeEv", ret_void);                    // nn::oe::Initialize
    reg("_ZN2nn3aoc10InitializeEv", ret_void);                   // nn::aoc::Initialize
    reg("_ZN2nn2fs24InitializeForApplicationEv", ret_void);      // nn::fs::InitializeForApplication
    reg("_ZN2nn4pctl10InitializeEv", ret_void);                  // nn::pctl::Initialize
    reg("_ZN2nn2lm10InitializeEv", ret_void);                    // nn::lm::Initialize (log manager service)
    (void)ret_success;
    register_sys_overrides(reg);
    register_nvn_overrides(reg);
    register_hid_overrides(reg);
    register_audio_overrides(reg);
    register_net_overrides(reg);
    register_movie_overrides(reg);
    register_applet_overrides(reg);
}

}  // namespace rt
