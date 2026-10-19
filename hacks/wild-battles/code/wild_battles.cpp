// The wild-battles hack (docs/PLAN.md, roadmap phase 5, step 4): touching a wild Pokémon in the field starts a battle
// with it, in place of the catch screen.
//
// A touch (0x71002fd8a0) makes the Pokémon from the field's object with 0x7100305790, which describes it (species,
// form, level, gender, shininess, IVs and size: one 0x60-byte entry) and builds it from that description
// (0x71002f6470), then starts the catch screen with it (0x7100305970). The scripts' wild battles (CallWildBattle:
// Snorlax, the legendaries) start a battle event (0x710028bc40) with a holder of such entries (0x2b8 bytes,
// 0x71002f5a40), from which the battle makes its wild Pokémon (0x71002f6180). This hack keeps the description the
// touch made and starts that battle with it instead of the catch screen.
//
// That battle event sets its battle up as a boss's (0x710028c610): after the game's plain wild battle setup
// (0x71009cccf0, which starts from zeros) it adds a 5-minute match timer, two flags (one of them, at the setup's
// +0x4a4, also turns off the EXP the battle gives, 0x71009d8a30), bits for Snorlax, Voltorb, Electrode, the legendary
// birds and Mewtwo, and an aura entry for each wild Pokémon. A touch's battle gets those set back to the plain setup's.
//
// After a scripted wild battle the after-battle event (0x7100319a60) opens the catch screen when the battle was won
// (its state 3). After a touch's battle it checks evolutions instead, as it does after a trainer battle (state 1),
// unless the battle was lost. When that event ends the screen fades in from black, as a script does after its wild
// battle (the event leaves the screen black: the script's FadeIn brings the field back).
//
// Catching: the battle core still has Sun and Moon's. A ball (item class 5) goes to the capture (0x7100a10e30,
// 0x7100a11000: HP, catch rate, the ball, status, critical captures, four shake checks), the battle says "You used a
// Poké Ball" and "Gotcha!", gives the capture's EXP and ends with result 5. The balls are in the battle bag when its
// table gives them a pocket (make_romfs.py), and the bag throws them as below. The caught Pokémon then goes where the
// catch screen's go, through the catch event (Receive).
//
// After a battle the field is made again and filled with new wild Pokémon, as when the player walks in; for a moment
// touches start nothing, so one standing at the player doesn't start the next battle at once.
//
// Checked headless (Viridian Forest): the battle starts without the timer, both sides' moves play, a won battle gives
// EXP, Run gets away, a Poké Ball from the bag catches (the Pokédex page, then "You tucked X away in the Pokémon Box"
// with a full party), and the player is back in the field. Not done: the throw has no animation (no ball, no shakes).
#include <string.h>

#include "decomp.h"
#include "gfl/mem.h"
#include "gfl/object.h"
#include "item/item.h"
#include "pokemon/pokemon.h"

using Ref = gfl::RefPtr<gfl::RefObject>;

// Three of these return a reference by value: the caller passes where it goes in x8, which a C++ return of a type
// with a destructor does too. C callers can't use them, which doesn't matter here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
extern "C" {
Ref ORIG_7100305790(void* unused, void* object);                                // the touched Pokémon
Ref ORIG_71002f6470(u8* holder, gfl::mem::MemoryResource* resource, const u8* description);   // a Pokémon
void ORIG_7100305970(void* unused, void* scene, void* pokemon, u64 spawner, u32 kind);         // the catch screen
u64 ORIG_7100319a60(u8* event);                                                  // the after-battle event's update
void ORIG_710028c610(u8* event, const void* wild);                               // the battle event's wild setup
void ORIG_71002f5a40(u8* holder);                                                // the holder's constructor
void ORIG_71002f5ec0(u8* holder);                                                // its setup from the game's state
Ref ORIG_710028bc40(Ref* field, gfl::mem::MemoryResource* resource, const void* wild);        // a wild battle's event
long ORIG_7100a9c650(u8* model, const u64* point);                               // a battle model's named point
u32 ORIG_71002fd8a0(void* unused, void* scene);                                  // a touch in the field
Ref FUN_710028ee10(u8* events, gfl::mem::MemoryResource* resource);             // a catch event (its handle)
void FUN_7100729440(void* to, const void* from);                                 // copies a Pokémon
u32 FUN_71007247c0(const void* pokemon);                                         // its species
u32 FUN_7100724b90(const void* pokemon);                                         // its ball
void ORIG_71009e79f0(u8* pokemon, u32 item);                                     // a battle: the ball that caught it
void FUN_710011ecb0(void* fader, u32 layer, u32 type, u32 frames);   // fades in (the FadeIn_ command: type 1 black)

extern u8* DAT_710160d338;   // its +0x68: the memory resource the field's events are made in
extern u8* DAT_710160d310;   // its +0x68: the field's event system
extern u8* DAT_710160d300;   // the screen's fader

// nn::os::GetSystemTick(): 19.2 MHz
u64 _ZN2nn2os13GetSystemTickEv();
// nn::diag::detail::LogImpl(LogMetaData const&, char const*, ...): the runtime writes it to letsgo.log
void _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(const void* meta, const char* format, ...);
}

