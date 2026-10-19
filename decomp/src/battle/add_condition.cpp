// Whether a condition can be given to a Pokémon (battle/add_condition.h).
#include "battle/add_condition.h"

#include "battle/calc.h"
#include "battle/event.h"
#include "battle/message.h"
#include "moves/data.h"

namespace battle {

namespace {

constexpr u32 kVarUnk1d = 0x1d;
constexpr u32 kVarCondition = 0x21;
constexpr u32 kVarFlag = 0x4a;
constexpr u32 kVarShowsMessage = 0x4c;
constexpr u32 kVarIgnoresImmunity = 0x5b;
constexpr u32 kVarCause = 0x6c;

constexpr u32 kEventPickCondition = 0x77;     // Telekinesis
constexpr u32 kEventMoveCondition = 0x78;     // the binding moves
constexpr u32 kEventConditionData = 0x79;     // Leech Seed, Foresight, Grip Claw
constexpr u32 kEventAbilityOff = 0x81;        // Gastro Acid: Flower Gift, Mold Breaker, Damp
constexpr u32 kEventBeforeAbilityChange = 0xcc;
constexpr u32 kEventImmune = 0x7c;            // Limber, Insomnia, Leaf Guard, Safeguard
constexpr u32 kEventImmuneMessage = 0x7e;
constexpr u32 kEventIgnoresImmunity = 0xd3;   // Corrosion
constexpr u32 kEventNoWeather = 0x92;         // Air Lock, Cloud Nine

constexpr s32 kConditionYawn = 0xe;
constexpr s32 kConditionGastroAcid = 0x10;
constexpr s32 kConditionLeechSeed = 0x12;
constexpr s32 kConditionPerishSong = 0x14;
constexpr s32 kConditionUnk0f = 0xf;

constexpr u32 kTypePoison = 3;
constexpr u32 kTypeSteel = 8;
constexpr u32 kTypeGrass = 0xb;
constexpr u32 kTypeElectric = 0xc;
constexpr u32 kTypeIce = 0xe;

constexpr u32 kConditionFromEvent = 0xffff;   // event 0x77 picks the condition
constexpr u32 kModeDefault = 0;
constexpr u32 kModeAgain = 2;                 // the condition may be given again
constexpr u32 kCauseUnk3 = 3;

constexpr u32 kCommandReaction = 0x65;        // to the entry Unk1798::IndexOf gives
constexpr u32 kCommandForm = 0x69;
constexpr u32 kCommandMessage = 0x7b;         // ServerCommands::PutMessage: a message without a name first
constexpr u32 kCommandPokemonMessage = 0x7c;  // one starting with a Pokémon's name
constexpr u32 kMessageEnd = 0xffff0000;

constexpr u32 kMessageFormChanged = 0x128;
constexpr u8 kNoEntry = 4;                    // Unk1798::IndexOf
constexpr u32 kConditionFreeze = 3;
constexpr u32 kSpeciesShaymin = 492;
constexpr u8 kFormSky = 1;
constexpr u32 kAbilityUnnerve = 127;
constexpr u8 kItemReactionCondition = 4;      // CallItemReaction's point

// The clients' reaction to a status given: paralysis, sleep, freeze, burn, poison, confusion, love
constexpr u32 kStatusReactions[7] = {0x2e, 0x2a, 0x2d, 0x2c, 0x2b, 0x2f, 0x30};

constexpr u8 kFailedFlag = 0x20;              // Flow::unk1511
constexpr u8 kFailedQuietlyFlag = 0x80;

// Whether one of a Pokémon's three types (BattlePokemon::GetTypes) is type.
bool TypesInclude(u32 types, u32 type) {
    return (types & 0x1f) == type || (types & 0x3e0) == type << 5 || (types & 0x7c00) == type << 10;
}

// "X is already ..." for the statuses and confusion
constexpr u32 kAlreadyMessages[6] = {0x178, 0x1a4, 0x18c, 0x164, 0x14c, 0x1d8};
// "It doesn't affect ..." by status, for cause 3
constexpr u32 kImmuneMessages[6] = {0x17c, 0x1a8, 0x190, 0x168, 0x150, 0x1dc};

}  // namespace

// 0x71009fdd30
bool CanMoveGiveCondition(Flow* flow, u32 move, BattlePokemon* attacker, BattlePokemon* defender) {
    u32 condition = moves::GetInflictCondition(move);
    u64 data;  // as in the original, left as it is for a duration of another kind (it isn't read)
    MakeConditionData(moves::GetInflictDuration(move), attacker, &data);
    if (condition == kConditionFromEvent) {
        PushVars();
        SetConstVar(kVarAttacker, attacker->GetId());
        SetConstVar(kVarDefender, defender->GetId());
        SetPointerVars47(&flow->message);
        SetVar(kVarCondition, 0);
        CallEventWithSkipChecks(flow, kEventPickCondition);
        condition = GetVar(kVarCondition);
        PopVars();
        if (condition == 0 || condition == kConditionFromEvent) return false;
    }
    return !CheckConditionPrevented(flow, defender, attacker, condition, &data, 0, 0, kModeDefault, true, false);
}

// 0x71009fde60
bool CheckConditionPrevented(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, s32 condition,
                             const u64* /* conditionData */, u32 cause, u32 unk1d, u32 mode, bool message,
                             bool quietIfFailed) {
    bool showMessage = message;
    u32 reason;
    bool has = target->HasCondition(condition);
    if (mode != kModeAgain && has) {
        reason = kConditionFailAlready;
    } else if (condition < 6 && target->GetStatus() != 0 && mode == 0) {
        reason = kConditionFailFails;
    } else {
        if (IgnoresConditionImmunity(flow, target, attacker, condition, cause) ||
            (reason = GetConditionImmunity(flow, target, condition)) == kConditionFailNone) {
            u32 saved = flow->unk2808.Save();
            bool prevented = CallConditionImmunityEvent(flow, attacker, target, condition, cause, &showMessage);
            if (showMessage && prevented) {
                PushVars();
                SetConstVar(kVarDefender, target->GetId());
                SetConstVar(kVarAttacker, attacker->GetId());
                SetConstVar(kVarCondition, condition);
                SetConstVar(kVarCause, cause);
                SetConstVar(kVarUnk1d, unk1d);
                CallEventWithSkipChecks(flow, kEventImmuneMessage);
                PopVars();
                flow->unk1511 |= kFailedFlag;
            }
            flow->unk2808.Restore(saved);
            return prevented;
        }
    }
    if (message && !(cause == kCauseUnk3 && reason == kConditionFailFails && quietIfFailed))
        PutConditionFailMessage(flow, target, reason, condition, cause);
    return true;
}

// 0x71009ff450
void AddCondition(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, u32 condition, const u64* conditionData,
                  bool message, bool noItemReaction, const BattleMessage* otherMessage) {
    SetCondition(flow, target, condition, conditionData);
    u32 status = condition - 1;
    if (status < 7) {
        u8 index = flow->unk1798.IndexOf(target->GetId());
        if (index != kNoEntry) flow->commands->Put(kCommandReaction, index, kStatusReactions[status]);
    }
    if (message) {
        MakeConditionMessage(condition, conditionData, target, &flow->message);
        PutBattleMessage(flow, &flow->message);
        flow->message.Clear();
    } else if (otherMessage) {
        PutBattleMessage(flow, otherMessage);
    }
    if (condition == kConditionFreeze && target->GetSpecies() == kSpeciesShaymin && target->form == kFormSky) {
        PushVars();
        SetConstVar(kVarPokemon, target->GetId());
        CallEventWithSkipChecks(flow, kEventBeforeAbilityChange);
        PopVars();
        RemoveAbilityHandlers(target);
        target->ChangeForm(0, false);
        flow->commands->Put(kCommandForm, target->GetId(), 0, 0);
        AddAbilityHandlers(target);
        flow->commands->PutMessage(kCommandPokemonMessage, kMessageFormChanged, target->GetId(), kMessageEnd);
    }
    u32 saved = flow->unk2808.Save();
    if (IsStatusCondition(condition)) {
        StatusAdded(flow, target, attacker, condition, *conditionData);
    } else if (condition == static_cast<u32>(kConditionGastroAcid)) {
        PushVars();
        SetConstVar(kVarPokemon, target->GetId());
        CallEvent(flow, kEventAbilityOff);
        PopVars();
        if (target->GetValue(kValueBaseAbility) == kAbilityUnnerve) UnnerveSuppressed(flow, target);
    } else {
        ConditionAdded(flow, target, attacker, condition);
    }
    flow->unk2808.Restore(saved);
    if (!noItemReaction) CallItemReaction(flow, target, kItemReactionCondition);
}

// 0x7100a04cf0
bool GiveMoveCondition(Flow* flow, BattlePokemon* attacker, BattlePokemon* defender, u32 /* unused */, u32 condition,
                       u64 conditionData, u32 cause, u32 unk1d, bool message, bool quietIfFailed) {
    flow->message.Clear();
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetPointerVars47(&flow->message);
    if (condition == kConditionFromEvent) {
        SetVar(kVarCondition, 0);
        CallEventWithSkipChecks(flow, kEventPickCondition);
        condition = GetVar(kVarCondition);
        PopVars();
        if (condition == 0 || condition == kConditionFromEvent) return false;
    } else {
        SetConstVar(kVarCondition, condition);
        CallEventWithSkipChecks(flow, kEventMoveCondition);
        PopVars();
    }
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, defender->GetId());
    SetConstVar(kVarCondition, condition);
    SetPointerVars23(&conditionData);
    CallEventWithSkipChecks(flow, kEventConditionData);
    PopVars();
    bool eventMessage = flow->message.HasText();
    if (CheckConditionPrevented(flow, defender, attacker, condition, &conditionData, cause, unk1d, kModeDefault, message,
                                quietIfFailed))
        return false;
    AddCondition(flow, defender, attacker, condition, &conditionData, !eventMessage, 0, nullptr);
    if (eventMessage) {
        PutBattleMessage(flow, &flow->message);
        flow->message.Clear();
    }
    return true;
}

