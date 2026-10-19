// The turn (docs/PLAN.md, roadmap phase 5, step 3): getting it ready, its loop (the actions in their order, the
// end of the turn) and what the battle does next.
#include "battle/event.h"
#include "battle/order.h"

// Not known yet: whether the action just run makes the battle go to state 3 (0x71009f28c0), two checks of
// something at flow+0x1354 (the first with 0), the list of the clients with an empty position (a position
// whose Pokémon is 0x1f), the party of a client (as PokemonList::GetParty) and what the setup does for a
// client's party then (it looks for Illusion, ability 149).
extern "C" bool FUN_71009f28c0(battle::Flow* flow);
extern "C" u8 FUN_7100a65f50(u8* unk, u32 index);
extern "C" bool FUN_7100a66080(u8* unk, battle::Unk1798* positions);
extern "C" bool FUN_71009f2c00(battle::Flow* flow, u8* list);
extern "C" battle::BattleParty* FUN_71009e0d60(battle::PokemonList* list, u32 client);
extern "C" void FUN_71009dab50(battle::Setup* setup, battle::BattleParty* party, u8 client);
// Not known yet either: what gets a turn ready (the first five), what comes after making the turn's actions
// (0x71009f1490), what ends a turn, the setup's turn limit and making the actions themselves (0x71009f1c30).
extern "C" void FUN_7100a510f0(void* unk);
extern "C" void FUN_7100a65970(u8* unk);
extern "C" void FUN_71009eeb60(void* unk);
extern "C" void FUN_71009f11d0(battle::Flow* flow);
extern "C" void FUN_71009f12b0(battle::Flow* flow);
extern "C" void FUN_71009f1490(battle::Flow* flow);
extern "C" void FUN_71009f1760(battle::Flow* flow);
extern "C" u8 FUN_71009d8d40(battle::Setup* setup);
extern "C" u8 FUN_71009f1c30(battle::Flow* flow, void* choices, battle::Action* actions);

namespace battle {

u32 RunTurn(Flow* flow, void* choices) {
    flow->state = 0;
    flow->commands->unk0 = 0;
    if (!flow->turnStarted) {
        FUN_7100a510f0(flow->unk14d0);
        FUN_7100a65970(flow->unk1354);
        for (u32 i = 0; i < 4; i++) flow->unk2ad5[i] = 0;
        WakeAllHandlerSets();
        FUN_71009eeb60(flow->unk58);
        ResetVars();
        FUN_71009f11d0(flow);
        FUN_71009f12b0(flow);
        MakeActions(flow, choices, flow->actions, &flow->actionCount, &flow->reorder, &flow->reorderFrom);
        FUN_71009f1490(flow);
        flow->turnStarted = 1;
    }
    flow->nextAction = RunActions(flow, flow->nextAction);
    u32 state = flow->state;
    if (state == 0 || state == 4 || state == 5 || state == 6) {
        FUN_71009f1760(flow);
        if (flow->state == 0) {
            u8 limit = FUN_71009d8d40(flow->setup);
            if (limit != 0 && flow->unk80 >= limit) flow->state = 7;
        }
    }
    return flow->state;
}

void MakeActions(Flow* flow, void* choices, Action* actions, u8* count, bool* reorder, u8* reorderFrom) {
    u8 made = FUN_71009f1c30(flow, choices, actions);
    *count = made;
    if (made != 0) {
        OrderActions(flow, actions, 0, made - 1, false);
        u8 total = *count;
        for (u64 i = 0; i < total; i++) {
            if ((actions[i].info & 0xf) != 6) continue;
            u8 id = actions[i].pokemon->GetId();
            for (u64 j = 0; j < total; j++) {
                if (actions[j].pokemon->GetId() == id && (actions[j].info & 0xf) == kActionMove)
                    actions[j].recalculate = 1;
            }
        }
        for (u64 i = 0; i < *count; i++) {
            if (((actions[i].info | 4) & 0xf) == 5) {
                *reorderFrom = i;
                *reorder = true;
                return;
            }
        }
    }
    *reorder = false;
}

u32 RunActions(Flow* flow, u32 from) {
    for (; from < flow->actionCount; from++) {
        if (flow->reorder && from == flow->reorderFrom)
            OrderActions(flow, flow->actions, flow->reorderFrom, flow->actionCount - 1, true);
        RunAction(flow, &flow->actions[from]);
        bool stop = FUN_71009f28c0(flow);
        if (IsSideOut(flow) && EndsWhenSideOut(flow->setup->GetUnk168())) {
            flow->state = 4;
            return from + 1;
        }
        if (flow->state == 1 || flow->state == 6) return from + 1;
        if (stop) {
            flow->state = 3;
            return from + 1;
        }
    }
    if (flow->state != 0) return flow->actionCount;

    u32 result = EndOfTurn(flow);
    if (IsSideOut(flow)) {
        flow->state = 4;
    } else if (result == 3) {
        flow->state = 5;
    } else if (result == 2) {
        flow->state = 1;
    } else if (result == 1) {
        flow->state = 3;
    } else {
        u8 first = FUN_7100a65f50(flow->unk1354, 0);
        bool second = FUN_7100a66080(flow->unk1354, &flow->unk1798);
        if (first == 0 && !second) {
            flow->state = 0;
            return flow->actionCount;
        }
        FUN_71009f2c00(flow, flow->emptyClients);
        for (u32 i = 0; i < flow->emptyClients[0]; i++) {
            BattleParty* party = FUN_71009e0d60(flow->pokemon, flow->emptyClients[1 + i]);
            flow->commands->Put(0x31, flow->emptyClients[1 + i]);
            FUN_71009dab50(flow->setup, party, flow->emptyClients[1 + i]);
        }
        flow->state = 2;
    }
    return flow->actionCount;
}

}  // namespace battle
