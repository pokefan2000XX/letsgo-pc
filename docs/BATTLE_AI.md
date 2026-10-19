# Battle AI scripts

How a trainer's Pokémon picks its move: seven Pawn scripts in `bin\battle\ai_script\`, run by the
battle code with one command of their own. They're the same format as the field scripts, so
`tools/amx.py` reads and writes them ([SCRIPTS.md](SCRIPTS.md)): all 7 come back byte for byte
from source. This is roadmap phase 5, step 5 ([PLAN.md](PLAN.md)).

| Script | Size |
|---|---|
| `btl_ai_basic.amx` | 32,742 bytes |
| `btl_ai_double.amx` | 28,541 |
| `btl_ai_expert.amx` | 70,956 |
| `btl_ai_item.amx` | 1,608 |
| `btl_ai_pokechange.amx` | 2,387 |
| `btl_ai_strong.amx` | 2,646 |
| `btl_ai_allowance.amx` | 376 |

```
python -P tools/amx.py disasm orig/romfs/bin/battle/ai_script/btl_ai_basic.amx -o build/btl_ai_basic.pasm
python -P tools/amx.py asm build/btl_ai_basic.pasm -o <mod>/romfs/bin/battle/ai_script/btl_ai_basic.amx
python -P tools/amx.py commands --ai          the AI_CMD subcommands below
```

## How they're built

Each script registers one command, `AI_CMD` (`0x7100a74140`), and calls it through a small
function of its own (`f_0010` in the basic script) that hands it five values after the public
variable at data 0 (the handler takes its low byte to find the AI it's running for). The first
of the five picks what `AI_CMD` does: 0 to 122, a switch in the handler (anything above returns
0). The rest are its arguments, and what it gives back is the call's value: 1 or 0 for the `If`
ones, a number for the `Get` ones.

```
    push.c 0
    push.c 0
    push.c 0
    push.c 0
    push.c 57
    push.c 40
    call f_0010          ; AI_CMD 57: GetBattleRule
```

`amx.py disasm` adds the comment after each such call, naming the subcommand: 3,237 of the
3,248 calls in the 7 scripts (the other 11 pass one another function worked out). The comment is only a
note: `asm` ignores it.

Where a subcommand asks about a Pokémon, its first argument says which (`who`): 0 the target,
1 the AI's own Pokémon, 2 the target's partner, 3 its own partner, 4 a Pokémon the AI is
checking (`0x7100a72310`). "The move" is the move the AI is weighing. Moves, types and
abilities are the game's numbers (types from Normal 0 to Fairy 17).

No subcommand changes a score: the scripts add to a public variable themselves (at data 8 in
the basic script, through `f_00b8`), most likely the move's score.

## The subcommands

From the handler's switch. A name ending in a number, or one that says what it reads (an
address, a field), is a placeholder: the code was read but not understood, so check it before
relying on it. Let's Go's Pokémon hold no items, so the item subcommands always give 0.

