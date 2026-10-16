// The player's game files: the base game NSP, its 1.0.2 update NSP and prod.keys. Asked for once with
// file pickers, remembered in config.ini, and checked at every start.
#include "setup.h"

#include <windows.h>
#include <commdlg.h>

#include <filesystem>
#include <fstream>

#include "rt.h"

namespace rt {
namespace {

const wchar_t* const kTitle = L"Pokémon: Let's Go, Pikachu!";

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

bool exists(const std::wstring& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

GameFiles load_config(const std::wstring& path) {
    GameFiles f;
    std::ifstream in{std::filesystem::path(path)};
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::wstring v = widen(line.substr(eq + 1));
        if (k == "base") f.base = v;
        else if (k == "update") f.update = v;
        else if (k == "keys") f.keys = v;
    }
    return f;
}

void save_config(const std::wstring& path, const GameFiles& f) {
    std::ofstream out{std::filesystem::path(path), std::ios::binary};
    out << "# The files Pokemon: Let's Go, Pikachu! starts from. Delete this file to choose them again.\n"
        << "base=" << narrow(f.base) << "\nupdate=" << narrow(f.update) << "\nkeys=" << narrow(f.keys) << "\n";
}

int message(const std::wstring& text, UINT flags) {
    return MessageBoxW(nullptr, text.c_str(), kTitle, flags | MB_SETFOREGROUND);
}

bool pick_file(const wchar_t* title, const wchar_t* filter, std::wstring* path) {
    std::vector<wchar_t> buf(32768, L'\0');
    std::wstring dir;
    if (!path->empty()) dir = std::filesystem::path(*path).parent_path().wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    *path = buf.data();
    return true;
}

enum Which { kBase, kUpdate, kKeys };

bool pick(Which w, GameFiles* f) {
    switch (w) {
    case kBase:
        return pick_file(L"Choose Pokémon: Let's Go, Pikachu! (the base game .nsp)",
                         L"Switch games (*.nsp)\0*.nsp\0All files\0*.*\0", &f->base);
    case kUpdate:
        return pick_file(L"Choose the game's update to version 1.0.2 (.nsp)",
                         L"Switch games (*.nsp)\0*.nsp\0All files\0*.*\0", &f->update);
    case kKeys:
        return pick_file(L"Choose your Switch's keys (prod.keys)", L"Keys (*.keys)\0*.keys\0All files\0*.*\0", &f->keys);
    }
    return false;
}

// Which file is wrong, and a sentence saying why
Which diagnose(const GameFiles& f, const std::string& error, std::wstring* why) {
    if (!exists(f.base)) { *why = L"The base game file wasn't found."; return kBase; }
    if (!exists(f.update)) { *why = L"The update file wasn't found."; return kUpdate; }
    if (!exists(f.keys)) { *why = L"The keys file (prod.keys) wasn't found."; return kKeys; }
    if (!nx::keys_work(f.keys, f.base)) {
        *why = L"These keys can't decrypt the game. Choose the prod.keys dumped from your Switch.";
        return kKeys;
    }
    switch (nx::identify_nsp(f.base, f.keys)) {
    case nx::NspKind::Base: break;
    case nx::NspKind::Update: *why = L"The file chosen as the base game is the update. Choose the game itself."; return kBase;
    case nx::NspKind::OtherGame: *why = L"The file chosen as the base game is a different game."; return kBase;
    default: *why = L"The file chosen as the base game isn't a Switch game (.nsp) that can be read."; return kBase;
    }
    switch (nx::identify_nsp(f.update, f.keys)) {
    case nx::NspKind::Update: break;
    case nx::NspKind::Base: *why = L"The file chosen as the update is the base game. Choose its update to 1.0.2."; return kUpdate;
    case nx::NspKind::OtherGame: *why = L"The file chosen as the update is for a different game."; return kUpdate;
    default: *why = L"The file chosen as the update isn't a Switch update (.nsp) that can be read."; return kUpdate;
    }
    *why = L"The game couldn't be read from these files: " + widen(error) + L".";
    return error.find("keys") != std::string::npos ? kKeys : kUpdate;
}

}  // namespace

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring app_data_dir() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    std::filesystem::path dir = std::filesystem::path(appdata && *appdata ? appdata : L".") / L"LetsGoPikachu";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

std::unique_ptr<nx::Game> open_player_game(GameFiles files, const std::wstring& data_dir, bool interactive) {
    std::wstring config = data_dir + L"\\config.ini";
    GameFiles saved = load_config(config);
    bool given = !files.base.empty() || !files.update.empty() || !files.keys.empty();
    if (files.base.empty()) files.base = saved.base;
    if (files.update.empty()) files.update = saved.update;
    if (files.keys.empty()) files.keys = saved.keys;
    bool asked = false;
    for (;;) {
        std::string error;
        if (exists(files.base) && exists(files.update) && exists(files.keys)) {
            if (auto game = nx::open_game(files.base, files.update, files.keys, &error)) {
                if (asked || !given) save_config(config, files);
                log("game files: %s, %s, %s", narrow(files.base).c_str(), narrow(files.update).c_str(),
                    narrow(files.keys).c_str());
                return game;
            }
        }
        std::wstring why;
        Which w = diagnose(files, error, &why);
        if (!interactive) fatal("%s", narrow(why).c_str());
        if (!asked && files.base.empty() && files.update.empty() && files.keys.empty()) {
            // first start: ask for all three
            if (message(L"Pokémon: Let's Go, Pikachu! runs from your own copy of the game. Choose these three "
                        L"files, dumped from your Switch:\n\n"
                        L"    1.  The game: Pokémon: Let's Go, Pikachu! (.nsp)\n"
                        L"    2.  Its update to version 1.0.2 (.nsp)\n"
                        L"    3.  Your Switch's keys (prod.keys)\n\n"
                        L"They stay where they are: the game reads them every time it starts.",
                        MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                return nullptr;
            for (Which k : {kBase, kUpdate, kKeys})
                if (!pick(k, &files)) return nullptr;
        } else {
            if (message(why + L"\n\nChoose it now?", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return nullptr;
            if (!pick(w, &files)) return nullptr;
        }
        asked = true;
    }
}

}  // namespace rt
