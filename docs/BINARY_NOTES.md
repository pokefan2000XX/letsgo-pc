# What's in the binary (v1.0.2)

Regenerate the numbers with `bash tools/prepare.sh`. Raw data:
`data/main_census.json`, `data/main_functions.csv`, `data/main_imports.csv`,
`data/main_vtables.csv`, `data/main_reachability.json`, `data/main_npdm.json`,
`data/sdk_*` and `orig/elf/*.json`.

## Build

| | |
|---|---|
| Title | Pokémon: Let's Go, Pikachu! `010003F003A34000`, v1.0.2 (update v131072) |
| main build ID | `c208db6a4ef4361f28b8402be0f7cd9a` |
| Codename / SDK | "beluga", NintendoSDK 5.4.151 |
| Analysis base | main at `0x7100000000` (all addresses in `data/` use it) |

## Modules

| Module | Code | Symbols | Role in the port |
|---|---|---|---|
| main | 13.84 MB | 32 exported functions, 842 imports | Recompiled; the decomp target |
| sdk (nnSdk) | 5.2 MB | 22,986 exports, fully named | Replaced by the runtime; reference for SDK behavior |
| subsdk0 (glslc) | 8.2 MB | 17 exports | Replaced (glslang) |
| subsdk1 (multimedia/movie) | 3.4 MB | 10,605 exports | Replaced (libvpx/libwebm) |
| rtld | 6.5 KB | | Replaced by our loader |

## main layout (module offsets)

| Range | Content |
|---|---|
| `0x0`–`0xd32ba8` | text. The module header is at 0; `.plt` is `0xd2fa58`–`0xd32ba8` (787 stubs) |
| `0xd33000`–`0x15261c4` | rodata: `.rela.dyn`, `.rela.plt`, `.dynsym`/`.dynstr`, `.eh_frame_hdr`, `.eh_frame` |
| `0x1527000`–`0x160c030` | data: `.dynamic`, `.got.plt`, `.got`, `.init_array` (400 constructors), `.fini_array` |
| `0x160c030`–`0x1ee1000` | bss (9.3 MB) |

- **Relocations:** 71,687 RELATIVE (60,009 point into code: vtables and
  callbacks), 2,316 ABS64, 45 GLOB_DAT, 787 JUMP_SLOT.
- **Exports:**
  - `nninitStartup` `0x7100003fb0`, the allocator setup that runs before static constructors
  - `nnMain` `0x7100004110`
  - the game's own `malloc`/`free`/`calloc`/`realloc`/`aligned_alloc`/`operator new`/`delete`
    at `0x7100003b20`–`0x7100003f40`. Host code that allocates memory the game will
    free must call these.
  - a few libc++ instantiations
- **Thread-local storage:** one slot at TPIDR_EL0+0x10, read by 5 instructions
  in one locking routine.
- **Floating-point control:** main never writes FPCR/FPSR and imports no `fenv`
  functions. The game runs with the default FP mode (round-to-nearest, no flush-to-zero).
