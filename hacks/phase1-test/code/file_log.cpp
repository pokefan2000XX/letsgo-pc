// The phase 1 test hack's hook (docs/PLAN.md, roadmap phase 1, step 4): the game's file open at
// 0x71000238e0 logs what it opens, then runs as it always does. That function isn't decompiled: the
// hook replaces it and calls the original through ORIG_71000238e0 (decomp/tools/build.py).
#include "decomp.h"

extern "C" u32 ORIG_71000238e0(void* file);

// nn::diag::detail::LogImpl(LogMetaData const&, char const*, ...): the runtime writes it to letsgo.log
extern "C" void _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(const void* meta, const char* format, ...);

namespace {

// The game's file object, as the original reads it: the archive the file is in (null for a file in
// RomFS), the key it is looked up by there, and the name of a RomFS file, whose path is at +0x60.
constexpr u32 kArchive = 0x148, kKey = 0x68, kName = 0x60, kNamePath = 0x60;

alignas(8) const u8 kMeta[0x80] = {};   // LogImpl's metadata; the runtime doesn't read it

template <typename T>
T At(const void* object, u32 offset) {
    return *reinterpret_cast<const T*>(static_cast<const u8*>(object) + offset);
}

}  // namespace

extern "C" u32 FUN_71000238e0(void* file) {
    if (At<const void*>(file, kArchive))
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "phase1-test: open %016lx in an archive", At<u64>(file, kKey));
    else if (const void* name = At<const void*>(file, kName))
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "phase1-test: open %s", At<const char*>(name, kNamePath));
    return ORIG_71000238e0(file);
}
