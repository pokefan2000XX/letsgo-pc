// Putting a message made beforehand (docs/PLAN.md, roadmap phase 5, step 3).
#pragma once
#include "battle/battle.h"

namespace battle {

// Puts a BattleMessage: kind 1 as ServerCommands::PutMessage's kind 0x7b, kind 2 as 0x7c (starting with a
// Pokémon's name), or 0x7d and 0x7e when it has a value; false for another kind.
bool PutBattleMessage(Flow* flow, const BattleMessage* message);

}  // namespace battle
