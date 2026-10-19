// A Pokémon: its record (core data, plus party stats for party Pokémon) and the accessor over it,
// in a reference-counted gflib3 object. Most of its methods read or change the record through the
// accessor and keep the party stats (level, stats, HP, CP) in step with what they depend on.
#pragma once
#include "decomp.h"
#include "gfl/mem.h"
#include "gfl/object.h"
#include "gfl/str.h"
#include "pokemon/accessor.h"
#include "pokemon/ids.h"
#include "pokemon/record.h"

namespace pokemon {

class Party;
struct EvolutionContext;

enum Stat : u32 { kStatHp, kStatAttack, kStatDefense, kStatSpAttack, kStatSpDefense, kStatSpeed };

// Kyurem's form-specific moves, each form's version: [0] Scary Face, Fusion Flare, Fusion Bolt;
// [1] Glaciate, Ice Burn, Freeze Shock
extern const u32 kKyuremMoves[2][3];

// What happened when a Pokémon was taught a move
enum LearnResult : u32 {
    kLearnLearned,          // into an empty slot
    kLearnMovesFull,        // all four slots are taken
    kLearnAlreadyKnown,
    kLearnNoMove,           // no (further) move to learn
};

// Which eggs IsEgg counts
enum EggCheck : u32 {
    kEggValid,              // an egg that isn't a bad egg
    kEggBad,                // a bad egg (a record whose checksum fails)
    kEggAny,
};

// A trainer as Pokémon records them: the ID, gender and name (more fields follow)
struct TrainerInfo {
    u32 id;
    u8 gender;
    u8 unk5;
    u16 name[13];
};
DECOMP_OFFSET(TrainerInfo, name, 6);

// Moves already offered while learning level-up moves, so each is offered once
struct MoveList {
    void Clear();
    bool Contains(u32 move);
    void AddIfNew(u32 move) {
        if (!Contains(move) && count < 55) {
            moves[count++] = move;
        }
    }

    u32 moves[55];
    u32 count;
};
DECOMP_SIZE(MoveList, 0xE0);

// Records are copied through these: the whole record, or just its core data
extern u8 g_scratchRecord[kRecordSize];
extern CoreData g_scratchCore;

class Pokemon : public gfl::RefObject {
public:
    // Made in a resource: blank (a zeroed record), new (a species at a level for a trainer ID, or
    // from a spec, with the rest rolled at random), or a copy of another's core data
    explicit Pokemon(gfl::mem::MemoryResource* resource);
    Pokemon(gfl::mem::MemoryResource* resource, u32 species, u16 level, u64 id);
    Pokemon(gfl::mem::MemoryResource* resource, const CreationSpec& spec);
    Pokemon(gfl::mem::MemoryResource* resource, const Pokemon& other);
    ~Pokemon() override;

    // Copies the core data in (party Pokémon then recalculate their stats and are healed)
    virtual void DeserializeCore(const CoreData* core);

    static void Initialize();             // when the Pokémon system starts up (empty)
    // Turns it into an egg: named "Egg", with its species' hatch cycles as its friendship. The name
    // is made in `resource`.
    void MakeEgg(gfl::mem::MemoryResource* resource);

    // The record's fields, as the accessor reads and writes them
    void SetLanguage(u8 language);
    void SetUnkAC(u8 value);
    void SetUnkAD(u8 value);
    void SerializeCore(CoreData* core) const;
    void Clear();
    u16 GetSpecies() const;
    void SetHeldItem(u16 item);
    u8 GetLanguage();
    u8 GetVersion();
    void SetVersion(u8 version);
    u8 GetBall();
    void SetBall(u8 ball);
    u8 GetUnkAE();
    u16 GetCp();
    u32 GetExp();
    u8 GetForm();
    u32 GetStatus();
    void SetStatus(u32 status);
    void ClearStatus();
    u16 GetMove(u8 slot);
    u16 GetRelearnMove(u8 slot);
    u8 GetGender();
    u8 GetNature();
    u8 GetAbility();
    void SetFavorite(u8 flag);
    bool IsFavorite();
    bool IsNicknamed();
    void GetNickname(gfl::str::StrBuf* out);
    void GetNickname(u16* out, u32 size);
    void GetOtName(gfl::str::StrBuf* out);
    void GetOtName(u16* out, u32 size);
    u8 GetOtGender();
    void SetOtGender(u8 gender);
    u8 GetFriendship();
    u32 GetUnk94();
    void SetUnk94(u32 value);
    void MarkBadEgg();
    u32 GetId();
    bool GetUnk100();
    void SetId(u32 id);
    u32 GetEncryptionKey();
    bool HasRibbon(u32 ribbon);
    void SetRibbon(u32 ribbon);
    bool IsFatefulEncounter();
    void SetFatefulEncounter(u8 flag);
    f32 GetHeightAbsolute();
    f32 GetWeightAbsolute();
    u8 GetHeightScalar();
    u8 GetWeightScalar();
    u8 GetUnkAC();
    u8 GetUnkAD();
    u8 GetUnkCB();
    void SetUnkCB(u8 value);
    u8 GetUnkCC();
    void SetUnkCC(u8 value);
    u8 GetUnkCD();
    void SetUnkCD(u8 value);
    u8 GetUnkCE();
    void SetUnkCE(u8 value);
    u8 GetUnkCF();
    void SetUnkCF(u8 value);
    u8 GetUnkD0();
    void SetUnkD0(u8 value);
    void Serialize(void* record) const;
    void Deserialize(const void* record);
    u16 GetHeldItem();

