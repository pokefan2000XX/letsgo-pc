#!/usr/bin/env bash
# Run every smoke test. Needs orig/ (tools/prepare.sh) and toolchain/ (tools/setup_toolchain.sh).
#   bash tests/run_smoke.sh
set -uo pipefail
cd "$(dirname "$0")/.."
export PATH="$PWD/toolchain/llvm-mingw/bin:$PATH"
NINJA=${NINJA:-"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"}
mkdir -p build/smoke
fail=0
run() {  # name, command...
  local name=$1; shift
  if out=$("$@" 2>&1); then echo "PASS  $name"; else echo "FAIL  $name"; echo "$out" | tail -15; fail=1; fi
}

run "memory model: main at 0x7100000000, all relocations" bash -c '
  clang++ -O2 -std=c++20 -static tests/smoke/load_main.cpp -o build/smoke/load_main.exe &&
  build/smoke/load_main.exe orig/elf/main.elf data/main_functions.csv | grep -q "^PASS"'

run "decomp link: ARM64 patch module calls nnMain by address" bash -c '
  clang++ --target=aarch64-none-elf -O2 -std=c++20 -fno-exceptions -fno-rtti -ffreestanding -nostdlib -nostdinc++ \
          -Wno-invalid-offsetof -fno-pic -mno-outline-atomics -c tests/smoke/patch_module.cpp -o build/smoke/patch_module.o &&
  ld.lld -T tests/smoke/patch_module.ld build/smoke/patch_module.o -o build/smoke/patch_module.elf &&
  llvm-objdump -d build/smoke/patch_module.elf | grep -q "bl.*0x7100004110 <nnMain>"'

run "NVN layouts identical on host and guest" bash -c 'python -P tests/smoke/nvn_layout.py | grep -q "^PASS"'

run "guest headers: musl + libc++ + nnsdk/nnware for AArch64" bash -c '
  rm -rf build/guest_headers &&
  cmake -S tests/smoke/guest_headers -B build/guest_headers -G Ninja -DCMAKE_MAKE_PROGRAM="'"$NINJA"'" \
        -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$PWD")/cmake/guest-aarch64.cmake" > /dev/null &&
  cmake --build build/guest_headers > /dev/null'

run "emulator oracle: original FNV-1a function matches reference" bash -c 'python -P tests/smoke/emu_fnv.py | grep -q "^PASS"'

run "scripts: tools/amx.py assembles and disassembles made-up scripts" bash -c 'python -P tests/smoke/amx_tool.py | grep -q "^PASS"'

run "messages: tools/msg.py builds and dumps made-up message files" bash -c 'python -P tests/smoke/msg_tool.py | grep -q "^PASS"'

run "flatbuffers: tools/flatbuf.py reads, builds and grows made-up tables" bash -c 'python -P tests/smoke/flatbuf_tool.py | grep -q "^PASS"'

run "archives: tools/sarc.py writes, reads and changes a made-up SARC" bash -c 'python -P tests/smoke/sarc_tool.py | grep -q "^PASS"'

run "layouts: tools/bflyt.py reads and changes a made-up layout and animation" bash -c 'python -P tests/smoke/bflyt_tool.py | grep -q "^PASS"'

run "fonts: tools/font.py encodes and decodes made-up fonts" bash -c 'python -P tests/smoke/font_tool.py | grep -q "^PASS"'

run "battle sequences: tools/bseq.py reads, dumps and builds a made-up sequence" bash -c 'python -P tests/smoke/bseq_tool.py | grep -q "^PASS"'

# M1: the recompiled game boots headless and presents frames (needs a built runtime;
# LETSGO_BUILD picks the build directory, default build/pc). Tests that run the game start from an
# empty save folder of their own (first boot), so your save in save/ is never touched.
run "headless boot: game presents frames at 30 fps" bash -c '
  exe="${LETSGO_BUILD:-build/pc}/letsgo.exe"; test -x "$exe" || { echo "no runtime at $exe (build it first)"; exit 1; }
  rm -rf build/smoke/save
  LETSGO_SAVE_DIR=build/smoke/save timeout 20 "$exe" --headless --orig orig > build/smoke/boot.log 2>&1
  grep -q "frame 1 presented" build/smoke/boot.log && grep -q "frame 300 presented" build/smoke/boot.log &&
  ! grep -q "FATAL" build/smoke/boot.log'

# M3: every NVN program the game ships translates to GLSL and compiles to SPIR-V: the
# NV_gpu_program5 corpus (tools/shader_census.py --dump-ctrl orig/shaders_ctrl) and the
# programs without assembly (LETSGO_DUMP_SHADERS dumps, deduplicated into orig/shaders_noasm_u)
run "shader translation: assembly and GPU-code programs compile" bash -c '
  exe="${LETSGO_BUILD:-build/pc}/shadertest.exe"; test -x "$exe" || { echo "no shadertest at $exe (build it first)"; exit 1; }
  for d in orig/shaders_ctrl orig/shaders_noasm_u; do
    test -d "$d" || { echo "missing $d"; exit 1; }
    "$exe" "$d" --show 3 | tail -1 | grep -q " 0 translation failures, 0 compile failures" || { "$exe" "$d" --show 3; exit 1; }
  done'

# M4: the title movie decodes (VP9 picture, Vorbis sound) and the game's own sound (Wwise's Opus)
# is audible. The two A presses get past the first-boot controller screen; LETSGO_AUDIO_WAV
# records each AudioOut stream (audio.wav the game's, audio-2.wav the movie's).
run "movie and sound: the title movie decodes, game and movie audio are audible" bash -c '
  exe="${LETSGO_BUILD:-build/pc}/letsgo.exe"; test -x "$exe" || { echo "no runtime at $exe (build it first)"; exit 1; }
  rm -f build/smoke/audio*.wav && rm -rf build/smoke/save
  LETSGO_SAVE_DIR=build/smoke/save LETSGO_AUDIO_WAV=build/smoke/audio.wav timeout 55 "$exe" --headless --orig orig --press A@f150,A@f270 > build/smoke/movie.log 2>&1
  grep -E "video decoder done: .*, ([6-9][0-9][0-9]) fetched by the game" build/smoke/movie.log &&
  python -P tests/smoke/audible.py build/smoke/audio.wav 2 && python -P tests/smoke/audible.py build/smoke/audio-2.wav 15 &&
  ! grep -q "FATAL" build/smoke/movie.log'

# Decomp: decomp/ builds into a patch module, and every decompiled function matches its original
# in the emulator (decomp/tools/difftest.py runs both on the same inputs)
run "decomp: decompiled functions behave like the originals" bash -c '
  python -P decomp/tools/build.py > build/smoke/decomp.log 2>&1 || { cat build/smoke/decomp.log; exit 1; }
  python -P decomp/tools/difftest.py > build/smoke/difftest.log 2>&1
  tail -1 build/smoke/difftest.log | grep -q " 0 failed" || { cat build/smoke/difftest.log; exit 1; }'

# Roadmap phase 1: mods replace and add files inside gfpak archives, a made-up one and three of the
# game's, which must hold exactly what they should afterwards (needs modstest.exe in the build)
run "mods folder: files replace and add to gfpak archives" bash -c '
  python -P tools/check_mods.py > build/smoke/mods.log 2>&1 || { tail -5 build/smoke/mods.log; exit 1; }
  grep -q "made-up archive: ok" build/smoke/mods.log'

exit $fail
