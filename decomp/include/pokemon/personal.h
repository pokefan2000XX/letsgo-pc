// Species data ("personal" data: base stats, types, abilities, gender ratio, growth rate), read
// for the species and form loaded last. Not decompiled yet (D1 step 3): these link to the originals.
#pragma once
#include "decomp.h"
#include "gfl/str.h"

namespace pokemon::personal {

enum Param : u32 {
    kParamType1 = 0x06,
    kParamGenderRatio = 0x14,   // 0: male only, 0xFE: female only, 0xFF: genderless
    kParamGrowthRate = 0x17,
    kParamAbility1 = 0x1A,
    kParamAbility2 = 0x1B,
    kParamAbilityHidden = 0x1C,
};

void Load(u32 species, u32 form);
u32 GetParam(u32 param);
// The EXP table of the species' growth rate
void LoadGrowthTable(u32 species, u32 form);
u32 GetMinExp(u8 level);

}  // namespace pokemon::personal

namespace pokemon {

// A species' name in a language (1-10); false when either is out of range
bool GetSpeciesName(gfl::str::StrBuf* out, u32 species, u32 language);
bool GetSpeciesName(u16* out, u32 species, u32 language);

}  // namespace pokemon
