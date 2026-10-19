// Species data ("personal" data): each species' and form's base stats, types, abilities, gender
// ratio, growth rate and more (one 0x54-byte entry each, PKHeX's PersonalInfoGG, all of them in
// personal_total.bin), level-up moves (a file per entry), evolutions, Mega Evolutions and EXP tables.
// Lookups go through global tables that hold the species and form loaded last; caches keep recent
// learnsets, evolutions and Mega Evolutions. The evolution, Mega Evolution and EXP tables aren't
// decompiled yet (they're outside D1): those link to the originals.
#pragma once
#include "decomp.h"
#include "gfl/fs.h"
#include "gfl/mem.h"
#include "gfl/object.h"
#include "gfl/str.h"

namespace pokemon::personal {

enum Param : u32 {
    kParamBaseHp = 0x00,
    kParamBaseAttack = 0x01,
    kParamBaseDefense = 0x02,
    kParamBaseSpeed = 0x03,
    kParamBaseSpAttack = 0x04,
    kParamBaseSpDefense = 0x05,
    kParamType1 = 0x06,
    kParamType2 = 0x07,
    kParamCatchRate = 0x08,
    kParamBaseExp = 0x09,
    kParamEvYieldHp = 0x0A,     // 0x0A-0x0F: EV yields (HP, Atk, Def, Spe, SpA, SpD)
    kParamUnk10 = 0x10,
    kParamHeldItem1 = 0x11,     // 0x11-0x13: wild held items
    kParamGenderRatio = 0x14,   // 0: male only, 0xFE: female only, 0xFF: genderless
    kParamHatchCycles = 0x15,
    kParamBaseFriendship = 0x16,
    kParamGrowthRate = 0x17,
    kParamEggGroup1 = 0x18,
    kParamEggGroup2 = 0x19,
    kParamAbility1 = 0x1A,
    kParamAbility2 = 0x1B,
    kParamAbilityHidden = 0x1C,
    kParamFormStatsIndex = 0x1D,
    kParamFormSprite = 0x1E,
    kParamFormCount = 0x1F,
    kParamColor = 0x20,
    kParamEvolutionStage = 0x21,
    kParamHeight = 0x22,        // the species' average height and weight
    kParamWeight = 0x23,
    kParamMachines = 0x24,      // 0x24-0x2B: which machines it learns, 32 per parameter
    kParamUnk2D = 0x2D,         // 0x2C reads as 0
    kParamUnk2E = 0x2E,
    kParamUnk2F = 0x2F,
    kParamUnk30 = 0x30,
    kParamUnk31 = 0x31,
};

// One species' or form's entry
struct PersonalInfo {
    u8 baseStats[6];            // HP, Atk, Def, Spe, SpA, SpD
    u8 types[2];
    u8 catchRate;
    u8 evolutionStage;
    u16 evYield;                // 2 bits per stat in the order above, then one more bit
    u16 heldItems[3];
    u8 genderRatio;
    u8 hatchCycles;
    u8 baseFriendship;
    u8 growthRate;
    u8 eggGroups[2];
    u8 abilities[3];
    u8 unk1B;
    u16 formStatsIndex;         // the entry of its form 1 (forms follow it), or 0 when it has none
    u16 formSprite;
    u8 formCount;
    u8 color;                   // in the low 6 bits
    u16 baseExp;
    u16 height;
    u16 weight;
    u32 machines[8];
    u32 unk48;
    u16 unk4C;
    u16 unk4E;
    u16 unk50;
    u8 unk52;
    u8 unk53;
};
DECOMP_SIZE(PersonalInfo, 0x54);

// The entries (personal_total.bin), loaded once for everything
extern void* g_personalSource;            // what LoadAll was given; null until it's loaded
extern gfl::mem::UniquePtr<PersonalInfo[]> g_personalData;

// A copy of one species' (or form's) entry
class PersonalTable {
public:
    explicit PersonalTable(gfl::mem::MemoryResource* resource);
    virtual ~PersonalTable();

    // The entries, once; `source` marks them loaded
    static void LoadAll(gfl::mem::MemoryResource* resource, void* source);
    static void UnloadAll();
    // The entry of a species' form (its own for form 0 or a form it doesn't have; 0 past 809)
    static u32 GetEntryIndex(s32 species, u8 form);

    void Load(s32 species, u8 form);
    u32 GetParam(u32 param) const;
    bool CanLearnMachine(u32 machine) const;
    u32 GetSpecies() const;
    u8 GetForm() const;

