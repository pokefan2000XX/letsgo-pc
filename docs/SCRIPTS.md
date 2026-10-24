# Scripts

Let's Go's events are Pawn scripts: talking to someone, picking up an item, a trainer spotting
the player, a story scene. `tools/amx.py` turns them into editable source and back, a mod
replaces or adds them, and a hack can give them new commands written in C++. This is roadmap
phase 6's script work ([PLAN.md](PLAN.md)); the commands themselves are written up in
[SCRIPT_COMMANDS.md](SCRIPT_COMMANDS.md).

## Where they are

- **Field scripts:** the 705 files of `bin\archive\script_amx.gfpak`. The archive stores each
  path only as a hash (FNV-1a 64 of `bin/script/amx/<name>.amx`). `tools/amx.py` names 703 of
  them: script_id_record.bin lists the event scripts' paths, and every zone in
  `ZoneNameHashTable.tbl` has a `<zone>.amx` and an `init_<zone>.amx`.
- **Battle AI:** the 7 scripts in `bin\battle\ai_script\` (`btl_ai_*.amx`), run by the battle
  code with its own command set (`battle_ai`): one command, `AI_CMD`, whose first argument picks
  one of 123 things (`0x7100a74140`), named in [BATTLE_AI.md](BATTLE_AI.md).

```
python -P tools/amx.py extract [outdir]             the field scripts, named, into build\scripts
python -P tools/amx.py disasm <file.amx> [-o out]   editable source
python -P tools/amx.py asm <file.pasm> -o <file.amx>
python -P tools/amx.py roundtrip [file|dir ...]     every script: disassemble, assemble, compare
python -P tools/amx.py commands [--uses]            the commands, with --uses their calls and arguments
python -P tools/amx.py info <file.amx>              header and tables
```

All 712 scripts come back from source byte for byte. Everything the tool prints is derived from
the player's own copy of the game, so it stays on this machine like `orig\`.

## Making a script mod

1. `python -P tools/amx.py extract`, then `python -P tools/amx.py disasm build\scripts\t001r0101.amx -o t001r0101.pasm`.
2. Edit the source.
3. `python -P tools/amx.py asm t001r0101.pasm -o mods\my-mod\romfs\bin\script\amx\t001r0101.amx`.

The mods folder puts it into `script_amx.gfpak` in place of the game's ([MODS.md](MODS.md)); a
script with a new name is added to the archive. A new event script also needs a record in
`bin\script\param\script_id\script_id_record.bin` and something that starts it (below; [A
cutscene of your own](#a-cutscene-of-your-own) goes through all of it).

Checked in the game (headless, from a save in the bedroom): the bedroom's two scripts, edited to
log their event through a new command, ran with the events below, and a script written from
scratch (frame counting, then `PokefanLog(count, PokefanAdd(count, 1000))` on its 60th run)
logged `60 1060`.

## How a script starts

Every event has a 64-bit **event hash**. The game runs a script by looking the hash up in
`script_id_record.bin` (0x7100194890, an unordered map loaded by 0x71001783e0), which gives the
AMX path and the event's message file. It loads the AMX (from a cache, or from
`script_amx.gfpak`), writes the hash into the script's public variable `g_mode` (hash
0xd7477c97, data address 0) and runs `main`, which switches on it:

```
main:
    proc
    load.p.pri 0                ; g_mode: the event
    switch t_0a28
...
t_0a28:
    casetbl
        default l_09f0          ; no such event: printf("scrid =%d Not Found \n", ...)
        case 0x846bfefe68855048 l_09a0
        case 0x14d1fe7a8c39292d l_0928
