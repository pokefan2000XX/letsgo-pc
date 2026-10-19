# Mods

A mod is a folder of files that take the place of the game's own. Both executables read them
at startup; the game files themselves are never changed, and a mod holds only the files it
changes.

## Where mods go

The runtime uses the first of these that's given or exists:

1. `--mods <folder>` on the command line, or the `LETSGO_MODS` environment variable
   (`LETSGO_MODS=off` turns mods off)
2. `mods\` beside the exe
3. `mods\` beside `orig\` (the development exe), or `%APPDATA%\LetsGoPikachu\mods\` (the
   player exe, or any exe run from the NSPs)

In that folder, every folder with a `romfs` folder inside is a mod:

```
mods\
  blue-rug\
    romfs\
      bin\field\model\t001r01\carpet01_t001r01.bntx
  new-music\
    romfs\
      bin\sound\...
```

A folder that has `romfs\` itself is one mod (`--mods hacks\phase1-test`). The emulators' layouts
work too, so their mods folders can be used as they are:
`contents\010003F003A34000\<mod>\romfs\` (Ryujinx and Ryubing) and
`contents\010003F003A34000\romfs\` (Atmosphère, also under `atmosphere\`). `exefs\` folders are
ignored: code changes are built into the exe instead (`hacks/README.md`).

Mods apply in name order. When two have the same file, the later one's is used, and the log
says so. `letsgo.log` lists the mods found, each file of theirs the game opens, and each archive
they change.

## Which file a mod file stands for

A mod file stands for the game file at the same path under `romfs\`: the path the game uses in
its RomFS, starting with `bin\`. Paths are case-sensitive, as on the Switch. A file there that
the game doesn't have is added, for code that asks for it.

Most of the game's files are inside its gfpak archives (`bin\archive\**\*.gfpak`). A mod file
reaches inside them by the same rule: the file's path is the one it has inside the archive,
which the archive stores only as a hash (its folder and name, such as
`bin\field\model\t001r01\carpet01_t001r01.bntx` in `bin\archive\field\t001r01.gfpak`). When the
game opens an archive, the runtime builds the archive the game sees:

- a mod file whose path is one of the archive's files replaces that file;
- a mod file in one of the archive's folders that isn't in the archive yet is added to that
  folder, unless the RomFS has that file on its own;
- everything else comes from the game's own archive, unchanged.

A mod can also replace a whole archive; its files then go into that archive the same way.
Files in a mod are stored as they are, uncompressed: the runtime compresses what it puts into
an archive (LZ4, as the game's are).

The game reads a whole archive into one buffer, and an archive much larger than its own may not
get one. So the archive the game sees leaves out the bytes of the files a mod replaces, and
grows only by what the mods' files add: 206 of the partner Pikachu's animations replaced came to
8.59 MB, against 8.66 MB for its own archive.

The hero's clothes have their own limit. Each clothing slot's textures (`tr0001_00_<slot>_*.bntx`)
load into a memory pool of a fixed size, set in the game's code (a table at `0x7100f49198`), and
a slot whose textures don't fit stops the game. Each texture takes its size rounded up to 4 KiB,
and about 200 bytes more.

| Slot | Pool | Free in it | The game's own textures |
|---|---|---|---|
| `skin`, `eye` | 2,270 KiB | 2,302,864 bytes | 2,130,368 bytes |
| `eye_lyc` | 230 KiB | 219,288 bytes | 184,432 bytes |
| `hair`, `tops`, `bag` | 600 KiB | 595,688 bytes | 540,896 bytes |
| `bottoms`, `cap` | 600 KiB | 595,688 bytes | 364,768 bytes |
| `shoes` | 210 KiB | 198,384 bytes | 102,624 bytes |

So the cap's colour texture can grow to about 580 KB, but not to twice its 356 KB.

Gfpaks keep only the hashes of their paths, so a mod needs its files' names. letsgostudio's
archive tools recover most of them from the game (`GfpakNames`). Files inside the `.arc` (SARC)
archives the menus use can't be replaced one by one yet; a mod can replace the whole `.arc`,
which `tools/sarc.py` rebuilds with changed files in it ([MENUS.md](MENUS.md)).

## Adding an area

An area of a mod's own (a room, here `t999r01` with one zone `t999r0101`) is all files but one
list the game keeps in its code. What the game opens for it, all under the mod's `romfs\`:

| File | What it is |
|---|---|
| `bin\field\param\placement\t999r01.bin` | its placement file: its zones, what's in them and its warps (in `placement.gfpak`'s folder, so it's added to that archive) |
| `bin\field\param\terrain\t999r01\terrain_data.bin` | its map pieces: one for the room, naming the files below |
| `bin\archive\field\t999r01.gfpak` | its textures and shaders, under `bin\field\model\t999r01\`: the game looks a model's textures up in the model's own folder |
| `bin\field\model\t999r01\t999r0101_00.gfbmdl`, `.gfbanm`, `.gfbanmcfg` | the room's model and its animation; the config names the animation (`t999r0101_00`) |
| `bin\field\model\t999r01\t999r0101_00_wall.gfbcol`, `_floor.gfbcol`, `_zone.gfbcol` | its collision: walls, floor, and the zone's extent |
| `bin\script\amx\t999r0101.amx`, `init_t999r0101.amx` | the zone's scripts (added to `script_amx.gfpak`) |
| `bin\message\English\script\t999r0101.dat`, `.tbl` | its messages |
| `bin\field\param\placement\table\AreaNameHashTable.tbl`, `ZoneNameHashTable.tbl` | the game's name tables with `t999r01` and `t999r0101` added at the end |

A warp to it, in another area's placement file, names the area and the zone, and where the
player arrives.

The list is of the 147 placement files the game reads when it starts. The game finds a zone's
scripts through the files it read, so an area that isn't on the list loads when the player warps
there (from its own path), but its scripts don't. The `new-areas` hack (`hacks\new-areas`) reads
the placement file of every area a mod adds to the end of `AreaNameHashTable.tbl`, after the
game's own, so its scripts load: build with `cmake -DLETSGO_HACKS=new-areas build\pc`. A zone's
messages come through the script records (`bin\script\param\script_id\script_id_record.bin`),
which don't have the new zone's scripts yet.

letsgostudio's `Field` classes read and write these files (`PlacementFile`, `TerrainFile`,
`CollisionFile`, `NameHashTable`, and `GfpakArchive` for the archive). Its
`pokebdsp lgpe new-area` command writes all of them into a mod folder, copying a room of the
game under new names, with a warp into it from another zone if wanted. The warp is a copy of one
the game has (`--like`): a door's mat fires when the player walks into it, stairs as soon as the
player is in them. A copy of the bedroom, entered through a copy of the front door's mat on the
bedroom floor:

```
pokebdsp lgpe new-area --source <prepared source> --from t001r01/t001r0102 --area t999r01
    --zone t999r0101 --output mods\my_hack --entrance t001r01/t001r0102 --at 6450,0,700
    --arrive 6450,0,590 --size 400,100,80 --like t001r01/t001r0101/0