    // The layout
    u32 species_;
    u8 form_;
    gfl::mem::UniquePtr<PersonalInfo> info_;
};
DECOMP_SIZE(PersonalTable, 0x30);

// A level-up move: {-1, -1} ends a list (of 55 at most)
struct LevelUpMove {
    u16 move;
    u16 level;                  // 0: learned on evolving
};
constexpr u32 kMaxLevelUpMoves = 55;

// One species' (or form's) level-up moves
class Learnset {
public:
    explicit Learnset(gfl::mem::MemoryResource* resource);
    ~Learnset();

    // Where learnsets are read from (set once)
    static void SetSource(gfl::mem::MemoryResource* resource, void* source);
    static void ClearSource();

    u32 GetSpecies() const;
    u8 GetForm() const;
    void Load(s32 species, u8 form);      // nothing to do when it's the one loaded
    // Reading in the background: StartLoad is false when there's nothing to read, FinishLoad
    // false until it's read
    bool StartLoad(s32 species, u8 form);
    bool FinishLoad();
    void CopyTo(Learnset* other) const;
    u8 GetCount() const;
    u32 GetKind(u16 index) const;         // 0: on evolving, 1: at level 1, 2: later (or past the end)
    u16 GetLevel(u16 index) const;        // past the end: the first one's
    u16 GetMove(u16 index) const;

    // The layout
    u32 species_;
    u8 form_;
    gfl::mem::UniquePtr<LevelUpMove[]> moves_;
    u8 count_;
};
DECOMP_SIZE(Learnset, 0x30);

extern void* g_learnsetSource;

// The learnsets read last, reused before reading again (oldest replaced first)
class LearnsetCache {
public:
    LearnsetCache(gfl::mem::MemoryResource* resource, u32 capacity);
    virtual ~LearnsetCache();

    bool Find(s32 species, u8 form, Learnset* out);   // copies it out when it's there
    void Add(const Learnset& learnset);                // unless it's there already

