// quick-boot: the boot goes straight to the opening without the controller screen (the Handheld
// controller is the one used), and a button press skips the "Presented by GAME FREAK" logo and the
// title demo's opening (the Pikachu silhouette and the Pokémon logo), up to "Press the A Button".
#include "decomp.h"

extern "C" {
u64 ORIG_710000e420(u8* boot, u8* sys);
u64 ORIG_710013a780(u8* procedures);
void ORIG_710004e2d0(u8* procedure, u32 mode);
u64 ORIG_7100126cf0(u8* layout);
void ORIG_7100073d80(u8* logo);
void ORIG_71001ddb70(u8* demo);
void ORIG_71001f1d20(u8* core);
void ORIG_7100663dd0(u8* player, u32 index, u32 flag);
// AK::SoundEngine::PostEvent(AkUniqueID, AkGameObjectID, flags, callback, cookie, externals, sources, playing)
u32 ORIG_71003b1e50(u32 event, u64 object, u32 flags, void* callback, void* cookie, u32 externals, void* sources,
                    u32 playing);
// A layout's animation player: an animation's last frame, and setting its frame.
float FUN_7100664170(u8* player, u32 index);
void FUN_7100663e90(float frame, u8* player, u32 index);

// nn::hid::GetNpadState(NpadHandheldState*, uint const&): the runtime's keyboard and gamepad
void _ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj(void* state, const u32& id);
// nn::diag::detail::LogImpl(LogMetaData const&, char const*, ...): the runtime writes it to letsgo.log
void _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(const void* meta, const char* format, ...);
}

namespace {

alignas(8) const u8 kMeta[0x80] = {};   // LogImpl's metadata; the runtime doesn't read it

template <typename T>
T& At(u8* object, u32 offset) {
    return *reinterpret_cast<T*>(object + offset);
}

// The buttons that skip: A, B, X, Y, L, R, ZL, ZR, + and - (not the sticks or the D-pad).
constexpr u64 kSkipButtons = 0xfcf;
constexpr u32 kHandheld = 0x20;

u64 held = 0;

// True when one of the skip buttons went down since the last call.
bool NewPress() {
    alignas(8) u8 state[0x28] = {};   // NpadHandheldState: sampling number, buttons, sticks, attributes
    _ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj(state, kHandheld);
    u64 buttons = At<u64>(state, 8) & kSkipButtons;
    bool pressed = (buttons & ~held) != 0;
    held = buttons;
    return pressed;
}

bool skip_logo = false;      // a press during the logo
bool logo_over = false;      // the logo has gone: presses now skip the title's opening
bool skip_opening = false;   // a press since then
int skip_wait = 0;           // the title demo's frames since the press
bool title_shown = false;    // "Press the A Button" has been on screen

}  // namespace

// ---- The controller screen ----
//
// The boot (0x710000e420, its step at +0x68) starts with the controller procedure in step 0, in
// mode 1: always ask which controller to use. Mode 0, which the game uses elsewhere, asks only when
// the connected controllers need it, and the runtime's one Handheld controller doesn't.

namespace {
bool boot_step0 = false;
}

extern "C" u64 FUN_710000e420(u8* boot, u8* sys) {
    u32 step = At<u32>(boot, 0x68);
    boot_step0 = step == 0;
    u64 result = ORIG_710000e420(boot, sys);
    boot_step0 = false;
    return result;
}

// The controller procedure's constructor (this, mode).
extern "C" void FUN_710004e2d0(u8* procedure, u32 mode) {
    if (boot_step0 && mode == 1) {
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "quick-boot: no controller screen at boot");
        mode = 0;
    }
    ORIG_710004e2d0(procedure, mode);
}

// ---- The GAME FREAK logo ----
//
// The logo is a layout (its class at 0x710152eb50). The whole logo, fade and shine included, is the
// layout's opening (0x7100126cf0, every frame while the layout's phase at +0x80 is 1): it waits
// (+0x84 = 1) for the opening's animations, then makes the layout active (+0x84 = 2, phase 2). A
// press makes it active at once. Active, the logo's update (0x7100073d80) ends it when its animation
// is done, sending its scene event 0 (the event at +0x260: id +0xc, +8 cleared) and going on to its
// step 2 (+0x268); after a press it doesn't wait for the animation.

namespace {
constexpr u64 kLogoClass = 0x710152eb50;
}

extern "C" u64 FUN_7100126cf0(u8* layout) {
    if (At<u64>(layout, 0) == kLogoClass) {
        if (NewPress()) skip_logo = true;
        if (skip_logo && At<u32>(layout, 0x84) == 1) {
            _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "quick-boot: skipped the GAME FREAK logo");
            At<u8>(layout, 0x8d) = 0;
            At<u32>(layout, 0x90) = 0;
            At<u32>(layout, 0x84) = 2;
        }
    }
    return ORIG_7100126cf0(layout);
}

extern "C" void FUN_7100073d80(u8* logo) {
    u32& step = At<u32>(logo, 0x268);
    if (step == 1 && skip_logo) {
        u8* event = At<u8*>(logo, 0x260);
        At<u32>(event, 0xc) = 0;
        At<u32>(event, 8) = 0;
        step = 2;
    } else {
        ORIG_7100073d80(logo);
    }
    if (step >= 2) {
        skip_logo = false;
        logo_over = true;
    }
}