- **Asset hashing:** Game Freak's FNV-1a 64 uses offset basis
  `0xCBF29CE484222645` (not the standard `…2325`), prime `0x100000001B3`. The
  basis is built inline in 800 places. `0x7100101b30` is a small leaf that
  compares two strings by this hash (it's the emulator smoke test).

## Cross-references (main)

| | |
|---|---|
| Call-graph edges | 178,658 direct calls, 15,605 tail calls, 7,098 address-taken references, 16,480 vtable uses |
| vtables | 6,328 (54,771 slots); 3,294 are referenced directly from code. The rest are presumably reached through relocated data such as static objects (not yet checked) |
| Other function-pointer tables | 3,528 |
| Reachable from boot (lower bound) | 43,208 of 59,324 functions and 809 of 842 imports, from `nninitStartup`, the 400 static constructors and `nnMain` |

Reachability follows direct calls, tail calls, taken addresses and the
vtables that reachable code stores. The 33 imports never reached are mostly
iostream internals, RTTI vtables and `__cxa_pure_virtual`, which appears only
as a vtable slot.

## sdk (nnSdk), for runtime reference

- **Size:** 35,386 functions (24,494 from `.eh_frame`, 10,810 PLT stubs: the sdk
  calls its own exports through the PLT). 18,618 have names (`data/sdk_functions.csv`, base `0x7180000000`).
- **System registers:** TPIDRRO_EL0 (54 reads, the thread-local region), CNTPCT_EL0
  (34, the system counter), FPCR/FPSR (only in the `fenv` functions).
- **In Ghidra:** analyzed alongside main in `orig/ghidra/letsgo`, so the real
  implementation of any nn:: function the runtime replaces can be read there.

## Instruction census (main)

- **Size:** 3,356,335 instructions in 60,111 functions (59,323 from `.eh_frame`,
  787 PLT stubs, 1 found by call target). 190 distinct mnemonics.
- **Control flow:** 178,658 BL, 109,832 BLR, 52,591 RET. There are 5,137 BR:
  1,930 jump tables and 3,207 computed jumps or tail calls.
- **SIMD/FP:** 50,288 SIMD instructions and 94,072 scalar FP instructions.
  SIMD is mostly ld1/st1/st2, tbl, zip/uzp/trn, dup/umov, bit ops and compares.
- **Atomics:** ldarb 7,270, ldar 2,513, ldaxr/stlxr 1,611, ldxr/stxr 891,
  stlr 214, clrex 1,968, dmb 9, yield 2. No LSE.
- **Not present:** SVCs, crypto, CRC. The only system register is TPIDR_EL0 (5 reads).
- **Traps:** 432 `0xe7ffdefe` words, mostly at the end of noreturn paths or as
  4-byte trap-only functions.
- **Exceptions:** 1 function has an LSDA, and it only calls `std::terminate`.

## Imports (main → sdk/glslc/multimedia)

- **Counts:** 787 functions and 55 data symbols, from `nnSdk.nss`, `glslc.nss` and `multimedia.nss`.
- **By area:**

  | Area | Functions | | Area | Functions |
  |---|---|---|---|---|
  | libc | ~155 | | nn::socket (+C API) | 40 + ~25 |
  | nn::os (+C API) | 86 + ~20 | | nn::bluetooth | 34 |
  | nn::hid | 82 | | nn::util | 34 |
  | libc++ | 45 | | movie | 28 |
  | nn::ldn | 26 | | nn::bcat | 24 |
  | nn::fontll | 24 | | nn::audio | 22 |
  | nn::fs | 19 | | nn::time / account / crypto | 17 each |
  | nn::oe / mem | 16 each | | nn::err | 14 |
  | nn::swkbd / nifm | 10 each | | nn::vi / prepo | 8 each |
  | glslc | 8 | | nvn | 1 (`nvnBootstrapLoader`) |

- **Most-called imports:** `__cxa_guard_acquire`/`release` (7,262 each),
  `nn::os::GetTlsValue` (4,492), `abort` (2,767), `memset` (2,392),
  `memcpy` (2,198), `nn::os::UnlockMutex`/`LockMutex` (1,080/972), `strlen` (835).

## Process (main.npdm)

- **Main thread:** priority 44, core 0, 1 MB stack. Cores 0–2, thread priorities 28–59, 39-bit address space.
- **Services (46):**
  - system: acc:u0 aoc:u apm appletOE set time:u pctl pl:u lm ldr:ro csrng
  - audio: audin:u audout:u audren:u hwopus
  - network: bcat:u bsd:u bsdcfg friend:u nifm:u nim:eca nsd:u ntc prepo:u sfdnsres ssl ldn:u
  - Bluetooth: bt btm:u
  - files: fsp-srv
  - input: hid irs nfc:mf:u nfc:user nfp:user
  - graphics: nvdrv vi:u
  - other: caps:su caps:u htc htc:tenv htcs mii:u mm:u pcm banana
- Only the imports matter for the port: main never talks to services directly.

## RomFS (orig/romfs)

- **Size:** 27,818 files, 4.43 GB. The update replaces 6.2 MB.
- **Archives:** `.gfpak` (GFLXPACK, LZ4 only), `.arc` (SARC: UI layouts).
- **Models and data:** `.gfb*` FlatBuffers (models, animations, cameras,
  collision), `.prmb`/`.bin`/`.tbl` tables.
- **Textures and effects:** `.bntx` (BC1/BC3/BC7/RGBA8 in samples, no ASTC),
  `.ptcl` (VFXB particles, 1.4 GB).
- **Shaders:** `.bnsh*` (Maxwell binaries plus NV assembly text).
- **Audio:** `.wem`/`.bnk` (Wwise, bank version 128 = 2017.2).
- **Video and fonts:** `.webm` movies, `.BFOTF`/`.bffnt` fonts.
- **Scripts:** 705 Pawn AMX field scripts (`bin/archive/script_amx.gfpak`) and
  7 battle AI scripts. They use 64-bit cells and compact encoding, and natives
  are referenced by 32-bit hash (277 distinct).

## Strings

15,001 string references in 4,391 functions. 260 source paths attribute 1,946
functions:

| Library | Functions |
|---|---|
| gflnet3 | 647 |
| SiCore | 570 |
| NEX OnlineCore | 296 |
| YEBIS | 253 |
| gflib3 | 150 |
| SiGfx | ~90 |
| game `prog/` | 31 |

The game's own code kept few asserts, so naming it relies on call graphs and
data. The tables are in `orig/analysis/`.

## NVN shader binaries

Each program stage is handed to `nvnProgramSetShaders` as a control section and a
data section ("code"). Offsets that matter (control section, little-endian u32):

| Offset | Meaning |
|---|---|
| 0x00 | magic 0x98761234 |
| 0x14, 0x18 | NV_gpu_program5 assembly text: offset, size (0 when glslc didn't embed it) |
| 0x20 | `program.local` constants used by the assembly |
| 0x6F0 | control section size |
| 0x6F8 | code size |
| 0x6FC, 0x700 | the compiler's constants (hardware constant buffer 1): size, offset in the data section |
| 0x704 | data section size |
| 0x708 | shader program header (SPH, 0x50 bytes) offset in the data section, 0x30 |
| 0x70C | registers used |

The data section is a 0x30-byte header (magic 0x12345678), the SPH, then Maxwell
instructions in groups of four 64-bit words (a scheduling word, then three
instructions), padded with a branch to itself, then the constants.

- 1346 of the game's programs embed assembly. NintendoWare's ui2d and font
  built-ins (in main), `system_resource/shader/movie.bnsh` and YEBIS's own
  shaders don't. They're translated from the machine code (99 stages seen so far).
- Hardware constant buffer slots: 0 driver data (storage buffer N's address at
  0x110 + 16N), 1 the compiler's constants, 2 bound texture handles (binding N at
  byte 0x20 + 8N; TEXS/TEX `TidB` = 8 + 2N), 3 + N uniform buffer N.
- Movies: only `bin/movies/PIKACHU_GO/title_male.webm` and `title_female.webm`
  (VP9 1920x1080 + Vorbis 48 kHz stereo, 23.03 s, unlaced SimpleBlocks).

## Movie player (main 0x7100665000-0x710066f000)

A port of the SDK's movie player sample, driving subsdk1's `movie::` API (in
the style of Android's MediaExtractor and MediaCodec).

- **Event thread:** one thread waits on every decoder's four events: input
  available, output available, format changed, error. It moves samples from
  the extractor into the decoders, then hands outputs to a video renderer and
  an audio renderer. Status 0 means OK and 0x2000 the end of the stream; buffer
  flag 4 marks the end of the stream.
- **Video renderer**, for each picture:
  - `GetOutputBufferFormat` (`width`, `height`, `nv12-y-stride`,
    `nv12-y-offset`, `nv12-uv-offset`)
  - `GetOutputFormat` (`nv12-colorspace`)
  - `GetOutputBuffer` into the NV12 texture's memory (capacity
    width × height × 3/2)
  - `ReleaseOutputBufferIndex`

  The media clock is system time anchored to a media time. A picture more than
  40 ms late counts as dropped.
- **Audio renderer:** AudioOut at 48 kHz with the track's channel count (mono
  is upmixed to stereo). It uses 16 buffers of 0x2000 bytes (0x8000 above two
  channels); in its accumulating mode it fills each to 0x1000 bytes (0x3000).
  The decoder output buffer it passes is the same size as one of those buffers.

