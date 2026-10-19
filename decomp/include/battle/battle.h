// The battle's objects, as far as decompiled code uses them (docs/PLAN.md, roadmap phase 5, step 3). Not
// decompiled: their functions link to the originals.
#pragma once
#include "decomp.h"

namespace pokemon {
class Pokemon;
}

namespace battle {

// Types, as the move data numbers them
constexpr u8 kTypeRock = 5;
constexpr u8 kTypeFire = 9;
constexpr u8 kTypeWater = 10;
constexpr u8 kTypeNone = 0x12;         // typeless: no type bonus, and no Pokémon has it

// Move categories
constexpr u32 kCategoryPhysical = 1;
constexpr u32 kCategorySpecial = 2;

// Weather (Field::GetWeather). 3 and 7 aren't used by the decompiled code.
constexpr u8 kWeatherSun = 1;
constexpr u8 kWeatherRain = 2;
constexpr u8 kWeatherSandstorm = 4;
constexpr u8 kWeatherHeavyRain = 5;
constexpr u8 kWeatherHarshSun = 6;

// What BattlePokemon::GetValue gives
constexpr u32 kValueAccuracyStage = 6;  // stages run 0 to 12, 6 being none
constexpr u32 kValueEvasionStage = 7;
constexpr u32 kValueAttack = 8;
constexpr u32 kValueDefense = 9;       // the two defenses swap under field effect 6, Wonder Room
constexpr u32 kValueSpAtk = 10;
constexpr u32 kValueSpDef = 11;
constexpr u32 kValueSpeed = 0xc;
constexpr u32 kValueHp = 0xd;
constexpr u32 kValueMaxHp = 0xe;
constexpr u32 kValueLevel = 0xf;
constexpr u32 kValueBaseAbility = 0x10;        // its ability even while it doesn't work
constexpr u32 kValueAbility = 0x11;

constexpr u32 kAbilityNoGuard = 99;
constexpr u32 kAbilityComatose = 213;

constexpr u32 kStatusParalysis = 1;
constexpr u32 kStatusSleep = 2;
constexpr u32 kStatusFreeze = 3;
constexpr u32 kStatusBurn = 4;

// Conditions (BattlePokemon::HasCondition): 1 to 5 are the statuses, and others the decompiled code names
// where it uses them.
constexpr u32 kConditionBurn = 4;
constexpr u32 kConditionPoison = 5;           // bad poison too (IsBadPoison)
constexpr u32 kConditionNightmare = 9;
constexpr u32 kConditionCurse = 10;

// A Pokémon in the battle.
struct BattlePokemon {
    u8 GetId();
    // Whether its HP is 0.
    bool IsFainted();
    // Whether it can fight: it isn't an egg (a bad egg either) and its HP isn't 0.
    bool CanFight();
    // Whether its HP is its maximum.
    bool IsAtFullHp();
    // The damage a condition does it at the end of a turn: a burn 1/16 of its maximum HP, poison 1/8, bad
    // poison 1/16 times the turns counted, Nightmare 1/4 while it sleeps (or has Comatose), Curse 1/4; 0 for
    // a condition that's off or does none.
    u32 GetConditionDamage(u32 condition);
    // The PP left of a move it knows (0 when it doesn't).
    u16 GetPpOfMove(u32 move);
    // A stat with its stages, the level and so on (kValue...).
    u32 GetValue(u32 value);
    u32 GetValueIgnoringStages(u32 value);
    // The stat a critical hit uses.
    u32 GetValueForCritical(u32 value);
    bool HasType(u8 type);
    // Its three types, 5 bits each from the bottom (the third is one a move added: Forest's Curse).
    u16 GetTypes();
    u32 GetStatus();
    // Whether one of its conditions is on (8-byte entries at +0x98). 0x10 stops its ability working and
    // 0x13 its item, so they're probably Gastro Acid and Embargo.
    bool HasCondition(u32 condition);
    // A condition's parameter: for 0x1e (Lock-On), the Pokémon it's on; for 7 (love), the one it loves.
    u16 GetConditionParam(u32 condition);
    // A condition's 8 bytes (+0x98 on): its kind in the low 3 bits (0 when it's off), then what it holds
    // (ConditionValue).
    u64 GetCondition(u32 condition);
    // A condition's turns counted so far (+0x1e8 on).
    u8 GetConditionTurns(u32 condition);
    // Takes away its status (and the conditions that go with one).
    void CureStatus();
    void CureCondition(u32 condition);
    // Counts a turn of sleep (+0x1ea) or confusion (+0x1ee): true when its turns are up, and it's cured.
    bool CountSleepTurn();
    bool CountConfusionTurn();
    // The slot (0 to 3) of a move it knows, 4 when it doesn't (or for move 0).
    u8 FindMove(u32 move);
    // The PP left in a slot (0 for a slot it hasn't).
    u16 GetPp(u8 slot);
    // The move it's locked into under condition 0x1a (+0x2f0), and the one its Choice item holds it to
    // (condition 0x1c's value, 0 when there's none).
    u32 GetLockedMove();
    u32 GetChoiceMove();
    // A bit of the flags at +0x300 (4 it flinched, 6 it was hit before moving, 9 its item doesn't work).
    bool HasFlag(u32 flag);
    void SetFlag(u32 flag);
    // Its own critical stage, at most 3: +0x2e3, 2 more for a flag (Focus Energy, probably) and 3 more under
    // condition 0x29 (Laser Focus, probably).
    u8 GetCriticalStage();
    u16 GetSpecies();
    // Changes its form (Shaymin's Sky Forme back to 0).
    void ChangeForm(u8 form, bool unk);
    // Its bracket in the turn order (+0x2e6): event 0xf sets it each turn (Quick Claw, Stall).
    u8 GetOrderBracket();
    void SetOrderBracket(u8 bracket);
    // Whether a stat's stage (1 attack to 7 evasion; 0 to 12 from +0x274, 6 being none) can go up, for stages
    // above 0, or down; false for other stats.
    bool CanChangeStage(u32 stat, s32 stages);
    // Whether it's behind a substitute (the substitute's HP at +0x3ea).
    bool HasSubstitute();
    // Raises or lowers a stat's stage by up to stages, as far as its limit: by how much it did.
    u8 RaiseStage(u32 stat, u8 stages);
    u8 LowerStage(u32 stat, u8 stages);

