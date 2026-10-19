// Frame timing and the costly work done between frames. With LETSGO_PERF=1 every slow frame is
// logged with what that time went to, and a summary of frame times every 600 frames.
#pragma once
#include <cstdint>

namespace rt::perf {

enum Work : int {
    ShaderTranslate,     // NV assembly or GPU code to GLSL
    ShaderCompile,       // GLSL to SPIR-V (glslang)
    ShaderDriver,        // SPIR-V to the GPU's code (vkCreateShadersEXT)
    ShaderWait,          // a draw waiting for a shader another thread is building
    TextureDecode,       // deswizzling and converting (ASTC decoding) texture data
    TextureUpload,       // staging a texture for the GPU (includes its decoding)
    kWorkCount
};

uint64_t ticks();                         // a high-resolution clock (QueryPerformanceCounter)
double ms(uint64_t ticks);
void add(Work w, uint64_t ticks);         // time spent on `w` by any thread
void frame();                             // a frame was presented
bool enabled();

struct Scope {
    explicit Scope(Work w) : w(w), t0(ticks()) {}
    ~Scope() { add(w, ticks() - t0); }
    Work w;
    uint64_t t0;
};

}  // namespace rt::perf
