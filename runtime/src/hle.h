// Shared helpers for host override implementations (hle_*.cpp).
#pragma once
#include <initializer_list>

#include "rt.h"

namespace rt {
inline void hle_void(Ctx* c) { (void)c; }
inline void hle_ok(Ctx* c) { c->x[0] = 0; }
uint64_t guest_alloc(uint64_t size);       // zeroed, guest-visible memory owned by the host
void register_sys_overrides(void (*reg)(const char*, rc_fn));
void register_nvn_overrides(void (*reg)(const char*, rc_fn));
void register_hid_overrides(void (*reg)(const char*, rc_fn));
void register_audio_overrides(void (*reg)(const char*, rc_fn));
void register_net_overrides(void (*reg)(const char*, rc_fn));
void register_movie_overrides(void (*reg)(const char*, rc_fn));
void register_applet_overrides(void (*reg)(const char*, rc_fn));
namespace fsx {
extern std::string g_orig, g_save;                  // game data dir, save data dir
bool read_whole_file(const char* guest_path, std::vector<uint8_t>* out);   // RomFS or host file
std::string host_path_of(const char* guest);        // "rom:/a/b" -> host path ("" if not mounted)
}
// call a guest function from a host override (clobbers caller-saved registers like any call)
uint64_t guest_call(Ctx* c, uint64_t fn, std::initializer_list<uint64_t> args);
// nn::os::SystemEventType backed by a new kernel event (as a service would hand out);
// returns the writable handle for kevent_signal. clear_mode: 0 manual, 1 auto
uint32_t attach_new_system_event(Ctx* c, uint64_t event, int clear_mode);
namespace hid {
// NpadButton bits and sticks in Switch units (+-32767, y up)
struct InputState { uint64_t buttons = 0; int32_t stick_l[2] = {0, 0}, stick_r[2] = {0, 0}; };
void set_host_input(const InputState& s);           // keyboard + gamepads, from the platform layer
InputState current_input();                         // what the game reads (host input + --press)
void start_input_script(const std::string& spec);   // --press A@8,PLUS@12.5
void start_control_file(const std::string& path);   // LETSGO_CONTROL: commands appended to a file
}
}  // namespace rt
