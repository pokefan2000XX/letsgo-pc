// smooth-moves: a scripted character can run on from one forced move into the next without stopping (2026-10-24).
//  - Each StartForceMove_ (0x7100185d50 -> 0x7100243370) gives the character a forced-move command. One still running when
//    the next starts isn't dropped: it waits while the newer one runs, then goes on, finds itself at or past its own target and
//    ends by putting the character there (0x71002dcd90). So a script that starts the next leg as the character nears a corner
//    (a run that turns as it goes, never stopping) saw the character snap back along the corners once the last leg ended.
//  - The command's update (0x71002dbec0, its vtable +0x38) is here. Its state is at +0x60 (1 starting, 2 turning on the spot,
//    3 moving, 4 and 5 ending) and +0x69 is set when it is done. A command seen starting is its character's newest; an older
//    one of the same character that is still going is ended where it is (state 0, done), as a cancelled one would be.
//  - The game's own scripts wait for a move to end before starting the next, so they never get here with two.
//  - A move turns on the spot first (16 frames) only when it heads more than 15 degrees off the character's facing
//    (0x71002dd040); under that it faces the new way at once and runs, so a run whose legs each bend less than that, each
//    started before the last one ends, is one continuous run.
#include "decomp.h"

namespace {

constexpr u32 kState = 0x60, kDone = 0x69;
constexpr s32 kStarting = 1;

template <typename T>
T& At(void* object, u32 offset) {
    return *reinterpret_cast<T*>(static_cast<u8*>(object) + offset);
}

// each character's newest forced-move command (a few dozen characters move at most)
struct Newest {
    void* character;
    void* command;
};
Newest g_newest[64];
u32 g_next;

void Remember(void* character, void* command) {
    for (Newest& n : g_newest)
        if (n.character == character) {
            n.command = command;
            return;
        }
    g_newest[g_next++ % 64] = {character, command};
}

void* NewestOf(void* character) {
    for (const Newest& n : g_newest)
        if (n.character == character) return n.command;
    return nullptr;
}

}  // namespace

extern "C" void ORIG_71002dbec0(void* command, void** character);

extern "C" void FUN_71002dbec0(void* command, void** character) {
    void* who = character ? *character : nullptr;
    if (who) {
        s32 state = At<s32>(command, kState);
        if (state == kStarting) {
            Remember(who, command);
        } else if (state > kStarting && !At<u8>(command, kDone)) {
            void* newest = NewestOf(who);
            if (newest && newest != command) {
                At<s32>(command, kState) = 0;
                At<u8>(command, kDone) = 1;
                return;
            }
        }
    }
    ORIG_71002dbec0(command, character);
}
