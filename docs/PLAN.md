# Plan: Let's Go Pikachu on PC

Goal: one Windows executable plus asset folders, built on your own machine from
your own dump, with no emulator. It starts as a **static recompilation** of the
Switch code and grows into a **full source decomp** of the game's own code, one
function at a time, so it stays playable and moddable the whole way.

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
- **Order:** the decomp roadmap below. Before changing a struct's layout,
  every function that touches it must be decompiled.
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

## Decomp roadmap

Phases run in order, and each is finished before the next starts. A phase is
finished when every function in it is replaced and checked against its
original (`decomp/tools/difftest.py`), and the game boots and loads a save
with all of them active.

| | Phase | Covers |
|---|---|---|
| D1 | Pokémon data (started 2026-10-15) | The 0x104-byte record and everything that reads or changes it, plus the party (main `0x710071c000`–`0x710073a400`, 619 functions) |
| D2 | Saves | `savedata.bin` and its blocks: bag, player and money, event flags and work values, Pokédex, box storage, GO Park, daycare, play time |
| D3 | Script commands | The 277 Pawn natives (their names are in the binary) |
| D4 | Game data tables | Loaders for base stats, moves, items, learnsets, evolutions, encounters, trainers |
| D5 | Battle | Turn order, damage, moves, status, trainer AI |
| D6 | Field and catching | Wild spawns, the catch formula, GO Park, daycare, trades, the partner Pokémon |
| D7 | Menus and screens | Party list, summary, bag, boxes, evolution |
| D8 | Game assets | The formats of the romfs files, extracted to editable files and rebuilt |

- **D1 steps:**
  1. The record: encryption, checksum, block order, and the object that
     decrypts and re-encrypts it around each access.
  2. The field accessors (about 200): species, form, level and EXP, moves and
     PP, IVs, AVs, nature, gender, shininess, nickname, original trainer,
     friendship, CP, height and weight.
  3. The Pokémon object: creating and copying, level up, healing, stats, the
     base-stat lookup.
  4. The party: six slots, add, remove, count, heal all.
- **D8 steps:**
  1. Archives and tables: gfpak, SARC, and the FlatBuffers and binary tables
     to JSON and back.
  2. Text and scripts: messages to text, and the 705 Pawn field scripts to
     readable source, both ways.
  3. Maps: terrain and collision, object and NPC placement, warps, spawn areas.
  4. Models, textures and animations to glTF and PNG/DDS, and back.
  5. Effects, UI layouts and sound banks, as far as their formats allow.

  The M6 mod loader reads rebuilt files from a folder that overrides romfs,
  so nothing gets repacked. Files extracted from a dump stay out of git like
  `orig/`: the repo keeps only the tools and the format descriptions.
- **Stays recompiled** unless a feature needs it (decision 12): Wwise,
  NintendoWare, YEBIS, NEX and gflib3's internals.
- **Later:** once the game's own code is decompiled, it can be compiled for
  x86-64 directly instead of going through the recompiler.

## Milestones

| | Milestone | Done when | New dependencies |
|---|---|---|---|
| M0 | Prep | Extraction, ELFs, function/import/vtable DBs, census, Ghidra project, guest toolchain, emulator oracle, smoke tests | llvm-mingw, Ghidra, musl, nnsdk/nnware, Unicorn (done) |
| M1 | Recompiler core + headless boot (**done** 2026-10-14) | All of main translated. The runtime loads it, runs static constructors and `nnMain` on a guest thread, and the game reaches its first frame present with a logging-only NVN | none |
| M2 | Window + input (**done** 2026-10-15) | SDL3 window, hid mapped to gamepad/keyboard as Handheld | SDL3 |
| M3 | Graphics (**done** 2026-10-15) | NVN on Vulkan, shader converter, texture deswizzle; title screen renders | Vulkan headers, glslang |
| M4 | Audio + movies (**done** 2026-10-15) | nn::audio output (Wwise itself runs inside main), WebM, software keyboard | opus, libogg/libvorbis, libvpx (nasm to build it) |
| M5 | Playable (started 2026-10-15) | Field, battle, menus and saves match Ryujinx | none |
| M6 | PC features | Resolution, frame rate, mod loader | none |

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
