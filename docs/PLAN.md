# Plan: Let's Go Pikachu on PC

Goal: one Windows executable plus asset folders, built on your own machine from
your own dump, with no emulator, as the base for a ROM hack: new models, maps,
sound, code, battle mechanics, cutscenes, cameras and UI. It's a **static
recompilation** of the Switch code, and the parts a hack needs get
**decompiled** into readable C++, one function at a time, so it stays playable
the whole way.

```
original main (ARM64)  +  patch module (decompiled C++, also compiled for ARM64)
                 \              /
            recompiler (recomp/)  ->  generated C, one function per guest function
                          |
runtime (C++, x86-64): loader, guest threads/contexts, indirect-call dispatcher,
the 842 SDK/libc imports, NVN on Vulkan, audio, input, files, saves
                          |
          LetsGo.exe  +  romfs/  +  shaders/  +  save/
```

## Decisions and the evidence behind them

Numbers come from `data/` (`main_census.json`, `main_functions.csv`,
`main_imports.csv`, `main_vtables.csv`, `main_reachability.json`) and the smoke
tests (`bash tests/run_smoke.sh`).

1. **Guest address == host address.** main is mapped at `0x7100000000`,
   host-implemented imports get trampolines at `0x7200000000`, and the
   decomp patch module sits at `0x7102000000`. Heap, stacks and TLS are host
   allocations, so pointers pass between recompiled and native code untouched.
   Both sides are little-endian and 64-bit: no byte swapping, no pointer
   translation. `tests/smoke/load_main.cpp` maps main there and applies all
   74,835 relocations on this PC.
2. **Function boundaries are known.** `.eh_frame` has 59,323 FDEs covering
   all of `.text` except alignment padding. The only uncovered bytes are the
   module header (48 bytes) and the PLT header (32 bytes). One helper without
   unwind info, `__clang_call_terminate`, is found through its call sites.
3. **One C function per guest function** (`void f_7100xxxxxx(Ctx*)`).
   - `BL` becomes a direct C call, and `B` into another function becomes a tail call.
   - `BLR` (109,832 sites) and computed `BR` (3,207) go through a dispatcher. It's a flat
     table indexed by `(addr - base) / 4` for main and the patch module, plus the trampoline range.
   - The 1,930 jump tables are recovered into `switch` statements.
   - `data/main_vtables.csv` lists 6,328 vtables (54,771 slots) and 3,528 other
     function-pointer tables: the bulk of the indirect-call targets.
4. **CPU state** lives in a per-thread context: X0–X30, SP, NZCV, V0–V31,
   FPCR/FPSR and TPIDR_EL0. Registers are cached in C locals inside each
   function and spilled around calls, so the C compiler can optimize them.
5. **ARMv8.0 with a small surface.** There are 190 distinct mnemonics, no LSE
   atomics, and no crypto or CRC instructions.
   - LDXR/STXR (891 pairs) and LDAXR/STLXR (1,611 pairs) become a host
     compare-exchange against the value seen at the load.
   - LDAR/STLR become acquire/release atomics, DMB becomes a fence, and CLREX
     clears the monitor.
6. **System registers: only TPIDR_EL0** (5 reads, one TLS slot at tp+0x10).
   There are no SVCs in main. Every OS call goes through an import.
7. **Floating point keeps ARM semantics where x86 differs.** About 94k scalar
   FP and 50k SIMD instructions use 47 FP mnemonics.
   - Float-to-int conversions (FCVTZS/U and friends) saturate.
   - FMIN/FMAX/FMINNM/FMAXNM follow ARM's NaN rules.
   - FMADD/FMSUB/FNMADD/FMLA are fused (`fma`).
   - FRECPE/FRSQRTE need ARM's exact estimate tables.
   - **FPCR:** main never writes FPCR or FPSR and imports none of the `fenv`
     functions. nnSdk only touches FPCR inside `fesetround`/`fesetenv`. So the
     game runs with round-to-nearest and no flush-to-zero, which is x86's
     default too. This assumes Horizon starts threads with FPCR = 0.
8. **C++ exceptions are effectively unused.** One function has an LSDA, and
   it only calls `std::terminate`. A throw is an abort.
9. **Traps:** the 432 `0xe7ffdefe` words and the `udf` words abort with the
   guest address.
