#!/usr/bin/env bash
# Recreate toolchain/ from pinned, hash-checked sources. Everything lands in
# toolchain/ (gitignored); nothing is installed system-wide except the Python
# packages (pip --user). Needs Git for Windows (this shell, curl, unzip, tar) and
# Python 3.11 or newer. build-dev.bat runs it when toolchain/ isn't ready.
#
#   bash tools/setup_toolchain.sh            what the build needs
#   bash tools/setup_toolchain.sh --ghidra   and Ghidra (README, Setup step 3), which needs a JDK 21
#
# CMake and Ninja: $CMAKE and $NINJA when set (build-dev.bat passes the ones it found), else the
# ones on PATH, else pinned copies downloaded into toolchain/cmake and toolchain/ninja.
set -euo pipefail
cd "$(dirname "$0")/.."
GHIDRA=
for a in "$@"; do
  case "$a" in
    --ghidra) GHIDRA=1 ;;
    *) echo "usage: bash tools/setup_toolchain.sh [--ghidra]" >&2; exit 2 ;;
  esac
done
mkdir -p toolchain/downloads toolchain/src build
DL=toolchain/downloads

fetch() {  # url file sha256
  if [ ! -f "$DL/$2" ]; then
    echo "downloading $2"
    curl -fsSL -o "$DL/$2.part" "$1"
    mv "$DL/$2.part" "$DL/$2"
  fi
  echo "$3 *$DL/$2" | sha256sum -c --quiet -
}

# CMake 4.4.2 and Ninja 1.13.2 (the digests GitHub publishes for the release assets), when none are given or on PATH
CMAKE=${CMAKE:-$(command -v cmake || true)}
if [ -z "$CMAKE" ]; then
  fetch https://github.com/Kitware/CMake/releases/download/v4.4.2/cmake-4.4.2-windows-x86_64.zip \
        cmake-4.4.2-windows-x86_64.zip e8139d85b3813bc38833142ae1940472e9a587e9b5d2718ac1804c60f4e57a64
  [ -d toolchain/cmake ] || { unzip -q "$DL/cmake-4.4.2-windows-x86_64.zip" -d toolchain && mv toolchain/cmake-4.4.2-windows-x86_64 toolchain/cmake; }
  CMAKE=toolchain/cmake/bin/cmake.exe
fi
NINJA=${NINJA:-$(command -v ninja || true)}
if [ -z "$NINJA" ]; then
  fetch https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip \
        ninja-win-1.13.2.zip 07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65
  [ -f toolchain/ninja/ninja.exe ] || unzip -q "$DL/ninja-win-1.13.2.zip" -d toolchain/ninja
  NINJA=toolchain/ninja/ninja.exe
fi
# CMake runs with these as given; the build files name Ninja by its Windows path
CMAKE=$(cygpath -u "$CMAKE")
NINJA=$(cygpath -m "$(cygpath -a "$NINJA")")
export CMAKE NINJA

# llvm-mingw: clang/lld/libc++ (host build, Itanium ABI) + AArch64 for guest code
fetch https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip \
      llvm-mingw-20261006-ucrt-x86_64.zip 317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a
[ -d toolchain/llvm-mingw ] || { unzip -q "$DL/llvm-mingw-20261006-ucrt-x86_64.zip" -d toolchain && mv toolchain/llvm-mingw-20261006-ucrt-x86_64 toolchain/llvm-mingw; }

# Ghidra 12.1.4, on request: it runs on the JDK 21 in $JDK, else $JAVA_HOME, else Eclipse Adoptium's newest
if [ -n "$GHIDRA" ]; then
  fetch https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip \
        ghidra_12.1.4_PUBLIC_20260921.zip ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db
  [ -d toolchain/ghidra ] || { unzip -q "$DL/ghidra_12.1.4_PUBLIC_20260921.zip" -d toolchain && mv toolchain/ghidra_12.1.4_PUBLIC toolchain/ghidra; }
  JDK=${JDK:-${JAVA_HOME:-$(ls -d "C:/Program Files/Eclipse Adoptium/jdk-21"* 2>/dev/null | sort -V | tail -1 || true)}}
  if [ -n "$JDK" ]; then
    sed -i "s|^JAVA_HOME_OVERRIDE=.*|JAVA_HOME_OVERRIDE=$(cygpath -m "$JDK")|" toolchain/ghidra/support/launch.properties
  else
    echo "No JDK 21 found: install one (adoptium.net), or set JDK, and run this again for Ghidra." >&2
  fi
