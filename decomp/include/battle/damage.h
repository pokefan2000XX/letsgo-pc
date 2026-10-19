// A move's damage (docs/PLAN.md, roadmap phase 5, step 3): the formula, and the events that let moves,
// abilities and items change each part of it (battle/event.h).
//
// CalcDamage: the fixed-damage events (0x59, 0x5a: Seismic Toss, Super Fang); otherwise the base damage
// from the power, the attack, the defense and the attacker's level, then the spread-move ratio, the
// weather, a critical hit (1.5), the random roll (85 to 100%), the same-type bonus, the type effectiveness,
// a burn (half, for physical moves), and the damage event's ratio (0x5b: Reflect, Earthquake on a digging
// Pokémon). Ratios are 4.12 fixed point: 0x1000 is 1.
#pragma once
#include "battle/battle.h"
#include "battle/calc.h"

namespace battle {

// The damage a move does: false when it's worked out the usual way, true when an event fixed it (Seismic
// Toss). effectiveness is 7 for neutral, each step above doubling and each below halving, 0 immune;
// targetRatio is less than 0x1000 when the move hits several Pokémon; lowestRoll takes 85% instead of a
// random roll; unk4f is passed on as variable 0x4f.
bool CalcDamage(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, u32 effectiveness,
                u32 targetRatio, bool critical, bool lowestRoll, bool unk4f, u16* damage);
// The move's power: its base power (event 0x46 may change it) times the power ratio (event 0x47).
u16 CalcPower(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move);
// The attack or special attack used (event 0x48 may take another Pokémon's, or ignore stat stages), times
// event 0x4a's ratio; ignoresBurn says whether a burn should leave the damage whole.
u16 CalcAttack(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, bool critical,
               bool* ignoresBurn);
// The defense or special defense (event 0x49 may take the other one, or ignore stat stages), 1.5 for a
// Rock type's special defense in a sandstorm, times event 0x4b's ratio.
u16 CalcDefense(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, bool critical);
// The same-type bonus's ratio: 1.5 when the Pokémon has the type (event 0x53 may say otherwise), then
// event 0x54 (Adaptability).
u32 CalcStabRatio(Flow* flow, BattlePokemon* pokemon, u8 type);

}  // namespace battle
