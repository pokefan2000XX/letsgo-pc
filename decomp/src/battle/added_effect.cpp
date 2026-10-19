// The chances of a move's added effects (battle/added_effect.h).
#include "battle/added_effect.h"

#include "battle/add_condition.h"
#include "battle/calc.h"
#include "battle/event.h"
#include "moves/data.h"

namespace battle {

namespace {

constexpr u32 kVarMoveId = 0x12;
constexpr u32 kVarCondition = 0x21;
constexpr u32 kVarStat = 0x25;
constexpr u32 kVarStages = 0x26;
constexpr u32 kVarMoveFlinch = 0x2b;
constexpr u32 kVarChance = 0x2c;
constexpr u32 kVarTimes = 0x3b;               // event 0x70: the stages are multiplied by it
constexpr u32 kVarFlag = 0x4a;
constexpr u32 kVarDoubles = 0x4e;             // Serene Grace

constexpr u32 kEventAddedEffect = 0x67;
constexpr u32 kEventStatChange = 0x70;
constexpr u32 kEventMoveCondition = 0x7b;
constexpr u32 kEventFlinchChance = 0x83;
constexpr u32 kEventFlinch = 0x84;
constexpr u32 kEventFlinchPrevented = 0x85;

constexpr u32 kRuleNoAddedEffects = 0xf;
constexpr u32 kRuleAddedEffectsAlways = 0;
constexpr s32 kNoCondition = 0xffff;
constexpr s32 kStatAll = 8;

constexpr u32 kFlagFlinched = 4;
constexpr u32 kFlagFocusing = 5;              // being hit makes it lose its focus
constexpr u32 kFlagHitBeforeMoving = 6;       // Focus Punch fails
constexpr u32 kChanceSure = 100;

constexpr u32 kEffectCondition = 4;           // moves::GetEffectKind
constexpr u32 kEffectStatChange = 6;

}  // namespace

// 0x7100a038a0
u32 GetFlinchChance(Flow* flow, u32 move, BattlePokemon* attacker) {
    u32 chance = moves::GetFlinchChance(move);
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarMoveFlinch, chance);
    SetVarRewritableOnce(kVarDoubles, 0);
    SetVarRewritableOnce(kVarFlag, 0);
    SetVar(kVarChance, 0);
    CallEventWithSkipChecks(flow, kEventFlinchChance);
    s32 cancelled = GetVar(kVarFlag);
    s32 doubles = GetVar(kVarDoubles);
    s32 extra = GetVar(kVarChance);
    PopVars();
    if (chance != 0 && cancelled != 0) return 0;
    return ((chance == 0 ? extra : 0) + chance) << (doubles != 0);
}

// 0x7100a03a50
bool RollFlinch(Flow* flow, BattlePokemon* target, u32 chance) {
    PushVars();
    SetConstVar(kVarDefender, target->GetId());
    SetVar(kVarChance, chance);
    SetVarRewritableOnce(kVarFlag, 0);
    CallEventWithSkipChecks(flow, kEventFlinch);
    s32 finalChance = GetVar(kVarChance);
    s32 prevented = GetVar(kVarFlag);
    PopVars();
    bool never = flow->setup->HasRule(kRuleNoAddedEffects);
    if (finalChance == 0 || prevented != 0 || never) return false;
    if (flow->setup->HasRule(kRuleAddedEffectsAlways)) return true;
    return Random(100) < static_cast<u8>(finalChance);
}

// 0x7100a03240
void ApplyAddedEffect(Flow* flow, const MoveParam* move, BattlePokemon* attacker, BattlePokemon* defender,
                      u32 /* unused */, bool skip) {
    u32 kind = moves::GetEffectKind(move->id);
    if (kind == kEffectCondition) {
        if (skip) return;
        u64 data;
        u32 condition = RollMoveCondition(flow, move->id, attacker, defender, &data);
        if (condition == 0 || defender->IsFainted()) return;
        GiveMoveCondition(flow, attacker, defender, 0, condition, data, 0, 0, false, false);
    } else if (kind == kEffectStatChange) {
        if (skip || defender->IsFainted() || !RollAddedEffect(flow, move, attacker, defender)) return;
        flow->statChangeNumber = flow->statChangeNumber == 0xffffffff ? 1 : flow->statChangeNumber + 1;
        ApplyMoveStatChanges(flow, move, attacker, defender, flow->statChangeNumber, false, false);
    }
}