```

script_id_record.bin is a FlatBuffer: field 0 of the root is the list of records, and a record
is {0: the event hash (u64), 1: the AMX path, 2: the message path}. It has 1,395 records. The
event hashes are opaque: hashes of names the game no longer has, and any unused u64 works for a
new event. The empty hash (FNV-1a 64 of "", 0xcbf29ce484222645) means no script.

**What gives the hash:**

- **Placed things.** Field 11 of a placement entry's Transform table (the first table of every
  NPC, object, item, trigger and area, see phase 3) is its event hash. The field object keeps it
  at +0x190, beside its own name hash (field 9, +0x188), which the script reads with
  `GetTargetFieldObjectID()`.
- **The field's dispatcher**, 0x71002f8a50, each frame, first match wins:
  1. a reserved script (`ReserveScript`/`WeakReserveScript`, up to four u32 arguments)
  2. a scene change (below)
  3. GO Park's fixed event
  4. automatic triggers: the player inside a trigger's shape whose condition holds (its scene
     work equals the value it was placed with)
  5. trainer sight: the fixed event 0x233d326bbb672632 in trainer.amx, with the trainer
  6. talking (A): trainers, NPCs, objects and talk areas within reach, facing them; their own
     event hash, or trainer.amx's for a trainer without one; items run fld_item.amx's event
  7. stepping on or touching an object that runs its event
  8. field moves and the partner: hiden_ougi, encount_item, partner_poke, by fixed hashes
- **Scene changes.** A zone script can have the public function `GetSceneChangeData(index)`.
  When the zone loads, 0x710019b490 calls it with 0, 1, 2, ... and takes four cells at a time
  until the first is -1: {event, scene work hash, value, extra}. The dispatcher runs the first
  event whose work has that value while the player is in the zone. Story events (main_event_*)
  start this way.
- **Entering a zone.** The zone's `init_<zone>.amx` runs with `g_mode` 0 when the zone is
  entered (0x710017cc70) and with 1 over and over while the field runs (0x710017cd10; about
  twice a frame in the test, from loading a save onward, and loading a save didn't run 0).
- **System events** of `init_scr.amx`, by fixed hashes (0x7100177650).

**Waiting.** A script runs until it returns, or until it executes `halt 12` (Pawn's "sleep"),
which hands control back to the game; the game continues it there the next frame (0x7100674d40
keeps continuing while the result is 12). The scripts wait through one function, called with a
frame count:

```
f_0008:    ; 1 argument
    proc
    _Suspend(arg0)
    zero.pri
    zero.alt
    halt 12
    zero.pri
    retn
```

and wait for something to finish by polling it:

```
l_1110:
    IsMsgWinEnd_()
    eq.p.c.pri 1
    jzer l_1150
    zero.pri
    retn
l_1150:
    f_0008(1)
    jump l_1110
