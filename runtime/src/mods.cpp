// The mods folder (mods.h). A mod's file is matched by its RomFS path as the game names it (case
// matters); a file inside a gfpak by the hashes the archive keeps of its path: Game Freak's FNV-1a 64
// of the full path ("bin/field/model/area02/house01_01_area02.bntx"), and of its folder ("bin/field/
// model/area02/") and name. The archive layout is the one letsgostudio's GfpakArchive writes and
// documents (docs/dev/formats/gfpak.md there).
#include "mods.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "rt.h"

namespace rt::mods {
namespace fs = std::filesystem;

namespace {

const char* const kTitle = "010003f003a34000";       // Let's Go, Pikachu!

struct ModFile {
    fs::path host;
    size_t mod;                                      // index in g_names
};
struct Loose {                                       // a mod file under bin/, as a gfpak would name it
    uint64_t path_hash, name_hash;
    std::string rel;
};

std::vector<std::string> g_names;                    // the mods, in the order they apply
std::unordered_map<std::string, ModFile> g_files;    // RomFS path -> the last mod's file
std::unordered_set<std::string> g_dirs;
std::vector<Loose> g_loose;
std::unordered_map<uint64_t, size_t> g_by_path;      // full path hash -> g_loose index
std::unordered_map<uint64_t, std::vector<size_t>> g_by_folder;   // folder hash -> g_loose indices

std::mutex g_lock;
std::map<std::string, std::shared_ptr<const Patched>> g_patched;  // by RomFS path; null: unchanged
std::map<std::string, std::vector<uint64_t>> g_held;             // a neighbour archive's path hashes, by RomFS path

std::string u8(const fs::path& p) {
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

uint64_t fnv(const std::string& s) {
    uint64_t h = 0xCBF29CE484222645ull;
    for (unsigned char ch : s) h = (h ^ ch) * 0x100000001B3ull;
    return h;
}

std::vector<fs::path> subdirs(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) out.push_back(it->path());
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return u8(a.filename()) < u8(b.filename()); });
    return out;
}

bool same_title(const fs::path& dir) {
    std::string n = u8(dir.filename());
    if (n.size() != strlen(kTitle)) return false;
    for (size_t i = 0; i < n.size(); i++)
        if (tolower((unsigned char)n[i]) != kTitle[i]) return false;
    return true;
}

