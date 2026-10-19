// Move data: power, type, PP and so on, by move. Not decompiled yet (D4): these link to the originals.
#pragma once
#include "decomp.h"

namespace moves {

// A move's PP with PP Ups applied (each adds a fifth of the base PP; more than 3 counts as 3)
u8 GetMaxPp(u32 move, u8 ppUps);
u8 GetType(u32 move);
// Its priority (signed: Trick Room's is -7)
int GetPriority(u32 move);
// Its accuracy (100 for a move that never misses)
u16 GetAccuracy(u32 move);
// Whether it never misses (its accuracy is 101 in the data)
bool NeverMisses(u32 move);
// Its critical stage (6 always lands one: Storm Throw, Frost Breath, Zippy Zap)
u8 GetCriticalStage(u32 move);
// Its category: 0 status, 1 physical, 2 special
u32 GetCategory(u32 move);
// Whether it has power (a status move hasn't)
bool HasPower(u32 move);
// One of its flags (8 sound, 9 fails under Gravity, 10 thaws its user, 0xc heals)
bool HasFlag(u32 move, u32 flag);
// What it does besides its damage: 4 it gives the target a condition, 6 it changes the target's stats, and so on
u32 GetEffectKind(u32 move);
// Its chance to make the target flinch
u32 GetFlinchChance(u32 move);
// The condition it gives (0 none), its chance, and how long it lasts: the kind in bits 0-3, then the least
// and the most turns, 6 bits each (battle::MakeConditionData)
u32 GetInflictCondition(u32 move);
u32 GetInflictChance(u32 move);
u16 GetInflictDuration(u32 move);
// Its stat changes (index 0 to 2): the chance, and the stat with its stages (a stat of 0 is none)
u8 GetStatChangeChance(u32 move, u32 index);
u32 GetStatChange(u32 move, u32 index, s32* stages);

}  // namespace moves