```

**Arguments.** An event has a target (the object's name hash, `GetTargetFieldObjectID()`) and
up to four u32 arguments (0x710017a750, the central "run event" function; a trainer's talk
passes its trainer hash and number), which the script reads with `TempWorkGet(0)` to
`TempWorkGet(3)`.

## A cutscene of your own

`hacks\new-cutscene\` is a whole one, the roadmap's phase 6 check. A trigger of its own in the
bedroom starts an event of its own. Its script fades the screen out and in, turns the player,
shows two lines of its own, walks the player back out of the trigger, and logs its start and end
through the new commands. `python -P hacks\new-cutscene\make_romfs.py` makes its files from your
copy of the game. Run it with `--mods hacks\new-cutscene` and an exe built with the
`new-script-command` hack. It takes five things:

1. **An event.** It's a name of your own (`POKEFAN_CUTSCENE_01`), and the hash is its FNV-1a 64
   (0x0f6e60b4b04f288c).
2. **A record** in `script_id_record.bin`: the hash, `bin/script/amx/pokefan_cutscene.amx` and
   `script/pokefan_cutscene.dat`.
3. **A trigger** in the zone's placement file (`t001r01.bin` for zone t001r0102, in
   `placement.gfpak`). It's an entry in the zone's list of scene triggers (field 14), a table
   holding the trigger:

   | Field | What | The new trigger |
   |---|---|---|
   | 0 | its transform: the position, its own name's hash (field 9), the flag that hides it (10) and the event (11) | (6650, 0, 573), `FNV("t001r0102_pokefan_cutscene")`, `FNV("")`, the event |
   | 1 | the shape: 1 a circle with field 3 as its radius (13 of the game's 126 triggers), 2 a box (113) | 2 |
   | 2 | the shape's transform; a box's size in cm is its scale (200 × 100 × 50 in most) | 200 × 100 × 300 |
   | 3 | a float: a circle's radius (145 to 350); 1.0 in 102 of the boxes, 130 to 700 in the others (not known) | 1.0 |
   | 4, 5 | the condition: a scene work's hash and the value it must have | `FNV("")` and 0: always |
   | 6 | a byte: 1 has the trigger wait until the player faces the way its shape is turned (below); 1 in the game's 19 elevator triggers, 0 in all the others | 0 |
   | 7 | a byte: 1 in 2 triggers (not known) | 0 |

   The trigger runs its event whenever the player is in it and the condition holds. If nothing
   changes, it runs again as soon as the event ends: without the walk back, the new one ran again
   0.13 s after each end. The game's story triggers avoid that by naming a work that their script
   then changes (the bedroom's two: work 0x00188d41bb7b57fb at 10 and at 1000). The new trigger's
   script walks the player out of the box instead.

   Field 6 set to 1 has the trigger run only while the player faces the way its shape is turned
   about y (the y of field 2's rotation): 0 towards the camera, 90 right, 180 away from the camera,
   270 left. The game's 19 triggers that set it are the elevators' (`elevator.amx`): boxes 80 to
   100 cm wide and 30 to 50 deep, their shapes turned 180, so the player faces the back wall. The
   other 107 leave field 6 at 0, and all but five of them leave their shapes unturned. Checked in
   the game (headless, a copy of the bedroom save, the new trigger with field 6 set): turned 90, it
   ran 0.5 s after the player walked right into it; turned 270, walking right didn't run it; turned
   180, walking right didn't, and a tap up, turning the player in the box, ran it 0.4 s later;
   turned 0, walking right didn't, and a tap down ran it 0.4 s later. A first try with field 6
   set and the shape unturned had the player walk in facing right and press A, so it never ran.
   make_romfs.py reads the placement file with `tools/flatbuf.py`, writes the longer list after
   the file's end, and points the zone at it.
4. **The script** (`pokefan_cutscene.pasm`). Its `main` switches on `g_mode` as the game's
   scripts do. Its data starts with the game's `.cell 0 0 99 99`, a cell for each public
   variable. `g_mode` is at address 0, which is where a script with no `.data` gets its strings;
   the assembler refuses that. The steps:
   - `FadeOut_(20, GetFnvHash64("BLACK"), 0, 1)`, then `FadeWait_()` until it returns 1; the
     same with `FadeIn_(20, ...)`;
   - `StartTurnAround_(player, 0, 0)`, then `CheckActionCommandRunningState_(player)` until it
     returns 0. Angles are absolute: at 0 the player faces the camera;
   - for each line: `MsgWin_(0, 0, label, 8, 0, 0, 0)`, `IsMsgWinEnd_()` until it returns 1,
     `ShowMsgWinCursor_(1)`, `ABKeyWait_()`, `ShowMsgWinCursor_(0)` and
     `PostSoundEventSE(0x62efcd1a)`; then `MsgWinClose()`;
   - `StartForceMove_(player, 1, 6380.0, 573.0, 0, 0, 0, 1)`, which turns the player toward
     (6380, 573) and walks there, then the same wait.

   The player's object hash is 0x7a29438e2b197b00. A float is a double in a cell, so 6380.0 is
   0x40b8ec0000000000.
5. **Its lines** (`pokefan_cutscene.txt`), built with `tools/msg.py` into
   `bin\message\English\script\pokefan_cutscene.dat` and its `.tbl`.

Checked in the game (headless, from a copy of a save in the bedroom). Walking right into the box
logged `1 0`. The screen faded, the player turned to face the camera, and both lines showed and
closed with A. Then the player walked back, the end logged `2 42`, and nothing ran again. A
press of A while a line is still being written finishes the line instead of closing it.

## Demos and their cameras

The game's prepared cutscenes are **demos**: a sequence (`bin\demo\sequence\<name>.bseq`, magic
`SESD`) plays models, animations, effects, sounds and a camera from `bin\demo\res\<name>\`, and
`bin\demo\demo_data.prmb` maps each demo's name to its sequence. A script plays one with
`PlayDemoScene_(name, a, b, c, d)` and sleeps until it ends. Bill's event plays
`PlayDemoScene_("ev0330_masaki", 1, 0, 1, 1)`; the name is a string the command hashes, and
the four flags aren't known yet.

- **Anywhere.** A demo brings its own scene, so it plays from any map. From the bedroom, Bill's
  teleporter demo took 31 s and Zapdos's (`sub_048_thunder`) 15 s.
- **Afterwards.** A demo leaves the screen faded. Bill's event moves its characters and the event
  camera, then fades in with `FadeIn_(15, GetFnvHash64("BLACK"))`; a cutscene does the same.
- **The flags matter.** Zapdos's own event plays it with `(0, 0, 1, 0)` and starts a battle
  straight after. Played that way and followed by another demo, the game crashed (a null read at
  `0x710031a47c`); with Bill's `(1, 0, 1, 1)` it didn't.
- **Cameras.** A sequence names its files by path, each in a field of 256 bytes, zero-padded:
  `data:/bin/demo/res/ev0330_masaki/camera/ev0330_cam_motion01_p.gfbcama` (`_p` and `_e` for the
  two games). The camera animations (`.gfbcama`, with `.gfbcam` settings beside them) go to JSON
  and back with the studio: `pokebdsp lgpe export-camera` and `import-camera` (letsgostudio's
  `docs/dev/formats/camera-animations.md`). A camera looks down its −z, with +y up.
- **A camera of your own.** Change a demo's camera file in place, or write a new one and change
  the path in the sequence. `hacks\new-camera\` does both (the roadmap's phase 7 check): a
  cutscene that plays Zapdos's demo with its field of view made 1.5 times wider, then Bill's with
  a new camera made from keys that circles the machines. Checked in the game (headless): both
  played with the changed cameras, and the screen came back on the bedroom.

## The format

Pawn 3.3's AMX, built with 64-bit cells:

| Offset | Field | Let's Go's value |
|---|---|---|
| 0x00 | size (s32) | the file's size |
| 0x04 | magic (u16) | 0xF1E1: 64-bit cells |
| 0x06 | file_version, amx_version (u8 each) | 10, 10 |
| 0x08 | flags (u16) | 0x1C: compact 0x04, sleep 0x08, no checks 0x10 (the AI scripts: 0x04) |
| 0x0A | defsize (u16) | 12: a table entry is a u64 value and a u32 name hash |
| 0x0C | cod, dat, hea, stp (s32 each) | code, data, heap start and stack top, in the memory image |
| 0x1C | cip (s32) | `main`'s code address, -1 for none |
| 0x20 | publics, natives, libraries, pubvars, tags, nametable, overlays (s32 each) | table offsets |

- **Names are hashes.** Natives (the commands), publics, public variables, libraries and tags
  give a 32-bit hash of their name instead of a name: h = h × 131 ^ c over its bytes. The name
  table is the 4 bytes `3f 00 00 00`.
- **Compact encoding.** From `cod` on, the file holds the code and the data as a stream of
  cells: each cell big-endian in 7-bit groups, 0x80 on every byte but the last, the sign in bit
  6 of the first, as short as possible (pawncc's rule). The game expands it into the memory
  image (`hea - cod` bytes of code and data), followed by the heap and the stack
  (`stp - hea`, 0x2000 bytes in every script).
- **Memory.** A script is loaded into an allocation of `stp` bytes (0x71006749b0 asks the VM's
  allocator for it and aborts when it gets none; `tools/amx.py info` shows `stp`). A zone's
  script and its `init_` script stay loaded while the player is in the zone, in a pool too small
  for much more than the game's own (seen headless 2026-10-24 with bigger scripts in Pallet's
  zones: `town001` at 42 KB with its init at 11 KB, Oak's lab at 44 + 9 KB and the bedroom at
  55 + 9 KB each aborted as the zone loaded; 29 + 11, 28 + 9 and 19 + 9 load). The game keeps its
  story scenes in scripts of their own (Pallet's triggers start `main_event_0030` and
  `main_event_0120`), loaded only when the event runs, in a pool that takes far bigger ones (the
  game's own run to 66 KB, `main_event_8060`, and 83 KB, `kisekae_shop`; scenes of 47 KB load):
  the event table names the script, whatever it is called, so a mod's big scenes go in scripts
  of their own too.
- **Opcodes.** The interpreter (0x7100670710, never decompiled) has 215: Pawn 3.3's 0-137,
  push2-push5, load.both, load.s.both, const, const.s, icall, iretn, iswitch, icasetbl, 51
  packed forms (the operand in the opcode cell's high 32 bits) and sysreq.d, sysreq.nd. It
  dispatches on the opcode cell's low 32 bits.
- **Jumps** are relative: the target is the jump's own address plus its operand. A case table
  is `casetbl count default (value address)*`, each address relative to its own cell, less 8.
- **Code addresses.** Pawn 3.3 has no function pointers, so the code's only absolute addresses
  are `cip` and the public functions' entries. `tools/amx.py` labels both, so they move with the
  code. (A version that didn't label publics left `GetSceneChangeData`'s entry where it was:
  the game called the end of `main` instead and crashed.)
- **Commands.** A call is `sysreq.n <native index> <argument bytes>`. The first call rewrites
  it in memory to `sysreq.nd <function> <argument bytes>` (Pawn 3's callback patching).

## Source

`tools/amx.py disasm` writes every field the file needs, so a script assembles back exactly:

```
.amx file_version=10 amx_version=10 flags=0x1c defsize=12
.stack 0x2000
.nametable 3f000000
.public GetSceneChangeData          ; name hash computed from the name
.native printf    ; 0xd6c79cd1
.native native_40e2ae38             ; a command whose name isn't known
.library 0xfbedaa83 0x0
.pubvar 0xd7477c97 0x0
.tag 0xca18aa2e 0x40000002

