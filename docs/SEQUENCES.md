# Battle sequences

A move's animation in battle is a sequence (`.bseq`): a timeline of commands that load particle
effects, models and camera animations, move the Pokémon, play sounds and so on, each from one
frame to another. Other battle effects are sequences too: a level up, a Poké Ball opening, the
weather. `tools/bseq.py` reads them, turns them into JSON and back. This is roadmap phase 5,
step 5 ([PLAN.md](PLAN.md)).

## Where they are

The 353 files of `bin\battle\waza\sequence\`:

| Name | What |
|---|---|
| `ew<move>.bseq` (228) | a move's animation, by its number in the move table (`ew033` Tackle, `ew084` Thunder Shock) |
| `ew<move>_<part>.bseq` (25) | a move's other parts (below) |
| `ee*.bseq` (66) | battle effects: `ee000` the empty one (no particles, models or sounds), `ee002` a level up, the `ee3xx` files the openings' camera work (`Battle_cutin`, with the `toujou` entrance cameras) |
| `em*.bseq` (16), `es*.bseq` (18) | more effects, not looked at yet |

The files they load are beside them: `particle\` (257 folders of `.ptcl`), `model\` and
`camera\`, named by path in the commands (`data:/bin/battle/waza/particle/ew033/ew033_df_hit.ptcl`).
The catch screen has its own sequences in the same format (`bin/app/capture/game/sequence/`, in
an archive), not read yet.

## Which file a move plays

The battle builds the path from the move's number (`0x7100a81470`):
`bin/battle/waza/sequence/ew%03d.bseq`. A move with more than one part plays
`ew%03d<part>.bseq` for its second part: `_attack` (moves 13, 19, 76, 91, 117, 130 and 143),
`_turn` (20, 73, 83 and 128), `_back` (46, 369 and 525), `_bomb` (120 and 153) and
`_barrierbreak` (280); moves 732 and 741 number theirs `_1` to `_3`.

Before loading it, the battle asks whether the file is there (`nn::fs::GetEntryType`). When
it isn't, it plays `ee000.bseq` instead, which has no particles, models or sounds: that's what a move without an
animation of its own does. The mods folder answers that question too, so a mod's `ew<move>.bseq`
gives a move an animation, whether the move had one or not, without any code. A new move (a
number above 742) also needs its own entry in the move table, which hasn't been tried.

## The format

All little-endian. A 16-byte header, the commands, then -1 (s32):

| Offset | What |
|---|---|
| 0x00 | `SESD` |
| 0x04 | version (u32): 3 in every file; the player reads the command fields below by it |
| 0x08 | 0 (u32) in every file |
| 0x0C | the last frame (u32): the sequence ends when it gets there |

Each command:

| Offset | What |
|---|---|
| 0x00 | start frame (s32) |
| 0x04 | end frame (s32), the same as the start for a command that happens once |
| 0x08 | `a` (u16): 0 to 44. A particle and the commands right after it share it, so it's probably the editor's row |
| 0x0A | `b` (u16): 0 in most commands, else 10 to 125 |
| 0x0C | `c` (u16): 0 in every file, which the player reads as a flag |
| 0x0E | the command (u16), 0 to 226 |
| 0x10 | its parameters |

The file doesn't give the parameters' size. The game has a table of them, one s32 per command
(`0x7100f97e30`, 227 commands, all multiples of 4), which `tools/bseq.py` reads from
`orig\exefs\main`. The commands' code is a table of 227 functions at `0x71015d4c70`, one per
command; each reads its parameters in order with a few small readers (below).

**How the game plays one** (`0x7100210510`, once a frame): from where it got to, it runs each
command whose start frame is the frame it's at, in the file's order, and stops at the first
whose start isn't. So the commands have to be sorted by start frame, as every file in the game
is: one put out of order stops itself and every command after it. `tools/bseq.py build` sorts
them (keeping the order of commands with the same start). The sequence ends at the header's
last frame.

## Commands

`python -P tools/bseq.py commands` lists all 227: the parameters' size, how often the game's
files use each (117 of them are used) and their types. Those whose strings make it plain what
they do:

| Command | What it holds |
|---|---|
| 54 | a particle file (`.ptcl`), then an int, true or false and three ints |
| 112 | a sound: a Wwise event (`Play_SE_w0068`, 64 bytes), then true or false |
| 113 | two sounds (voices: `Play_vo_bl_pk_battle_001_start`), then true or false |
| 44, 46 | a camera animation (`.gfbcam`) |
| 77 | a model (`.gfbmdl`), then true or false, two ints, true or false |
| 103 | a model's animation list (`.gfbanmcfg`) |
| 106 | a model's name |
| 17, 18, 170 | a character's animation, by name (`to_ba0041_sally02`) |
| 4, 60, 61, 68, 94 | a joint's name (`Origin`, `Waist`, 32 bytes) among numbers |
| 115 | the sequence's name (`Waza_ew0033`), always at frame 0 |
| 0 | nothing: 1,046 empty commands, kept as the editor left them |

**The parameters' types come from the game's code.** A command's code reads its parameters one
after another with a few small functions, each taking the next value from the file:

| Reader | Reads | Letter |
|---|---|---|
| `0x71002104f0` | an int (4 bytes) | `i` |
| `0x7100210680` | a float (4 bytes) | `f` |
| `0x71002106a0` | three floats, x, y and z (12 bytes) | `v` |
| `0x7100210740` | true or false (4 bytes, true when not 0) | `b` |
| `0x7100210770` | a 64-bit value (8 bytes; command 178 looks for an object with that key) | `h` |
| `0x7100210790` | a file's path (256 bytes, ended by a NUL) | `s256` |
| `0x71002106f0` | that many bytes: a name, 32 or 64 bytes here | `s32`, `s64` |

Three places read a command's parameters. The table at `0x71015d4c70` plays them, through the
function `0x7100aa0eb0`. Another function (`0x7100210880`) runs every command up to a given frame
at once, as when jumping ahead, and for those `0x7100aa0eb0` uses a switch instead, with code
for only commands 20, 44 to 48, 54, 77, 78, 103, 115 and 188. And a second table, at `0x71015d5388`, run
through `0x7100aa2290` by a second handler the battle sets up beside the first (`0x7100aa0b80`
makes both), has code for only four commands: 54 (a particle file), 77 (a model), 78 and 188.
Command 77's path is read only there and in the switch; its function in the first table is
empty. When the second handler runs hasn't been followed.

Listing those reads in order gives each command's types, and they add up to the size table's
for every command any of the three reads (where two or three read one, they agree). No read is
inside a branch, so each runs every time, in the order written, which is the order in the file:

- 132 commands with parameters have their types this way (107 of them in the game's files);
  `tools/bseq.py` has them as `SIGNATURES`, a letter per value (`54: "s256 i b i i i"`).
- 59 and 82 read 24 of their 28 bytes; the last 4 aren't read.
- 23 commands take no parameters (0, 55 and 56 among the ones the files use).
- 70 commands have parameters but no code reads them in this version: their functions are
  empty in all three places. Three are in the game's files: 11 twice, 189 once and 208 eight
  times; the battle does nothing with them.

Twelve commands that no file uses also take a name or a path: 26, 65, 83, 87 and 91 a joint's
(32 bytes), 99, 100 and 101 two names (32 bytes each), 45 and 48 two paths like 44's, 102 a path
and 114 a 64-byte name. What each command does with its values is known only for the few read
so far: 62 sets the particles of its row (`a`) to a scale ([x, y, z]), at once or gradually;
58 puts them at a Pokémon's place; 54 loads the particle file.

## The tool

```
python -P tools/bseq.py info <file.bseq>                 header and commands, one per line
python -P tools/bseq.py dump <file.bseq> [-o out.json]   editable JSON
python -P tools/bseq.py build <file.json> -o <file.bseq>
python -P tools/bseq.py roundtrip [file|dir ...]         every sequence: read, dump, build, compare
python -P tools/bseq.py commands                         the commands, with their parameters' types
```

The JSON has the header's values and one line per command (here from Quick Attack's):

```json
{"start": 16, "end": 16, "a": 0, "b": 0, "c": 0, "command": 112, "what": "sound (Wwise event)", "params": ["Play_SE_w0216", true]}
{"start": 17, "end": 65, "a": 8, "b": 0, "c": 0, "command": 54, "what": "particle file", "params": ["data:/bin/battle/waza/particle/ew098/ew098_at_bgkem.ptcl", 0, false, 0, 6, 0]}
{"start": 17, "end": 17, "a": 8, "b": 0, "c": 0, "command": 63, "params": [[0.0, 15.0, 0.0], true, 0]}
```

Each value has its type from the game's code (above): an int, a float (the shortest decimal
that is the same float, `0.3`), `[x, y, z]`, `true` or `false`, a 64-bit value, or a text. A
text can't be longer than its bytes less one. The bytes no code reads are ints. `build` checks
each value against its type and says which one is wrong; a whole number where a float goes is
that float. `what` is only a note: `build` ignores it. A value its type can't show (a float
that isn't a number, a "true or false" other than 0 or 1, a text with bytes after its end) puts
that command's bytes in `raw`, as ints, instead of `params`; none of the game's 17,011 commands
needs it.

All 353 files come back byte for byte, through `build` and through the JSON. Everything the
tool prints comes from the player's own game, so it stays on this machine like `orig\`.

## Checked in the game

Headless, from a copy of the Viridian Forest save, in wild battles (the `wild-battles` hack), on
2026-10-17:

- **Quick Attack with fire:** its sequence dumped to JSON, three particle files changed to
  Flamethrower's (`ew053_fire_muzzle`, `ew053_fire_ran`, `ew053_fire_hit`) and a sound to
  Flamethrower's (`Play_SE_w0102`), built back and put in a mod as
  `bin/battle/waza/sequence/ew098.bseq`. When Quick Attack was picked, the log showed the game
  reading the mod's file. Pikachu dashed as before, with a burst of fire where the unchanged
  move kicks up dust, and the target was hit with flames and embers where the unchanged move
  shows a white star (a run without the mod, the same day, for comparison).
- **Thunder Shock** (`ew084.bseq`, changed the same way) was read from the mod too, but the wild
  Oddish moved first and the shots ended before Pikachu's turn.

## Not yet

- What most commands do and what their values mean: their types are known (from the code), and
  each command's function is in the table at `0x71015d4c70`.
- The catch screen's sequences (same format, in an archive).
- A new move's entry in the move table, so a number above 742 can be used at all.
