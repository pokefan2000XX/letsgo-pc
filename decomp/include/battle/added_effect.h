// The chances of a move's added effects (docs/PLAN.md, roadmap phase 5, step 3): a condition (Thunderbolt's
// paralysis), a flinch, and a stat change (Psychic's special defense drop), each from the move data and then
// an event that abilities and items answer.
#pragma once
#include "battle/battle.h"

namespace battle {

// The condition a move gives its target, or 0: the move data's condition, chance and duration (the condition's
// 8 bytes, MakeConditionData), then event 0x7b (Serene Grace doubles the chance, Sheer Force and Shield Dust
// stop it; a condition of 0xffff is none); then the roll. conditionData gets the condition's 8 bytes when
// one is given.
u32 RollMoveCondition(Flow* flow, u32 move, BattlePokemon* attacker, BattlePokemon* defender, u64* conditionData);
// A move's chance to make its target flinch: the move data's, or for a move without one, event 0x83's (King's
// Rock, Stench); doubled when event 0x83 says so (Serene Grace); 0 when it stops a move's own (Sheer Force).
u32 GetFlinchChance(Flow* flow, u32 move, BattlePokemon* attacker);
// Whether the target flinches: event 0x84 may change the chance or stop it (Inner Focus, Shield Dust), then
// the roll.
bool RollFlinch(Flow* flow, BattlePokemon* target, u32 chance);
// Whether a move's added effect happens: the chance of its first stat change in the move data, then event
// 0x67 (Serene Grace, Sheer Force, Shield Dust), then the roll.
bool RollAddedEffect(Flow* flow, const MoveParam* move, BattlePokemon* attacker, BattlePokemon* defender);
// A move's stat change (index 0 to 2 in the move data): the stat and its stages, then event 0x70, which may
// change both and multiply the stages (variable 0x3b). A stat of 8 comes back as 0.
void GetMoveStatChange(Flow* flow, u32 move, u32 index, BattlePokemon* attacker, BattlePokemon* defender, s32* stat,
                       s32* stages);

// A flinch from a move with this chance: true when the target flinches (flag 4), or instead loses its focus
// when it has flag 5 (flag 6: Focus Punch fails). A flinch that was sure (chance 100 or more) and didn't happen
// calls event 0x85.
bool ApplyFlinch(Flow* flow, BattlePokemon* target, u32 chance);
// A damaging move's added effect, by its kind: 4 a condition (RollMoveCondition, then GiveMoveCondition), 6 a
// stat change (RollAddedEffect, then ApplyMoveStatChanges with the next Flow::statChangeNumber). Nothing when
// skip is set, nor on a defender that has fainted. unused isn't read.
void ApplyAddedEffect(Flow* flow, const MoveParam* move, BattlePokemon* attacker, BattlePokemon* defender,
                      u32 unused, bool skip);

}  // namespace battle
