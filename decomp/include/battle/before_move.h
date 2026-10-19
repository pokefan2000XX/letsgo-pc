// Whether a Pokémon can use its move this turn (docs/PLAN.md, roadmap phase 5, step 3).
//
// GetMoveFailReason, in order: a turn of sleep is counted (it may wake up); asleep, it can't move unless
// event 0x22 says so (Snore, Sleep Talk); frozen, it can't unless its move thaws it (moves with flag 10:
// Flame Wheel, Scald; event 0xd4 can stop that: Burn Up) or it thaws (20%); a disobedient Pokémon loafs
// around or naps; a move without PP left fails; under a rule (the battle's byte +0x4a1), so do moves with flag
// 0xe; event 0x23 can give any reason (Truant); it may have flinched, or lost its focus (Focus Punch after
// being hit); Disable, Heal Block (healing moves), Throat Chop (sound moves), Gravity (moves with flag 9: Fly,
// Splash), Encore, a lock into one move (Outrage, probably) and a Choice item's lock, Taunt (moves without
// power) and Imprison stop the move; confused, it hurts itself 33% of the time; paralyzed, it can't move 25%
// of the time; in love, 50%. Struggle skips Encore through Imprison, and a move with MoveParam::unk20 set
// skips Disable, Heal Block, Throat Chop, Gravity, Encore, Taunt and Imprison.
#pragma once
#include "battle/battle.h"

namespace battle {

// Why a Pokémon can't use its move (GetMoveFailReason), 0 when it can. Each picks the message
// (0x71009f7ae0).
enum MoveFailReason : u32 {
    kMoveFailNone = 0,
    kMoveFailNoPp = 1,
    kMoveFailAsleep = 2,
    kMoveFailParalyzed = 3,
    kMoveFailFrozen = 4,
    kMoveFailConfused = 5,         // it hurts itself
    kMoveFailFlinched = 6,
    kMoveFailLostFocus = 7,        // Focus Punch
    kMoveFailInLove = 8,
    kMoveFailDisabled = 9,
    kMoveFailTaunted = 0xa,
    kMoveFailImprisoned = 0xc,
    kMoveFailHealBlocked = 0xd,
    kMoveFailLocked = 0x11,        // Encore, a lock into one move, a Choice item
    kMoveFailGravity = 0x14,
    kMoveFailLoafing = 0x16,       // it disobeys
    kMoveFailNapping = 0x17,       // it disobeys and takes a nap
    kMoveFailRule = 0x1a,
    kMoveFailThroatChop = 0x20,
};

// Why the Pokémon can't use the move this turn (see above), or 0. noPpCheck or bit 3 of flags leaves the PP
// check out; targets (may be null) go to event 0x23.
u32 GetMoveFailReason(Flow* flow, BattlePokemon* pokemon, const MoveParam* move, bool noPpCheck, PokemonSet* targets,
                      const u8* flags);
// Counts a turn of sleep, and wakes the Pokémon (telling the clients, with the message) when its turns are up.
void CountSleep(Flow* flow, BattlePokemon* pokemon);
// For a frozen Pokémon: true when its move thaws it (see above); otherwise it thaws 20% of the time (cured
// with the message) and the answer is false.
bool CheckThaw(Flow* flow, BattlePokemon* pokemon, u32 move);
// Cures the Pokémon's status, telling the clients; with message, also the message for waking up (status 2),
// thawing (3) or another status.
void CureStatus(Flow* flow, BattlePokemon* pokemon, u32 status, bool message);
// Whether the Pokémon hurts itself instead of moving: a disobedient one that does (Flow::disobedience 3), or a
// confused one 33% of the time once a turn of confusion has been counted (it may snap out of it).
bool CheckConfusion(Flow* flow, BattlePokemon* pokemon);
// The message for a Pokémon in love (condition 7, naming the one it loves).
void PutInLoveMessage(Flow* flow, BattlePokemon* pokemon);
// Calls an event that may give a reason a move fails (variable 0x28; event 0x23: Truant) with the move and its
// targets. move may be null: then the event gets the move's number and its category from the data.
u32 CallMoveFailEvent(Flow* flow, BattlePokemon* pokemon, u32 moveId, u32 event, const MoveParam* move,
                      PokemonSet* targets);

}  // namespace battle
