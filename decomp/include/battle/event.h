// The battle's events (docs/PLAN.md, roadmap phase 5, step 3): how a move's, an ability's or an item's
// effect code runs. Everything that has effects registers a set of handlers, each an event number and a
// function; when the battle reaches a point where effects may act (working out a move's power, a Pokémon
// switching in, the end of a turn), it calls that event's handlers in priority order. A handler reads and
// changes the event's variables, a small stack of labelled values the battle sets up before calling.
#pragma once
#include "battle/battle.h"

namespace battle {

// What a set of handlers belongs to: AddHandlerSet's first parameter.
enum HandlerKind : u32 {
    kKindMove = 0,        // a move, while a Pokémon uses it (0x7100a38010)
    kKindPosition = 1,    // something on one place in the battle (5 of them)
    kKindSide = 2,        // something on one side (18)
    kKindField = 3,       // something on the whole field (10)
    kKindAbility = 4,
    kKindItem = 5,
    kKind6 = 6,           // one set, 0x1d (0x7100a50150)
    kKind7 = 7,           // 0x7100a1c0d0's, from a table of 6 (the Poké Balls, items 1 to 4, and 107)
    kKindRemoved = 8,     // taken out by MarkRemoved, freed by FreeRemovedHandlerSets
    kKindAny = 9,         // for CallHandlers: every kind
};

constexpr u8 kNoPokemon = 0x1f;
constexpr u32 kEvent8b = 0x8b;        // only sets that AnswerEvent8b answer it
constexpr u32 kEventCb = 0xcb;        // items answer it even where their effects are stopped

// Some of the events, named for what answers them and the variables they have (tools/battle_events.py
// maps all 197: where each is called, its variables, and every move, ability and item answering it).
constexpr u32 kEventBasePower = 0x46;  // a move works out its own base power (Flail, Eruption: kVarBasePower)
constexpr u32 kEventPower = 0x47;      // the power's ratio (Technician, type-boosting items: kVarPowerRatio)
constexpr u32 kEventAttack = 0x4a;     // the attacker's attack stat (Huge Power, Thick Fat: kVarAttack, kVarRatio)
constexpr u32 kEventDefense = 0x4b;    // the defender's defense stat (Marvel Scale, Fur Coat: kVarDefense, kVarRatio)
constexpr u32 kEventDamage = 0x5b;     // the damage's ratio (Reflect, Earthquake on a digging Pokémon: kVarRatio)
constexpr u32 kEventComesIn = 0x6d;    // a Pokémon comes in (entry hazards, Intimidate, Drizzle)

// Some of the variables' labels.
constexpr u32 kVarPokemon = 2;         // the Pokémon an event is about
constexpr u32 kVarAttacker = 3;        // in a move's events, the Pokémon using it
constexpr u32 kVarDefender = 4;        // and its target
constexpr u32 kVarBasePower = 0x36;
constexpr u32 kVarPowerRatio = 0x37;   // 0x1000 is 1, kept between 0x29 and 0x200000
constexpr u32 kVarDamage = 0x38;
constexpr u32 kVarAttack = 0x39;
constexpr u32 kVarDefense = 0x3a;
constexpr u32 kVarRatio = 0x3b;        // the stat or damage ratio of kEventAttack, kEventDefense and kEventDamage

struct HandlerSet;

// A handler: the event it answers and the function called with that event. work is the set's own.
using HandlerFunction = void (*)(HandlerSet* set, Flow* flow, u8 pokemon, u32* work);
struct Handler {
    u32 event;
    HandlerFunction function;
};
DECOMP_SIZE(Handler, 0x10);

// A check that can stop other sets' handlers: (this set, the battle, the other's kind, the event, the
// other's ID, the other's Pokémon) gives true to skip the other's handler.
using SkipCheck = bool (*)(HandlerSet* set, Flow* flow, u32 kind, u32 event, u16 id, u8 pokemon);

// One thing's handlers, registered with the battle (0x50 bytes, 211 at most).
struct HandlerSet {
    // flags: the event depth it was added at (its handlers wait until a deeper event, or the next one
    // once every event has ended), the handler count, and these
    static constexpr u32 kAddedAt = 0xffff;
    static constexpr u32 kCountShift = 16;
    static constexpr u32 kRunning = 1u << 24;            // one of its handlers is running
    static constexpr u32 kAsleep = 1u << 25;             // left out for now
    static constexpr u32 kAnswersEvent8b = 1u << 26;
    static constexpr u32 kRemoveWhenDone = 1u << 27;     // removed while running: freed when it ends
    static constexpr u32 kReentrant = 1u << 28;          // may run again while it runs
    static constexpr u32 kInUse = 1u << 29;
    static constexpr u32 kSuppressed = 1u << 30;         // left out, and so is its skip check
    static constexpr u32 kNextToCall = 1u << 31;         // where the event goes on after the current set