| # | Name | Arguments | What it gives |
|---|---|---|---|
| 0 | `IfRandomLessThan` | n | a random number 0-255 is below n |
| 1 | `IfRandomGreaterThan` | n | a random number 0-255 is above n |
| 2 | `IfRandomEqual` | n | a random number 0-255 is n |
| 3 | `IfRandomNotEqual` | n | a random number 0-255 isn't n |
| 4 | `IfHpPercentBelow` | who, percent | its HP percent (rounded down) is below |
| 5 | `IfHpPercentAbove` | who, percent | its HP percent (rounded down) is above |
| 6 | `IfHpPercentEqual` | who, percent | its HP percent (rounded up) is |
| 7 | `IfHpPercentNotEqual` | who, percent | its HP percent (rounded up) isn't |
| 8 | `IfHasStatus` | who | it has a status (paralysis, sleep, freeze, burn, poison) |
| 9 | `IfNoStatus` | who | it has no status |
| 10 | `IfHasCondition` | who, condition | a condition is on (BattlePokemon::HasCondition) |
| 11 | `IfNotCondition` | who, condition | a condition isn't on |
| 12 | `IfBadlyPoisoned` | who | its poison is bad poison |
| 13 | `IfNotBadlyPoisoned` | who | its poison isn't bad poison |
| 14 | `IfFlag` | who, flag | one of its flags (bits at +0x303) is set |
| 15 | `IfNotFlag` | who, flag | that flag isn't set |
| 16 | `IfSideCondition` | who, condition | its side has a side condition (by number) |
| 17 | `IfNoSideCondition` | who, condition | its side hasn't |
| 18 | `GetConditionValue20` | who | condition 0x14's value when it's on, else 0 |
| 19 | `GetConditionCounter20` | who | condition 0x14's counter when it's on, else 0 |
| 20 | `GetChoiceMove` | who | the move a Choice item holds it to (condition 0x1c's value), else 0 |
| 21 | `IfHasDamagingMove` | - | the AI's Pokemon knows a move with power |
| 22 | `IfHasNoDamagingMove` | - | it knows none |
| 23 | `GetTurnCount` | - | the battle's turn count |
| 24 | `GetType` | which | a type: 0/2 the target's first/second, 1/3 its own, 4 the move's, 5/7 the target's partner's, 6/8 its partner's |
| 25 | `IfCanUseMove` | who, move | it knows the move, has PP for it, and nothing stops it |
| 26 | `IfMoveHasPower` | move | the move has power (isn't a status move) |
| 27 | `GetMovePower` | - | the move's power |
| 28 | `CompareMoveDamage` | - | 2 when the move does the most damage of the AI's moves, 1 when another does more, 0 when it does none |
| 29 | `GetLockedMove` | who | the move it's locked into (condition 0x1a), else 0 |
| 30 | `IfFaster` | which | speeds: 0 the AI's Pokemon is faster, 1 the target is, 2 the same |
| 31 | `CountPartyAbleToFight` | who | how many of its side's Pokemon not in battle can fight |
| 32 | `GetMoveField28` | - | the move's data field 0x1c |
| 33 | `GetAbility` | who | its ability: its own side's real one; an opponent's as shown, else one its species can have |
| 34 | `IfEffectiveness` | attacker, defender, move, value | type effectiveness (7 neutral, each step up doubling): 9 means above 8, 5 below 6, else equal |
| 35 | `GetEffectiveness` | attacker, defender, move | the move's type effectiveness (7 neutral; 7 when it can't tell) |
| 36 | `IfHasMoreEffectiveMove` | who, target, value | it has a usable move whose effectiveness on the target is above value |
| 37 | `IfHasMoveOfEffectiveness` | who, target, value | it has a usable move whose effectiveness on the target is value |
| 38 | `IfPartyHasStatus` | who | a Pokemon of its side not in battle has a status |
| 39 | `IfPartyHasNoStatus` | who | none has |
| 40 | `GetWeather` | - | the weather (0 when Air Lock suppresses it) |
| 41 | `IfValueBelow` | who, value, n | a value (BattlePokemon::GetValue: 1-7 stages with 6 none, 8 Attack, 9 Defense, 10 Sp. Atk, 11 Sp. Def, 12 Speed, 13 HP, 14 max HP, 15 level, 17 ability) is below n |
| 42 | `IfValueAbove` | who, value, n | the value is above n |
| 43 | `IfValueEqual` | who, value, n | the value is n |
| 44 | `IfValueNotEqual` | who, value, n | the value isn't n |
| 45 | `IfMoveKnocksOut` | - | the move's damage, as the AI works it out, is at least the target's HP |
| 46 | `IfMoveDoesntKnockOut` | - | it isn't |
| 47 | `IfKnowsMove` | who, move | it knows the move (an opponent: one the AI has seen it use) |
| 48 | `IfDoesntKnowMove` | who, move | it doesn't |
| 49 | `If76ed0` | who, x | 0x7100a76ed0 |
| 50 | `IfNot76ed0` | who, x | not 0x7100a76ed0 |
| 51 | `Stop` | - | sets the AI's done flag (+0x220); returns 0 |
| 52 | `GetHeldItem` | who | its held item: always 0 in Let's Go |
| 53 | `GetHeldItemEffect` | who | its held item's effect: 0 |
| 54 | `GetValue18` | who | value 0x12 |
| 55 | `IfFlag0` | who | flag bit 0 at +0x303 |
| 56 | `GetCounter0` | who | counter 0 at +0x307 |
| 57 | `GetBattleRule` | - | the battle's rule (1 to 3 have partners) |
| 58 | `GetBattleKind` | - | a value of the battle's setup (+0x80); 113 asks whether it's 1 |
| 59 | `GetZero` | who | always 0 (0x71009e6da0) |
| 60 | `GetMoveType` | - | the move's type |
| 61 | `GetMovePower2` | - | the move's power, as 27 |
| 62 | `GetMoveField28_2` | - | the move's data field 0x1c, as 32 |
| 63 | `GetCounter3` | who | counter 3 at +0x307 |
| 64 | `IfHigherLevel` | which | levels: 0 the AI's Pokemon's is higher, 1 the target's is, 2 the same |
| 65 | `IfTargetHasCondition11` | - | condition 0xb is on the target |
| 66 | `IfTargetHasCondition11_2` | - | the same as 65 |
| 67 | `IfPositionsLinked` | - | 0x71009e2000 on its own and another position |
| 68 | `IfHasType` | who, type | one of its three types is type (18, none, never) |
| 69 | `IfAbility` | who, ability | its ability, as 33, is |
| 70 | `IfFlag13` | who | flag bit 0xd at +0x303 |
| 71 | `IfHeldItem` | who, item | its held item is item (always 0 in Let's Go) |
| 72 | `IfFieldEffect` | effect | a field effect is on (6 Wonder Room, ...) |
| 73 | `GetSideCondition` | who, condition | the side condition's count (layers or turns) |
| 74 | `IfPartyHasHurt` | who | a Pokemon of its side not in battle can fight and has lost HP |
| 75 | `IfPartyHasUsedPp` | who | a Pokemon of its side not in battle has used PP |
| 76 | `GetHeldItemValue6` | who | its item's value 6 unless condition 0x13 (Embargo) is on: 0 in Let's Go |
| 77 | `GetMovePp` | - | the PP left of the move |
| 78 | `IfMovesUsed4800` | who | it knows more than one move and 0x71009e4800 is at least their count less one |
| 79 | `GetMoveCategory` | - | the move's category (0 status, 1 physical, 2 special) |
| 80 | `GetTargetLockedMoveCategory` | - | the category of the move the target is locked into |
| 81 | `GetValueA0a1f0` | who | 0x7100a0a1f0 for it |
| 82 | `GetValue2E8` | who | its 16 bits at +0x2e8 |
| 83 | `IfPartyHasHarderHitter` | flag | one of its side's Pokemon not in battle would hurt the target more |
| 84 | `IfHasSuperEffectiveMove` | who, target | one of its moves is super effective on the target |
| 85 | `IfLockedMoveHitsHarder` | who, flag | the move it's locked into would hurt the target more than the AI's Pokemon's best |
| 86 | `SumRaisedStages` | who | how many stages its 7 stats are raised, added up |
| 87 | `GetValueDifference` | who, value | its value less the AI's Pokemon's |
| 88 | `GetValue` | who, value | a value, as 41 |
| 89 | `ComparePartyMoveDamage` | flag | 2/1/0 as 28, over the side's Pokemon |
| 90 | `IfFainted` | who | it has fainted |
| 91 | `IfNotFainted` | who | it hasn't |
| 92 | `GetAbilityNow` | who | value 17: the ability it has now |
| 93 | `IfHasSubstitute` | who | it's behind a substitute |
| 94 | `GetSpecies` | who | its species |
| 95 | `GetForm` | who | its form |
| 96 | `IfGameValueBelow` | n | the value at 0x71016490f0 is below n |
| 97 | `IfGameValueAbove` | n | it's above n |
| 98 | `IfGameValueEqual` | n | it's n |
| 99 | `IfGameValueNotEqual` | n | it isn't n |
| 100 | `IfPositionEffect3` | who | 0x7100a0a570 with 3 for its position |
| 101 | `IfAttackBelowSpAtk` | who | its Attack is below its Sp. Atk |
| 102 | `IfAttackAboveSpAtk` | who | its Attack is above its Sp. Atk |
| 103 | `IfAttackNotSpAtk` | who | they differ |
| 104 | `IfFlag2_0` | who | flag bit 0 at +0x306 |
| 105 | `IfValue26E` | who, n | its byte at +0x26e is n |
| 106 | `IfHandler10` | n | 0x7100a689d0 with 10 and n |
| 107 | `GetValue2DE` | who | its 16 bits at +0x2de |
| 108 | `IfSetupFlag65` | - | a flag of the battle's setup (+0x80, +0x65) |
| 109 | `IfPokemonFlag728900` | who | 0x7100728900 on its Pokemon data |
| 110 | `If696e0` | who | 0x7100a696e0 for it |
| 111 | `IfState5420` | who | 0x71009e5420 isn't 0x18 |
| 112 | `GetHiddenPowerType` | who | its Hidden Power type |
| 113 | `IfBattleKind1` | - | 58 is 1 |
| 114 | `GetMax76fc0` | who | the larger of 0x7100a76fc0 against the target and the target's partner |
| 115 | `IfMoveAbsorbed` | who, move | its ability takes the move's type: Ground Levitate, Fire Flash Fire, Water Water Absorb/Storm Drain/Dry Skin, Grass Sap Sipper, Electric Volt Absorb/Lightning Rod/Motor Drive |
| 116 | `Get6f90` | who | 0x71009e6f90's value when it says yes, else 0 |
| 117 | `GetMove` | - | the move |
| 118 | `GetValue90` | - | the AI's 16 bits at +0x90 |
| 119 | `IfAbc0` | who | 0x7100a0abc0 with 1, 2 or 3 for it |
| 120 | `CountParty5b0` | who | 0x71009df5b0 on its side's Pokemon |
| 121 | `GetMove0500` | - | 0x71009d0500 for the move and the AI's Pokemon |
| 122 | `IfHasSuperEffectiveMove2` | who, target | as 84, through 0x7100a09a50 |

## Not yet

- The placeholders above.
- Changing the AI in the game: no script was changed and tried in a battle yet.