    bool IsEmpty() const;                       // no species
    // Markings: six marks, each 0 (none) to 2
    bool HasMarking(s32 mark);
    u16 GetMarking(s32 mark);
    void SetMarking(s32 mark, s32 color);
    void ClearMarking(s32 mark);
    void SetAllMarkings(const s32* marks);
    void GetAllMarkings(s32* marks);
    void AddUnkAE(u8 amount);             // capped at 255
    void SetUnk3CLowByte(u8 value);
    void SetUnk3CHighByte(u8 value);
    // Where and when it was met or hatched, by field (0-9, 14-17)
    u32 GetMetInfo(u32 field);
    void SetMetInfo(u32 field, u32 value);

    // Individual values (Stat order); Hyper Training counts as 31
    u8 GetIv(u32 stat);
    u8 GetEffectiveIv(s32 stat);
    bool IsHyperTrained(s32 stat);
    void SetIv(u32 stat, u32 value);
    u8 GetHiddenPowerType();
    // Effort values: 252 per stat, 510 in all
    u8 GetEv(u32 stat);
    u32 GetEvTotal();
    void SetEv(u32 stat, u32 value);
    void AddEv(u32 stat, u32 amount);
    void SubtractEv(u32 stat, u32 amount);
    u32 ClampEv(u32 current, u32 value);
    // Awakening values: 200 per stat
    u8 GetAv(u32 stat);
    void SetAv(u32 stat, u32 value);
    void AddAv(u32 stat, u8 amount);
    void SetUnkD7(u8 value);              // capped at 100; recalculates the stats
    void SetHyperTrained(s32 stat);
    static s8 GetNatureModifier(u16 nature, u32 stat);
    u32 GetNatureFlavor(u32 flavor);

    // Stats: the party stats when there are any, otherwise calculated
    u16 GetStat(u32 stat);
    u16 GetMaxHp();
    u16 GetHp();
    u16 CalcMaxHp();
    u16 CalcAttack();
    u16 CalcDefense();
    u16 CalcSpAttack();
    u16 CalcSpDefense();
    u16 CalcSpeed();
    // Max HP from the stored data; HP moves with it (unless keepFainted and it's 0)
    void UpdateMaxHp(bool keepFainted);
    void UpdateCp();
    void RecalcStats(bool keepFainted);   // level, every stat and CP
    void SetHp(u32 hp);                   // up to max HP
    void RecoverHp(u32 amount);
    bool IsFullHp();
    bool IsFainted() const;
    bool HasStatus();

    // Moves: four slots, each with its PP and PP Ups
    void SetMove(u8 slot, u32 move);      // with full PP and no PP Ups
    void ClearMove(u8 slot);
    bool HasMove(u32 move);
    u8 GetMoveCount();
    void SwapMoves(u8 slot1, u8 slot2);
    void CompactMoves();                  // moves to the front, empty slots to the back
    LearnResult LearnMove(u32 move);
    void PushMove(u32 move);              // learns it, forgetting the first move when full
    void ResetMoves();                    // the last four level-up moves up to its level
    // Level-up moves for its current level, one per call: `*index` counts the ones taken so far.
    // The move goes in `*move`; moves in `seen` (optional) are skipped, and the move is added.
    LearnResult LearnLevelUpMove(u32* index, u32* move, MoveList* seen);
    // The same for evolution moves; `*index` is where to continue in the species' list
    LearnResult LearnEvolutionMove(u32* index, u32* move, MoveList* seen);
    void UpdateKyuremMoves(u8 form);      // its fusion moves follow its form
    bool CanLearnMachine(u32 machine);
    void SetRelearnMove(u8 slot, u16 move);
    u8 GetMovePp(u8 slot);
    void SetMovePp(u8 slot, u8 pp);       // up to the move's max PP
    u8 GetMaxPp(u8 slot);
    void RecoverPp(u8 slot, u8 amount);
    void RecoverAllPp();
    u8 GetMovePpUps(u8 slot);
    void SetMovePpUps(u8 slot, u8 ppUps); // 3 at most