namespace {

constexpr u32 kHolderSize = 0x2b8;
constexpr u32 kEntries = 0x58, kEntrySize = 0x60;   // the holder's wild Pokémon, 6 entries
constexpr u32 kCount = 0x298;                        // how many of them there are
constexpr u32 kResource = 0x68;
constexpr u32 kState = 0x68;                         // an event's state
constexpr u32 kAfterBattleProc = 0x88;               // the after-battle event's battle process
constexpr u32 kProcSetup = 0x68;                     // the process's battle setup, at its battle event's +0xe0
constexpr u32 kSetupInEvent = 0xe0, kEventHolder = 0xc0;
constexpr u32 kSetupResult = 0x508;                  // 0 lost, 1 won, 3 ran away, 5 a ball caught the wild Pokémon
constexpr u32 kChecksEvolutions = 1, kOpensCatchScreen = 3, kEnds = 4;
constexpr u32 kCaught = 5;
constexpr u32 kAfterBattlePokemon = 0xc0;            // the after-battle event's copy of the wild Pokémon
// The catch event (0x710031bed0, its update 0x710031c070): the catch screen's starter (0x7100305970) fills it in
constexpr u32 kCatchPokemon = 0x90;                  // its own Pokémon, a copy of the caught one
constexpr u32 kCatchKind = 0xa8;                     // 0 a field Pokémon, 1 GO Park, 2 the tutorial
constexpr u32 kCatchResult = 0xac;                   // 0 caught, 1 it got away
constexpr u32 kCatchSpawner = 0xb8, kCatchSpawnerKind = 0xc0;
constexpr u32 kCatchPopsMusic = 0x3ad;               // state 7 gives the field back its music
constexpr u32 kCatchReceives = 3;                    // the state after the catch screen
constexpr u64 kQuiet = 3 * 19200000;                 // ticks after a battle in which touches start nothing
// What 0x710028c610 adds for a boss, in the battle event (its setup is at +0xe0)
constexpr u32 kMatchTime = 0x562;                    // u16 seconds: 300
constexpr u32 kBossFlags = 0x574;                    // u32: | 0x80
constexpr u32 kBossSpecies = 0x578;                  // u32: a bit for each of the boss species' rules
constexpr u32 kBoss = 0x584;                         // u8: 1 (no EXP, among others)
constexpr u32 kAuras = 0x280, kAuraSize = 0x20;      // each wild Pokémon's: u32 1, then 5 bytes from its description

alignas(8) const u8 kMeta[0x80] = {};   // LogImpl's metadata; the runtime doesn't read it

bool g_touching;                        // 0x7100305790 is running
bool g_described;                       // g_description is the touched Pokémon's
alignas(8) u8 g_description[kEntrySize];
gfl::RefObject* g_holder;               // the holder of the touch's battle (a reference), until its after-battle event
u8* g_after;                            // that battle's after-battle event, until it ends
u64 g_spawner;                          // the touched Pokémon's spawner and its kind, for the catch event
u32 g_spawnerKind;
bool g_caught;                          // that battle caught its Pokémon
u64 g_backAt;                           // when the field came back after the last one (system ticks), or 0
u8 g_ball;                              // with this ball (Gen 7's ball numbers)

template <typename T>
T& At(void* object, u32 offset) {
    return *reinterpret_cast<T*>(static_cast<u8*>(object) + offset);
}

// As gfl::Create makes it: allocated and constructed with the resource current, its allocation recorded.
gfl::RefObject* MakeHolder(gfl::mem::MemoryResource* resource) {
    gfl::mem::ResourceScope scope(resource);
    u8* holder = static_cast<u8*>(resource->Allocate(kHolderSize, 8));
    if (holder == nullptr) resource->Dump();
    ORIG_71002f5a40(holder);
    At<void*>(holder, 0x08) = resource;
    At<void*>(holder, 0x10) = holder;
    At<u64>(holder, 0x18) = kHolderSize;
    At<u64>(holder, 0x20) = 8;
    return reinterpret_cast<gfl::RefObject*>(holder);
}

// A Pokémon caught in battle goes where the catch screen's go: the catch event takes it from the state after the
// catch screen (3) with the result "caught", so it is put in the party or a box and in the Pokédex (state 5,
// 0x7100b08120), the caught Pokémon's screen shows, and the spawner counts it (state 10). The field's music and
// screen are already back (the battle's), so state 7 skips that. As the after-battle event does for its catch screen
// (0x7100319f80), the catch event gets a copy of the after-battle event's Pokémon.
void Receive(const u8* pokemon) {
    auto* resource = At<gfl::mem::MemoryResource*>(DAT_710160d338, kResource);
    Ref handle = FUN_710028ee10(At<u8*>(DAT_710160d310, 0x68), resource);
    if (handle.Get() == nullptr) return;
    u8* event = At<u8*>(handle.Get(), 0x60) - 0x50;   // the handle points at the event's counted part
    FUN_7100729440(At<void*>(event, kCatchPokemon), pokemon);
    // The after-battle event's copy was made before the battle; the battle gave its own Pokémon the ball.
    if (g_ball != 0) static_cast<pokemon::Pokemon*>(At<void*>(event, kCatchPokemon))->SetMetInfo(8, g_ball);
    g_ball = 0;
    At<u32>(event, kCatchKind) = 0;
    At<u32>(event, kCatchResult) = 0;
    At<u64>(event, kCatchSpawner) = g_spawner;
    At<u32>(event, kCatchSpawnerKind) = g_spawnerKind;
    At<u8>(event, kCatchPopsMusic) = 0;
    At<u32>(event, kState) = kCatchReceives;
    _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "wild-battles: caught Pokemon %u in ball %u",
                                                        FUN_71007247c0(pokemon),
                                                        FUN_7100724b90(At<void*>(event, kCatchPokemon)));
}

}  // namespace