    u8 unk0[0x60];
    pokemon::Pokemon* pokemon;     // the Pokémon it is
    u8 unk68[0x30];
    u64 conditions[0x2a];          // GetCondition
    u8 conditionCounters[0x2a];    // turns counted: sleep, confusion, bad poison
    u8 unk212[0xcf];
    u8 form;
};
DECOMP_OFFSET(BattlePokemon, pokemon, 0x60);
DECOMP_OFFSET(BattlePokemon, conditions, 0x98);
DECOMP_OFFSET(BattlePokemon, conditionCounters, 0x1e8);
DECOMP_OFFSET(BattlePokemon, form, 0x2e1);

// A client's Pokémon in the battle (PokemonList::GetParty).
struct BattleParty {
    // How many of its Pokémon can fight (BattlePokemon::CanFight).
    u32 CountFighters();

    BattlePokemon* members[6];
    u8 count;
};
DECOMP_OFFSET(BattleParty, count, 0x30);

// The battle's Pokémon by ID.
struct PokemonList {
    BattlePokemon* Get(u8 id);
    // A client's party (0x38 bytes each from +0x60).
    BattleParty* GetParty(u32 client);
};

struct Setup;

// A set of Pokémon, such as a move's targets.
struct PokemonSet {
    u8 Count();
    // The Pokémon at index (null past the end).
    BattlePokemon* Get(u8 index);
};

// A condition's value (for Disable and Encore, the move), from its 8 bytes (BattlePokemon::GetCondition).
u16 ConditionValue(const u64* condition);
// The Pokémon that caused a condition.
u32 ConditionCause(const u64* condition);
// The turns a condition lasts: bits 8-13 for kinds 2 and 4, else 0.
u8 ConditionTurnLimit(const u64* condition);
// A flag in a condition's 8 bytes: bit 30 for kinds 1 to 3, bit 36 for kind 4. For a binding move it makes the
// damage 1/6 instead of 1/8, so it's probably the Binding Band.
bool ConditionFlag(const u64* condition);
// A condition's 8 bytes: its kind in bits 0-2 and the Pokémon that caused it in bits 3-7, then by kind: 1 a
// count in bits 8-13 and a value from bit 14 (bad poison is count 15), 2 turns in bits 8-13, 3 a Pokémon in
// bits 8-13, 4 turns in bits 8-13 and a Pokémon in bits 14-19.
u64 MakeConditionCounted(u8 cause, u32 count, u16 value);
u64 MakeConditionTurns(u8 cause, u32 turns);
u64 MakeConditionTied(u8 cause, u8 pokemon);
u64 MakeConditionTurnsTied(u8 cause, u8 pokemon, u32 turns);
// Whether a poison condition is bad poison.
bool IsBadPoison(const u64* condition);

// A message for the clients being put together: its number, its kind (1 one without a name first, 2 one starting
// with a Pokémon's name; 0 none), its arguments and a value that goes with some (PutBattleMessage).
struct BattleMessage {
    u16 id;
    u16 info;              // bits 0-6 the kind, 7-13 how many arguments, 14 the value is there
    u32 args[7];
    u32 value;

