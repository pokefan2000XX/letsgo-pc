// The turn order (docs/PLAN.md, roadmap phase 5, step 3): each action of a turn gets a key, and the actions
// run from the highest key down.
//
// A key packs, from the top: the action's kind (most kinds that aren't moves go before every move), the
// move's priority plus 7 (event 0x11 may change it: Prankster, Gale Wings), the Pokémon's bracket (event
// 0xf: Quick Claw and the Custap Berry move it up, Stall down; 1 is usual) and its speed (event 0x13:
// Swift Swim, Chlorophyll; halved by paralysis, reversed in Trick Room).
#pragma once
#include "battle/battle.h"

namespace battle {

// Puts actions first to last in their turn order: their keys, then the brackets (event 0xf) of those
// using moves, sorted by key each time; equal keys go in random order.
void OrderActions(Flow* flow, Action* actions, u8 first, u8 last, bool onlyMarked);
// An action's key (its kind, its move's priority after event 0x11, its Pokémon's speed).
u32 CalcOrderKey(Flow* flow, Action* action);
// A Pokémon's speed: its speed stat times event 0x13's ratio, halved by paralysis (unless the event says
// not), at most 10000; with trickRoom, 10000 minus that when event 0x13 says so.
u32 CalcSpeed(Flow* flow, BattlePokemon* pokemon, bool trickRoom);

// The key (another file of the original): kind 1, a move, keeps its priority and kind 5 gets priority 0;
// kinds 0, 2, 3, 4, 6, 7 and 8 have fixed places ahead of every move, and kind 9 is last.
u32 MakeOrderKey(u32 mode, BattlePokemon* pokemon, const u32* info, u8 unk, u32 speed, u32 move, u32 priority);
u32 SetOrderBracket(u32 key, u32 bracket);
u32 OrderKeyWithoutKind(u32 key);
int OrderKeyPriority(u32 key);

}  // namespace battle