extern "C" Ref FUN_7100305790(void* unused, void* object) {
    g_touching = true;
    g_described = false;
    Ref pokemon = ORIG_7100305790(unused, object);
    g_touching = false;
    return pokemon;
}

extern "C" Ref FUN_71002f6470(u8* holder, gfl::mem::MemoryResource* resource, const u8* description) {
    if (g_touching) {
        memcpy(g_description, description, kEntrySize);
        g_described = true;
    }
    return ORIG_71002f6470(holder, resource, description);
}
#pragma clang diagnostic pop

extern "C" void FUN_7100305970(void* unused, void* scene, void* pokemon, u64 spawner, u32 kind) {
    if (!g_described) {
        ORIG_7100305970(unused, scene, pokemon, spawner, kind);
        return;
    }
    g_described = false;
    g_spawner = spawner;
    g_spawnerKind = kind;

    auto* resource = At<gfl::mem::MemoryResource*>(DAT_710160d338, kResource);
    Ref holder(MakeHolder(resource));
    auto* h = reinterpret_cast<u8*>(holder.Get());
    ORIG_71002f5ec0(h);
    memcpy(h + kEntries, g_description, kEntrySize);
    At<u32>(h, kCount) = 1;

    // As CallWildBattle starts its battle (0x71002899e0, state 3): mode 0 and the holder, in the field's event
    // system. The holder keeps the constructor's empty battlefield hashes, so the battle takes the area's own.
    struct Wild {
        u32 mode;
        gfl::RefObject* holder;
    } wild = {0, holder.Get()};
    Ref field(At<gfl::RefObject*>(DAT_710160d310, 0x68));
    Ref event = ORIG_710028bc40(&field, resource, &wild);

    if (g_holder != nullptr) static_cast<gfl::RefCounted*>(g_holder)->Release();
    g_holder = holder.Get();   // a reference kept until the battle's after-battle event
    static_cast<gfl::RefCounted*>(g_holder)->AddRef();
    _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "wild-battles: a battle with Pokemon %u, level %u",
                                                        At<u32>(g_description, 0), At<u8>(g_description, 5));
}

extern "C" void FUN_710028c610(u8* event, const void* wild) {
    ORIG_710028c610(event, wild);
    if (g_holder == nullptr || At<gfl::RefObject*>(event, kEventHolder) != g_holder) return;
    At<u16>(event, kMatchTime) = 0;
    At<u32>(event, kBossFlags) &= ~0x80u;
    At<u32>(event, kBossSpecies) = 0;
    At<u8>(event, kBoss) = 0;
    for (u32 i = 0; i < 6; i++) {
        u8* aura = static_cast<u8*>(static_cast<void*>(event)) + kAuras + i * kAuraSize;
        memset(aura, 0, 9);
    }
}