// 0x7100a04ef0
bool IgnoresConditionImmunity(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, s32 condition, u32 cause) {
    if (!attacker) return false;
    PushVars();
    SetConstVar(kVarAttacker, attacker->GetId());
    SetConstVar(kVarDefender, target->GetId());
    SetConstVar(kVarCondition, condition);
    SetConstVar(kVarCause, cause);
    SetVarRewritableOnce(kVarIgnoresImmunity, 0);
    CallEventWithSkipChecks(flow, kEventIgnoresImmunity);
    bool ignores = GetVar(kVarIgnoresImmunity) != 0;
    PopVars();
    return ignores;
}

// 0x7100a04fb0
u32 GetConditionImmunity(Flow* flow, BattlePokemon* pokemon, s32 condition) {
    PushVars();
    SetVarRewritableOnce(kVarFlag, 0);
    CallEventWithSkipChecks(flow, kEventNoWeather);
    s32 noWeather = GetVar(kVarFlag);
    PopVars();
    bool sunny = IsSunny(noWeather ? 0 : flow->field->GetWeather());
    if (condition == static_cast<s32>(kStatusFreeze) && sunny) return kConditionFailFails;
    switch (condition) {
    case kConditionBurn:
        return TypesInclude(pokemon->GetTypes(), kTypeFire) ? kConditionFailImmune : kConditionFailNone;
    case kConditionPoison: {
        u32 types = pokemon->GetTypes();
        return TypesInclude(types, kTypeSteel) || TypesInclude(types, kTypePoison) ? kConditionFailImmune : kConditionFailNone;
    }
    case kStatusFreeze:
        return TypesInclude(pokemon->GetTypes(), kTypeIce) ? kConditionFailImmune : kConditionFailNone;
    case kStatusParalysis:
        return TypesInclude(pokemon->GetTypes(), kTypeElectric) ? kConditionFailImmune : kConditionFailNone;
    case kConditionYawn:
        return pokemon->GetStatus() != 0 ? kConditionFailFails : kConditionFailNone;
    case kConditionGastroAcid:
        return IsAbilityPermanent(pokemon->GetValue(kValueBaseAbility)) ? kConditionFailFails : kConditionFailNone;
    case kConditionLeechSeed:
        return TypesInclude(pokemon->GetTypes(), kTypeGrass) ? kConditionFailImmune : kConditionFailNone;
    }
    return kConditionFailNone;
}

