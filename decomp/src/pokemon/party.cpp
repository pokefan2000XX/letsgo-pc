// The party (pokemon/party.h)
#include "pokemon/party.h"

#include <utility>

namespace pokemon {

// 0x7100720d80: six empty Pokémon (made as level 1 species 0)
Party::Party(gfl::mem::MemoryResource* resource) {
    for (u32 i = 0; i < kMaxMembers; i++) {
        members_[i] = gfl::Create<PartyPokemon>()(resource, resource, 0, 1, 0);
    }
    Clear();
}

// 0x7100721070, deleting: 0x7100721210. The members go last to first, each pointer cleared; the
// members' own destructors would clear it too, but the compiler drops that store at the end of the
// party's lifetime, so they're reset here.
Party::~Party() {
    for (s32 i = kMaxMembers - 1; i >= 0; i--) {
        members_[i].Reset();
    }
}

// 0x71007213b0
bool Party::Add(const Pokemon& pokemon) {
    if (pokemon.IsEmpty()) {
        return false;
    }
    if (count_ >= kMaxMembers || GetMember(count_) == nullptr) {
        return false;
    }
    GetMember(count_)->CopyFrom(pokemon);
    count_++;
    if (GetMember(partner_)->IsEgg(kEggAny)) {
        UpdatePartner();
    }
    return true;
}

// 0x7100721480: a slot past the members is filled where it is, after any empty ones
void Party::Set(u32 index, const Pokemon& pokemon) {
    if (index >= kMaxMembers || GetMember(index) == nullptr || pokemon.IsEmpty()) {
        return;
    }
    bool wasEmpty = GetMember(index)->IsEmpty();
    GetMember(index)->CopyFrom(pokemon);
    if (wasEmpty) {
        count_++;
    }
    if (partner_ == index) {
        UpdatePartner();
    }
}

// 0x7100721550: the partner stays the same Pokémon, or is chosen again when it's the one removed
void Party::Remove(u32 index) {
    if (index >= kMaxMembers || count_ == 0 || members_[index].Get() == nullptr || GetMember(index)->IsEmpty()) {
        return;
    }
    GetMember(index)->Clear();
    Compact();
    count_--;
    if (partner_ == index) {
        UpdatePartner();
    } else if (partner_ > index) {
        partner_--;
    }
}

// 0x7100721610: each empty slot, last to first, is moved to the end
void Party::Compact() {
    for (s32 i = kMaxMembers - 2; i >= 0; i--) {
        if (members_[i]->IsEmpty()) {
            for (u32 j = i; j < kMaxMembers - 1; j++) {
                members_[j].Swap(members_[j + 1]);
            }
        }
    }
}

// 0x7100721700: the partner's index follows the swap, but not the compacting that a swap with an
// empty slot needs
void Party::Swap(u8 a, u8 b) {
    if (a == b || a >= kMaxMembers || b >= kMaxMembers) {
        return;
    }
    members_[a].Swap(members_[b]);
    Compact();
    if (partner_ == a) {
        partner_ = b;
    } else if (partner_ == b) {
        partner_ = a;
    }
}

// 0x71007217c0: the partner moves with its Pokémon
void Party::Reorder(const u8* order) {
    gfl::RefPtr<PartyPokemon> old[kMaxMembers];
    for (u32 i = 0; i < count_; i++) {
        old[i] = std::move(members_[i]);
    }
    bool movedPartner = false;
    for (u32 i = 0; i < count_; i++) {
        members_[i] = std::move(old[order[i]]);
        if (!movedPartner && order[i] == partner_) {
            partner_ = i;
            movedPartner = true;
        }
    }
}

// 0x7100721960
PartyPokemon* Party::GetMember(u32 index) {
    return const_cast<PartyPokemon*>(static_cast<const Party*>(this)->GetMember(index));
}

// 0x7100721970
const PartyPokemon* Party::GetMember(u32 index) const {
    if (index >= kMaxMembers) {
        return nullptr;
    }
    return members_[index].Get();
}

// 0x7100721990
u8 Party::GetMemberCount() const {
    return count_;
}

// 0x71007219a0
bool Party::IsFull() const {
    return count_ == kMaxMembers;
}

// 0x71007219b0
u32 Party::Count(CountKind kind) const {
    return Count(kind, 0);
}

// 0x71007219c0: an unknown kind counts nothing
u32 Party::Count(CountKind kind, u8 skipped) const {
    u32 total = 0;
    for (u32 i = 0; i < count_; i++, skipped >>= 1) {
        if (skipped & 1) {
            continue;
        }
        const PartyPokemon* member = GetMember(i);
        if (member == nullptr || member->IsEmpty()) {
            continue;
        }
        switch (kind) {
        case kCountMembers:
            total++;
            break;
        case kCountCanBattle:
            if (!member->IsEgg(kEggAny) && !member->IsFainted()) {
                total++;
            }
            break;
        case kCountNotEggs:
            if (!member->IsEgg(kEggAny)) {
                total++;
            }
            break;
        case kCountValidEggs:
            if (member->IsEgg(kEggValid)) {
                total++;
            }
            break;
        case kCountBadEggs:
            if (member->IsEgg(kEggBad)) {
                total++;
            }
            break;
        case kCountEggs:
            if (member->IsEgg(kEggAny)) {
                total++;
            }
            break;
        }
    }
    return total;
}

// 0x7100721b20: all six slots
bool Party::HasSpecies(u32 species) const {
    for (u32 i = 0; i < kMaxMembers; i++) {
        const PartyPokemon* member = GetMember(i);
        if (member != nullptr && !member->IsEgg(kEggAny) && member->GetSpecies() == species) {
            return true;
        }
    }
    return false;
}

// 0x7100721cc0
void Party::CopyFrom(const Party& other) {
    for (u32 i = 0; i < kMaxMembers; i++) {
        GetMember(i)->CopyFrom(*other.GetMember(i));
    }
    count_ = other.count_;
}

// 0x7100721e40
void Party::Clear() {
    for (u32 i = 0; i < kMaxMembers; i++) {
        GetMember(i)->Clear();
    }
    count_ = 0;
    partner_ = 0;
}

// 0x7100721ef0
u32 Party::SerializeCores(void* out) const {
    SerializedParty* party = static_cast<SerializedParty*>(out);
    for (u32 i = 0; i < kMaxMembers; i++) {
        GetMember(i)->SerializeCore(reinterpret_cast<CoreData*>(party->members[i]));
    }
    return sizeof(party->members);
}

// 0x7100721fe0
u32 Party::GetCoresSize() const {
    return sizeof(SerializedParty::members);
}

// 0x7100721ff0
u32 Party::Serialize(void* out, bool withCalcData) const {
    SerializedParty* party = static_cast<SerializedParty*>(out);
    for (u32 i = 0; i < kMaxMembers; i++) {
        if (withCalcData) {
            GetMember(i)->Serialize(party->members[i]);
        } else {
            GetMember(i)->SerializeCore(reinterpret_cast<CoreData*>(party->members[i]));
        }
    }
    party->count = count_;
    party->partner = partner_;
    party->unk61A = 0;
    return sizeof(SerializedParty);
}

// 0x71007221a0
u32 Party::Serialize(void* out) const {
    Serialize(out, false);
    return sizeof(SerializedParty);
}

// 0x71007221c0
void Party::DeserializeCores(const void* in) {
    const SerializedParty* party = static_cast<const SerializedParty*>(in);
    count_ = 0;
    for (u32 i = 0; i < kMaxMembers; i++) {
        GetMember(i)->DeserializeCore(reinterpret_cast<const CoreData*>(party->members[i]));
        if (!GetMember(i)->IsEmpty()) {
            count_++;
        }
    }
}

// 0x71007223d0
void Party::Deserialize(const void* in, bool withCalcData) {
    const SerializedParty* party = static_cast<const SerializedParty*>(in);
    u32 found = 0;
    for (u32 i = 0; i < kMaxMembers; i++) {
        if (withCalcData) {
            GetMember(i)->Deserialize(party->members[i]);
        } else {
            GetMember(i)->DeserializeCore(reinterpret_cast<const CoreData*>(party->members[i]));
        }
        if (!GetMember(i)->IsEmpty()) {
            found++;
        }
    }
    count_ = party->count;
    partner_ = party->partner;
    if (found != count_) {
        count_ = found;
    }
}

// 0x7100722750
void Party::Deserialize(const void* in) {
    Deserialize(in, false);
}

// 0x7100722760
void Party::HealAll() {
    u8 count = count_;
    for (u32 i = 0; i < count; i++) {
        GetMember(i)->Heal();
    }
}

// 0x71007227c0
void Party::SetPartner(u8 index) {
    partner_ = index;
}

// 0x71007227d0
u8 Party::GetPartner() const {
    return partner_;
}

// 0x71007227e0
void Party::UpdatePartner() {
    for (u32 i = 0; i < count_; i++) {
        if (!GetMember(i)->IsEgg(kEggAny)) {
            partner_ = i;
            return;
        }
    }
    partner_ = 0;
}

}  // namespace pokemon