extern "C" u64 FUN_7100319a60(u8* event) {
    u8* setup = nullptr;
    if (At<u32>(event, kState) == 0 && g_holder != nullptr && At<u8*>(event, kAfterBattleProc) != nullptr) {
        u8* s = At<u8*>(At<u8*>(event, kAfterBattleProc), kProcSetup);
        if (s != nullptr && At<gfl::RefObject*>(s - kSetupInEvent, kEventHolder) == g_holder) setup = s;
    }
    u64 done = ORIG_7100319a60(event);
    if (setup != nullptr) {
        u32& state = At<u32>(event, kState);
        if (state == kOpensCatchScreen || (state == kEnds && At<u32>(setup, kSetupResult) != 0)) state = kChecksEvolutions;
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "wild-battles: the battle ended with result %u",
                                                            At<u32>(setup, kSetupResult));
        g_caught = At<u32>(setup, kSetupResult) == kCaught;
        static_cast<gfl::RefCounted*>(g_holder)->Release();
        g_holder = nullptr;
        g_after = event;
    }
    if (event == g_after && done == 0 && g_caught && At<u32>(event, kState) == kEnds) {
        // As the after-battle event opens its catch screen (state 3, 0x7100319f80): the frame before it ends.
        if (At<u8*>(event, kAfterBattlePokemon) != nullptr) Receive(At<u8*>(event, kAfterBattlePokemon));
        g_caught = false;
    }
    if (event == g_after && done != 0) {
        // As a script does after its wild battle (sub_event_047), the screen fades in from black.
        FUN_710011ecb0(DAT_710160d300, 0, 1, 15);
        _ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz(kMeta, "wild-battles: back in the field");
        g_after = nullptr;
        g_backAt = _ZN2nn2os13GetSystemTickEv();
        g_caught = false;
    }
    return done;
}

// A battle model's named point (a joint such as "Waist", by its FNV-1a 64 hash), for a move's effects: 0x7100a9c650
// finds it in the model behind the battle model (its +0x1e0, or +0x1e8 when +0x192 is set) and crashes when there is
// none. In a touch's battle the wild Pokémon's battle model (slot 1) never gets one: when the battle view makes its
// models (0x7100a7a490), the battle core says nothing stands at that position yet (0x71009db9a0), so only slot 0 is
// loaded (0x7100a990b0), and the Pokémon the player sees is drawn some other way. Its moves then ask slot 1 for points.
// With no model behind it the point isn't found, as for a model without that joint: the callers skip that effect.
extern "C" long FUN_7100a9c650(u8* model, const u64* point) {
    bool other = At<u8>(model, 0x192) != 0 && At<void*>(model, 0x1e8) != nullptr;
    if (!other && At<void*>(model, 0x1e0) == nullptr) return 0;
    return ORIG_7100a9c650(model, point);
}

// A ball caught the wild Pokémon: 0x71009e79f0 gives the battle's Pokémon the ball (its item, as a ball number).
extern "C" void FUN_71009e79f0(u8* pokemon, u32 item) {
    ORIG_71009e79f0(pokemon, item);
    g_ball = item::GetBallId(static_cast<u16>(item));
    if (g_ball == 0) g_ball = 4;
}

// The battle bag's "Use this item" (0x710076ba50) goes on only for the items it knows: medicine, battle items and the
// escape items (Poké Doll, Fluffy Tail, Poké Toy: 0x7100a18f60, which only the bag calls). Balls are taken as escape
// items, so the bag asks which Pokémon (any will do) and gives the battle the ball, which it throws at the wild one.
extern "C" bool ORIG_7100a18f60(u32 item);
extern "C" bool FUN_7100a18f60(u32 item) {
    return ORIG_7100a18f60(item) || item::GetBallId(static_cast<u16>(item)) != 0;
}

// A touch in the field (0x71002fd8a0: it finds the touched Pokémon and opens the catch screen or its event). After a
// battle the field is made again and filled with new wild Pokémon, as when the player walks in, so one can be standing
// at the player: for a moment after the field is back, touches start nothing and the player can walk away.
extern "C" u32 FUN_71002fd8a0(void* unused, void* scene) {
    if (g_backAt != 0) {
        if (_ZN2nn2os13GetSystemTickEv() - g_backAt < kQuiet) return 0;
        g_backAt = 0;
    }
    return ORIG_71002fd8a0(unused, scene);
}
