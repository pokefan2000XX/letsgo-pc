// EvolutionChecker: evolution and Mega Evolution rules (pokemon/evolution.h).
#include "pokemon/evolution.h"

#include "item/item.h"
#include "moves/data.h"
#include "pokemon/party.h"
#include "pokemon/personal.h"
#include "pokemon/system.h"

namespace pokemon {

// 0x7100722960
EvolutionChecker::EvolutionChecker() {}

// 0x71007229f0, deleting: 0x7100722a10
EvolutionChecker::~EvolutionChecker() {}

// 0x7100722a30
bool EvolutionChecker::HasEvolutions(Pokemon* pokemon) {
    u16 species = pokemon->GetSpecies();
    personal::LoadEvolutions(species, pokemon->GetForm());
    return personal::GetEvolutionCount() != 0;
}

// 0x7100722a80
u16 EvolutionChecker::GetLevelUpEvolution(Pokemon* pokemon, const Party* party, const EvolutionContext* context,
                                          u32* index) {
    u16 species = pokemon->GetSpecies();
    u8 form = pokemon->GetForm();
    if (BlocksEvolution(pokemon->GetHeldItem())) {
        return species;
    }
    personal::LoadEvolutions(species, form);
    u8 count = personal::GetEvolutionCount();
    for (u32 i = 0; i < count; i++) {
        EvolutionEntry entry;
        entry.method = personal::GetEvolutionMethod(i);
        entry.param = personal::GetEvolutionParam(i);
        entry.species = personal::GetEvolutionSpecies(i);
        entry.level = personal::GetEvolutionLevel(i);
        if (CheckLevelUpEvolution(pokemon, party, context, &entry)) {
            if (index != nullptr) {
                *index = i;
            }
            return entry.species;
        }
    }
    return species;
}

static u8 GetCurrentFriendship(Pokemon* pokemon, const EvolutionContext* context) {
    if (pokemon->IsOriginalTrainer(&context->trainer)) {
        return pokemon->GetFriendship();
    }
    return pokemon->GetHandlerFriendship();
}

// 0x7100722b90
bool EvolutionChecker::CheckLevelUpEvolution(Pokemon* pokemon, const Party* party, const EvolutionContext* context,
                                             const EvolutionEntry* entry) {
    if (pokemon->GetLevel() < entry->level) {
        return false;
    }
    u8 friendship = GetCurrentFriendship(pokemon, context);
    switch (entry->method) {
    case kEvolveFriendship:
        return friendship >= 220;
    case kEvolveFriendshipDay:
        return friendship >= 220 && context->day != 0;
    case kEvolveFriendshipNight:
        return friendship >= 220 && context->night != 0;
    case kEvolveLevel:
    case kEvolveNinjask:
        return true;
    case kEvolveAttackHigher: {
        u16 attack = pokemon->GetStat(kStatAttack);
        return pokemon->GetStat(kStatDefense) < attack;
    }
    case kEvolveAttackEqual: {
        u16 attack = pokemon->GetStat(kStatAttack);
        return pokemon->GetStat(kStatDefense) == attack;
    }
    case kEvolveDefenseHigher: {
        u16 attack = pokemon->GetStat(kStatAttack);
        return attack < pokemon->GetStat(kStatDefense);
    }
    case kEvolveKeyLow:
        return (pokemon->GetEncryptionKey() >> 16) % 10 < 5;
    case kEvolveKeyHigh:
        return (pokemon->GetEncryptionKey() >> 16) % 10 > 4;
    case kEvolveHeldItemDay:
        return pokemon->GetHeldItem() == entry->param && context->day != 0;
    case kEvolveHeldItemNight:
        return pokemon->GetHeldItem() == entry->param && context->night != 0;
    case kEvolveKnowsMove:
        for (u8 slot = 0; slot < 4; slot++) {
            if (pokemon->GetMove(slot) == entry->param) {
                return true;
            }
        }
        return false;
    case kEvolveWithPartySpecies:
        return party->HasSpecies(entry->param);
    case kEvolveLevelMale:
        return pokemon->GetGender() == 0;
    case kEvolveLevelFemale:
    case kEvolveLevelFemaleForm1:
        return pokemon->GetGender() == 1;
    case kEvolveElectricField:
        return context->electricField != 0;
    case kEvolveForestField:
        return context->forestField != 0;
    case kEvolveColdField:
        return context->coldField != 0;
    case kEvolveUpsideDown:
        return context->upsideDown != 0;
    case kEvolveFriendshipMoveType:
        if (GetCurrentFriendship(pokemon, context) < 50) {
            return false;
        }
        for (u8 slot = 0; slot < 4; slot++) {
            if (moves::GetType(pokemon->GetMove(slot)) == entry->param) {
                return true;
            }
        }
        return false;
    case kEvolveDarkTypeInParty: {
        u8 count = party->GetMemberCount();
        for (u32 i = 0; i < count; i++) {
            const Pokemon* member = party->GetMember(i);
            if (member->IsEgg(kEggAny)) {
                continue;
            }
            if (member->GetType1() == kTypeDark || member->GetType2() == kTypeDark) {
                return true;
            }
        }
        return false;
    }
    case kEvolveWeather:
        return context->weather != 0;
    case kEvolveDay:
        return context->day != 0;
    case kEvolveNight:
        return context->night != 0;
    case kEvolveFriendship50:
        return GetCurrentFriendship(pokemon, context) >= 50;
    case kEvolveVersion:
        return GetSystem()->version == entry->param;
    case kEvolveVersionDay:
        return GetSystem()->version == entry->param && context->day != 0;
    case kEvolveVersionNight:
        return GetSystem()->version == entry->param && context->night != 0;
    case kEvolveSummit:
        return context->summit != 0;
    case kEvolveDusk:
        return context->dusk != 0;
    case kEvolveUltraSpace:
        return context->ultraSpace != 0;
    }
    return false;
}

// 0x7100723020: Floette's Eternal Flower form doesn't evolve
u16 EvolutionChecker::GetItemEvolution(Pokemon* pokemon, const EvolutionContext* context, u16 item, u32* index) {
    u16 species = pokemon->GetSpecies();
    u8 form = pokemon->GetForm();
    if (BlocksEvolution(pokemon->GetHeldItem())) {
        return species;
    }
    if (species == kSpeciesFloette && pokemon->GetForm() == kFormFloetteEternal) {
        return kSpeciesFloette;
    }
    personal::LoadEvolutions(species, form);
    u8 count = personal::GetEvolutionCount();
    for (u32 i = 0; i < count; i++) {
        EvolutionEntry entry;
        entry.method = personal::GetEvolutionMethod(i);
        entry.param = personal::GetEvolutionParam(i);
        entry.species = personal::GetEvolutionSpecies(i);
        entry.level = personal::GetEvolutionLevel(i);
        if (CheckItemEvolution(pokemon, context, item, &entry)) {
            if (index != nullptr) {
                *index = i;
            }
            return entry.species;
        }
    }
    return species;
}

// 0x7100723150
bool EvolutionChecker::CheckItemEvolution(Pokemon* pokemon, const EvolutionContext* context, u16 item,
                                          const EvolutionEntry* entry) {
    switch (entry->method) {
    case kEvolveItem:
        return entry->param == item;
    case kEvolveItemMale:
        return entry->param == item && pokemon->GetGender() == 0;
    case kEvolveItemFemale:
        return entry->param == item && pokemon->GetGender() == 1;
    case kEvolveItemUltraSpace:
        return entry->param == item && context->ultraSpace != 0;
    }
    return false;
}

// 0x7100723220: an Everstone doesn't stop Kadabra
u16 EvolutionChecker::GetTradeEvolution(Pokemon* pokemon, Pokemon* partner, u32* index) {
    u16 species = pokemon->GetSpecies();
    u8 form = pokemon->GetForm();
    if (pokemon->IsEgg(kEggAny)) {
        return species;
    }
    bool blocked = BlocksEvolution(pokemon->GetHeldItem());
    if (species != kSpeciesKadabra && blocked) {
        return species;
    }
    personal::LoadEvolutions(species, form);
    u8 count = personal::GetEvolutionCount();
    for (u32 i = 0; i < count; i++) {
        EvolutionEntry entry;
        entry.method = personal::GetEvolutionMethod(i);
        entry.param = personal::GetEvolutionParam(i);
        entry.species = personal::GetEvolutionSpecies(i);
        entry.level = personal::GetEvolutionLevel(i);
        if (CheckTradeEvolution(pokemon, partner, &entry)) {
            *index = i;
            return entry.species;
        }
    }
    return species;
}

// 0x7100723340: trading for another species is only Shelmet and Karrablast, whatever the parameter
bool EvolutionChecker::CheckTradeEvolution(Pokemon* pokemon, Pokemon* partner, const EvolutionEntry* entry) {
    switch (entry->method) {
    case kEvolveTrade:
        return true;
    case kEvolveTradeHeldItem:
        return pokemon->GetHeldItem() == entry->param;
    case kEvolveTradeSpecies:
        if (partner == nullptr || BlocksEvolution(partner->GetHeldItem()) || partner->IsEgg(kEggAny)) {
            return false;
        }
        return (pokemon->GetSpecies() == kSpeciesShelmet && partner->GetSpecies() == kSpeciesKarrablast) ||
               (pokemon->GetSpecies() == kSpeciesKarrablast && partner->GetSpecies() == kSpeciesShelmet);
    }
    return false;
}

static bool MegaConditionMet(Pokemon* pokemon, u16 method, u16 item, u16 heldItem) {
    switch (method) {
    case 1:
        return heldItem == item;
    case 2:
        return !item::IsZCrystal(heldItem) && pokemon->HasMove(kMoveDragonAscent);
    case 3:
        return true;
    }
    return false;
}

// 0x7100723430: none while Mega Evolved
u8 EvolutionChecker::GetMegaForm(Pokemon* pokemon) {
    u8 form = pokemon->GetForm();
    if (pokemon->GetUnk100()) {
        return form;
    }
    personal::LoadMegaEvolutions(pokemon->GetSpecies());
    u8 count = personal::GetMegaEvolutionCount();
    for (u32 i = 0; i < count; i++) {
        u8 megaForm = personal::GetMegaEvolutionForm(i);
        u16 method = personal::GetMegaEvolutionMethod(i);
        u16 item = personal::GetMegaEvolutionItem(i);
        if (MegaConditionMet(pokemon, method, item, pokemon->GetHeldItem())) {
            return megaForm;
        }
    }
    return form;
}

// 0x7100723580
bool EvolutionChecker::CanMegaEvolveTo(Pokemon* pokemon, u8 form) {
    if (pokemon->GetUnk100()) {
        return false;
    }
    personal::LoadMegaEvolutions(pokemon->GetSpecies());
    u8 count = personal::GetMegaEvolutionCount();
    for (u32 i = 0; i < count; i++) {
        if (personal::GetMegaEvolutionForm(i) != form) {
            continue;
        }
        u16 method = personal::GetMegaEvolutionMethod(i);
        u16 item = personal::GetMegaEvolutionItem(i);
        if (MegaConditionMet(pokemon, method, item, pokemon->GetHeldItem())) {
            return true;
        }
    }
    return false;
}

// 0x71007236e0
bool EvolutionChecker::IsMegaStone(u32 species, u16 item) {
    personal::LoadMegaEvolutions(species);
    u8 count = personal::GetMegaEvolutionCount();
    for (u32 i = 0; i < count; i++) {
        u16 method = personal::GetMegaEvolutionMethod(i);
        u16 stone = personal::GetMegaEvolutionItem(i);
        if (method == 1 && stone == item) {
            return true;
        }
    }
    return false;
}

// 0x7100723760
bool EvolutionChecker::BlocksEvolution(u16) {
    return false;
}

}  // namespace pokemon
