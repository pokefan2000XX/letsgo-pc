// The party: six Pokémon objects, the members first and empty ones after them, and which member
// is the partner (scripts' SetPartnerIndex_). The objects are made with the party: adding, setting
// and removing copy records into them or clear them, and only reordering moves them around.
#pragma once
#include "decomp.h"
#include "gfl/object.h"
#include "pokemon/pokemon.h"

namespace pokemon {

// What Party::Count counts (among the members that aren't empty)
enum CountKind : u32 {
    kCountMembers,
    kCountCanBattle,        // neither an egg (of any kind) nor fainted
    kCountNotEggs,
    kCountValidEggs,
    kCountBadEggs,
    kCountEggs,             // of any kind
};

// The party as saves keep it: each member's record, its party stats included, then the count and
// the partner
struct SerializedParty {
    u8 members[6][kRecordSize];
    u8 count;
    u8 partner;
    u16 unk61A;             // 0
};
DECOMP_OFFSET(SerializedParty, count, 0x618);
DECOMP_SIZE(SerializedParty, 0x61C);

class Party : public gfl::RefObject {
public:
    static constexpr u32 kMaxMembers = 6;

    explicit Party(gfl::mem::MemoryResource* resource);
    ~Party() override;

    // Copies a Pokémon in after the last member; false when it's empty or the party is full
    virtual bool Add(const Pokemon& pokemon);
    // Copies a Pokémon over a slot (empty or not); an empty Pokémon is ignored
    virtual void Set(u32 index, const Pokemon& pokemon);
    // Clears a member and moves the ones after it up
    virtual void Remove(u32 index);
    virtual void Swap(u8 a, u8 b);
    // Puts the member at order[i] at i, for each member; order holds each member's index once
    virtual void Reorder(const u8* order);
    virtual PartyPokemon* GetMember(u32 index);
    virtual const PartyPokemon* GetMember(u32 index) const;  // null past the last slot
    virtual bool IsFull() const;
    // The other party's records and count (not its partner)
    virtual void CopyFrom(const Party& other);
    virtual void Clear();
    // Makes the first member that isn't an egg the partner (the first member when they all are)
    virtual void UpdatePartner();

    u8 GetMemberCount() const;
    u32 Count(CountKind kind) const;
    // Leaves out the members whose bits are set in `skipped` (bit 0 is the first member)
    u32 Count(CountKind kind, u8 skipped) const;
    bool HasSpecies(u32 species) const;   // a member that isn't an egg
    void HealAll();                       // HP, status and PP
    u8 GetPartner() const;
    void SetPartner(u8 index);

    // Saving and loading a SerializedParty (the sizes are returned). The Cores versions do only
    // each record's core data; the others also do the count and partner, and the party stats when
    // asked. Loading counts the members itself (Deserialize: when the saved count is wrong).
    u32 SerializeCores(void* out) const;
    u32 GetCoresSize() const;
    u32 Serialize(void* out, bool withCalcData) const;
    u32 Serialize(void* out) const;       // without party stats
    void DeserializeCores(const void* in);
    void Deserialize(const void* in, bool withCalcData);
    void Deserialize(const void* in);     // without party stats

    // Moves the empty slots after the members, keeping the members in order
    void Compact();

    gfl::RefPtr<PartyPokemon> members_[kMaxMembers];
    u8 count_ = 0;
    u8 partner_ = 0;          // a member's index (6, none, in some parties)
};
DECOMP_OFFSET(Party, members_, 0x58);
DECOMP_OFFSET(Party, count_, 0x88);
DECOMP_OFFSET(Party, partner_, 0x89);
DECOMP_SIZE(Party, 0x90);

}  // namespace pokemon
