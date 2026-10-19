// Learnset and LearnsetCache: species' level-up moves (pokemon/personal.h).
#include <string.h>

#include "pokemon/personal.h"
#include "pokemon/system.h"

namespace pokemon::personal {

// 0x7100720240
void Learnset::SetSource(gfl::mem::MemoryResource*, void* source) {
    if (g_learnsetSource == nullptr) {
        g_learnsetSource = source;
    }
}

// 0x7100720260
void Learnset::ClearSource() {
    g_learnsetSource = nullptr;
}

// 0x7100720270
Learnset::Learnset(gfl::mem::MemoryResource* resource) : species_(0), form_(0), count_(0) {
    moves_ = gfl::mem::MakeUniqueArray<LevelUpMove>(resource, kMaxLevelUpMoves);
}

// 0x7100720390
Learnset::~Learnset() {}

// 0x71007203c0
u32 Learnset::GetSpecies() const {
    return species_;
}

// 0x71007203d0
u8 Learnset::GetForm() const {
    return form_;
}

// The moves before the end marker
static u8 CountMoves(const LevelUpMove* moves) {
    u8 count = 0;
    for (u32 i = 0; i < kMaxLevelUpMoves; i++) {
        if (moves[i].move == 0xFFFF && moves[i].level == 0xFFFF) {
            break;
        }
        count++;
    }
    return count;
}

// 0x71007203e0: a file that isn't there leaves the moves as they were (and they're counted again)
void Learnset::Load(s32 species, u8 form) {
    if (species_ == static_cast<u32>(species) && form_ == form) {
        return;
    }
    gfl::fs::Path path = GetLearnsetPath(PersonalTable::GetEntryIndex(species, form));
    gfl::fs::ReadRequest request{};
    gfl::fs::ReadFile(&request, path, 0, GetSystem()->fileDevice, 0);
    size_t size = request.file_->size_;
    if (size != 0) {
        size_t full = sizeof(LevelUpMove) * kMaxLevelUpMoves;
        memcpy(moves_.Get(), request.file_->data_, size < full ? size : full);
    }
    count_ = CountMoves(moves_.Get());
    species_ = species;
    form_ = form;
}

// 0x7100720600
bool Learnset::StartLoad(s32 species, u8 form) {
    if (species_ == static_cast<u32>(species) && form_ == form) {
        return false;
    }
    gfl::fs::Path path = GetLearnsetPath(PersonalTable::GetEntryIndex(species, form));
    GetFileLoader()->Read(path, moves_.Get(), sizeof(LevelUpMove) * kMaxLevelUpMoves);
    species_ = species;
    form_ = form;
    count_ = 0;
    return true;
}

// 0x71007206c0
bool Learnset::FinishLoad() {
    if (!GetFileLoader()->IsDone(moves_.Get())) {
        return false;
    }
    if (count_ == 0) {
        count_ = CountMoves(moves_.Get());
    }
    return true;
}

// 0x7100720750
void Learnset::CopyTo(Learnset* other) const {
    memcpy(other->moves_.Get(), moves_.Get(), sizeof(LevelUpMove) * kMaxLevelUpMoves);
    other->species_ = species_;
    other->form_ = form_;
    other->count_ = count_;
}

// 0x71007207a0
u8 Learnset::GetCount() const {
    return count_;
}

// 0x71007207b0
u32 Learnset::GetKind(u16 index) const {
    if (index >= count_) {
        return 2;
    }
    u16 level = moves_[index].level;
    if (level == 0) {
        return 0;
    }
    return level == 1 ? 1 : 2;
}

// 0x71007207f0
u16 Learnset::GetLevel(u16 index) const {
    return moves_[index < count_ ? index : 0].level;
}

// 0x7100720810
u16 Learnset::GetMove(u16 index) const {
    return moves_[index < count_ ? index : 0].move;
}

// 0x7100720830
LearnsetCache::LearnsetCache(gfl::mem::MemoryResource* resource, u32 capacity) : capacity_(capacity), next_(0) {
    entries_ = gfl::mem::MakeObjectArray<Learnset>(resource, capacity, resource);
}

// 0x7100720ab0, deleting: 0x7100720b60
LearnsetCache::~LearnsetCache() {}

// 0x7100720c00
bool LearnsetCache::Find(s32 species, u8 form, Learnset* out) {
    for (u32 i = 0; i < capacity_; i++) {
        Learnset& entry = entries_[i];
        if (entry.GetSpecies() == static_cast<u32>(species) && entry.GetForm() == form) {
            entry.CopyTo(out);
            return true;
        }
    }
    return false;
}

// 0x7100720cb0
void LearnsetCache::Add(const Learnset& learnset) {
    u32 species = learnset.GetSpecies();
    u8 form = learnset.GetForm();
    for (u32 i = 0; i < capacity_; i++) {
        if (entries_[i].GetSpecies() == species && entries_[i].GetForm() == form) {
            return;
        }
    }
    learnset.CopyTo(&entries_[next_]);
    next_ = (next_ + 1) % capacity_;
}

}  // namespace pokemon::personal
