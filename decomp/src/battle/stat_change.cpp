// Whether a Pokémon's stat stage can change, and changing it (battle/stat_change.h).
#include "battle/stat_change.h"

#include "battle/event.h"
#include "battle/message.h"

namespace battle {

namespace {

constexpr u32 kVarUnk1d = 0x1d;
constexpr u32 kVarStat = 0x25;
constexpr u32 kVarStages = 0x26;
constexpr u32 kVarPrevented = 0x4a;
constexpr u32 kVarUnk69 = 0x69;

constexpr u32 kEventStages = 0x71;            // Simple, Contrary
constexpr u32 kEventPrevent = 0x72;           // Clear Body, Hyper Cutter, Keen Eye, Mist
constexpr u32 kEventPreventedMessage = 0x73;
constexpr u32 kEventChanged = 0x74;           // Defiant, Competitive

constexpr u32 kCommandStatUp = 0xa;
constexpr u32 kCommandStatDown = 0xb;
constexpr u32 kCommandStatUpExtra = 0x4d;
constexpr u32 kCommandStatDownExtra = 0x4e;
constexpr u32 kCommandPokemonMessage = 0x7c;  // ServerCommands::PutMessage: a message starting with a name
constexpr u32 kMessageEnd = 0xffff0000;
constexpr u32 kMessageRose = 0x24;
constexpr u32 kMessageRoseNamed = 0x4f0;
constexpr u32 kMessageFell = 0x78;
constexpr u32 kMessageNoHigher = 0xcc;
constexpr u32 kMessageNoLower = 0xe8;
constexpr u32 kMessageUnaffected = 0x118;

constexpr u8 kFailedFlag = 0x20;              // Flow::unk1511

}  // namespace

// 0x71009fa4b0
void ApplyStatStage(Flow* flow, BattlePokemon* pokemon, u32 stat, s32 stages, u16 unk4, bool message, bool extra) {
    u8 id = pokemon->GetId();
    if (stages >= 1) {
        u8 risen = pokemon->RaiseStage(stat, stages);
        flow->commands->Put(kCommandStatUp, id, static_cast<u8>(stat), risen);
        if (extra) flow->commands->Put(kCommandStatUpExtra, id, static_cast<u8>(stat), risen);
        if (!message) return;
        if (unk4 != 0)
            flow->commands->PutMessage(kCommandPokemonMessage, kMessageRoseNamed, id, unk4, stat, risen, kMessageEnd);
        else
            flow->commands->PutMessage(kCommandPokemonMessage, kMessageRose, id, stat, risen, kMessageEnd);
        return;
    }
    u8 fallen = pokemon->LowerStage(stat, -stages);
    flow->commands->Put(kCommandStatDown, id, static_cast<u8>(stat), fallen);
    if (extra) flow->commands->Put(kCommandStatDownExtra, id, static_cast<u8>(stat), fallen);
    flow->commands->PutMessage(kCommandPokemonMessage, kMessageFell, id, stat, fallen, kMessageEnd);
}

// 0x7100a05830
bool CanChangeStatStage(Flow* flow, u8 causer, BattlePokemon* target, u32 stat, s32 stages, u32 unk69, u8 unk3,
                        u32 /* unused */, u32 unk1d, bool message, bool ignoresSubstitute, bool quiet) {
    if (IsSideOut(flow) && EndsWhenSideOut(flow->setup->GetUnk168())) return false;
    PushVars();
    SetConstVar(kVarPokemon, target->GetId());
    SetConstVar(kVarAttacker, unk3);
    SetConstVar(kVarStat, stat);
    SetVarRewritableOnce(kVarStages, stages);
    CallEventWithSkipChecks(flow, kEventStages);
    s32 finalStages = GetVar(kVarStages);
    PopVars();
    if (!target->CanChangeStage(stat, finalStages)) {
        if (message && !quiet) {
            u8 id = target->GetId();
            flow->commands->PutMessage(kCommandPokemonMessage, finalStages < 1 ? kMessageNoLower : kMessageNoHigher,
                                       id, stat, kMessageEnd);
            flow->unk1511 |= kFailedFlag;
        }
        flow->statFailures.Add(kStatFailLimit);
        return false;
    }
    if (target->HasSubstitute() && !ignoresSubstitute) {
        u8 id = target->GetId();
        if (id != causer) {
            if (message && !quiet) flow->commands->PutMessage(kCommandPokemonMessage, kMessageUnaffected, id, kMessageEnd);
            flow->statFailures.Add(kStatFailSubstitute);
            return false;
        }
    }
    if (IsStatChangeAllowed(flow, target, stat, causer, finalStages, unk69, unk1d)) return true;
    flow->statFailures.Add(kStatFailPrevented);
    if (!message || quiet) return false;
    flow->unk1511 |= kFailedFlag;
    u32 saved = flow->unk2808.Save();
    PushVars();
    SetConstVar(kVarPokemon, target->GetId());
    SetConstVar(kVarUnk1d, unk1d);
    CallEventWithSkipChecks(flow, kEventPreventedMessage);
    PopVars();
    flow->unk2808.Restore(saved);
    return false;
}

// 0x7100a05a80
bool ChangeStatStage(Flow* flow, u8 causer, BattlePokemon* target, u32 stat, s32 stages, u32 unk69, u8 unk3,
                     u32 unk7, u32 unk1d, bool message, bool riseMessage, const BattleMessage* preMessage,
                     bool ignoresSubstitute, bool extra) {
    bool changed = CanChangeStatStage(flow, causer, target, stat, stages, unk69, unk3, unk7, unk1d, message,
                                      ignoresSubstitute, false);
    if (changed) {
        if (preMessage) PutBattleMessage(flow, preMessage);
        PushVars();
        SetConstVar(kVarPokemon, target->GetId());
        SetConstVar(kVarAttacker, unk3);
        SetConstVar(kVarStat, stat);
        SetVarRewritableOnce(kVarStages, stages);
        CallEventWithSkipChecks(flow, kEventStages);
        s32 finalStages = GetVar(kVarStages);
        PopVars();
        ApplyStatStage(flow, target, stat, finalStages, unk7, riseMessage, extra);
        u32 saved = flow->unk2808.Save();
        PushVars();
        SetConstVar(kVarAttacker, causer);
        SetConstVar(kVarPokemon, target->GetId());
        SetConstVar(kVarStat, stat);
        SetConstVar(kVarStages, finalStages);
        CallEventWithSkipChecks(flow, kEventChanged);
        PopVars();
        flow->unk2808.Restore(saved);
    }
    return changed;
}

// 0x7100a05c10
bool IsStatChangeAllowed(Flow* flow, BattlePokemon* target, u32 stat, u8 causer, s32 stages, u32 unk69, u32 unk1d) {
    PushVars();
    SetConstVar(kVarPokemon, target->GetId());
    SetConstVar(kVarAttacker, causer);
    SetConstVar(kVarStat, stat);
    SetConstVar(kVarStages, stages);
    SetConstVar(kVarUnk69, unk69);
    SetConstVar(kVarUnk1d, unk1d);
    SetVarRewritableOnce(kVarPrevented, 0);
    CallEventWithSkipChecks(flow, kEventPrevent);
    s32 prevented = GetVar(kVarPrevented);
    PopVars();
    return prevented == 0;
}

}  // namespace battle
