// Shared definitions for decompiled game code (built for the Switch's CPU by decomp/tools/build.py).
//
// A decompiled function replaces the original when its symbol is listed in decomp/symbols.txt
// with the original's address (or is named FUN_<address>). Functions it calls and data it uses
// are linked to the originals the same way, so decompiled code mixes freely with original code.
#pragma once
#include <cstddef>
#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

// What the compiler uses for the destructors of global objects: decompiled static initializers
// call it themselves (in this game it does nothing, since the game never exits)
extern "C" int __cxa_atexit(void (*destructor)(void*), void* object, void* dso);
extern "C" void* __dso_handle;

// checks a member's offset against the original layout
#define DECOMP_OFFSET(type, member, offset) \
    static_assert(offsetof(type, member) == (offset), #type "::" #member " is at " #offset " in the original")
#define DECOMP_SIZE(type, size) static_assert(sizeof(type) == (size), "sizeof(" #type ") is " #size " in the original")
