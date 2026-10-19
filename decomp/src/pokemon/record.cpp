// The Pokémon record: encryption, checksum, the default records, making new records, and the
// formulas that work on its values (stats, combat power, level, shininess). See pokemon/record.h.
#include "pokemon/record.h"

#include <string.h>

#include "pokemon/accessor.h"
#include "pokemon/personal.h"
#include "pokemon/system.h"

namespace pokemon {

// Tables in the game's data
// The natures' stat modifiers (+1, -1, 0) for Atk, Def, SpA, SpD and Spe
extern const s8 kNatureModifiers[25][5];
// How each nature takes each flavor
extern const u32 kNatureFlavors[25][5];
// Hidden Power's type for each of 16 values
extern const u8 kHiddenPowerTypes[16];

struct ItemType {
    u16 item;
    u8 type;
    u8 pad;
};
// Arceus's plates and Z-Crystals; Silvally's memories
extern const ItemType kPlateTypes[34];
extern const ItemType kMemoryTypes[17];

namespace {

// The game's linear congruential generator (the one Pokémon games have used since Gen 3)
u32 NextSeed(u32 seed) {
    return seed * 0x41C64E6D + 0x6073;
}

// Friendship's share of a stat, in percent: 100 at 0 friendship, 110 at 255
s32 FriendshipFactor(u8 friendship) {
    return static_cast<s32>((static_cast<f32>(friendship) / 255.0f / 10.0f + 1.0f) * 100.0f);
}

// A non-HP stat before awakening values
u32 CalcStatBase(u32 level, u16 base, u16 iv, s8 natureModifier, s32 friendshipFactor) {
    u16 doubled = base * 2 + iv;
    u32 stat = doubled * level / 100 + 5;
    if (natureModifier == 1) {
        stat = static_cast<u16>(stat * 110) / 100;
    } else if (natureModifier == -1) {
        stat = static_cast<u16>(stat * 90) / 100;
    }
    return static_cast<u16>(stat * friendshipFactor) / 100;
}

u16 CalcStat(u32 stat, u8 level, u16 base, u16 iv, u16 nature, u8 av, u8 friendship) {
    return CalcStatBase(level, base, iv, kNatureModifiers[nature][stat], FriendshipFactor(friendship)) + av;
}

}  // namespace

// 0x7100739420
void EncodeData(void* data, u32 size, u32 seed) {
    u16* words = static_cast<u16*>(data);
    for (u32 i = 0; i < size / 2; i++) {
        seed = NextSeed(seed);
        words[i] ^= seed >> 16;
    }
}

// 0x71007394b0
void DecodeData(void* data, u32 size, u32 seed) {
    u16* words = static_cast<u16*>(data);
    for (u32 i = 0; i < size / 2; i++) {
        seed = NextSeed(seed);
        words[i] ^= seed >> 16;
    }
}

// 0x71007393b0
u16 CalcChecksum(const void* data, u32 size) {
    const u16* words = static_cast<const u16*>(data);
    u16 sum = 0;
    for (u32 i = 0; i < size / 2; i++) {
        sum += words[i];
    }
    return sum;
}

// 0x7100738770: a bad egg reads as an egg (kSpeciesEgg) with 1 max HP and everything else zero
void InitDefaultRecords() {
    g_badEggCalc = CalcData();
    g_badEggBlockA = BlockA();
    g_badEggBlockB = BlockB();
    g_badEggBlockC = BlockC();
    g_badEggBlockD = BlockD();
    g_badEggCalc.maxHp = 1;
    g_badEggBlockA.species = kSpeciesEgg;
}

// 0x7100739540
gfl::mem::UniquePtr<CoreData> CreateCoreData(gfl::mem::MemoryResource* resource) {
    gfl::mem::UniquePtr<CoreData> core = gfl::mem::MakeUnique<CoreData>(resource);
    memset(core.Get(), 0, sizeof(CoreData));
    return core;
}

// 0x7100739640
gfl::mem::UniquePtr<CoreData> CreateCoreData(gfl::mem::MemoryResource* resource, u32 species, u16 level, u64 id) {
    CreationSpec spec;
    spec.species = species;
    spec.level = level;
    spec.id = id;
    return CreateCoreData(resource, spec);
}

// 0x71007396b0
gfl::mem::UniquePtr<CoreData> CreateCoreData(gfl::mem::MemoryResource* resource, const CreationSpec& spec) {
    gfl::mem::UniquePtr<CoreData> core = CreateCoreData(resource);
    CreationSpec resolved;
    ResolveSpec(&resolved, &spec);
    InitCoreData(core.Get(), &resolved);
    return core;
}

// How far a size scalar is from the average (128)
static s32 SizeDistance(u16 scalar) {
    return scalar > 0x7F ? scalar - 0x80 : 0x80 - scalar;
}

// 0x7100739820. Random values are rolled in field order (the key, the trainer ID, the PID, IVs,
// ability, nature, form, gender), the size last.
void ResolveSpec(CreationSpec* out, const CreationSpec* spec) {
    memcpy(out, spec, sizeof(CreationSpec));
    if (out->key == CreationSpec::kRandom) {
        out->key = Random();
    }
    if (out->id == CreationSpec::kRandom) {
        out->id = Random();
    }
    u32 id = out->id;
    if (out->pid == CreationSpec::kPidRolls) {
        for (s32 i = 0; i < out->shinyRolls; i++) {
            out->pid = Random();
            if (IsShiny(out->id, out->pid)) {
                break;
            }
        }
    } else if (out->pid == CreationSpec::kPidShiny) {
        out->pid = MakeShinyPid(id, Random());
    } else if (out->pid == CreationSpec::kPidNotShiny) {
        out->pid = MakeNonShinyPid(id, Random());
    }
    if (out->level > 100) {
        out->level = 100;
    }
    if (out->perfectIvs != 0) {
        for (s16& iv : out->ivs) {
            iv = -1;
        }
        if (out->perfectIvs >= 6) {
            out->perfectIvs = 0;
        }
        for (u8 i = 0; i < out->perfectIvs; i++) {
            s16* iv;
            do {
                iv = &out->ivs[static_cast<u8>(Random(6))];
            } while (*iv != -1);
            *iv = 31;
        }
    }
    for (s16& iv : out->ivs) {
        if (iv == -1) {
            iv = Random(32);
        }
    }
    if (out->ability == 0xFE) {
        out->ability = Random(3);
    } else if (out->ability == 0xFF) {
        out->ability = Random(2);
    }
    if (out->nature == -1) {
        out->nature = Random(25);
    }
    if (out->form == 0xFF) {
        personal::LoadMegaEvolutions(out->species);
        if (personal::GetMegaEvolutionCount() != 0) {
            out->form = 0;
        } else {
            personal::Load(out->species, 0);
            out->form = Random(static_cast<u8>(personal::GetParam(personal::kParamFormCount)));
        }
    }
    if (out->gender == 0xFF) {
        personal::Load(out->species, out->form);
        u32 ratio = personal::GetParam(personal::kParamGenderRatio);
        if (ratio == 0) {
            out->gender = 0;
        } else if (ratio == 0xFF) {
            out->gender = 2;
        } else if (ratio == 0xFE) {
            out->gender = 1;
        } else {
            out->gender = Random(0xFC) + 1 < ratio;     // female below the ratio
        }
    }
    out->gender = GetValidGender(out->species, out->form, out->gender);
    if (out->friendship == 0xFFFF) {
        personal::Load(out->species, out->form);
        out->friendship = personal::GetParam(personal::kParamBaseFriendship);
    }
    u16 height = 0xFFFF;
    u16 weight = 0xFFFF;
    for (s32 i = 0; i < out->sizeRolls; i++) {
        u16 rolledHeight = RollSize(out->height);
        u16 rolledWeight = RollSize(out->weight);
        if (height == 0xFFFF || SizeDistance(rolledHeight) > SizeDistance(height)) {
            height = rolledHeight;
        }
        if (weight == 0xFFFF || SizeDistance(rolledWeight) > SizeDistance(weight)) {
            weight = rolledWeight;
        }
    }
    out->height = height;
    out->weight = weight;
}

// 0x7100739cd0: met at its level in version 0, with the game's language and the console's region
void InitCoreData(CoreData* core, const CreationSpec* spec) {
    Accessor accessor;
    accessor.AttachDecoded(core, nullptr);
    if (spec->species != 0) {
        accessor.SetEncryptionKey(spec->key);
        accessor.SetId(spec->id);
        accessor.SetPid(spec->pid);
        accessor.SetSpecies(spec->species);
        accessor.SetForm(spec->form);
        accessor.SetPartyLevel(spec->level);
        accessor.SetNature(spec->nature);
        accessor.SetGender(spec->gender);
        accessor.SetMetLevel(spec->level);
        accessor.SetVersion(0);
        accessor.SetFriendship(spec->friendship);
        accessor.SetLanguage(GetSystem()->language);
        accessor.SetCountry(GetSystem()->country);
        accessor.SetRegion(GetSystem()->region);
        accessor.SetConsoleRegion(GetSystem()->consoleRegion);
        accessor.SetIvHp(spec->ivs[0]);
        accessor.SetIvAttack(spec->ivs[1]);
        accessor.SetIvDefense(spec->ivs[2]);
        accessor.SetIvSpAttack(spec->ivs[3]);
        accessor.SetIvSpDefense(spec->ivs[4]);
        accessor.SetIvSpeed(spec->ivs[5]);
        accessor.SetAbility(GetAbilityId(spec->species, spec->form, spec->ability));
        accessor.SetFirstAbility(spec->ability == 0);
        accessor.SetSecondAbility(spec->ability == 1);
        accessor.SetHiddenAbility(spec->ability == 2);
        accessor.SetFavorite(0);
        personal::LoadGrowthTable(spec->species, spec->form);
        accessor.SetExp(personal::GetMinExp(spec->level));
        accessor.SetHeightScalar(spec->height);
        accessor.SetWeightScalar(spec->weight);
    }
}

// 0x7100739ed0
u16 RollSize(u16 value) {
    s16 kind = static_cast<s16>(value);
    if (kind < 0x100) {
        return value;
    }
    switch (kind - 0x100) {
    case 0: {
        u32 first = Random(0x81);
        return Random(0x80) + first;
    }
    case 1:
        return Random(0x10);
    case 2:
        return Random(0x20) + 0x10;
    case 3:
        return Random(0xA0) + 0x30;
    case 4:
        return Random(0x20) + 0xD0;
    case 5:
        return Random(0x10) + 0xF0;
    }
    return 0x80;
}

// 0x7100739fc0
gfl::mem::UniquePtr<CalcData> CreateCalcData(gfl::mem::MemoryResource* resource, u32 encryptionKey) {
    gfl::mem::UniquePtr<CalcData> calc = gfl::mem::MakeUnique<CalcData>(resource);
    *calc = CalcData();
    EncodeData(calc.Get(), sizeof(CalcData), encryptionKey);
    return calc;
}

// 0x7100738870: Shedinja always has 1 HP
u16 CalcHp(u32 species, u8 level, u16 base, u16 iv, u8 av) {
    if (species == 292) {
        return 1;
    }
    u16 doubled = base * 2 + iv;
    return doubled * level / 100 + level + av + 10;
}

// 0x71007388b0
u16 CalcAttack(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8, u8 friendship) {
    return CalcStat(0, level, base, iv, nature, av, friendship);
}

// 0x7100738970
u16 CalcDefense(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8, u8 friendship) {
    return CalcStat(1, level, base, iv, nature, av, friendship);
}

// 0x7100738a30
u16 CalcSpAttack(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8, u8 friendship) {
    return CalcStat(2, level, base, iv, nature, av, friendship);
}

// 0x7100738af0
u16 CalcSpDefense(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8, u8 friendship) {
    return CalcStat(3, level, base, iv, nature, av, friendship);
}

// 0x7100738bb0
u16 CalcSpeed(u8 level, u16 base, u16 iv, u16 nature, u8 av, u8, u8 friendship) {
    return CalcStat(4, level, base, iv, nature, av, friendship);
}

// 0x7100738c70
s8 GetNatureModifier(u16 nature, u32 stat) {
    if (stat - 1 > 4) {
        return 0;
    }
    return kNatureModifiers[nature][stat - 1];
}

// 0x7100738800: the highest level whose minimum EXP the Pokémon has reached
u8 CalcLevelFromExp(u32 species, u32 form, u32 exp) {
    personal::LoadGrowthTable(species, form);
    u8 level = 1;
    while (level < 101 && personal::GetMinExp(level) <= exp) {
        level++;
    }
    return level >= 2 ? level - 1 : 1;
}

// 0x7100738da0
s32 CalcBaseCp(u16 level, u16 nature, u16 baseHp, u16 baseAtk, u16 baseDef, u16 baseSpA, u16 baseSpD,
               u16 baseSpe, u16 ivHp, u16 ivAtk, u16 ivDef, u16 ivSpA, u16 ivSpD, u16 ivSpe, u8 friendship) {
    s32 factor = FriendshipFactor(friendship);
    u16 doubledHp = baseHp * 2 + ivHp;
    u16 total = doubledHp * level / 100 + level + 10;
    total += CalcStatBase(level, baseAtk, ivAtk, kNatureModifiers[nature][0], factor);
    total += CalcStatBase(level, baseDef, ivDef, kNatureModifiers[nature][1], factor);
    total += CalcStatBase(level, baseSpA, ivSpA, kNatureModifiers[nature][2], factor);
    total += CalcStatBase(level, baseSpD, ivSpD, kNatureModifiers[nature][3], factor);
    total += CalcStatBase(level, baseSpe, ivSpe, kNatureModifiers[nature][4], factor);
    return static_cast<s32>(static_cast<f32>(total) * 6.0f * static_cast<f32>(level) / 100.0f);
}

// 0x7100738ca0: capped at 10000
u16 CalcCp(u8 level, u16 nature, u32, u8 friendship, u16 baseHp, u16 baseAtk, u16 baseDef, u16 baseSpA,
           u16 baseSpD, u16 baseSpe, u16 ivHp, u16 ivAtk, u16 ivDef, u16 ivSpA, u16 ivSpD, u16 ivSpe, u16 avHp,
           u16 avAtk, u16 avDef, u16 avSpA, u16 avSpD, u16 avSpe) {
    s32 base = CalcBaseCp(level, nature, baseHp, baseAtk, baseDef, baseSpA, baseSpD, baseSpe, ivHp, ivAtk, ivDef,
                          ivSpA, ivSpD, ivSpe, friendship);
    u16 avTotal = avAtk + avHp + avDef + avSpA + avSpD + avSpe;
    s32 avShare = static_cast<s32>((static_cast<f32>(level) * 4.0f / 100.0f + 2.0f) * static_cast<f32>(avTotal));
    u16 cp = base + avShare;
    return cp < 10000 ? cp : 10000;
}

// 0x7100739050
bool IsShiny(u32 id, u32 pid) {
    return ((id >> 16) ^ (id & 0xFFF0) ^ (pid >> 16) ^ (pid & 0xFFF0)) < 0x10;
}

// 0x7100739070
u32 MakeNonShinyPid(u32 id, u32 pid) {
    return IsShiny(id, pid) ? pid ^ 0x10000000 : pid;
}

// 0x71007390a0
u32 MakeShinyPid(u32 id, u32 pid) {
    if (IsShiny(id, pid)) {
        return pid;
    }
    u32 high = ((id >> 16) ^ id ^ pid) & 0xFFFF;
    return (pid & 0xFFFF) | (high << 16);
}

// 0x71007390e0: 0 male, 1 female, 2 genderless
u32 GetValidGender(u32 species, u32 form, u32 gender) {
    personal::Load(species, form);
    switch (static_cast<u8>(personal::GetParam(personal::kParamGenderRatio))) {
    case 0xFE:
        return 1;
    case 0xFF:
        return 2;
    case 0x00:
        return 0;
    }
    return gender == 2 ? 0 : gender;
}

// 0x7100739140: slot 0 and 1 are the regular abilities, 2 the hidden one
u16 GetAbilityId(u32 species, u32 form, u8 slot) {
    u32 param = slot == 2 ? personal::kParamAbilityHidden
              : slot == 1 ? personal::kParamAbility2 : personal::kParamAbility1;
    personal::Load(species, form);
    return personal::GetParam(param);
}

// 0x7100739190: from the lowest bit of each IV
u8 GetHiddenPowerType(u8 ivHp, u8 ivAtk, u8 ivDef, u8 ivSpe, u8 ivSpA, u8 ivSpD) {
    u8 bits = (ivHp & 1) | (ivAtk & 1) << 1 | (ivDef & 1) << 2 | (ivSpe & 1) << 3 | (ivSpA & 1) << 4 |
              (ivSpD & 1) << 5;
    return kHiddenPowerTypes[bits * 15 / 63];
}

// 0x71007391e0
u32 GetNatureFlavor(u32 nature, u32 flavor) {
    return kNatureFlavors[nature][flavor];
}

// 0x7100739200
u8 GetPlateType(u32 item) {
    for (const ItemType& entry : kPlateTypes) {
        if (entry.item == item) {
            return entry.type;
        }
    }
    return 0;
}

// 0x7100739370
u8 GetMemoryType(u32 item) {
    for (const ItemType& entry : kMemoryTypes) {
        if (entry.item == item) {
            return entry.type;
        }
    }
    return 0;
}

// 0x710073a0c0
u16 GetBitPair(u16 bits, u16 index) {
    u16 shift = index * 2;
    return (bits & (3 << shift)) >> shift;
}

// 0x710073a0e0
u16 SetBitPair(u16 bits, u16 index, u16 value) {
    u16 shift = index * 2;
    return (bits & ~(3 << shift)) | value << shift;
}

// 0x710073a110
u32 SetHighByte(u32 value, u8 byte) {
    return (value & ~0xFF00u) | byte << 8;
}

// 0x710073a120
u32 SetLowByte(u32 value, u8 byte) {
    return (value & ~0xFFu) | byte;
}

}  // namespace pokemon
