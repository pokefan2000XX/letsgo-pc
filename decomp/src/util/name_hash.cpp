// Names compared by Game Freak's 64-bit FNV-1a hash (offset basis 0xCBF29CE484222645, prime
// 0x100000001B3), the hash they use for file and object names. The class isn't identified yet:
// callers reach it through reference-counted handles, and its name string is at +0x88.
#include "decomp.h"

namespace {

u64 Fnv1a64(const char* s) {
    u64 h = 0xCBF29CE484222645ull;
    for (; *s != '\0'; s++) {
        h ^= (u8)*s;
        h *= 0x100000001B3ull;
    }
    return h;
}

}  // namespace

struct UnkNamedObject {
    u8 unk0[0x88];
    const char* name;
};
DECOMP_OFFSET(UnkNamedObject, name, 0x88);

// 0x7100101b30
extern "C" bool UnkNamedObject_NameEquals(const UnkNamedObject* self, const char* name) {
    return Fnv1a64(self->name) == Fnv1a64(name);
}

// 0x7100101ba0
extern "C" const char* UnkNamedObject_GetName(const UnkNamedObject* self) {
    return self->name;
}