bool read_host(const fs::path& p, std::vector<uint8_t>* out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out->resize(n > 0 ? (size_t)n : 0);
    bool ok = n >= 0 && fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
void wr16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
void wr64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
uint64_t align16(uint64_t v) { return (v + 15) & ~15ull; }

constexpr uint64_t kHeader = 0x28, kFolderHeader = 16, kFolderEntry = 16, kRecord = 24;
constexpr uint32_t kVersion = 0x1000, kUnused = 0xCC, kLz4 = 2, kLevel = 9;

struct Entry {
    uint64_t path_hash, name_hash = 0;
    uint16_t level, compression;
    uint32_t size, stored;
    uint64_t offset;                                 // in the archive, or in the tail for a mod's
    bool ours = false;
};
struct Folder {
    uint64_t hash;
    std::vector<size_t> entries;                     // ids (indices in the entry list as read, then added ones)
};

// The path hashes a gfpak holds (its table of them), or none when it can't be read
std::vector<uint64_t> path_hashes(uint64_t size, const ReadFn& read) {
    uint8_t h[kHeader];
    if (size < kHeader || !read(0, h, kHeader) || memcmp(h, "GFLXPACK", 8)) return {};
    uint64_t n = rd32(h + 16), at = rd64(h + 32);
    if (n > (1u << 24) || at > size || n * 8 > size - at) return {};
    std::vector<uint8_t> table((size_t)(n * 8));
    if (n && !read(at, table.data(), table.size())) return {};
    std::vector<uint64_t> out((size_t)n);
    for (uint64_t i = 0; i < n; i++) out[i] = rd64(&table[i * 8]);
    return out;
}

// Builds the patched archive, or returns null (logging why when the archive can't be read)
std::shared_ptr<const Patched> build(const std::string& rel, uint64_t size, const ReadFn& read,
                                     const std::function<bool(const std::string&)>& in_romfs, const NeighboursFn& neighbours) {
    auto bad = [&](const char* why) -> std::shared_ptr<const Patched> {
        log("mods: %s isn't a gfpak the mods can change (%s); the game gets it as it is", rel.c_str(), why);
        return nullptr;
    };
    uint8_t h[kHeader];
    if (size < kHeader || !read(0, h, kHeader)) return bad("too short");
    if (memcmp(h, "GFLXPACK", 8) || rd32(h + 8) != kVersion || rd32(h + 12)) return bad("unknown header");
    uint64_t n = rd32(h + 16), nf = rd32(h + 20), rec = rd64(h + 24), hashes = rd64(h + 32);
    if (n > (1u << 24) || nf > (1u << 24) || rec > size || n * kRecord > size - rec || hashes > size ||
        n * 8 > size - hashes || kHeader + nf * 8 > size)
        return bad("tables out of range");
    // the tables, read at once (in the game's layout the records come last, after the folders)
    uint64_t head_len = std::max({rec + n * kRecord, hashes + n * 8, kHeader + nf * 8});
    std::vector<uint8_t> old((size_t)head_len);
    if (!read(0, old.data(), head_len)) return bad("unreadable tables");
    std::vector<Entry> entries((size_t)n);
    std::vector<Folder> folders((size_t)nf);
    std::vector<bool> placed((size_t)n);
    for (uint64_t f = 0; f < nf; f++) {
        uint64_t at = rd64(&old[kHeader + 8 * f]);
        uint8_t fh[kFolderHeader];
        if (at > size || kFolderHeader > size - at || !read(at, fh, kFolderHeader)) return bad("folder out of range");
        uint64_t count = rd32(fh + 8);
        if (count > n || count * kFolderEntry > size - at - kFolderHeader) return bad("folder out of range");
        std::vector<uint8_t> table((size_t)(count * kFolderEntry));
        if (count && !read(at + kFolderHeader, table.data(), table.size())) return bad("unreadable folder");
        folders[f].hash = rd64(fh);
        for (uint64_t i = 0; i < count; i++) {
            uint32_t index = rd32(&table[i * kFolderEntry + 8]);
            if (index >= n || placed[index]) return bad("a file in two folders");
            placed[index] = true;
            entries[index].name_hash = rd64(&table[i * kFolderEntry]);
            folders[f].entries.push_back(index);
        }
    }
    for (uint64_t i = 0; i < n; i++) {
        if (!placed[i]) return bad("a file in no folder");
        const uint8_t* r = &old[rec + i * kRecord];
        Entry& e = entries[i];
        e.path_hash = rd64(&old[hashes + i * 8]);
        e.level = rd16(r);
        e.compression = rd16(r + 2);
        e.size = rd32(r + 4);
        e.stored = rd32(r + 8);
        e.offset = rd64(r + 16);
        if (e.offset > size || e.stored > size - e.offset) return bad("an entry out of range");
    }

    // what the mods change: entries they replace, and files they add to its folders
    std::unordered_map<uint64_t, size_t> by_path;
    for (size_t i = 0; i < entries.size(); i++) by_path[entries[i].path_hash] = i;
    std::vector<std::pair<size_t, size_t>> changes;  // (entry id, g_loose index)
    for (size_t i = 0; i < entries.size(); i++) {
        auto it = g_by_path.find(entries[i].path_hash);
        if (it != g_by_path.end()) changes.push_back({i, it->second});
    }
    size_t replaced = changes.size();
    std::vector<size_t> order(entries.size()), pos_of;   // ids in the new archive's order, and back
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    auto reindex = [&] {
        pos_of.resize(entries.size());
        for (size_t i = 0; i < order.size(); i++) pos_of[order[i]] = i;
    };
    reindex();
    // the files the game has in this archive's neighbours, read the first time a mod file might be added
    std::unordered_set<uint64_t> beside;
    bool beside_read = false;
    auto held_beside = [&](uint64_t hash) {
        if (!beside_read) {
            beside_read = true;
            if (neighbours)
                for (const Neighbour& nb : neighbours(rel)) {
                    if (nb.rel == rel) continue;
                    auto known = g_held.find(nb.rel);
                    if (known == g_held.end()) known = g_held.emplace(nb.rel, path_hashes(nb.size, nb.read)).first;
                    beside.insert(known->second.begin(), known->second.end());
                }
        }
        return beside.count(hash) != 0;
    };
    size_t kept_out = 0;
    for (Folder& f : folders) {
        auto it = g_by_folder.find(f.hash);
        if (it == g_by_folder.end()) continue;
        for (size_t l : it->second) {
            if (by_path.count(g_loose[l].path_hash) || in_romfs(g_loose[l].rel)) continue;
            if (held_beside(g_loose[l].path_hash)) { kept_out++; continue; }   // another archive's file
            // after the folder's last file, so a folder's files stay together as the game's tool writes them
            size_t pos = f.entries.empty() ? order.size() : 0;
            for (size_t id : f.entries) pos = std::max(pos, pos_of[id] + 1);
            Entry e{};
            e.path_hash = g_loose[l].path_hash;
            e.name_hash = g_loose[l].name_hash;
            e.level = kLevel;
            entries.push_back(e);
            by_path[e.path_hash] = entries.size() - 1;
            order.insert(order.begin() + (ptrdiff_t)pos, entries.size() - 1);
            f.entries.push_back(entries.size() - 1);
            changes.push_back({entries.size() - 1, l});
            reindex();
        }
    }
    if (kept_out) RT_TRACE("mods: %s: %zu mod files of its folders are other archives' and stay out", rel.c_str(), kept_out);
    if (changes.empty()) return nullptr;

    // the mods' entries, LZ4 like the game's
    auto p = std::make_shared<Patched>();
    for (auto [id, l] : changes) {
        std::vector<uint8_t> content;
        const ModFile& m = g_files.at(g_loose[l].rel);
        if (!read_host(m.host, &content) || content.size() > 0xFFFFFFF0u) {
            log("mods: %s: can't read %s's %s; the game gets its own", rel.c_str(), g_names[m.mod].c_str(), g_loose[l].rel.c_str());
            if (id < n) continue;
            return nullptr;                          // an added file can't be left out once it has a place
        }
        std::vector<uint8_t> block = lz4_encode(content.data(), content.size());
        Entry& e = entries[id];
        e.compression = kLz4;
        e.size = (uint32_t)content.size();
        e.stored = (uint32_t)block.size();
        e.offset = p->tail.size();
        e.ours = true;
        p->tail.insert(p->tail.end(), block.begin(), block.end());
        p->tail.resize((size_t)align16(p->tail.size()));
        RT_TRACE("mods: %s: %s %s from %s", rel.c_str(), id < n ? "replaced" : "added", g_loose[l].rel.c_str(), g_names[m.mod].c_str());
    }

    // the new tables, in the game's layout
    uint64_t count = order.size();
    uint64_t hash_at = kHeader + 8 * nf, at = hash_at + 8 * count;
    std::vector<uint64_t> folder_at(folders.size());
    for (size_t f = 0; f < folders.size(); f++) {
        folder_at[f] = at;
        at += kFolderHeader + kFolderEntry * folders[f].entries.size();
    }
    uint64_t rec_at = at;
    at = align16(at + kRecord * count);

    // then the bytes of the archive's entries that stay, in their order, as runs of the archive copied as
    // they are (an entry joins the run before it when only padding is between them), each at the same
    // place within 16 bytes as in the archive; a replaced entry's bytes are left out
    std::vector<size_t> kept;
    for (size_t i = 0; i < n; i++)
        if (!entries[i].ours) kept.push_back(i);
    std::sort(kept.begin(), kept.end(), [&](size_t a, size_t b) { return entries[a].offset < entries[b].offset; });
    std::vector<uint64_t> moved(entries.size());
    const uint64_t tables_len = at;
    for (size_t k = 0; k < kept.size();) {
        uint64_t from = entries[kept[k]].offset, end = from + entries[kept[k]].stored;
        size_t j = k + 1;
        for (; j < kept.size() && entries[kept[j]].offset <= align16(end); j++)
            end = std::max(end, entries[kept[j]].offset + entries[kept[j]].stored);
        uint64_t run_at = align16(at) + (from & 15);
        for (size_t m = k; m < j; m++) moved[kept[m]] = run_at + (entries[kept[m]].offset - from);
        p->runs.push_back({run_at, from, end - from});
        at = run_at + (end - from);
        k = j;
    }
    p->tail_at = align16(at);
    p->size = p->tail_at + p->tail.size();

    std::vector<uint8_t>& o = p->head;
    o.assign((size_t)tables_len, 0);
    memcpy(o.data(), "GFLXPACK", 8);
    wr32(&o[8], kVersion);
    wr32(&o[16], (uint32_t)count);
    wr32(&o[20], (uint32_t)nf);
    wr64(&o[24], rec_at);
    wr64(&o[32], hash_at);
    std::vector<uint32_t> index_of(entries.size());
    for (size_t i = 0; i < order.size(); i++) {
        index_of[order[i]] = (uint32_t)i;
        const Entry& e = entries[order[i]];
        wr64(&o[hash_at + 8 * i], e.path_hash);
        uint8_t* r = &o[rec_at + kRecord * i];
        wr16(r, e.level);
        wr16(r + 2, e.compression);
        wr32(r + 4, e.size);
        wr32(r + 8, e.stored);
        wr32(r + 12, kUnused);
        wr64(r + 16, e.ours ? p->tail_at + e.offset : moved[order[i]]);
    }
    for (size_t f = 0; f < folders.size(); f++) {
        uint8_t* q = &o[folder_at[f]];
        wr64(&o[kHeader + 8 * f], folder_at[f]);
        wr64(q, folders[f].hash);
        wr32(q + 8, (uint32_t)folders[f].entries.size());
        wr32(q + 12, kUnused);
        for (size_t i = 0; i < folders[f].entries.size(); i++) {
            uint8_t* fe = q + kFolderHeader + kFolderEntry * i;
            wr64(fe, entries[folders[f].entries[i]].name_hash);
            wr32(fe + 8, index_of[folders[f].entries[i]]);
            wr32(fe + 12, kUnused);
        }
    }
    log("mods: %s: %zu %s replaced, %zu added (%llu bytes, was %llu)", rel.c_str(), replaced, replaced == 1 ? "file" : "files",
        changes.size() - replaced, (unsigned long long)p->size, (unsigned long long)size);
    activity("mods  %s", rel.c_str());
    return p;
}

}  // namespace