.code
main:
    proc
    ...

.data
    .cell 0 0 99 99
s_0020:
    .string "scrid =%d Not Found \n"
```

**Labels** name code addresses: `main`, `f_XXXX` (a function), `l_XXXX` (a jump target),
`t_XXXX` (a case table), a public's name, and `s_XXXX` in the data (a string; XXXX is the
address in hex). A function that only calls one command whose name ends in `_` takes that
name without it (`StartTurnAround` wraps `StartTurnAround_`), as the game's include files
name them. Labels you write can be any name.

**Instructions** are Pawn's, by name (`load.p.pri 0`, `jzer l_1150`, `stack.p -8`); a packed
opcode takes one number.

**Statements.** Calls are written as statements wherever they stand for exactly the instructions
the compiler wrote:

| Statement | Instructions |
|---|---|
| `Name(a, b)` | push the arguments last to first, then `sysreq.n Name 16` (a command) or `push.p.c 16` and `call` (a function) |
| `var1 = Name(a)` | the call, then store PRI |
| `if Name(a) goto l_10b0` | the call, then `jnz` |
| `ifnot Name(a) goto l_10b0` | the call, then `jzer` |

Arguments: a number; `argN` (the function's Nth argument, at frm+24+8N); `varN` (its Nth
local, at frm-8N); `g_XXXX` (the global at data address XXXX); `&argN`, `&varN` (their
addresses); a `"string"` or a data label (its address). A function makes room for locals with
`stack.p -8` each and frees them before it returns; it returns PRI (`zero.pri`, `retn` returns
0).

**Strings** are one character a cell, ending with a 0 cell. A string the code refers to is
written as `.string "..."` with its label, and an argument or constant that points to it as the
string itself (by its label when two strings have the same text). A string you write in a
statement that isn't in the data yet is added to its end. Escapes: `\n \r \t \\ \" \xHH \uHHHH`.
An array of strings starts with a table of offsets, so its address stays a number.

