// The species data's tables, made at startup, and the lookups everything else uses
// (pokemon/personal.h).
#include "pokemon/personal.h"

#include <string.h>

#include "pokemon/ids.h"
#include "pokemon/system.h"

namespace pokemon {

// 0x710071f980
bool GetSpeciesName(u16* out, u32 species, u32 language) {
    bool found = false;
    if (static_cast<s32>(species) <= 811 && language - 1 <= 9) {
        gfl::str::StrBuf* buffer = personal::g_nameBuffer.Get();
        u32 entry = personal::PersonalTable::GetEntryIndex(species, 0);
        found = personal::g_speciesNames[language]->GetString(entry, buffer);
    }
    gfl::str::StrNCopy(out, personal::g_nameBuffer->GetPtr(), 13);
    return found;
}

// 0x710071fa10
bool GetSpeciesName(gfl::str::StrBuf* out, u32 species, u32 language) {
    if (static_cast<s32>(species) > 811 || language - 1 > 9) {
        return false;
    }
    u32 entry = personal::PersonalTable::GetEntryIndex(species, 0);
    return personal::g_speciesNames[language]->GetString(entry, out);
}

}  // namespace pokemon

namespace pokemon::personal {

extern const u32 kNameBufferLength;       // characters

// 0x710071e8d0
void Initialize(gfl::mem::MemoryResource* resource) {
    PersonalTable::LoadAll(resource, GetSystem()->personalSource);
    Learnset::SetSource(resource, GetSystem()->learnsetSource);
    SetGrowthSource(resource, GetSystem()->growthSource);
    SetEvolutionSource(resource, GetSystem()->evolutionSource);
    SetMegaSource(resource, GetSystem()->megaSource);
    g_nameBuffer = gfl::Create<gfl::str::StrBuf>()(resource, kNameBufferLength);
    for (u32 i = 0; i < 11; i++) {
        if (GetSystem()->speciesNames[i].unk18_ != 0) {
            u32 mode = 1;
            g_speciesNames[i] = gfl::Create<gfl::str::MsgData>()(resource, GetSystem()->speciesNames[i], mode);
        }
    }
    g_personal = gfl::mem::MakeUnique<PersonalTable>(resource, resource);
    u32 capacity = GetSystem()->unkCacheCapacity;
    g_unkCache = gfl::mem::MakeUnique<UnkCache>(resource, resource, capacity);
    g_growth = gfl::mem::MakeUnique<GrowthTable>(resource);
    capacity = GetSystem()->growthCacheCapacity;
    g_growthCache = gfl::mem::MakeUnique<GrowthCache>(resource, resource, capacity);
    g_levelUpMoves = gfl::mem::MakeUnique<Learnset>(resource, resource);
    g_noSpeciesMoves = gfl::mem::MakeUnique<Learnset>(resource, resource);
    g_unkLearnset = gfl::mem::MakeUnique<Learnset>(resource, resource);
    capacity = GetSystem()->learnsetCacheCapacity;
    g_learnsetCache = gfl::mem::MakeUnique<LearnsetCache>(resource, resource, capacity);
    g_evolutions = gfl::mem::MakeUnique<EvolutionTable>(resource, resource);
    capacity = GetSystem()->evolutionCacheCapacity;
    g_evolutionCache = gfl::mem::MakeUnique<EvolutionCache>(resource, resource, capacity);
    g_megaEvolutions = gfl::mem::MakeUnique<MegaTable>(resource, resource);
    capacity = GetSystem()->megaCacheCapacity;
    g_megaCache = gfl::mem::MakeUnique<MegaCache>(resource, resource, capacity);
    g_growthPersonal = gfl::mem::MakeUnique<PersonalTable>(resource, resource);
    g_megaCheck = gfl::mem::MakeUnique<MegaTable>(resource, resource);

    gfl::fs::Path path = GetPmsDataPath();
    gfl::fs::ReadRequest request{};
    gfl::fs::ReadFile(&request, path, 0, GetSystem()->fileDevice, 0);
    size_t size = request.file_->size_;
    if (size != 0) {
        g_pmsData = gfl::mem::UniquePtr<u8[]>(static_cast<u8*>(resource->Allocate(size, 0x10)), resource, size, 0x10);
    }
    if (g_pmsData) {
        memcpy(g_pmsData.Get(), request.file_->data_, size);
    }
    g_noSpeciesMoves->Load(0, 0);
}

// 0x710071f540
void Finalize() {
    g_nameBuffer.Reset();
    g_personal.Reset();
    g_unkCache.Reset();
    g_growth.Reset();
    g_growthCache.Reset();
    g_unkLearnset.Reset();
    g_noSpeciesMoves.Reset();
    g_levelUpMoves.Reset();
    g_learnsetCache.Reset();
    g_evolutions.Reset();
    g_evolutionCache.Reset();
    g_megaEvolutions.Reset();
    g_megaCache.Reset();
    for (gfl::RefPtr<gfl::str::MsgData>& names : g_speciesNames) {
        names.Reset();
    }
    g_growthPersonal.Reset();
    g_megaCheck.Reset();
    g_pmsData.Reset();
    ClearMegaSource();
    ClearEvolutionSource();
    ClearGrowthSource();
    Learnset::ClearSource();
    PersonalTable::UnloadAll();
}

// ---- lookups --------------------------------------------------------------------------------------

// 0x710071fa80
void Load(u32 species, u32 form) {
    g_personal->Load(species, form);
}

// 0x710071faa0
u32 GetParam(u32 param) {
    return g_personal->GetParam(param);
}

// 0x710071fac0
bool CanLearnMachine(u16 index) {
    return g_personal->CanLearnMachine(index);
}

// 0x710071fae0
void LoadGrowthTable(u32 species, u32 form) {
    GrowthTable* table = g_growth.Get();
    g_growthPersonal->Load(species, form);
    u32 growthRate = g_growthPersonal->GetParam(kParamGrowthRate);
    if (!g_growthCache->Find(growthRate, table)) {
        table->Load(growthRate);
        g_growthCache->Add(table);
    }
}

// 0x710071fb70
u32 GetMinExp(u8 level) {
    return g_growth->GetMinExp(level < 100 ? level : 100);
}

// 0x710071fba0: species 0 gets species 0's list (read at startup)
void LoadLevelUpMoves(u32 species, u32 form) {
    Learnset* learnset = g_levelUpMoves.Get();
    if (species == 0) {
        g_noSpeciesMoves->CopyTo(learnset);
        return;
    }
    if (!g_learnsetCache->Find(species, form, learnset)) {
        learnset->Load(species, form);
        g_learnsetCache->Add(*learnset);
    }
}

// 0x710071fc30
u8 GetLevelUpMoveCount() {
    return g_levelUpMoves->GetCount();
}

// 0x710071fc40
u16 GetLevelUpMoveLevel(u16 index) {
    return g_levelUpMoves->GetLevel(index);
}

// 0x710071fc60
u16 GetLevelUpMove(u16 index) {
    return g_levelUpMoves->GetMove(index);
}

// 0x710071fc80
u32 GetLevelUpMoveKind(u16 index) {
    return g_levelUpMoves->GetKind(index);
}

// 0x710071fca0
void LoadEvolutions(u32 species, u32 form) {
    if (!g_evolutionCache->Find(species, form, g_evolutions.Get())) {
        g_evolutions->Load(species, form);
        g_evolutionCache->Add(g_evolutions.Get());
    }
}

// 0x710071fd10
u8 GetEvolutionCount() {
    return g_evolutions->GetCount();
}

// 0x710071fd20
u16 GetEvolutionMethod(u8 index) {
    return g_evolutions->GetMethod(index);
}

// 0x710071fd40
u16 GetEvolutionParam(u8 index) {
    return g_evolutions->GetParam(index);
}

// 0x710071fd60
u16 GetEvolutionSpecies(u8 index) {
    return g_evolutions->GetSpecies(index);
}

// 0x710071fd80
u8 GetEvolutionForm(u8 index) {
    return g_evolutions->GetForm(index);
}

// 0x710071fda0
bool HasEvolutionForm(u8 index) {
    return g_evolutions->HasForm(index);
}

// 0x710071fdc0
u8 GetEvolutionLevel(u8 index) {
    return g_evolutions->GetLevel(index);
}

// 0x710071fde0
void LoadMegaEvolutions(u32 species) {
    if (!g_megaCache->Find(species, g_megaEvolutions.Get())) {
        g_megaEvolutions->Load(species);
        g_megaCache->Add(g_megaEvolutions.Get());
    }
}

// 0x710071fe40
u8 GetMegaEvolutionCount() {
    return g_megaEvolutions->GetCount();
}

// 0x710071fe50
u16 GetMegaEvolutionMethod(u8 index) {
    return g_megaEvolutions->GetMethod(index);
}

// 0x710071fe70
u16 GetMegaEvolutionItem(u8 index) {
    return g_megaEvolutions->GetItem(index);
}

// 0x710071fe90
u8 GetMegaEvolutionForm(u8 index) {
    return g_megaEvolutions->GetForm(index);
}

// 0x710071feb0
bool HasMegaForm(u32 species, u8 form) {
    if (!g_megaCache->Find(species, g_megaCheck.Get())) {
        g_megaEvolutions->Load(species);
        g_megaCache->Add(g_megaEvolutions.Get());
    }
    u8 count = g_megaCheck->GetCount();
    for (u8 i = 0; i < count; i++) {
        u16 method = g_megaCheck->GetMethod(i);
        u8 megaForm = g_megaCheck->GetForm(i);
        if (method != 0 && megaForm == form) {
            return true;
        }
    }
    return false;
}

// 0x710071ff80
bool IsPrimalForm(u32 species, u8 form) {
    return (species == kSpeciesKyogre && form == 1) || (species == kSpeciesGroudon && form == 1);
}

// 0x710071ffc0
bool IsMiniorMeteorForm(u8 form) {
    return form < 7;
}

// 0x710071ffd0
u8 GetMiniorMeteorForm(u8 form) {
    return form >= 7 && form <= 13 ? kMiniorForms[form - 7][0] : form;
}

// 0x7100720000
u8 GetMiniorCoreForm(u8 form) {
    return form <= 6 ? kMiniorForms[form][1] : form;
}

}  // namespace pokemon::personal