// 0x7100a05360
bool CallConditionImmunityEvent(Flow* flow, BattlePokemon* attacker, BattlePokemon* target, s32 condition, u32 cause,
                                bool* message) {
    PushVars();
    SetConstVar(kVarAttacker, attacker ? attacker->GetId() : kNoPokemon);
    SetConstVar(kVarDefender, target->GetId());
    SetConstVar(kVarCondition, condition);
    SetConstVar(kVarCause, cause);
    SetVarRewritableOnce(kVarFlag, 0);
    SetVarRewritableOnce(kVarShowsMessage, *message);
    CallEventWithSkipChecks(flow, kEventImmune);
    bool prevented = GetVar(kVarFlag) != 0;
    *message = GetVar(kVarShowsMessage) != 0;
    PopVars();
    return prevented;
}

// 0x7100a051f0
void PutConditionFailMessage(Flow* flow, BattlePokemon* pokemon, u32 reason, s32 condition, u32 cause) {
    u8 id = pokemon->GetId();
    u32 text;
    if (reason == kConditionFailImmune) {
        u32 index = condition - 1;
        text = cause == kCauseUnk3 && index < 6 ? kImmuneMessages[index] : 0x118;
    } else if (reason == kConditionFailAlready && static_cast<u32>(condition - 1) < 0x14) {
        if (condition <= 6) {
            text = kAlreadyMessages[condition - 1];
        } else if (condition == kConditionUnk0f) {
            flow->commands->PutMessage(kCommandPokemonMessage, 0x20, id, kMessageEnd);
            flow->unk1511 |= kFailedFlag;
            return;
        } else if (condition == kConditionPerishSong) {
            flow->unk1511 |= kFailedQuietlyFlag;
            return;
        } else {
            flow->commands->PutMessage(kCommandMessage, 0x51, kMessageEnd);
            flow->unk1511 |= kFailedFlag;
            return;
        }
    } else {
        flow->commands->PutMessage(kCommandMessage, 0x51, kMessageEnd);
        flow->unk1511 |= kFailedFlag;
        return;
    }
    flow->commands->PutMessage(kCommandPokemonMessage, text, id, kMessageEnd);
    flow->unk1511 |= kFailedFlag;
}

}  // namespace battle
