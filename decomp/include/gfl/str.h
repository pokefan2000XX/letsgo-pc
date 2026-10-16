// Game Freak's text library: strings of 16-bit characters with tag sequences (0x10, length, data)
// that are copied and compared whole. Not decompiled yet: these link to the originals.
#pragma once
#include "decomp.h"

namespace gfl::str {

// A string with its own buffer
class StrBuf {
public:
    // Copies text up to the buffer's capacity; returns the new length
    u16 SetStr(const u16* text);
    const u16* GetPtr() const;
};

// Copies at most size - 1 characters and a terminator; returns the number copied
u32 StrNCopy(u16* dst, const u16* src, u32 size);
bool StrEqual(const u16* a, const u16* b);

}  // namespace gfl::str