## Boot and title (found for hacks/quick-boot)

- **Boot:** `0x710000e420` runs every frame, its step at +0x68. Step 0 makes the
  controller procedure (constructor `0x710004e2d0(this, mode)`) in mode 1, always
  ask; mode 0 asks only when the connected controllers need it. Step 6 makes the
  title procedure (logo, then the title demo) and step 7 waits for it.
- **Procedures:** `0x710013a780` updates a manager's procedures every frame
  (`0x710013a160` runs one: vtable +0x38 init, +0x48 update, +0x50 end).
- **Layouts:** a layout's phase at +0x80 (1 opening, 2 active, 3 closing). The
  opening `0x7100126cf0` waits (+0x84 = 1) for the layout's opening animations;
  the active phase calls vtable +0xa0 every frame. The GAME FREAK logo (class
  `0x710152eb50`, `bin/app/title_pbgf`) is all opening; its +0xa0
  (`0x7100073d80`) ends it with scene event 0 (+0x260: id +0xc).
- **Layout animations:** a player keeps 0x40-byte slots at +0x258 (count +0x278):
  start `0x7100663dd0(player, index, flag)`, last frame
  `0x7100664170(player, index)`, set frame `0x7100663e90(frame, player, index)`.
- **Demos:** the loader `0x71001ddb70` (demo +0x220 is its core, the core's
  sequence at +0x98) runs every command once, which makes a track per command
  (`0x7100210880(seq, -1, 1)`). The core's clock (+0x680, from -1) goes up 2 a
  frame (`0x71001f1d20`); each track (`0x71001f5550`) catches up through every
  frame it hasn't had. The seek `0x71001efdd0` sets the clock and +0x692, which
  makes tracks jump without starting or ending what's on the way. Demo commands'
  parameter sizes: `0x7100f4c810` (the battle's are at `0x7100f97e30`).
- **Title demo** (`bin/demo/sequence/sd9010_title.bseq`, 3480 frames): the
  opening to frame 564, then "Press the A Button" (layout `anime_btn_loop`, and
  commands 66/67 at 564-3399). The logo's `anime_in` starts at frame 25 and
  runs on the layout's clock (270 frames). The title's music
  (`Play_BGM_opening`) starts at 250.

## Battle camera (found 2026-10-23)

- **The default shot:** `0x7100a7e290(battle, eye, target)` gives the camera a
  battle settles on once the player's Pokémon is out (the send-out sequence
  `ee420` ends by moving there with command 38) and comes back to: an eye and a
  target from a table kept by the two Pokémon's size classes (the Pokémon
  config's field 7, `PokemonConfig.SizeClass`; 4 x 4 for single battles,
  another for double), the same for every Pokémon of a class. The battle's
  other cameras start from the Pokémon's points and grow with its camera scale
  (1 + (field 9 / 100 - 1) / 2), so a Pokémon given bigger sizes is framed by
  them but fills this one; a hook can move the eye and target out about the
  Pokémon after the original runs.
- **A battle Pokémon's view** (set up by `0x7100a9aa50`; the player's is the
  battle's view slot 0, `battle+0x448`): species +0x1c4 and form +0x1c8, as the
  battle's poke_data looks them up; its place is +0xb0 plus +0xf0
  (`0x7100a96d90` adds the two vectors).
