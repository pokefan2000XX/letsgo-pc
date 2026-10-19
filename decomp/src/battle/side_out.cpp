// Whether a side has no Pokémon left that can fight (battle/battle.h), and what it asks: the setup's clients and
// sides (two functions of the setup's file and two of the rules' file), and a party's count of Pokémon that can
// fight (its party's file). The check is in the battle's main file; EndsWhenSideOut is in the rules' file.
#include "battle/battle.h"

namespace battle {

namespace {

constexpr u8 kClients = 4;
constexpr u8 kSides = 4;

}  // namespace

// 0x71009e1fa0
u8 SideOfClientIn(u32 /* kind */, s32 client) {
    if (client > 3) return 0;
    return client & 1;
}

// 0x71009e1ef0
bool HasSideIn(u32 /* kind */, u8 side) {
    if (side > 3) return false;
    return side < 2;
}

// 0x71009d7120
u8 Setup::SideOfClient(u8 client) {
    return SideOfClientIn(kind, client);
}

// 0x71009d9b80
bool Setup::HasSide(u8 side) {
    return HasSideIn(kind, side);
}

// 0x71009df260
u32 BattleParty::CountFighters() {
    u32 fighters = 0;
    for (u64 i = 0; i < count; i++) fighters += members[i]->CanFight();
    return fighters;
}

// 0x71009f2d80
bool IsSideOut(Flow* flow) {
    bool fighting[kSides] = {};
    for (u8 client = 0; client < kClients; client++) {
        if (!flow->setup->HasClient(client)) continue;
        BattleParty* party = flow->pokemon->GetParty(client);
        u8 side = flow->setup->SideOfClient(client);
        if (static_cast<u8>(party->CountFighters()) != 0) fighting[side] = true;
    }
    for (u8 side = 0; side < kSides; side++)
        if (flow->setup->HasSide(side) && !fighting[side]) return true;
    return false;
}

// 0x7100a54dd0
bool EndsWhenSideOut(u32 /* kind */) {
    return true;
}

}  // namespace battle
