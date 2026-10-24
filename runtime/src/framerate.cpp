// Frame rate (M6): the game at 60 frames a second as well as its own 30, switched with Tab.
//
// The game steps its world by a frame timer, not by frames. The application object (vtable
// 0x7101527428) keeps one at app+0x110 (app+0x138 points to it) with two slots, each a period in
// nanoseconds taken from the table at 0x7100f7eeb8 (60, 30, 20, 15 and 12 frames a second). Slot 1 is
// the world's step (the field's movement; 0x710067bcc0 steps an animation by slot 1 / 16.67 ms), slot 0
// the display's: the render task (0x7100029d30) presents at an interval of slot 0 / 16.67 ms. The
// engine's own methods that set them pick a period from the frame's measured time (0x71000393e0,
// 0x7100039520); Let's Go replaces them with two that always pick 30 (0x7100007af0 sets slot 1 every
// frame, 0x7100007b10 slot 0). The ones here pick 60 or 30, so at 60 the game draws twice as often and
// moves at the same speed.
//
// Not everything steps by the timer. The message printer steps once a frame: each frame 0x71001b09a0 (a
// window's update, through its vtable) counts down a wait (+0x1a4) and runs the printer (0x7100678f10),
// whose state (+0xa8) 4 prints [+0xa4] characters a frame with [+0x9d] frames between them, and whose
// state 0 with mode (+0x90) 2 counts frames to go on by itself (+0xa3 up to +0xa2). At 60 that runs only
// every other frame, so text prints and waits at the game's speed; its other states (waiting for a
// button) run every frame. Scripts count frames too: a script's Wait(n) is n sleeps of a frame each, and
// its polls (FadeWait_, IsMsgWinEnd_, ...) come once a frame, so at 60 a script runs (0x7100674d40, every
// script's run through the vtables) only every other frame, and so does a screen fade's step (0x710011fe10,
// FadeOut_'s and FadeIn_'s frame counts and every menu's fades). A field character's move action (vtable 0x7101542790)
// steps it along its facing by a frame's worth (+0x6c and +0x70 walking, +0x74 and +0x78 running: -0.5, -2, -2 and -3.5) and
// its walk or run animation's root motion takes it on by the clock, so at 60 those steps are halved (without that the player
// ran at 331 units a second, not 391). At 60 the input the game sees changes only on the frames those every-other-frame
// counts run (hle_hid.cpp), so a press's first frame is never one they skip. Event camera moves and scripted turns take
// their length in frames (EvCameraMove's 1 / frames a frame, StartTurnAround_'s frames at its command's +0x9c), so at 60
// those lengths are doubled as the command starts: as long as at 30 (the opening's pans took 1.0 s, not 1.6), still
// moving every frame.
//
// A battle (the battle camera's update, 0x7100a95f50, ran in the last two frames) counts frames in its update
// (0x7100a7cce0, once a frame): its sequences (0x7100aa0bc0 steps the two 0x7100210510 play), its task groups
// (0x7100b0df10: each task's +0x38, given 1, a frame), its flow (0x7100a7cff0, 0x7100a7d140, 0x7100a7d9a0,
// 0x7100a7dd40, 0x7100a8e5a0) and its +0x420 and +0x4a8 (0x71000b8490, 0x7100aa3350). At 60 those run every other
// frame and the rest of the update every frame: the camera controller (0x7100ad6ba0) and the +0x488 list
// (0x7100a9efe0, the models), which step by the clock, so the battle draws 60 times a second at its own speed
// (headless 2026-10-24, a trainer battle: a move, the faint, the EXP messages and the trainer's return within
// 0.1 s of 30's, frame-to-frame motion even, no crash through three trainer battles). Held back
// together they drew the battle at 30 (the camera and models every other frame) and slowed what steps by the clock;
// the sequences alone held back crashed at the send-out (a camera command, 0x7100ae3630, found its object gone:
// the tasks ran ahead).
//
// The choice is kept in settings.ini beside the saves (LETSGO_FPS=60 or 30 sets it for one run).
#include <atomic>
#include <cstdlib>
#include <string>

#include "rt.h"

