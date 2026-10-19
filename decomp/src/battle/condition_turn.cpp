// A condition's turn at the end of a turn (battle/status_damage.h): the part in the conditions' own file.
#include "battle/status_damage.h"

#include "battle/calc.h"
#include "battle/event.h"

namespace battle {

namespace {

constexpr u32 kConditionBind = 8;
constexpr u32 kConditionLeechSeed = 0x12;
constexpr u32 kConditionPerishSong = 0x14;
constexpr u32 kConditionIngrain = 0x15;
constexpr u32 kConditionEncore = 0x17;
constexpr u32 kConditionAquaRing = 0x24;

constexpr u32 kRecordMessage = 3;
constexpr u32 kRecordHeal = 5;
constexpr u32 kRecordDamage = 6;
constexpr u32 kRecordConditionEnds = 10;
constexpr u32 kRecordPokemon = 0x14;

constexpr u32 kCauseLeechSeed = 0x24;         // DamageRecord::cause
constexpr u32 kCauseBind = 0x25;
constexpr u8 kEffectPlays = 2;                // DamageRecord::flags
constexpr u16 kEffectLeechSeed = 0x45;
constexpr s32 kNoEffect = -1;
constexpr u8 kNoPosition = 4;
constexpr u32 kHealDrained = 0x1000000;       // HealRecord::header

constexpr u16 kKindPokemon = 2;               // BattleMessage: a message starting with a Pokémon's name
constexpr u16 kMessagePerishCount = 0x484;     // the message numbers are btl_set's lines
constexpr u16 kMessageLeechSeed = 0x32e;
constexpr u16 kMessageIngrain = 0x3dc;
constexpr u16 kMessageAquaRing = 0x326;
constexpr u16 kMessageBind = 0x1f0;

constexpr u32 kMoveBind = 20;
constexpr u32 kMoveWrap = 35;
constexpr u32 kMoveFireSpin = 83;
constexpr u32 kMoveClamp = 128;
constexpr u32 kMoveWhirlpool = 250;
constexpr u32 kMoveSandTomb = 328;
constexpr u32 kMoveMagmaStorm = 463;
constexpr u32 kMoveInfestation = 611;

// The effect a binding move's damage plays.
s32 BindEffect(u32 move) {
    switch (move) {
    case kMoveBind: return 0x46;
    case kMoveWrap: return 0x47;
    case kMoveFireSpin: return 0x48;
    case kMoveMagmaStorm: return 0x49;
    case kMoveClamp: return 0x4a;
    case kMoveWhirlpool: return 0x4b;
    case kMoveSandTomb: return 0x4c;
    case kMoveInfestation: return 0x4e;
    default: return kNoEffect;
    }
}

// 1/16 of the maximum HP back (Ingrain, Aqua Ring).
void HealSixteenth(Flow* flow, BattlePokemon* pokemon, u16 message) {
    u8 id = pokemon->GetId();
    auto* record = reinterpret_cast<HealRecord*>(flow->NewRecord(kRecordHeal, id));
    record->pokemon = id;
    record->from = kNoPokemon;
    record->amount = static_cast<u16>(MaxHpFraction(pokemon, 16));
    record->message.Setup(kKindPokemon, message);
    record->message.AddArg(id);
    flow->PushRecord(reinterpret_cast<BattleRecord*>(record));
}

}  // namespace

// 0x7100a14140
void PerishSongTurn(Flow* flow, BattlePokemon* pokemon) {
    if (pokemon->IsFainted()) return;
    u64 entry = pokemon->GetCondition(kConditionPerishSong);
    u8 limit = ConditionTurnLimit(&entry);
    s32 left = limit - pokemon->GetConditionTurns(kConditionPerishSong);
    if (left < 1) return;
    u8 id = pokemon->GetId();
    auto* record = reinterpret_cast<MessageRecord*>(flow->NewRecord(kRecordMessage, id));
    record->message.Setup(kKindPokemon, kMessagePerishCount);
    record->message.AddArg(id);
    record->message.AddArg(static_cast<u8>(left));
    flow->PushRecord(reinterpret_cast<BattleRecord*>(record));
}

// 0x7100a14210
void LeechSeedTurn(Flow* flow, BattlePokemon* pokemon) {
    u64 entry = pokemon->GetCondition(kConditionLeechSeed);
    u16 position = ConditionValue(&entry);
    u8 seeder = flow->PokemonAtPosition(position);
    if (seeder == kNoPokemon) return;
    u16 drained = static_cast<u16>(MaxHpFraction(pokemon, 8));
    u16 hp = static_cast<u16>(pokemon->GetValue(kValueHp));
    u16 amount = drained > hp ? hp : drained;
    u8 id = pokemon->GetId();
    u8 cause = static_cast<u8>(ConditionCause(&entry));
    auto* damage = reinterpret_cast<DamageRecord*>(flow->NewRecord(kRecordDamage, cause));
    damage->pokemon = id;
    damage->amount = amount;
    damage->cause = kCauseLeechSeed;
    damage->causedBy = cause;
    damage->message.Setup(kKindPokemon, kMessageLeechSeed);
    damage->message.AddArg(id);
    damage->flags |= kEffectPlays;
    damage->effectFrom = static_cast<u8>(position);
    damage->effect = kEffectLeechSeed;
    damage->effectTo = flow->PositionOf(id);
    flow->PushRecord(reinterpret_cast<BattleRecord*>(damage));

    auto* heal = reinterpret_cast<HealRecord*>(flow->NewRecord(kRecordHeal, id));
    heal->pokemon = seeder;
    heal->from = id;
    heal->amount = amount;
    heal->header |= kHealDrained;
    flow->PushRecord(reinterpret_cast<BattleRecord*>(heal));

    auto* after = reinterpret_cast<PokemonRecord*>(flow->NewRecord(kRecordPokemon, id));
    after->pokemon = seeder;
    flow->PushRecord(reinterpret_cast<BattleRecord*>(after));
}

// 0x7100a143a0
void IngrainTurn(Flow* flow, BattlePokemon* pokemon) {
    if (pokemon->IsAtFullHp()) return;
    HealSixteenth(flow, pokemon, kMessageIngrain);
}

// 0x7100a14450
void BindTurn(Flow* flow, BattlePokemon* pokemon) {
    if (pokemon->IsFainted()) return;
    u8 id = pokemon->GetId();
    u64 entry = pokemon->GetCondition(kConditionBind);
    u16 move = ConditionValue(&entry);
    s32 effect = BindEffect(move);
    u8 cause = static_cast<u8>(ConditionCause(&entry));
    auto* record = reinterpret_cast<DamageRecord*>(flow->NewRecord(kRecordDamage, cause));
    record->pokemon = id;
    record->cause = kCauseBind;
    record->causedBy = cause;
    record->amount = static_cast<u16>(MaxHpFraction(pokemon, ConditionFlag(&entry) ? 6 : 8));
    if (effect != kNoEffect) {
        record->effect = static_cast<u16>(effect);
        record->flags |= kEffectPlays;
        record->effectFrom = flow->PositionOf(id);
        record->effectTo = kNoPosition;
    }
    record->message.Setup(kKindPokemon, kMessageBind);
    record->message.AddArg(id);
    record->message.AddArg(move);
    flow->PushRecord(reinterpret_cast<BattleRecord*>(record));
}

// 0x7100a14620
void AquaRingTurn(Flow* flow, BattlePokemon* pokemon) {
    if (pokemon->IsAtFullHp() || pokemon->IsFainted()) return;
    HealSixteenth(flow, pokemon, kMessageAquaRing);
}

// 0x7100a147b0
void EndOfTurnCondition(Flow* flow, BattlePokemon* pokemon, u32 condition) {
    if (pokemon->IsFainted()) return;
    u32 damage = pokemon->GetConditionDamage(condition);
    if (damage != 0) StatusDamage(flow, pokemon, condition, damage);
    switch (condition) {
    case kConditionBind:
        BindTurn(flow, pokemon);
        return;
    case kConditionLeechSeed:
        LeechSeedTurn(flow, pokemon);
        return;
    case kConditionPerishSong:
        PerishSongTurn(flow, pokemon);
        return;
    case kConditionIngrain:
        IngrainTurn(flow, pokemon);
        return;
    case kConditionEncore: {
        u64 entry = pokemon->GetCondition(kConditionEncore);
        if (pokemon->GetPpOfMove(ConditionValue(&entry)) != 0) return;
        u8 id = pokemon->GetId();
        BattleRecord* record = flow->NewRecord(kRecordConditionEnds, id);
        record->condition = kConditionEncore;
        record->pokemon = id;
        record->unk14 = 1;
        flow->PushRecord(record);
        return;
    }
    case kConditionAquaRing:
        AquaRingTurn(flow, pokemon);
        return;
    }
}

}  // namespace battle
