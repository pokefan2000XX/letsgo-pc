// The game's files read straight from the player's own dump: the base game NSP and the v1.0.2
// update NSP, decrypted with the player's prod.keys. Nothing from the game is stored anywhere
// else: RomFS reads are decrypted on demand, with the update's BKTR patch applied on the fly.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt::nx {

// An opened game: the v1.0.2 RomFS and ExeFS
class Game {
public:
    virtual ~Game() = default;
    // RomFS paths are relative to the root ("bin/..."); false if there's no such file
    virtual bool file_size(const std::string& path, uint64_t* size) const = 0;
    virtual bool is_dir(const std::string& path) const = 0;
    virtual bool read(const std::string& path, uint64_t offset, void* out, uint64_t size) const = 0;
    // An ExeFS file of the update (main, sdk, main.npdm, ...)
    virtual std::vector<uint8_t> exefs(const std::string& name) const = 0;
};

// Opens the base and update NSPs; on failure returns null and sets *error to a sentence for the player
std::unique_ptr<Game> open_game(const std::wstring& base_nsp, const std::wstring& update_nsp,
                                const std::wstring& keys, std::string* error);

// Which of the two NSPs a file is (by its program's title ID), without needing keys for the rest
enum class NspKind { Unknown, Base, Update, OtherGame, Unreadable };
NspKind identify_nsp(const std::wstring& path, const std::wstring& keys);

// Whether these keys decrypt an NCA header of this NSP (the header key is right)
bool keys_work(const std::wstring& keys, const std::wstring& nsp);

// An NSO's segments, decompressed and laid out at their module offsets (text, rodata, data, bss)
struct Nso {
    std::vector<uint8_t> image;     // text..data; bss follows
    uint32_t text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    uint32_t dynsym_size;           // bytes of .dynsym (its symbol count)
    uint8_t build_id[32];
};
bool load_nso(const std::vector<uint8_t>& file, Nso* out);

}  // namespace rt::nx
