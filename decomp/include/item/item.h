// Items: each item's data (a 0x24-byte record in its own file, item%03d.dat: price, held and Fling
// effects, pocket, and what using it does), a table of per-item flags, and the lists of items the
// game checks by ID (Poké Balls, TMs and HMs, Berries, Mega Stones, Z-Crystals, Candies). The
// record's layout is the Gen 6 one pk3DS documents, which the game's reads agree with. IDs are the
// main series' (1 Master Ball ... 16 Cherish Ball, 328-419 TM01-TM92, 420-425 HM01-HM06).
#pragma once
#include "decomp.h"
#include "gfl/mem.h"
#include "gfl/object.h"

namespace item {

constexpr u32 kFlagCount = 0x422;         // items in the flag table

// An item's record
struct ItemData {
    u16 price;                            // in tens
    u8 heldEffect;
    u8 heldArgument;
    u8 naturalGiftEffect;
    u8 flingEffect;
    u8 flingPower;
    u8 naturalGiftPower;
    u16 packed;                           // Natural Gift's type (5 bits), two flags, the pocket (4 bits), 5 bits
    u8 fieldEffect;
    u8 battleEffect;
    u8 useKind;                           // 1: `use` says what using it does; 0 and 2: its first byte is the value
    u8 itemClass;
    u8 unk0E;                             // two 4-bit values
    u8 sortIndex;
    // What using it does: bit flags (u32 at 0: statuses cured, revive, level up, evolve, stat
    // boosts; u16 at 4 and u8 at 6: PP, HP, EVs, friendship), then EV changes in the stored order
    // (HP, Atk, Def, Spe, SpA, SpD), the HP and PP restored and three friendship changes
    u8 use[0x12];
    u8 unk22[2];
};
DECOMP_SIZE(ItemData, 0x24);

// What Item::GetParam reads (the use data's only with use kind 1; with 0 or 2, every use parameter
// is the first byte)
enum Param : u32 {
    kParamId,
    kParamPrice,
    kParamHeldEffect,
    kParamHeldArgument,
    kParamNaturalGiftEffect,
    kParamFlingEffect,
    kParamFlingPower,
    kParamNaturalGiftPower,
    kParamNaturalGiftType,
    kParamUnk09,
    kParamUnk0A,
    kParamPocket,
    kParamUnk0C,
    kParamFieldEffect,
    kParamBattleEffect,
    kParamUseKind,
    kParamItemClass,
    kParamUnk11,
    kParamUnk12,
    kParamSortIndex,
    kParamCureSleep,                      // 0x14
    kParamCurePoison,
    kParamCureBurn,
    kParamCureFreeze,
    kParamCureParalysis,
    kParamCureConfusion,
    kParamCureInfatuation,
    kParamGuardSpec,
    kParamRevive,                         // 0x1C
    kParamReviveAll,
    kParamLevelUp,
    kParamEvolve,
    kParamBoostAttack,                    // 0x20: stages raised in battle
    kParamBoostDefense,
    kParamBoostSpAttack,
    kParamBoostSpDefense,
    kParamBoostSpeed,
    kParamBoostAccuracy,
    kParamBoostCritical,
    kParamPpUp,                           // 0x27
    kParamPpMax,
    kParamRestorePp,                      // one move's
    kParamRestoreAllPp,
    kParamRestoreHp,
    kParamChangeEvHp,                     // 0x2C: flags for the EV changes
    kParamChangeEvAttack,
    kParamChangeEvDefense,
    kParamChangeEvSpeed,
    kParamChangeEvSpAttack,
    kParamChangeEvSpDefense,              // 0x31
    kParamEvAboveCap,
    kParamFriendship1,
    kParamFriendship2,
    kParamFriendship3,
    kParamEvHp,                           // 0x36: the EV changes (signed)
    kParamEvAttack,
    kParamEvDefense,
    kParamEvSpeed,
    kParamEvSpAttack,
    kParamEvSpDefense,
    kParamHpAmount,                       // 0x3C
    kParamPpAmount,
    kParamFriendshipAmount1,              // 0x3E (signed)
    kParamFriendshipAmount2,
    kParamFriendshipAmount3,
};

// What using an item does, as Item::GetEffect sorts it
enum Effect : u32 {
    kEffectReviveAll,                     // Sacred Ash
    kEffectLevelUp,                       // Rare Candy
    kEffectCureSleep,
    kEffectCurePoison,
    kEffectCureBurn,
    kEffectCureFreeze,
    kEffectCureParalysis,
    kEffectCureConfusion,
    kEffectCureAll,                       // the six above
    kEffectCureInfatuation,
    kEffectRestoreHp,                     // also curing everything (Full Restore)
    kEffectRevive,
    kEffectRaiseEv,                       // 12-17: HP, Atk, Def, SpA, SpD, Spe
    kEffectLowerEv = kEffectRaiseEv + 6,  // 18-23
    kEffectEvolve = kEffectLowerEv + 6,   // evolution stones
    kEffectPpUp,
    kEffectPpMax,
    kEffectRestorePp,
    kEffectNone,
};

// An item's data, loaded by ID
class Item : public gfl::RefObject {
public:
    explicit Item(bool clear);            // ID 0; the data zeroed if `clear`
    explicit Item(u32 id);
    ~Item() override;

    void Load(u32 id);                    // nothing changes but the ID when there's no such file
    void CopyFrom(const Item& other);
    s32 GetParam(u32 param) const;
    Effect GetEffect() const;
    bool IsUsedOnMove() const;            // PP Up, PP Max, and restoring one move's PP

    // The layout
    ItemData data_;
    u32 id_;

private:
    void Read(u32 id);                    // what the constructor loads with (the same as Load)
};
DECOMP_OFFSET(Item, data_, 0x54);
DECOMP_OFFSET(Item, id_, 0x78);
DECOMP_SIZE(Item, 0x80);

// The per-item flags (one byte per item, from their own file)
extern gfl::mem::UniquePtr<u8[]> g_flags;
void LoadFlags(gfl::mem::MemoryResource* resource);
void UnloadFlags();
u8 GetFlagsLow(u32 item, u32 kind);       // the low 3 bits, for kind 11 only (0 otherwise)
bool GetFlag6(u32 item);
bool GetFlag7(u32 item);

// Poké Balls: ball IDs 1-26 (1-16: Master Ball to Cherish Ball, then Fast, Level, Lure, Heavy,
// Love, Friend, Moon, Sport and Dream Ball, then Beast Ball), 0 for anything else
extern const u16 kBallItems[26];
u8 GetBallId(u16 item);
u16 GetBallItem(s32 ball);

// TMs (0-99) and HMs (100-106) by the index the move tables use; -1 for anything else
extern const u16 kMachineItems[100];
extern const u16 kMachineMoves[100];
extern const u16 kHmMoves[7];
bool IsMachine(u16 item);
s32 GetMachineIndex(u16 item);
u16 GetMachineMove(u16 item);             // 0 for anything else
u16 GetMachineMoveByIndex(u32 index);

// Berries, by their index in the berry list (73 of them)
extern const u16 kBerries[73];
bool IsBerry(u16 item);
u16 GetBerry(u8 index);                   // 0 past the end

bool IsMegaStone(u16 item);
bool IsGem(u16 item);
bool IsZCrystal(u16 item);                // the ones kept in the bag
bool IsHeldZCrystal(u16 item);
// The Let's Go items 960-1057 (the Candies)
extern const u16 kCandies[98];
bool IsCandy(u16 item);
bool IsUnk121(u16 item);                  // 121-128 except 126 (what they are isn't known yet)

}  // namespace item
