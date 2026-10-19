// A turn order key (battle/order.h).
#include "battle/order.h"

namespace battle {

// 0x7100a50e80
u32 MakeOrderKey(u32 mode, BattlePokemon*, const u32* info, u8 unk, u32 speed, u32, u32 priority) {
    u32 kind;
    switch (*info & 0xf) {
    case 0:
        return 0x1402000;
    case kActionMove:
        kind = 0;
        break;
    case 2:
        kind = 0xc00000;
        priority = 0;
        break;
    case 3:
        kind = 0x1000000;
        priority = 0;
        break;
    case 4:
        kind = unk == 1 && mode == 0 ? 0 : 0x1400000;
        priority = 0;
        break;
    case 5:
        kind = 0;
        priority = 7;
        break;
    case 6:
        kind = 0x800000;
        priority = 0;
        break;
    case 7:
        kind = 0x1400000;
        priority = 0;
        break;
    case 8:
        kind = 0x400000;
        priority = 0;
        break;
    case 9:
        return 0;
    default:
        kind = 0;
        priority = 0;
        break;
    }
    return kind | (speed & 0x1fff) | (priority & 0x3f) << 16 | 0x2000;
}

// 0x7100a50f30
u32 SetOrderBracket(u32 key, u32 bracket) {
    return (key & ~0xe000u) | (bracket & 7) << 13;
}

// 0x7100a50f40
u32 OrderKeyWithoutKind(u32 key) {
    return key & 0x3fffff;
}

// 0x7100a50f50
int OrderKeyPriority(u32 key) {
    return (int)(key >> 16 & 0xff) - 7;
}

}  // namespace battle