// ---- the file's global objects: set up at startup, destroyed at exit (what the compiler writes) -----

namespace pokemon::personal {

// 0x710071e150. These destroy with Reset (the pointer cleared before it's released): our compiler
// drops a destructor's last stores to its object, and the game's left the global cleared.
void DestroyNameBuffer(gfl::RefPtr<gfl::str::StrBuf>* buffer) {
    buffer->Reset();
}

// 0x710071e190: the last language's first
void DestroySpeciesNames(void*) {
    for (s32 i = 10; i >= 0; i--) {
        g_speciesNames[i].Reset();
    }
}

// 0x710071e4f0
void DestroyUnkCache(gfl::mem::UniquePtr<UnkCache>* cache) {
    cache->Reset();
}

// 0x710071e550: both species tables'
void DestroyPersonalTable(gfl::mem::UniquePtr<PersonalTable>* table) {
    table->Reset();
}

// 0x710071e5b0
void DestroyGrowth(gfl::mem::UniquePtr<GrowthTable>* table) {
    table->Reset();
}

// 0x710071e610
void DestroyGrowthCache(gfl::mem::UniquePtr<GrowthCache>* cache) {
    cache->Reset();
}

// 0x710071e670: the three learnsets'
void DestroyLearnset(gfl::mem::UniquePtr<Learnset>* learnset) {
    learnset->Reset();
}

// 0x710071e6c0
void DestroyLearnsetCache(gfl::mem::UniquePtr<LearnsetCache>* cache) {
    cache->Reset();
}

// 0x710071e720
void DestroyEvolutions(gfl::mem::UniquePtr<EvolutionTable>* table) {
    table->Reset();
}

// 0x710071e780
void DestroyEvolutionCache(gfl::mem::UniquePtr<EvolutionCache>* cache) {
    cache->Reset();
}

// 0x710071e7e0: both Mega Evolution tables'
void DestroyMegaTable(gfl::mem::UniquePtr<MegaTable>* table) {
    table->Reset();
}

// 0x710071e840
void DestroyMegaCache(gfl::mem::UniquePtr<MegaCache>* cache) {
    cache->Reset();
}

// 0x710071e8a0
void DestroyPmsData(gfl::mem::UniquePtr<u8[]>* data) {
    data->Reset();
}

}  // namespace pokemon::personal