namespace rt {
namespace {

constexpr uint64_t kPeriods = 0x7100f7eeb8;          // int64 nanoseconds: 60, 30, 20, 15, 12 frames a second
constexpr uint64_t kWorldPeriod = 0x7100007af0;      // app vtable +0x20: the timer's slot 1
constexpr uint64_t kDisplayPeriod = 0x7100007b10;    // app vtable +0x28: the timer's slot 0
constexpr uint64_t kMessageUpdate = 0x71001b09a0;    // a message window's update, once a frame
constexpr uint64_t kBattleCamera = 0x7100a95f50;     // the battle camera's update, once a frame in battle
constexpr uint64_t kScriptRun = 0x7100674d40;        // a script's run, or its going on after halt 12 (vtables)
constexpr uint64_t kFadeUpdate = 0x710011fe10;       // a screen fade's update (its vtable +0x28), once a frame
constexpr uint64_t kFadeUpdateEnd = 0x7100120370;    // the update's last part, which it ends in (timed by the clock)
constexpr uint64_t kMoveAction = 0x710021b5c0;       // a field character's move action's update (its vtable +0x38), once a frame
// script commands that take a length in frames (the handler's x1 points at its argument cells, 8 bytes each from +8)
struct FramesArg {
    uint64_t handler;
    uint32_t offset;                                 // the frames cell
};
constexpr FramesArg kFramesArgs[] = {
    {0x7100180dd0, 0x08},                            // EvCameraMove(frames, position, target, ease, fov, roll)
    {0x71001809d0, 0x08},                            // EvCameraEnd(frames, ease)
    {0x7100180f00, 0x08},                            // EvCameraMoveOffset(frames, offset, ease, ...)
    {0x7100181010, 0x08},                            // EvCameraMoveOffsetLookAt(frames, offset, ease, ...)
    {0x7100186bd0, 0x18},                            // StartTurnAround_(obj, angle, frames)
    {0x7100186c10, 0x18},                            // StartTurnAroundToTargetObject_(obj, target, frames)
    {0x7100186c40, 0x20},                            // StartTurnAroundToTargetPosition_(obj, x, z, frames)
};

std::atomic<bool> g_fps60{false};
bool g_keep = true;                                  // Tab's choice is written to settings.ini
std::atomic<uint64_t> g_frame{0};                    // frames the game has begun
std::atomic<uint64_t> g_battle_frame{~0ull - 8};     // the last one a battle ran in

uint64_t& m64(uint64_t a) { return *(uint64_t*)(uintptr_t)a; }
uint32_t& m32(uint64_t a) { return *(uint32_t*)(uintptr_t)a; }

bool in_battle() { return g_frame - g_battle_frame <= 2; }
bool at60() { return g_fps60; }
// at 60, the frames a once-a-frame count skips (every other one)
bool gate() { return at60() && (g_frame & 1); }

}  // namespace

// at 60 in a battle, the frames its frame-counted systems skip
bool battle_gate() { return in_battle() && gate(); }

namespace {

// the timer's slot set to the period the frame rate asks for, as the timer's own setter (0x7100039620)
// writes it: slot n's period is at timer + 0x10 + 0x10 * n
void set_slot(uint64_t app, int slot) {
    uint64_t timer = m64(app + 0x138);
    m64(timer + 0x10 + 0x10 * (uint64_t)slot) = m64(kPeriods + 8 * (at60() ? 0 : 1));
}
// both slots every frame, so a switch shows from the next frame on; it's called once at each frame's start
void world_period(Ctx* c) {
    g_frame++;
    set_slot(c->x[0], 1);
    set_slot(c->x[0], 0);
}
void display_period(Ctx* c) { set_slot(c->x[0], 0); }

void call_original(uint64_t addr, Ctx* c) { rc_lookup(kOriginalsBase + (addr - kMainBase))(c); }

// at 60, printing and counting frames only every other frame (returning 1, "going on", as those states do)
void message_update(Ctx* c) {
    uint64_t w = c->x[0];
    bool counting = m32(w + 0xa8) == 4 || m32(w + 0x1a4) != 0 || (m32(w + 0xa8) == 0 && m32(w + 0x90) == 2);
    if (gate() && counting) {
        c->x[0] = 1;
        return;
    }
    call_original(kMessageUpdate, c);
}

// at 60, a script runs (or goes on after a wait) only every other frame; the frames between answer
// 12, "sleeping", as a waiting script does, and it goes on the next frame
void script_run(Ctx* c) {
    if (gate()) {
        c->x[0] = 12;
        return;
    }
    call_original(kScriptRun, c);
}

// at 60, a fade's frames (+0x2a8 counting up to its length, +0x2ac) and colour steps come every other
// frame; the update's last part runs every frame
void fade_update(Ctx* c) {
    call_original(gate() ? kFadeUpdateEnd : kFadeUpdate, c);
}

// at 60, a move action's steps (a frame's worth each) halved while it updates; a step it sets itself is kept
void move_action(Ctx* c) {
    float* step = (float*)(uintptr_t)(c->x[0] + 0x6c);
    float was[4], half[4];
    bool scale = at60();
    for (int i = 0; i < 4; i++) {
        was[i] = step[i];
        half[i] = scale ? was[i] * 0.5f : was[i];
        step[i] = half[i];
    }
    call_original(kMoveAction, c);
    for (int i = 0; i < 4; i++)
        if (step[i] == half[i]) step[i] = was[i];
}

// at 60, a script command's length in frames doubled while it starts, so a camera move or a turn takes as long as at 30
// and still moves every frame (0, "at once" or the command's own length, stays)
template <int N>
void frames_arg(Ctx* c) {
    int64_t* frames = (int64_t*)(uintptr_t)(c->x[1] + kFramesArgs[N].offset);
    int64_t was = *frames;
    if (at60() && was > 0) *frames = was * 2;
    call_original(kFramesArgs[N].handler, c);
    *frames = was;
}

void battle_camera(Ctx* c) {
    g_battle_frame = g_frame.load();
    call_original(kBattleCamera, c);
}

void save() {
    if (g_keep) set_setting("fps", g_fps60 ? "60" : "30");
}

}  // namespace

void framerate_init() {
    g_fps60 = setting("fps") == "60";
    if (const char* e = getenv("LETSGO_FPS")) {
        g_fps60 = atoi(e) == 60;
        g_keep = false;                              // a test's choice isn't kept
    }
    override_main(kWorldPeriod, world_period);
    override_main(kDisplayPeriod, display_period);
    override_main(kMessageUpdate, message_update);
    override_main(kBattleCamera, battle_camera);
    override_main(kScriptRun, script_run);
    override_main(kFadeUpdate, fade_update);
    override_main(kMoveAction, move_action);
    override_main(kFramesArgs[0].handler, frames_arg<0>);
    override_main(kFramesArgs[1].handler, frames_arg<1>);
    override_main(kFramesArgs[2].handler, frames_arg<2>);
    override_main(kFramesArgs[3].handler, frames_arg<3>);
    override_main(kFramesArgs[4].handler, frames_arg<4>);
    override_main(kFramesArgs[5].handler, frames_arg<5>);
    override_main(kFramesArgs[6].handler, frames_arg<6>);
    log("frame rate: %d frames a second (Tab switches)", g_fps60 ? 60 : 30);
}

bool fps60() { return g_fps60; }
bool fps60_odd_frame() { return gate(); }

void toggle_fps60() {
    g_fps60 = !g_fps60;
    log("frame rate: %d frames a second", g_fps60 ? 60 : 30);
    save();
}

}  // namespace rt

// a battle's frame-counted systems, every other frame at 60: strong definitions in place of the recompiled weak aliases,
// so direct calls come here too
#define RT_BATTLE_COUNTED(addr)               \
    extern "C" void o##addr(Ctx* c);          \
    extern "C" void f##addr(Ctx* c) {         \
        if (rt::battle_gate()) return;        \
        o##addr(c);                           \
    }
RT_BATTLE_COUNTED(7100aa0bc0)   // the battle's sequences (0x7100210510 steps each a frame)
RT_BATTLE_COUNTED(7100b0df10)   // a task group (the battle update's +0x428, +0x430, +0x438): each task's +0x38
RT_BATTLE_COUNTED(7100aa3350)   // the battle update's +0x4a8
RT_BATTLE_COUNTED(71000b8490)   // the battle update's +0x420
RT_BATTLE_COUNTED(7100a7cff0)   // the battle update's own parts, its flow
RT_BATTLE_COUNTED(7100a7d140)
RT_BATTLE_COUNTED(7100a7d9a0)
RT_BATTLE_COUNTED(7100a7dd40)
RT_BATTLE_COUNTED(7100a8e5a0)
