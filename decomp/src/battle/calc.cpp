// The battle's arithmetic (battle/calc.h).
#include "battle/calc.h"

namespace battle {

namespace {

// By accuracy stage (0 to 12): the accuracy times the first, divided by the second
constexpr u8 kAccuracyStageRatios[13][2] = {{6, 18}, {6, 16}, {6, 14}, {6, 12}, {6, 10}, {6, 8}, {6, 6},
                                            {8, 6},  {10, 6}, {12, 6}, {14, 6}, {16, 6}, {18, 6}};
// By critical stage: a critical hit lands 1 in this many
constexpr u8 kCriticalOdds[4] = {24, 8, 2, 1};

}  // namespace

// 0x71009cfac0
u32 BaseDamage(u32 power, u32 attack, u32 level, u32 defense) {
    u32 levelFactor = level * 2 / 5 + 2;
    u32 damage = defense == 0 ? 0 : attack * power * levelFactor / defense;
    return damage / 50 + 2;
}

// 0x71009cfb00
u32 ApplyEffectiveness(u32 damage, u32 effectiveness) {
    if (effectiveness == 0) return 0;
    if (effectiveness < 7) return damage >> (7 - effectiveness);
    if (effectiveness == 7 || effectiveness > 13) return damage;
    return damage << (effectiveness - 7);
}

// 0x71009cfb90
u32 Random(u32 range) {
    return g_random->Next(range);
}

// 0x71009cfc10
u32 MulRatio(u32 value, u32 ratio) {
    u32 product = value * ratio;
    return (product >> 12) + ((product & 0xfff) > 0x800 ? 1 : 0);
}

// 0x71009cfed0
u32 MaxHpFraction(BattlePokemon* pokemon, u32 divisor) {
    u32 maxHp = pokemon->GetValue(kValueMaxHp);
    return maxHp < divisor ? 1 : maxHp / divisor;
}

// 0x71009cff00
u8 AccuracyAfterStages(u8 accuracy, u8 stage) {
    if (stage > 12) return accuracy;
    u32 value = static_cast<u32>(kAccuracyStageRatios[stage][0] * accuracy) / kAccuracyStageRatios[stage][1];
    return value < 100 ? value : 100;
}

// 0x71009cff40
bool RollCritical(u8 stage, s32 divisor) {
    if (stage > 3) return false;
    u8 odds = kCriticalOdds[stage] / divisor;
    return static_cast<u8>(g_random->Next(odds != 0 ? odds : 1)) == 0;
}

// 0x71009d00e0
bool IsStatusCondition(u32 condition) {
    return static_cast<s32>(condition) < 6;
}

// 0x71009d0250
bool IsSunny(u8 weather) {
    return weather == kWeatherSun || weather == kWeatherHarshSun;
}

// 0x71009d0290
void MakeConditionData(u16 duration, BattlePokemon* pokemon, u64* out) {
    u32 first = duration >> 4 & 0x3f;
    u32 second = duration >> 10 & 0x3f;
    u32 least = first > second ? second : first;
    u32 most = first > second ? first : second;
    switch (duration & 0xf) {
    case 1:
        *out = MakeConditionCounted(pokemon->GetId(), second, first);
        return;
    case 2: {
        u32 turns = g_random->Next(most - least + 1) + least;
        *out = MakeConditionTurns(pokemon->GetId(), turns);
        return;
    }
    case 3: {
        u8 cause = pokemon->GetId();
        *out = MakeConditionTied(cause, pokemon->GetId());
        return;
    }
    case 4: {
        u8 cause = pokemon->GetId();
        u8 tiedTo = pokemon->GetId();
        u32 turns = g_random->Next(most - least + 1) + least;
        *out = MakeConditionTurnsTied(cause, tiedTo, turns);
        return;
    }
    }
}

// 0x71009d03d0
u8 RollHitCount(u8 maxHits) {
    if (maxHits != 5) return maxHits;
    u8 roll = g_random->Next(100);
    if (roll < 35) return 2;
    if (roll < 70) return 3;
    return roll < 85 ? 4 : 5;
}

// 0x71009d04b0
bool RollPercent(u32 chance) {
    return static_cast<u32>(g_random->Next(100)) < chance;
}

// 0x71009d01e0
u32 WeatherRatio(u8 weather, u8 type) {
    switch (weather) {
    case kWeatherSun:
    case kWeatherHarshSun:
        return type == kTypeFire ? 0x1800 : type == kTypeWater ? 0x800 : 0x1000;
    case kWeatherRain:
    case kWeatherHeavyRain:
        return type == kTypeFire ? 0x800 : type == kTypeWater ? 0x1800 : 0x1000;
    }
    return 0x1000;
}

}  // namespace battle
