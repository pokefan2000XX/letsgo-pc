// Vulkan device layer for the NVN backend: device, memory, the bindless
// descriptor set that mirrors the NVN texture/sampler pools, an upload ring,
// timeline-semaphore submission and the swapchain.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "vk.h"

namespace gpu {

constexpr uint32_t kMaxTextures = 65536;      // bindless array sizes (NVN pool ids index them)
constexpr uint32_t kMaxSamplers = 4096;
// every shader's push constants (the pipeline layout's and each shader object's): vec4 pc_, x the factor
// taking gl_FragCoord to the game's pixels (1 / the render targets' scale), y the render scale
constexpr VkPushConstantRange kPushConstants{VK_SHADER_STAGE_ALL, 0, 16};

struct Alloc {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    int block = -1;
    uint8_t* ptr = nullptr;        // host-visible memory is mapped persistently
};

// guest memory imported as a Vulkan buffer (VK_EXT_external_memory_host): GPU addresses
// are guest addresses, so any address inside the range is (buf, addr - base)
struct HostBuffer {
    uint64_t base = 0, size = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t gpu = 0;                      // device address of `base` (0 without buffer device address)
};

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    VkDeviceSize import_align = 4096;
    bool host_import = false;              // VK_EXT_external_memory_host
    bool bda = false;                      // buffer device address (shaders reach storage buffers by pointer)
    VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE, set1_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t submitted = 0;                // last timeline value handed to the queue
    std::recursive_mutex queue_lock;       // VkQueue access, submission bookkeeping
    VkSampler default_sampler = VK_NULL_HANDLE;
    VkImageView null_view = VK_NULL_HANDLE, null_uview = VK_NULL_HANDLE, null_iview = VK_NULL_HANDLE;
};
extern Device g;

bool init();                               // false (logged) if Vulkan can't be used

// memory
Alloc alloc_memory(const VkMemoryRequirements& req, VkMemoryPropertyFlags want);
void free_memory(const Alloc& a);
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);

// guest memory pools
void pool_register(uint64_t base, uint64_t size, uint32_t nvn_flags);
void pool_unregister(uint64_t base);
// the imported buffer holding [addr, addr + size); nullptr if no pool covers it
const HostBuffer* host_buffer(uint64_t addr, uint64_t size = 1);

// upload staging: a host-visible buffer of its own; free it with staging_free (after the GPU used it)
struct Staging {
    VkBuffer buf = VK_NULL_HANDLE;
    Alloc mem;
    uint8_t* ptr = nullptr;
};
Staging staging_alloc(VkDeviceSize size, bool readback = false);   // readback: CPU-cached memory, for the CPU to read
void staging_free(const Staging& s);       // safe to call from a deferred op: frees after that submission

// per-draw scratch for the command buffer being recorded (caller holds g.queue_lock): host-visible,
// coherent; valid until that command buffer has executed
struct Scratch {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* ptr = nullptr;
};
Scratch scratch(VkDeviceSize size, VkDeviceSize align = 256);

// submission: commands recorded into `cb` run after everything submitted earlier
struct Cmd {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    int slot = -1;
};
Cmd begin_cmd();                                          // caller holds g.queue_lock
uint64_t submit_cmd(Cmd& c, VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
void wait_value(uint64_t value);                          // block until the GPU reached `value`
uint64_t completed_value();

// work recorded at the start of the next command buffer (texture uploads, layout changes), in order
void defer(std::function<void(VkCommandBuffer)> fn);
// run `fn` once the GPU has finished everything up to timeline value `after` (object destruction)
void retire(uint64_t after, std::function<void()> fn);
// a buffer of zeros (64 KiB) for unbound uniform buffers and missing vertex attributes, and its
// device address (0 without buffer device address) for unbound storage buffers
VkBuffer zero_buffer();
uint64_t zero_address();
// texture pool id `id` is made at the render scale (set 0's binding 5, which shaders read for
// texelFetch and textureSize)
void set_texture_scaled(uint32_t id, bool scaled);

// a full pipeline barrier (all writes visible to all later reads); NVN barriers map to it
void full_barrier(VkCommandBuffer cb);

// presentation: blits the rectangle (x, y, w, h) of `image` (GENERAL layout, bottom-up rows when
// flip_y) to the window's swapchain; no-op without a window
// shows the (x, y, w, h) part of `image` in the window, letterboxed; `srgb`: the image holds
// sRGB-encoded colors (the window's swapchain then encodes too, so the blit keeps them as they are)
void present(VkImage image, bool srgb, int32_t x, int32_t y, uint32_t w, uint32_t h, bool flip_y);

}  // namespace gpu
