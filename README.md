# letsgo-pc

A work in progress native Windows port of **Pokémon: Let's Go, Pikachu!** (v1.0.2). It isn't an
emulator: the game's Switch code is recompiled ahead of time from ARM64 to x86-64
and runs on a PC reimplementation of the Switch SDK, with its own Vulkan renderer,
sound and input. The recompiled code is being replaced with decompiled C++, one
function at a time, and the port is a base for ROM hacks.

This repository holds the source code and the downloads. Nothing from the game is
in it: the game builds and runs from your own dump.

<p align="center">
  <img src="screenshots/bedroom.jpg" width="49%" alt="The player's bedroom">
  <img src="screenshots/pallet-town.jpg" width="49%" alt="Pallet Town">
  <img src="screenshots/home.jpg" width="49%" alt="The player's house in Pallet Town">
  <img src="screenshots/oak-pidgey.jpg" width="49%" alt="Professor Oak and Pidgey">
  <img src="screenshots/lab-poke-balls.jpg" width="49%" alt="Oak's lab, choosing a Poké Ball">
  <img src="screenshots/lab.jpg" width="49%" alt="Oak's lab">
</p>

## Status

Work In Progress (recomp roughly 80% complete; 741 of the game's 59,324 functions
decompiled so far, see [Decompiled so far](#decompiled-so-far))

Works:

- The logos, the intro movie and the professor's introduction
- Naming the player and the rival (a text box opens over the window)
- The bedroom, Pallet Town, Oak's Pikachu event, the catch tutorial and the lab
- Saving and continuing
- Sound, music and movies
- Keyboard and gamepads, with rumble; F11 for fullscreen
- **60 FPS** (new in v0.3.0): Tab while
  playing (or `fps=60` in `settings.ini`) runs the game at 60 frames a second instead of
  its own 30. Walking, text, scripts, camera moves, screen fades and battles keep the
  game's own pace; only turning on the spot is still quicker than at 30.
- **A sharper picture** (also new in v0.3.0): `scale=` in `settings.ini` renders the
  game that many times as wide and high. `auto`, the default, picks 2 on a 1440p or 4K
  monitor.

Not yet:

- **Motion controls.** A PC has no gyro, so the motion sensors always report at
  rest.
- **Some graphics effects** are missing (effects that use compute shaders, such
  as automatic exposure).
- **Anything online or wireless.** Trading, battling other players, Mystery
  Gifts and Pokémon GO transfers don't work.
- **Let's Go, Eevee!** isn't supported.

## Decompiled so far

Decompiled code is readable C++ that replaces the game's code function by function. Everything
not decompiled yet keeps running the recompiled original, so the game works at every step, and
every decompiled function is checked against its original in an emulator
(`decomp/tools/difftest.py`). 741 functions so far:

- **Pokémon data, all 613 functions:** a Pokémon's record and every field in it, stats,
  natures, the party, evolution, learnsets, the species tables and the item checks.
- **The battle core, 121 functions:** the damage formula, the turn order, whether a move hits
  and whether it's a critical hit, the turn itself (making the actions, running them, the end
  of the turn), whether a Pokémon can move (sleep, paralysis, confusion), the damage conditions
  do at the end of a turn (Bind, Leech Seed, Perish Song, Ingrain, Aqua Ring), which conditions
  can be given (type immunities), added effects' chances, stat stages, and the battle's events,
  which moves' handlers answer.
- **Field placement and the game's name hashes.**

Beyond the decompiled code, the parts of the engine a hack needs are mapped and documented in
[docs/](docs):

- **Scripts** ([SCRIPTS.md](docs/SCRIPTS.md), [SCRIPT_COMMANDS.md](docs/SCRIPT_COMMANDS.md)):
  the game's Pawn scripts, an assembler and disassembler (`tools/amx.py`), the script commands
  with their arguments, how events and triggers start them, and how much script memory a zone has.
- **Battle sequences** ([SEQUENCES.md](docs/SEQUENCES.md)): move animations as JSON
  (`tools/bseq.py`), every command's parameters typed from the game's code.
- **Menus and fonts** ([MENUS.md](docs/MENUS.md)), **the battle AI**
  ([BATTLE_AI.md](docs/BATTLE_AI.md)), **mods** ([MODS.md](docs/MODS.md)) and notes on the
  binary ([BINARY_NOTES.md](docs/BINARY_NOTES.md)), the battle camera included.
- **60 FPS** ([PLAN.md](docs/PLAN.md), M6): which systems count frames and which go by the
  clock. Battles run every frame while their frame-counted parts run every other one, and
  script lengths given in frames are doubled.

Hacks built on it are in [hacks/](hacks): wild battles in place of the catch screen (with EXP,
running and Poké Balls), script commands written in C++, new cutscenes, cameras, areas, sounds,
fonts and menus, a new move effect, a quicker boot, scripted characters that run from one
move into the next without stopping, and room for Pokémon from later games.

## Requirements

- Windows 10 or 11, 64-bit
- A GPU whose driver has Vulkan 1.3 and the `VK_EXT_shader_object` extension.
  Recent NVIDIA drivers have it; for other GPUs, look yours up on
  [gpuinfo.org](https://vulkan.gpuinfo.org/listdevices.php).
- Your own copy of the game, dumped from your own Switch:
  - the base game NSP, v1.0.0 (title ID `010003F003A34000`)
  - the v1.0.2 update NSP (title ID `010003F003A34800`)
  - `prod.keys` from the same Switch

The downloads include none of these, and they won't be shared here. Issues asking
for them will be closed.

## Getting started

Download `LetsGoPikachu.exe` from the [latest release](https://github.com/pokefan2000XX/letsgo-pc/releases/latest).
It's one file, with SDL and the codecs linked in and nothing from the game, and it runs
from the three files under Requirements.

1. Put `LetsGoPikachu.exe` in any folder and start it. There's no console
   window.
2. The first start explains what it needs, then opens three file pickers: the
   game, its update, the keys. Nothing is copied or extracted. The game reads
   the NSPs where they are, decrypting what it needs as it goes, so they have
   to stay there.
3. If a file is wrong (the update chosen as the game, a different game, keys
   that can't decrypt it, a file that has moved), a message says which file and
   why, and offers its picker again.

Everything it writes goes to `%APPDATA%\LetsGoPikachu\`: `config.ini` (the
three paths; delete it to choose again), `save\`, `letsgo.log` and
`shader_cache.bin` (the game's shaders, translated once and kept, so they
never stutter again; delete it any time). A fatal
error shows a message box that points at the log. `--base`, `--update` and
`--keys` give the files on the command line instead. Paths given that way
aren't saved to `config.ini` unless a picker had to correct one.

`settings.ini` beside the saves keeps two choices. `fps=60` runs the game at 60 frames a
second instead of its own 30 (Tab switches it while playing; text, scripts and screen fades
keep the game's pace, battles included). `scale=` is the picture's size: the game's
screen-sized render targets are made that many times as wide and high, so a big or maximized
window shows a sharp picture. `auto`, the default, picks the monitor's height over the game's
1080, rounded up (2 on a 1440p or 4K monitor); `1` to `4` set it.

Mods go in `mods\` beside the exe or in `%APPDATA%\LetsGoPikachu\mods\`, one
folder each with a `romfs\` inside, as for Ryujinx
([docs/MODS.md](docs/MODS.md)).

## Controls

Every keyboard and gamepad drives the Switch's handheld controls. A gamepad's
face buttons follow their printed labels, so the button marked A confirms.
PlayStation pads use the Xbox positions (Cross is A).

| Switch | Keyboard (laid out like DeSmuME and melonDS) | Gamepad |
|---|---|---|
| Left stick (walking, menus) | Arrow keys (hold Left Shift for half tilt) | Left stick |
| A / B / X / Y | X / Z / S / A | Face buttons by label |
| L / R | Q / W | Shoulder buttons |
| ZL / ZR | E / R | Triggers |
| Plus / Minus | Enter / Backspace (or Right Shift) | Start / Back |
| D-pad | I / K / J / L (up, down, left, right) | D-pad |
| Right stick | Numpad 8 4 2 6 | Right stick |
| Stick clicks | F / G | Stick clicks |
| Fullscreen | F11 | |

## Building from source

Nothing from the game is in this repository. `orig/` (extracted from your dump)
and `toolchain/` are ignored by git.

- [docs/PLAN.md](docs/PLAN.md): architecture, decisions and milestones
- [docs/BINARY_NOTES.md](docs/BINARY_NOTES.md): what's in the binary
- [docs/MODS.md](docs/MODS.md): mods, files that take the place of the game's

`build-dev.bat` builds the development `letsgo.exe` (Setup, step 4), with the hacks named in
[hacks/README.md](hacks/README.md). The players' `LetsGoPikachu.exe` is on the releases page.

### Setup

1. **Toolchain:** `bash tools/setup_toolchain.sh` downloads everything into
   `toolchain/`, pinned and hash-checked, and installs the Python packages with `pip --user`:

   | Tool | Version / source | Purpose |
   |---|---|---|
   | `llvm-mingw/` | 20261006 (clang 23.1.3), github.com/mstorsjo/llvm-mingw | Host build with the Itanium C++ ABI, ARM64 guest code, objdump/readelf/cxxfilt |
   | `ghidra/` | 12.1.4, github.com/NationalSecurityAgency/ghidra | Decompiler and analysis database. Uses the installed JDK 21 (`support/launch.properties`) |
   | `guest-sysroot/` | musl 1.2.6 headers (musl.libc.org) + guest libc++ config | C/C++ standard headers for ARM64 decomp code (`tools/make_guest_sysroot.py`) |
   | `nnsdk/`, `nnware/` | open-ead, pinned commits | Nintendo SDK, NVN, nn::gfx and NintendoWare declarations. No license: local use only, never copied into this repo |
   | `nnheaders/` | open-ead, frozen | Predecessor of nnsdk/nnware, kept for reference |
   | `SDL3-static/` | 3.4.18 source, github.com/libsdl-org/SDL, built static | Window, keyboard, gamepads, rumble, sound. Linked into the exe, so there's no `SDL3.dll` |
   | `src/Vulkan-Headers-*` | vulkan-sdk-1.4.363.0, github.com/KhronosGroup/Vulkan-Headers | Vulkan API headers (the loader `vulkan-1.dll` comes with the GPU driver) |
   | `glslang/` | 16.6.0, github.com/KhronosGroup/glslang, built static without the optimizer | GLSL → SPIR-V for translated shaders |
   | `codecs/` | opus 1.5.2, libogg 1.3.6, libvorbis 1.3.7 (downloads.xiph.org); libvpx 1.17.0 (github.com/webmproject/libvpx), VP9 decoder only, assembled with nasm 3.02 (nasm.us) | The game's Opus sound, the movies' VP9 picture and Vorbis sound. Built static by `tools/setup_codecs.sh` |
   | Python | cryptography, lz4, capstone, unicorn 2.1.4 | Extraction, analysis, ARM64 emulator oracle |

   Also needed, installed separately:
   - Git for Windows, whose Git Bash runs the `.sh` scripts;
   - Python 3.11 or newer on PATH;
   - CMake and Ninja, either on PATH or from Visual Studio / its Build Tools with
     "C++ CMake tools for Windows" (the batch files in step 4 find either).

2. **Extract and analyze your dump**, from the three files under Requirements:

   ```bash
   bash tools/prepare.sh "BASE.nsp" "UPDATE.nsp" "prod.keys"
   ```

3. **Ghidra project** (`orig/ghidra/letsgo`). Takes several minutes per module:

   ```bash
   cmd //c "tools\\ghidra\\import.cmd main"
   cmd //c "tools\\ghidra\\import.cmd sdk"
   ```

   Open it in the GUI with `toolchain/ghidra/ghidraRun.bat`. Decompiled C for
   chosen functions goes to `orig/`, because it's derived from the game:

   ```bash
   cmd //c "tools\\ghidra\\decompile.cmd orig\\decomp 0x7100004110"
   ```

4. **Build.** `build-dev.bat` builds `build/pc/letsgo.exe`, the development executable: a
   console window with the log. It runs the game extracted in `orig/` and has the test and
   debug options (step 5).

   Double-click it, or run it from a command prompt. It:
   - checks that steps 1 and 2 are done;
   - runs the recompiler the first time;
   - configures `build/pc` the first time;
   - builds `letsgo.exe`.

   `--recomp` runs the recompiler again; do that after `recomp/` changes. The
   first build compiles about 95,000 recompiled functions and takes a while.
   After that, only what changed is rebuilt. Started from Explorer, the window
   stays open until a key is pressed so you can read the result.

   **Building by hand**, from Git Bash in the repository folder:

   ```bash
   # 1. Recompile main and sdk to C in build/gen/ (again after recomp/ changes)
   python -P recomp/recomp.py main sdk
   # 2. Configure build/pc (once, and again after step 1 adds or removes files)
   cmake -S . -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$PWD")/cmake/host-x86_64.cmake"
   # 3. Build the development exe (build/pc/letsgo.exe), the release exe
   #    (build/pc/LetsGoPikachu.exe), or both
   cmake --build build/pc --target letsgo
   cmake --build build/pc --target LetsGoPikachu
   ```

   - `build/gen/` is derived from the game and never committed.
   - Always give `--target`. A plain `cmake --build build/pc` builds everything:
     both executables, `tools/shadertest` and `tools/modstest`.
   - If CMake or Ninja aren't on PATH, use the copies Visual Studio installs under
     `Common7/IDE/CommonExtensions/Microsoft/CMake/`, and add
     `-DCMAKE_MAKE_PROGRAM=<path to ninja.exe>` to step 2.
   - `-DLETSGO_CHECK_CALLS=ON` (used for `build/pc-check`) makes every call check
     that callee-saved registers and `sp` survive it, which catches recompiler bugs.
   - Either build also compiles the decompiled code in `decomp/` (see Decompiling
     below) whenever it changes.

5. **Run.** Double-click `build/pc/letsgo.exe`, or run it from a shell. It
   finds `orig/` next to the exe or up to two folders above it. Saves go to
   `save/` beside `orig/`; logs go to `letsgo.log` in the working folder and
   to the console window. The game renders with Vulkan 1.3+ (it needs
   `VK_EXT_shader_object`); `--no-gpu` keeps the logging-only renderer and
   shows a status screen instead (frame rate, the input the game sees, the
   files it loads). Sound plays through the default output device. When the
   game asks for text (the player's and rival's names), a small text box opens
   over the window.

   ```bash
   build/pc/letsgo.exe
   ```

   | Option | Effect |
   |---|---|
   | `--orig <dir>` | Game files somewhere else |
   | `--mods <dir>` | Mods from this folder, or the one mod in it ([docs/MODS.md](docs/MODS.md); default `mods/` beside the exe, then beside `orig/`) |
   | `--base <nsp> --update <nsp> --keys <file>` | Runs from the NSPs, as the player exe does, instead of `orig/`. Saves and `config.ini` then go to `%APPDATA%\LetsGoPikachu\` |
   | `--headless` | No window (tests, servers) |
   | `-v` | Verbose: file opens, thread starts, input queries |
   | `--press A@8,A@f330,SL_UP@12:1.5` | Taps buttons at the given seconds, or `fN` once frame N is shown, in the order given (A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK). `:S` holds for S seconds (default 0.15); `SL_UP` `SL_DOWN` `SL_LEFT` `SL_RIGHT` (and `SR_*`) push a stick all the way |
   | `--dump-threads 15` | Logs every guest thread's wait and call chain at that time (repeatable) |
   | `--screenshot out.png@12[,b.png@f600...]` | Saves the window's contents (or with the GPU, the presented frame) at those times or frames, in the order given; works with `--headless` |
   | `--keyboard "Ash;Gary"` | Answers the game's text requests in order instead of the text box (a headless run with no answer left enters "Red") |
   | `--no-gpu` | Logging-only NVN and the status screen, no Vulkan |
   | `--nvn-trace N` | Logs every NVN call made during frame N |

   Debugging and tests use environment variables:

   | Variable | Effect |
   |---|---|
   | `LETSGO_DUMP_FRAME=N[,M...]` | Saves frame N's render targets and sampled textures (BCn as `.dds`), a line per draw (`draws.txt`: programs, cull and depth state, vertex formats, textures), the vertex stage's uniform data and first vertex (`data.txt`) and the GLSL of each program to `build/frame/` (`LETSGO_DUMP_DIR` to change). `tS` instead of N: the first frame after S seconds |
   | `LETSGO_DEBUG_FS=1` | Every fragment program writes magenta, to show what rasterizes |
   | `LETSGO_DUMP_SHADERS=dir` | Writes the control section and code of every program without NV assembly (input for `shadertest`) |
   | `LETSGO_VK_VALIDATION=1` | Enables the Vulkan validation layer, if installed |
   | `LETSGO_TEX_LOG=1`, `LETSGO_PROG_LOG=1` | Log texture creation and pool registration, program setup and binding |
   | `LETSGO_AUDIO_WAV=x.wav` | Records each audio output stream: the game's to `x.wav`, the next one (a movie's) to `x-2.wav`, and so on |
   | `LETSGO_MUTE=1` | No sound |
   | `LETSGO_SAVE_DIR=dir` | Saves somewhere else (tests use their own, so your save is never touched) |
   | `LETSGO_MODS=dir` | Mods from this folder, like `--mods`; `off` runs without mods |
   | `LETSGO_NO_DECOMP=all` | Runs the original code instead of every decompiled replacement; `=7100101b30,...` only for those functions (finds which replacement breaks something) |
   | `LETSGO_DECOMP_TRACE=1` | Logs the first call of each replaced function |
   | `LETSGO_TURBO[=S]` | Presents as fast as possible (for the first S seconds): logos, text and menus run many times faster in test runs; movies and sound keep real time |
   | `LETSGO_PERF=1` | Logs every frame that took over 40 ms with what the time went to (shader builds, texture uploads), and frame-time statistics every 600 frames |
   | `LETSGO_SHADER_CACHE=file` | Where translated shaders are kept between runs (default `cache/shader_cache.bin` beside `save/`; the player exe keeps it in `%APPDATA%\LetsGoPikachu\`), or `off` |
   | `LETSGO_SHADER_WARMUP=0` | Doesn't create the cached shaders in the background at startup |
   | `LETSGO_NO_VSYNC_LOCK=1` | Times frames by a 60 Hz clock even when the display runs at a multiple of 60 Hz (normally frames follow the display's own refresh) |
   | `LETSGO_CONTROL=file` | Drives a running game from lines appended to `file`: `A`, `SL_UP:1.5` (hold), `wait 2`, `shot out.png`, `turbo on`/`turbo off`, `calls out.tsv` (with `LETSGO_CALLS`) |
   | `LETSGO_FPS=60` or `30` | The frame rate for this run, not kept in `settings.ini` |
   | `LETSGO_SCALE=N` | The render scale for this run (`settings.ini`'s `scale=`; a `--headless` run is 1 without it) |
   | `LETSGO_HANDHELD=1` | The game in handheld mode (1280x720) instead of docked (1920x1080) |
   | `LETSGO_CALLS=1` | Counts the indirect calls of each main function; the control file's `calls out.tsv` writes them (busiest first) and starts again |
   | `LETSGO_BACKTRACE=addr[,...]` | Logs the first calls of those main functions with x0 to x3 and the call chain (`LETSGO_BACKTRACE_AFTER=S`: only after S seconds) |
   | `LETSGO_FILES=1` | Logs each game file the first time it's opened |
   | `LETSGO_DIALOG_TEST=prefix` | Each text box saves a picture of itself to `prefix-N.png`, then answers itself (its initial text, or "Test"), so a windowed test runs hands-free |
   | `SDL_AUDIO_DRIVER=disk` | Makes a `--headless` run play through SDL's disk driver: silent, timed like a sound card, output in `SDL_AUDIO_DISK_OUTPUT_FILE` (default `sdlaudio.raw`) |

   **Controls.** Every keyboard and gamepad drives the Switch's Handheld
   controller. A gamepad's face buttons follow their printed labels, so the
   button marked A confirms; PlayStation pads use the Xbox positions (Cross is
   A). Rumble goes to every connected gamepad.

   | Switch | Keyboard (laid out like DeSmuME and melonDS) | Gamepad |
   |---|---|---|
   | Left stick (walking, menus) | Arrow keys (hold Left Shift for half tilt) | Left stick |
   | A / B / X / Y | X / Z / S / A | Face buttons by label |
   | L / R | Q / W | Shoulder buttons |
   | ZL / ZR | E / R | Triggers |
   | Plus / Minus | Enter / Backspace (or Right Shift) | Start / Back |
   | D-pad | I / K / J / L (up, down, left, right) | D-pad |
   | Right stick | Numpad 8 4 2 6 | Right stick |
   | Stick clicks | F / G | Stick clicks |
   | Fullscreen | F11 | |
   | 60 frames a second on or off (kept in `settings.ini`) | Tab | |

6. **Smoke tests:** `bash tests/run_smoke.sh`. The boot, shader and movie tests use
   `build/pc` (set `LETSGO_BUILD` to use another build). The shader test reads
   `orig/shaders_ctrl` (`python -P tools/shader_census.py --dump-ctrl orig/shaders_ctrl`)
   and `orig/shaders_noasm_u` (programs dumped with `LETSGO_DUMP_SHADERS`).

### Decompiling

Decompiled code is readable C++ in `decomp/`, compiled for the Switch's CPU with the
original's ABI and struct layouts, and recompiled together with the game. Each function
replaces the original it was written for; everything else keeps running the recompiled
original, so the game works at every step. What gets decompiled, and when, follows the
roadmap in [docs/PLAN.md](docs/PLAN.md); [decomp/README.md](decomp/README.md) has the
conventions.

| Path | Contents |
|---|---|
| `decomp/src/<area>/` | The decompiled code (`.cpp`), one folder per part of the game |
| `decomp/include/` | `decomp.h` (basic types, layout checks `DECOMP_OFFSET` and `DECOMP_SIZE`) and each area's headers |
| `decomp/symbols.txt` | Names of original functions and data by address |
| `decomp/tests/<area>/` | Differential tests (`test_*.py`) |
| `decomp/tools/` | `build.py` builds the code into the game, `difftest.py` checks it against the originals, `progress.py` counts what's done |

**How a function gets replaced.** Give the decompiled function a name and list that name
with the original's address in `decomp/symbols.txt` (a function named `FUN_7100101b30` needs
no entry). The same file links everything the code calls or reads that isn't decompiled
yet. Calls to the SDK (`memcpy`, `nn::` functions) need no entry: they go through main's
own import stubs.

**Building.** Building either executable (`build-dev.bat`, or
`cmake --build build/pc --target letsgo`) runs `decomp/tools/build.py` whenever `decomp/`
changes. It compiles the code, links `build/decomp/patch.elf` at `0x7102000000`, and
recompiles it into `build/gen/patch`. The recompiled originals are weak symbols, so a
decompiled function overrides the original for every caller (direct calls, vtables,
callbacks) without regenerating main.

**Checking.** `python -P decomp/tools/difftest.py` runs each `decomp/tests/**/test_*.py` twice
in the emulator: once against the originals, once against the decompiled versions. Both runs
must record the same results (`t.check(value)`) and make the same SDK calls. In the game,
`LETSGO_NO_DECOMP` switches replacements back to the originals for comparison.

**Hooks and hacks.** A replacement can call the original it replaces through `ORIG_<address>`,
so code can be added to a function without decompiling it ([decomp/README.md](decomp/README.md),
Hooks). A hack in `hacks/<name>/` has its own code, tests and files for the game; `-DLETSGO_HACKS=<name>`
builds its code into the executables ([hacks/README.md](hacks/README.md)). build-dev.bat builds
`new-script-command`, `quick-boot` and `smooth-moves` in. `wild-battles` is left for you to add
(`cmake "-DLETSGO_HACKS=new-script-command;quick-boot;smooth-moves;wild-battles" build/pc`), and
its `make_romfs.py` puts the Poké Balls in the battle bag.

**Reading the original.** `python -P tools/disasm.py <address>` prints a function with the
names of what it calls and the strings it uses; `python -P tools/refs.py <address>` lists
who references an address (code and data, after `tools/refs.py build`); Ghidra's view is in
`orig/ghidra/letsgo`. Their output comes from the game, so it stays in the terminal or `orig/`.

### Layout

The repository has three parts, plus the tools and tests they share. Nothing from the
game is in it: `orig/`, `build/`, `toolchain/` and `save/` are ignored by git.

**Recomp: running the original code on PC**

| Path | Contents |
|---|---|
| `recomp/` | The static recompiler: ARM64 decoder (`a64.py`), C emitter (`emit.py`), jump tables and computed gotos (`recomp.py`), its Unicorn differential test (`semtest.py`) and a decoder check against Capstone (`check_decoder.py`) |

**Runtime: the PC side both halves run on**

| Path | Contents |
|---|---|
| `runtime/` | Loader, Horizon kernel subset, `nn::` service overrides (fs, hid, audio out and Opus, network, software keyboard, ...), the mods folder (`mods.cpp`: files that replace the game's, inside its gfpak archives too), the `movie::` library on a WebM parser, libvpx and libvorbis (`hle_movie.cpp`, `webm.cpp`), SDL window, input and sound (`platform.cpp`) |
| `runtime/src/gpu/` | NVN on Vulkan (`nvn_vk.cpp`, `device.cpp`), texture layouts and formats (`texture.cpp`, `astc.cpp`), shader translators: NV_gpu_program5 assembly (`nvasm.cpp`) and Maxwell machine code (`maxwell.cpp`) to GLSL, glslang to SPIR-V (`spirv.cpp`) |

**Decomp: readable source replacing the original**

| Path | Contents |
|---|---|
| `decomp/` | Decompiled game code, its tests and its build tools (see Decompiling) |
| `hacks/` | Hacks: new code and hooks built into the game (`-DLETSGO_HACKS`), and their files for the game, read as mods ([hacks/README.md](hacks/README.md)) |

**Shared tools**

| Path | Contents |
|---|---|
| `tools/setup_toolchain.sh`, `tools/setup_codecs.sh` | Download and build the toolchain (step 1) |
| `tools/prepare.sh` | Extracts and analyzes your dump into `orig/` and `data/` (step 2) |
| `tools/nxextract.py` | NSP → ExeFS + RomFS with the update applied (BKTR), checked against the IVFC hash tree |
| `tools/nso2elf.py` | NSO → ELF with rebuilt sections, for LLVM tools, Ghidra and the recompiler |
| `tools/analyze.py` | Function list (from `.eh_frame`), import map, instruction census |
| `tools/xrefs.py` | vtables, call graph, boot reachability |
| `tools/strxref.py` | Which function references which string or source path |
| `tools/refs.py`, `tools/disasm.py` | Who references an address; a function's disassembly with names filled in |
| `tools/npdm.py` | Process metadata (main thread, services) |
| `tools/ghidra/` | Headless import (`import.cmd`), function seeding, decompiled-C export |
| `tools/emu/guest_emu.py` | Runs original functions under Unicorn with main at its real address: the test oracle for the recompiler and the decomp |
| `tools/make_guest_sysroot.py` | Builds `toolchain/guest-sysroot` from the musl source |
| `tools/extract_fonts.py` | System shared fonts from the firmware installed for Ryujinx → `orig/fonts/` |
| `tools/gen_nvn_procs.py` | NVN entry point and device-constant tables for the runtime |
| `tools/check_overrides.py` | Checks that every host override name matches a real symbol |
| `tools/vtables_of.py` | Prints a class's vtable slots by name (e.g. `movie::Decoder` in subsdk1) |
| `tools/shader_census.py`, `tools/gfpak.py` | Census of the game's shaders and textures; dumps NVN control sections |
| `tools/maxwell_ref.py`, `tools/maxwell_dis.py` | Maxwell opcode table and field layouts from Ryujinx (MIT); disassembler; generates `runtime/src/gpu/maxwell_ops.inc` |
| `tools/shadertest/` | Translates and compiles every dumped program the way the runtime does |
| `tools/modstest/`, `tools/check_mods.py` | The mods folder's archive patching run on a made-up archive and the game's own, and checked entry by entry |
| `tools/amx.py`, `tools/msg.py`, `tools/flatbuf.py` | The field scripts to editable source and back, the messages to text and back, and FlatBuffers read, built and made longer without a schema compiler ([docs/SCRIPTS.md](docs/SCRIPTS.md)); the battle AI scripts' subcommands named ([docs/BATTLE_AI.md](docs/BATTLE_AI.md)) |
| `tools/bseq.py` | The battle sequences (move animations) to JSON and back, each parameter typed as the game's code reads it ([docs/SEQUENCES.md](docs/SEQUENCES.md)) |
| `tools/sarc.py`, `tools/bflyt.py`, `tools/font.py`, `tools/studio.py` | The menus' archives (SARC) read and written, their layouts' panes listed and changed, the game's fonts to OpenType and back, and letsgostudio's command line run from a hack's script ([docs/MENUS.md](docs/MENUS.md)) |

**Build, tests and data**

| Path | Contents |
|---|---|
| `build-dev.bat` | Builds `letsgo.exe` (development) into `build/pc`, through `tools/build_exe.cmd` (step 4) |
| `CMakeLists.txt`, `cmake/host-x86_64.cmake` | The `letsgo` and `LetsGoPikachu` executables: runtime, recompiled code and the decomp module |
| `cmake/guest-aarch64.cmake` | CMake toolchain for ARM64 guest (decomp) code |
| `tests/` | `run_smoke.sh` and the smoke tests it runs |
| `docs/` | The plan and roadmap (`PLAN.md`), what's in the binary (`BINARY_NOTES.md`) and mods (`MODS.md`) |
| `data/` | Function, import and vtable databases, census, reachability. Addresses and names only, safe to commit |
| `orig/` | Your extracted game (`exefs/`, `romfs/`, `elf/`, `analysis/`, `ghidra/`). Never committed |
| `toolchain/` | Everything from step 1. Not committed |

### Smoke tests

| Test | Proves |
|---|---|
| `load_main.cpp` | main maps at `0x7100000000` on this PC and all 74,835 relocations apply |
| `patch_module.cpp` | ARM64 decomp code links against original functions by address, with matching class layouts |
| `nvn_layout.py` | All 96 NVN types have identical layouts on the PC and the Switch |
| `guest_headers/` | musl + libc++ + nnsdk/nnware (SDK, NintendoWare, nn::gfx, NVN) compile for the guest, and symbol names match the game's imports |
| `emu_fnv.py` | The emulator runs an original game function and its results match an independent implementation |
| headless boot | The recompiled game boots, presents its first frame, and keeps presenting at 30 fps without errors |
| shader translation | Every NVN program the game ships translates to GLSL and compiles to SPIR-V |
| movie and sound | The title movie decodes and plays to its end, and both the game's sound (Opus) and the movie's (Vorbis) are audible (`audible.py` checks the recordings) |
| decomp | The decompiled code builds, and every decompiled function behaves like its original in the emulator (`decomp/tools/difftest.py`) |
| mods folder | Mods replace and add files inside a made-up gfpak and the game's own, which hold exactly what they should (`tools/check_mods.py`) |

## Reporting problems

Open an [issue](https://github.com/pokefan2000XX/letsgo-pc/issues) that says what happened and what you were doing,
plus your GPU and driver version. Attach `letsgo.log` from
`%APPDATA%\LetsGoPikachu\` (for a development build, the folder you started it in).

## Legal

This project isn't affiliated with or endorsed by Nintendo, Game Freak,
Creatures Inc. or The Pokémon Company. Pokémon and Pokémon: Let's Go, Pikachu!
are trademarks of their respective owners. The downloads contain none of the
game's assets (models, textures, sound, text or movies), so the game won't run
without your own dump.

This project's own work is under the [MIT License](LICENSE), which grants no
rights to the game or its code, assets or trademarks. The licenses of
the libraries built into the executables are in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