10. **The SDK is recompiled too** (refined during M1). `sdk` is translated the
    same way as `main` and loaded at `0x7180000000`, so libc, libc++, printf,
    math, nn::util, nn::crypto and `nn::os` internals run exactly as on the
    Switch.
    - The runtime replaces the Switch at two thin boundaries:
      - the kernel: the SDK's 60 SVC wrappers, in `runtime/src/kernel.cpp`
      - service-backed `nn::` functions: anything that would do IPC (fs, hid,
        vi, audio, account, applet...), overridden by symbol name in
        `runtime/src/hle.cpp`
    - The loader resolves symbols in ELF interposition order (main first: the
      game's own `malloc` wins), then applies overrides.
    - Startup reproduces rtld: `nn::init::Start(main thread, arg buffer, cbA, cbB)`.
      cbB runs `main`'s and then the SDK's DT_INIT.
    - 809 of the 842 imports are statically reachable from the boot entry
      points (`data/main_reachability.json`). Any import nobody provides logs
      its name when called.
    - Hot paths must be cheap: `__cxa_guard_*` (7,262 call sites each),
      `nn::os::GetTlsValue` (4,492) and `nn::os::Lock/UnlockMutex` (~1,000).
    - Data imports (nn::util math tables, hid NpadId constants, libc++
      vtables, `std::nothrow`) are provided with the guest layout.
    - `nninitStartup` sizes the game heap from `nn::os::QueryMemoryInfo`
      (`SetMemoryHeapSize`, `AllocateMemoryBlock`). The runtime has to report
      and reserve Switch-sized ranges and commit them lazily.
11. **Graphics:** the only NVN import is `nvnBootstrapLoader`. Every NVN
    call goes through the function pointers it returns, so the runtime hands
    back trampolines into an NVN-on-Vulkan layer.
    - nnsdk's `nvn/nvn.h` declares all 476 NVN function-pointer types and 96
      NVN types. All 96 have the same size and alignment on host and guest
      (`tests/smoke/nvn_layout.py`), so the runtime includes it directly and
      works on the game's NVN objects in place. NVNdevice is 12 KB and NVNqueue
      8 KB of game-owned storage.
    - Shaders (`.bnsh`: Maxwell binaries plus NV assembly text) are converted
      once to SPIR-V.
    - `glslcCompile` (runtime GLSL compiles, normally done by subsdk0) maps to glslang.
12. **Middleware stays recompiled.** Wwise 2017.2, SiSDK 3.0 + YEBIS,
    NintendoWare, nn::gfx, NEX/Pia, protobuf, curl, boost and FreeType are all
    inside main. They're decompiled only if a feature needs it.
13. **Toolchain** (`tools/setup_toolchain.sh`, everything pinned and hash-checked):
    - llvm-mingw (clang 23.1.3) builds the x86-64 runtime and generated C with
      the Itanium C++ ABI, and compiles the ARM64 decomp code.
    - Windows `long` is 32-bit while the guest's is 64-bit, so runtime structs
      that mirror guest memory use fixed-width types.
    - The rest of the toolchain is Ghidra 12.1.4, musl 1.2.6 headers, nnsdk +
      nnware and Unicorn 2.1.4, as described in the decomp track.

## Decomp track (runs alongside the milestones)

- **Build:** decompiled C++ is compiled for ARM64 with the same ABI and layout
  as the original, then linked into the patch module against original functions
  and data defined by address (`tests/smoke/patch_module.cpp`).
- **Guest toolchain:** `cmake/guest-aarch64.cmake`.
  - Target and code generation: aarch64-linux-musl, Cortex-A57, inline LL/SC
    atomics, unwind tables kept (the recompiler reads them), `-fno-exceptions -fno-rtti`.
  - Version defines: NintendoSDK/NintendoWare 5.4.151, so the headers pick
    this build's layouts. For example, `nn::os::ThreadType` is 0x1C0 bytes before SDK 6.0.
  - Headers: musl 1.2.6 (`toolchain/guest-sysroot`), llvm-mingw's libc++
    with a guest `__config_site` (std::__1 like the game's), and open-ead nnsdk + nnware.
  - `tests/smoke/guest_headers` compiles the SDK, NintendoWare, nn::gfx and
    NVN headers this way, and the symbols it references match the game's
    import names exactly.
- **Integration (built 2026-10-15):** decompiled C++ lives in `decomp/`.
  - `decomp/tools/build.py` compiles it for the guest, resolves what it uses
    (`decomp/symbols.txt`, `FUN_<address>` names, main's import stubs), links
    `build/decomp/patch.elf` at 0x7102000000 and recompiles it. CMake runs it
    whenever `decomp/` changes.
  - The recompiler emits each original as `o<addr>` with a weak alias
    `f<addr>`, the name every direct call, tail call and dispatch-table entry
    uses. The patch module defines a strong `f<addr>` for each function it
    replaces. That stub runs the decompiled version, or the original when
    `LETSGO_NO_DECOMP` asks for it. main is never regenerated for a replacement.
  - The patch image is compiled into the exe and copied to its guest address at
    startup; `rc_lookup` covers its range.
- **Verification:** differential testing against an oracle. Byte-matching isn't needed.
  `decomp/tools/difftest.py` runs each `decomp/tests/test_*.py` against the
  originals and against the decompiled versions (patch.elf loaded into the
  emulator) and compares the recorded results and SDK calls.
  - The oracle is `tools/emu/guest_emu.py`: Unicorn runs original functions
    with main at its real address and imports served by Python handlers
    (`tests/smoke/emu_fnv.py`).
  - Decompiled functions (compiled for ARM64) and recompiled ones are both
    compared against it on the same inputs. Decomp verification can therefore
    start before the PC runtime exists.
- **What gets decompiled:** what each phase of the roadmap below needs. Before
  changing a struct's layout, every function that touches it must be decompiled.
- **Naming aids:**
  - Ghidra project `orig/ghidra/letsgo`: main (seeded with all 60,111
    functions) and sdk, both analyzed. Export C with `tools\ghidra\decompile.cmd`.
  - the 260 source paths in assert strings (1,946 functions, mostly middleware)
  - the call graph (`orig/analysis/main_callgraph.tsv`) and the sdk's 18,618
    named functions (`data/sdk_functions.csv`)
  - vtables and their slots (`data/main_vtables.csv`)
  - pkNX's FlatBuffers schemas
  - the Pawn native table (277 script commands lead to their handlers)
- **SDK coverage:** nnsdk + nnware declare 353 of the 618 nn/C-API imports.
  - Strong: os 60/86, fs 18/19, socket 40/40, ssl 19/19, mem 15/16, oe 14/16, vi 8/8.
  - Weak: hid 13/81, audio 2/22, bluetooth 0/34, ldn 4/26. Those get typed
    from the sdk's symbol names.
  - nnheaders (their frozen predecessor) is kept for reference only.

## Roadmap

The port is the base for a ROM hack. Its phases follow the hack's goals, most
important first, after the groundwork every goal needs. Code is decompiled only
where a phase needs it; everything else keeps running recompiled.

| | Phase | For | Done when |
|---|---|---|---|
| D1 | Pokémon data (**done** 2026-10-16) | everything | The record, the Pokémon object, the item and species data and the party are decompiled (main `0x710071c3f0`–`0x710073a130`, 613 functions) |
| 1 | Groundwork (**done** 2026-10-16) | every goal | A test hack (a changed file inside an archive and a hooked function) works in both exes |
| 2 | Models (**done** 2026-10-16) | new characters, objects and Pokémon | A new character and a new object (not replacements) appear in the field, and a replaced Pokémon model shows in the field and in battle |
| 3 | Maps (**done** 2026-10-16) | editing maps, new areas | An edited map (moved and added objects, a new warp) and a new area load and play |
| 4 | Audio (**done** 2026-10-16) | new and replaced sound and music | Replaced music and a new sound play in the game |
| 5 | Code and battle | new code, new battle mechanics, normal wild battles | A new mechanic (a new move effect) works in a trainer battle, and a wild Pokémon starts a normal battle where it can be fought, fled from or caught |
| 6 | Cutscenes (**done** 2026-10-17) | new in-game cutscenes | A new cutscene plays from a new trigger, with existing and new script commands |
| 7 | Cameras (**check passed** 2026-10-17; step 2 waits for a hack that needs it) | editing camera sequences | An edited and a new camera sequence play in a cutscene |
| 8 | Textures and UI (**check passed** 2026-10-17) | textures, menus, fonts, text | An edited layout, texture and text show in a menu |

Phases run in order, and so do the steps in each. Every step ends checked:
format tools rebuild an unchanged file byte for byte (or as a file the game
loads the same way), decompiled code passes `decomp/tools/difftest.py`, and the
game boots and loads a save with it active. Before changing a struct's layout,
every function that touches it must be decompiled. Files extracted from a dump
stay out of git like `orig/`: the repo keeps only the tools and the format
descriptions.

- **1. Groundwork**
  1. Archives: unpack and repack `.gfpak` (GFLXPACK, LZ4) and `.arc` (SARC).
     Gfpaks store paths only as hashes, so the tools keep a list of names.
  2. Mods folder: the game reads a file from `mods/` instead of the romfs when
     there's one, including files inside gfpaks, in both exes
     ([MODS.md](MODS.md)). A hack ships only its own files; the player's dump
     supplies the rest.
  3. Hooks: a replacement can call the original it replaces
     (`ORIG_<address>`, which runs `o<addr>`), so code can be added to a
     function without decompiling it first.
  4. A test hack that uses all three.
- **2. Models**
  1. Models and animations (`.gfb*` FlatBuffers) and textures (`.bntx`) to
     glTF and PNG/DDS, and back.
  2. Replacing a model needs only the mods folder.
  3. Adding one: the tables that register characters, field objects and
     Pokémon models, and the code that loads them, decompiled where it has
     fixed IDs or counts.
  4. New models use the game's materials and shaders. New shaders would need a
     compiler for the Switch's GPU and aren't planned.
- **3. Maps**
  1. Terrain, collision, placement (NPCs, objects, warps, triggers, spawn
     areas: `bin/field/param/placement/`), the area tables and the town map.
  2. The field code that loads areas and moves between them, reads placement,
     runs triggers and checks collision (decompiled). It pins the formats down
     and shows which limits are in code.
  3. New areas: whatever adding one needs beyond data.
- **4. Audio**
  1. Wwise banks (`.bnk`, version 128 = Wwise 2017.2) and streams (`.wem`):
     replacing sounds and music.
  2. New sounds and music: banks built with Wwise 2017.2's tools, and the
     tables that pick each area's music.
  3. Wwise itself isn't decompiled. Code that plays a new sound uses phase 1's
     hooks.
- **5. Code and battle**
  1. Writing new code: new files in `decomp/`, hooks, debugging.
  2. Moves and trainers to JSON and back.
  3. The battle core (decompiled): turn order, using a move, move effects,
     damage, status, switching and the end of a battle.
  4. Wild battles in place of the catch screen. The legendaries are already
     battled before they're caught, so the battle engine can fight a wild
     Pokémon. The work:
     - touching a wild Pokémon in the field starts a battle (the field's
       encounter code, next to phase 3's)
     - catching in battle: Poké Balls in the battle bag, the catch formula, the
       throw and shake animations, and what follows a catch (party or box,
       Pokédex, nickname)
     - the rest of a wild battle: running away, and EXP for defeating one
  5. Move animations (`bin/battle/waza/sequence/*.bseq`), so new moves can
     have them. The 7 battle AI scripts get phase 6's script tools.
- **6. Cutscenes**
  1. The 705 field scripts (Pawn AMX with 64-bit cells and compact encoding,
     calling commands by hash) to readable source and back, and messages to
     text and back.
  2. The 277 script commands (decompiled), written up as the scripting API:
     moving characters, animations, the camera, text, sound, fades, waits and
     flags.
  3. Event triggers: how a script starts (talking, stepping on a trigger,
     entering an area), from phase 3.
  4. New script commands in C++, where a cutscene needs something the game
     can't do.
- **7. Cameras**
  1. Camera animation files (`.gfb*`) to an editable form and back.
  2. The field camera (decompiled): following the player, each area's camera
     settings, and phase 6's camera commands.
- **8. Textures and UI**
  1. Textures anywhere, with phase 2's tools.
  2. UI layouts and their animations (NintendoWare layouts in `.arc`), fonts
     and UI text.
  3. The UI code (decompiled), only for new screens or new menu behavior.
- **Only when a hack needs it** (no phase of its own):
  - Saves: more event flags or values than the save has free, or new things
    to save.
  - Data tables (base stats, items, learnsets, evolutions, encounters). pkNX
    already reads several of them.
  - Other menus, the Pokédex, the rest of the field (the catch screen, GO
    Park, daycare, trades) and the rest of battle (its UI, camera and effects).
- **Never decompiled.** The game only calls into these, so they stay
  recompiled (decision 12):
  - networking: NEX/Pia, Game Freak's gflnet3, curl, protobuf
  - rendering and effects: SiSDK, YEBIS, nn::gfx, NintendoWare's 3D and effect
    libraries
  - sound: Wwise
  - NintendoWare's layout and font libraries, FreeType
  - the Pawn script interpreter, gflib3's internals (memory, files, threads,
    containers), boost and the SDK
- **Skipped completely** (no decompiling and no format work):
  - online and local wireless play: trades, battles, Mystery Gift
  - the Pokémon GO link (transfers into GO Park)
  - Poké Ball Plus
  - the system and settings screens
  - compiling the game for x86-64 without the recompiler, which would need
    all of it decompiled
- **D1 steps (done):** the record (encryption, checksum, block order) and the
  object that decrypts it around each access; about 200 field accessors; the
  Pokémon object, with the item data and the species data's tables that share
  its range; the party.
- **Phase 1 results (done 2026-10-16):**
  - Archives: letsgostudio reads and rebuilds every gfpak (643) and `.arc`
    (374) byte for byte, and names 88% of the gfpaks' full paths.
  - Mods folder (`runtime/src/mods.cpp`): a mod file replaces the RomFS file
    at its path, or the file with that path inside a gfpak, and one in a
    gfpak folder that the RomFS doesn't have on its own is added to that
    folder. When the game opens an archive, the runtime builds the one it
    sees: new tables, the archive's own bytes where they were, then the mods'
    entries (LZ4). Through mods, `tools/check_mods.py` put 44,458 of the
    game's files back into the 594 archives it could name files in, and every
    entry checked out.
  - Hooks: `ORIG_<address>` resolves to a second copy of main's addresses
    (`ORIGINALS` in `recomp/recomp.py`), which `rc_lookup` and the emulator
    map to the original's body ([decomp/README.md](../decomp/README.md)).
  - The test hack (`hacks/phase1-test`): a hook on the file open
    (`0x71000238e0`) logs each file and calls the original, and the bedroom's
    rugs (inside `t001r01.gfpak`) and the hero's cap (a file of its own) are
    recolored blue. Checked headless from the bedroom save in both exes, the
    player exe reading the NSPs. Its difftest passes.
- **Phase 2 results (done 2026-10-16):**
  - Textures (step 1): letsgostudio reads and rebuilds every BNTX (18,331
    files, 27,272 textures) byte for byte, and turns textures into DDS,
    `.astc` or PNG and back, with its own BC1 to BC7 encoders.
    `pokebdsp lgpe extract|textures|export|import` does it from the command
    line; the result goes into `mods/` at the file's RomFS path.
  - In the game (headless, bedroom save): the rug imported from pictures at
    128 × 128, 384 × 192 and 1,024 × 1,024 (16 times its own data) and the
    hero's cap re-encoded at its own size show. The cap made larger (708 KB
    or 1.4 MB against 356 KB) stops the game: its loader
    (`0x7100033740`, which sets up a texture file it was given) gets no
    buffer, so the hero's clothes have a size limit that step 3 looks at.
  - The hero's clothes limit (looked at in step 3): each clothing slot's
    textures load into a memory pool of a fixed size, 600 KiB for the cap
    ([MODS.md](MODS.md) has every slot's). The texture read
    (`0x7100032220`) takes its buffer from the pool and goes on without one.
    The sizes are a table in the code (`0x7100f49198`, used by
    `0x71000aebd0`), and `0x7100781380` has the same sizes written in, so
    raising them means changing both. That waits for a hack that needs it.
  - Models (step 1): letsgostudio reads and rebuilds every GFBMDL (1,115
    models) byte for byte, and turns them into glTF for Blender and back
    (`pokebdsp lgpe export-model|import-model`): skeleton, skin, meshes and
    base colours with the shader's texture transform. An import is rebuilt
    on the model it replaces, matching bones, meshes and materials by name.
    All 1,115 come back with the same bones, triangles and vertices.
  - From Blender, export with Data > Mesh > Vertex Color: Active. Its
    default drops the colours' alpha, where Pokémon keep data.
  - Replacing a model (step 2), in the game (headless new game, Professor
    Oak's introduction): the partner Pikachu (`bd0001_00`, a character
    model) with its ears made longer and a cone hat added in Blender, and
    Cubone with the hat, show changed and animate with the new parts.
  - A replaced Pokémon in the field and in battle (step 2), played by the user
    on 2026-10-16 with cone hats added in Blender to six early Pokémon: the
    hats show on wild Weedle and Caterpie in Viridian Forest (the encounter and
    the catch screen) and on Pidgey in battle. With this, phase 2's done
    criteria are met.
  - Animations (step 1): letsgostudio reads and rebuilds every GFBANM (7,916
    animations) byte for byte, and samples their tracks as the game's
    samplers do: fixed, per-frame and keyed tracks, holds, and packed
    rotations. `export-model --animations` puts them into glTF with the
    model. `import-animation` brings one back on the animation it replaces,
    keeping the material and visibility tracks and events glTF can't carry.
    Step 1 is done.
  - Replacing an animation (step 2), in the game (headless, the intro):
    - the partner Pikachu's three intro animations, written with the waist
      turned 60°, make it lean;
    - the same three, with an ear turned in Blender, turn the ear.
  - The mods folder, fixed on the way:
    - **The fault:** 206 animations replaced in one gfpak grew the patched
      archive from 8.7 to 13.7 MB. The game's buffer for it wasn't
      allocated, and the game stopped.
    - **The fix:** a patched archive leaves out the bytes of the entries a
      mod replaces and compresses the mods' entries better (hash-chain LZ4).
      It now grows only by what the mods add, and those 206 come to 8.59 MB.
    - **Rechecked:** `tools/check_mods.py --real all` passes again.
    - **Logging:** a host crash on a game thread now logs its module and the
      guest backtrace.
  - Closing the window no longer crashes on the way out: `SDL_Quit` freed the
    audio streams while the audio thread could still be feeding one (seen in
    `SDL_LockMutex`). The quit paths end the process at once instead.
  - Adding models (step 3), in the game (headless, bedroom save, walked
    downstairs):
    - **A new character:** a new entry in `bin/chara/table/chara_model_data.bin`
      and an archive of its own (`tr0999_00_newnpc.gfpak`, a recoloured copy of
      a model under a new name, borrowing another character's animations). It
      shows in the kitchen in the mother's place, and talking to it behaves as
      the mother does (no reply at that point in the story).
    - **A new object:** a new model and texture added to `buildmodel.gfpak`
      through `mods/` (a blue doll at 1.6 times the size), placed where the
      bedroom's Pikachu doll was.
    - **How characters are registered:** the table keys each one by FNV-1a
      hashes of a category (`npc`, `buddy`, `object`), a name and sometimes a
      variant, and gives its model, animation configs (`.gfbanmcfg`) and
      archives. Placement names an NPC by the same two hashes; field objects
      name their model by path.
    - **Limits:** the game loads the table whole as a FlatBuffer
      (`0x71000d2850`) and found the 148th entry, so no fixed count was hit.
    - **Not yet:** both use existing placement spots. Adding placement
      entries is phase 3's placement format.
  - Pokémon models (step 3): `bin/pokemon/table/poke_resource_table.gfbpmcatalog`
    gives each species, form, sex and colouring its model, config, archive
    and animation configs. letsgostudio reads and writes it byte for byte
    (`PokemonModelCatalog`).
    - **In the game** (headless, the intro): Cubone's entries pointed at a
      new archive, a copy of Cubone's with the hat added, show Cubone in the
      hat. Cubone's own archive was left as it was.
    - **Limits:** the game keeps the entries in a hash map sized from their
      count (`0x7100173550`), so there's no fixed count. Animation configs
      go in six places by use (`battle`, `capture`, `app`, `field`, `ride`,
      `buddy`), and there's no place for any other.
  - The game's materials on a new model (step 4): Blender's monkey head, given
    the bedroom doll's material in Blender and imported on the doll's model
    (`pokebdsp lgpe import-model`), stands on the dresser drawn by the doll's
    shader and texture. New geometry needs only a template model whose
    materials and shaders it uses.
- **Phase 3 results (done 2026-10-16):**
  - **Formats (step 1):** letsgostudio reads and rebuilds these byte for byte,
    each against every file the game has:
    - the 147 placement files the game loads
      (`bin/archive/field/placement.gfpak`, `PlacementFile`);
    - the 698 collision files (`.gfbcol`, `CollisionFile`);
    - the 144 terrain files (`terrain_data.bin`, `TerrainFile`): an area's map
      pieces, each with a box, a model, its animations, a wall and a floor;
    - the area, zone and vanish flag name tables (`AHTB`, `NameHashTable`);
    - the town map's 43 places (`beluga_townmap_data.bin`, `TownMapFile`):
      72-byte records of a kind, a zone, a second zone, a label, the place's
      areas on the map, its picture, guide and facilities.

    They're FlatBuffers built by flatc 1.7, with a vtable slot for every field
    (which shows each table's field count). The placement files were built
    from JSON in schema order. Collision and terrain files have their own
    build orders, which the schema-driven reader and writer take as options.
  - **What placement files hold:** an area's zones (257 in all) and everything
    placed in each: NPCs, objects, warps, items, trainers, grass, cameras,
    triggers, effects and more. Nearly everything placed starts with the same
    transform: position, rotation in degrees, scale, its name's hash and the
    flag that hides it. Shapes are transforms too. A warp names its
    destination area by name and its zone by hash.
  - **In the game** (headless, bedroom save):
    - the Pikachu doll moved from the dresser to the floor, and a second one
      added, show where they were put;
    - a new warp across the bedroom floor, a copy of the front door's, takes
      the player out into Pallet Town;
    - a new wall across the bedroom, four triangles added to its wall
      collision, stops the player before that warp, while walking left and
      right stays free.

    That is the edited map of phase 3's done criteria.
  - **A new area (step 3).** `t999r01`, with one zone `t999r0101`, is a copy
    of the bedroom with five dolls in a row. A warp in the bedroom leads into
    it, and a warp in it leads out to Pallet Town; both work. Its files:
    - its placement file at its own path;
    - a terrain file of its own;
    - the room's archive with its entries moved to `bin/field/model/t999r01/`;
    - the room's model, animation, collision, scripts and messages under the
      new names, with the animation's name changed inside its config;
    - its names in the area and zone tables.

    Leaving pieces out showed the game stops without the zone collision, or
    with another area's terrain, archive or animation config, and, once the
    zone's scripts load, without the zone's name or either script.

    letsgostudio's `pokebdsp lgpe new-area` now makes such an area from any
    room, with an entrance copied from a warp the user picks
    ([MODS.md](MODS.md#adding-an-area)). Its copy of the bedroom matches the
    first new area's files. In the game, an entrance copied from the front
    door's mat led into it when walked into, and the copy's own stairs led
    down to the player's house. A warp's kind (door or mat, stairs, point) is
    field 12 of its table; a copy of the stairs fires as soon as the player is
    in it.
  - **What the field code does with areas** (step 2, from the code and
    traces):
    - when the field starts, the game reads the placement files on a list of
      147 paths in its code (`0x710012cee0`), into a vector of read requests
      at +0x530 of the field object;
    - it finds a zone by searching those files for its hash (`0x7100132960`,
      `0x7100132aa0`), and an area by its name;
    - when the player warps to an area not on the list, it reads
      `bin/field/param/placement/<area>.bin` instead, which loads the room but
      not its zones' scripts;
    - entering an area loads its terrain file, the zones' messages and scripts
      (`bin/script/amx/<zone>.amx` and `init_<zone>.amx`), the area's archive,
      each zone's collision, and then, in a room, the piece's model, wall,
      floor and animations from the terrain file;
    - a zone's messages come through the script records
      (`bin/script/param/script_id/script_id_record.bin`: a script's hash, its
      AMX path and its message path), which is phase 6's work.
  - **Decompiled (step 2):** the placement lookups the field finds zones and
    areas with, in `decomp/src/field/placement.cpp` (`field::FieldSystem`,
    `decomp/include/field/field_system.h`): a file's root by index
    (`0x7100132900`), a zone by its hash (`0x7100132960`) and a zone with its
    area's name (`0x7100132aa0`). Four difftests on made-up placement files
    (875 checks: named, unnamed and short tables, no files, random sets) match
    the originals. In the game the first two ran decompiled from the bedroom
    save into the new area and out; the third wasn't called on that route.
  - **Limits and hooks:** the 147-file list is the only fixed limit found so
    far. The `new-areas` hack (`hacks/new-areas`) reads, after the list, the
    placement file of every area a mod adds to the end of
    `AreaNameHashTable.tbl`. That makes the new zone's scripts load. With no
    mods, the game runs as before.
  - **Left for later phases:** a new area's messages, which come through the
    script records (phase 6); saving in a new area, not checked yet; outdoor
    areas, whose zones are pieces of a bigger map with block archives; and a
    new place on the town map, whose layout's 48 areas are all used (phase
    8's UI work).
- **Phase 4 results (done 2026-10-16):**
  - **Formats (step 1):** letsgostudio reads and rebuilds the 265 Wwise banks
    (version 128, Wwise 2017.2) byte for byte, with every music track,
    segment, switch and container (`PokeBdsp.Lgpe/Audio/`, adapted from its
    BDSP code for version 135: 40-byte track clips, 24-byte destination
    rules). Of the 1,134 media, 1,133 are Wwise Opus (letsgo-pc decodes them
    with libopus) and one is Wwise PCM (the partner Eevee's voice), so new
    sound is written as PCM and needs no Wwise tools.
  - **What picks the music (step 2):** field 5 of a zone's information table
    is a state ID in the group `bgm_field`; the field music switch in
    `BGM_Common.bnk` plays a container of segments for each of 33 states, and
    the same objects are in `BGM_Event.bnk` and `FieldFootsteps_Normal.bnk`
    (loaded first). Pallet Town's music is a 22-second segment played once,
    then a 39-second one repeating.
  - **How sounds play:** the game posts Wwise events by name, through its
    wrapper `0x710016d670` to `GetIDFromString` (`0x71003b0270`) and
    `PostEvent` (`0x71003b1e50`).
  - **In the game** (headless, bedroom save, output recorded with
    `LETSGO_AUDIO_WAV`):
    - `pokebdsp lgpe replace-music` put a 440 Hz tone in place of the house's
      music: the game streamed the new file and the tone carried all of the
      output's energy, repeating without a gap (the game's own music spreads
      over many frequencies);
    - `pokebdsp lgpe add-sound` added `Play_pokebdsp_new_sound` (a 1,500 Hz
      tone) to `System_Common.bnk`, and the `new-sound` hack (a hook on
      `PostEvent`) posted it whenever the menu opened: it played after each
      of two presses of X.

    That is phase 4's done criteria. The user hasn't heard them yet.
  - **Music of a zone's own (step 2, the tables that pick each area's
    music):** `pokebdsp lgpe new-music` copies what the field music switch
    plays for a zone's state, gives the switch a new state that plays the
    copy, points the copy at a WAV and sets the zone's information table to
    the new state. In the game (headless, recorded):
    - the bedroom given its own music (a 660 Hz tone) played it, and with
      the ground floor given it instead, the bedroom played Pallet Town's
      music as the unmodified game does;
    - a new area (the `new-area` copy of the bedroom, entered from the save)
      given its own music played it, and walking down its stairs into the
      house switched to Pallet Town's music.

    Walking from the bedroom to the ground floor wasn't checked: the save's
    stairs are behind the story's first event.
  - **Left:** loops and intros inside new music; replacing one of the game's
    sounds in place.
- **Phase 5 progress (started 2026-10-16):**
  - **Moves and trainers to JSON and back (step 2):** letsgostudio reads and
    rebuilds the move table (`bin/pokelib/waza/waza_data.bin`, 743 moves of
    40 bytes) and the 602 trainers with their teams byte for byte, and
    `pokebdsp lgpe export-moves|import-moves|export-trainers|import-trainers`
    turn them into JSON with the game's English names and back into a mod
    folder (layouts from pkNX, checked on the dump). In the game (headless,
    with a temporary hack that called the game's own functions): with
    Tackle's power made 80, the game's move-data function returned 80 (40
    without the mod); with the rival's first team changed (trainer 1's Eevee
    made level 9 with Tackle, Thunderbolt and Growl), the game's trainer
    loaders returned the new team. The battle itself is the user's to check
    (`build/mods_run/battle_test` holds both changes).
  - **What the code does with them:** the move table loads at start and each
    field has a function (`0x710073c8e0` power, `0x710073c840` PP with PP Ups,
    `0x710073c950` type, ...), which loads the move into a cached object
    (`0x7100740070`) and reads the field by number (`0x7100740110`). A
    trainer loads when the battle starts (`0x71001d8700` its data,
    `0x7100b0f640` its team, always 240 bytes), and `0x71001d9210` turns any
    trainer number above 601, or 0, into 1: new trainers need that changed.
  - **How the battle runs a move's effect (for step 3 and the done
    criterion):** the battle code is about 5,900 functions at
    `0x71009c0000`-`0x7100ab0000`. Effects beyond a move's data are handlers,
    as in Sun and Moon: when a Pokémon uses a move, `0x7100a38010` looks the
    move up in a table of 351 (`0x71015cd300`: the move's number, then a
    function returning its handlers and their count) and registers them with
    `battle::AddHandlerSet` (kind 0 is a move; 211 sets at most, kept in
    priority order). A handler is an event number and a function called with
    (its set, the battle, the Pokémon, the set's work values); it reads the
    event's variables with `battle::GetVar` and changes them with
    `RewriteVar` and `MultiplyVar` (by a 4.12 fixed-point ratio). Facade's
    two show the pattern: on event `0x47`, when its user is the attacker
    (variable 3), it doubles variable `0x37` (the power ratio) if the user is
    burned, paralyzed or poisoned (`0x7100a095e0` gives the Pokémon,
    `0x71009e3f70` its status). Tackle, Thunderbolt and other moves whose
    data says everything have none. Abilities have a table of 226
    (`0x71015caa10`) and items one of 191 (`0x71015c8170`), kept from Sun
    and Moon although Let's Go's Pokémon have no abilities and hold no
    items; a table of 6 (`0x71015c8110`, kind 7) holds the Poké Balls
    (items 1 to 4) and item 107.
  - **A new move effect (step 1, writing new code):** the `new-move-effect`
    hack hooks `0x7100a38010` and gives Tackle a handler of its own on event
    `0x47` that multiplies its power by 5, registered the way the game
    registers its own (`hacks/new-move-effect`, written with the decompiled
    events below). Checked in the emulator on the game's event code: Tackle
    registers once (a second time is refused, as the original refuses a
    move it already has), and event `0x47`, called the way the power
    calculation (`0x7100a03c30`) calls it, gives a power ratio of `0x5000`
    (5) when Tackle's user attacks and `0x1000` (1) when another Pokémon
    does. The game boots with it; the battle is the user's to try:
    in the rival's first battle, the rival's Eevee's Tackle should take
    about five times the usual share of Pikachu's HP
    (`cmake -DLETSGO_HACKS=new-move-effect build/pc`).
  - **The battle's events, decompiled (step 3, first part):** the source
    file that runs every effect, `0x7100a66740`-`0x7100a67af0` (44
    functions), is `decomp/src/battle/event.cpp` (`battle/event.h`):
    - handler sets: added in priority order from a pool of 211, removed
      (or marked to be freed when the handler running ends), put to sleep,
      suppressed for one Pokémon's sets of a kind, looked up by kind and
      Pokémon; kinds 0 moves, 1 positions, 2 sides, 3 the field, 4
      abilities, 5 items (from the IDs in their tables: Intimidate and
      Clear Body, the berries)
    - calling an event: every set in priority order, skipping sets asleep,
      suppressed, running, or added during the same event, going on with
      the right set when a handler removes sets; the version nearly every
      event uses (142 calls) first asks every set's skip check and the
      owner's conditions (an ability's owner under condition `0x10`, an
      item's under `0x13` or with field effect 7, probably Gastro Acid,
      Embargo and Magic Room)
    - the event variables: frames of up to 96 labelled values, each
      rewritable, rewritable once, a ratio kept between a minimum and a
      maximum, or constant.
    Checked: 5 difftests (`decomp/tests/battle/test_event.py`, about 4,300
    checks: random sequences of every operation, events through made-up
    handlers that remove sets as they run, skip checks and conditions
    faked); 6 planted bugs were all caught. The game boots with it; the
    battle (the user's test above) runs through it.
  - **The events, mapped:** `python -P tools/battle_events.py [event...]`
    lists every event (197 numbers): where the battle calls it, the variables
    its frame sets up and reads back, and every move, ability and item that
    answers it, by name. The ones `battle/event.h` names: `0x46` a move's
    own base power (Flail, Eruption), `0x47` the power's ratio (Technician,
    type-boosting items), `0x4a` the attack stat (Huge Power, Thick Fat),
    `0x4b` the defense stat (Marvel Scale, Fur Coat), `0x5b` the damage's
    ratio (Reflect, Earthquake on a digging Pokémon), `0x6d` a Pokémon
    coming in (entry hazards, Intimidate); and the variables `2` (the
    Pokémon an event is about), `3` the attacker, `4` its target, `0x36`
    the base power, `0x37` the power's ratio, `0x38` the damage, `0x39`
    attack, `0x3a` defense and `0x3b` their ratio.
  - **A move's damage, decompiled (step 3, second part):**
    `decomp/src/battle/damage.cpp` and `calc.cpp` (`battle/damage.h`, 10
    functions): the damage calculation (`0x7100a016d0`), its power, attack,
    defense and same-type parts, and the battle's arithmetic from another
    file (the base damage, 4.12 ratios rounded up only above a half, the type
    effectiveness, the weather's ratio, random numbers). In order: the
    fixed-damage events (`0x59`, `0x5a`: Seismic Toss, Super Fang); the base
    damage, ((2 × level / 5 + 2) × power × attack / defense) / 50 + 2; the
    spread-move ratio; the weather (sun and rain make Fire and Water moves
    1.5 or 0.5, unless event `0x92` suppresses the weather: Air Lock); a
    critical hit (1.5); the random roll (85 to 100%); the same-type bonus
    (1.5, events `0x53` and `0x54`); the type effectiveness (7 is neutral,
    each step above doubling and each below halving, 0 immune); a burn (half
    for a physical move, unless event `0x4a` says not: Facade, Guts); and
    event `0x5b`'s ratio (Reflect). A sandstorm makes a Rock type's special
    defense 1.5. The roll's rule check (`0x71009ce420`) always says no in
    Let's Go. Checked: 3 difftests (`decomp/tests/battle/test_damage.py`,
    about 16,900 checks on made-up Pokémon, with made-up handlers changing
    every variable the events have); 10 planted bugs were all caught (one
    after its case was made more common in the tests).
  - **The turn order, decompiled (step 3, third part):**
    `decomp/src/battle/order.cpp` and `order_key.cpp` (`battle/order.h`, 7
    functions). Each action gets a key: its kind (most kinds that aren't
    moves go before every move), its move's priority plus 7 after event
    `0x11` (Prankster), its Pokémon's bracket after event `0xf` (Quick Claw,
    Stall) and its speed (the stat times event `0x13`'s ratio, halved by
    paralysis, at most 10,000, reversed in Trick Room; the key keeps its low
    13 bits). The actions are sorted highest first, equal keys in random
    order, before and after the brackets. Checked: 3 difftests
    (`test_order.py`, about 11,500 checks); 8 of 8 planted bugs were caught
    (two after the tests gained turns full of equal keys and speeds just
    over 10,000).
  - **Whether a move hits, and critical hits (step 3, fourth part):**
    `decomp/src/battle/hit.cpp` (`battle/hit.h`, 6 functions) and two more
    functions of the arithmetic file (`calc.cpp`, which now has
    `battle/calc.h`). A move hits without a roll under No Guard, Lock-On or
    Telekinesis, when it never misses (accuracy 101 in the move data) or when
    event `0x3f` says so (Thunder in the rain, Stomp on a Minimized Pokémon).
    Otherwise its accuracy (event `0x41`: Compound Eyes, Bright Powder,
    Gravity) goes through the attacker's accuracy stage less the target's
    evasion stage (event `0x40`: Keen Eye, Unaware; from 1/3 to 3 times),
    times 1.2 after a Micle Berry, at most 100, and 10 less when the target
    has 200 friendship or more (the player's Pokémon dodge). A critical hit
    takes the move's stage plus the attacker's, then event `0x44` (Super
    Luck, Scope Lens; Battle Armor and Lucky Chant stop it), at most 3: 1 in
    24, 8, 2 or 1, twice as likely with 255 friendship; stage 6 always lands
    one (Storm Throw, Frost Breath, Zippy Zap). A one-hit KO move never hits
    a Pokémon of a higher level, nor one that event `0x87` protects (Sturdy),
    always hits under No Guard or Lock-On, and otherwise hits its accuracy
    (event `0x42`: Sheer Cold) plus the difference of the levels, in 100.
    Checked: 5 difftests (`test_hit.py`, about 65,500 checks; each hit check
    is played again with the roll forced until its exact chance is found);
    22 of 22 planted bugs were caught (four after the tests began finding
    the exact chance and reading each variable's limits), and 10 of 10 in
    the one-hit KO check.
  - **Whether a Pokémon can use its move (step 3, fifth part):**
    `decomp/src/battle/before_move.cpp` (`battle/before_move.h`, 7
    functions) and two rolls of the arithmetic file (a percentage, and how
    many times a move of 2 to 5 hits hits: 2 or 3 35% of the time each, 4 or
    5 15%). In order: a turn of sleep is counted (it may wake up); asleep,
    it can't move unless event `0x22` says so (Snore, Sleep Talk); frozen, it
    can't unless its move thaws it (Flame Wheel; event `0xd4` can stop that:
    Burn Up) or it thaws (20%); a disobedient Pokémon loafs around or naps;
    a move without PP left fails; event `0x23` can stop it (Truant); then
    flinching, Focus Punch after a hit, Disable, Heal Block, Throat Chop,
    Gravity, Encore, a lock into one move, a Choice item's lock, Taunt and
    Imprison; confused, it hurts itself 33% of the time; paralyzed, it can't
    move 25% of the time; in love, 50%. Each of the 18 reasons picks its
    message. Checked: 3 difftests (`test_before_move.py`, about 34,000
    checks, every reason coming up); 26 of 26 planted bugs were caught.
  - **A condition's damage at the end of a turn (step 3, sixth part):**
    `decomp/src/battle/status_damage.cpp`, `condition_turn.cpp` and
    `battle_pokemon.cpp` (`battle/status_damage.h`; the first functions of
    the conditions' file and of the battle Pokémon's file) and the fraction
    of the maximum HP in `calc.cpp` (at least 1): 4 functions. For each
    condition a Pokémon that hasn't fainted has: a burn takes 1/16 of its
    maximum HP, poison 1/8, bad poison 1/16 times the turns counted,
    Nightmare 1/4 while it sleeps, Curse 1/4; event `0x82` may change that
    (Poison Heal, Heatproof) and event `0x98` stop it (Magic Guard); then
    Bind, Leech Seed, Perish Song and Ingrain have their turn (the eleventh
    part below), and Encore ends once its move has no PP left. Checked:
    4 difftests (`test_status_damage.py`, about 35,000 checks); 19 of 19
    planted bugs were caught.
  - **Whether a condition can be given (step 3, seventh part):**
    `decomp/src/battle/add_condition.cpp` (`battle/add_condition.h`, 7
    functions) and the sun check in `calc.cpp`. A Pokémon can't get a
    condition it has (unless the caller allows it again), nor a status
    while it has one; its types stop some (Electric types can't be
    paralyzed, Fire types burned, Poison and Steel types poisoned, Ice types
    frozen, Grass types seeded), nothing freezes in harsh sunlight, Yawn
    fails on a Pokémon with a status, and Gastro Acid on an ability that
    stays, unless event `0xd3` lets the condition past the types
    (Corrosion); then event `0x7c` can stop it (Limber, Insomnia, Leaf
    Guard, Safeguard), with event `0x7e`'s message. Each failure has its
    message. A move's condition is given after event `0x77` picks it when
    the move data says `0xffff` (Telekinesis), event `0x78` sees it (the
    binding moves) and event `0x79` can change its 8 bytes (Leech Seed,
    Foresight, Grip Claw); a message those events leave is put after the
    condition instead of its own. Checked: 4 difftests
    (`test_add_condition.py` and `test_give_condition.py`, about 55,000
    checks); 22 of 22 planted bugs were caught, and 13 of 13 in giving a
    move's condition.
  - **A move's added effects (step 3, eighth part):**
    `decomp/src/battle/added_effect.cpp` (`battle/added_effect.h`, 7
    functions) and the condition maker in `calc.cpp`. A move's chance to make
    its target flinch goes through event `0x83`: a move without one can get
    one (King's Rock, Razor Fang, Stench), it can be doubled (Serene Grace)
    or a move's own taken away (Sheer Force); then the flinch itself can be
    stopped by event `0x84` (Inner Focus, Shield Dust). The condition a move
    gives, with its chance and how long it lasts (counted, a random number
    of turns between the move data's least and most, tied to the attacker,
    or both), goes through event `0x7b` (Shield Dust, Serene Grace, Sheer
    Force, Tri Attack's pick, the binding moves). The chance of a move's
    stat change goes through event `0x67`, and the stat change itself
    through event `0x70` (Growth, Ancient Power, Gear Up), which can also
    multiply the stages. A damaging move's added effect goes by the move
    data's kind: 4 gives the condition rolled (unless the target has
    fainted), 6 makes the stat changes rolled, each numbered. A flinch sets
    the target's flinch flag, or makes a Pokémon that is focusing lose its
    focus (Focus Punch fails); a sure flinch that doesn't happen calls event
    `0x85`. Checked: 3 difftests (`test_added_effect.py`, about 32,400
    checks, every duration kind); 29 of 29 planted bugs were caught.
  - **Whether a stat stage can change (step 3, ninth part):**
    `decomp/src/battle/stat_change.cpp` (`battle/stat_change.h`, 4
    functions). The stages go through event `0x71` first (Simple,
    Contrary). A change fails once the battle is decided (a side has no
    Pokémon left that can fight), when the stage is at +6 or -6 ("won't go
    any higher/lower"), when the Pokémon is behind a substitute and another
    Pokémon causes it ("it doesn't affect", unless the caller lets it past),
    or when event `0x72` stops it (Clear Body, White Smoke, Hyper Cutter,
    Keen Eye, Big Pecks, Flower Veil, Full Metal Body, Mist), with event
    `0x73`'s message; each failure is added to a list of five at
    `Flow+0x2c1c`. A change that happens raises or lowers the stage as far
    as its limit, tells the clients (commands `0xa` and `0xb`) and puts its
    message ("fell" always, "rose" when the caller asks), then event `0x74`
    lets Defiant and Competitive answer it. Checked: 2 difftests
    (`test_stat_change.py` and `test_stat_stage.py`, about 31,300 checks);
    29 of 29 planted bugs were caught.
  - **Putting a message made beforehand (step 3, tenth part):**
    `decomp/src/battle/message.cpp` (`battle/message.h`, 1 function), and
    the fields of a message being put together (`BattleMessage` in
    `battle.h`: its number, its kind, its arguments and a value). Kind 1 is
    put as a message on its own, kind 2 as one starting with a Pokémon's
    name, each with its value when it has one; the condition and stat
    change code above use it. Checked: 1 difftest (`test_message.py`, about
    4,300 checks); 9 of 9 planted bugs were caught.
  - **The conditions' own turns (step 3, eleventh part, 2026-10-17):** the
    five functions the end of a turn calls after a condition's damage, in
    `decomp/src/battle/condition_turn.cpp` (`battle/status_damage.h`). They
    queue records for the rest of the turn to carry out (`Flow::NewRecord`:
    kind 3 a message, 5 HP restored, 6 damage that doesn't come from a move,
    `0x14` a Pokémon; their fields are in `battle.h`). Bind and the other
    binding moves take 1/8 of the maximum HP (1/6 with a flag in the
    condition, probably the Binding Band) and play an effect for Bind, Wrap,
    Fire Spin, Clamp, Whirlpool, Sand Tomb, Magma Storm and Infestation.
    Leech Seed, when there's a Pokémon at the seeder's position, takes 1/8
    (at most the HP left, compared in 16 bits), gives the same HP to that
    Pokémon, then queues a record about it. Perish Song puts its count while
    turns are left. Ingrain and Aqua Ring (condition `0x24`, named by its
    message, `BTL_STRID_SET_AquaRingRecover`) restore 1/16 of the maximum HP
    unless it's full (Aqua Ring not after fainting). Checked: 4 difftests
    (`test_condition_turn.py`, about 80,000 checks, the records compared as
    pushed and after); 17 of 17 planted bugs were caught.
  - **Whether a side is out (step 3, twelfth part, 2026-10-17):** the check
    the battle makes for its end, `IsSideOut` (`0x71009f2d80`, in
    `decomp/src/battle/side_out.cpp`), with what it asks: a side is out when
    it takes part (sides 0 and 1) and none of its clients taking part (0 to 3;
    a client's side is its number's low bit) has a Pokémon in its party that
    can fight, that is one that isn't an egg (a bad egg either) and whose HP
    isn't 0 (`BattlePokemon::CanFight`, `BattleParty::CountFighters`, the
    setup's `SideOfClient` and `HasSide` and their rules). Whether a battle
    ends once a side is out (`EndsWhenSideOut`) is always yes in Let's Go. 8
    functions; the stat changes' code (ninth part) called `IsSideOut` and
    `EndsWhenSideOut` as originals until now. The turn's loop (`0x71009f1530`,
    thirteenth part below) asks it after each action and stops the turn there
    when it says yes. Checked: 3 difftests (`test_side_out.py`, about 76,000
    checks); 10 of 10 planted bugs were caught; all 119 difftests pass. In the
    game (headless, from a copy of the forest save, with the eleventh part
    too): a wild Oddish was fought until it fainted, the battle ended with
    result 1 (won) and the player was back in the field. What the battle does
    once a side is out (the result, the end of the battle) isn't decompiled
    yet.
  - **The turn's loop (step 3, thirteenth part, 2026-10-17):** `RunActions`
    (`0x71009f1530`, in `decomp/src/battle/turn.cpp`) runs the turn's actions
    in their order from the one it's given, putting the rest in order again
    first when the flow asks for it (`OrderActions` from `reorderFrom`). After
    each one it stops the turn when a side is out (state 4, twelfth part),
    when the state is 1 or 6, or when a check made after the action says so
    (state 3; `0x71009f28c0`, not decompiled). When all have run, the end of
    the turn comes next (`EndOfTurn`, `0x71009f36a0`, not decompiled), then
    what the battle does next: state 4 when a side is out, 5, 1 or 3 when the
    end of the turn stops partway, otherwise the next turn (0), unless one of
    two checks of something at flow+0x1354 says yes: then each client with an
    empty position (one without a Pokémon) gets server command 0x31, and the
    state is 2. The flow's fields it uses are named (`state`, `actionCount`,
    `reorder`, `reorderFrom`, `actions`, `emptyClients`); `Action` moved to
    `battle.h` for that. Its caller is the fourteenth part below. 1 function.
    Checked: 1 difftest (`test_turn.py`, about 28,000 checks, everything the
    loop calls faked); 22 of 22 planted bugs were caught; all 120 difftests
    pass. In the game (headless, from a copy of the forest save, with the
    eleventh and twelfth parts too): a wild Pidgey was fought until it fainted
    after a few turns, the battle ended with result 1 (won) and the player was
    back in the field.
  - **The turn (step 3, fourteenth part, 2026-10-17):** `RunTurn`
    (`0x71009f10a0`, in `decomp/src/battle/turn.cpp`), which runs a turn or
    goes on with it. The first time (`turnStarted` not set) it gets the turn
    ready (seven calls, `WakeAllHandlerSets` and `ResetVars` among them) and
    makes the turn's actions (`MakeActions`, fifteenth part below; from its
    second argument, likely what the clients chose), then calls
    `0x71009f1490`; then it runs `RunActions` from the action where it stopped
    last (`nextAction`). When the state is then 0, 4, 5 or 6, what ends the
    turn comes next (`0x71009f1760`, not decompiled), and a turn that ends
    with state 0 makes the state 7 when flow+0x80 reaches a limit the setup
    gives (`0x71009d8d40`, 0 for none). It gives the state. 1 function.
    Checked: 1 difftest (`test_turn_driver.py`, about 46,000 checks,
    everything it calls faked, the loop too); 18 of 18 planted bugs were
    caught; all 121 difftests pass. In the game (headless, from a copy of the
    forest save, with the eleventh to thirteenth parts too): a wild Pidgey was
    fought for two turns until it fainted, the battle ended with result 1
    (won) and the player was back in the field.
  - **Making the turn's actions (step 3, fifteenth part, 2026-10-17):**
    `MakeActions` (`0x71009f1340`, in `decomp/src/battle/turn.cpp`): the
    actions themselves come from `0x71009f1c30` (not decompiled), then it puts
    them all in order (`OrderActions`). An action of kind 6 marks the move
    (kind 1) of its Pokémon, found by ID, to be ordered again, and the first
    action of kind 1 or 5 is where `RunActions` does that (`reorderFrom`, with
    `reorder` set; false when there's none). 1 function. Checked: 1 difftest
    (`test_make_actions.py`, about 15,600 checks, the actions, the order and
    the IDs faked); 14 of 14 planted bugs were caught; all 122 difftests pass.
    In the game (headless, from a copy of the forest save, with the eleventh
    to fourteenth parts too): a wild Butterfree was fought over several turns
    until it fainted, the battle ended with result 1 (won) and the player was
    back in the field.
  - **Wild battles (step 4, 2026-10-17):** the `wild-battles` hack starts a
    battle where a touch would open the catch screen: it keeps the 0x60-byte
    description the touch makes of the Pokémon (`0x7100305790`), puts it in
    a holder and starts the battle event the scripts' wild battles use
    (`CallWildBattle`, `0x710028bc40`), then takes away the boss setup that
    event adds (the 5-minute timer, the flag that also turns off EXP, the
    boss species' bits, the auras). After it the after-battle event checks
    evolutions instead of opening the catch screen, and the screen fades in
    from black as a script does after its wild battle (the event leaves it
    black). A move's effect that asks the wild Pokémon's battle model for a
    joint (`Waist`) crashed the game: in this battle that model (slot 1) is
    never loaded, because the battle core reports the position empty when
    the view makes its models (`0x71009db9a0`), and the Pokémon is drawn
    another way; the hack makes such a lookup find nothing, as for a model
    without that joint.
    - **Catching:** the battle core still has Sun and Moon's. An item of
      class 5 (a ball) goes to the capture (`0x7100a10e30`, `0x7100a11000`:
      HP, catch rate, the ball's bonus with the Heavy and Beast Ball cases,
      status, critical captures by Pokédex count, four shake checks), and
      the battle client still says "You used a Poké Ball", "Gotcha! X was
      caught!" (or "Oh no! The Pokémon broke free!") and gives the capture's
      EXP; the battle ends with result 5. The battle bag lists an item when
      its byte in `bin/pokelib/item/battle_pocket.dat` isn't 0 (1 medicine,
      2 battle items): `hacks/wild-battles/make_romfs.py` writes the table
      with the balls as medicine. The bag's "Use this item" only goes on for
      items it knows (`0x710076ba50`): the hack has balls taken as escape
      items (`0x7100a18f60`, Poké Doll and the like, which only the bag
      asks about), so the bag asks which Pokémon (any will do) and the battle
      gets the ball (`0x7100a50b80` packs it: kind 2, the item at bit 7).
      The caught Pokémon then goes where the catch screen's go: the hack
      makes the catch event (`0x710028ee10`, its update `0x710031c070`) a
      frame before the after-battle event ends, as that event does for its
      catch screen (`0x7100319f80`), with a copy of the after-battle event's
      Pokémon (its +0xc0) given the ball that caught it, from its state after
      the catch screen (3) with the result "caught": the Pokédex page, the
      party or "You tucked X away in the Pokémon Box" (`0x7100b08120`), and
      the spawner's count (state 10).
    - **After a battle** the field is made again and filled with new wild
      Pokémon, as when the player walks in (a log of the field's objects
      showed none during the battle and all new ones after; the defeated one
      isn't brought back), so one can stand at the player: for 3 seconds
      after the field is back the touch (`0x71002fd8a0`) starts nothing.
    - **Checked** headless in Viridian Forest (a driver walks until something
      touches the player, then plays the battle): Butterfree, Pidgey, Oddish
      and Pikachu battles start without the timer, the menu has Fight,
      Pokémon, Bag and Run, both sides' moves play, a won battle gives EXP
      (Pikachu's bar grows), Run gets away (result 3), a Poké Ball from the
      bag caught a Pidgey on the first throw, and the player is back in the
      field each time. **Not done:** the throw has no animation (no ball, no
      shakes: the wild Pokémon stays on screen during the messages), and the
      bag's extra "Which Pokémon will you use it on?" step for a ball.
  - **Move animations (step 5, 2026-10-17)** ([SEQUENCES.md](SEQUENCES.md)):
    `tools/bseq.py` reads the battle sequences (`.bseq`: 353 files, the
    `SESD` format: a timeline of commands, each a start and an end frame,
    three values and its number), turns them into JSON with their texts
    (particle, model and camera files, sounds) and back; all 353 come back
    byte for byte. The files don't give a command's parameter size: the game
    has a table of them (`0x7100f97e30`, 227 commands), which the tool reads
    from `orig/exefs/main`. The battle plays `ew%03d.bseq` for a move by its
    number (`0x7100a81470`), after asking whether the file is there, and
    `ee000.bseq` (no particles, models or sounds) when it isn't; the mods
    folder answers that question, so a mod's file gives any move an
    animation. In the game (headless, a wild battle): Quick Attack with
    Flamethrower's particles and a sound, made through the JSON, hit the
    target with fire. letsgostudio's Moves page shows a move's animation, changes its
    texts and gives a move another's. Every parameter in the JSON has the type
    the game's code reads it as (int, float, x/y/z, true or false, text), from
    the readers each command's functions call: 132 commands typed that way, all
    adding up to the size table's; 70 have bytes no code reads, and 23 take
    none.
  - **The battle AI scripts (step 5, 2026-10-17)** ([BATTLE_AI.md](BATTLE_AI.md)):
    the 7 scripts already go through `tools/amx.py` (all 7 come back byte for
    byte from source). They call one command, `AI_CMD` (`0x7100a74140`), whose
    first argument picks one of 123 things from a switch: random rolls, HP,
    status, conditions, types and type effectiveness, stats, the move being
    weighed (power, type, category, PP, whether it knocks out), speeds,
    levels, abilities, the party. Read from the handler and named in
    `amx.py` (`commands --ai`), 35 of them placeholders for what the
    code reads; `disasm` notes the subcommand at 3,237 of the scripts' 3,248
    calls. No AI was changed in the game yet.
- **Phase 6 results (done 2026-10-17), the script work first
  ([SCRIPTS.md](SCRIPTS.md)):**
  - **Scripts to source and back (step 1):** `tools/amx.py` reads the format
    (Pawn 3.3's AMX with 64-bit cells, compact encoding, 215 opcodes with
    the packed ones, names as 32-bit hashes) and writes editable source:
    labels, commands by name, calls as statements (`var1 = FlagGet(12)`,
    `if IsMsgWinEnd_() goto l_1150`), strings as text, public functions by
    name. All 712 scripts (705 field, 7 battle AI) come back byte for byte;
    703 of the 705 field scripts are named (script_id_record.bin and every
    zone's `<zone>.amx` and `init_<zone>.amx`). A smoke test checks it on
    made-up scripts (`tests/smoke/amx_tool.py`).
  - **Messages to text and back (step 1):** `tools/msg.py` turns a message
    file (UTF-16 lines encrypted by index, with an `AHTB` label table) into
    text with its codes written out (`\n`, `\r`, `\c`, `[WAIT n]`,
    `[VAR code(args)]`…) and back. All 5,380 files of every language come
    back byte for byte, and a smoke test checks it on made-up messages
    (`tests/smoke/msg_tool.py`). See [SCRIPTS.md](SCRIPTS.md#messages).
  - **The commands (step 2):** the 12 tables the game registers give all
    389 names (the scripts call 277); `tools/amx.py commands --uses` lists
    them with their handlers and the argument counts of every call. What each
    does, from the handlers (Ghidra) and the calls:
    [SCRIPT_COMMANDS.md](SCRIPT_COMMANDS.md), all 277 the scripts use by
    topic, with the uncertain rows marked; the 112 unused listed.
  - **Triggers (step 3):** an event is a 64-bit hash, given by a placed
    thing (field 11 of its Transform), a scene change (a zone script's
    `GetSceneChangeData`), a fixed hash in the code (trainer sight, items,
    field moves) or a reserved script; script_id_record.bin maps it to the
    AMX and its message file, and the script's `main` switches on it. Zone
    init scripts run with 0 on entering the zone and 1 repeatedly.
  - **New commands in C++ (step 4):** the `new-script-command` hack adds a
    table of its own where the field registers its seven and its four, so
    any script can call `PokefanLog` and `PokefanAdd` by name.
  - **Checked in the game** (headless, a save in the bedroom): the bedroom's
    zone and init scripts, edited to log their event through `PokefanLog`,
    ran with event 1 about twice a frame (init) and the zone's event
    `0x14d1fe7a8c39292d`; a script written from scratch counted its runs and
    logged `PokefanLog(60, PokefanAdd(60, 1000))` = `60 1060` on the 60th.
    The first edit crashed the game: the tool hadn't labelled the public
    function, so its entry stayed behind when code moved; it labels them now.
  - **The check: a new cutscene from a new trigger** (`hacks/new-cutscene/`,
    [SCRIPTS.md](SCRIPTS.md#a-cutscene-of-your-own)). `make_romfs.py` adds a
    box trigger to the bedroom's placement file (with `tools/flatbuf.py`,
    which writes the zone's longer list of triggers after the file's end) and
    a record for the new event to script_id_record.bin. It also assembles
    the script and builds its two lines. The script fades the screen out and
    in, turns the player, shows the two lines with A to close each, and
    walks the player back out of the trigger. It calls the existing commands
    and logs its start and end with `PokefanLog` and `PokefanAdd`. Checked
    in the game (headless, from a copy of a bedroom save): `1 0` when the
    player walked in, and `2 42` once, at the end. The trigger runs its event
    whenever the player is in it and its condition holds, so the walk back
    matters: without it the event ran again 0.13 s after each end. A trigger
    byte that 19 of the game's 22 unconditional triggers set (field 6) kept
    the new one from running at all. It has a trigger wait until the player
    faces the way its shape is turned, as the elevators' 19 do (checked in
    the game 2026-10-17, [SCRIPTS.md](SCRIPTS.md)). The
    assembler now refuses a script whose strings would cover a public
    variable, which a first draft without `.data` did.
- **Phase 7 progress (started 2026-10-17):**
  - **Camera animations to an editable form and back (step 1):** the
    demos' and battles' camera animations (`.gfbcama`, 155 files, all
    loose) are GFBANM's tracks in three groups: lens (near, far, field of
    view in radians, aspect), transform (rotation, position, up, target,
    roll) and depth of field. The studio reads and writes them byte for
    byte and turns them into JSON and back (`pokebdsp lgpe export-camera`
    and `import-camera`). The JSON takes keys, which the import turns into
    a value a frame, and rotations that face a point; all 155 come back
    byte for byte through it. Demo cameras turn by their rotation and look
    down their −z; battle cameras look at a target. The `.gfbcam` settings
    files beside them aren't read yet.
  - **The check: an edited and a new camera sequence in a cutscene**
    (`hacks/new-camera/`, [SCRIPTS.md](SCRIPTS.md#demos-and-their-cameras)).
    A cutscene of its own (`new-cutscene`'s code, with its own event and
    script) plays two of the game's demos with `PlayDemoScene_`. Zapdos's
    has its camera edited through JSON (the field of view 1.5 times wider
    all the way through). Bill's teleporter demo has a new camera, made
    from keys around the point its own cameras look at, in a new file
    that its sequence is changed to name (paths sit in 256-byte fields).
    Checked in the game (headless, a copy of a bedroom save): both demos
    played with the changed cameras, the screen faded back in on the
    bedroom and the cutscene logged its steps to the end. Playing Zapdos's
    demo with its own event's flags, `(0, 0, 1, 0)`, and then another demo
    crashed the game; with Bill's `(1, 0, 1, 1)` both played.
  - **Not yet (step 2):** the field camera (following the player, each
    area's camera settings) isn't decompiled; nothing needed it for the
    check. Nor is adding a demo of its own (an entry in
    `demo_data.prmb`).
- **Phase 8 progress (started 2026-10-17):** ([MENUS.md](MENUS.md))
  - **Textures anywhere (step 1):** phase 2's tools (the studio's BNTX
    export and import) do the menus' textures too, once their archive's
    BNTX is taken out. The menus' archives are SARCs (374 `.arc`, all in
    `bin/app/`), which `tools/sarc.py` reads and writes, all 374 byte for
    byte. The mods folder doesn't reach inside them, so a mod has a whole
    `.arc`.
  - **Layouts and UI text (step 2):** `tools/bflyt.py` lists a layout's
    panes (1,091 layouts, 18,821 panes, walked) and changes a pane's place,
    turn, scale, size, alpha and whether it shows, in place. It also lists
    what an animation keys (4,363 animations), since an animation
    overwrites those values while it plays. The menus' words are message
    files, which `tools/msg.py` already does. The fonts (`bin/font/`,
    OpenType XORed with the system fonts' key) go to `.otf` and back with
    `tools/font.py`, all 3 byte for byte. `FOT-UDKakugoC80Pro-DB` draws the
    English menus' and message boxes' text, and the game takes a TrueType
    font in its place: `hacks/new-font` with a Windows font showed it in
    the main menu and a message box (headless).
  - **The check: an edited layout, texture and text in a menu**
    (`hacks/menu-edit/`). The main menu's Bag button raised and made a
    quarter larger (its opening animations key only the buttons' x), its
    icon turned from yellow to purple in the BC2 blocks through the studio,
    and BAG renamed BACKPACK. Checked in the game (headless, a copy of a
    save in Viridian Forest): all three showed in the X menu.
  - **New pictures (2026-10-17):** the studio's Menus page adds a picture to
    a menu, a copy of another's pane and material with a texture of its own
    (the layout rebuilt, every other section the same); one added to the main
    menu showed in the X menu (headless).
  - **Not yet:** removing panes, adding other kinds, and changing materials
    or animations (a rebuild of the file, not a change in place); the bitmap
    font (`num_00.bffnt`) and the archives' font settings (`.bfcpx`); the
    menus' code (step 3, only for new screens or behaviour).

## Milestones

| | Milestone | Done when | New dependencies |
|---|---|---|---|
| M0 | Prep | Extraction, ELFs, function/import/vtable DBs, census, Ghidra project, guest toolchain, emulator oracle, smoke tests | llvm-mingw, Ghidra, musl, nnsdk/nnware, Unicorn (done) |
| M1 | Recompiler core + headless boot (**done** 2026-10-14) | All of main translated. The runtime loads it, runs static constructors and `nnMain` on a guest thread, and the game reaches its first frame present with a logging-only NVN | none |
| M2 | Window + input (**done** 2026-10-15) | SDL3 window, hid mapped to gamepad/keyboard as Handheld | SDL3 |
| M3 | Graphics (**done** 2026-10-15) | NVN on Vulkan, shader converter, texture deswizzle; title screen renders | Vulkan headers, glslang |
| M4 | Audio + movies (**done** 2026-10-15) | nn::audio output (Wwise itself runs inside main), WebM, software keyboard | opus, libogg/libvorbis, libvpx (nasm to build it) |
| M5 | Playable (started 2026-10-15) | Field, battle, menus and saves match Ryujinx | none |
| M6 | PC features | Resolution, frame rate (the mod loader is roadmap phase 1) | none |

## M1 results

- **Boot:** main and sdk are recompiled (95,508 functions). The game runs its
  400 constructors and `nnMain`, sets up NVN through `nn::gfx`, presents its
  first frame 0.35 s after start, then presents steadily at 30 fps (present
  interval 2 on a virtual 60 Hz display).
- **How far it gets:** it loads its resident data (fonts, messages, models,
  Wwise banks), shows the first-boot controller and strap screens, and
  responds to input (`--press`). The title screen loads, and the run stops
  cleanly when the title's background movie starts, because movies are M4.
- **Runtime surface:** 216 `nn::` overrides (fs, oe, settings, account, time,
  pl fonts, hid, audio out, hardware Opus, Bluetooth LE, ldn, bcat, prepo,
  friends, nifm/socket/ssl offline) and a kernel subset (threads,
  arbitration, events, memory, info).
- **Carried forward:**
  - M3: YEBIS post-effects reject the logging NVN (`E_INVALIDARG` from the
    auto-exposure luminance query). The NVN API version 53/207 is still
    unverified. 10 newer NVN entry points the game asks for return null.
  - M4: the title movies are VP9 + Vorbis WebM (`bin/movies/PIKACHU_GO/`).
    Implement the `movie::` API (Android MediaExtractor/MediaCodec style;
    vtables via `tools/vtables_of.py`).
  - Software keyboard, BCAT downloads, NSA tokens and real sockets are
    left until the game first reaches them.
- **Lessons:**
  - hid must signal style-set update events when they're bound, or the game
    never sees a controller.
  - The exe opts out of high-entropy ASLR, so the loader's own allocations
    stay out of the fixed guest regions.
  - Guest threads end with the OS's `ExitThread`. `__builtin_longjmp`
    across recompiled frames crashed under Windows SEH.

## M2 results

- **Window:** SDL 3.4.18 on the process's main thread. Guest threads never
  call SDL; input and rumble cross over through locked state and atomics.
  Until M3 renders, the window shows a status screen (frame rate, the input
  the game sees, recent file loads). `--screenshot` captures it, also with
  the invisible dummy video driver.
- **Input:** keyboard and every gamepad merge into the Handheld controller.
  Face buttons follow the printed labels, with a 12% radial stick dead zone.
  The stick-direction bits (NpadButton 16-23) are derived the way hid does,
  at half deflection. `--press` still works on top.
- **Rumble:** the SDK sends vibration through `hid::detail::SendVibrationValue(s)`
  via its PLT. The Handheld's two motors combine into the gamepad's low and
  high motors.
- **Usability:** double-clicking the exe works (it finds `orig/`). Saves
  live in `save/` beside `orig/`. Fatal stops show a message box. The
  exception handler ignores first-chance exceptions outside the exe
  (drivers, system DLLs).
- **Still stops at** the title screen's movie (M4).

## M3 results

- **Backend** (`runtime/src/gpu/`): NVN on Vulkan 1.3+ with `VK_EXT_shader_object`,
  dynamic rendering, push descriptors for uniform buffers, and a bindless set
  that mirrors the NVN texture and sampler pools. Guest memory pools are imported
  as Vulkan buffers (`VK_EXT_external_memory_host`); storage buffers are read
  through buffer device addresses. Command buffers record op lists that replay
  into one Vulkan command buffer per `QueueSubmitCommands`. Frames reach the
  window through a swapchain blit of the `nvnWindowSetCrop` rectangle.
- **Shaders:** two translators to GLSL 4.60, compiled to SPIR-V by glslang.
  - 1346 programs carry the NV_gpu_program5 assembly glslc embedded in their
    control section (`nvasm.cpp`).
  - 99 stages have only Maxwell (SM 5.x) machine code: NintendoWare's ui2d and
    font built-ins, the movie's YUV shader, YEBIS post-effects (`maxwell.cpp`).
    The opcode table comes from Ryujinx (MIT). Branches become a dispatch loop
    with an SSY/SYNC stack.
  - NVN's hardware constant buffer slots: 0 driver data (storage buffer
    addresses at 0x110 + 16N), 1 the compiler's constants (in the code blob at
    control+0x700), 2 texture handles (binding N at byte 0x20 + 8N), 3 + N
    uniform buffer N.
- **Textures:** block-linear deswizzle; BCn uploads compressed; ASTC and packed
  16-bit formats convert. Packaged textures load from CPU_NO_ACCESS pools at
  creation. Textures the CPU writes through a mapping (the font glyph caches)
  refresh before each submission, found with Windows write-watch on the heap.
- **Movies (M4 groundwork):** `movie::` is host code (`hle_movie.cpp`,
  `webm.cpp`). It parses the WebM, speaks the player's event protocol
  (input/output/format/error events, status 0x2000 = end of stream) and paces
  the decoders to the stream. Video outputs black NV12 frames until the VP9
  decoder arrives, so the title movie plays its 23 s as a black screen.
- **How far it gets:** the first-boot controller screens, "Presented by GAME
  FREAK", the title movie (black), language selection, and the professor's intro
  with Oak and Pikachu in 3D through YEBIS's bloom chain. Text renders in
  Latin, Japanese, Korean and Chinese scripts.
- **Carried forward:**
  - Compute dispatch is a no-op (YEBIS auto-exposure). Four compute programs
    sample 3D textures, which the assembly translator doesn't support yet.
  - `ReportCounter` writes CPU timestamps, and samples-passed counters report 1.
  - Geometry and tessellation stages are unsupported (none seen yet).
  - Two NVN textures over the same memory are separate Vulkan images.
  - The ASTC decoder and the 16-bit packed formats' bit orders aren't checked
    against real data yet.
  - Oak and Pikachu show as white silhouettes during "Welcome to the world of
    Pokémon!". That's probably the intended effect; compare with Ryujinx.
- **Lessons:**
  - NVN programs are separable. Bindings are per stage: fragment-only programs
    and `BindProgram(null, FRAGMENT)` are common.
  - The movie renderer binds `nvnDeviceGetTexelFetchHandle` handles (a texture
    with no sampler).
  - A branch or SSY target on a scheduling word means that group's first
    instruction.
  - The first launch after a rebuild can be slow (antivirus scan of the new
    exe), so test scripts time input by frame (`--press A@f150`).
  - Debug tools that paid off: `LETSGO_DUMP_FRAME` (render targets, sampled
    textures, per-draw state and GLSL) and `--nvn-trace`.

## M4 results

- **Sound:** Wwise runs inside main and mixes into `nn::audio` AudioOut: a
  6-channel stream from boot, plus a 2-channel one the movie player opens.
  AudioOut plays through SDL3's default device and releases each buffer once
  the device has played it, so the game's audio timing follows the sound card.
  Without a device, a clock stands in. Wwise's streams use the Switch's
  hardware Opus format (an 8-byte big-endian packet header), decoded with
  libopus 1.5.2.
- **Movies:** subsdk1's `movie::` is host code.
  - `webm.cpp` parses the container. The decoders use libvpx 1.17.0 (VP9 to
    NV12, written into the game's texture memory) and libvorbis 1.3.7 (16-bit
    PCM in 1024-frame chunks, one AudioOut buffer each).
  - Decoding runs on the game thread that sends the input, about 2 ms per
    1080p frame.
  - The game paces playback: it presents each picture when it's due and feeds
    audio as AudioOut drains. A decoder keeps at most 8 pictures or 64 audio
    chunks unreleased and refuses input past that.
  - The 23.0 s title movie plays in 24.0 s, all 691 pictures with its music.
- **Software keyboard:** `nn::swkbd::ShowKeyboard` opens a small Win32 text
  box over the game window. The game's text-check callback (length, name
  rules) runs on the answer. `--keyboard "Ash;Gary"` scripts the answers for
  tests. The profanity filter (`nn::ngc`) accepts everything.
- **Recompiler:** the Pawn script interpreter (main 0x7100670710) dispatches
  opcodes with a computed goto: a table of label addresses stored as RELATIVE
  relocations. `recomp.py` makes those addresses labels in their function and
  turns a BR without a jump table into a switch over them. With that, the
  game's scripts run past naming.
- **Windowed check:** on the RTX 5060, sound plays through WASAPI, and both
  name boxes open over the game window (`LETSGO_DIALOG_TEST` captures and
  answers them).
- **How far it gets (headless, 30 fps):**
  - the title movie with picture and sound, then the title screen's music
  - language selection and the professor's intro
  - naming the player and the rival
  - the player's bedroom in Pallet Town ("It's a Nintendo Switch!")
- **Carried forward:**
  - The bedroom renders with black floor and walls (M5).
  - BCAT downloads, NSA tokens and real sockets are still offline stubs.
- **Lessons:**
  - Bound a decoder by its unreleased outputs, not its inputs: the game's
    pacing arrives as releases.
  - At the end of a stream the game marks every input buffer it holds as the
    end. One end-of-stream output answers them all.
  - Saving a screenshot stalls the render thread and stretches a movie. Time
    playback in runs without screenshots.

## M5 progress

- **Face culling was inverted.** The backend keeps NVN's lower-left origin in
  Vulkan's y-down framebuffer (the same rows in memory), and that flips the
  sign of a triangle's area: NVN's counter-clockwise is Vulkan's clockwise.
  Closed models still looked plausible from their back faces, but single-sided
  walls and floors vanished. That was the black bedroom, and the white
  silhouettes in the professor's intro (an M3 carry-over).
- **Input history:** the game also reads controllers through
  `nn::hid::GetNpadStates`, which the runtime hadn't overridden. Menus see no
  input without it. Npad states now come from a history of samples (one per
  4 ms, newest first), shared with `GetNpadState`.
- **Checked headless**, driven through `LETSGO_CONTROL`:
  - the bedroom, the stairs, the living room, Pallet Town
  - Oak's Pidgey and Pikachu event, the catch tutorial and its Poké Ball effects
  - Oak's lab: choosing Pikachu, the nickname prompt, the rival getting Eevee
  - the X menu, saving (`save/savedata.bin`), and continuing from the title
    screen
- **From your testing:**
  - The picture was too dark: the game's frames are sRGB, and the copy into a
    plain UNORM swapchain decoded them without re-encoding. The swapchain now
    matches the frames' encoding.
  - The keyboard follows the DS emulators (DeSmuME, melonDS): arrows move, X/Z/S/A
    are A/B/X/Y.
  - The save screen showed UTC. The game turns local time into a "GMT" posix
    time with `ToPosixTime(calendar, rule for "GMT")`, then shows it as UTC, so
    `LoadTimeZoneRule` now keeps the zone and `ToPosixTime` honors it.
  - Play-testing through the first rival battle found no further problems.
- **Test aids:** `LETSGO_TURBO` (fast-forward), `LETSGO_CONTROL` (drive a
  running game from a file), `--press` holds and stick directions,
  time-triggered frame dumps (`LETSGO_DUMP_FRAME=t70`) with per-draw state and
  data, `LETSGO_DEBUG_FS`. A save in the bedroom (`build/test_saves/`) skips the
  intro.
- **Open:**
  - There's no gyro on PC, so the sensors report at rest; Poké Ball aiming
    hasn't been tried beyond the tutorial throw.
  - The M3 carry-overs (compute dispatch, 3D textures, counters, aliasing,
    ASTC) remain.
