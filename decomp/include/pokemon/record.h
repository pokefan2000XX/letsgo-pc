// A Pokémon's stored data: the 0x104-byte record (PKHeX calls this format PB7).
//
// The record is CoreData (0xE8 bytes: a header and four 0x38-byte blocks) followed by CalcData
// (0x1C bytes of party stats, only kept for party Pokémon). Both are encrypted with the header's
// encryption key, and the key also decides the order of the four blocks in memory. The game never
// unshuffles them: every access looks up where its block is (Accessor in accessor.h). The checksum
// is the sum of the decrypted blocks' 16-bit words; a record that fails it is a bad egg, and its
// fields read from fixed default blocks instead.
//
// Field names follow PKHeX where the game's use agrees; offsets in comments are from the start of
// the decrypted, unshuffled record.
#pragma once
#include "decomp.h"
#include "gfl/mem.h"
#include "pokemon/ids.h"

namespace pokemon {

constexpr u32 kBlockSize = 0x38;
constexpr u32 kBlockCount = 4;

struct BlockA {
    u16 species;                // 0x08
    u16 heldItem;               // 0x0A
    u32 id;                     // 0x0C  trainer ID in the low 16 bits, secret ID in the high
    u32 exp;                    // 0x10
    u8 ability;                 // 0x14
    u8 abilityFlags;            // 0x15  bit 0, 1, 2: first, second, hidden ability; bit 3: favorite
    u16 markings;               // 0x16  2 bits per mark
    u32 pid;                    // 0x18
    u8 nature;                  // 0x1C
    u8 genderForm;              // 0x1D  bit 0: fateful encounter, bits 1-2: gender, bits 3-7: form
    u8 ev[6];                   // 0x1E  HP, Atk, Def, Spe, SpA, SpD
    u8 av[6];                   // 0x24  awakening values, same order
    u8 unk2A;                   // 0x2A
    u8 pokerus;                 // 0x2B
    f32 heightAbsolute;         // 0x2C
    u32 ribbons[2];             // 0x30  64 flags
    u8 unk38[2];                // 0x38
    u8 heightScalar;            // 0x3A
    u8 weightScalar;            // 0x3B
    u32 unk3C;                  // 0x3C
};
DECOMP_SIZE(BlockA, kBlockSize);
DECOMP_OFFSET(BlockA, pid, 0x10);
DECOMP_OFFSET(BlockA, ev, 0x16);
DECOMP_OFFSET(BlockA, ribbons, 0x28);
DECOMP_OFFSET(BlockA, heightScalar, 0x32);

struct BlockB {
    u16 nickname[13];           // 0x40  12 characters and a terminator
    u16 moves[4];               // 0x5A
    u8 pp[4];                   // 0x62
    u8 ppUps[4];                // 0x66
    u16 relearnMoves[4];        // 0x6A
    u8 unk72[2];                // 0x72
    u32 ivs;                    // 0x74  5 bits each: HP, Atk, Def, Spe, SpA, SpD; bit 30: egg; bit 31: nicknamed
};
DECOMP_SIZE(BlockB, kBlockSize);
DECOMP_OFFSET(BlockB, moves, 0x1A);
DECOMP_OFFSET(BlockB, ivs, 0x34);

struct BlockC {
    u16 handlerName[13];        // 0x78  the latest trainer other than the original one
    u8 handlerGender;           // 0x92
    u8 currentHandler;          // 0x93  0: the original trainer
    u32 unk94;                  // 0x94
    u8 unk98[0x0A];             // 0x98
    u8 handlerFriendship;       // 0xA2
    u8 unkA3;                   // 0xA3
    u8 unkA4;                   // 0xA4  (Gen 7: memory intensity)
    u8 unkA5;                   // 0xA5  (Gen 7: memory)
    u8 unkA6;                   // 0xA6  (Gen 7: memory feeling)
    u8 unkA7;                   // 0xA7
    u16 unkA8;                  // 0xA8  (Gen 7: memory variable)
    u8 unkAA[2];                // 0xAA
    u8 unkAC;                   // 0xAC
    u8 unkAD;                   // 0xAD
    u8 unkAE;                   // 0xAE  only read and written for party Pokémon
    u8 unkAF;                   // 0xAF
};
DECOMP_SIZE(BlockC, kBlockSize);
DECOMP_OFFSET(BlockC, currentHandler, 0x1B);
DECOMP_OFFSET(BlockC, handlerFriendship, 0x2A);

struct BlockD {
    u16 otName[13];             // 0xB0  original trainer
    u8 otFriendship;            // 0xCA
    u8 unkCB[6];                // 0xCB
    u8 eggDate[3];              // 0xD1  year (from 2000), month, day
    u8 metDate[3];              // 0xD4
    u8 unkD7;                   // 0xD7
    u16 eggLocation;            // 0xD8
    u16 metLocation;            // 0xDA
    u8 ball;                    // 0xDC
    u8 metLevelOtGender;        // 0xDD  bits 0-6: met level, bit 7: original trainer's gender
    u8 hyperTraining;           // 0xDE
    u8 version;                 // 0xDF
    u8 country;                 // 0xE0
    u8 region;                  // 0xE1
    u8 consoleRegion;           // 0xE2
    u8 language;                // 0xE3
    f32 weightAbsolute;         // 0xE4
};
DECOMP_SIZE(BlockD, kBlockSize);
DECOMP_OFFSET(BlockD, otFriendship, 0x1A);
DECOMP_OFFSET(BlockD, language, 0x33);

// Party stats, recalculated from the stored data (not part of a boxed Pokémon)
struct CalcData {
    u32 status;                 // 0xE8  status condition
    u8 level;                   // 0xEC
    u8 unkED;                   // 0xED
    u8 unkEE;                   // 0xEE
    u8 unkEF;                   // 0xEF
    u16 hp;                     // 0xF0  current HP
    u16 maxHp;                  // 0xF2
    u16 attack;                 // 0xF4
    u16 defense;                // 0xF6
    u16 speed;                  // 0xF8
    u16 spAttack;               // 0xFA
    u16 spDefense;              // 0xFC
    u16 cp;                     // 0xFE  combat power
    u8 unk100;                  // 0x100
    u8 unk101;                  // 0x101
    u8 unk102[2];               // 0x102
};
DECOMP_SIZE(CalcData, 0x1C);
DECOMP_OFFSET(CalcData, hp, 0x08);
DECOMP_OFFSET(CalcData, cp, 0x16);

struct CoreData {
    u32 encryptionKey;          // 0x00  also picks the block order
    u16 flags;                  // 0x04  kFlagBadEgg
    u16 checksum;               // 0x06
    u8 blocks[kBlockCount][kBlockSize];  // 0x08  encrypted, in the order kBlockOrder gives
};
DECOMP_SIZE(CoreData, 0xE8);
DECOMP_OFFSET(CoreData, blocks, 0x08);

constexpr u16 kFlagBadEgg = 1 << 2;    // the checksum failed
constexpr u32 kRecordSize = sizeof(CoreData) + sizeof(CalcData);

// For each shuffle value (bits 13-17 of the key), the position of block A, B, C and D
extern const u8 kBlockOrder[32][kBlockCount];

inline u32 ShuffleValue(u32 encryptionKey) {
    return (encryptionKey >> 13) & 31;
}

// What a bad egg's fields read as (set up by InitDefaultRecords), and where writes to them go
extern CalcData g_badEggCalc;
extern BlockA g_badEggBlockA;
extern BlockB g_badEggBlockB;
extern BlockC g_badEggBlockC;
extern BlockD g_badEggBlockD;
extern CalcData g_discardCalc;
extern BlockA g_discardBlockA;
extern BlockB g_discardBlockB;
extern BlockC g_discardBlockC;
extern BlockD g_discardBlockD;

// Encryption: each 16-bit word is XORed with the high half of the next value of a linear
// congruential generator seeded with the key. Encoding and decoding are the same operation.
void EncodeData(void* data, u32 size, u32 seed);
void DecodeData(void* data, u32 size, u32 seed);
u16 CalcChecksum(const void* data, u32 size);

void InitDefaultRecords();

// What a new Pokémon is made from. Fields left at their "random" values are rolled when it's made
// (ResolveSpec); the defaults make a random level 1 egg.
struct CreationSpec {
    static constexpr u64 kRandom = ~0ull;
    static constexpr u64 kPidNotShiny = 0x1FFFFFFFF;   // a random PID that isn't shiny
    static constexpr u64 kPidShiny = 0x2FFFFFFFF;      // a random shiny PID
    static constexpr u64 kPidRolls = 0x3FFFFFFFF;      // a random PID, rerolled until shiny up to shinyRolls times