fi

# musl 1.2.6 headers for the guest sysroot (SHA-256 of the release tarball; its
# SHA-512 matches Alpine's aports record)
fetch https://musl.libc.org/releases/musl-1.2.6.tar.gz musl-1.2.6.tar.gz \
      d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
[ -d toolchain/src/musl-1.2.6 ] || tar -xzf "$DL/musl-1.2.6.tar.gz" -C toolchain/src
python -P tools/make_guest_sysroot.py --musl toolchain/src/musl-1.2.6 --out toolchain/guest-sysroot

# Configures and builds a CMake project with llvm-mingw, installed into a toolchain/ folder:
#   cm name source-dir install-dir extra-cmake-args...    (output in build/<name>.log)
cm() {
  local name=$1 src=$2 out
  out="$(cygpath -m "$PWD")/$3"
  shift 3
  echo "building $name"
  "$CMAKE" -S "$src" -B "build/$name" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$PWD")/cmake/host-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$out" "$@" > "build/$name.log" 2>&1 \
    && "$CMAKE" --build "build/$name" >> "build/$name.log" 2>&1 \
    && "$CMAKE" --install "build/$name" >> "build/$name.log" 2>&1 \
    || { echo "building $name failed: see build/$name.log" >&2; return 1; }
}

# SDL 3.4.18 (M2: window, keyboard, gamepads), built with llvm-mingw as a static library so the
# release is a single exe; the hash is the digest GitHub publishes for the release asset
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
      SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
[ -d toolchain/src/SDL3-3.4.18 ] || tar -xzf "$DL/SDL3-3.4.18.tar.gz" -C toolchain/src
[ -f toolchain/SDL3-static/lib/libSDL3.a ] || cm sdl3-static toolchain/src/SDL3-3.4.18 toolchain/SDL3-static \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF

# M3: Vulkan headers and glslang (GLSL -> SPIR-V), built with llvm-mingw as static libraries.
# GitHub publishes no digests for tag archives; these hashes are pinned from the first download.
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz \
      Vulkan-Headers-vulkan-sdk-1.4.363.0.tar.gz 4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
[ -d toolchain/src/Vulkan-Headers-vulkan-sdk-1.4.363.0 ] || tar -xzf "$DL/Vulkan-Headers-vulkan-sdk-1.4.363.0.tar.gz" -C toolchain/src
fetch https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz \
      glslang-16.6.0.tar.gz 9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
[ -d toolchain/src/glslang-16.6.0 ] || tar -xzf "$DL/glslang-16.6.0.tar.gz" -C toolchain/src
[ -f toolchain/glslang/lib/libglslang.a ] || cm glslang toolchain/src/glslang-16.6.0 toolchain/glslang \
  -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=ON -DGLSLANG_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DGLSLANG_ENABLE_INSTALL=ON

# M4: opus, libogg + libvorbis and libvpx (with nasm), built static into toolchain/codecs
bash tools/setup_codecs.sh

# Nintendo SDK / NintendoWare / NVN declarations (no license: local use only)
clone() {  # repo dir commit
  [ -d "toolchain/$2" ] || git clone -q "https://github.com/$1.git" "toolchain/$2"
  git -C "toolchain/$2" -c advice.detachedHead=false checkout -q "$3"
}
clone open-ead/nnsdk nnsdk 3565f8c
clone open-ead/nnware nnware 8a9e2d2
clone open-ead/nnheaders nnheaders 246c3b4   # the guest code's SDK headers (cmake/guest-aarch64.cmake)

# Python packages: cryptography, lz4, capstone (extraction, analysis) and unicorn (emulator oracle)
python -m pip install --user --quiet --disable-pip-version-check --only-binary=:all: cryptography lz4 capstone
printf 'unicorn==2.1.4 --hash=sha256:d7107500c64ce5c168fbff6bef9485b5db1350050036f4cea568650cf8bdbdf5\n' > "$DL/unicorn-req.txt"
python -m pip install --user --quiet --disable-pip-version-check --require-hashes --only-binary=:all: -r "$DL/unicorn-req.txt"
echo "toolchain ready"