```

## Sound and music

The game's sound is Wwise's: banks and media in `bin\sound\NX64\wwise\bank\`, all loose files,
so a mod replaces them whole. A zone's music is a state (field 5 of the zone's information in its
placement file) that the field music switch in `BGM_Common.bnk` follows; the same objects are in
`BGM_Event.bnk` and `FieldFootsteps_Normal.bnk`, and the game loads `FieldFootsteps_Normal.bnk`
first, so a changed piece of music needs all three. letsgostudio writes them:

```
pokebdsp lgpe replace-music --source <prepared source> --zone t001r0102 --wav music.wav
    --output mods\my_hack
pokebdsp lgpe new-music --source <prepared source> --zone t999r0101 --wav music.wav
    --output mods\my_hack
pokebdsp lgpe add-sound --source <prepared source> --bank System_Common
    --like Play_SE_System_Decide00 --name Play_pokebdsp_new_sound --wav sound.wav
    --output mods\my_hack
```

`replace-music` plays the WAV in place of the zone's music (and every zone that shares it: the
player's house plays Pallet Town's), repeating it whole. `new-music` gives the zone music of its
own instead, a copy of its music under a new state that plays the WAV, and writes the zone's
placement file with that state; the zones that shared its music keep it. It works on a new area's
zones too, when the area is in the same mod folder. `add-sound` adds an event that plays the
WAV, made like one of the game's. The WAVs become Wwise PCM media (`<id>.wem`), which the game
plays as it plays the partner Eevee's voice. A new event plays only when something posts it: the
`new-sound` hack (`hacks\new-sound`) posts `Play_pokebdsp_new_sound` when the menu opens. Build
with `cmake -DLETSGO_HACKS=new-sound build\pc`.

`LETSGO_AUDIO_WAV=out.wav` records what the game plays, for checking without speakers.

## Moves and trainers

The moves are one table, `bin\pokelib\waza\waza_data.bin`, and each trainer two files,
`bin\trainer\trainer_data\trainer_data_nnn.bin` and its team in `trainer_poke\`. letsgostudio turns
them into JSON and back:

```
pokebdsp lgpe export-moves --source <prepared source> --output moves.json
pokebdsp lgpe import-moves --source <prepared source> --file moves.json --output mods\my_hack
pokebdsp lgpe export-trainers --source <prepared source> --output trainers.json
pokebdsp lgpe import-trainers --source <prepared source> --file trainers.json --output mods\my_hack
```

An import changes only what the file lists, so a file of changes is enough:

```json
{"trainers": [{"id": 1, "pokemon": [{"species": "Eevee", "level": 9, "moves": ["Tackle", "Thunderbolt", "Growl"]}]}]}
```

Trainer 1 (`TRID_PRIVAL_01`) is the rival's first battle. The game reads the move table at start
and a trainer's files when the battle starts. It takes trainer numbers 1 to 601 only, so a new
trainer needs code (letsgostudio's `docs/dev/formats/moves-and-trainers.md`).

## Checking

`python -P tools/check_mods.py` checks the archive patching outside the game, on a made-up archive
and on some of the game's own (`cmake --build build/pc --target modstest` first). With
`--real all --names <list>` it puts every file it can name back into every archive through a
mod and checks each one. `hacks/phase1-test` is a mod made from your game that shows in the
first room.