// The destructors above, as the game registers them
extern "C" {
void FUN_710071e150(void*);
void FUN_710071e190(void*);
void FUN_710071e4f0(void*);
void FUN_710071e550(void*);
void FUN_710071e5b0(void*);
void FUN_710071e610(void*);
void FUN_710071e670(void*);
void FUN_710071e6c0(void*);
void FUN_710071e720(void*);
void FUN_710071e780(void*);
void FUN_710071e7e0(void*);
void FUN_710071e840(void*);
void FUN_710071e8a0(void*);
}

namespace pokemon::personal {

template <class T>
static void Construct(T* object, void (*destructor)(void*)) {
    new (object) T();
    __cxa_atexit(destructor, object, &__dso_handle);
}

// 0x710071e2c0
void InitializeGlobals() {
    Construct(&g_nameBuffer, FUN_710071e150);
    for (gfl::RefPtr<gfl::str::MsgData>& names : g_speciesNames) {
        new (&names) gfl::RefPtr<gfl::str::MsgData>();
    }
    __cxa_atexit(FUN_710071e190, nullptr, &__dso_handle);
    Construct(&g_unkCache, FUN_710071e4f0);
    Construct(&g_personal, FUN_710071e550);
    Construct(&g_growthPersonal, FUN_710071e550);
    Construct(&g_growth, FUN_710071e5b0);
    Construct(&g_growthCache, FUN_710071e610);
    Construct(&g_levelUpMoves, FUN_710071e670);
    Construct(&g_noSpeciesMoves, FUN_710071e670);
    Construct(&g_unkLearnset, FUN_710071e670);
    Construct(&g_learnsetCache, FUN_710071e6c0);
    Construct(&g_evolutions, FUN_710071e720);
    Construct(&g_evolutionCache, FUN_710071e780);
    Construct(&g_megaCheck, FUN_710071e7e0);
    Construct(&g_megaEvolutions, FUN_710071e7e0);
    Construct(&g_megaCache, FUN_710071e840);
    Construct(&g_pmsData, FUN_710071e8a0);
}

}  // namespace pokemon::personal
