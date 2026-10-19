// Whether a Pokémon's stat stage can change, and changing it (docs/PLAN.md, roadmap phase 5, step 3).
//
// The stages first go through event 0x71 (Simple, Contrary). A change fails once the battle is decided (a side
// has no Pokémon left that can fight), when the stage is at its limit (+6 or -6), when the Pokémon is behind a
// substitute and another Pokémon causes the change, or when event 0x72 stops it (Clear Body, White Smoke,
// Hyper Cutter, Keen Eye, Big Pecks, Flower Veil, Full Metal Body, Mist); event 0x73 then has their message.
// After a change, event 0x74 lets Defiant and Competitive answer it.
//
// ApplyStatStage: the stage rises or falls as far as its limit, and the clients are told (commands 0xa and 0xb,
// and 0x4d and 0x4e as well when extra is set) and get the message ("rose", or with unk4 named, message 0x4f0;
// "fell" always, the rise only when message is set).
#pragma once
#include "battle/battle.h"

namespace battle {

// Why a stat change failed (Flow::statFailures).
enum StatFailReason : u32 {
    kStatFailLimit = 2,
    kStatFailSubstitute = 3,
    kStatFailPrevented = 4,
};

// Whether event 0x72 lets a change of stages to the target's stat happen (nothing sets variable 0x4a); causer
// is the Pokémon causing it.
bool IsStatChangeAllowed(Flow* flow, BattlePokemon* target, u32 stat, u8 causer, s32 stages, u32 unk69, u32 unk1d);
// Whether the target's stat can change by stages (after event 0x71, whose variable 3 is unk3). The failures
// are added to Flow::statFailures, and when message is set and quiet isn't they have a message: "won't go any
// higher/lower", "it doesn't affect" (the substitute; ignoresSubstitute lets it past), or event 0x73's.
// unused isn't read.
bool CanChangeStatStage(Flow* flow, u8 causer, BattlePokemon* target, u32 stat, s32 stages, u32 unk69, u8 unk3,
                        u32 unused, u32 unk1d, bool message, bool ignoresSubstitute, bool quiet);
// Changes a Pokémon's stat by stages (above 0 raises it, others lower it).
void ApplyStatStage(Flow* flow, BattlePokemon* pokemon, u32 stat, s32 stages, u16 unk4, bool message, bool extra);
// Changes the target's stat by stages when it can (CanChangeStatStage, never quiet): puts preMessage first
// (may be null), then the change and its message (ApplyStatStage), then event 0x74.
// Whether it changed.
bool ChangeStatStage(Flow* flow, u8 causer, BattlePokemon* target, u32 stat, s32 stages, u32 unk69, u8 unk3,
                     u32 unk7, u32 unk1d, bool message, bool riseMessage, const BattleMessage* preMessage,
                     bool ignoresSubstitute, bool extra);

}  // namespace battle
