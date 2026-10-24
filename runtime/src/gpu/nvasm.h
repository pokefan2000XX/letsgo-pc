// NV_gpu_program5 assembly -> GLSL (Vulkan) translator.
//
// Every NVN shader binary the game ships carries, in its control section, the
// NV_gpu_program5 assembly the offline compiler produced (header offset 0x14:
// text offset, 0x18: text size, 0x20: offset of the program.local constants).
// That text is translated to GLSL 4.60 here and compiled to SPIR-V by glslang.
//
// Model: registers are uvec4 bit patterns; each instruction loads its operands
// in the instruction's data type (float/int/uint), computes, and stores bits
// through the write mask, so integer and float data survive moves untouched.
// Resources use one fixed layout shared with the Vulkan backend:
//   set 0 (bindless, mirrors the NVN texture and sampler pools):
//     0 texture2D g_tex[]   1 utexture2D g_utex[]   2 itexture2D g_itex[]   3 sampler g_smp[]
//     4 texture2DArray g_texa[] (every texture again, as an array view)
//     5 uint g_scaled[] (a bit per texture id: made at the render scale)
//   set 1 (pushed per draw): constant buffer N of a stage at kStageBase[stage] + N
//     (N = 0..13; NVN puts bound texture handles in buffer 14; maxwell.h puts the NVN
//     driver data in buffer 15), storage buffer N at 32 + N.
//   push constants: vec4 pc_ (gpu.h's kPushConstants).
// Texture handles (nvnDeviceGetTextureHandle) are (sampler id << 20) | texture id.
//
// The render scale (nvn_vk.cpp): screen-sized render targets are made `scale` times the
// game's size, and programs keep working in the game's pixels. A fragment program reads
// gl_FragCoord as fc_ (x and y over the render targets' scale). texelFetch and
// textureSize of a scaled texture take and give the game's texel coordinates (sk_(id) is
// a texture's scale); a fragment program drawing at the scale fetches the texel under its
// own pixel within the game's (ph_), so a full-screen pass keeps the scaled detail.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nvasm {

enum class Stage { Vertex, Fragment, Compute };
constexpr int kStageBase[3] = {0, 16, 0};     // set-1 binding of constant buffer 0, per stage
constexpr int kStorageBase = 32;
constexpr int kMaxCbufs = 16;

struct Reflection {
    Stage stage = Stage::Vertex;
    uint32_t cbufs = 0;          // bit N: program.buffer[N] is read
    uint32_t storage = 0;        // bit N: program.storage[N] is accessed
    uint32_t inputs = 0;         // vertex.attrib[N] / fragment.attrib[N]
    uint32_t int_inputs = 0;     // vertex attributes read as integers (need an integer vertex format)
    uint32_t outputs = 0;        // vertex: result.attrib[N]; fragment: result.color[N]
    uint32_t group_size[3] = {1, 1, 1};
    bool textures = false;
};

struct Options {
    bool gl_depth = true;        // NVN depth mode NEAR_IS_MINUS_W: remap clip z from [-w, w] to [0, w]
    uint32_t sint_inputs = 0;    // integer vertex attributes whose format is signed (declared ivec4)
    uint32_t uint_inputs = 0;    // maxwell.h only: vertex attributes whose format is unsigned integer
};

struct Result {
    bool ok = false;
    std::string error;           // what failed, with the offending statement
    std::string glsl;
    std::string asm_text;        // the source, for logs and caches
    Reflection refl;
};

// `control` is NVNshaderData::control; `size` bounds every read from it.
Result translate(const uint8_t* control, size_t size, const Options& opt = {});

// the render scale's declarations (pc_, g_scaled, sk_, and ph_ for a fragment program) and the
// first line of main() of a fragment program that reads fc_
std::string scale_decls(Stage stage);
constexpr const char* kFragCoordLine = "    vec4 fc_ = vec4(gl_FragCoord.xy * pc_.x, gl_FragCoord.zw);\n";

}  // namespace nvasm
