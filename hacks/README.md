# hacks/

A hack is a folder here with any of:

| Path | Contents |
|---|---|
| `code/` | C++ built into the game's code, like `decomp/src/` (new functions, and hooks) |
| `tests/` | Differential tests for its code, like `decomp/tests/` |
| `romfs/` | Its files for the game, read as a mod (`docs/MODS.md`) |

**Code.** `cmake -DLETSGO_HACKS=name[;other] build/pc` builds those hacks' `code/` into both
executables, next to the decompiled code (`-DLETSGO_HACKS=` builds them out again). A new build
directory starts with `new-script-command;quick-boot;smooth-moves` (CMakeLists.txt); the others, `wild-battles`
among them, are added by name. A function in
`code/` that's named for an original (`FUN_<address>`, or a name in `decomp/symbols.txt`)
replaces it, as decompiled code does; it can still call the original through
`ORIG_<address>`, so code can be added to a function without decompiling it
(`decomp/README.md`, Hooks). `python -P decomp/tools/build.py --hack name` builds it alone, and
`python -P decomp/tools/difftest.py --hack name` runs its tests with the decomp's.

**Battle code.** Parts of the battle are decompiled (`decomp/include/battle/`,
`decomp/src/battle/`): its events, the damage, the turn order, whether a move hits (one-hit KO moves
too) and lands a critical hit, whether a side has no Pokémon left that can fight, the turn (making
its actions and running them in order, then the end of the turn), whether a Pokémon can use its move
(sleep, paralysis, confusion and the rest), a condition's damage at the end of a turn and what Bind,
Leech Seed, Perish Song, Ingrain and Aqua Ring do then, whether a condition can be given (type
immunities), the chances of a move's added effects, and whether a stat stage can change (its limits,
a substitute, Clear Body and the like). A hack can register handlers for moves (`event.h`:
`AddHandlerSet`, events such as `kEventPower`, variables such as `kVarPowerRatio`) and call those
parts (`damage.h`, `order.h`, `hit.h`, ...); `new-move-effect/` is the example. To change one of
those parts itself (the damage formula, paralysis's chance, a type's immunity), edit its decompiled
source: its difftests then show what differs from the original. A hack can't also replace a function
the decomp replaces, since the build refuses two replacements for one address.
`python -P tools/battle_events.py [event]` lists every event, the variables it has and what answers
it.

**Files.** `--mods hacks/<name>` runs the game with a hack's `romfs/`, or copy the hack into a
mods folder. Files made from the game stay out of git, so `hacks/*/romfs/` is ignored: a hack
that changes the game's files commits the script that makes them from the player's own copy.