void init(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        log("mods: none (no folder %s)", u8(root).c_str());
        return;
    }
    std::vector<std::pair<std::string, fs::path>> mods;   // (name, its romfs folder)
    auto add = [&](const std::string& name, const fs::path& dir) {
        if (fs::is_directory(dir / "exefs", ec))
            log("mods: %s: its exefs/ isn't used (code changes are built in: hacks/)", name.c_str());
        if (fs::is_directory(dir / "romfs", ec)) mods.push_back({name, dir / "romfs"});
    };
    if (fs::is_directory(root / "romfs", ec)) {
        add(u8(root.filename()), root);
    } else {
        for (const fs::path& d : subdirs(root)) add(u8(d.filename()), d);
        for (const fs::path& contents : {root / "contents", root / "atmosphere" / "contents"})
            for (const fs::path& title : subdirs(contents)) {
                if (!same_title(title)) continue;
                add("atmosphere", title);
                for (const fs::path& d : subdirs(title)) add(u8(d.filename()), d);
            }
    }
    std::stable_sort(mods.begin(), mods.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [name, romfs] : mods) {
        size_t files = 0;
        for (auto it = fs::recursive_directory_iterator(romfs, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string rel = u8(it->path().lexically_relative(romfs));
            auto prev = g_files.find(rel);
            if (prev != g_files.end()) log("mods: %s: %s's is used instead of %s's", rel.c_str(), name.c_str(), g_names[prev->second.mod].c_str());
            g_files[rel] = {it->path(), g_names.size()};
            files++;
        }
        log("mods: %s (%zu files)", name.c_str(), files);
        g_names.push_back(name);
    }
    if (g_names.empty()) log("mods: none in %s", u8(root).c_str());
    for (auto& [rel, m] : g_files) {
        for (size_t s = rel.find('/'); s != std::string::npos; s = rel.find('/', s + 1)) g_dirs.insert(rel.substr(0, s));
        if (rel.compare(0, 4, "bin/")) continue;
        size_t slash = rel.rfind('/');
        g_loose.push_back({fnv(rel), fnv(rel.substr(slash + 1)), rel});
        g_by_path[g_loose.back().path_hash] = g_loose.size() - 1;
        g_by_folder[fnv(rel.substr(0, slash + 1))].push_back(g_loose.size() - 1);
    }
    for (auto& [hash, list] : g_by_folder)               // added in path order, whatever the map's order
        std::sort(list.begin(), list.end(), [](size_t a, size_t b) { return g_loose[a].rel < g_loose[b].rel; });
}

