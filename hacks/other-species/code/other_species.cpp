// other-species: Pokémon of species Let's Go hasn't got (1 to 809) are kept in the party and boxes, not made bad eggs.
//  - When a save is loaded, 0x7100131d30 goes through the Pokémon in storage and marks a bad egg (Pokemon::MarkBadEgg) each one
//    whose species isn't 1-151, 808 or 809, whose form isn't one the game allows (Alolan forms for the species that have them,
//    form 8 for Pikachu, form 1 for Eevee) or that knows a move outside the 230 the game has (the sorted list at 0x7100f4982c).
//    Its second argument's bit 0 skips the marking. A Pokémon from a later game put in a save showed as a fainted egg and
//    couldn't battle.
//  - Here the original runs with marking skipped (it still does its other work for the Pokémon it finds valid), then the same
//    rules mark bad eggs, with the species allowed being every one with species data (1 to 809).
#include "decomp.h"
#include "gfl/object.h"

using Ref = gfl::RefPtr<gfl::RefObject>;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
extern "C" {
void ORIG_7100131d30(u8* self, u64 flags);
u16 FUN_71001c0e90(void* storage);                // how many Pokémon it holds
Ref FUN_71001c0d30(void* storage, u32 index);    // one of them
u32 FUN_71007247c0(const void* pokemon);          // its species
u8 FUN_71007268c0(const void* pokemon);           // its form
s32 FUN_7100726cc0(const void* pokemon, u32 slot);   // a move
void FUN_7100728120(void* pokemon);               // Pokemon::MarkBadEgg
extern const s32 DAT_7100f4982c[];                // the moves the game has, sorted (230)

static bool FormAllowed(u32 species, u8 form) {
    if (form == 0) return true;
    switch (species) {
    case 25: return form == 8;                    // partner Pikachu
    case 19: case 20: case 26: case 27: case 28: case 37: case 38: case 50: case 51: case 52: case 53:
    case 74: case 75: case 76: case 88: case 89: case 103: case 105: case 133:
        return form == 1;                         // Alolan forms, and partner Eevee
    }
    return true;
}

static bool MoveKnown(s32 move) {
    for (u32 i = 0; i < 230; i++) {
        if (DAT_7100f4982c[i] == move) return true;
        if (DAT_7100f4982c[i] > move) return false;
    }
    return false;
}

void FUN_7100131d30(u8* self, u64 flags) {
    ORIG_7100131d30(self, flags | 1);
    if (flags & 1) return;
    void* storage = *reinterpret_cast<void**>(*reinterpret_cast<u8**>(self + 0x58) + 0xb0);
    if (storage == nullptr) return;
    u16 count = FUN_71001c0e90(storage);
    for (u32 i = 0; i < count; i++) {
        Ref held = FUN_71001c0d30(storage, i);
        void* pokemon = held.Get();
        if (pokemon == nullptr) continue;
        u32 species = FUN_71007247c0(pokemon);
        bool valid = species >= 1 && species <= 809 && FormAllowed(species, FUN_71007268c0(pokemon));
        for (u32 slot = 0; valid && slot < 4; slot++) valid = MoveKnown(FUN_7100726cc0(pokemon, slot));
        if (!valid) FUN_7100728120(pokemon);
    }
}
}
#pragma clang diagnostic pop
