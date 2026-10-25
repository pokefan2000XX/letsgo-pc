cmake_minimum_required(VERSION 3.20)
project(letsgo C CXX)
# Build with cmake/host-x86_64.cmake. Generated code comes from
#   python -P recomp/recomp.py main sdk   (into build/gen)

set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "recompiler output")
file(GLOB GEN_MAIN "${GEN_DIR}/main/*.c")
file(GLOB GEN_SDK "${GEN_DIR}/sdk/*.c")
if(NOT GEN_MAIN OR NOT GEN_SDK)
  message(FATAL_ERROR "no generated code in ${GEN_DIR}: run recomp/recomp.py first")
endif()

# Recompiled guest code: strict IEEE (no FMA contraction - ARM rounds fmul and fadd
# separately), wrapping signed arithmetic, no errno from math functions.
set(GUEST_FLAGS -O2 -march=x86-64-v3 -ffp-contract=off -fwrapv -fno-math-errno -fno-strict-aliasing
    -Wno-parentheses-equality -Wno-unused-label -Wno-pedantic)

option(LETSGO_CHECK_CALLS "verify callee-saved registers after every guest call (debug)" OFF)
if(LETSGO_CHECK_CALLS)
  list(APPEND GUEST_FLAGS -DRC_CHECK_CALLS)
endif()

