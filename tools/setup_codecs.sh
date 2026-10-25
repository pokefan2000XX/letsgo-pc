#!/usr/bin/env bash
# M4 codecs, built with llvm-mingw as static libraries into toolchain/codecs:
#   opus 1.5.2         the game's sound (Wwise streams are the Switch's hardware Opus)
#   libogg + libvorbis the title movies' audio (Vorbis)
#   libvpx 1.17.0      the title movies' picture (VP9 decoder only), with nasm for its x86 code
# Called by tools/setup_toolchain.sh, which sets $CMAKE and $NINJA; safe to rerun (finished parts are skipped).
#
#   bash tools/setup_codecs.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p toolchain/downloads toolchain/src
DL=toolchain/downloads
SRC=toolchain/src
OUT="$(cygpath -m "$PWD")/toolchain/codecs"
CMAKE=${CMAKE:-cmake}
NINJA=${NINJA:-$(cygpath -m "$(command -v ninja)")}
export PATH="$PWD/toolchain/llvm-mingw/bin:$PWD/toolchain/nasm:$PATH"

fetch() {  # url file sha256
  if [ ! -f "$DL/$2" ]; then curl -sSL -o "$DL/$2" "$1"; fi
  echo "$3 *$DL/$2" | sha256sum -c -
}

# Xiph's digests come from the SHA256SUMS files next to the releases. GitHub publishes none for
# tag archives and nasm.us none for this zip: those are pinned from the first download.
fetch https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz opus-1.5.2.tar.gz \
      65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
fetch https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz libogg-1.3.6.tar.gz \
      83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638
fetch https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz libvorbis-1.3.7.tar.gz \
      0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
fetch https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz libvpx-1.17.0.tar.gz \
      1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a
fetch https://www.nasm.us/pub/nasm/releasebuilds/3.02/win64/nasm-3.02-win64.zip nasm-3.02-win64.zip \
      161d0bfaff53c2f9e9f3e69fd0672323ebabafd1268976a5cec11be92a19aee7

for t in opus-1.5.2 libogg-1.3.6 libvorbis-1.3.7 libvpx-1.17.0; do
  [ -d "$SRC/$t" ] || tar -xzf "$DL/$t.tar.gz" -C "$SRC"
done
[ -d toolchain/nasm ] || { unzip -q "$DL/nasm-3.02-win64.zip" -d toolchain && mv toolchain/nasm-3.02 toolchain/nasm; }

cm() {  # name source-dir extra-cmake-args...
  local name=$1 src=$2
  shift 2
  echo "building $name"
  "$CMAKE" -S "$src" -B "build/$name" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$PWD")/cmake/host-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_INSTALL_PREFIX="$OUT" -DCMAKE_PREFIX_PATH="$OUT" "$@" > "build/$name.log" 2>&1 \
    && "$CMAKE" --build "build/$name" >> "build/$name.log" 2>&1 && "$CMAKE" --install "build/$name" >> "build/$name.log" 2>&1 \
    || { echo "building $name failed: see build/$name.log" >&2; return 1; }
}
mkdir -p build
[ -f toolchain/codecs/lib/libogg.a ] || cm libogg "$SRC/libogg-1.3.6" -DINSTALL_DOCS=OFF
[ -f toolchain/codecs/lib/libvorbis.a ] || cm libvorbis "$SRC/libvorbis-1.3.7"
[ -f toolchain/codecs/lib/libopus.a ] || cm opus "$SRC/opus-1.5.2" -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF

# libvpx: its own configure + make (GNU make ships with llvm-mingw as mingw32-make). Its makefiles
# can't take a path with a space, so make calls itself by name (MAKE=, found on PATH), the source
# is reached by a relative path, and the library and the decoder's headers are copied in by hand
# instead of make install.
if [ ! -f toolchain/codecs/lib/libvpx.a ]; then
  echo "building libvpx"
  rm -rf build/libvpx && mkdir -p build/libvpx
  (
    cd build/libvpx
    CC=x86_64-w64-mingw32-clang CXX=x86_64-w64-mingw32-clang++ LD=x86_64-w64-mingw32-clang AR=llvm-ar \
    NM=llvm-nm STRIP=llvm-strip AS=nasm \
      ../../toolchain/src/libvpx-1.17.0/configure --target=x86_64-win64-gcc --as=nasm \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-bins \
        --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder --enable-static --disable-shared \
        --enable-runtime-cpu-detect > ../libvpx.log
    mingw32-make MAKE=mingw32-make -j"$(nproc)" >> ../libvpx.log 2>&1
  ) || { echo "building libvpx failed: see build/libvpx.log" >&2; exit 1; }
  mkdir -p toolchain/codecs/include/vpx toolchain/codecs/lib
  for h in vp8.h vp8dx.h vpx_codec.h vpx_decoder.h vpx_frame_buffer.h vpx_image.h vpx_integer.h; do
    cp "$SRC/libvpx-1.17.0/vpx/$h" toolchain/codecs/include/vpx/
  done
  cp build/libvpx/libvpx.a toolchain/codecs/lib/
fi
ls toolchain/codecs/lib
echo "codecs ready"
