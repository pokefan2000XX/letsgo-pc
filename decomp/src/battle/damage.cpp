// A move's damage (battle/damage.h).
#include "battle/damage.h"

#include "battle/event.h"

namespace battle {

namespace {

// The damage's own variables
constexpr u32 kVarMoveId = 0x12;
constexpr u32 kVarMoveType = 0x19;
constexpr u32 kVarMoveUnk18 = 0x1c;
constexpr u32 kVarMoveCategory = 0x1e;
constexpr u32 kVarDamageRatio2 = 0x3c;      // event 0x5c's ratio
constexpr u32 kVarFixedDamage = 0x3d;
constexpr u32 kVarDamageIsFixed = 0x3e;
constexpr u32 kVarEffectiveness = 0x3f;
constexpr u32 kVarStatOf = 0x42;            // whose attack: Foul Play's is the target's
constexpr u32 kVarDefenseStat = 0x43;
constexpr u32 kVarOtherDefense = 0x44;      // Psyshock: the other defense
constexpr u32 kVarNoWeather = 0x4a;         // in its own frame: Air Lock, Cloud Nine
constexpr u32 kVarStab = 0x4d;
constexpr u32 kVarCritical = 0x4e;
constexpr u32 kVarUnk4f = 0x4f;
constexpr u32 kVarFlag = 0x5b;              // ignore stat stages, or has the same-type bonus
constexpr u32 kVarIgnoresBurn = 0x5c;
constexpr u32 kVarMoveUnk10 = 0x65;

constexpr u32 kEventFixedDamage = 0x59;
constexpr u32 kEventFixedDamage2 = 0x5a;
constexpr u32 kEventAttackStat = 0x48;      // which Pokémon's attack, and whether its stages count
constexpr u32 kEventDefenseStat = 0x49;
constexpr u32 kEventStab = 0x53;
constexpr u32 kEventStabRatio = 0x54;
constexpr u32 kEventDamageRatio2 = 0x5c;
constexpr u32 kEventDamageDone = 0x5d;
constexpr u32 kEventNoWeather = 0x92;

// Whether the weather is suppressed (event 0x92, in a frame of its own).
bool WeatherSuppressed(Flow* flow) {
    PushVars();
    SetVarRewritableOnce(kVarNoWeather, 0);
    CallEventWithSkipChecks(flow, kEventNoWeather);
    s32 suppressed = GetVar(kVarNoWeather);
    PopVars();
    return suppressed != 0;
}

u32 StatOf(BattlePokemon* pokemon, u32 stat, bool ignoreStages, bool critical) {
    if (ignoreStages) return pokemon->GetValueIgnoringStages(stat);
    if (critical) return pokemon->GetValueForCritical(stat);
    return pokemon->GetValue(stat);
}

}  // namespace

// 0x7100a016d0
bool CalcDamage(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, u32 effectiveness,
                u32 targetRatio, bool critical, bool lowestRoll, bool unk4f, u16* damage) {
    PushVars();
    SetConstVar(kVarEffectiveness, effectiveness);
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarCritical, critical);
    SetConstVar(kVarMoveType, move->type);
    SetConstVar(kVarMoveId, move->id);
    SetConstVar(kVarMoveUnk10, move->unk10);
    SetConstVar(kVarMoveCategory, move->category);
    SetConstVar(kVarMoveUnk18, move->unk18);
    SetConstVar(kVarUnk4f, unk4f);
    SetVar(kVarFixedDamage, 0);
    SetVar(kVarDamageIsFixed, 0);
    CallEventWithSkipChecks(flow, kEventFixedDamage);
    CallEventWithSkipChecks(flow, kEventFixedDamage2);
    u32 result = GetVar(kVarFixedDamage);
    s32 fixed = GetVar(kVarDamageIsFixed);
    if (!fixed) {
        bool ignoresBurn;
        u16 power = CalcPower(flow, attacker, defender, move);
        u16 attack = CalcAttack(flow, attacker, defender, move, critical, &ignoresBurn);
        u16 defense = CalcDefense(flow, attacker, defender, move, critical);
        u8 level = attacker->GetValue(kValueLevel);
        u32 value = BaseDamage(power, attack, level, defense);
        if (targetRatio != 0x1000) value = MulRatio(value, targetRatio);
        u8 weather = WeatherSuppressed(flow) ? 0 : flow->field->GetWeather();
        u32 ratio = WeatherRatio(weather, move->type);
        if (ratio != 0x1000) value = MulRatio(value, ratio);
        if (critical) value = value * 15 / 10;
        if (!flow->setup->HasRule(8)) {
            u16 roll = lowestRoll ? 85 : (u16)(100 - Random(16));
            value = roll * value / 100;
        }
        if (move->type != kTypeNone) value = MulRatio(value, CalcStabRatio(flow, attacker, move->type));
        value = ApplyEffectiveness(value, effectiveness);
        if (move->category == kCategoryPhysical && attacker->GetStatus() == kStatusBurn && !ignoresBurn)
            value = value * 50 / 100;
        SetRatioVar(kVarRatio, 0x1000, 0x29, 0x20000);
        SetVar(kVarDamage, value);
        SetVarRewritableOnce(0x66, 0);
        SetVarRewritableOnce(0x67, 0);
        CallEventWithSkipChecks(flow, kEventDamage);
        s32 damageRatio = GetVar(kVarRatio);
        result = MulRatio(GetVar(kVarDamage), damageRatio);
        if (result == 0) result = 1;
    }
    if (result != 0 && flow->damageRatioSwitch->on != 0) {
        SetVarRewritableOnce(kVarDamageRatio2, 0x1000);
        CallEventWithSkipChecks(flow, kEventDamageRatio2);
        result = MulRatio(result, GetVar(kVarDamageRatio2));
        if (result == 0) result = 1;
    }
    CallEventWithSkipChecks(flow, kEventDamageDone);
    PopVars();
    *damage = result;
    return fixed != 0;
}

