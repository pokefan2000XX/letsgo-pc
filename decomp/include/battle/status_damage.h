// A condition's damage at the end of a turn (docs/PLAN.md, roadmap phase 5, step 3).
//
// EndOfTurnCondition, for each condition a Pokémon has (unless it has fainted): the damage
// (BattlePokemon::GetConditionDamage: a burn 1/16 of its maximum HP, poison 1/8, bad poison 1/16 times the
// turns counted, Nightmare 1/4 while it sleeps, Curse 1/4; at least 1), then the condition's own turn: Bind,
// Leech Seed, Perish Song, Ingrain and Aqua Ring, and Encore ends once its move has no PP left.
//
// The conditions' turns queue records (Flow::NewRecord) for the rest of the turn to carry out:
// - Bind (and the other binding moves): damage of 1/8 of the maximum HP (1/6 with the condition's flag,
//   probably a Binding Band), with the move's effect for Bind, Wrap, Fire Spin, Clamp, Whirlpool, Sand Tomb,
//   Magma Storm and Infestation.
// - Leech Seed: when there's a Pokémon at the seeder's position, damage of 1/8 of the maximum HP (at most the
//   HP left) with effect 0x45, the same HP drained to the seeder, then a record about the seeder.
// - Perish Song: the count's message while turns are left.
// - Ingrain and Aqua Ring (condition 0x24): 1/16 of the maximum HP restored unless it's full (Aqua Ring not
//   after fainting).
//
// StatusDamage: event 0x82 may change the damage (Poison Heal, Heatproof); event 0x98 may stop damage that
// doesn't come from a move (Magic Guard); then the clients are told, with the condition's message, and
// ApplyIndirectDamage takes it off.
#pragma once
#include "battle/battle.h"

namespace battle {

// A condition's turn at the end of a turn: its damage, then what else it does.
void EndOfTurnCondition(Flow* flow, BattlePokemon* pokemon, u32 condition);
// A condition's damage to a Pokémon (see above).
void StatusDamage(Flow* flow, BattlePokemon* pokemon, u32 condition, u32 damage);
// What some conditions do at the end of a turn, after their damage (see above).
void BindTurn(Flow* flow, BattlePokemon* pokemon);
void LeechSeedTurn(Flow* flow, BattlePokemon* pokemon);
void PerishSongTurn(Flow* flow, BattlePokemon* pokemon);
void IngrainTurn(Flow* flow, BattlePokemon* pokemon);
void AquaRingTurn(Flow* flow, BattlePokemon* pokemon);

}  // namespace battle
