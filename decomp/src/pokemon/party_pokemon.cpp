// PartyPokemon: a Pokémon with party stats (pokemon/pokemon.h). A new or copied one gets its stats
// calculated and is healed.
#include "pokemon/pokemon.h"

namespace pokemon {

// 0x71007291a0: the Pokémon system's startup calls it after Pokemon::Initialize
void PartyPokemon::Initialize() {}

// 0x71007291b0
PartyPokemon::PartyPokemon(gfl::mem::MemoryResource* resource) : Pokemon(resource) {
    AddCalcData(resource);
}

// 0x7100729200
void PartyPokemon::AddCalcData(gfl::mem::MemoryResource* resource) {
    calc_ = CreateCalcData(resource, GetEncryptionKey());
    accessor_->AttachEncoded(core_.Get(), calc_.Get());
}

// 0x71007292d0
PartyPokemon::PartyPokemon(gfl::mem::MemoryResource* resource, u32 species, u16 level, u64 id)
    : Pokemon(resource, species, level, id) {
    AddCalcData(resource);
    bool started = StartFastMode();
    accessor_->ClearCalcData();
    RecalcStats(true);
    Heal();
    EndFastMode(started);
}

// 0x7100729350
PartyPokemon::PartyPokemon(gfl::mem::MemoryResource* resource, const CreationSpec& spec) : Pokemon(resource, spec) {
    AddCalcData(resource);
    bool started = StartFastMode();
    accessor_->ClearCalcData();
    RecalcStats(true);
    Heal();
    EndFastMode(started);
}

// 0x71007293d0
PartyPokemon::PartyPokemon(gfl::mem::MemoryResource* resource, const Pokemon& other) : Pokemon(resource) {
    AddCalcData(resource);
    CopyFrom(other);
}

// 0x7100729440
void PartyPokemon::CopyFrom(const Pokemon& other) {
    other.accessor_->Serialize(g_scratchRecord);
    accessor_->Deserialize(g_scratchRecord);
}

// 0x7100729480, deleting: 0x71007294a0
PartyPokemon::~PartyPokemon() {}

// 0x71007294e0
void PartyPokemon::SerializeCore(CoreData* core) const {
    Pokemon::SerializeCore(core);
}

// 0x71007294f0
void PartyPokemon::DeserializeCore(const CoreData* core) {
    Pokemon::DeserializeCore(core);
    bool started = StartFastMode();
    accessor_->ClearCalcData();
    RecalcStats(true);
    Heal();
    EndFastMode(started);
}

}  // namespace pokemon
