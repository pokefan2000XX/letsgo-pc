// modstest: the runtime's mods folder (runtime/src/mods.cpp) on the game's own archives, outside the
// game. It puts a mods folder's files into a gfpak the way the runtime does when the game opens it, and
// writes the archive the game would read. tools/check_mods.py makes test mods and checks the results.
//
//   build/pc/modstest.exe <mods folder> <gfpak> <its RomFS path> <output file>
#include <cstdio>
#include <vector>

#include "mods.h"
#include "rt.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: modstest <mods folder> <gfpak> <its RomFS path (bin/archive/...)> <output file>\n");
        return 2;
    }
    rt::mods::init(argv[1]);
    FILE* f = fopen(argv[2], "rb");
    if (!f) {
        fprintf(stderr, "modstest: can't open %s\n", argv[2]);
        return 1;
    }
    _fseeki64(f, 0, SEEK_END);
    uint64_t size = (uint64_t)_ftelli64(f);
    rt::mods::ReadFn base = [f](uint64_t off, void* out, uint64_t n) {
        _fseeki64(f, (int64_t)off, SEEK_SET);
        return fread(out, 1, (size_t)n, f) == n;
    };
    auto patched = rt::mods::patch_archive(argv[3], size, base, [](const std::string&) { return false; });
    FILE* out = fopen(argv[4], "wb");
    if (!out) {
        fprintf(stderr, "modstest: can't write %s\n", argv[4]);
        return 1;
    }
    // read in pieces of an odd size, so that reads cross from the tables to the archive's bytes to the mods'
    uint64_t total = patched ? patched->size : size;
    std::vector<uint8_t> buf(65521);
    for (uint64_t at = 0; at < total;) {
        uint64_t n = patched ? rt::mods::read(*patched, at, buf.data(), buf.size(), base)
                             : (base(at, buf.data(), std::min<uint64_t>(buf.size(), total - at)) ? std::min<uint64_t>(buf.size(), total - at) : 0);
        if (!n) {
            fprintf(stderr, "modstest: read failed at 0x%llx\n", (unsigned long long)at);
            return 1;
        }
        fwrite(buf.data(), 1, (size_t)n, out);
        at += n;
    }
    fclose(out);
    printf("%s: %s, %llu bytes\n", argv[3], patched ? "changed" : "unchanged", (unsigned long long)total);
    return 0;
}
