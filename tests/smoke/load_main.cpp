// Smoke test for the recompilation memory model: map main's code and data at
// the address the analysis uses (0x7100000000), apply every dynamic
// relocation, and check that the relocated pointers land where they should.
// Guest addresses == host addresses is what lets recompiled and hand-written
// code share pointers, so this has to work on the target PC.
//
// Build: clang++ -O2 -std=c++20 -static tests/smoke/load_main.cpp -o build/smoke/load_main.exe
// Run:   build/smoke/load_main.exe orig/elf/main.elf data/main_functions.csv
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static constexpr uint64_t kBase = 0x7100000000ull;
static constexpr uint64_t kImportStubs = 0x7200000000ull;  // host-implemented imports live here

struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
              uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Shdr { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; };
struct Sym  { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
struct Rela { uint64_t offset, info; int64_t addend; };

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: load_main main.elf main_functions.csv\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(in)), {});
    auto* eh = reinterpret_cast<const Ehdr*>(elf.data());
    auto* ph = reinterpret_cast<const Phdr*>(elf.data() + eh->phoff);
    auto* sh = reinterpret_cast<const Shdr*>(elf.data() + eh->shoff);

    uint64_t image_end = 0;
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == 1) image_end = std::max(image_end, ph[i].vaddr + ph[i].memsz);
    image_end = (image_end + 0xFFFF) & ~0xFFFFull;

    auto* base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(kBase), image_end,
                                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* stubs = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(kImportStubs), 0x10000,
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (reinterpret_cast<uint64_t>(base) != kBase || reinterpret_cast<uint64_t>(stubs) != kImportStubs) {
        std::printf("FAIL: could not reserve the fixed guest addresses (got %p, %p)\n", base, stubs);
        return 1;
    }
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == 1) std::memcpy(base + ph[i].vaddr, elf.data() + ph[i].offset, ph[i].filesz);

    // dynamic symbols + relocation tables
    const Sym* syms = nullptr;
    std::vector<const Shdr*> relas;
    const Shdr* init_array = nullptr;
    for (int i = 0; i < eh->shnum; i++) {
        if (sh[i].type == 11) syms = reinterpret_cast<const Sym*>(base + sh[i].addr);
        if (sh[i].type == 4) relas.push_back(&sh[i]);
        if (sh[i].type == 14) init_array = &sh[i];
    }
    size_t n_rel = 0, n_imports = 0, n_relative = 0, relative_to_text = 0;
    uint64_t text_end = 0;
    for (int i = 0; i < eh->phnum; i++) if (ph[i].type == 1 && (ph[i].flags & 1)) text_end = ph[i].vaddr + ph[i].memsz;
    std::set<uint32_t> import_ids;
    for (auto* s : relas) {
        auto* r = reinterpret_cast<const Rela*>(base + s->addr);
        for (size_t k = 0; k < s->size / sizeof(Rela); k++, n_rel++) {
            uint32_t type = static_cast<uint32_t>(r[k].info), sym = static_cast<uint32_t>(r[k].info >> 32);
            auto* slot = reinterpret_cast<uint64_t*>(base + r[k].offset);
            if (type == 1027) {  // RELATIVE
                *slot = kBase + r[k].addend;
                n_relative++;
                if (static_cast<uint64_t>(r[k].addend) < text_end) relative_to_text++;
            } else if (type == 257 || type == 1025 || type == 1026) {  // ABS64 / GLOB_DAT / JUMP_SLOT
                const Sym& y = syms[sym];
                if (y.shndx != 0) {
                    *slot = kBase + y.value + (type == 257 ? r[k].addend : 0);
                } else {
                    import_ids.insert(sym);
                    *slot = kImportStubs + sym * 16 + (type == 257 ? r[k].addend : 0);
                    n_imports++;
                }
            } else {
                std::printf("FAIL: unhandled relocation type %u at 0x%llx\n", type, (unsigned long long)r[k].offset);
                return 1;
            }
        }
    }

    // every init_array entry must now point at a known function start
    std::set<uint64_t> starts;
    std::ifstream csv(argv[2]);
    std::string line;
    std::getline(csv, line);
    while (std::getline(csv, line)) starts.insert(std::stoull(line.substr(0, line.find(',')), nullptr, 16));
    size_t ctors = init_array->size / 8, ctors_ok = 0;
    auto* ia = reinterpret_cast<const uint64_t*>(base + init_array->addr);
    for (size_t i = 0; i < ctors; i++) ctors_ok += starts.count(ia[i]);

    std::printf("mapped main at 0x%llx (%.1f MB incl. bss), import stubs at 0x%llx\n",
                (unsigned long long)kBase, image_end / 1e6, (unsigned long long)kImportStubs);
    std::printf("relocations applied: %zu (RELATIVE %zu, %zu of them into code; import slots %zu for %zu symbols)\n",
                n_rel, n_relative, relative_to_text, n_imports, import_ids.size());
    std::printf("static constructors: %zu/%zu point at known function starts\n", ctors_ok, ctors);
    std::printf("host ABI: sizeof(long)=%zu sizeof(void*)=%zu (guest: 8/8 -> decomp code must use fixed-width types)\n",
                sizeof(long), sizeof(void*));
    bool ok = ctors_ok == ctors;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
