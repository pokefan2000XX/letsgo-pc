// Vulkan without an import library: entry points are loaded from vulkan-1.dll
// (installed with the GPU driver) into these function pointers.
#pragma once
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

namespace vk {
#define VKF_GLOBAL(f) extern PFN_##f f;
#define VKF_INSTANCE(f) extern PFN_##f f;
#define VKF_DEVICE(f) extern PFN_##f f;
#include "vk_funcs.inc"
#undef VKF_GLOBAL
#undef VKF_INSTANCE
#undef VKF_DEVICE

bool load_global();                     // vulkan-1.dll + global entry points
void load_instance(VkInstance inst);
void load_device(VkDevice dev);
const char* result_name(VkResult r);
}  // namespace vk

// VK_CHECK(call): fatal with the call's text and result name on failure
#define VK_CHECK(x)                                                                                       \
    do {                                                                                                  \
        VkResult r_ = (x);                                                                                \
        if (r_ != VK_SUCCESS) rt::fatal("Vulkan: %s failed: %s", #x, vk::result_name(r_));                \
    } while (0)
