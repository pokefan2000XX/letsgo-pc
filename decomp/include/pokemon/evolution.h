// Evolution and Mega Evolution rules: which species a Pokémon evolves into by leveling up, by an
// item or by trading, checked against the species' evolution list (personal data), and which Mega
// form it can take. The checker is a reference-counted gflib3 object the Pokémon system holds; the
// game may derive its own (BlocksEvolution is virtual).
#pragma once
#include "decomp.h"
#include "gfl/object.h"
#include "pokemon/pokemon.h"

namespace pokemon {

class Party;

// How a species evolves (the evolution list's methods)
enum EvolutionMethod : u32 {
    kEvolveFriendship = 1,
    kEvolveFriendshipDay,
    kEvolveFriendshipNight,
    kEvolveLevel,
    kEvolveTrade,
    kEvolveTradeHeldItem,
    kEvolveTradeSpecies,            // Shelmet and Karrablast for each other
    kEvolveItem,
    kEvolveAttackHigher,            // Attack above Defense
    kEvolveAttackEqual,
    kEvolveDefenseHigher,
    kEvolveKeyLow,                  // by the encryption key: (key >> 16) % 10 below 5
    kEvolveKeyHigh,
    kEvolveNinjask,
    kEvolveShedinja,
    kEvolveBeauty,
    kEvolveItemMale,
    kEvolveItemFemale,
    kEvolveHeldItemDay,
    kEvolveHeldItemNight,
    kEvolveKnowsMove,
    kEvolveWithPartySpecies,
    kEvolveLevelMale,
    kEvolveLevelFemale,
    kEvolveElectricField,
    kEvolveForestField,
    kEvolveColdField,
    kEvolveUpsideDown,
    kEvolveFriendshipMoveType,      // friendship 50+ and a move of the parameter's type
    kEvolveDarkTypeInParty,
    kEvolveWeather,
    kEvolveDay,
    kEvolveNight,
    kEvolveLevelFemaleForm1,        // into the next form
    kEvolveFriendship50,
    kEvolveVersion,
    kEvolveVersionDay,
    kEvolveVersionNight,
    kEvolveSummit,
    kEvolveDusk,
    kEvolveUltraSpace,
    kEvolveItemUltraSpace,
};

// Where and when an evolution is checked (each flag nonzero when it holds)
struct EvolutionContext {
    u8 electricField;
    u8 coldField;
    u8 forestField;
    u8 summit;
    u8 ultraSpace;
    u8 day;
    u8 night;
    u8 dusk;
    u8 upsideDown;                  // the console held upside down
    u8 weather;
    u8 unkA[2];
    TrainerInfo trainer;            // who is evolving it
};
DECOMP_OFFSET(EvolutionContext, trainer, 0xC);

// One entry of a species' evolution list, as the checks read it
struct EvolutionEntry {
    u32 method;
    u16 param;                      // an item, move, species, type or version, by method
    u32 species;
    u8 level;                       // the lowest level it happens at
};
DECOMP_OFFSET(EvolutionEntry, species, 8);
DECOMP_OFFSET(EvolutionEntry, level, 0xC);

class EvolutionChecker : public gfl::RefObject {
public:
    EvolutionChecker();
    ~EvolutionChecker() override;

    bool HasEvolutions(Pokemon* pokemon);
    // The species it evolves into (its own when none); `index` gets the list entry when there's one
    u16 GetLevelUpEvolution(Pokemon* pokemon, const Party* party, const EvolutionContext* context, u32* index);
    u16 GetItemEvolution(Pokemon* pokemon, const EvolutionContext* context, u16 item, u32* index);
    u16 GetTradeEvolution(Pokemon* pokemon, Pokemon* partner, u32* index);
    bool CheckLevelUpEvolution(Pokemon* pokemon, const Party* party, const EvolutionContext* context,
                               const EvolutionEntry* entry);
    bool CheckItemEvolution(Pokemon* pokemon, const EvolutionContext* context, u16 item, const EvolutionEntry* entry);
    bool CheckTradeEvolution(Pokemon* pokemon, Pokemon* partner, const EvolutionEntry* entry);

    // Mega Evolution: by a held Mega Stone, by knowing Dragon Ascent (Rayquaza, unless it holds a
    // Z-Crystal), or always. The form it can take, or its own form when none.
    u8 GetMegaForm(Pokemon* pokemon);
    bool CanMegaEvolveTo(Pokemon* pokemon, u8 form);
    bool IsMegaStone(u32 species, u16 item);  // the species' Mega Stone

    // Whether a held item stops evolution; none does here
    virtual bool BlocksEvolution(u16 item);
};

}  // namespace pokemon
