// The field's placement files, as the field looks things up in them (field/field_system.h).
#include "field/field_system.h"

// std::__vector_base_common<true>::__throw_out_of_range() const: vector::at's out-of-range path
extern "C" [[noreturn]] void _ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv(const void* self);

namespace field {

namespace {

// FlatBuffers as the placement files are: a table starts with the signed offset back to its vtable; a
// vtable has its own size, the table's, then a 16-bit offset for each field (0 for one left out). The
// lookups read a table's field 0 without checking the vtable has it, as the original does.
const u8* VTable(const u8* table) {
    return table - *reinterpret_cast<const s32*>(table);
}

u16 Slot(const u8* vtable, u32 field) {
    return *reinterpret_cast<const u16*>(vtable + 4 + 2 * field);
}

const u8* Follow(const u8* at) {
    return at + *reinterpret_cast<const u32*>(at);
}

const u8* Root(const ReadRequest& request) {
    return Follow(*reinterpret_cast<const u8* const*>(request.file + kReadFileData));
}

// The offset of a table's field 1, or 0 when its vtable is too short to have one or leaves it out.
u16 SecondField(const u8* vtable) {
    return *reinterpret_cast<const u16*>(vtable) < 7 ? 0 : Slot(vtable, 1);
}

// A placement file's zone with this name: the zones are the root's field 0, a zone's information its
// field 0, and the information's field 1 the hash of the zone's name (0 when left out).
const u8* ZoneIn(const u8* root, u64 name) {
    const u8* zones = Follow(root + Slot(VTable(root), 0));
    u32 count = *reinterpret_cast<const u32*>(zones);
    for (u32 i = 0; i < count; i++) {
        const u8* zone = Follow(zones + 4 + 4 * i);
        const u8* info = Follow(zone + Slot(VTable(zone), 0));
        u16 at = SecondField(VTable(info));
        if (at == 0 ? name == 0 : *reinterpret_cast<const u64*>(info + at) == name) return zone;
    }
    return nullptr;
}

}  // namespace

// 0x7100132900
const u8* FieldSystem::PlacementRoot(int index) const {
    u64 count = (u64)(placements_end - placements_begin);
    if (count <= (u64)(s64)index) _ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv(&placements_begin);
    return Root(placements_begin[index]);
}

// 0x7100132960
const u8* FieldSystem::FindZone(u64 name) const {
    u64 count = (u64)(placements_end - placements_begin);
    for (u64 i = 0; i < count; i++)
        if (const u8* zone = ZoneIn(Root(placements_begin[i]), name)) return zone;
    return nullptr;
}

// 0x7100132aa0
const u8* FieldSystem::FindZone(u64 name, u64* area) const {
    u64 count = (u64)(placements_end - placements_begin);
    for (u64 i = 0; i < count; i++) {
        const u8* root = Root(placements_begin[i]);
        u16 at = SecondField(VTable(root));
        u64 area_name = at == 0 ? 0 : *reinterpret_cast<const u64*>(root + at);
        if (const u8* zone = ZoneIn(root, name)) {
            *area = area_name;
            return zone;
        }
    }
    return nullptr;
}

}  // namespace field
