// Whether a move hits, and critical hits (battle/hit.h).
#include "battle/hit.h"

#include "battle/calc.h"
#include "battle/event.h"
#include "moves/data.h"

namespace battle {

namespace {

constexpr u32 kVarMoveId = 0x12;
constexpr u32 kVarMoveCategory = 0x1e;
constexpr u32 kVarAccuracyStage = 0x2d;
constexpr u32 kVarEvasionStage = 0x2e;
constexpr u32 kVarAccuracy = 0x31;
constexpr u32 kVarCriticalStage = 0x32;
constexpr u32 kVarNoCritical = 0x4a;        // Battle Armor, Lucky Chant
constexpr u32 kVarIgnoresEvasion = 0x54;    // Keen Eye, Chip Away
constexpr u32 kVarHits = 0x5b;

constexpr u32 kEventSkipsAccuracyCheck = 0x3f;
constexpr u32 kEventStages = 0x40;
constexpr u32 kEventAccuracy = 0x41;
constexpr u32 kEventCritical = 0x44;
constexpr u32 kEventOneHitKoAccuracy = 0x42;  // Sheer Cold
constexpr u32 kEventOneHitKo = 0x87;          // Sturdy
constexpr u32 kVarProtected = 0x4a;         // Sturdy

constexpr u32 kConditionForesight = 0x11;   // a raised evasion doesn't count
constexpr u32 kConditionHitsAll = 0x1d;
constexpr u32 kConditionLockOn = 0x1e;      // its parameter is the target
constexpr u32 kConditionTelekinesis = 0x21;
constexpr u32 kFlagMicleBerry = 0xe;
constexpr u32 kRuleAlwaysHit = 7;

constexpr u8 kStageNone = 6;
constexpr u8 kCriticalAlways = 6;
constexpr u8 kFriendshipDodges = 200;
constexpr u8 kFriendshipMax = 255;

}  // namespace

// 0x71009fd1e0
bool AlwaysHits(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender) {
    if (flow->unk1798.Check(flow->setup, attacker->GetId()) && attacker->GetValue(kValueAbility) == kAbilityNoGuard)
        return true;
    if (flow->unk1798.Check(flow->setup, defender->GetId()) && defender->GetValue(kValueAbility) == kAbilityNoGuard)
        return true;
    if (attacker->HasCondition(kConditionHitsAll)) return true;
    return attacker->HasCondition(kConditionLockOn) &&
           static_cast<u8>(attacker->GetConditionParam(kConditionLockOn)) == defender->GetId();
}

// 0x71009fe7a0
bool CheckHit(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move, bool* dodged) {
    if (dodged) *dodged = false;
    if (AlwaysHits(flow, attacker, defender) || SkipsAccuracyCheck(flow, attacker, defender, move->id) ||
        defender->HasCondition(kConditionTelekinesis))
        return true;
    u8 accuracy = CalcAccuracy(flow, kEventAccuracy, attacker, defender, move);
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetVarRewritableOnce(kVarIgnoresEvasion, 0);
    SetVar(kVarAccuracyStage, attacker->GetValue(kValueAccuracyStage));
    SetVar(kVarEvasionStage, defender->GetValue(kValueEvasionStage));
    SetRatioVar(kVarRatio, 0x1000, 0x29, 0x20000);
    CallEventWithSkipChecks(flow, kEventStages);
    s8 accuracyStage = GetVar(kVarAccuracyStage);
    s8 evasionStage = GetVar(kVarEvasionStage);
    u8 ignoresEvasion = GetVar(kVarIgnoresEvasion);
    if ((defender->HasCondition(kConditionForesight) && evasionStage > kStageNone) || ignoresEvasion)
        evasionStage = kStageNone;
    if (attacker->HasFlag(kFlagMicleBerry)) MultiplyVar(kVarRatio, 0x1333);
    u32 ratio = GetVar(kVarRatio);
    PopVars();
    s32 stage = accuracyStage + kStageNone - evasionStage;
    if (stage < 0) stage = 0;
    if (stage > 12) stage = 12;
    u32 percent = MulRatio(AccuracyAfterStages(accuracy, stage), ratio);
    if (percent > 100) percent = 100;
    if (flow->setup->GetFriendship(defender) >= kFriendshipDodges) {
        percent = percent > 10 ? percent - 10 : 0;
        if (dodged) *dodged = true;
    }
    bool alwaysHit = flow->setup->HasRule(kRuleAlwaysHit);
    return Random(100) < (alwaysHit ? 100 : percent);
}

// 0x71009fe9f0
bool SkipsAccuracyCheck(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, u32 move) {
    if (moves::NeverMisses(move)) return true;
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetVarRewritableOnce(kVarHits, 0);
    CallEventWithSkipChecks(flow, kEventSkipsAccuracyCheck);
    bool skips = GetVar(kVarHits) != 0;
    PopVars();
    return skips;
}

// 0x71009fea90
u32 CalcAccuracy(Flow* flow, u32 event, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move) {
    u16 accuracy = moves::GetAccuracy(move->id);
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarMoveId, move->id);
    SetConstVar(kVarMoveCategory, move->category);
    SetVarRewritableOnce(kVarAccuracy, accuracy);
    SetRatioVar(kVarRatio, 0x1000, 0x19a, 0x20000);
    CallEventWithSkipChecks(flow, event);
    s32 value = GetVar(kVarAccuracy);
    s32 ratio = GetVar(kVarRatio);
    PopVars();
    return MulRatio(value, ratio);
}

// 0x7100a05de0
bool CheckOneHitKo(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, const MoveParam* move) {
    u8 attackerLevel = attacker->GetValue(kValueLevel);
    u8 defenderLevel = defender->GetValue(kValueLevel);
    if (attackerLevel < defenderLevel) return false;
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetVarRewritableOnce(kVarProtected, 0);
    CallEventWithSkipChecks(flow, kEventOneHitKo);
    s32 prevented = GetVar(kVarProtected);
    PopVars();
    if (prevented) return false;
    if (AlwaysHits(flow, attacker, defender)) return true;
    u8 chance = attackerLevel - defenderLevel + CalcAccuracy(flow, kEventOneHitKoAccuracy, attacker, defender, move);
    return Random(100) < chance;
}

// 0x7100a09250
bool CheckCritical(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, u32 move, bool* friendship) {
    u8 moveStage = moves::GetCriticalStage(move);
    u32 stage = moveStage;
    if (moveStage != kCriticalAlways) stage = moveStage + attacker->GetCriticalStage();
    PushVars();
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarAttacker, attacker->GetId());
    SetVar(kVarCriticalStage, static_cast<u16>(stage));
    SetVarRewritableOnce(kVarNoCritical, 0);
    CallEventWithSkipChecks(flow, kEventCritical);
    s32 prevented = GetVar(kVarNoCritical);
    bool critical = prevented == 0;
    if (moveStage != kCriticalAlways && prevented == 0) {
        s32 finalStage = GetVar(kVarCriticalStage);
        if (finalStage > 3) finalStage = 3;
        u8 points = flow->setup->GetFriendship(attacker);
        critical = RollCritical(finalStage, points == kFriendshipMax ? 2 : 1);
        if (critical) *friendship = points == kFriendshipMax;
    }
    PopVars();
    return critical;
}

}  // namespace battle