// 0x7100a03c30
u16 CalcPower(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move) {
    u16 basePower = move->power;
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarMoveId, move->id);
    SetConstVar(kVarMoveType, move->type);
    SetConstVar(kVarMoveCategory, move->category);
    SetConstVar(kVarMoveUnk10, move->unk10);
    SetVarRewritableOnce(0x5f, 0);
    SetVarRewritableOnce(0x60, 0);
    SetVarRewritableOnce(kVarBasePower, basePower);
    CallEventWithSkipChecks(flow, kEventBasePower);
    SetRatioVar(kVarPowerRatio, 0x1000, 0x29, 0x200000);
    CallEventWithSkipChecks(flow, kEventPower);
    u16 power = GetVar(kVarBasePower);
    s32 ratio = GetVar(kVarPowerRatio);
    PopVars();
    return MulRatio(power, ratio);
}

// 0x7100a03d50
u16 CalcAttack(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, bool critical,
               bool* ignoresBurn) {
    u32 stat = move->category == kCategorySpecial ? kValueSpAtk : kValueAttack;
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetVar(kVarStatOf, kNoPokemon);
    SetVar(kVarFlag, 0);
    CallEventWithSkipChecks(flow, kEventAttackStat);
    u32 statOf = GetVar(kVarStatOf);
    BattlePokemon* pokemon = attacker;
    if ((u8)statOf != kNoPokemon) pokemon = flow->pokemon->Get(statOf);
    u16 value = StatOf(pokemon, stat, GetVar(kVarFlag) != 0, critical);
    SetConstVar(kVarMoveId, move->id);
    SetConstVar(kVarMoveType, move->type);
    SetConstVar(kVarMoveCategory, move->category);
    SetVar(kVarAttack, value);
    SetRatioVar(kVarRatio, 0x1000, 0x19a, 0x20000);
    SetVarRewritableOnce(kVarIgnoresBurn, 0);
    CallEventWithSkipChecks(flow, kEventAttack);
    u16 attack = GetVar(kVarAttack);
    s32 ratio = GetVar(kVarRatio);
    u32 result = MulRatio(attack, ratio);
    *ignoresBurn = GetVar(kVarIgnoresBurn) != 0;
    PopVars();
    return result;
}

// 0x7100a03f10
u16 CalcDefense(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, bool critical) {
    u32 category = move->category;
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    bool special = category == kCategorySpecial;
    u32 stat = special ? kValueSpDef : kValueDefense;
    SetConstVar(kVarDefenseStat, stat);
    SetVar(kVarOtherDefense, 0);
    SetVarRewritableOnce(kVarFlag, 0);
    CallEventWithSkipChecks(flow, kEventDefenseStat);
    if (GetVar(kVarOtherDefense) & 1) {
        category = special ? kCategoryPhysical : kCategorySpecial;
        stat = special ? kValueDefense : kValueSpDef;
    }
    u8 ignoreStages = GetVar(kVarFlag);
    PopVars();
    u16 value = StatOf(defender, stat, ignoreStages != 0, critical);
    if (!WeatherSuppressed(flow) && flow->field->GetWeather() == kWeatherSandstorm) {
        bool rock = defender->HasType(kTypeRock);
        if (stat == kValueSpDef && rock) value = MulRatio(value, 0x1800);
    }
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarMoveId, move->id);
    SetConstVar(kVarMoveType, move->type);
    SetConstVar(kVarMoveCategory, category);
    SetVar(kVarDefense, value);
    SetRatioVar(kVarRatio, 0x1000, 0x19a, 0x20000);
    CallEventWithSkipChecks(flow, kEventDefense);
    u16 defense = GetVar(kVarDefense);
    s32 ratio = GetVar(kVarRatio);
    PopVars();
    return MulRatio(defense, ratio);
}

// 0x7100a04170
u32 CalcStabRatio(Flow* flow, BattlePokemon* pokemon, u8 type) {
    bool has = pokemon->HasType(type);
    PushVars();
    SetConstVar(kVarPokemon, pokemon->GetId());
    SetVarRewritableOnce(kVarFlag, has);
    CallEventWithSkipChecks(flow, kEventStab);
    bool stab = GetVar(kVarFlag) != 0;
    SetConstVar(kVarStab, stab);
    SetVar(kVarRatio, stab ? 0x1800 : 0x1000);
    CallEventWithSkipChecks(flow, kEventStabRatio);
    s32 ratio = GetVar(kVarRatio);
    PopVars();
    return ratio;
}

}  // namespace battle
