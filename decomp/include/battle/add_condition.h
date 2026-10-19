// Whether a condition can be given to a Pokémon (docs/PLAN.md, roadmap phase 5, step 3).
//
// CheckConditionPrevented, in order: a Pokémon that has the condition already can't get it again (unless mode
// is 2), nor a status while it has one (mode 0); its types and the weather (GetConditionImmunity: Electric types
// can't be paralyzed, Fire types burned, Poison and Steel types poisoned, Ice types frozen, Grass types seeded,
// and nothing freezes in harsh sunlight), unless event 0xd3 says they don't count (Corrosion); then event 0x7c
// (Limber, Insomnia, Leaf Guard, Safeguard, Uproar), whose message comes from event 0x7e. Whatever stops it,
// the failure's message can follow (PutConditionFailMessage).
#pragma once
#include "battle/battle.h"

namespace battle {

// Why a condition can't be given (GetConditionImmunity, PutConditionFailMessage)
enum ConditionFailReason : u32 {
    kConditionFailNone = 0,
    kConditionFailAlready = 1,     // it has the condition already
    kConditionFailImmune = 2,      // "It doesn't affect ...": its type
    kConditionFailFails = 3,       // "But it failed!": another status, the sun, an ability that stays
};

// Whether the condition can't be given to target (see above): true when it's stopped. With message, the
// failure's message follows, except a status from cause 3 failing for reason 3 with quietIfFailed set.
// conditionData isn't read.
bool CheckConditionPrevented(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, s32 condition,
                             const u64* conditionData, u32 cause, u32 unk1d, u32 mode, bool message,
                             bool quietIfFailed);
// Gives the target a condition (SetCondition), sends a status's reaction to the clients, puts its message
// (MakeConditionMessage) when message is set and otherMessage (may be null) when it isn't; a frozen Shaymin in
// its Sky Forme goes back to its Land Forme. Then what follows: StatusAdded for a status, event 0x81 for Gastro
// Acid (and UnnerveSuppressed when its ability is Unnerve), ConditionAdded for the rest; and, unless
// noItemReaction, the target's item may react (CallItemReaction, point 4).
void AddCondition(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, u32 condition, const u64* conditionData,
                  bool message, bool noItemReaction, const BattleMessage* otherMessage);
// Whether a move's condition (moves::GetInflictCondition; 0xffff lets event 0x77 pick one: Telekinesis) can be
// given to the defender, with CheckConditionPrevented's message.
bool CanMoveGiveCondition(Flow* flow, u32 move, BattlePokemon* attacker, BattlePokemon* defender);
// Gives the defender a condition that the attacker's move rolled (RollMoveCondition), unless
// CheckConditionPrevented stops it: true when it's given. 0xffff lets event 0x77 pick one, otherwise event 0x78
// sees it (the binding moves); event 0x79 can change its 8 bytes (Leech Seed, Foresight, Grip Claw). A message
// the events made (in Flow::message) is put after it, instead of AddCondition's own. unused isn't read.
bool GiveMoveCondition(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, u32 unused, u32 condition,
                       u64 conditionData, u32 cause, u32 unk1d, bool message, bool quietIfFailed);
// Whether event 0xd3 lets the attacker's condition past the target's types (Corrosion); false without an
// attacker.
bool IgnoresConditionImmunity(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, s32 condition, u32 cause);
// Why the Pokémon's types or the weather stop a condition (see above), or 0. Also: Yawn fails on a Pokémon with
// a status, and Gastro Acid on an ability that can't be taken away.
u32 GetConditionImmunity(Flow* flow, BattlePokemon* pokemon, s32 condition);
// Event 0x7c: whether an ability, an item or a side's effect stops the condition (variable 0x4a); message says
// whether to show it, and a handler may turn it off (variable 0x4c).
bool CallConditionImmunityEvent(Flow* flow, BattlePokemon* attacker, BattlePokemon* target, s32 condition, u32 cause,
                                bool* message);
// The message for a condition that couldn't be given, by reason (and for reason 1, by condition), and marks the
// action as having failed (Flow::unk1511).
void PutConditionFailMessage(Flow* flow, BattlePokemon* pokemon, u32 reason, s32 condition, u32 cause);

}  // namespace battle
