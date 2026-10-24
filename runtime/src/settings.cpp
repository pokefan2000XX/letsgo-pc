// settings.ini beside the saves: the player's choices, one a line ("fps=60", "scale=2"). Lines this
// version doesn't know are kept as they are.
//
// The render scale (gpu/nvn_vk.cpp) is chosen here. The game's screen-sized render targets are made
// `scale` times as wide and high, so a window bigger than the game's 1920x1080 (the handheld's
// 1280x720) gets that many more pixels, and a smaller one a sharper picture. "auto", the default,
// takes the monitor's height over the game's, rounded up: 2 on a 1440p or 4K monitor, 1 on a 1080p
// one. A number from 1 to 4 sets it. LETSGO_SCALE chooses for one run; a headless run is 1 without it.
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rt.h"

namespace rt {
namespace {

std::mutex g_lock;
std::string g_file;                                              // settings.ini, or empty (nothing kept)
std::vector<std::pair<std::string, std::string>> g_values;       // in the file's order
int g_scale = 1;

void write() {
    if (g_file.empty()) return;
    std::ofstream out{std::filesystem::path(g_file), std::ios::binary};
    out << "# letsgo-pc's settings: fps=30 or 60 (Tab switches), scale=auto or 1 to 4 (the game's picture times that)\n";
    for (auto& [k, v] : g_values) out << k << "=" << v << "\n";
}

}  // namespace

void settings_init(const std::string& file) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_file = file;
    std::ifstream in{std::filesystem::path(file)};
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        g_values.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
}

std::string setting(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto& [k, v] : g_values)
        if (k == key) return v;
    return "";
}

void set_setting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = std::find_if(g_values.begin(), g_values.end(), [&](auto& kv) { return kv.first == key; });
    if (it != g_values.end()) it->second = value;
    else g_values.emplace_back(key, value);
    write();
}

void render_scale_init(bool headless) {
    const char* env = getenv("LETSGO_SCALE");
    std::string choice = env ? env : setting("scale");
    int s = atoi(choice.c_str());
    if (s < 1) {
        // auto: the monitor's real mode (not the DPI-scaled size a program that isn't DPI-aware is told)
        s = 1;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (!headless && EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
            int game_h = getenv("LETSGO_HANDHELD") ? 720 : 1080;
            s = ((int)dm.dmPelsHeight + game_h - 1) / game_h;
        }
    }
    g_scale = std::clamp(s, 1, 4);
    log("render scale: %d (%s)", g_scale, choice.empty() || choice == "auto" ? "auto" : env ? "LETSGO_SCALE" : "settings.ini");
}

int render_scale() { return g_scale; }

}  // namespace rt