    void Clear();
    // Whether it holds a message (its kind isn't 0).
    bool HasText();
    // Sets its number and kind, with no arguments yet.
    void Setup(u16 kind, u16 id);
    // Adds an argument, up to 8 (an eighth goes where the value is).
    void AddArg(u32 arg);
};
DECOMP_OFFSET(BattleMessage, args, 4);
DECOMP_OFFSET(BattleMessage, value, 0x20);
// Messages with their arguments in an array: ServerCommands::PutMessage's kinds 0x7b (PutMessageArgs) and 0x7c
// (PutPokemonMessageArgs), and 0x7d and 0x7e with a value.
struct Flow;
void PutMessageArgs(Flow* flow, u16 message, u32 count, const u32* args);
void PutPokemonMessageArgs(Flow* flow, u16 message, u32 count, const u32* args);
void PutMessageArgsWithValue(Flow* flow, u16 message, u32 value, u32 count, const u32* args);
void PutPokemonMessageArgsWithValue(Flow* flow, u16 message, u32 value, u32 count, const u32* args);
// Makes a condition's damage message (burn, poison, Nightmare, Curse), naming the Pokémon.
bool SetConditionDamageMessage(BattleMessage* message, BattlePokemon* pokemon, u32 condition);

// Something the battle queues to happen (Flow::NewRecord).
struct BattleRecord {
    u8 unk0[4];
    u32 condition;         // for kind 10: the condition that ends
    u8 pokemon;
    u8 unk9[0xb];
    u8 unk14;
};
DECOMP_OFFSET(BattleRecord, condition, 4);
DECOMP_OFFSET(BattleRecord, unk14, 0x14);

// Records of other kinds, after the header Flow::NewRecord writes (the kind, the Pokémon, the size).
// Kind 3: a message.
struct MessageRecord {
    u32 header;
    BattleMessage message;
};
DECOMP_OFFSET(MessageRecord, message, 4);
// Kind 5: HP restored.
struct HealRecord {
    u32 header;            // bit 24: drained from another Pokémon (Leech Seed)
    u16 amount;
    u8 pokemon;            // the Pokémon it goes to
    u8 from;               // the Pokémon it's drained from, or kNoPokemon
    u8 unk8[4];
    BattleMessage message;
};
DECOMP_OFFSET(HealRecord, amount, 4);
DECOMP_OFFSET(HealRecord, message, 0xc);
// Kind 6: damage that doesn't come from a move.
struct DamageRecord {
    u32 header;
    u32 cause;             // 0x24 Leech Seed, 0x25 a binding move
    u16 amount;
    u8 pokemon;
    u8 causedBy;           // the Pokémon that caused it
    u8 flags;              // bit 1: an effect plays
    u8 unkd;
    u16 effect;            // the battle effect: 0x45 Leech Seed, 0x46 to 0x4e the binding moves
    u8 effectFrom;         // the positions it plays from and to (4 none)
    u8 effectTo;
    u8 unk12[2];
    BattleMessage message;
};
DECOMP_OFFSET(DamageRecord, amount, 8);
DECOMP_OFFSET(DamageRecord, flags, 0xc);
DECOMP_OFFSET(DamageRecord, effect, 0xe);
DECOMP_OFFSET(DamageRecord, message, 0x14);
// Kind 0x14: a Pokémon (Leech Seed's seeder, after it drains).
struct PokemonRecord {
    u32 header;
    u8 pokemon;
};
DECOMP_OFFSET(PokemonRecord, pokemon, 4);

// The field: weather and field effects.
struct Field {
    u8 GetWeather();
    // Whether a field effect is in force (2 Gravity, 3 Imprison, 6 Wonder Room, 7 Magic Room; 0 for one
    // above 10).
    bool HasEffect(u32 effect);
    // Whether a Pokémon that used Imprison against this one knows the move.
    bool IsImprisoned(Setup* setup, PokemonList* pokemon, BattlePokemon* user, u32 move);
};

// The battle's parameters (Setup+0x80).
struct BattleParams {
    u32 unk0;              // Setup::GetUnk80Value
    u8 unk4[0x49d];
    u8 unk4a1;             // when set, moves with flag 0xe fail (reason 0x1a)
};
DECOMP_OFFSET(BattleParams, unk4a1, 0x4a1);

// The battle's setup.
struct Setup {
    // Whether a rule is on; always false in Let's Go (it ends in 0x71009ce420, which returns 0).
    bool HasRule(u32 rule);
    // The u32 its +0x80 points to (MakeOrderKey uses it for kind 4).
    u32 GetUnk80Value();
    // The friendship that gives the friendship effects (200 or more dodges moves, 255 doubles the chance of a
    // critical hit; +0x2e2), or 0: only the player's Pokémon have it, and only when the u32 at +0x80 is at most 1.
    u8 GetFriendship(BattlePokemon* pokemon);
    // The u32 at +0x168.
    u32 GetUnk168();
    // Whether a client (0 to 3: the player, the opponent and their partners) takes part in this battle.
    bool HasClient(u8 client);
    // The side a client is on (SideOfClientIn).
    u8 SideOfClient(u8 client);
    // Whether a side takes part in this battle (HasSideIn).
    bool HasSide(u8 side);