| Hack | What it shows |
|---|---|
| `phase1-test/` | The roadmap's phase 1 check: a hook logs every file the game opens, and the first room's rug and the hero's cap turn blue |
| `new-areas/` | Areas a mod adds (roadmap phase 3, step 3): a hook reads the placement file of every area named at the end of `AreaNameHashTable.tbl` when the field starts, as the game reads its own 147 (docs/MODS.md, Adding an area) |
| `new-sound/` | A sound a mod adds (roadmap phase 4): a hook on Wwise's `PostEvent` posts `Play_pokebdsp_new_sound` whenever the game posts `Play_SE_System_Menu00`, the menu opening (docs/MODS.md, Sound and music) |
| `new-move-effect/` | A move effect written as new code (roadmap phase 5): a hook on the battle's move handler lookup (`0x7100a38010`) gives Tackle a handler of its own, which multiplies its power by 5, written with the decompiled events (`battle/event.h`; docs/PLAN.md, Phase 5 progress). Not yet tried in a battle |
| `wild-battles/` | Wild battles in place of the catch screen (roadmap phase 5, step 4): touching a wild Pokémon starts a battle with it, through the battle event the scripts' wild battles use, without the boss setup (match timer, flags, auras); EXP and Run work, and Poké Balls from the battle bag catch it (Sun and Moon's capture, still in the battle core), after which it goes to the party or a box through the catch screen's own event. `make_romfs.py` puts the balls in the battle bag's table, so run it with `--mods hacks/wild-battles`. No throw animation yet (docs/PLAN.md, Phase 5 progress) |
| `new-script-command/` | Script commands written in C++ (roadmap phase 6): hooks on the two functions that register the field's command tables add a table of its own, so any script can call `PokefanLog(...)` (writes its arguments to letsgo.log) and `PokefanAdd(a, b)` by name (docs/SCRIPTS.md) |
| `new-cutscene/` | A cutscene of its own from a trigger of its own (roadmap phase 6's check): walking right from the bedroom save's spot starts it; it fades the screen, turns the player, shows two new lines and walks the player back, logging its start and end through `new-script-command`'s commands. `make_romfs.py` assembles the script, builds the lines and adds the trigger and the event's record to the game's files, so run it first, with an exe that has `new-script-command` (docs/SCRIPTS.md, A cutscene of your own) |
| `new-camera/` | Camera sequences (roadmap phase 7's check): a cutscene like `new-cutscene`'s, with its own event, plays Zapdos's demo with its camera edited (the field of view 1.5 times wider) and Bill's teleporter demo with a new camera made from keys, a new file its sequence is changed to name. `make_romfs.py` makes the camera files with the studio's `pokebdsp lgpe export-camera` and `import-camera`, so it needs letsgostudio built (`--studio` or `POKEBDSP` give its pokebdsp.exe); run it first, with an exe that has `new-script-command` (docs/SCRIPTS.md, Demos and their cameras) |
| `menu-edit/` | Menus (roadmap phase 8's check): the main menu's Bag button raised and made larger (its layout, with `tools/bflyt.py`), its icon turned from yellow to purple (its texture, through the studio's `pokebdsp lgpe export` and `import`) and BAG renamed BACKPACK (the menu's message file). `make_romfs.py` puts the layout and texture back into the menu's archive with `tools/sarc.py`; it needs letsgostudio built, like `new-camera` (docs/MENUS.md) |
| `quick-boot/` | A quicker start: no controller screen at boot (the boot asks for the controller procedure in mode 1, always ask; mode 0 asks only when the connected controllers need it, and the runtime's one Handheld controller doesn't), and a button press skips the "Presented by GAME FREAK" logo (its layout's opening counts as done) and the title demo's opening up to "Press the A Button" (the demo's clock goes to frame 562 and every track catches up in one frame, starting and ending what it would have; the logo's animation is finished and the opening's own sound left out). A press in the white load after the logo counts for the title. It hooks Wwise's `PostEvent` as `new-sound/` does, so the two can't be built together |
| `new-font/` | Fonts (roadmap phase 8): the font of the English menus and message boxes (`FOT-UDKakugoC80Pro-DB`) replaced with an OpenType or TrueType font you give with `--font`, encoded by `tools/font.py`. Fonts are someone's work, so the hack has no font of its own (docs/MENUS.md, Fonts) |
| `catalog-room/` | Room for more Pokémon in the model catalog (`poke_resource_table.gfbpmcatalog`): the catalog shares a heap with a 0x41944800-byte sub-heap the game carves out right after it (`0x71000af6e0`), and with 656 more entries (179 more Pokémon) that allocation failed and the game stopped at start. The hook makes the sub-heap 128 KB smaller (it keeps about 158 KB spare after its eight child heaps), giving the catalog room for about 330 more entries. letsgostudio writes catalogs to fit and says when one needs this hack |
| `other-species/` | Pokémon of species Let's Go has no models of its own for (1 to 809, as the species data has them) kept in the party and boxes: when a save loads, `0x7100131d30` marks a bad egg (Pokemon::MarkBadEgg, `0x7100728120`) each stored Pokémon whose species isn't 1–151, 808 or 809, whose form the game doesn't allow or that knows a move outside its 230 (the sorted list at `0x7100f4982c`). A Pokémon from a later game put in a save showed as a fainted egg. The hook runs the original with the marking off, then marks by the same rules with every species 1–809 allowed. A mod that adds Pokémon from later games needs it |
| `smooth-moves/` | Scripted characters that run on from one forced move into the next without stopping: a `StartForceMove_` started while the character's last one still runs makes the older one end where it is, in the command's update (`0x71002dbec0`). Without it the older one waits, then puts the character at its own target, a snap back once the newer move goes on. The game's own scripts always wait for a move to end, so only scripts that chain moves (a run that turns as it goes) see a difference (docs/SCRIPT_COMMANDS.md, `StartForceMove_`) |
