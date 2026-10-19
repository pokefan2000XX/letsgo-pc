// The new-move-effect hack (docs/PLAN.md, roadmap phase 5): a move effect the game doesn't have, written as new code.
//
// The battle gives a move its effect code through handlers (decomp/include/battle/event.h): when a Pokémon uses a
// move, 0x7100a38010 looks the move up in a table of 351 moves (0x71015cd300: the move, then a function that returns
// its handlers and their count) and registers those handlers for the Pokémon. A handler is an event number and a
// function the battle calls with that event; it reads and changes the event's variables. Facade's power handler,
// for example, answers kEventPower when its user is the attacker and doubles kVarPowerRatio when the user is burned,
// paralyzed or poisoned. tools/battle_events.py lists every event, its variables and what answers it.
//
// This hook gives Tackle, which has no handlers in the game, one of its own: it multiplies Tackle's power by 5
// whenever its user attacks with it. Everything else is the game's: other moves go to the original, and Tackle is
// registered the way the original registers a move it has handlers for.
#include "battle/event.h"

extern "C" bool ORIG_7100a38010(void* pokemon, u32 move, u32 priority);
extern "C" u32 ORIG_71009e2f30(void* pokemon);   // the Pokémon's ID in the battle

namespace {

using namespace battle;

constexpr u32 kTackle = 33;

void TacklePower(HandlerSet*, Flow*, u8 pokemon, u32*) {
    if (GetVar(kVarAttacker) == pokemon) MultiplyVar(kVarPowerRatio, 5 * 0x1000);
}

const Handler kTackleHandlers[] = {{kEventPower, TacklePower}};

}  // namespace

extern "C" bool FUN_7100a38010(void* pokemon, u32 move, u32 priority) {
    if (move != kTackle) return ORIG_7100a38010(pokemon, move, priority);
    // As the original: a Pokémon has at most 8 move handler sets, and each move's once.
    u8 id = ORIG_71009e2f30(pokemon);
    u32 sets = 0;
    for (HandlerSet* set = FirstHandlerSet(kKindMove, id); set; set = NextHandlerSet(set), sets++)
        if (sets > 7 || GetId(set) == move) return false;
    AddHandlerSet(kKindMove, move, 2, priority, id, kTackleHandlers, 1);
    return true;
}
