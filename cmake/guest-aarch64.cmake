# CMake toolchain for guest (Switch-side) code: decompiled C++ that is compiled
# for AArch64 with the same ABI as the original game, then linked into the
# patch module and recompiled together with main.
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/guest-aarch64.cmake ...
#
# Needs toolchain/llvm-mingw, toolchain/guest-sysroot (tools/make_guest_sysroot.py)
# and toolchain/nnheaders.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

get_filename_component(LETSGO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LETSGO_LLVM "${LETSGO_ROOT}/toolchain/llvm-mingw/bin")
set(LETSGO_SYSROOT "${LETSGO_ROOT}/toolchain/guest-sysroot")

set(CMAKE_C_COMPILER "${LETSGO_LLVM}/clang.exe")
set(CMAKE_CXX_COMPILER "${LETSGO_LLVM}/clang++.exe")
set(CMAKE_ASM_COMPILER "${LETSGO_LLVM}/clang.exe")
set(CMAKE_LINKER "${LETSGO_LLVM}/ld.lld.exe")
set(CMAKE_AR "${LETSGO_LLVM}/llvm-ar.exe")
set(CMAKE_RANLIB "${LETSGO_LLVM}/llvm-ranlib.exe")
# aarch64 Linux/musl gives the same LP64 + AArch64 C++ ABI as the Switch toolchain
# and lets libc++ and musl headers configure themselves normally.
set(CMAKE_C_COMPILER_TARGET aarch64-unknown-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-linux-musl)
set(CMAKE_ASM_COMPILER_TARGET aarch64-unknown-linux-musl)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Tegra X1 = Cortex-A57 (ARMv8.0: no LSE). Inline LL/SC atomics like the
# original (no outline-atomic helper calls). Keep unwind tables: the
# recompiler finds function boundaries in .eh_frame.
# The game was built with NintendoSDK and NintendoWare 5.4.151; the SDK headers
# pick struct layouts by version (e.g. nn::os::ThreadType is 0x1C0 before 6.0).
set(_letsgo_common "-mcpu=cortex-a57 -mno-outline-atomics -fno-pic -fno-plt -ffunction-sections -fdata-sections -funwind-tables -fno-stack-protector -nostdlibinc -DSWITCH -DNNSDK -DNN_SDK_MAJOR=5 -DNN_SDK_MINOR=4 -DNN_SDK_PATCH=151 -DNN_WARE_MAJOR=5 -DNN_WARE_MINOR=4 -DNN_WARE_PATCH=151")
# SDK declarations: open-ead/nnsdk (nnSdk, nn::gfx, NVN) + open-ead/nnware (NintendoWare).
set(_letsgo_inc "-isystem \"${LETSGO_SYSROOT}/include\"")
foreach(_d nnsdk/include nnsdk/lib/gfx/include nnsdk/lib/nvn/include
           nnware/lib/atk/include nnware/lib/font/include nnware/lib/g3d/include
           nnware/lib/ui2d/include nnware/lib/vfx/include)
  string(APPEND _letsgo_inc " -isystem \"${LETSGO_ROOT}/toolchain/${_d}\"")
endforeach()
set(_letsgo_cxx_inc "-nostdinc++ -isystem \"${LETSGO_SYSROOT}/include/c++/config\" -isystem \"${LETSGO_ROOT}/toolchain/llvm-mingw/include/c++/v1\"")

set(CMAKE_C_FLAGS_INIT "${_letsgo_common} ${_letsgo_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_letsgo_common} -fno-exceptions -fno-rtti ${_letsgo_cxx_inc} ${_letsgo_inc}")
set(CMAKE_ASM_FLAGS_INIT "${_letsgo_common}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -nostdlib -static")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
