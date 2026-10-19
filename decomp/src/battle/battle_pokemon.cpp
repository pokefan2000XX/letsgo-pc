// The battle's Pokémon (battle/battle.h): the functions of its file (0x71009e2110-0x71009e80c0) that are
// decompiled.
#include "battle/battle.h"

#include "battle/calc.h"
#include "pokemon/pokemon.h"

namespace battle {

// 0x71009e3e60
bool BattlePokemon::CanFight() {
    if (pokemon->IsEgg(pokemon::kEggAny)) return false;
    return GetValue(kValueHp) != 0;
}

// 0x71009e40b0
u32 BattlePokemon::GetConditionDamage(u32 condition) {
    if (!(conditions[condition] & 7)) return 0;
    switch (condition) {
    case kConditionBurn:
        return MaxHpFraction(this, 16);
    case kConditionPoison:
        if (IsBadPoison(&conditions[condition])) return MaxHpFraction(this, 16) * conditionCounters[condition];
        return MaxHpFraction(this, 8);
    case kConditionNightmare:
        if (GetValue(kValueAbility) != kAbilityComatose && !(conditions[kStatusSleep] & 7)) return 0;
        return MaxHpFraction(this, 4);
    case kConditionCurse:
        return MaxHpFraction(this, 4);
    }
    return 0;
}

}  // namespace battle
