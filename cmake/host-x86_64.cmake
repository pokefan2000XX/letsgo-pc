# CMake toolchain for the PC build (x86-64 Windows, Itanium C++ ABI via llvm-mingw).
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/host-x86_64.cmake ...
get_filename_component(LETSGO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LETSGO_LLVM "${LETSGO_ROOT}/toolchain/llvm-mingw/bin")
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "${LETSGO_LLVM}/clang.exe")
set(CMAKE_CXX_COMPILER "${LETSGO_LLVM}/clang++.exe")
set(CMAKE_RC_COMPILER "${LETSGO_LLVM}/llvm-rc.exe")
set(CMAKE_AR "${LETSGO_LLVM}/llvm-ar.exe")
set(CMAKE_RANLIB "${LETSGO_LLVM}/llvm-ranlib.exe")
