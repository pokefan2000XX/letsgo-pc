// The turn order (battle/order.h).
#include "battle/order.h"

#include "battle/calc.h"
#include "battle/event.h"
#include "moves/data.h"

namespace battle {

namespace {

constexpr u32 kVarBracket = 0x11;
constexpr u32 kVarMoveId = 0x12;
constexpr u32 kVarMoveType = 0x19;
constexpr u32 kVarPriority = 0x1c;           // the move's priority plus 7
constexpr u32 kVarSpeed = 0x35;
constexpr u32 kVarReversed = 0x53;           // Trick Room
constexpr u32 kVarParalysisSlows = 0x5b;

constexpr u32 kEventBracket = 0xf;
constexpr u32 kEventPriority = 0x11;
constexpr u32 kEventSpeed = 0x13;

constexpr u32 kCommandBracket = 0x45;
constexpr u32 kMaxSpeed = 10000;

// Highest key first; when two keys are equal, a coin decides.
void SortActions(Action* actions, u8 first, u8 last) {
    for (u32 i = first; i < last; i++) {
        for (u32 j = i + 1; j <= last; j++) {
            if (actions[i].key <= actions[j].key && (actions[i].key != actions[j].key || Random(2) != 0)) {
                Action swap = actions[i];
                actions[i] = actions[j];
                actions[j] = swap;
            }
        }
    }
}

}  // namespace

// 0x71009f3080
void OrderActions(Flow* flow, Action* actions, u8 first, u8 last, bool onlyMarked) {
    for (u32 i = first; i <= last; i++)
        if (!onlyMarked || actions[i].recalculate) actions[i].key = CalcOrderKey(flow, &actions[i]);
    SortActions(actions, first, last);
    for (u32 i = first; i <= last; i++) {
        Action& action = actions[i];
        if ((onlyMarked && !action.recalculate) || (action.info & 0xf) != kActionMove) continue;
        u32 saved = flow->unk2808.Save();
        BattlePokemon* pokemon = action.pokemon;
        u8 bracket = pokemon->GetOrderBracket();
        PushVars();
        SetConstVar(kVarPokemon, pokemon->GetId());
        SetVarRewritableOnce(kVarBracket, bracket);
        CallEventWithSkipChecks(flow, kEventBracket);
        s32 newBracket = GetVar(kVarBracket);
        PopVars();
        action.key = SetOrderBracket(action.key, newBracket);
        pokemon = action.pokemon;
        pokemon->SetOrderBracket(newBracket);
        flow->commands->Put(kCommandBracket, pokemon->GetId(), 1);
        flow->unk2808.Restore(saved);
    }
    SortActions(actions, first, last);
}

// 0x71009f4030
u32 CalcOrderKey(Flow* flow, Action* action) {
    u32 mode = flow->setup->GetUnk80Value();
    u32 move = action->GetMove();
    u32 saved = flow->unk2808.Save();
    u32 speed = CalcSpeed(flow, action->pokemon, true);
    flow->unk2808.Restore(saved);
    u32 priority = 0;
    if ((action->info & 0xf) == kActionMove) {
        u32 saved2 = flow->unk2808.Save();
        BattlePokemon* pokemon = action->pokemon;
        int movePriority = moves::GetPriority(move) + 7;
        u8 type = moves::GetType(move);
        PushVars();
        SetConstVar(kVarAttacker, pokemon->GetId());
        SetConstVar(kVarMoveId, move);
        SetConstVar(kVarMoveType, type);
        SetVar(kVarPriority, movePriority);
        CallEventWithSkipChecks(flow, kEventPriority);
        priority = GetVar(kVarPriority);
        PopVars();
        flow->unk2808.Restore(saved2);
    }
    return MakeOrderKey(mode, action->pokemon, &action->info, action->unk14, speed, move, priority);
}

// 0x71009f4190
u32 CalcSpeed(Flow* flow, BattlePokemon* pokemon, bool trickRoom) {
    u32 stat = pokemon->GetValue(kValueSpeed);
    PushVars();
    SetConstVar(kVarPokemon, pokemon->GetId());
    SetConstVar(kVarSpeed, stat);
    SetVar(kVarParalysisSlows, 1);
    SetVar(kVarReversed, 0);
    SetRatioVar(kVarRatio, 0x1000, 0x19a, 0x20000);
    CallEventWithSkipChecks(flow, kEventSpeed);
    s32 value = GetVar(kVarSpeed);
    u32 speed = MulRatio(value, GetVar(kVarRatio));
    if (pokemon->GetStatus() == kStatusParalysis && GetVar(kVarParalysisSlows)) speed = speed * 50 / 100;
    if (speed >> 4 >= 0x271) speed = kMaxSpeed;
    if (trickRoom && GetVar(kVarReversed)) speed = kMaxSpeed - speed;
    PopVars();
    return speed;
}

}  // namespace battle
