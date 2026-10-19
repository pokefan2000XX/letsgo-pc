// The new-areas hack (docs/PLAN.md, roadmap phase 3, step 3): areas a mod adds to the game.
//
// When the field starts, the game reads every placement file from a list of 147 paths in its code
// (0x710012cee0), and it finds a zone's scripts by searching those files. An area that isn't on the
// list still loads when the player warps there, from bin/field/param/placement/<area>.bin, but its
// zones' scripts don't. This hook reads, after the game's own, the placement file of every area a mod
// names in AreaNameHashTable.tbl after the game's 147, the way the original reads each of its own.
#include "decomp.h"

#include <nn/fs.h>

extern "C" void ORIG_710012cee0(void* field);

// nn::diag::detail::LogImpl(LogMetaData const&, char const*, ...): the runtime writes it to letsgo.log
extern "C" void _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(const void* meta, const char* format, ...);

extern "C" void FUN_7100135960(void* path, const char* const* text, u32 flags);              // a path from "data:/..."
extern "C" void FUN_71000adc70(void* request, void* path, u32 flags, u64 heap, u64 owner);     // gfl::fs::ReadFile
extern "C" void FUN_7100139760(void* requests, const void* request);                           // their vector's push_back
extern "C" void FUN_7100023c70(void* file, void* request);                                     // gfl::fs::File::RemoveRequest

extern "C" u8* DAT_710160d338;    // the object whose +0x108 the reads are given
extern "C" u8 DAT_7101527768[];   // gfl::fs::ReadRequest's vtable, where an object points
extern "C" u8 DAT_7101527638[];   // gfl::fs::FileRequest's, its base

namespace {

constexpr u32 kGameAreas = 147;                  // the areas on the game's list, and the first names in the table
constexpr u32 kRequestSize = 0x110;              // a gfl::fs::ReadRequest
constexpr u32 kFile = 0x08;                      // its file (reference-counted)
constexpr u32 kDone = 0x10, kFailed = 0x90;      // its two callbacks: 4 words (destroy at +8), then their storage at +0x20
constexpr u32 kRequests = 0x530;                 // the field's vector of them, one for each placement file
constexpr u32 kOwner = 0x510;
constexpr u64 kTableSize = 0x10000;
constexpr char kTable[] = "data:/bin/field/param/placement/table/AreaNameHashTable.tbl";
constexpr char kFolder[] = "data:/bin/field/param/placement/";

alignas(8) const u8 kMeta[0x80] = {};            // LogImpl's metadata; the runtime doesn't read it
alignas(8) u8 g_table[kTableSize];

using Fn = void (*)(void*);

template <typename T>
T& At(void* object, u32 offset) {
    return *reinterpret_cast<T*>(static_cast<u8*>(object) + offset);
}

// A reference-counted object's interface, at +0x48: 0x10 takes a reference, 0x18 drops one.
void Ref(u8* object, u32 slot) {
    u8* counted = object + 0x48;
    (*reinterpret_cast<Fn*>(*reinterpret_cast<u8**>(counted) + slot))(counted);
}

void DropCallback(u8* request, u32 at) {
    if (Fn destroy = At<Fn>(request, at + 8)) {
        for (u32 i = 0; i < 4; i++) At<u64>(request, at + 8 * i) = 0;
        destroy(request + at + 0x20);
    }
}

void DropPathPart(u8* path, u32 at) {
    if (u8* part = At<u8*>(path, at)) {
        Ref(part, 0x18);
        At<u8*>(path, at) = nullptr;
    }
}

// What the original's loop does for each of its paths.
void Read(void* field, const char* path) {
    alignas(16) u8 request[kRequestSize];
    At<u8*>(request, 0) = DAT_7101527768;
    At<u64>(request, kFile) = 0;
    for (u32 i = 0; i < 4; i++) At<u64>(request, kDone + 8 * i) = At<u64>(request, kFailed + 8 * i) = 0;

    alignas(8) u8 file_path[0x28] = {};
    const char* text = path;
    FUN_7100135960(file_path, &text, 0);
    FUN_71000adc70(request, file_path, 0, At<u64>(DAT_710160d338, 0x108), At<u64>(field, kOwner));
    DropPathPart(file_path, 0x20);
    DropPathPart(file_path, 0);

    FUN_7100139760(static_cast<u8*>(field) + kRequests, request);

    // The request's destruction, as the loop does it.
    At<u8*>(request, 0) = DAT_7101527638;
    if (u8* file = At<u8*>(request, kFile)) {
        Ref(file, 0x10);
        FUN_7100023c70(file, request);
        u8* held = At<u8*>(request, kFile);
        At<u8*>(request, kFile) = nullptr;
        if (held) Ref(held, 0x18);
        Ref(file, 0x18);
    }
    DropCallback(request, kFailed);
    DropCallback(request, kDone);
    if (u8* held = At<u8*>(request, kFile)) {
        Ref(held, 0x18);
        At<u8*>(request, kFile) = nullptr;
    }
}

u64 ReadTable() {
    nn::fs::FileHandle handle;
    if (nn::fs::OpenFile(&handle, kTable, nn::fs::OpenMode_Read).IsFailure()) return 0;
    long size = 0;
    u64 got = 0;
    if (nn::fs::GetFileSize(&size, handle).IsSuccess() && size > 0 && (u64)size <= kTableSize &&
        nn::fs::ReadFile(handle, 0, g_table, (u64)size).IsSuccess())
        got = (u64)size;
    nn::fs::CloseFile(handle);
    return got;
}

}  // namespace

extern "C" void FUN_710012cee0(void* field) {
    ORIG_710012cee0(field);

    // AHTB: a count, then each name's 64-bit hash, its 16-bit length with its 0, and the name.
    u64 size = ReadTable();
    if (size < 8 || g_table[0] != 'A' || g_table[1] != 'H' || g_table[2] != 'T' || g_table[3] != 'B') return;
    u32 count = At<u32>(g_table, 4);
    u64 at = 8;
    for (u32 i = 0; i < count && at + 10 <= size; i++) {
        u16 length = At<u16>(g_table, (u32)at + 8);
        const char* name = reinterpret_cast<const char*>(g_table + at + 10);
        at += 10 + length;
        if (i < kGameAreas || length < 2 || at > size || name[length - 1] != '\0') continue;

        char path[sizeof(kFolder) + 0x100 + 4];
        if (length - 1 > 0x100) continue;
        u32 n = 0;
        for (const char* s = kFolder; *s; s++) path[n++] = *s;
        for (u32 k = 0; k + 1 < length; k++) path[n++] = name[k];
        for (const char* s = ".bin"; *s; s++) path[n++] = *s;
        path[n] = '\0';
        Read(field, path);
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "new-areas: %s", path + 6);
    }
}