    // The layout
    gfl::mem::UniquePtr<Learnset[]> entries_;
    u32 capacity_;
    u32 next_;
};
DECOMP_SIZE(LearnsetCache, 0x30);

// Tables outside D1 (not decompiled yet): only what's used here is declared
class GrowthTable {                       // a growth rate's EXP per level
public:
    GrowthTable();
    virtual ~GrowthTable();
    void Load(u32 growthRate);
    u32 GetMinExp(u8 level) const;
    u8 unk8[0x198];
};
DECOMP_SIZE(GrowthTable, 0x1A0);

class GrowthCache {
public:
    GrowthCache(gfl::mem::MemoryResource* resource, u32 capacity);
    virtual ~GrowthCache();
    bool Find(u32 growthRate, GrowthTable* out);
    void Add(GrowthTable* table);
    u8 unk8[0x28];
};
DECOMP_SIZE(GrowthCache, 0x30);

class EvolutionTable {
public:
    explicit EvolutionTable(gfl::mem::MemoryResource* resource);
    virtual ~EvolutionTable();
    void Load(u32 species, u32 form);
    u8 GetCount() const;
    u16 GetMethod(u8 index) const;
    u16 GetParam(u8 index) const;
    u16 GetSpecies(u8 index) const;
    u8 GetForm(u8 index) const;
    bool HasForm(u8 index) const;
    u8 GetLevel(u8 index) const;
    u8 unk8[0x28];
};
DECOMP_SIZE(EvolutionTable, 0x30);

class EvolutionCache {
public:
    EvolutionCache(gfl::mem::MemoryResource* resource, u32 capacity);
    virtual ~EvolutionCache();
    bool Find(u32 species, u32 form, EvolutionTable* out);
    void Add(EvolutionTable* table);
    u8 unk8[0x28];
};
DECOMP_SIZE(EvolutionCache, 0x30);

class MegaTable {
public:
    explicit MegaTable(gfl::mem::MemoryResource* resource);
    virtual ~MegaTable();
    void Load(u32 species);
    u8 GetCount() const;
    u16 GetMethod(u8 index) const;
    u16 GetItem(u8 index) const;
    u8 GetForm(u8 index) const;
    u8 unk8[0x28];
};
DECOMP_SIZE(MegaTable, 0x30);

class MegaCache {
public:
    MegaCache(gfl::mem::MemoryResource* resource, u32 capacity);
    virtual ~MegaCache();
    bool Find(u32 species, MegaTable* out);
    void Add(MegaTable* table);
    u8 unk8[0x28];
};
DECOMP_SIZE(MegaCache, 0x30);

class UnkCache {                          // made with the others, used elsewhere
public:
    UnkCache(gfl::mem::MemoryResource* resource, u32 capacity);
    virtual ~UnkCache();
    u8 unk8[0x28];
};
DECOMP_SIZE(UnkCache, 0x30);

// Where the growth, evolution and Mega Evolution tables are read from, and forgetting it
void SetGrowthSource(gfl::mem::MemoryResource* resource, void* source);
void SetEvolutionSource(gfl::mem::MemoryResource* resource, void* source);
void SetMegaSource(gfl::mem::MemoryResource* resource, void* source);
void ClearGrowthSource();
void ClearEvolutionSource();
void ClearMegaSource();

// The tables (made by Initialize)
extern gfl::RefPtr<gfl::str::StrBuf> g_nameBuffer;
extern gfl::mem::UniquePtr<UnkCache> g_unkCache;
extern gfl::mem::UniquePtr<PersonalTable> g_personal;
extern gfl::mem::UniquePtr<PersonalTable> g_growthPersonal;   // for the growth rate
extern gfl::mem::UniquePtr<GrowthTable> g_growth;
extern gfl::mem::UniquePtr<GrowthCache> g_growthCache;
extern gfl::mem::UniquePtr<Learnset> g_levelUpMoves;
extern gfl::mem::UniquePtr<Learnset> g_noSpeciesMoves;       // species 0's, copied for species 0
extern gfl::mem::UniquePtr<Learnset> g_unkLearnset;
extern gfl::mem::UniquePtr<LearnsetCache> g_learnsetCache;
extern gfl::mem::UniquePtr<EvolutionTable> g_evolutions;
extern gfl::mem::UniquePtr<EvolutionCache> g_evolutionCache;
extern gfl::mem::UniquePtr<MegaTable> g_megaCheck;            // what HasMegaForm reads
extern gfl::mem::UniquePtr<MegaTable> g_megaEvolutions;
extern gfl::mem::UniquePtr<MegaCache> g_megaCache;
extern gfl::mem::UniquePtr<u8[]> g_pmsData;                  // pms_total.bin, for other code
extern gfl::RefPtr<gfl::str::MsgData> g_speciesNames[11];      // by language

// Makes the tables in `resource` (the system says where from and how big the caches are), and
// destroys them
void Initialize(gfl::mem::MemoryResource* resource);
void Finalize();

void Load(u32 species, u32 form);
u32 GetParam(u32 param);
// The EXP table of the species' growth rate
void LoadGrowthTable(u32 species, u32 form);
u32 GetMinExp(u8 level);                  // up to level 100
// Technical machines: whether the species loaded last can learn machine `index`
bool CanLearnMachine(u16 index);

// Level-up moves of a species, in level order: entries of a move and the level it's learned at
// (0: when evolving into the species)
void LoadLevelUpMoves(u32 species, u32 form);
u8 GetLevelUpMoveCount();
u16 GetLevelUpMoveLevel(u16 index);
u16 GetLevelUpMove(u16 index);
// 0: learned on evolving, 1: at level 1, 2: at a later level (or past the end)
u32 GetLevelUpMoveKind(u16 index);

// Evolutions of a species (up to 8 entries)
void LoadEvolutions(u32 species, u32 form);
u8 GetEvolutionCount();
u16 GetEvolutionMethod(u8 index);
u16 GetEvolutionParam(u8 index);
u16 GetEvolutionSpecies(u8 index);
bool HasEvolutionForm(u8 index);        // otherwise the form stays
u8 GetEvolutionForm(u8 index);
u8 GetEvolutionLevel(u8 index);

// Mega Evolutions of a species (up to 2): the form, and how (1: held item, 2: knowing Dragon
// Ascent, 3: always)
void LoadMegaEvolutions(u32 species);
u8 GetMegaEvolutionCount();
u8 GetMegaEvolutionForm(u8 index);
u16 GetMegaEvolutionMethod(u8 index);
u16 GetMegaEvolutionItem(u8 index);
// Whether one of a species' Mega Evolutions is to `form`. A species not in the cache is loaded
// into the Mega Evolution table, but g_megaCheck is what's read (the game does this).
bool HasMegaForm(u32 species, u8 form);

bool IsPrimalForm(u32 species, u8 form);  // Primal Kyogre and Primal Groudon
// Minior: forms 0-6 are its meteor forms, 7-13 its cores (the same colors in the same order)
extern const u8 kMiniorForms[7][2];       // {meteor, core}
bool IsMiniorMeteorForm(u8 form);
u8 GetMiniorMeteorForm(u8 form);          // a core's meteor form; anything else stays
u8 GetMiniorCoreForm(u8 form);            // a meteor form's core; anything else stays

}  // namespace pokemon::personal

namespace pokemon {

// A species' name in a language (1-10); false when either is out of range (the u16 version still
// copies the name buffer)
bool GetSpeciesName(gfl::str::StrBuf* out, u32 species, u32 language);
bool GetSpeciesName(u16* out, u32 species, u32 language);

}  // namespace pokemon