    u8 unk0[0x80];
    BattleParams* params;
    u8 unk88[0xe0];
    u32 kind;              // the battle's kind (GetUnk168)
};
DECOMP_OFFSET(Setup, params, 0x80);
DECOMP_OFFSET(Setup, kind, 0x168);
// A client's side in a battle of a kind: the low bit of its number (0 the player's side, 1 the opponent's), 0 for
// a number above 3. Let's Go doesn't look at the kind.
u8 SideOfClientIn(u32 kind, s32 client);
// Whether a side takes part in a battle of a kind: 0 and 1 do.
bool HasSideIn(u32 kind, u8 side);

// Something at Flow+0x1798 that No Guard needs: up to 4 entries of {on, ?, Pokémon ID}.
struct Unk1798 {
    u8 entries[4][3];

    // Whether the Pokémon has an entry that's on and the setup's check for that entry's number says yes.
    bool Check(Setup* setup, u8 pokemon);
    // The number of the Pokémon's entry, 4 when it has none.
    u8 IndexOf(u8 pokemon);
};

// What the battle tells its clients (the screen): a command and its arguments.
struct ServerCommands {
    void Put(u32 command, ...);
    // A message (kind 0x7b, or 0x7c starting with a Pokémon's name) and its arguments, ended by 0xffff0000.
    void PutMessage(u32 kind, u32 message, ...);

    u64 unk0;              // RunTurn clears it first
};

// Something at Flow+0x2808 that the turn order saves and restores around its events.
struct Unk2808 {
    u32 Save();
    void Restore(u32 saved);
};

// Up to 5 numbers that the stat change check adds why a change failed to, while it's on (Flow at +0x2c1c).
struct FailureList {
    void Add(u32 reason);

    u32 entries[5];
    u32 count;
    bool on;
};

// Something whose +0x20 turns on event 0x5c's ratio for the damage (Flow at +0x24e0).
struct DamageRatioSwitch {
    u8 unk0[0x20];
    u32 on;
};

// The battle's random numbers (xoroshiro128+).
struct RandomGenerator {
    // 0 to range - 1.
    u64 Next(u64 range);
};
extern RandomGenerator* g_random;

// The kinds of action (Action::info's low 4 bits) that the decompiled code tells apart
constexpr u32 kActionMove = 1;

// An action of the turn (0x18 bytes).
struct Action {
    BattlePokemon* pokemon;
    u32 info;              // its kind in the low 4 bits, then what it is
    u8 unkc[4];
    u32 key;               // its place in the turn (MakeOrderKey)
    u8 unk14;              // MakeOrderKey's unk for kind 4
    u8 unk15[2];
    u8 recalculate;        // OrderActions(onlyMarked) works out only these again