const fs::path* file(const std::string& rel, bool opened) {
    static const bool trace = getenv("LETSGO_FILES") != nullptr;   // every file the game opens, the first time
    if (trace && opened) {
        static std::unordered_set<std::string> seen;
        std::lock_guard<std::mutex> lk(g_lock);
        if (seen.insert(rel).second) log("file: %s", rel.c_str());
    }
    auto it = g_files.find(rel);
    if (it == g_files.end()) return nullptr;
    if (opened) {
        static std::unordered_set<std::string> told;
        std::lock_guard<std::mutex> lk(g_lock);
        if (told.insert(rel).second) log("mods: %s is %s's", rel.c_str(), g_names[it->second.mod].c_str());
    }
    return &it->second.host;
}

bool is_dir(const std::string& rel) { return g_dirs.count(rel) != 0; }

std::shared_ptr<const Patched> patch_archive(const std::string& rel, uint64_t size, const ReadFn& read,
                                             const std::function<bool(const std::string&)>& in_romfs, const NeighboursFn& neighbours) {
    if (g_loose.empty() || rel.size() < 6 || rel.compare(rel.size() - 6, 6, ".gfpak")) return nullptr;
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_patched.find(rel);
    if (it != g_patched.end()) return it->second;
    return g_patched[rel] = build(rel, size, read, in_romfs, neighbours);
}