    HandlerSet* prev;
    HandlerSet* next;
    const Handler* handlers;
    SkipCheck skipCheck;
    u32 kind;
    u32 priority;          // (9 - sub-priority) << 24 | priority: the list runs from the highest
    u32 flags;
    u32 work[7];           // the handlers' own values
    u16 id;                // the move's, the ability's, the item's... number
    u8 pokemon;            // given to the handlers: the Pokémon (or the place) the set is for
    u8 owner;              // the Pokémon it belongs to (moves, abilities, items, kind 6), or kNoPokemon
};
DECOMP_OFFSET(HandlerSet, kind, 0x20);
DECOMP_OFFSET(HandlerSet, work, 0x2c);
DECOMP_OFFSET(HandlerSet, id, 0x48);
DECOMP_SIZE(HandlerSet, 0x50);

constexpr u32 kMaxHandlerSets = 211;

// The handler sets, the free ones (g_freeHandlerSets[g_handlerSetsUsed] is the next to hand out), the
// list in priority order, and how many events are being called inside one another.
extern HandlerSet g_handlerSets[kMaxHandlerSets];
extern u32 g_handlerSetsUsed;
extern u32 g_eventDepth;
extern HandlerSet* g_firstHandlerSet;
extern HandlerSet* g_freeHandlerSets[kMaxHandlerSets];

// The event variables: a stack of labelled values in frames. A frame runs from g_vars.frame to the first
// empty label, and label 1 marks where the frame below ends. Each value has a mode saying how handlers
// may change it.
constexpr u32 kMaxVars = 96;
constexpr u16 kVarFrameMark = 1;
enum VarMode : u8 {
    kVarModeRewritable = 0,        // RewriteVar changes it
    kVarModeRewritableOnce = 1,    // RewriteVar changes it once
    kVarModeRewritten = 2,         // and then it's kept
    kVarModeRatio = 3,             // MultiplyVar changes it, kept between a minimum and a maximum
    kVarModeConst = 4,
};
struct EventVars {
    u32 frame;
    u16 labels[kMaxVars];
    s32 values[kMaxVars];
    s32 max[kMaxVars];
    s32 min[kMaxVars];
    u8 modes[kMaxVars];
};
DECOMP_OFFSET(EventVars, values, 0xc4);
DECOMP_OFFSET(EventVars, modes, 0x544);
extern EventVars g_vars;

// Resets every handler set and the variables.
void ResetEvents();

// Registers a thing's handlers; null when all 211 sets are taken.
HandlerSet* AddHandlerSet(u32 kind, u16 id, u32 subPriority, u32 priority, u8 pokemon, const Handler* handlers,
                          u32 count);
// Frees a set, or marks it to be freed when its running handler ends.
void RemoveHandlerSet(HandlerSet* set);
// Marks a set removed; FreeRemovedHandlerSets frees every set marked so.
void MarkRemoved(HandlerSet* set);
bool IsMarkedRemoved(const HandlerSet* set);
void FreeRemovedHandlerSets();

// Sleep leaves a set out of events until it wakes; each returns whether it changed anything.
bool PutToSleep(HandlerSet* set);
bool WakeUp(HandlerSet* set);
void WakeAllHandlerSets();
// The moves a Pokémon owns: asleep when FUN_7100a3a670 says so (their work[6] is set), and awake.
void PutMovesToSleep(u16 owner);
void WakeMoves(u16 owner);
// A Pokémon's sets of one kind: left out of events, skip checks included, and back.
void Suppress(u16 owner, u32 kind);
bool Unsuppress(u16 owner, u32 kind);

void AnswerEvent8b(HandlerSet* set);
// A running set may run again in events its handler calls.
void AllowReentry(HandlerSet* set);
void SetSkipCheck(HandlerSet* set, SkipCheck check);
void ClearSkipCheck(HandlerSet* set);

u16 GetId(const HandlerSet* set);
u8 GetOwner(const HandlerSet* set);
u32 GetWork(const HandlerSet* set, u8 index);
void SetWork(HandlerSet* set, u8 index, u32 value);
void ClearWork(HandlerSet* set);

// A Pokémon's sets of one kind (by the Pokémon they're for), in priority order.
HandlerSet* FirstHandlerSet(u32 kind, u8 pokemon);
HandlerSet* NextHandlerSet(HandlerSet* set);

// Calls an event's handlers, every set in priority order; the second also asks the skip checks first.
void CallEvent(Flow* flow, u32 event);
void CallEventWithSkipChecks(Flow* flow, u32 event);
// One set's handler for an event, if it has one and may run; gives the set the event goes on with.
HandlerSet* CallHandlers(Flow* flow, u32 event, u32 kind, bool checkSkips, HandlerSet* set);
// Whether a set's handler is left out: its owner's conditions, the field, and every skip check.
bool ShouldSkip(Flow* flow, HandlerSet* set, u32 event);

// The variables. Each Set puts a value in the current frame (its own slot, or the first free one).
void ResetVars();          // only when a frame is open
void PushVars();
void PopVars();
void SetVar(u32 label, s32 value);
void SetConstVar(u32 label, s32 value);
void SetVarRewritableOnce(u32 label, s32 value);
void MakeVarRewritableOnce(u32 label);
void SetRatioVar(u32 label, s32 value, s32 min, s32 max);
bool RewriteVar(u32 label, s32 value);
// Multiplies a ratio variable by a 4.12 fixed-point ratio (0x1000 is 1), rounded, then kept between its
// minimum and maximum unless both are 0.
void MultiplyVar(u32 label, s32 ratio);
s32 GetVar(u32 label);     // 0 when the frame hasn't it
bool FindVar(u32 label, s32* value);
// A pointer kept in two variables: 0x47 holds its high half and 0x48 its low, or 0x23 and 0x24.
void SetPointerVars47(void* pointer);
void* GetPointerVars47();
void SetPointerVars23(void* pointer);
void* GetPointerVars23();

}  // namespace battle