    void Heal();                          // HP, status and PP

    // Names and trainers
    void SetNickname(const gfl::str::StrBuf* name);   // a species name isn't counted as a nickname
    void ClearNickname();                 // back to the species name
    bool IsNicknameSpeciesName();         // in the game's language
    void SetOtName(const gfl::str::StrBuf* name);
    bool IsOriginalTrainer(const TrainerInfo* trainer);
    // Hands it to a trainer: back to its original trainer, or to a new one with the species' base
    // friendship. True when the trainer is its original trainer.
    bool SetCurrentTrainer(const TrainerInfo* trainer);
    // Friendship (255 at most); the current trainer's also changes the stats
    void SetFriendship(u32 friendship);
    void AddFriendship(u32 amount);
    void SubtractFriendship(u32 amount);
    void SetOtFriendship(u32 friendship);
    u8 GetHandlerFriendship();
    void SetHandlerFriendship(u32 friendship);

    bool IsEgg(EggCheck check) const;
    bool IsShiny();
    bool HasPokerus();
    // Fast mode keeps the record decrypted across many accesses. Start returns whether it started
    // it; End ends it only when told this caller started it, and returns whether it did.
    bool StartFastMode();
    bool EndFastMode(bool started);

    // Evolution: whether its species has any, and the species it would evolve into now (true when
    // that's another species; `index` gets the evolution list entry)
    bool CanEvolve();
    bool GetLevelUpEvolution(const EvolutionContext* context, const Party* party, u32* species, u32* index);
    bool GetItemEvolution(const EvolutionContext* context, u32 item, u32* species, u32* index);
    bool GetTradeEvolution(Pokemon* partner, u32* species, u32* index);
    void Evolve(u32 species, u8 index);   // by the evolution list's entry `index`
    // A new species or form: ability (same slot), gender and stats follow
    void ChangeSpecies(u32 species, u32 form);
    void ChangeForm(u32 form);
    bool FixGenderForForm();
    // Mega Evolution. Unk100 is set while Mega Evolved, and Unk101 holds the form to go back to.
    void MegaEvolve();
    bool CanMegaEvolve();
    void MegaEvolveTo(u8 form);
    void RevertMegaEvolution();
    void ClearMegaEvolution();
    u8 GetType1() const;
    u8 GetType2() const;

    // Size: the scalars (0-255) and the absolute height and weight they give for the species
    u32 GetHeightClass();                 // 0 (XS) to 4 (XL)
    void UpdateAbsoluteSize();            // for the species data loaded last
    void SetAbsoluteSize(f32 height, f32 weight);

    // Level and EXP (level 100 at most)
    u8 GetLevel();
    void AddLevels(u8 levels);
    void SetExp(u32 exp);
    void AddExp(u32 exp);
    u32 GetLevelExp();                    // EXP at the start of the current level
    u32 GetNextLevelExp();

    // The layout
    gfl::mem::UniquePtr<CoreData> core_;
    gfl::mem::UniquePtr<CalcData> calc_;  // party Pokémon only
    gfl::mem::UniquePtr<Accessor> accessor_;

private:
    // A new Pokémon's remaining state: the game's language and the console's region, its level-up
    // moves, its species name, full HP and PP, and its absolute size
    void InitNewPokemon();
};
DECOMP_OFFSET(Pokemon, core_, 0x58);
DECOMP_OFFSET(Pokemon, calc_, 0x78);
DECOMP_OFFSET(Pokemon, accessor_, 0x98);
DECOMP_SIZE(Pokemon, 0xB8);

// A Pokémon in a party (or one that can be put in one): it has party stats, kept calculated
class PartyPokemon : public Pokemon {
public:
    // As Pokémon's constructors, with the party stats calculated and full HP and PP; the copy
    // copies the whole record, party stats included
    explicit PartyPokemon(gfl::mem::MemoryResource* resource);
    PartyPokemon(gfl::mem::MemoryResource* resource, u32 species, u16 level, u64 id);
    PartyPokemon(gfl::mem::MemoryResource* resource, const CreationSpec& spec);
    PartyPokemon(gfl::mem::MemoryResource* resource, const Pokemon& other);
    ~PartyPokemon() override;

    void DeserializeCore(const CoreData* core) override;

    static void Initialize();             // when the Pokémon system starts up (empty)
    void SerializeCore(CoreData* core) const;
    void CopyFrom(const Pokemon& other);  // the whole record

private:
    // New party stats for the record (zeroed, encrypted with its key)
    void AddCalcData(gfl::mem::MemoryResource* resource);
};
DECOMP_SIZE(PartyPokemon, 0xB8);

}  // namespace pokemon
