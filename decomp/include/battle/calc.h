// The battle's arithmetic (docs/PLAN.md, roadmap phase 5, step 3): a file of its own in the original. Ratios
// are 4.12 fixed point: 0x1000 is 1.
#pragma once
#include "battle/battle.h"

namespace battle {

u32 BaseDamage(u32 power, u32 attack, u32 level, u32 defense);
u32 ApplyEffectiveness(u32 damage, u32 effectiveness);
u32 Random(u32 range);
// value times a 4.12 ratio, rounded up only above a half
u32 MulRatio(u32 value, u32 ratio);
// The weather's ratio for a move's type: sun makes fire 1.5 and water 0.5, rain the other way round.
u32 WeatherRatio(u8 weather, u8 type);
// Whether the weather is sun or harsh sun.
bool IsSunny(u8 weather);
// An accuracy after the stages: stage 6 leaves it, each one above adds a third and each one below takes it
// to 3/4, 3/5 and so on down to 1/3 at 0. At most 100; a stage above 12 leaves the accuracy as it is.
u8 AccuracyAfterStages(u8 accuracy, u8 stage);
// Whether a critical hit lands: 1 in 24, 8, 2 and 1 for stages 0 to 3, each divided by divisor (2 for a
// Pokémon at the most friendship; a 0 left is 1). Never above stage 3.
bool RollCritical(u8 stage, s32 divisor);
// A fraction of a Pokémon's maximum HP, at least 1.
u32 MaxHpFraction(BattlePokemon* pokemon, u32 divisor);
// A condition's 8 bytes for a move's duration (the move data's kind in bits 0-3, then the least and the most
// turns, 6 bits each): kind 1 counted, 2 a random number of turns, 3 tied to the Pokémon, 4 both; for another
// kind, out isn't written. pokemon is the one that causes it.
void MakeConditionData(u16 duration, BattlePokemon* pokemon, u64* out);
// Whether a condition is a status (1 to 5, and 0), not one of the others.
bool IsStatusCondition(u32 condition);
// Whether a roll of 0 to 99 comes under chance: true chance% of the time.
bool RollPercent(u32 chance);
// How many times a move that hits 2 to 5 times hits: 2 or 3 35% of the time each, 4 or 5 15%. Any other
// maximum than 5 is the answer as it is.
u8 RollHitCount(u8 maxHits);

}  // namespace battle
