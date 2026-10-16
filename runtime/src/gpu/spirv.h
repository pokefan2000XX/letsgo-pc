// GLSL -> SPIR-V through glslang (Vulkan 1.3 rules, SPIR-V 1.6).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nvasm.h"

namespace gpu {
// false with the compiler's messages in `log` when the source doesn't compile
bool compile_glsl(nvasm::Stage stage, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& log);
}
