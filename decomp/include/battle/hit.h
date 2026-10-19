// Whether a move hits, and whether it lands a critical hit (docs/PLAN.md, roadmap phase 5, step 3).
//
// CheckHit: a move hits without a roll when No Guard is in play on either Pokémon, the attacker has Lock-On
// on the target, the move never misses (accuracy 101 in the move data) or event 0x3f says so (Thunder in the
// rain, Stomp on a Minimized Pokémon), or the target has condition 0x21 (Telekinesis, probably). Otherwise the
// accuracy (event 0x41: Compound Eyes, Bright Powder, Gravity) goes through the attacker's accuracy stage less
// the target's evasion stage (event 0x40: Keen Eye, Unaware; under Foresight a raised evasion doesn't count),
// times 1.2 after a flag (the Micle Berry, probably), at most 100, and 10 less when the target has 200
// friendship or more: the player's Pokémon dodge.
//
// CheckCritical: the move's critical stage plus the attacker's own, then event 0x44 (Super Luck, Scope Lens;
// Battle Armor and Lucky Chant stop it), at most 3: 1 in 24, 8, 2 or 1, twice as likely with 255 friendship.
// A move of stage 6 (Storm Throw, Frost Breath, Zippy Zap) always lands one unless event 0x44 stops it.
//
// CheckOneHitKo: a one-hit KO move never hits a Pokémon of a higher level, nor one that event 0x87 protects
// (Sturdy); it always hits under No Guard or Lock-On, and otherwise hits its accuracy (event 0x42: Sheer Cold)
// plus the difference of the levels, in 100.
#pragma once
#include "battle/battle.h"

namespace battle {

// Whether the move hits; dodged (may be null) says whether the target's friendship took 10 off the accuracy,
// however the roll went.
bool CheckHit(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, bool* dodged);
// Whether the move hits whatever its accuracy: No Guard in play on either Pokémon (Unk1798's check first), or
// the attacker's condition 0x1d, or its 0x1e (Lock-On, Mind Reader) on this target.
bool AlwaysHits(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender);
// Whether the move skips the accuracy check: it never misses, or event 0x3f says so (Thunder and Hurricane in
// the rain, Blizzard in hail, Stomp and Body Slam on a Minimized Pokémon, Pursuit on one switching out).
bool SkipsAccuracyCheck(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, u32 move);
// The move's accuracy after an event: 0x41 for the hit check, 0x42 for one-hit KO moves (Sheer Cold).
u32 CalcAccuracy(Flow* flow, u32 event, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move);
// Whether a one-hit KO move hits: never a target of a higher level, nor one that event 0x87 protects (Sturdy);
// always under No Guard or Lock-On; otherwise its accuracy (event 0x42) plus the levels' difference, in 100.
bool CheckOneHitKo(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move);
// Whether the move lands a critical hit; friendship says whether the attacker's 255 friendship doubled the
// odds, and is only written when a roll lands one.
bool CheckCritical(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, u32 move, bool* friendship);

}  // namespace battle
