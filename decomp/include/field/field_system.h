// The field's placement files (docs/PLAN.md, roadmap phase 3, step 2). When the field starts it reads
// every placement file on a list of 147 in its code (0x710012cee0), each through a read request kept in
// a vector in the field's object; a zone is found by searching those files for the hash of its name.
#pragma once
#include "decomp.h"

namespace field {

// A gfl::fs::ReadRequest (0x110 bytes): its file, whose bytes, once read, are at +0x170.
struct ReadRequest {
    void* vtable;
    u8* file;
    u8 callbacks[0x100];
};
DECOMP_SIZE(ReadRequest, 0x110);

constexpr u32 kReadFileData = 0x170;

// The field's object, as far as the placement files go.
struct FieldSystem {
    // A placement file's root table, by its place in the vector (the game's list, in order).
    const u8* PlacementRoot(int index) const;
    // The zone table whose information (its field 0) has this name hash (its field 1), searching every
    // placement file in order; null when none has it.
    const u8* FindZone(u64 name) const;
    // The same, also giving the hash of the zone's area's name (the root's field 1).
    const u8* FindZone(u64 name, u64* area) const;

    u8 unk0[0x530];
    ReadRequest* placements_begin;
    ReadRequest* placements_end;
    ReadRequest* placements_capacity;
};
DECOMP_OFFSET(FieldSystem, placements_begin, 0x530);

}  // namespace field
