// PersonalTable: species and form entries of the species data (pokemon/personal.h).
#include <string.h>

#include "pokemon/personal.h"
#include "pokemon/system.h"

namespace pokemon::personal {

// 0x710071d800: the entries are copied whole, so a file that isn't a multiple of an entry's
// size writes past them
void PersonalTable::LoadAll(gfl::mem::MemoryResource* resource, void* source) {
    if (g_personalSource != nullptr) {
        return;
    }
    g_personalSource = source;
    gfl::fs::Path path = GetPersonalDataPath();
    gfl::fs::ReadRequest request{};
    gfl::fs::ReadFile(&request, path, 0, GetSystem()->fileDevice, 0);
    size_t size = request.file_->size_;
    if (size != 0) {
        g_personalData = gfl::mem::MakeUniqueArray<PersonalInfo>(resource, size / sizeof(PersonalInfo));
        memcpy(g_personalData.Get(), request.file_->data_, size);
    }
}

// 0x710071dad0
void PersonalTable::UnloadAll() {
    if (g_personalSource != nullptr) {
        g_personalData.Reset();
        g_personalSource = nullptr;
    }
}

// 0x710071db30
PersonalTable::PersonalTable(gfl::mem::MemoryResource* resource) : species_(0), form_(0) {
    info_ = gfl::mem::MakeUnique<PersonalInfo>(resource);
}

// 0x710071dc60, deleting: 0x710071dca0
PersonalTable::~PersonalTable() {}

// 0x710071dcf0
u32 PersonalTable::GetSpecies() const {
    return species_;
}

// 0x710071dd00
u8 PersonalTable::GetForm() const {
    return form_;
}

// 0x710071dd10
void PersonalTable::Load(s32 species, u8 form) {
    u32 index = GetEntryIndex(species, form);
    memcpy(info_.Get(), &g_personalData.Get()[index], sizeof(PersonalInfo));
    species_ = species;
    form_ = form;
}

// 0x710071ddd0
u32 PersonalTable::GetEntryIndex(s32 species, u8 form) {
    if (species > 809 || (species | 1) == 811) {
        return 0;
    }
    if (form == 0) {
        return species;
    }
    const PersonalInfo& entry = g_personalData.Get()[species];
    if (entry.formStatsIndex == 0) {
        return species;
    }
    return form < entry.formCount ? entry.formStatsIndex + form - 1 : species;
}

// 0x710071de40
u32 PersonalTable::GetParam(u32 param) const {
    const PersonalInfo& info = *info_;
    switch (param) {
    case kParamBaseHp:
    case kParamBaseAttack:
    case kParamBaseDefense:
    case kParamBaseSpeed:
    case kParamBaseSpAttack:
    case kParamBaseSpDefense:
        return info.baseStats[param - kParamBaseHp];
    case kParamType1: return info.types[0];
    case kParamType2: return info.types[1];
    case kParamCatchRate: return info.catchRate;
    case kParamBaseExp: return info.baseExp;
    case kParamEvYieldHp:
    case kParamEvYieldHp + 1:
    case kParamEvYieldHp + 2:
    case kParamEvYieldHp + 3:
    case kParamEvYieldHp + 4:
    case kParamEvYieldHp + 5:
        return info.evYield >> (param - kParamEvYieldHp) * 2 & 3;
    case kParamUnk10: return info.evYield >> 12 & 1;
    case kParamHeldItem1:
    case kParamHeldItem1 + 1:
    case kParamHeldItem1 + 2:
        return info.heldItems[param - kParamHeldItem1];
    case kParamGenderRatio: return info.genderRatio;
    case kParamHatchCycles: return info.hatchCycles;
    case kParamBaseFriendship: return info.baseFriendship;
    case kParamGrowthRate: return info.growthRate;
    case kParamEggGroup1: return info.eggGroups[0];
    case kParamEggGroup2: return info.eggGroups[1];
    case kParamAbility1: return info.abilities[0];
    case kParamAbility2: return info.abilities[1];
    case kParamAbilityHidden: return info.abilities[2];
    case kParamFormStatsIndex: return info.formStatsIndex;
    case kParamFormSprite: return info.formSprite;
    case kParamFormCount: return info.formCount;
    case kParamColor: return info.color & 0x3F;
    case kParamEvolutionStage: return info.evolutionStage;
    case kParamHeight: return info.height;
    case kParamWeight: return info.weight;
    case kParamUnk2D: return info.unk1B;
    case kParamUnk2E: return info.unk4C;
    case kParamUnk2F: return info.unk4E;
    case kParamUnk30: return info.unk50;
    case kParamUnk31: return info.unk52;
    }
    if (param >= kParamMachines && param < kParamMachines + 8) {
        return info.machines[param - kParamMachines];
    }
    return 0;
}

// 0x710071e0e0
bool PersonalTable::CanLearnMachine(u32 machine) const {
    return (GetParam((machine >> 5) + kParamMachines) & 1u << (machine & 0x1F)) != 0;
}

}  // namespace pokemon::personal

// ---- the file's global objects: set up at startup, destroyed at exit (what the compiler writes) -----

namespace pokemon::personal {

// 0x710071d7d0
void DestroyPersonalData(gfl::mem::UniquePtr<PersonalInfo[]>* data) {
    data->Reset();
}

}  // namespace pokemon::personal

extern "C" void FUN_710071d7d0(void*);    // DestroyPersonalData, as the game registers it

namespace pokemon::personal {

// 0x710071e120
void InitializePersonalTableGlobals() {
    new (&g_personalData) gfl::mem::UniquePtr<PersonalInfo[]>();
    __cxa_atexit(FUN_710071d7d0, &g_personalData, &__dso_handle);
}

}  // namespace pokemon::personal
