// The player's game files (setup.cpp): where they are, asked for on the first start.
#pragma once
#include <memory>
#include <string>

#include "nx.h"

namespace rt {

struct GameFiles {
    std::wstring base, update, keys;
};

// %APPDATA%\LetsGoPikachu (created): config.ini, the save and the log of the single-file build
std::wstring app_data_dir();

// Opens the game from the files given (empty: the ones remembered in config.ini). When they're missing or
// wrong, asks for them with file pickers if interactive, else stops with the reason. Null if the player
// cancels.
std::unique_ptr<nx::Game> open_player_game(GameFiles files, const std::wstring& data_dir, bool interactive);

std::string narrow(const std::wstring& w);   // UTF-8

}  // namespace rt
