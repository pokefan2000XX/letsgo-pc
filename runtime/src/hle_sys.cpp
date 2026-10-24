// Host overrides for service-backed nn:: APIs: fs, oe (applet), account,
// time, vi (display handles), nifm (network: offline), err.
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "hle.h"
#include "mods.h"
#include "nx.h"

namespace rt {

// ---- small helpers ------------------------------------------------------------------------
static std::mutex g_galloc_lock;
static uint64_t g_galloc_next = kAliasBase + 0x10000;    // first 64 KiB: nnosInitialize argument buffer
uint64_t guest_alloc(uint64_t size) {
    std::lock_guard<std::mutex> lk(g_galloc_lock);
    uint64_t a = (g_galloc_next + 15) & ~15ull;
    g_galloc_next = a + size;
    mem_commit(a, size);
    mem_track(a & ~0xFFFull, ((a + size + 0xFFF) & ~0xFFFull) - (a & ~0xFFFull), kMemAlias, 3);
    memset((void*)a, 0, size);
    return a;
}
static const char* cstr(uint64_t p) { return (const char*)(uintptr_t)p; }

// ---- nn::fs -------------------------------------------------------------------------------
namespace fsx {
constexpr uint32_t kPathNotFound = 0x202, kPathAlreadyExists = 0x402, kOutOfRange = 0x1B4202;
struct File {
    FILE* f;
    std::string path;
    bool romfs = false;                 // a RomFS file (read-only): the game's or a mod's
    bool rom = false;                   // read from the player's NSPs (g_game)
    std::string rom_path;
    uint64_t size = 0;                  // a RomFS file's, as the game sees it
    uint64_t base_size = 0;             // the file a patched archive is made from
    std::shared_ptr<const mods::Patched> patched;   // a gfpak the mods change (mods.h)
};
// the RomFS mount when the game comes from the player's NSPs: paths under it go to g_game
static const std::string kRomMount = "\x01rom";
static std::mutex lock;
static std::unordered_map<std::string, std::string> mounts;   // "rom" -> host dir
static std::unordered_set<std::string> rom_mounts;            // the mounts that are the RomFS
static std::unordered_map<uint64_t, File> files;
static uint64_t next_handle = 0x1000;
std::string g_orig = "orig";
std::string g_save = "save";

static std::string host_path(const char* p) {
    std::string s = p;
    auto c = s.find(":/");
    if (c == std::string::npos) return {};
    std::string m = s.substr(0, c);
    auto it = mounts.find(m);
    if (it == mounts.end()) return {};
    return it->second + "/" + s.substr(c + 2);
}

// for other host overrides (movie::Extractor::SetDataSource): "" if no mount matches
std::string host_path_of(const char* guest) {
    std::lock_guard<std::mutex> lk(lock);
    return host_path(guest);
}

// the RomFS path ("bin/...") of a path under a RomFS mount
static bool rom_rel_of(const char* guest, std::string* rel) {
    std::string s = guest;
    size_t c = s.find(":/");
    if (c == std::string::npos || !rom_mounts.count(s.substr(0, c))) return false;
    size_t i = c + 2;
    while (i < s.size() && s[i] == '/') i++;
    *rel = s.substr(i);
    return true;
}

static void MountRom(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    rom_mounts.insert(cstr(c->x[0]));
    if (g_game) {
        mounts[cstr(c->x[0])] = kRomMount;
        log("fs: MountRom(\"%s\") -> the game's NSPs", cstr(c->x[0]));
    } else {
        mounts[cstr(c->x[0])] = g_orig + "/romfs";
        log("fs: MountRom(\"%s\") -> %s/romfs", cstr(c->x[0]), g_orig.c_str());
    }
    c->x[0] = 0;
}

// the bytes of the file under a RomFS file: from the player's NSPs, or a host file
static uint64_t base_read(const File& f, uint64_t off, void* out, uint64_t size) {
    if (off >= f.base_size) return 0;
    uint64_t n = std::min(size, f.base_size - off);
    if (f.rom) return g_game->read(f.rom_path, off, out, n) ? n : 0;
    _fseeki64(f.f, (int64_t)off, SEEK_SET);
    return fread(out, 1, (size_t)n, f.f);
}
// A RomFS file: a mod's when one has it, else the game's (the player's NSPs, or orig/romfs), with the
// mods' files put in when it's a gfpak they change
static bool open_rom(const char* guest, const std::string& rel, File* fe) {
    fe->romfs = true;
    if (const std::filesystem::path* m = mods::file(rel, true)) {
        fe->path = rel;
        fe->f = _wfopen(m->c_str(), L"rb");
    } else if (g_game) {
        if (!g_game->file_size(rel, &fe->base_size)) return false;
        fe->rom = true;
        fe->rom_path = rel;
    } else {
        fe->path = host_path(guest);
        fe->f = fopen(fe->path.c_str(), "rb");
    }
    if (!fe->rom) {
        if (!fe->f) return false;
        _fseeki64(fe->f, 0, SEEK_END);
        fe->base_size = (uint64_t)_ftelli64(fe->f);
    }
    fe->size = fe->base_size;
    auto in_romfs = [](const std::string& path) {         // the game's own files, not the mods'
        uint64_t size;
        return g_game ? g_game->file_size(path, &size) : std::filesystem::is_regular_file(g_orig + "/romfs/" + path);
    };
    // the archives beside it that can share its folders: the others in its folder, the one named after its
    // folder one level up (bin/archive/field/area02.gfpak beside bin/archive/field/area02/), and those in
    // the folder named after it; the game's own (a mod's archive of the same path takes their place)
    auto neighbours = [](const std::string& rel) {
        std::vector<mods::Neighbour> out;
        auto gfpaks_in = [](const std::string& dir) {
            std::vector<std::string> found;
            if (g_game) {
                for (const std::string& p : g_game->list(dir))
                    if (p.size() > 6 && p.compare(p.size() - 6, 6, ".gfpak") == 0) found.push_back(p);
            } else {
                std::error_code ec;
                for (auto it = std::filesystem::directory_iterator(g_orig + "/romfs/" + dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
                    if (it->is_regular_file(ec) && it->path().extension() == ".gfpak") found.push_back(dir + "/" + it->path().filename().string());
            }
            return found;
        };
        auto add = [&](const std::string& path) {
            uint64_t size = 0;
            if (path == rel) return;
            if (g_game) {
                if (!g_game->file_size(path, &size)) return;
                out.push_back({path, size, [path](uint64_t off, void* p, uint64_t n) { return g_game->read(path, off, p, n); }});
            } else {
                std::string host = g_orig + "/romfs/" + path;
                std::error_code ec;
                size = std::filesystem::file_size(host, ec);
                if (ec) return;
                out.push_back({path, size, [host](uint64_t off, void* p, uint64_t n) {
                    FILE* f = fopen(host.c_str(), "rb");
                    if (!f) return false;
                    _fseeki64(f, (int64_t)off, SEEK_SET);
                    bool ok = fread(p, 1, (size_t)n, f) == n;
                    fclose(f);
                    return ok;
                }});
            }
        };
        size_t slash = rel.rfind('/');
        std::string dir = slash == std::string::npos ? "" : rel.substr(0, slash);
        for (const std::string& p : gfpaks_in(dir)) add(p);
        if (!dir.empty()) add(dir + ".gfpak");
        for (const std::string& p : gfpaks_in(rel.substr(0, rel.size() - 6))) add(p);
        return out;
    };
    fe->patched = mods::patch_archive(rel, fe->base_size, [fe](uint64_t off, void* out, uint64_t n) { return base_read(*fe, off, out, n) == n; },
                                      in_romfs, neighbours);
    if (fe->patched) fe->size = fe->patched->size;
    return true;
}
// Reads an open file; from the game's NSPs outside the lock (they're positional and slow next to
// everything else), host files under it
static uint64_t read_at(File* f, uint64_t off, void* out, uint64_t size, std::unique_lock<std::mutex>& lk) {
    if (!f->romfs) {
        _fseeki64(f->f, (int64_t)off, SEEK_SET);
        return fread(out, 1, (size_t)size, f->f);
    }
    File copy = *f;
    if (copy.rom) lk.unlock();
    if (!copy.patched) return base_read(copy, off, out, size);
    return mods::read(*copy.patched, off, out, size, [&](uint64_t o, void* p, uint64_t n) { return base_read(copy, o, p, n) == n; });
}

// a whole file, wherever it is (for host code like the movie library): false if it doesn't exist
bool read_whole_file(const char* guest, std::vector<uint8_t>* out) {
    std::unique_lock<std::mutex> lk(lock);
    std::string rel;
    if (rom_rel_of(guest, &rel)) {
        File f{};
        if (!open_rom(guest, rel, &f)) return false;
        out->resize((size_t)f.size);
        bool ok = read_at(&f, 0, out->data(), f.size, lk) == f.size;
        if (f.f) fclose(f.f);
        return ok;
    }
    std::string hp = host_path(guest);
    lk.unlock();
    if (hp.empty()) hp = guest;
    FILE* f = fopen(hp.c_str(), "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    out->resize((size_t)_ftelli64(f));
    _fseeki64(f, 0, SEEK_SET);
    bool ok = fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}
static void QueryMountRomCacheSize(Ctx* c) { *(uint64_t*)c->x[0] = 0x1000; c->x[0] = 0; }
static void MountSaveData(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    std::filesystem::create_directories(g_save);
    mounts[cstr(c->x[0])] = g_save;
    log("fs: MountSaveData(\"%s\") -> %s", cstr(c->x[0]), g_save.c_str());
    c->x[0] = 0;
}
static void Unmount(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    mounts.erase(cstr(c->x[0]));
    rom_mounts.erase(cstr(c->x[0]));
}
// The last RomFS file this thread looked for and didn't find. The game goes on without checking for some
// files (a species' learnset, wot_NNN.bin, when a trainer's team has a Pokémon Let's Go doesn't have) and
// then uses a handle it never got, so the bad-handle error names it.
static thread_local std::string t_missing;
static void OpenFile(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    int mode = (int)c->x[2];
    std::string rel;
    if (rom_rel_of(cstr(c->x[1]), &rel)) {   // the game's own files (and the mods'): read-only
        File fe{};
        bool found = !(mode & 2) && open_rom(cstr(c->x[1]), rel, &fe);
        RT_TRACE("fs: OpenFile(\"%s\", %d) -> %s", cstr(c->x[1]), mode, found ? "ok" : "not found");
        if (!found) {
            if (fe.f) fclose(fe.f);
            t_missing = rel;
            c->x[0] = kPathNotFound;
            return;
        }
        activity("load  %s", rel.c_str());
        uint64_t h = next_handle++;
        files[h] = std::move(fe);
        *(uint64_t*)c->x[0] = h;
        c->x[0] = 0;
        return;
    }
    std::string hp = host_path(cstr(c->x[1]));
    FILE* f = hp.empty() ? nullptr : fopen(hp.c_str(), (mode & 2) ? "r+b" : "rb");
    RT_TRACE("fs: OpenFile(\"%s\", %d) -> %s", cstr(c->x[1]), mode, f ? "ok" : "not found");
    if (!f) { c->x[0] = kPathNotFound; return; }
    activity("%s %s", (mode & 2) ? "write" : "load ", strncmp(cstr(c->x[1]), "data:/", 6) ? cstr(c->x[1]) : cstr(c->x[1]) + 6);
    uint64_t h = next_handle++;
    files[h] = {f, hp};
    *(uint64_t*)c->x[0] = h;
    c->x[0] = 0;
}
static File* file(uint64_t h) {
    auto it = files.find(h);
    if (it == files.end()) {
        if (t_missing.empty()) fatal("fs: bad file handle 0x%llx", (unsigned long long)h);
        fatal("The game needed %s, which isn't in the game's files or the mods (a Pokemon, move or item Let's Go "
              "doesn't have can cause this).\n\nfs: bad file handle 0x%llx", t_missing.c_str(), (unsigned long long)h);
    }
    return &it->second;
}
static void CloseFile(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    File* f = file(c->x[0]);
    if (f->f) fclose(f->f);
    files.erase(c->x[0]);
}
static void GetFileSize(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    File* f = file(c->x[1]);
    if (f->romfs) {
        *(int64_t*)c->x[0] = (int64_t)f->size;
        c->x[0] = 0;
        return;
    }
    _fseeki64(f->f, 0, SEEK_END);
    *(int64_t*)c->x[0] = _ftelli64(f->f);
    c->x[0] = 0;
}
// ReadFile(FileHandle, long offset, void*, size_t)
static void ReadFile(Ctx* c) {
    std::unique_lock<std::mutex> lk(lock);
    uint64_t n = read_at(file(c->x[0]), c->x[1], (void*)c->x[2], c->x[3], lk);
    c->x[0] = n == c->x[3] ? 0 : kOutOfRange;
}
// ReadFile(size_t* out, FileHandle, long offset, void*, size_t)
static void ReadFile2(Ctx* c) {
    std::unique_lock<std::mutex> lk(lock);
    *(uint64_t*)c->x[0] = read_at(file(c->x[1]), c->x[2], (void*)c->x[3], c->x[4], lk);
    c->x[0] = 0;
}
// WriteFile(FileHandle, long offset, void const*, size_t, WriteOption const&)
static void WriteFile(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    File* f = file(c->x[0]);
    if (f->romfs) { c->x[0] = kPathNotFound; return; }
    _fseeki64(f->f, (int64_t)c->x[1], SEEK_SET);
    fwrite((const void*)c->x[2], 1, (size_t)c->x[3], f->f);
    c->x[0] = 0;
}
static void FlushFile(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    File* f = file(c->x[0]);
    if (f->f) fflush(f->f);
    c->x[0] = 0;
}
static void SetFileSize(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    File* f = file(c->x[0]);
    if (f->romfs) { c->x[0] = kPathNotFound; return; }
    fflush(f->f);
    _chsize_s(_fileno(f->f), (int64_t)c->x[1]);
    c->x[0] = 0;
}
static void CreateFile(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    std::string hp = host_path(cstr(c->x[0])), rel;
    if (hp.empty() || rom_rel_of(cstr(c->x[0]), &rel)) { c->x[0] = kPathNotFound; return; }
    if (std::filesystem::exists(hp)) { c->x[0] = kPathAlreadyExists; return; }
    std::filesystem::create_directories(std::filesystem::path(hp).parent_path());
    FILE* f = fopen(hp.c_str(), "wb");
    if (f) { _chsize_s(_fileno(f), (int64_t)c->x[1]); fclose(f); }
    c->x[0] = f ? 0 : kPathNotFound;
}
static void DeleteFile(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    std::string hp = host_path(cstr(c->x[0])), rel;
    c->x[0] = (!hp.empty() && !rom_rel_of(cstr(c->x[0]), &rel) && std::filesystem::remove(hp)) ? 0 : kPathNotFound;
}
// GetEntryType(DirectoryEntryType*, char const*): 0 = directory, 1 = file
static void GetEntryType(Ctx* c) {
    std::lock_guard<std::mutex> lk(lock);
    std::string hp = host_path(cstr(c->x[1])), rel;
    if (rom_rel_of(cstr(c->x[1]), &rel)) {   // a mod's, then the game's (orig/romfs is a host folder, below)
        uint64_t size;
        int type = mods::file(rel) ? 1 : mods::is_dir(rel) ? 0 : -1;
        if (type < 0 && g_game) type = g_game->is_dir(rel) ? 0 : g_game->file_size(rel, &size) ? 1 : -1;
        if (type >= 0 || g_game) {
            if (type < 0) { c->x[0] = kPathNotFound; return; }
            *(uint32_t*)c->x[0] = (uint32_t)type;
            c->x[0] = 0;
            return;
        }
    }
    std::error_code ec;
    auto st = std::filesystem::status(hp, ec);
    if (hp.empty() || ec || !std::filesystem::exists(st)) { c->x[0] = kPathNotFound; return; }
    *(uint32_t*)c->x[0] = std::filesystem::is_directory(st) ? 0 : 1;
    c->x[0] = 0;
}
static void EnsureSaveData(Ctx* c) { std::filesystem::create_directories(g_save); c->x[0] = 0; }
static void IsSaveDataExisting(Ctx* c) { c->x[0] = std::filesystem::exists(g_save) ? 1 : 0; }
static void CommitSaveData(Ctx* c) { c->x[0] = 0; }
}  // namespace fsx

// ---- nn::oe (applet) --------------------------------------------------------------------------
static uint64_t lang_code(const char* s) {
    uint64_t v = 0;
    memcpy(&v, s, strlen(s) < 8 ? strlen(s) : 8);
    return v;
}
// Docked (Console), so the game renders at 1920x1080 as it does on a TV; LETSGO_HANDHELD=1 gives the handheld's 1280x720
static bool handheld() {
    static const bool h = getenv("LETSGO_HANDHELD") != nullptr;
    return h;
}
static void oe_GetOperationMode(Ctx* c) { c->x[0] = handheld() ? 0 : 1; }      // Handheld / Console
static void oe_GetPerformanceMode(Ctx* c) { c->x[0] = handheld() ? 0 : 1; }    // Normal / Boost
static void oe_GetDesiredLanguage(Ctx* c) { c->x[0] = lang_code("en-US"); }
static void oe_GetCurrentFocusState(Ctx* c) { c->x[0] = 1; }      // InFocus
static void oe_TryPopNotificationMessage(Ctx* c) { c->x[0] = 0; }
static void oe_GetExpectedVolumeBalance(Ctx* c) { *(float*)c->x[0] = 1.0f; *(float*)c->x[1] = 1.0f; }
static void oe_GetPseudoDeviceId(Ctx* c) {
    static const uint8_t id[16] = {0x4c, 0x47, 0x50, 0x43, 0x2d, 0x6c, 0x65, 0x74, 0x73, 0x67, 0x6f, 0x2d, 0x70, 0x63, 0, 1};
    memcpy((void*)c->x[0], id, 16);
}

// ---- nn::settings ---------------------------------------------------------------------------------
// LanguageCode::Make(Language): the system's fixed language-code table
static void settings_MakeLanguageCode(Ctx* c) {
    static const char* codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                  "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    uint32_t lang = (uint32_t)c->x[0];
    c->x[0] = lang < sizeof codes / sizeof *codes ? lang_code(codes[lang]) : lang_code("en-US");
}

// ---- nn::account ------------------------------------------------------------------------------
static const uint64_t kUid[2] = {0x4C47504350430001ull, 0x0000000000000001ull};
static void acc_OpenPreselectedUser(Ctx* c) { uint64_t* h = (uint64_t*)c->x[0]; h[0] = 1; h[1] = kUid[0]; h[2] = kUid[1]; c->x[0] = 0; }
static void acc_OpenUser(Ctx* c) { uint64_t* h = (uint64_t*)c->x[0]; h[0] = 1; h[1] = kUid[0]; h[2] = kUid[1]; c->x[0] = 0; }
static void acc_GetUserId(Ctx* c) { memcpy((void*)c->x[0], kUid, 16); c->x[0] = 0; }
static void acc_GetNickname(Ctx* c) { memset((void*)c->x[0], 0, 33); strcpy((char*)c->x[0], "Player"); c->x[0] = 0; }
static void acc_GetUserExistence(Ctx* c) { *(uint8_t*)c->x[0] = 1; c->x[0] = 0; }
static void acc_ShowUserSelector(Ctx* c) { memcpy((void*)c->x[0], kUid, 16); c->x[0] = 0; }
static void acc_NetworkUnavailable(Ctx* c) { c->x[0] = 0x2027C; }  // account: network service account unavailable

// ---- nn::time ---------------------------------------------------------------------------------
static int64_t now_posix() { return (int64_t)time(nullptr); }
static void time_ok(Ctx* c) { c->x[0] = 0; }
static void time_IsInitialized(Ctx* c) { c->x[0] = 1; }
static void time_GetCurrentTime(Ctx* c) { *(int64_t*)c->x[0] = now_posix(); c->x[0] = 0; }
static const uint8_t kSteadySource[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static void time_GetCurrentTimePoint(Ctx* c) {
    uint8_t* p = (uint8_t*)c->x[0];
    int64_t v = (int64_t)(ticks() / 19200000);
    memcpy(p, &v, 8);
    memcpy(p + 8, kSteadySource, 16);
    c->x[0] = 0;
}
static void time_GetSpanBetween(Ctx* c) {
    int64_t a, b;
    memcpy(&a, (void*)c->x[1], 8);
    memcpy(&b, (void*)c->x[2], 8);
    *(int64_t*)c->x[0] = b - a;
    c->x[0] = 0;
}
static uint64_t pack_calendar(const struct tm& t) {
    struct { int16_t year; int8_t month, day, hour, minute, second, pad; } ct = {
        (int16_t)(t.tm_year + 1900), (int8_t)(t.tm_mon + 1), (int8_t)t.tm_mday, (int8_t)t.tm_hour,
        (int8_t)t.tm_min, (int8_t)t.tm_sec, 0};
    uint64_t v;
    memcpy(&v, &ct, 8);
    return v;
}
// ToCalendarTime(CalendarTime*, CalendarAdditionalInfo*, PosixTime const&)
static void time_ToCalendarTime(Ctx* c) {
    time_t t = (time_t)*(int64_t*)c->x[2];
    struct tm lt;
    localtime_s(&lt, &t);
    uint64_t v = pack_calendar(lt);
    memcpy((void*)c->x[0], &v, 8);
    if (c->x[1]) {
        uint8_t* ai = (uint8_t*)c->x[1];
        memset(ai, 0, 0x18);
        int32_t dow = lt.tm_wday, doy = lt.tm_yday;
        memcpy(ai, &dow, 4);
        memcpy(ai + 4, &doy, 4);
        memcpy(ai + 8, "UTC", 3);
    }
    c->x[0] = 0;
}
static void time_ToCalendarTimeInUtc(Ctx* c) {
    time_t t = (time_t)*(int64_t*)c->x[0];
    struct tm ut;
    gmtime_s(&ut, &t);
    c->x[0] = pack_calendar(ut);
}
static int64_t calendar_to_posix(const uint8_t* p, bool utc) {
    int16_t year;
    memcpy(&year, p, 2);
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = (int8_t)p[2] - 1;
    t.tm_mday = (int8_t)p[3];
    t.tm_hour = (int8_t)p[4];
    t.tm_min = (int8_t)p[5];
    t.tm_sec = (int8_t)p[6];
    return utc ? (int64_t)_mkgmtime(&t) : (int64_t)mktime(&t);
}
static void time_ToPosixTimeFromUtc(Ctx* c) { c->x[0] = (uint64_t)calendar_to_posix((const uint8_t*)c->x[0], true); }
// TimeZoneRule (0x4000 bytes, opaque to the game): we keep the location name it was loaded for.
// Zones with no offset ("GMT", "UTC", "Etc/UTC", ...) convert as UTC, anything else as the PC's zone.
// The game turns local wall time into a "GMT" posix time this way and shows it with
// ToCalendarTimeInUtc, so the GMT rule must not apply the PC's offset.
static bool rule_is_utc(const char* name) {
    for (const char* z : {"GMT", "UTC", "UCT", "Zulu", "Universal", "Greenwich", "Etc/GMT", "Etc/UTC", "Etc/UCT",
                          "Etc/Zulu", "Etc/Universal", "Etc/Greenwich", "Etc/GMT0", "Etc/GMT+0", "Etc/GMT-0", "GMT0"})
        if (!strcmp(name, z)) return true;
    return false;
}
// LoadTimeZoneRule(TimeZoneRule*, LocationName const&)
static void time_LoadTimeZoneRule(Ctx* c) {
    char* rule = (char*)c->x[0];
    memset(rule, 0, 0x24);
    strncpy(rule, (const char*)c->x[1], 0x23);
    c->x[0] = 0;
}
// ToPosixTime(int* count, PosixTime* out, int outCount, CalendarTime const&, TimeZoneRule const&)
static void time_ToPosixTime(Ctx* c) {
    *(int32_t*)c->x[0] = 1;
    *(int64_t*)c->x[1] = calendar_to_posix((const uint8_t*)c->x[3], rule_is_utc((const char*)c->x[4]));
    c->x[0] = 0;
}
static void time_zero_span(Ctx* c) { c->x[0] = 0; }

// ---- nn::vi ---------------------------------------------------------------------------------
static uint64_t g_display, g_layer, g_native_window;
static void vi_OpenDefaultDisplay(Ctx* c) {
    if (!g_display) g_display = guest_alloc(0x100);
    *(uint64_t*)c->x[0] = g_display;
    c->x[0] = 0;
}
static void vi_CreateLayer(Ctx* c) {
    if (!g_layer) g_layer = guest_alloc(0x100);
    *(uint64_t*)c->x[0] = g_layer;
    c->x[0] = 0;
}
static void vi_GetNativeWindow(Ctx* c) {
    if (!g_native_window) g_native_window = guest_alloc(0x100);
    *(uint64_t*)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// ---- nn::pl (system shared fonts, extracted from firmware by tools/extract_fonts.py) ---------------
static std::mutex g_font_lock;
static uint64_t g_font_addr[8], g_font_size[8];
// A font collection's first font as a standalone TrueType file (its tables copied after a fresh directory)
static std::vector<uint8_t> first_font_of(const std::vector<uint8_t>& f) {
    auto be32 = [&](size_t o) { return o + 4 <= f.size() ? (uint32_t)f[o] << 24 | f[o + 1] << 16 | f[o + 2] << 8 | f[o + 3] : 0u; };
    if (f.size() < 12 || memcmp(f.data(), "ttcf", 4)) return f;
    uint32_t dir = be32(12);
    uint32_t n = dir + 6 <= f.size() ? (uint32_t)(f[dir + 4] << 8 | f[dir + 5]) : 0;
    if (!n || dir + 12 + 16ull * n > f.size()) return {};
    std::vector<uint8_t> out(f.begin() + dir, f.begin() + dir + 12 + 16 * n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = be32(dir + 12 + 16 * i + 8), len = be32(dir + 12 + 16 * i + 12);
        if ((uint64_t)off + len > f.size()) return {};
        while (out.size() % 4) out.push_back(0);
        uint32_t at = (uint32_t)out.size();
        for (int k = 0; k < 4; k++) out[12 + 16 * i + 8 + k] = (uint8_t)(at >> (24 - 8 * k));
        out.insert(out.end(), f.begin() + off, f.begin() + off + len);
    }
    return out;
}

// The Switch's system fonts. From orig/fonts (extracted from firmware) when there is one; otherwise
// Windows' own fonts for the same scripts, so nothing of the console's is needed.
static std::vector<uint8_t> system_font(uint32_t type) {
    std::vector<uint8_t> data;
    if (!g_game) {
        std::string path = fsx::g_orig + "/fonts/" + std::to_string(type) + ".ttf";
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) fatal("shared font %u missing (%s): run tools/extract_fonts.py", type, path.c_str());
        fseek(f, 0, SEEK_END);
        data.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), f) != data.size()) fatal("short read on %s", path.c_str());
        fclose(f);
        return data;
    }
    static const char* const kCandidates[6][4] = {
        {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc", "arial.ttf"},   // standard (Japanese, Latin)
        {"msyh.ttc", "simsun.ttc", "arial.ttf", nullptr},             // Chinese (simplified)
        {"msyh.ttc", "simsun.ttc", "arial.ttf", nullptr},             // Chinese (simplified), extension
        {"msjh.ttc", "mingliub.ttc", "msyh.ttc", "arial.ttf"},        // Chinese (traditional)
        {"malgun.ttf", "gulim.ttc", "arial.ttf", nullptr},            // Korean
        {"seguisym.ttf", "arial.ttf", nullptr, nullptr},              // Nintendo's symbols
    };
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, MAX_PATH);
    for (const char* name : kCandidates[type < 6 ? type : 0]) {
        if (!name) break;
        std::string path = std::string(windir) + "\\Fonts\\" + name;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        _fseeki64(f, 0, SEEK_END);
        std::vector<uint8_t> raw((size_t)_ftelli64(f));
        _fseeki64(f, 0, SEEK_SET);
        bool ok = fread(raw.data(), 1, raw.size(), f) == raw.size();
        fclose(f);
        if (ok) data = first_font_of(raw);
        if (!data.empty()) {
            log("pl: system font %u is Windows' %s", type, name);
            return data;
        }
    }
    fatal("Windows has no font for the game's system font %u (looked in %s\\Fonts).", type, windir);
}

static void load_font(uint32_t type) {
    std::lock_guard<std::mutex> lk(g_font_lock);
    if (type >= 8 || g_font_addr[type]) return;
    std::vector<uint8_t> data = system_font(type);
    uint64_t a = guest_alloc((uint64_t)data.size() + 16);
    memcpy((void*)a, data.data(), data.size());
    g_font_addr[type] = a;
    g_font_size[type] = (uint64_t)data.size();
    log("pl: shared font %u loaded (%zu bytes)", type, data.size());
}
static void pl_GetLoadState(Ctx* c) { c->x[0] = 1; }              // Loaded
static void pl_GetAddress(Ctx* c) { load_font((uint32_t)c->x[0]); c->x[0] = g_font_addr[(uint32_t)c->x[0] & 7]; }
static void pl_GetSize(Ctx* c) { load_font((uint32_t)c->x[0]); c->x[0] = g_font_size[(uint32_t)c->x[0] & 7]; }

// ---- nn::crypto: system RNG ---------------------------------------------------------------------------
static void crypto_RandomBytes(Ctx* c) {
    BCryptGenRandom(nullptr, (PUCHAR)(uintptr_t)c->x[0], (ULONG)c->x[1], BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

// ---- nn::nifm / nn::err ---------------------------------------------------------------------------
static void nifm_false(Ctx* c) { c->x[0] = 0; }
static void nifm_unavailable(Ctx* c) { c->x[0] = 0x46E; }          // nifm: network not available
static void err_show(Ctx* c) { log("nn::err: game asked to show an error (0x%llx)", (unsigned long long)c->x[0]); }

void register_sys_overrides(void (*reg)(const char*, rc_fn)) {
    using namespace fsx;
    reg("_ZN2nn2fs8MountRomEPKcPvm", MountRom);
    reg("_ZN2nn2fs22QueryMountRomCacheSizeEPm", QueryMountRomCacheSize);
    reg("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", MountSaveData);
    reg("_ZN2nn2fs7UnmountEPKc", Unmount);
    reg("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", OpenFile);
    reg("_ZN2nn2fs9CloseFileENS0_10FileHandleE", CloseFile);
    reg("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", GetFileSize);
    reg("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", ReadFile);
    reg("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", ReadFile2);
    reg("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", WriteFile);
    reg("_ZN2nn2fs9FlushFileENS0_10FileHandleE", FlushFile);
    reg("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", SetFileSize);
    reg("_ZN2nn2fs10CreateFileEPKcl", CreateFile);
    reg("_ZN2nn2fs10DeleteFileEPKc", DeleteFile);
    reg("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", GetEntryType);
    reg("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", EnsureSaveData);
    reg("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", IsSaveDataExisting);
    reg("_ZN2nn2fs14CommitSaveDataEPKc", CommitSaveData);
    reg("_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", hle_void);

    reg("_ZN2nn2oe16GetOperationModeEv", oe_GetOperationMode);
    reg("_ZN2nn2oe18GetPerformanceModeEv", oe_GetPerformanceMode);
    reg("_ZN2nn2oe18GetDesiredLanguageEv", oe_GetDesiredLanguage);
    reg("_ZN2nn2oe20GetCurrentFocusStateEv", oe_GetCurrentFocusState);
    reg("_ZN2nn2oe25TryPopNotificationMessageEPj", oe_TryPopNotificationMessage);
    reg("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", oe_GetExpectedVolumeBalance);
    reg("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", oe_GetPseudoDeviceId);
    for (const char* s : {"_ZN2nn2oe17FinishStartupLogoEv", "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",
                          "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",
                          "_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",
                          "_ZN2nn2oe28SetResumeNotificationEnabledEb",
                          "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb",
                          "_ZN2nn2oe21SetExpectedThemeColorENS0_14ThemeColorTypeE",
                          "_ZN2nn2oe24SetExpectedVolumeBalanceEff"})
        reg(s, hle_void);

    reg("_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", settings_MakeLanguageCode);
    reg("_ZN2nn7account10InitializeEv", hle_void);
    reg("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", acc_OpenPreselectedUser);
    reg("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", acc_OpenUser);
    reg("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", acc_GetUserId);
    reg("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", acc_GetNickname);
    reg("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", acc_GetUserExistence);
    reg("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", acc_ShowUserSelector);
    reg("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", acc_NetworkUnavailable);

    reg("_ZN2nn4time10InitializeEv", time_ok);
    reg("_ZN2nn4time8FinalizeEv", time_ok);
    reg("_ZN2nn4time13IsInitializedEv", time_IsInitialized);
    reg("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_GetCurrentTime);
    reg("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_GetCurrentTime);
    reg("_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", time_GetCurrentTimePoint);
    reg("_ZN2nn4time14GetSpanBetweenEPlRKNS0_20SteadyClockTimePointES4_", time_GetSpanBetween);
    reg("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", time_ToCalendarTime);
    reg("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", time_ToCalendarTimeInUtc);
    reg("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", time_ToPosixTimeFromUtc);
    reg("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", time_ToPosixTime);
    reg("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", time_LoadTimeZoneRule);
    reg("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", time_ok);
    reg("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", time_zero_span);

    reg("_ZN2nn2vi10InitializeEv", hle_void);
    reg("_ZN2nn2vi8FinalizeEv", hle_void);
    reg("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay);
    reg("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer);
    reg("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow);
    reg("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", hle_ok);
    reg("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", hle_void);
    reg("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", hle_void);

    reg("_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", hle_void);
    reg("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_GetLoadState);
    reg("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_GetAddress);
    reg("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_GetSize);
    reg("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", crypto_RandomBytes);
    reg("_ZN2nn4nifm10InitializeEv", hle_ok);
    reg("_ZN2nn4nifm19SetLocalNetworkModeEb", hle_void);
    reg("_ZN2nn4nifm20SubmitNetworkRequestEv", hle_void);
    reg("_ZN2nn4nifm20CancelNetworkRequestEv", hle_void);
    reg("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", nifm_false);
    reg("_ZN2nn4nifm18IsNetworkAvailableEv", nifm_false);
    reg("_ZN2nn4nifm26HandleNetworkRequestResultEv", nifm_unavailable);
    reg("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_unavailable);
    reg("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_unavailable);
    reg("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable);

    reg("_ZN2nn3err9ShowErrorENS_6ResultE", err_show);
    reg("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", err_show);
    reg("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", err_show);
    reg("_ZN2nn3err23ShowNetworkServiceErrorENS_6ResultE", err_show);
    reg("_ZN2nn3err23ShowNetworkServiceErrorENS0_9ErrorCodeE", err_show);
}

}  // namespace rt