uint64_t read(const Patched& p, uint64_t offset, void* out, uint64_t size, const ReadFn& read_base) {
    if (offset >= p.size) return 0;
    size = std::min(size, p.size - offset);
    uint8_t* o = (uint8_t*)out;
    for (uint64_t done = 0; done < size;) {
        uint64_t at = offset + done, want = size - done, k;
        if (at < p.head.size()) {
            k = std::min(want, p.head.size() - at);
            memcpy(o + done, &p.head[at], k);
        } else if (at >= p.tail_at) {
            k = std::min(want, p.size - at);
            memcpy(o + done, &p.tail[at - p.tail_at], k);
        } else {
            // in the run that starts at or before it, or in the zeros before the next run (or the tail)
            auto next = std::upper_bound(p.runs.begin(), p.runs.end(), at, [](uint64_t v, const Patched::Run& r) { return v < r.at; });
            if (next != p.runs.begin() && at < (next - 1)->at + (next - 1)->len) {
                const Patched::Run& r = *(next - 1);
                k = std::min(want, r.at + r.len - at);
                if (!read_base(r.from + (at - r.at), o + done, k)) return done;
            } else {
                k = std::min(want, (next == p.runs.end() ? p.tail_at : next->at) - at);
                memset(o + done, 0, k);
            }
        }
        done += k;
    }
    return size;
}