// 0x7100a03990
bool ApplyFlinch(Flow* flow, BattlePokemon* target, u32 chance) {
    if (target->HasFlag(kFlagFocusing)) {
        target->SetFlag(kFlagHitBeforeMoving);
        return true;
    }
    if (RollFlinch(flow, target, chance)) {
        target->SetFlag(kFlagFlinched);
        return true;
    }
    if (chance >= kChanceSure) {
        u32 saved = flow->unk2808.Save();
        PushVars();
        SetVar(kVarPokemon, target->GetId());
        CallEventWithSkipChecks(flow, kEventFlinchPrevented);
        PopVars();
        flow->unk2808.Restore(saved);
    }
    return false;
}

// 0x7100a04b50
u32 RollMoveCondition(Flow* flow, u32 move, BattlePokemon* attacker, BattlePokemon* defender, u64* conditionData) {
    u32 condition = moves::GetInflictCondition(move);
    u16 duration = moves::GetInflictDuration(move);
    u32 chance = moves::GetInflictChance(move);
    u64 data;                                 // as in the original, left as it is for a duration of another kind
    MakeConditionData(duration, attacker, &data);
    PushVars();
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarMoveId, move);
    SetPointerVars23(&data);
    SetVar(kVarCondition, condition);
    SetVarRewritableOnce(kVarFlag, 0);
    SetVar(kVarChance, chance);
    CallEventWithSkipChecks(flow, kEventMoveCondition);
    s32 finalCondition = GetVar(kVarCondition);
    s32 finalChance = GetVar(kVarChance);
    s32 prevented = GetVar(kVarFlag);
    u32 given = finalCondition == kNoCondition ? 0 : finalCondition;
    PopVars();
    if (flow->setup->HasRule(kRuleNoAddedEffects) || given == 0 || finalChance < 1 ||
        static_cast<u8>(prevented) != 0)
        return 0;
    if (Random(100) >= static_cast<u8>(finalChance) && !flow->setup->HasRule(kRuleAddedEffectsAlways)) return 0;
    *conditionData = data;
    return given;
}

// 0x7100a05600
bool RollAddedEffect(Flow* flow, const MoveParam* move, BattlePokemon* attacker, BattlePokemon* defender) {
    u8 chance = moves::GetStatChangeChance(move->id, 0);
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarMoveId, move->id);
    SetVarRewritableOnce(kVarFlag, 0);
    SetVar(kVarChance, chance);
    CallEventWithSkipChecks(flow, kEventAddedEffect);
    s32 prevented = GetVar(kVarFlag);
    s32 finalChance = GetVar(kVarChance);
    PopVars();
    if (flow->setup->HasRule(kRuleNoAddedEffects) || static_cast<u8>(prevented) != 0) return false;
    return Random(100) < static_cast<u8>(finalChance) || flow->setup->HasRule(kRuleAddedEffectsAlways);
}

// 0x7100a05720
void GetMoveStatChange(Flow* flow, u32 move, u32 index, BattlePokemon* attacker, BattlePokemon* defender, s32* stat,
                       s32* stages) {
    *stat = moves::GetStatChange(move, index, stages);
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetVar(kVarStat, *stat);
    SetVar(kVarStages, *stages);
    SetVar(kVarTimes, 1);
    CallEventWithSkipChecks(flow, kEventStatChange);
    *stat = GetVar(kVarStat);
    *stages = GetVar(kVarStages);
    u8 times = GetVar(kVarTimes);
    if (times >= 2) *stages *= times;
    PopVars();
    if (*stat == kStatAll) *stat = 0;
}

}  // namespace battle
