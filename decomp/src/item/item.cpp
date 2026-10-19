// Items (item/item.h): an item's data, the per-item flags, and the item lists checked by ID.
#include "item/item.h"

#include <string.h>

#include "gfl/fs.h"
#include "pokemon/system.h"

namespace item {

// ---- an item's data -------------------------------------------------------------------------------

// 0x710071c420
Item::Item(bool clear) : id_(0) {
    if (clear) {
        data_ = ItemData();
    }
}

// 0x710071c4d0
Item::Item(u32 id) {
    Read(id);
}

// 0x710071c570
void Item::Read(u32 id) {
    gfl::fs::Path path = pokemon::GetItemDataPath(id);
    gfl::fs::ReadRequest request{};
    gfl::fs::ReadFile(&request, path, 0, pokemon::GetSystem()->fileDevice, 0);
    if (request.file_->size_ != 0) {
        memcpy(&data_, request.file_->data_, sizeof(ItemData));
    }
    id_ = id;
}

// 0x710071c720, deleting: 0x710071c740
Item::~Item() {}

// 0x710071c760
void Item::Load(u32 id) {
    gfl::fs::Path path = pokemon::GetItemDataPath(id);
    gfl::fs::ReadRequest request{};
    gfl::fs::ReadFile(&request, path, 0, pokemon::GetSystem()->fileDevice, 0);
    if (request.file_->size_ != 0) {
        memcpy(&data_, request.file_->data_, sizeof(ItemData));
    }
    id_ = id;
}

// 0x710071c910
void Item::CopyFrom(const Item& other) {
    id_ = other.id_;
    memcpy(&data_, &other.data_, sizeof(ItemData));
}

static u32 Bits(u32 value, u32 shift, u32 width) {
    return value >> shift & ((1u << width) - 1);
}

// 0x710071c930
s32 Item::GetParam(u32 param) const {
    switch (param) {
    case kParamId: return id_;
    case kParamPrice: return data_.price * 10;
    case kParamHeldEffect: return data_.heldEffect;
    case kParamHeldArgument: return data_.heldArgument;
    case kParamNaturalGiftEffect: return data_.naturalGiftEffect;
    case kParamFlingEffect: return data_.flingEffect;
    case kParamFlingPower: return data_.flingPower;
    case kParamNaturalGiftPower: return data_.naturalGiftPower;
    case kParamNaturalGiftType: return Bits(data_.packed, 0, 5);
    case kParamUnk09: return Bits(data_.packed, 5, 1);
    case kParamUnk0A: return Bits(data_.packed, 6, 1);
    case kParamPocket: return Bits(data_.packed, 7, 4);
    case kParamUnk0C: return data_.packed >> 11;
    case kParamFieldEffect: return data_.fieldEffect;
    case kParamBattleEffect: return data_.battleEffect;
    case kParamUseKind: return data_.useKind;
    case kParamItemClass: return data_.itemClass;
    case kParamUnk11: return data_.unk0E & 0xF;
    case kParamUnk12: return data_.unk0E >> 4;
    case kParamSortIndex: return data_.sortIndex;
    }
    if (data_.useKind != 1) {
        return data_.useKind == 0 || data_.useKind == 2 ? data_.use[0] : 0;
    }
    u32 flags;
    memcpy(&flags, &data_.use[0], sizeof(flags));
    u16 flags2;
    memcpy(&flags2, &data_.use[4], sizeof(flags2));
    u8 flags3 = data_.use[6];
    const s8* changes = reinterpret_cast<const s8*>(&data_.use[7]);
    if (param >= kParamCureSleep && param < kParamBoostAttack) {
        return Bits(flags, param - kParamCureSleep, 1);
    }
    if (param >= kParamBoostAttack && param <= kParamBoostSpeed) {
        return Bits(flags, 12 + (param - kParamBoostAttack) * 4, 4);
    }
    switch (param) {
    case kParamBoostAccuracy: return Bits(flags2, 0, 4);
    case kParamBoostCritical: return Bits(flags2, 4, 2);
    }
    if (param >= kParamPpUp && param <= kParamChangeEvSpAttack) {
        return Bits(flags2, 6 + (param - kParamPpUp), 1);
    }
    if (param >= kParamChangeEvSpDefense && param <= kParamFriendship3) {
        return Bits(flags3, param - kParamChangeEvSpDefense, 1);
    }
    if (param >= kParamEvHp && param <= kParamEvSpDefense) {
        return changes[param - kParamEvHp];
    }
    switch (param) {
    case kParamHpAmount: return data_.use[0xD];
    case kParamPpAmount: return data_.use[0xE];
    case kParamFriendshipAmount1:
    case kParamFriendshipAmount2:
    case kParamFriendshipAmount3:
        return static_cast<s8>(data_.use[0xF + (param - kParamFriendshipAmount1)]);
    }
    return 0;
}

// 0x710071cc60. The checks go in this order: an item that cures sleep and restores HP counts as
// curing sleep.
Effect Item::GetEffect() const {
    if (data_.useKind != 1) {
        return kEffectNone;
    }
    u32 flags;
    memcpy(&flags, &data_.use[0], sizeof(flags));
    u16 flags2;
    memcpy(&flags2, &data_.use[4], sizeof(flags2));
    const s8* evs = reinterpret_cast<const s8*>(&data_.use[7]);
    if (flags & 1u << 9) {
        return kEffectReviveAll;
    }
    if (flags & 1u << 10) {
        return kEffectLevelUp;
    }
    switch (flags & 0x3F) {
    case 0x01: return kEffectCureSleep;
    case 0x02: return kEffectCurePoison;
    case 0x04: return kEffectCureBurn;
    case 0x08: return kEffectCureFreeze;
    case 0x10: return kEffectCureParalysis;
    case 0x20: return kEffectCureConfusion;
    case 0x3F: return flags2 & 1u << 10 ? kEffectRestoreHp : kEffectCureAll;
    }
    if (flags & 1u << 6) {
        return kEffectCureInfatuation;
    }
    if (flags2 & 1u << 10) {
        return kEffectRestoreHp;
    }
    if (flags & 1u << 8) {
        return kEffectRevive;
    }
    // Checked HP, Atk, Def, SpA, Spe, SpD; stored HP, Atk, Def, Spe, SpA, SpD; the effects go
    // HP, Atk, Def, SpA, SpD, Spe
    static constexpr struct {
        u8 stored;
        u8 effect;
    } kEvChecks[6] = {{0, 0}, {1, 1}, {2, 2}, {4, 3}, {3, 5}, {5, 4}};
    for (const auto& check : kEvChecks) {
        s8 change = evs[check.stored];
        if (change > 0) {
            return static_cast<Effect>(kEffectRaiseEv + check.effect);
        }
        if (change < 0) {
            return static_cast<Effect>(kEffectLowerEv + check.effect);
        }
    }
    if (flags & 1u << 11) {
        return kEffectEvolve;
    }
    if (flags2 & 1u << 6) {
        return kEffectPpUp;
    }
    if (flags2 & 1u << 7) {
        return kEffectPpMax;
    }
    return flags2 & 3u << 8 ? kEffectRestorePp : kEffectNone;
}

// 0x710071ce30
bool Item::IsUsedOnMove() const {
    Effect effect = GetEffect();
    if (effect == kEffectPpUp || effect == kEffectPpMax) {
        return true;
    }
    return effect == kEffectRestorePp && GetParam(kParamRestorePp) != 0;
}

// ---- the per-item flags ---------------------------------------------------------------------------

// 0x710071cec0: the file's size rounded up to 4 bytes
void LoadFlags(gfl::mem::MemoryResource* resource) {
    gfl::fs::Path path = pokemon::GetItemFlagsPath();
    gfl::fs::ReadRequest request{};
    gfl::fs::ReadFile(&request, path, 0, pokemon::GetSystem()->fileDevice, 0);
    size_t size = request.file_->size_;
    if (size != 0) {
        size_t padded = size % 4 != 0 ? size + 4 - size % 4 : size;
        g_flags = gfl::mem::MakeUniqueArray<u8>(resource, padded);
        memcpy(g_flags.Get(), request.file_->data_, size);
    }
}

// 0x710071d180
void UnloadFlags() {
    g_flags.Reset();
}

// 0x710071d1b0
u8 GetFlagsLow(u32 item, u32 kind) {
    if (item < kFlagCount && kind == 11) {
        return g_flags.Get()[item] & 7;
    }
    return 0;
}

// 0x710071d1e0
bool GetFlag6(u32 item) {
    if (item >= kFlagCount) {
        return false;
    }
    return g_flags.Get()[item] >> 6 & 1;
}

// 0x710071d210
bool GetFlag7(u32 item) {
    if (item >= kFlagCount) {
        return false;
    }
    return static_cast<s8>(g_flags.Get()[item]) < 0;
}

// ---- items by ID ----------------------------------------------------------------------------------

// 0x710071d240
u8 GetBallId(u16 item) {
    if (item >= 1 && item <= 16) {
        return item;
    }
    if (item >= 492 && item <= 499) {
        return item - 492 + 17;
    }
    if (item == 576) {
        return 25;
    }
    return item == 851 ? 26 : 0;
}

// 0x710071d360
u16 GetBallItem(s32 ball) {
    if (ball == 0 || ball > 26) {
        return 0;
    }
    return kBallItems[ball - 1];
}

// The HMs' items, after the 100 TMs
static s32 GetHmIndex(u16 item) {
    if (item >= 420 && item <= 425) {
        return item - 420;
    }
    return item == 737 ? 6 : -1;
}

// 0x710071d390
bool IsMachine(u16 item) {
    for (u32 i = 0; i < 100; i++) {
        if (kMachineItems[i] == item) {
            return true;
        }
    }
    return GetHmIndex(item) >= 0;
}

// 0x710071d3f0
s32 GetMachineIndex(u16 item) {
    for (u32 i = 0; i < 100; i++) {
        if (kMachineItems[i] == item) {
            return i;
        }
    }
    s32 hm = GetHmIndex(item);
    return hm >= 0 ? 100 + hm : -1;
}

// 0x710071d4a0
u16 GetMachineMove(u16 item) {
    for (u32 i = 0; i < 100; i++) {
        if (kMachineItems[i] == item) {
            return kMachineMoves[i];
        }
    }
    s32 hm = GetHmIndex(item);
    return hm >= 0 ? kHmMoves[hm] : 0;
}

// 0x710071d560: HMs from index 100, unchecked past them
u16 GetMachineMoveByIndex(u32 index) {
    return index < 100 ? kMachineMoves[index] : kHmMoves[index - 100];
}

static u8 FindBerry(u16 item) {
    for (u32 i = 0; i < 73; i++) {
        if (kBerries[i] == item) {
            return i;
        }
    }
    return 0xFF;
}

// 0x710071d5a0
bool IsBerry(u16 item) {
    return FindBerry(item) != 0xFF;
}

// 0x710071d610
u16 GetBerry(u8 index) {
    return index < 73 ? kBerries[index] : 0;
}

// 0x710071d640: 656-685 and the ORAS ones (752-770, but not 765 or 766)
bool IsMegaStone(u16 item) {
    if (item >= 656 && item < 686) {
        return true;
    }
    u32 offset = item - 752;
    return offset <= 18 && (1u << offset & 0x79FFF) != 0;
}

// 0x710071d690: 548-564 and the Fairy Gem
bool IsGem(u16 item) {
    return (item >= 548 && item < 565) || item == 715;
}

// 0x710071d6b0: 776-794, 798-806, 836, 921-926
bool IsZCrystal(u16 item) {
    return (item >= 776 && item <= 794) || (item >= 798 && item <= 806) || item == 836 ||
           (item >= 921 && item <= 926);
}

// 0x710071d6f0: 807-835, 927-932
bool IsHeldZCrystal(u16 item) {
    return (item >= 807 && item < 836) || (item >= 927 && item <= 932);
}

// 0x710071d720
bool IsCandy(u16 item) {
    for (u32 i = 0; i < 98; i++) {
        if (kCandies[i] == item) {
            return true;
        }
    }
    return false;
}

// 0x710071d770
bool IsUnk121(u16 item) {
    u16 offset = item - 121;
    if (offset > 7) {
        return false;
    }
    return 0xDFu >> static_cast<u8>(offset) & 1;
}

}  // namespace item

// ---- the file's global objects: set up at startup, destroyed at exit (what the compiler writes) -----

namespace item {

// 0x710071c3f0
void DestroyFlags(gfl::mem::UniquePtr<u8[]>* flags) {
    flags->Reset();
}

}  // namespace item

extern "C" void FUN_710071c3f0(void*);    // item::DestroyFlags, as the game registers it

namespace item {

// 0x710071d7a0
void InitializeGlobals() {
    new (&g_flags) gfl::mem::UniquePtr<u8[]>();
    __cxa_atexit(FUN_710071c3f0, &g_flags, &__dso_handle);
}

}  // namespace item
