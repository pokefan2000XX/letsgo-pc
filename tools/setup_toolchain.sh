#!/usr/bin/env bash
# Recreate toolchain/ from pinned, hash-checked sources. Everything lands in
# toolchain/ (gitignored); nothing is installed system-wide except the Python
# packages (pip --user). Needs Git, curl, unzip, tar, Python 3 and a JDK 21
# (for Ghidra; set JDK below).
#
#   bash tools/setup_toolchain.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p toolchain/downloads toolchain/src
DL=toolchain/downloads
JDK=${JDK:-"C:/Program Files/Eclipse Adoptium/jdk-21.0.12.101-hotspot"}

fetch() {  # url file sha256
  if [ ! -f "$DL/$2" ]; then curl -sSL -o "$DL/$2" "$1"; fi
  echo "$3 *$DL/$2" | sha256sum -c -
}

# llvm-mingw: clang/lld/libc++ (host build, Itanium ABI) + AArch64 for guest code
fetch https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip \
      llvm-mingw-20261006-ucrt-x86_64.zip 317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a
[ -d toolchain/llvm-mingw ] || { unzip -q "$DL/llvm-mingw-20261006-ucrt-x86_64.zip" -d toolchain && mv toolchain/llvm-mingw-20261006-ucrt-x86_64 toolchain/llvm-mingw; }

# Ghidra 12.1.4
fetch https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip \
      ghidra_12.1.4_PUBLIC_20260921.zip ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db
if [ ! -d toolchain/ghidra ]; then
  unzip -q "$DL/ghidra_12.1.4_PUBLIC_20260921.zip" -d toolchain && mv toolchain/ghidra_12.1.4_PUBLIC toolchain/ghidra
  sed -i "s|^JAVA_HOME_OVERRIDE=.*|JAVA_HOME_OVERRIDE=$JDK|" toolchain/ghidra/support/launch.properties
fi

# musl 1.2.6 headers for the guest sysroot (SHA-256 of the release tarball; its
# SHA-512 matches Alpine's aports record)
fetch https://musl.libc.org/releases/musl-1.2.6.tar.gz musl-1.2.6.tar.gz \
      d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
[ -d toolchain/src/musl-1.2.6 ] || tar -xzf "$DL/musl-1.2.6.tar.gz" -C toolchain/src
python -P tools/make_guest_sysroot.py --musl toolchain/src/musl-1.2.6 --out toolchain/guest-sysroot

# SDL 3.4.18 (M2: window, keyboard, gamepads), built with llvm-mingw as a static library so the
# release is a single exe; the hash is the digest GitHub publishes for the release asset
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
      SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
[ -d toolchain/src/SDL3-3.4.18 ] || tar -xzf "$DL/SDL3-3.4.18.tar.gz" -C toolchain/src
if [ ! -f toolchain/SDL3-static/lib/libSDL3.a ]; then
  NINJA=${NINJA:-"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"}
  mkdir -p build
  cmake -S toolchain/src/SDL3-3.4.18 -B build/sdl3-static -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$PWD")/cmake/host-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_INSTALL_DOCS=OFF -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$PWD")/toolchain/SDL3-static" > build/sdl3-configure.log
  cmake --build build/sdl3-static > build/sdl3-build.log && cmake --install build/sdl3-static > build/sdl3-install.log
fi

# M3: Vulkan headers and glslang (GLSL -> SPIR-V), built with llvm-mingw as static libraries.
# GitHub publishes no digests for tag archives; these hashes are pinned from the first download.
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz \
      Vulkan-Headers-vulkan-sdk-1.4.363.0.tar.gz 4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
[ -d toolchain/src/Vulkan-Headers-vulkan-sdk-1.4.363.0 ] || tar -xzf "$DL/Vulkan-Headers-vulkan-sdk-1.4.363.0.tar.gz" -C toolchain/src
fetch https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz \
      glslang-16.6.0.tar.gz 9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
[ -d toolchain/src/glslang-16.6.0 ] || tar -xzf "$DL/glslang-16.6.0.tar.gz" -C toolchain/src
if [ ! -f toolchain/glslang/lib/libglslang.a ]; then
  NINJA=${NINJA:-"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"}
  cmake -S toolchain/src/glslang-16.6.0 -B build/glslang -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$PWD")/cmake/host-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=ON -DGLSLANG_TESTS=OFF -DBUILD_SHARED_LIBS=OFF \
        -DGLSLANG_ENABLE_INSTALL=ON -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$PWD")/toolchain/glslang" > /dev/null
  cmake --build build/glslang > /dev/null && cmake --install build/glslang > /dev/null
fi

# M4: opus, libogg + libvorbis and libvpx (with nasm), built static into toolchain/codecs
bash tools/setup_codecs.sh

# Nintendo SDK / NintendoWare / NVN declarations (no license: local use only)
clone() {  # repo dir commit
  [ -d "toolchain/$2" ] || git clone -q "https://github.com/$1.git" "toolchain/$2"
  git -C "toolchain/$2" -c advice.detachedHead=false checkout -q "$3"
}
clone open-ead/nnsdk nnsdk 3565f8c
clone open-ead/nnware nnware 8a9e2d2
clone open-ead/nnheaders nnheaders 246c3b4   # superseded by nnsdk + nnware, kept for reference

# Python packages: cryptography, lz4, capstone (analysis) and unicorn (emulator oracle)
python -m pip install --user --only-binary=:all: cryptography lz4 capstone
printf 'unicorn==2.1.4 --hash=sha256:d7107500c64ce5c168fbff6bef9485b5db1350050036f4cea568650cf8bdbdf5\n' > "$DL/unicorn-req.txt"
python -m pip install --user --require-hashes --only-binary=:all: -r "$DL/unicorn-req.txt"
echo "toolchain ready"
