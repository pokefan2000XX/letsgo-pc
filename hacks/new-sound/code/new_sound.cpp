// The new-sound hack (docs/PLAN.md, roadmap phase 4): a sound the game doesn't have, played by new code.
//
// The game plays its sounds as Wwise events by name: its wrapper (0x710016d670) turns a name into its
// ID (AK::SoundEngine::GetIDFromString, 0x71003b0270, the 32-bit FNV-1 hash of the lowercase name)
// and posts it on a game object (AK::SoundEngine::PostEvent, 0x71003b1e50). This hook posts the event
// Play_pokebdsp_new_sound on the same game object whenever the game posts kTrigger. A mod adds that
// event to System_Common.bnk (letsgostudio's `pokebdsp lgpe add-sound`, docs/MODS.md); without it,
// Wwise finds no such event and nothing more plays.
#include "decomp.h"

// AkPlayingID PostEvent(AkUniqueID, AkGameObjectID, AkUInt32 flags, AkCallbackFunc, void* cookie,
//                       AkUInt32 externals, AkExternalSourceInfo*, AkPlayingID)
extern "C" u32 ORIG_71003b1e50(u32 event, u64 object, u32 flags, void* callback, void* cookie, u32 externals,
                               void* sources, u32 playing);

// nn::diag::detail::LogImpl(LogMetaData const&, char const*, ...): the runtime writes it to letsgo.log
extern "C" void _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(const void* meta, const char* format, ...);

namespace {

// A Wwise ID, as GetIDFromString makes it.
constexpr u32 Id(const char* name) {
    u32 hash = 0x811C9DC5;
    for (; *name; name++) {
        char c = *name >= 'A' && *name <= 'Z' ? char(*name - 'A' + 'a') : *name;
        hash = (hash * 0x01000193) ^ u8(c);
    }
    return hash;
}

constexpr u32 kTrigger = Id("Play_SE_System_Menu00");
constexpr u32 kNewSound = Id("Play_pokebdsp_new_sound");
static_assert(Id("bgm_field") == 0xC777063C, "Wwise IDs are FNV-1 of the lowercase name");

alignas(8) const u8 kMeta[0x80] = {};            // LogImpl's metadata; the runtime doesn't read it

}  // namespace

extern "C" u32 FUN_71003b1e50(u32 event, u64 object, u32 flags, void* callback, void* cookie, u32 externals,
                              void* sources, u32 playing) {
    u32 id = ORIG_71003b1e50(event, object, flags, callback, cookie, externals, sources, playing);
    if (event == kTrigger) {
        u32 added = ORIG_71003b1e50(kNewSound, object, 0, nullptr, nullptr, 0, nullptr, 0);
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "new-sound: posted %08x after %08x: playing %u", kNewSound, event, added);
    }
    return id;
}
