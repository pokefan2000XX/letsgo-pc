// Putting a message made beforehand (battle/message.h).
#include "battle/message.h"

namespace battle {

namespace {

constexpr u32 kKindMask = 0x7f;               // BattleMessage::info
constexpr u32 kCountShift = 7;
constexpr u32 kCountMask = 0x7f;
constexpr u32 kHasValue = 0x4000;

constexpr u32 kKindPlain = 1;
constexpr u32 kKindPokemon = 2;

}  // namespace

// 0x71009fa760
bool PutBattleMessage(Flow* flow, const BattleMessage* message) {
    u32 kind = message->info & kKindMask;
    u32 count = message->info >> kCountShift & kCountMask;
    if (message->info & kHasValue) {
        if (kind == kKindPokemon) {
            PutPokemonMessageArgsWithValue(flow, message->id, message->value, count, message->args);
            return true;
        }
        if (kind == kKindPlain) {
            PutMessageArgsWithValue(flow, message->id, message->value, count, message->args);
            return true;
        }
        return false;
    }
    if (kind == kKindPokemon) {
        PutPokemonMessageArgs(flow, message->id, count, message->args);
        return true;
    }
    if (kind == kKindPlain) {
        PutMessageArgs(flow, message->id, count, message->args);
        return true;
    }
    return false;
}

}  // namespace battle
