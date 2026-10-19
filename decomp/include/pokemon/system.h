// The Pokémon system: one global object with what Pokémon need from the rest of the game (the game's
// language, version and region, the evolution rules, the random number generator). Not decompiled
// yet: only the fields used so far are named.
#pragma once
#include "decomp.h"
#include "gfl/fs.h"

namespace pokemon {

class EvolutionChecker;

struct System {
    void* personalSource;     // where the species data tables are read from
    void* learnsetSource;
    void* growthSource;
    void* evolutionSource;
    void* megaSource;
    u8 unk28[0x10];
    u32 unkCacheCapacity;
    u32 growthCacheCapacity;
    u32 learnsetCacheCapacity;
    u32 evolutionCacheCapacity;
    u32 megaCacheCapacity;
    u8 unk4C[0x9C];
    gfl::fs::Path speciesNames[11];   // by language, the files to read them from (unk18_ is 0 for none)
    u8 unk2F8[8];
    EvolutionChecker* evolution;
    u8 language;              // the game's text language
    u8 country;               // the console's country and region, as records store them
    u8 region;
    u8 consoleRegion;
    u8 version;               // the game version
    u8 unk30D[3];
    u32 (*random)();          // 32 random bits
    u32 (*randomBelow)(u32 max);
    void* fileDevice;         // where the game's data files are read from
};
DECOMP_OFFSET(System, megaCacheCapacity, 0x48);
DECOMP_OFFSET(System, speciesNames, 0xE8);
DECOMP_OFFSET(System, evolution, 0x300);
DECOMP_OFFSET(System, language, 0x308);
DECOMP_OFFSET(System, version, 0x30C);
DECOMP_OFFSET(System, random, 0x310);
DECOMP_OFFSET(System, randomBelow, 0x318);
DECOMP_OFFSET(System, fileDevice, 0x320);

System* GetSystem();

// Random numbers from the system's generator: 32 bits, or below `max`
u32 Random();
u32 Random(u32 max);

// The data files' paths: an item's data (item%03d.dat), the per-item flags, the species data
// (personal_total.bin), a species entry's level-up moves, and pms_total.bin
gfl::fs::Path GetItemDataPath(u32 item);
gfl::fs::Path GetItemFlagsPath();
gfl::fs::Path GetPersonalDataPath();
gfl::fs::Path GetLearnsetPath(u32 entry);
gfl::fs::Path GetPmsDataPath();

// Reads files in the background
class FileLoader {
public:
    void Read(const gfl::fs::Path& path, void* buffer, size_t size);
    bool IsDone(void* buffer);            // whether the read into `buffer` has finished
};
FileLoader* GetFileLoader();

}  // namespace pokemon