**Commands** are called by name. The game's commands and the commands of the hacks in `hacks\`
(found in their C++) can be called without a `.native` line; other names need one (`.native
MyCommand`), and `native_XXXXXXXX` calls the command with that hash. A script whose command
isn't registered fails when it calls it.

## Messages

A script's lines are in its message file, which the event table names (its third field, such as `script/forest001.dat`). The file is `bin\message\<language>\script\<name>.dat`, with a label table `<name>.tbl` beside it. `common\` holds the rest of the game's text: names, items, and trainers' lines in `trmsg`.

A script shows a line by the 64-bit hash (FNV-1a 64) of its label. It passes the hash to `MsgWin_(0, 0, label, type, …)` or to a talk function of its own. For example, forest001's bug catcher calls `f_1c28(0x42b8c530b5989886, …)` for `msg_forest001_bugcatcher_01`.

```
python -P tools/msg.py extract [outdir]           every English file as text (build\messages)
python -P tools/msg.py dump <file.dat> [out.txt]  one file as text
python -P tools/msg.py build <in.txt> <out.dat>   back to a message file, and its label table beside it
python -P tools/msg.py roundtrip [files...]       dump and build each file (default: every language's), compare
```

**The text form.** Each message is one line, `label<TAB>text`. A nonzero attribute is kept as `label@3<TAB>text`. A last row `#end<TAB>msg_<name>_max` keeps the label table's end marker, a label that names no line. Codes in the text:

