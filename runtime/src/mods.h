// The mods folder (docs/MODS.md): files that take the place of the game's RomFS files, files inside its
// gfpak archives among them, without touching the game's own files. hle_sys.cpp's nn::fs reads through it.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rt::mods {

// Finds the mods in root and logs them: root itself when it has a romfs/ folder (one mod), else every
// folder in it that has one, and the emulators' layouts (contents/010003F003A34000/<mod>/romfs/, and
// Atmosphere's contents/010003F003A34000/romfs/). They apply in name order: when two mods have the same
// file, the later one's is used.
void init(const std::filesystem::path& root);

// The file a mod puts at a RomFS path ("bin/..."), or null when no mod has one; when the game opens it
// (opened), the first time is logged
const std::filesystem::path* file(const std::string& rel, bool opened = false);
bool is_dir(const std::string& rel);                 // whether a mod has a folder at this RomFS path

// A gfpak with the mods' files put in, laid out as: its header and tables (rebuilt), the bytes of the
// archive's own entries that stay (in their order, the ones a mod replaces left out, so the archive
// grows only by what the mods' files add), then the mods' entries. A mod file whose path is an entry's
// replaces that entry, and one in a folder the archive has is added to that folder, unless the RomFS
// has that file on its own (in_romfs).
struct Patched {
    std::vector<uint8_t> head;                       // [0, head.size())
    struct Run {
        uint64_t at, from, len;                      // the archive's [from, +len) at at
    };
    std::vector<Run> runs;                           // in order, between the head and the tail
    std::vector<uint8_t> tail;                       // [tail_at, size)
    uint64_t tail_at = 0, size = 0;
};
using ReadFn = std::function<bool(uint64_t offset, void* out, uint64_t size)>;   // all of it, or false
// One of the game's other archives that can hold files of the same folders as the one being changed (an
// area's pieces each have an archive of their own and share its model folder with the area's archive): its
// RomFS path, its size and a reader
struct Neighbour {
    std::string rel;
    uint64_t size;
    ReadFn read;
};
using NeighboursFn = std::function<std::vector<Neighbour>(const std::string& rel)>;
// The archive at rel as the mods change it (size and read give the archive the game has: its own or a
// mod's), built the first time and kept; null when no mod changes it or it isn't a gfpak. A mod file is
// added to a folder of it only when the game has that file nowhere: not on its own (in_romfs) and not in
// one of its neighbours, where the game reads it from (else every piece of an area would get every other
// piece's model, and the area would no longer fit in the game's memory).
std::shared_ptr<const Patched> patch_archive(const std::string& rel, uint64_t size, const ReadFn& read,
                                             const std::function<bool(const std::string& rel)>& in_romfs,
                                             const NeighboursFn& neighbours = nullptr);
// Reads a patched archive; returns how many bytes it read (fewer at its end)
uint64_t read(const Patched& p, uint64_t offset, void* out, uint64_t size, const ReadFn& read_base);

// The LZ4 block (no frame) a gfpak stores an entry as
std::vector<uint8_t> lz4_encode(const uint8_t* in, size_t size);

}  // namespace rt::mods
