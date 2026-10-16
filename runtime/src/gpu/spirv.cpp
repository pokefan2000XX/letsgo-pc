#include "spirv.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <mutex>

namespace gpu {

bool compile_glsl(nvasm::Stage stage, const std::string& glsl, std::vector<uint32_t>& spirv, std::string& log) {
    static std::once_flag init;
    std::call_once(init, [] { glslang_initialize_process(); });
    glslang_stage_t st = stage == nvasm::Stage::Vertex     ? GLSLANG_STAGE_VERTEX
                         : stage == nvasm::Stage::Fragment ? GLSLANG_STAGE_FRAGMENT
                                                           : GLSLANG_STAGE_COMPUTE;
    glslang_input_t in = {};
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = st;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = glsl.c_str();
    in.default_version = 460;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = (glslang_messages_t)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    in.resource = glslang_default_resource();
    glslang_shader_t* sh = glslang_shader_create(&in);
    bool ok = glslang_shader_preprocess(sh, &in) && glslang_shader_parse(sh, &in);
    if (!ok) {
        log = std::string(glslang_shader_get_info_log(sh)) + glslang_shader_get_info_debug_log(sh);
        glslang_shader_delete(sh);
        return false;
    }
    glslang_program_t* pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, (int)(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))) {
        log = glslang_program_get_info_log(pr);
        glslang_program_delete(pr);
        glslang_shader_delete(sh);
        return false;
    }
    glslang_program_SPIRV_generate(pr, st);
    spirv.resize(glslang_program_SPIRV_get_size(pr));
    glslang_program_SPIRV_get(pr, spirv.data());
    if (const char* m = glslang_program_SPIRV_get_messages(pr)) log = m;
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return !spirv.empty();
}

}  // namespace gpu
