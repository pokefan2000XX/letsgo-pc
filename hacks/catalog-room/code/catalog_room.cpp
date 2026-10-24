// catalog-room: room for more Pokémon in the model catalog (bin/pokemon/table/poke_resource_table.gfbpmcatalog).
//  - nnMain makes one of the game's fixed heaps 0x41a00000 bytes (nnMain+0x244, kept at the heap table's +0xa8). The catalog
//    is loaded into it (0x71001733a0, from 0x710013b650): the file, then a 312-byte block for each entry. Right after it, the
//    game carves a sub-heap of 0x41944800 bytes out of the same heap (0x71000af6e0, from 0x71000ae5e0, the size read from
//    0x7100f49148). That leaves the catalog and its neighbours 768,000 bytes: the game's own catalog (732 entries) fits with
//    about 70 KB spare, and one with 179 more Pokémon (1,388 entries, its strings stored once) was 9.4 KB short, so the
//    sub-heap's allocation failed and the game stopped at start.
//  - The sub-heap is then cut into eight child heaps of fixed sizes (0x71000af7a0, the sizes from 0x7100f49150), which leave
//    it about 286 KB spare (seen: with 1 MB taken, the sixth child didn't fit). Here the sub-heap is made 128 KB smaller, which
//    gives the catalog 128 KB more (room for about 330 more entries) and leaves the sub-heap about 158 KB spare.
#include "decomp.h"

extern "C" {
void ORIG_71000af6e0(void* self, void** heap, const u64** size);

void FUN_71000af6e0(void* self, void** heap, const u64** size) {
    static const u64 smaller = 0x41944800ull - 0x20000ull;
    if (size != nullptr && *size != nullptr && **size == 0x41944800ull) {
        const u64* given = &smaller;
        ORIG_71000af6e0(self, heap, &given);
        return;
    }
    ORIG_71000af6e0(self, heap, size);
}
}
