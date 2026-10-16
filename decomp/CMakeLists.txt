# Decompiled game code, compiled for the Switch's CPU with cmake/guest-aarch64.cmake. Not built
# directly: decomp/tools/build.py builds this, links it at its guest address and recompiles it
# with the rest of the game (the main build runs it).
cmake_minimum_required(VERSION 3.20)
project(letsgo_decomp C CXX)

file(GLOB_RECURSE DECOMP_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(decomp STATIC ${DECOMP_SRC})
target_include_directories(decomp PRIVATE include)
set_target_properties(decomp PROPERTIES CXX_STANDARD 20 C_STANDARD 11)
# Game memory is reinterpreted freely; FP contraction would change results (the original's fused
# operations are written as fma); the game's operator delete has no size parameter.
target_compile_options(decomp PRIVATE -O2 -Wall -Wextra -Wno-invalid-offsetof -fno-strict-aliasing
                       -ffp-contract=off -fno-sized-deallocation)