    // The move, for an action of kind 1 (0 otherwise). Not decompiled.
    u32 GetMove();
};
DECOMP_OFFSET(Action, key, 0x10);
DECOMP_OFFSET(Action, recalculate, 0x17);
DECOMP_SIZE(Action, 0x18);

// The battle as it runs: every handler gets it.
struct Flow {
    // The battle's Pokémon with this ID (0x1f and others that aren't there give null).
    BattlePokemon* GetPokemon(u8 id);
    // Whether a field effect is in force (6 is Wonder Room, 7 stops items working: Magic Room).
    bool HasFieldEffect(u32 effect);
    // A new record of a kind, about a Pokémon, to fill in and push.
    BattleRecord* NewRecord(u32 kind, u8 pokemon);
    void PushRecord(BattleRecord* record);
    // The Pokémon at a position (unk1798's entry), kNoPokemon when there's none.
    u8 PokemonAtPosition(u32 position);
    // A Pokémon's position, 4 when it has none.
    u8 PositionOf(u32 pokemon);

    u8 unk0[0x58];
    void* unk58;
    Setup* setup;
    PokemonList* pokemon;
    ServerCommands* commands;
    u8 unk78[8];
    u32 unk80;             // RunTurn makes the state 7 when it reaches a limit the setup gives
    // What the battle does next (RunActions sets it): 0 the next turn, 2 once the clients with an empty
    // position are told (server command 0x31), 4 a side is out; 1 and 6 stop the turn's actions too; 3 and 5
    // aren't known.
    u32 state;
    Field* field;
    u8 unk90[0x12c4];
    u8 unk1354[0x17c];
    void* unk14d0;
    u8 unk14d8[0x1c];
    // The clients (0 to 3) with an empty position, their count first (0x71009f2c00 lists them).
    u8 emptyClients[5];
    u8 unk14f9[7];
    u32 statChangeNumber;  // the last added stat change's number (the next is 1 after -1)
    u8 turnStarted;        // RunTurn has made the turn's actions
    u8 unk1505[3];
    u8 actionCount;
    u8 nextAction;         // the action RunActions goes on from (RunTurn keeps it)
    u8 reorderFrom;        // with reorder: the action from which the rest are put in order again
    bool reorder;
    u8 unk150c[5];
    u8 unk1511;            // 0x20: the action failed (with a message), 0x80: without one
    u8 unk1512[0x2e];
    Action actions[25];    // actionCount of them (room up to unk1798 assumed)
    Unk1798 unk1798;
    u8 unk17a4[0xd3c];
    DamageRatioSwitch* damageRatioSwitch;
    u8 unk24e8[0x238];
    BattleMessage message;
    u8 unk2744[0xc0];
    // How the Pokémon acting now disobeys (0x71009f4b20): 0 it doesn't, 2 it naps, 3 it hurts itself, 4 it
    // loafs around.
    u32 disobedience;
    Unk2808 unk2808;
    u8 unk2809[0x2cc];
    u8 unk2ad5[4];         // RunTurn clears them when it starts a turn
    u8 unk2ad9[0x143];
    FailureList statFailures;
};
DECOMP_OFFSET(Flow, setup, 0x60);
DECOMP_OFFSET(Flow, pokemon, 0x68);
DECOMP_OFFSET(Flow, commands, 0x70);
DECOMP_OFFSET(Flow, unk80, 0x80);
DECOMP_OFFSET(Flow, state, 0x84);
DECOMP_OFFSET(Flow, field, 0x88);
DECOMP_OFFSET(Flow, unk1354, 0x1354);
DECOMP_OFFSET(Flow, unk14d0, 0x14d0);
DECOMP_OFFSET(Flow, emptyClients, 0x14f4);
DECOMP_OFFSET(Flow, statChangeNumber, 0x1500);
DECOMP_OFFSET(Flow, turnStarted, 0x1504);
DECOMP_OFFSET(Flow, actionCount, 0x1508);
DECOMP_OFFSET(Flow, reorder, 0x150b);
DECOMP_OFFSET(Flow, actions, 0x1540);
DECOMP_OFFSET(Flow, unk1511, 0x1511);
DECOMP_OFFSET(Flow, unk1798, 0x1798);
DECOMP_OFFSET(Flow, damageRatioSwitch, 0x24e0);
DECOMP_OFFSET(Flow, message, 0x2720);
DECOMP_OFFSET(Flow, disobedience, 0x2804);

// Takes damage that doesn't come from a move off a Pokémon (events 0x99 and 0x9a: Emergency Exit), telling the
// clients with the message (may be null); cause is what caused it (variable 0x6a), causedBy the Pokémon.
u32 ApplyIndirectDamage(Flow* flow, BattlePokemon* pokemon, u32 damage, u32 cause, u32 causedBy,
                        BattleMessage* message, bool unk);
// Whether an ability can't be taken away or changed (Gastro Acid fails on it).
bool IsAbilityPermanent(u32 ability);
// Sets a condition with its 8 bytes on the target.
void SetCondition(Flow* flow, BattlePokemon* target, u32 condition, const u64* conditionData);
// Makes the message for a condition given (in the conditions' file).
void MakeConditionMessage(u32 condition, const u64* conditionData, BattlePokemon* pokemon, BattleMessage* message);
// What follows a condition given: for a status (event 0x7d and the like), for another condition, and when Gastro
// Acid stops Unnerve (the berries held can be eaten again, probably).
void StatusAdded(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, u32 condition, u64 conditionData);
void ConditionAdded(Flow* flow, BattlePokemon* target, BattlePokemon* attacker, u32 condition);
void UnnerveSuppressed(Flow* flow, BattlePokemon* pokemon);
// When the Pokémon holds an item, event 0xaf at this point (variable 0x34): its item may react (berries).
void CallItemReaction(Flow* flow, BattlePokemon* pokemon, u8 timing);
// Takes a Pokémon's ability handlers away (kind 4), or adds them for the ability it has.
void RemoveAbilityHandlers(BattlePokemon* pokemon);
void AddAbilityHandlers(BattlePokemon* pokemon);
// A move's stat changes on its defender (or its user), numbered.
struct MoveParam;
void ApplyMoveStatChanges(Flow* flow, const MoveParam* move, BattlePokemon* attacker, BattlePokemon* defender,
                          u32 number, bool unk5, bool unk6);
// Whether a side has no Pokémon left that can fight: a side taking part (0 or 1) none of whose clients taking part
// has one in its party.
bool IsSideOut(Flow* flow);
// Whether a battle of this kind (Setup::GetUnk168) is over once a side is out: always true in Let's Go.
bool EndsWhenSideOut(u32 kind);
// Runs the turn's actions from the first given on, in order (putting the rest in order again first at
// reorderFrom), stopping when a side is out or the state says so; then the end of the turn, and the next
// state. Gives the action to go on from (actionCount when all ran).
u32 RunActions(Flow* flow, u32 from);
// Runs the turn, or goes on with it: the first time (turnStarted not set) it gets the turn ready and makes its
// actions (0x71009f1340, from choices: likely what the clients chose); then RunActions from nextAction, and after
// it, when the state is 0, 4, 5 or 6, what ends the turn (0x71009f1760); a turn that ends with state 0 makes it 7
// when unk80 reaches the setup's limit (0 for none). Gives the state.
u32 RunTurn(Flow* flow, void* choices);
// Makes the turn's actions (0x71009f1c30, not decompiled: from choices) and puts them in order. An action of kind 6
// marks the move (kind 1) of its Pokémon to be ordered again, and the first action of kind 1 or 5 is where
// RunActions does that (reorder and reorderFrom; reorder stays false without one).
void MakeActions(Flow* flow, void* choices, Action* actions, u8* count, bool* reorder, u8* reorderFrom);
// Runs an action of the turn (not decompiled).
void RunAction(Flow* flow, Action* action);
// The end of the turn's steps (flow+0x1506 counts them; not decompiled): 0 when they're done, 1, 2 or 3 when
// one stops them.
u32 EndOfTurn(Flow* flow);
DECOMP_OFFSET(Flow, unk2808, 0x2808);
DECOMP_OFFSET(Flow, unk2ad5, 0x2ad5);
DECOMP_OFFSET(Flow, statFailures, 0x2c1c);

// A move as the battle uses it.
struct MoveParam {
    u32 id;
    u32 chosenId;          // the move whose PP it uses (the one chosen, when another move turned into this)
    u8 unk8[2];
    u8 type;
    u8 unkb;
    u32 category;
    u8 unk10;              // variable 0x65
    u8 unk11[7];
    u32 unk18;             // variable 0x1c
    u16 power;
    u8 unk1e[2];
    u32 unk20;             // variable 0x17; when set, Disable, Taunt, Gravity and the like don't stop it
};
DECOMP_OFFSET(MoveParam, type, 0xa);
DECOMP_OFFSET(MoveParam, category, 0xc);
DECOMP_OFFSET(MoveParam, unk18, 0x18);
DECOMP_OFFSET(MoveParam, power, 0x1c);
DECOMP_OFFSET(MoveParam, unk20, 0x20);

}  // namespace battle