// A compressor in the way of LZ4's high-compression mode: at each place the longest match among up to 64
// earlier places with the same first four bytes (a hash chain over the last 64 KiB), taken one byte later
// when that one is longer by more than a byte. It keeps the block format's end rules (the last 5 bytes are
// literals, and the last match starts 12 or more bytes before the end), which decoders like the game's
// may rely on. The game's own entries come out about as small.
std::vector<uint8_t> lz4_encode(const uint8_t* in, size_t n) {
    std::vector<uint8_t> out(n + n / 255 + 16);
    size_t o = 0, anchor = 0, p = 0;
    const int64_t last_start = (int64_t)n - 12, match_end = (int64_t)n - 5;
    constexpr int kHashBits = 16, kAttempts = 64;
    constexpr size_t kWindow = 65535;
    std::vector<int64_t> head(1 << kHashBits, -1), chain(n, -1);
    auto u32 = [&](size_t i) { uint32_t v; memcpy(&v, in + i, 4); return v; };
    auto hash = [&](size_t i) { return (u32(i) * 2654435761u) >> (32 - kHashBits); };
    size_t inserted = 0;                             // places before this are in the chains
    auto longest = [&](size_t at, size_t* from) {
        for (; inserted < at; inserted++) {
            uint32_t h = hash(inserted);
            chain[inserted] = head[h];
            head[h] = (int64_t)inserted;
        }
        size_t best = 0;
        int64_t c = head[hash(at)];
        for (int tries = 0; tries < kAttempts && c >= 0 && at - (size_t)c <= kWindow; tries++, c = chain[(size_t)c]) {
            if (u32((size_t)c) != u32(at)) continue;
            size_t len = 4;
            while ((int64_t)(at + len) < match_end && in[(size_t)c + len] == in[at + len]) len++;
            if (len > best) {
                best = len;
                *from = (size_t)c;
            }
        }
        return best;
    };
    auto length = [&](size_t rest) {
        for (; rest >= 255; rest -= 255) out[o++] = 255;
        out[o++] = (uint8_t)rest;
    };
    auto literals = [&](size_t count, size_t match_code) {
        out[o++] = (uint8_t)((std::min<size_t>(count, 15) << 4) | std::min<size_t>(match_code, 15));
        if (count >= 15) length(count - 15);
        memcpy(&out[o], in + anchor, count);
        o += count;
    };
    while ((int64_t)p <= last_start) {
        size_t from = 0, len = longest(p, &from);
        if (len < 4) {
            p++;
            continue;
        }
        size_t later_from = 0;
        if ((int64_t)(p + 1) <= last_start && longest(p + 1, &later_from) > len + 1) {
            p++;                                     // the match one byte on is better: this byte is a literal
            continue;
        }
        literals(p - anchor, len - 4);
        out[o++] = (uint8_t)((p - from) & 255);
        out[o++] = (uint8_t)((p - from) >> 8);
        if (len - 4 >= 15) length(len - 4 - 15);
        p += len;
        anchor = p;
    }
    literals(n - anchor, 0);
    out.resize(o);
    return out;
}

}  // namespace rt::mods
