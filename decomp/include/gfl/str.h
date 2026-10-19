// Game Freak's text library: strings of 16-bit characters with tag sequences (0x10, length, data)
// that are copied and compared whole. Not decompiled yet: these link to the originals.
#pragma once
#include "decomp.h"
#include "gfl/fs.h"
#include "gfl/mem.h"
#include "gfl/object.h"

namespace gfl::str {

// A string with its own buffer, allocated in the resource current when it's constructed
class StrBuf : public RefObject {
public:
    explicit StrBuf(u16 capacity);        // in characters, the terminator included
    ~StrBuf() override;

    // Copies text up to the buffer's capacity; returns the new length
    u16 SetStr(const u16* text);
    const u16* GetPtr() const;

private:
    mem::UniquePtr<u16> text_;
    u16 capacity_;
    u16 length_;
    bool unk7C_;                          // set once the buffer is allocated
};
DECOMP_SIZE(StrBuf, 0x80);

// A message file (one string per index, in one language), read from a path
class alignas(16) MsgData : public RefObject {
public:
    MsgData(const fs::Path& path, u32 mode);
    ~MsgData() override;
    // The string at `index` into `out`; false when there's none
    bool GetString(u32 index, StrBuf* out);

    u8 unk58_[0x178];
};
DECOMP_SIZE(MsgData, 0x1D0);

// Copies at most size - 1 characters and a terminator; returns the number copied
u32 StrNCopy(u16* dst, const u16* src, u32 size);
bool StrEqual(const u16* a, const u16* b);

}  // namespace gfl::str