| Code | Means |
|---|---|
| `\n` | a new line |
| `\r` | scroll the box |
| `\c` | clear the box |
| `[WAIT n]` | a pause |
| `[~ n]` | an empty line |
| `[VAR code(args)]` | a variable; `[VAR 0100(0000)]` is the player's name |
| `\\`, `\[` | a backslash, a bracket |
| `\x{hhhh}` | any other unit |
| `[RAW …]` | a broken code, kept as it is |
| `[NOEND]` | a line without its terminator |

**The file.** Each line is UTF-16, encrypted with a key that starts at 0x7C89 + 0x2983 × the line's index and turns left 3 bits after each unit.

All 5,380 message files of every language come back byte for byte. A smoke test checks the tool on made-up messages (`tests\smoke\msg_tool.py`).

**To add a line:**
1. Add a row before `#end` with a label of your own.
2. Build the file into `mods\<mod>\romfs\bin\message\English\script\<name>.dat`; the `.tbl` goes beside it.
3. Give the label's hash to `MsgWin_` in the script.

Every language has its own files, so a line added in English isn't in the others.

## New commands in C++

`hacks\new-script-command` adds two, so any script can call them by name:

```cpp
s64 PokefanAdd(void*, const s64* params) {    // params[0]: argument bytes; then one cell each
    return params[0] / 8 >= 2 ? params[1] + params[2] : 0;
}
const Command kCommands[] = {{"PokefanLog", PokefanLog}, {"PokefanAdd", PokefanAdd}, {nullptr, nullptr}};

extern "C" void FUN_710017dbf0(void* scripts) {   // registers the field's seven command tables
    ORIG_710017dbf0(scripts);
    ORIG_7100674ae0(scripts, kCommands);          // registers one table of {name, function}
}
```

The field registers its tables when it prepares a script: 0x710017dbf0 registers seven
(field, character, party, camera, item, demo, battle), 0x710019d490 four, for the init
scripts; both go through 0x7100674ae0, which hashes each name and gives the script's commands of
those hashes their functions. A command gets the script (`AMX*`, at its holder +0x60) and its
arguments and returns one cell. A string argument is the data address of its first character,
in the script's memory (`amx` base + `dat` + address).

Build with the hack: `cmake -DLETSGO_HACKS=new-script-command build/pc` (several: `"a;b"`).

## Commands

The game registers 389 commands in 12 tables; its scripts call 277 of them. `tools/amx.py
commands --uses` lists each with its group, handler, the argument counts its calls use, and how
many calls and scripts use it. [SCRIPT_COMMANDS.md](SCRIPT_COMMANDS.md) says what they do.

| Group | Commands | For |
|---|---|---|
| character | 131 | moving, turning, looking at things, animations, creating and deleting objects |
| field | 99 | messages, flags and works, sound, fades, scripts, the field itself |
| party | 50 | the party, Pokémon, the partner |
| camera | 31 | event cameras (`EvCamera*`) |
| item | 20 | the bag |
| float | 16 | Pawn's float library (`float`, `floatadd`, `floatcmp`, ...) |
| demo | 14 | demo scenes and movies |
| battle | 13 | starting battles (`CallTrainerBattleCore`, `CallWildBattle`, ...) |
| core | 12 | Pawn's core library |
| console, suspend | 1 each | `printf`, `_Suspend` |
| battle_ai | 1 | the battle AI scripts' command |

The most used: `PostSoundEventSE` (3,161 calls), `float`, `printf`, `EvCameraMove`,
`GetFnvHash64`, `ShowMsgWinCursor_`, `floatsub`, `floatadd`, `PG_WordSetRegister`,
`GetFieldObjectAngle_`, `GetTargetFieldObjectID`, `FlagSet`, `MsgWin_`.
