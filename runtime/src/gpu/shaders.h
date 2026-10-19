// Shader variants for the NVN backend: translation (NV assembly or GPU code to GLSL), glslang (GLSL
// to SPIR-V) and the driver (vkCreateShadersEXT), done once per distinct shader and shared by every
// program that uses it.
//
// Builds run on worker threads from the moment the game sets a program's shaders (it does that while
// loading a model or effect), so they're usually done by the program's first draw; a draw that gets
// there first finishes the build itself rather than queueing behind others. Translated shaders are
// kept in a cache file between runs, and the shader objects of cached shaders are created in the
// background at startup, so a shader seen in an earlier run costs nothing when it's needed.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nvasm.h"
#include "vk.h"

namespace gpu {

// a program stage's code, as the game gave it
struct ShaderSource {
    int stage = 0;                           // 0 vertex, 1 fragment, 2 compute
    bool maxwell = false;                    // GPU code (no NV assembly)
    std::vector<uint8_t> control, code;      // copies of the control section (and of the GPU code)
    uint64_t hash[2] = {};                   // of all the above and the translator's version
};

struct ShaderBuild;                          // one variant of a source: reflection, SPIR-V, shader object

// the file translated shaders are kept in between runs (empty: none); call before the first shader
void shaders_set_cache(const std::string& path);
// frame dumps: every variant keeps its GLSL, and the cache isn't used
void shaders_keep_glsl();

std::shared_ptr<ShaderSource> make_source(int stage, bool maxwell, const uint8_t* control, size_t control_size,
                                         const uint8_t* code = nullptr, size_t code_size = 0);
// the build of `src` for the vertex inputs' integer types `key` (as shader_variant computes it),
// shared by every program with this code, and queued for a worker if it isn't built yet.
// `prefetch`: also queue the other variants this code needed in earlier runs
std::shared_ptr<ShaderBuild> shader_build(const std::shared_ptr<ShaderSource>& src, uint32_t key, bool prefetch = false);
// the variant's reflection, from the cache or a finished translation, or by translating on this
// thread. False if the code can't be translated (`error` says why)
bool shader_reflection(ShaderBuild& b, nvasm::Reflection& refl, std::string& error);
// the shader object (null if building it failed), finishing the build on this thread if no worker
// has started it, or waiting for the worker that has
VkShaderEXT shader_object(ShaderBuild& b);
// the GLSL the variant was compiled from, for frame dumps ("" when it came from the cache)
std::string shader_glsl(ShaderBuild& b);

}  // namespace gpu
