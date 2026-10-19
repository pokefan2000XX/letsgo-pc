// Whether a Pokémon can use its move this turn (battle/before_move.h).
#include "battle/before_move.h"

#include "battle/calc.h"
#include "battle/event.h"
#include "moves/data.h"

namespace battle {

namespace {

constexpr u32 kVarMoveId = 0x12;
constexpr u32 kVarCalled = 0x17;
constexpr u32 kVarMoveType = 0x19;
constexpr u32 kVarMoveUnk18 = 0x1c;
constexpr u32 kVarMoveCategory = 0x1e;
constexpr u32 kVarFailReason = 0x28;
constexpr u32 kVarTargetCount = 5;           // the targets' IDs follow from 6
constexpr u32 kVarFlag = 0x4a;

constexpr u32 kEventMovesWhileAsleep = 0x22;   // Snore, Sleep Talk
constexpr u32 kEventMoveFails = 0x23;          // Truant
constexpr u32 kEventNoThaw = 0xd4;             // Burn Up

constexpr u32 kConditionConfusion = 6;
constexpr u32 kConditionInLove = 7;
constexpr u32 kConditionTaunt = 0xb;
constexpr u32 kConditionDisable = 0xd;
constexpr u32 kConditionHealBlock = 0xf;
constexpr u32 kConditionEmbargo = 0x13;
constexpr u32 kConditionEncore = 0x17;
constexpr u32 kConditionThroatChop = 0x18;
constexpr u32 kConditionMoveLock = 0x1a;
constexpr u32 kConditionChoiceLock = 0x1c;

constexpr u32 kFlagFlinched = 4;
constexpr u32 kFlagHitBeforeMoving = 6;      // Focus Punch fails

constexpr u32 kMoveFlagSound = 8;
constexpr u32 kMoveFlagGravity = 9;          // fails under Gravity: Fly, Splash
constexpr u32 kMoveFlagThaws = 10;
constexpr u32 kMoveFlagHeals = 0xc;
constexpr u32 kMoveFlagRule = 0xe;

constexpr u32 kFieldGravity = 2;
constexpr u32 kFieldImprison = 3;
constexpr u32 kFieldMagicRoom = 7;

constexpr u32 kMoveStruggle = 0xa5;
constexpr u32 kMoveFocusPunch = 0x108;

constexpr u32 kDisobeyNaps = 2;
constexpr u32 kDisobeyHurtsItself = 3;
constexpr u32 kDisobeyLoafs = 4;

constexpr u8 kNoSlot = 4;
constexpr u32 kMessageEnd = 0xffff0000;

// What the clients are told (ServerCommands::Put)
constexpr u32 kCommandStatus = 0x12;
constexpr u32 kCommandCondition = 0x13;
constexpr u32 kCommandStatusIcon = 0x4b;
constexpr u32 kCommandReaction = 0x65;       // to the entry Unk1798::IndexOf gives
constexpr u32 kCommandMessage = 0x7b;        // PutMessage: a message without the Pokémon's name first
constexpr u32 kCommandPokemonMessage = 0x7c;

}  // namespace

// 0x71009ff990
u32 GetMoveFailReason(Flow* flow, BattlePokemon* pokemon, const MoveParam* move, bool noPpCheck, PokemonSet* targets,
                      const u8* flags) {
    u32 called = move->unk20;
    CountSleep(flow, pokemon);
    if (pokemon->GetStatus() == kStatusSleep) {
        u32 id = move->id;
        PushVars();
        SetConstVar(kVarPokemon, pokemon->GetId());
        SetConstVar(kVarFailReason, kMoveFailAsleep);
        SetConstVar(kVarMoveId, id);
        SetVarRewritableOnce(kVarFlag, 0);
        CallEventWithSkipChecks(flow, kEventMovesWhileAsleep);
        s32 movesAnyway = GetVar(kVarFlag);
        PopVars();
        if (!movesAnyway) return kMoveFailAsleep;
    }
    bool thawed = CheckThaw(flow, pokemon, move->id);
    u32 status = pokemon->GetStatus();
    if (!thawed && status == kStatusFreeze) return kMoveFailFrozen;
    if (flow->disobedience == kDisobeyLoafs) return kMoveFailLoafing;
    if (flow->disobedience == kDisobeyNaps) return kMoveFailNapping;
    if (!noPpCheck && !(*flags & 8)) {
        u8 slot = pokemon->FindMove(move->chosenId);
        if (slot != kNoSlot && pokemon->GetPp(slot) == 0) return kMoveFailNoPp;
    }
    if (flow->setup->params->unk4a1 && moves::HasFlag(move->id, kMoveFlagRule)) return kMoveFailRule;
    u32 reason = CallMoveFailEvent(flow, pokemon, move->id, kEventMoveFails, move, targets);
    if (reason) return reason;
    if (pokemon->HasFlag(kFlagHitBeforeMoving) && move->id == kMoveFocusPunch) return kMoveFailLostFocus;
    if (pokemon->HasFlag(kFlagFlinched)) return kMoveFailFlinched;
    if (!called) {
        if (pokemon->HasCondition(kConditionDisable) && move->id != kMoveStruggle) {
            u64 disable = pokemon->GetCondition(kConditionDisable);
            if (move->id == ConditionValue(&disable)) return kMoveFailDisabled;
        }
        if (pokemon->HasCondition(kConditionHealBlock) && moves::HasFlag(move->id, kMoveFlagHeals))
            return kMoveFailHealBlocked;
    }
    if (pokemon->HasCondition(kConditionThroatChop)) {
        bool sound = moves::HasFlag(move->id, kMoveFlagSound);
        if (!called && sound) return kMoveFailThroatChop;
    }
    if (flow->field->HasEffect(kFieldGravity)) {
        bool grounded = moves::HasFlag(move->id, kMoveFlagGravity);
        if (!called && grounded) return kMoveFailGravity;
    }
    if (move->id != kMoveStruggle) {
        if (!called && pokemon->HasCondition(kConditionEncore)) {
            u64 encore = pokemon->GetCondition(kConditionEncore);
            u16 encored = ConditionValue(&encore);
            if (encored != kMoveStruggle && move->id != encored) return kMoveFailLocked;
        }
        u32 locked = move->id;
        if (pokemon->HasCondition(kConditionMoveLock)) {
            locked = pokemon->GetLockedMove();
            if (locked != move->id) return kMoveFailLocked;
        }
        if (locked != kMoveStruggle && !flow->field->HasEffect(kFieldMagicRoom) &&
            !pokemon->HasCondition(kConditionEmbargo) && pokemon->HasCondition(kConditionChoiceLock) &&
            pokemon->GetChoiceMove() != locked)
            return kMoveFailLocked;
        bool taunted = pokemon->HasCondition(kConditionTaunt);
        if (!called && taunted && !moves::HasPower(move->id)) return kMoveFailTaunted;
        if (!called && flow->field->HasEffect(kFieldImprison) &&
            flow->field->IsImprisoned(flow->setup, flow->pokemon, pokemon, move->id))
            return kMoveFailImprisoned;
    }
    if (CheckConfusion(flow, pokemon)) return kMoveFailConfused;
    if (status == kStatusParalysis && RollPercent(25)) return kMoveFailParalyzed;
    if (pokemon->HasCondition(kConditionInLove)) {
        PutInLoveMessage(flow, pokemon);
        return RollPercent(50) ? kMoveFailInLove : kMoveFailNone;
    }
    return kMoveFailNone;
}

// 0x71009ffdf0
void CureStatus(Flow* flow, BattlePokemon* pokemon, u32 status, bool message) {
    u8 id = pokemon->GetId();
    pokemon->CureStatus();
    flow->commands->Put(kCommandStatus, id);
    flow->commands->Put(kCommandStatusIcon, id, 0);
    if (!message) return;
    u32 text = status == kStatusFreeze ? 0x188 : status == kStatusSleep ? 0x1a0 : 0x1c8;
    flow->commands->PutMessage(kCommandPokemonMessage, text, id, kMessageEnd);
}

// 0x71009ffeb0
void CountSleep(Flow* flow, BattlePokemon* pokemon) {
    if (pokemon->GetStatus() != kStatusSleep || !pokemon->CountSleepTurn()) return;
    u8 id = pokemon->GetId();
    pokemon->CureStatus();
    flow->commands->Put(kCommandStatus, id);
    flow->commands->Put(kCommandStatusIcon, id, 0);
    flow->commands->PutMessage(kCommandPokemonMessage, 0x1a0, id, kMessageEnd);
}

// 0x71009fff60
bool CheckThaw(Flow* flow, BattlePokemon* pokemon, u32 move) {
    if (pokemon->GetStatus() != kStatusFreeze) return false;
    PushVars();
    SetConstVar(kVarAttacker, pokemon->GetId());
    SetConstVar(kVarMoveId, move);
    SetVarRewritableOnce(kVarFlag, 0);
    CallEventWithSkipChecks(flow, kEventNoThaw);
    s32 noThaw = GetVar(kVarFlag);
    PopVars();
    if (!noThaw && moves::HasFlag(move, kMoveFlagThaws)) return true;
    if (RollPercent(20)) CureStatus(flow, pokemon, kStatusFreeze, true);
    return false;
}

// 0x7100a00030
u32 CallMoveFailEvent(Flow* flow, BattlePokemon* pokemon, u32 moveId, u32 event, const MoveParam* move,
                      PokemonSet* targets) {
    u32 unk18, called, category;
    if (move) {
        unk18 = move->unk18;
        called = move->unk20;
        category = move->category;
    } else {
        category = moves::GetCategory(moveId);
        called = 0;
        unk18 = 0;
    }
    u8 count = targets ? targets->Count() : 0;
    PushVars();
    SetConstVar(kVarMoveId, moveId);
    SetConstVar(kVarCalled, called);
    SetConstVar(kVarPokemon, pokemon->GetId());
    SetConstVar(kVarMoveUnk18, unk18);
    SetConstVar(kVarMoveCategory, category);
    SetVarRewritableOnce(kVarFailReason, 0);
    SetConstVar(kVarTargetCount, count);
    for (u8 i = 0; i < count; i++) SetConstVar(kVarTargetCount + 1 + i, targets->Get(i)->GetId());
    if (move) SetConstVar(kVarMoveType, move->type);
    CallEventWithSkipChecks(flow, event);
    u32 reason = GetVar(kVarFailReason);
    PopVars();
    return reason;
}

// 0x7100a00190
bool CheckConfusion(Flow* flow, BattlePokemon* pokemon) {
    if (flow->disobedience == kDisobeyHurtsItself) {
        flow->commands->PutMessage(kCommandMessage, 0xcf, pokemon->GetId(), kMessageEnd);
        return true;
    }
    if (!pokemon->HasCondition(kConditionConfusion)) return false;
    bool snapsOut = pokemon->CountConfusionTurn();
    u8 id = pokemon->GetId();
    if (snapsOut) {
        pokemon->CureCondition(kConditionConfusion);
        flow->commands->Put(kCommandCondition, id, kConditionConfusion);
        flow->commands->PutMessage(kCommandPokemonMessage, 0x1d4, pokemon->GetId(), kMessageEnd);
        return false;
    }
    u8 entry = flow->unk1798.IndexOf(id);
    if (entry != 4) flow->commands->Put(kCommandReaction, entry, 0x2f);
    flow->commands->PutMessage(kCommandPokemonMessage, 0x1d0, pokemon->GetId(), kMessageEnd);
    return RollPercent(33);
}

// 0x7100a00420
void PutInLoveMessage(Flow* flow, BattlePokemon* pokemon) {
    u16 loves = pokemon->GetConditionParam(kConditionInLove);
    u8 entry = flow->unk1798.IndexOf(pokemon->GetId());
    if (entry != 4) flow->commands->Put(kCommandReaction, entry, 0x30);
    flow->commands->PutMessage(kCommandPokemonMessage, 0x1bc, pokemon->GetId(), loves, kMessageEnd);
}

}  // namespace battle