// ---- The title demo's opening ----
//
// The title screen is a demo, bin/demo/sequence/sd9010_title.bseq: the opening runs to frame 564,
// where "Press the A Button" starts (the layout's anime_btn_loop and the commands that wait for A).
// A demo loads (0x71001ddb70, every frame until it plays; its core at +0x220, the core's sequence
// at +0x98) by running every command of its sequence once, which makes a track for each; then the
// core's clock (+0x680, from -1) goes up 2 a frame (0x71001f1d20) and each track catches up with
// it, starting, stepping and ending through every frame it hasn't had. So moving the clock on
// plays the opening in one frame, with every model, camera, light, sound and effect started and
// stopped as it would be. (The game's own seek, 0x71001efdd0, sets a flag at +0x692 that makes the
// tracks jump instead, skipping what starts and ends on the way, like the title's own layout.)
// The clock goes to 562, so the next frame is 564's; the layout animations the opening started
// (the logo's anime_in, from frame 25, on its own clock) are set to their last frame on that frame,
// before 564's commands start "Press the A Button". The opening's own sound, Play_sd9010_title,
// isn't posted on the way (the title's music and Pikachu's cry are).
//
// A press after the logo, while the demo loads, counts too. The skip comes a frame after the press, so the title
// doesn't take the same press as the A that starts the game. The opening plays again when the title
// has waited long enough (the boot starts the demo over), and a press skips it then too.

namespace {

constexpr s32 kTitleFrame = 564;
constexpr char kTitleName[] = "sd9010_title";

// A Wwise ID: the 32-bit FNV-1 hash of the lowercase name (AK::SoundEngine::GetIDFromString).
constexpr u32 WwiseId(const char* name) {
    u32 hash = 0x811c9dc5;
    for (; *name; name++) hash = (hash * 0x01000193) ^ u8(*name >= 'A' && *name <= 'Z' ? *name - 'A' + 'a' : *name);
    return hash;
}
// The opening's own sound (its frame-63 command), in sd9010_title.bnk.
constexpr u32 kOpeningSound = WwiseId("Play_sd9010_title");

u8* title_core = nullptr;

// The layout animations started during the title's opening (the logo's anime_in), to finish when
// it's skipped: they run on their own clock, not the demo's.
struct Animation { u8* player; u32 index; };
Animation opening_animations[8];
int opening_animation_count = 0;
bool opening_running = false;     // the title's clock is between 0 and 564
bool finish_animations = false;   // skipped: finish them when the clock gets to 564

// The title's sequence: its file (+0x170) names it in its frame-0 command 38.
bool IsTitle(u8* sequence) {
    const u8* file = At<u8*>(sequence, 0x170);
    if (!file) return false;
    bool found = false;
    for (u32 i = 0x10; i < 0x1000 && !found; i++) {
        u32 n = 0;
        while (n < sizeof(kTitleName) && file[i + n] == static_cast<u8>(kTitleName[n])) n++;
        found = n == sizeof(kTitleName);   // the name and its NUL
    }
    return found;
}

}  // namespace

// The procedures' update (0x710013a780, every frame): after the logo the screen stays white for a
// few seconds while its procedure closes and the title's demo loads, and a press then is for the
// title's opening.
extern "C" u64 FUN_710013a780(u8* procedures) {
    if (logo_over && !title_shown && NewPress()) skip_opening = true;
    return ORIG_710013a780(procedures);
}

extern "C" void FUN_71001ddb70(u8* demo) {
    ORIG_71001ddb70(demo);
    // Every demo loads here first, so a demo made where the title's was is seen for what it is.
    u8* core = At<u8*>(demo, 0x220);
    u8* sequence = core ? At<u8*>(core, 0x98) : nullptr;
    if (sequence && IsTitle(sequence)) {
        if (core != title_core) opening_animation_count = 0, opening_running = false;
        title_core = core;
    } else if (core == title_core) {
        title_core = nullptr;
    }
}

// A layout animation starting (0x7100663dd0: player, index, flag).
extern "C" void FUN_7100663dd0(u8* player, u32 index, u32 flag) {
    ORIG_7100663dd0(player, index, flag);
    if (opening_running && opening_animation_count < 8) opening_animations[opening_animation_count++] = {player, index};
}

extern "C" void FUN_71001f1d20(u8* core) {
    ORIG_71001f1d20(core);
    if (core != title_core) return;
    s32& frame = At<s32>(core, 0x680);
    if (frame < 0) return;
    if (!opening_running && frame < kTitleFrame) opening_animation_count = 0;   // the opening starting
    opening_running = frame < kTitleFrame;
    if (frame >= kTitleFrame) {
        if (finish_animations) {
            for (int i = 0; i < opening_animation_count; i++) {
                Animation& a = opening_animations[i];
                FUN_7100663e90(FUN_7100664170(a.player, a.index), a.player, a.index);
            }
            finish_animations = false;
        }
        skip_opening = false;
        skip_wait = 0;
        opening_animation_count = 0;
        title_shown = true;
        return;
    }
    if (!skip_opening && NewPress()) skip_opening = true;
    if (skip_opening && ++skip_wait > 1) {
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "quick-boot: skipped the title's opening from frame %d", frame);
        frame = kTitleFrame - 2;
        finish_animations = true;
        skip_opening = false;
        skip_wait = 0;
    }
}

extern "C" u32 FUN_71003b1e50(u32 event, u64 object, u32 flags, void* callback, void* cookie, u32 externals,
                              void* sources, u32 playing) {
    if (finish_animations && event == kOpeningSound) {
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "quick-boot: the opening's sound left out");
        return 0;   // AK_INVALID_PLAYING_ID
    }
    return ORIG_71003b1e50(event, object, flags, callback, cookie, externals, sources, playing);
}