    u64 key = kRandom;                    // the encryption key
    u64 pid = kPidRolls;
    u64 id = kRandom;                     // the original trainer's ID
    u32 species = kSpeciesEgg;
    u8 form = 0;                          // 0xFF: random (0 for species with Mega Evolutions)
    u16 level = 1;                        // 100 at most
    u16 gender = 0xFF;                    // 0xFF: by the species' gender ratio
    s16 nature = -1;                      // -1: random
    u8 ability = 0xFF;                    // slot 0-2; 0xFF: 0 or 1 at random, 0xFE: 0, 1 or 2
    u8 shinyRolls = 1;
    s16 ivs[6] = {-1, -1, -1, -1, -1, -1};    // in Stat order; -1: random
    u32 friendship = 0xFFFF;              // 0xFFFF: the species' base friendship
    u8 perfectIvs = 0;                    // how many of the random IVs are 31 instead (6 or more: none)
    u8 sizeRolls = 1;                     // the most extreme of this many size rolls is kept (0: none)
    u16 weight = 0x100;                   // a scalar, or how to roll one (RollSize)
    u16 height = 0x100;
};
DECOMP_SIZE(CreationSpec, 0x40);

// New zeroed records from a memory resource. CalcData is returned encrypted with the key.
gfl::mem::UniquePtr<CoreData> CreateCoreData(gfl::mem::MemoryResource* resource);
gfl::mem::UniquePtr<CalcData> CreateCalcData(gfl::mem::MemoryResource* resource, u32 encryptionKey);
// A new Pokémon's record (encrypted): from a spec, or of a species at a level with a random PID
gfl::mem::UniquePtr<CoreData> CreateCoreData(gfl::mem::MemoryResource* resource, const CreationSpec& spec);
gfl::mem::UniquePtr<CoreData> CreateCoreData(gfl::mem::MemoryResource* resource, u32 species, u16 level, u64 id);
// The spec with everything left to chance rolled
void ResolveSpec(CreationSpec* out, const CreationSpec* spec);
// Writes a resolved spec's values into a record (nothing when the species is 0)
void InitCoreData(CoreData* core, const CreationSpec* spec);
// A size scalar from a spec's value: 0-255 as it is, 0x100 at random (two rolls added, so average
// sizes are likelier), 0x101-0x105 at random within XS, S, M, L or XL, and 128 for anything above.
// Values that are negative as an s16 stay as they are.
u16 RollSize(u16 value);

// Stat formulas (Let's Go: awakening values add flat, friendship adds up to 10%, no EVs)
u16 CalcHp(u32 species, u8 level, u16 base, u16 iv, u8 av);
u16 CalcAttack(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8 unused, u8 friendship);
u16 CalcDefense(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8 unused, u8 friendship);
u16 CalcSpAttack(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8 unused, u8 friendship);
u16 CalcSpDefense(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8 unused, u8 friendship);
u16 CalcSpeed(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8 unused, u8 friendship);
// +1, -1 or 0 for stats 1-5 (Atk, Def, SpA, SpD, Spe), 0 for anything else
s8 GetNatureModifier(u16 nature, u32 stat);
u8 CalcLevelFromExp(u32 species, u32 form, u32 exp);

// Combat power: from the stats without awakening values, plus the awakening values' share
s32 CalcBaseCp(u16 level, u16 nature, u16 baseHp, u16 baseAtk, u16 baseDef, u16 baseSpA, u16 baseSpD,
               u16 baseSpe, u16 ivHp, u16 ivAtk, u16 ivDef, u16 ivSpA, u16 ivSpD, u16 ivSpe, u8 friendship);
u16 CalcCp(u8 level, u16 nature, u32 unused, u8 friendship, u16 baseHp, u16 baseAtk, u16 baseDef, u16 baseSpA,
           u16 baseSpD, u16 baseSpe, u16 ivHp, u16 ivAtk, u16 ivDef, u16 ivSpA, u16 ivSpD, u16 ivSpe, u16 avHp,
           u16 avAtk, u16 avDef, u16 avSpA, u16 avSpD, u16 avSpe);

// Shininess: the trainer's and the PID's 16-bit halves XOR to less than 16
bool IsShiny(u32 id, u32 pid);
u32 MakeNonShinyPid(u32 id, u32 pid);
u32 MakeShinyPid(u32 id, u32 pid);

// The gender a species can have (fixed for single-gender and genderless species)
u32 GetValidGender(u32 species, u32 form, u32 gender);
u16 GetAbilityId(u32 species, u32 form, u8 slot);
u8 GetHiddenPowerType(u8 ivHp, u8 ivAtk, u8 ivDef, u8 ivSpe, u8 ivSpA, u8 ivSpD);
// How a nature takes a flavor (spicy, dry, sweet, bitter, sour): 0 neutral, 1 likes, 2 dislikes
u32 GetNatureFlavor(u32 nature, u32 flavor);

// Arceus's type from its held plate or Z-Crystal, Silvally's from its memory (0: none)
u8 GetPlateType(u32 item);
u8 GetMemoryType(u32 item);

// 2-bit fields in a 16-bit value (markings), and the two low bytes of a 32-bit value
u16 GetBitPair(u16 bits, u16 index);
u16 SetBitPair(u16 bits, u16 index, u16 value);
u32 SetHighByte(u32 value, u8 byte);
u32 SetLowByte(u32 value, u8 byte);

}  // namespace pokemon
