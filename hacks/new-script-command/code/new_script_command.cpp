// The new-script-command hack (docs/PLAN.md, roadmap phase 6, step 4): script commands written in C++.
//
// A script calls a command (a Pawn native) by the 32-bit hash of its name (h = h * 131 ^ c, docs/SCRIPTS.md). When the
// field prepares a script it registers its command tables with it: 0x710017dbf0 registers seven, 0x710019d490 four, each
// through 0x7100674ae0, which hashes every name in a table of {name, function} pairs (ending with a null name) and gives
// the script's commands of those hashes their functions. A command is called with the script and its arguments (the
// first cell holds their size in bytes, then one 64-bit cell each) and returns a cell to the script.
//
// This hook registers a table of its own after the game's, so any script can call these commands by name:
//   PokefanLog(a, b, c, d)   writes its arguments (up to four) to letsgo.log; returns how many it had
//   PokefanAdd(a, b)         returns a + b
// A script names them like any command: `var1 = PokefanAdd(40, 2)` then `PokefanLog(var1)` in tools/amx.py's source.
#include "decomp.h"

extern "C" void ORIG_710017dbf0(void* scripts);                    // registers the field's command tables
extern "C" void ORIG_710019d490(void* scripts);                    // the shorter set
extern "C" void ORIG_7100674ae0(void* scripts, const void* table);  // registers one table
extern "C" void _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(const void* meta, const char* format, ...);

namespace {

alignas(8) const u8 kMeta[0x80] = {};   // LogImpl's metadata; the runtime doesn't read it

using Function = s64 (*)(void* amx, const s64* params);
struct Command {
    const char* name;
    Function function;
};

s64 Count(const s64* params) {
    return params[0] / 8;
}

s64 PokefanLog(void*, const s64* params) {
    s64 n = Count(params);
    s64 v[4] = {};
    for (s64 i = 0; i < n && i < 4; i++) v[i] = params[1 + i];
    _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "script: PokefanLog %lld argument(s): %lld %lld %lld %lld",
                                                        (long long)n, (long long)v[0], (long long)v[1], (long long)v[2],
                                                        (long long)v[3]);
    return n;
}

s64 PokefanAdd(void*, const s64* params) {
    return Count(params) >= 2 ? params[1] + params[2] : 0;
}

const Command kCommands[] = {
    {"PokefanLog", PokefanLog},
    {"PokefanAdd", PokefanAdd},
    {nullptr, nullptr},
};

}  // namespace

extern "C" void FUN_710017dbf0(void* scripts) {
    ORIG_710017dbf0(scripts);
    ORIG_7100674ae0(scripts, kCommands);
    _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "new-script-command: registered (set 1) %p", scripts);
}

extern "C" void FUN_710019d490(void* scripts) {
    ORIG_710019d490(scripts);
    ORIG_7100674ae0(scripts, kCommands);
    _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "new-script-command: registered (set 2) %p", scripts);
}
