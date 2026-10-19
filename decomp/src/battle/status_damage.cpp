// A condition's damage at the end of a turn (battle/status_damage.h): the part in the battle's main file.
#include "battle/status_damage.h"

#include "battle/event.h"

namespace battle {

namespace {

constexpr u32 kVarCondition = 0x21;
constexpr u32 kVarDamageCause = 0x6a;
constexpr u32 kVarTakesDamage = 0x5b;

constexpr u32 kEventConditionDamage = 0x82;   // Poison Heal, Heatproof
constexpr u32 kEventIndirectDamage = 0x98;    // Magic Guard

// What caused the damage (variable 0x6a), for conditions 4 to 10
constexpr u32 kDamageCauses[7] = {39, 38, 0, 0, 0, 40, 41};

constexpr u32 kCommandReaction = 0x65;

}  // namespace

// 0x7100a07f90
void StatusDamage(Flow* flow, BattlePokemon* pokemon, u32 condition, u32 damage) {
    u32 saved = flow->unk2808.Save();
    PushVars();
    SetConstVar(kVarPokemon, pokemon->GetId());
    SetConstVar(kVarCondition, condition);
    SetVar(kVarDamage, damage);
    CallEventWithSkipChecks(flow, kEventConditionDamage);
    s32 finalDamage = GetVar(kVarDamage);
    PopVars();
    if (finalDamage != 0) {
        u32 index = condition - kConditionBurn;
        u32 cause = index < 7 ? kDamageCauses[index] : 0;
        u64 entry = pokemon->GetCondition(condition);
        u32 causedBy = ConditionCause(&entry);
        PushVars();
        SetConstVar(kVarDefender, pokemon->GetId());
        SetConstVar(kVarDamageCause, cause);
        SetVarRewritableOnce(kVarTakesDamage, 1);
        CallEventWithSkipChecks(flow, kEventIndirectDamage);
        s32 takesDamage = GetVar(kVarTakesDamage);
        PopVars();
        if (takesDamage != 0) {
            u32 reaction = 0;
            switch (condition) {
            case kConditionBurn: reaction = 0x2c; break;
            case kConditionPoison: reaction = 0x2b; break;
            case kConditionNightmare: reaction = 0x44; break;
            case kConditionCurse: reaction = 0x43; break;
            }
            if (reaction != 0) {
                u8 slot = flow->unk1798.IndexOf(pokemon->GetId());
                if (slot != 4) flow->commands->Put(kCommandReaction, slot, reaction);
            }
            BattleMessage* message = &flow->message;
            message->Clear();
            SetConditionDamageMessage(message, pokemon, condition);
            ApplyIndirectDamage(flow, pokemon, finalDamage, cause, causedBy, message, true);
        }
    }
    flow->unk2808.Restore(saved);
}

}  // namespace battle