# Decompiled code (decomp/): decomp/tools/build.py compiles it for the Switch's CPU, links it at its
# guest address and recompiles it into build/gen/patch. Its functions override the originals
# they replace at link time (recomp.py: originals are weak aliases).
find_program(PYTHON_EXE NAMES python python3 REQUIRED)
file(GLOB_RECURSE DECOMP_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/decomp/src/*" "${CMAKE_SOURCE_DIR}/decomp/include/*")
# A hack's code (hacks/<name>/code) is built into the same module, for the hacks named here
# (-DLETSGO_HACKS=name;other). Its files for the game go in a mods/ folder (runtime/src/mods.cpp).
# A new build directory (build-dev.bat) starts with these.
set(LETSGO_HACKS "new-script-command;quick-boot;smooth-moves" CACHE STRING
    "hacks whose code is built into the decomp module (hacks/<name>/code)")
set(DECOMP_HACK_ARGS)
foreach(hack ${LETSGO_HACKS})
  list(APPEND DECOMP_HACK_ARGS --hack "${hack}")
  file(GLOB_RECURSE HACK_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/hacks/${hack}/code/*")
  list(APPEND DECOMP_INPUTS ${HACK_INPUTS})
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/decomp_hacks.txt" CONTENT "${LETSGO_HACKS}\n")   # rebuilds when the list changes
# The module is built in <DECOMP_OUT_DIR>/decomp and <DECOMP_OUT_DIR>/gen/patch. A second build of this tree
# with other hacks gives it a folder of its own, or the two builds take turns overwriting one module.
set(DECOMP_OUT_DIR "${CMAKE_SOURCE_DIR}/build" CACHE PATH "where the decomp module is built (<dir>/decomp, <dir>/gen/patch)")
set(GEN_PATCH "${DECOMP_OUT_DIR}/gen/patch/f000.c" "${DECOMP_OUT_DIR}/gen/patch/table.c")
add_custom_command(OUTPUT ${GEN_PATCH}
  COMMAND "${PYTHON_EXE}" -P "${CMAKE_SOURCE_DIR}/decomp/tools/build.py" --ninja "${CMAKE_MAKE_PROGRAM}" --cmake "${CMAKE_COMMAND}"
          --out "${DECOMP_OUT_DIR}" --gen "${GEN_DIR}" ${DECOMP_HACK_ARGS}
  DEPENDS ${DECOMP_INPUTS} "${CMAKE_SOURCE_DIR}/decomp/symbols.txt" "${CMAKE_SOURCE_DIR}/decomp/patch.ld" "${CMAKE_BINARY_DIR}/decomp_hacks.txt"
          "${CMAKE_SOURCE_DIR}/decomp/CMakeLists.txt" "${CMAKE_SOURCE_DIR}/decomp/tools/build.py"
          "${CMAKE_SOURCE_DIR}/recomp/recomp.py" "${CMAKE_SOURCE_DIR}/recomp/emit.py" "${GEN_DIR}/main/table.c"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Building the decomp module (decomp/)")

add_library(guest OBJECT ${GEN_MAIN} ${GEN_SDK} ${GEN_PATCH})
target_include_directories(guest PRIVATE runtime/include)
target_compile_options(guest PRIVATE ${GUEST_FLAGS})
set_target_properties(guest PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

# glslang (GLSL -> SPIR-V) from toolchain/glslang, built by tools/setup_toolchain.sh
set(glslang_DIR "${CMAKE_SOURCE_DIR}/toolchain/glslang/lib/cmake/glslang")
find_package(glslang REQUIRED CONFIG)
set(VULKAN_HEADERS "${CMAKE_SOURCE_DIR}/toolchain/src/Vulkan-Headers-vulkan-sdk-1.4.363.0/include")

# the NVN shader translator, shared by the runtime and tools/shadertest
add_library(gpu_shaders STATIC runtime/src/gpu/nvasm.cpp runtime/src/gpu/maxwell.cpp runtime/src/gpu/spirv.cpp)
target_compile_options(gpu_shaders PRIVATE -O2 -Wall)
set_target_properties(gpu_shaders PROPERTIES CXX_STANDARD 20)
target_link_libraries(gpu_shaders PUBLIC glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)

add_executable(shadertest tools/shadertest/main.cpp)
target_include_directories(shadertest PRIVATE runtime/src)
set_target_properties(shadertest PROPERTIES CXX_STANDARD 20)
target_link_libraries(shadertest PRIVATE gpu_shaders)
target_link_options(shadertest PRIVATE -static)

# the mods folder's archive patching (runtime/src/mods.cpp) on the game's own archives: tools/check_mods.py
add_executable(modstest tools/modstest/main.cpp runtime/src/mods.cpp runtime/src/log.cpp)
target_include_directories(modstest PRIVATE runtime/include runtime/src)
target_compile_options(modstest PRIVATE -O2 -Wall)
set_target_properties(modstest PROPERTIES CXX_STANDARD 20)
target_link_options(modstest PRIVATE -static)

# The runtime (the PC side of the game), built into both executables
set(RUNTIME_SOURCES
  runtime/src/main.cpp runtime/src/log.cpp runtime/src/perf.cpp runtime/src/memory.cpp runtime/src/kernel.cpp runtime/src/loader.cpp
  runtime/src/nx.cpp runtime/src/setup.cpp runtime/src/mods.cpp runtime/src/settings.cpp runtime/src/framerate.cpp
  runtime/src/hle.cpp runtime/src/hle_sys.cpp runtime/src/hle_hid.cpp runtime/src/hle_audio.cpp runtime/src/hle_net.cpp
  runtime/src/hle_movie.cpp runtime/src/hle_applet.cpp runtime/src/webm.cpp runtime/src/nvn.cpp runtime/src/platform.cpp
  runtime/src/gpu/device.cpp runtime/src/gpu/texture.cpp runtime/src/gpu/astc.cpp runtime/src/gpu/nvn_vk.cpp
  runtime/src/gpu/shaders.cpp
  runtime/src/rc_support.c)
# The shader cache (runtime/src/gpu/shaders.cpp) keeps translations made by this version of the
# translator: a hash of its sources and glslang's version. Editing them reconfigures, which updates it.
set(SHADER_TRANSLATOR runtime/src/gpu/nvasm.cpp runtime/src/gpu/nvasm.h runtime/src/gpu/maxwell.cpp
    runtime/src/gpu/maxwell.h runtime/src/gpu/spirv.cpp runtime/src/gpu/shaders.cpp)
set(_shader_sums "${glslang_VERSION}")
foreach(f ${SHADER_TRANSLATOR})
  file(SHA256 "${CMAKE_SOURCE_DIR}/${f}" _sum)
  string(APPEND _shader_sums "${_sum}")
endforeach()
string(SHA256 _shader_version "${_shader_sums}")
string(SUBSTRING "${_shader_version}" 0 16 _shader_version)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SHADER_TRANSLATOR})
set_source_files_properties(runtime/src/gpu/shaders.cpp PROPERTIES COMPILE_DEFINITIONS "LETSGO_SHADER_VERSION=0x${_shader_version}ull")
# SDL3 (window, keyboard, gamepads, sound), linked in: toolchain/SDL3-static, built from source by
# tools/setup_toolchain.sh
set(SDL3_DIR "${CMAKE_SOURCE_DIR}/toolchain/SDL3-static/lib/cmake/SDL3")
find_package(SDL3 3.4 REQUIRED CONFIG COMPONENTS SDL3-static)
# M4 codecs (opus, vorbis/ogg, VP9) from toolchain/codecs, built by tools/setup_codecs.sh
set(CODECS "${CMAKE_SOURCE_DIR}/toolchain/codecs")

function(letsgo_executable target)
  target_include_directories(${target} PRIVATE runtime/include runtime/src ${VULKAN_HEADERS} ${CODECS}/include)
  target_compile_options(${target} PRIVATE -O2 -march=x86-64-v3 -ffp-contract=off -Wall -Wno-unused-function)
  set_target_properties(${target} PROPERTIES CXX_STANDARD 20 C_STANDARD 11 C_EXTENSIONS ON)
  target_link_options(${target} PRIVATE -static -Wl,--stack,67108864 -Wl,--disable-high-entropy-va
                      -Wl,-Map=${CMAKE_BINARY_DIR}/${target}.map)
  target_link_libraries(${target} PRIVATE gpu_shaders SDL3::SDL3-static ${CODECS}/lib/libopus.a ${CODECS}/lib/libvpx.a
                        ${CODECS}/lib/libvorbis.a ${CODECS}/lib/libogg.a pthread bcrypt winmm comdlg32 shell32 dwmapi)
endfunction()

# letsgo: the development build. The game comes from orig/ (or the NSPs with --base/--update/--keys),
# and a console shows the log.
add_executable(letsgo ${RUNTIME_SOURCES} $<TARGET_OBJECTS:guest>)
letsgo_executable(letsgo)

# LetsGoPikachu: the single-file build for players. It contains no game files: the game comes from the
# player's own base game NSP, update NSP and prod.keys, chosen on the first start. No console window.
add_executable(LetsGoPikachu WIN32 ${RUNTIME_SOURCES} $<TARGET_OBJECTS:guest>)
letsgo_executable(LetsGoPikachu)
target_compile_definitions(LetsGoPikachu PRIVATE LETSGO_DIST=1)
