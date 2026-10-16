// Vulkan device layer (see gpu.h).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "../rt.h"
#include "gpu.h"

namespace vk {
#define VKF_GLOBAL(f) PFN_##f f;
#define VKF_INSTANCE(f) PFN_##f f;
#define VKF_DEVICE(f) PFN_##f f;
#include "vk_funcs.inc"
#undef VKF_GLOBAL
#undef VKF_INSTANCE
#undef VKF_DEVICE

static PFN_vkGetInstanceProcAddr gipa;

bool load_global() {
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) return false;
    gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!gipa) return false;
#define VKF_GLOBAL(f) f = (PFN_##f)gipa(nullptr, #f);
#define VKF_INSTANCE(f)
#define VKF_DEVICE(f)
#include "vk_funcs.inc"
#undef VKF_GLOBAL
#undef VKF_INSTANCE
#undef VKF_DEVICE
    return vkCreateInstance != nullptr;
}
void load_instance(VkInstance inst) {
#define VKF_GLOBAL(f)
#define VKF_INSTANCE(f) f = (PFN_##f)gipa(inst, #f);
#define VKF_DEVICE(f)
#include "vk_funcs.inc"
#undef VKF_GLOBAL
#undef VKF_INSTANCE
#undef VKF_DEVICE
}
void load_device(VkDevice dev) {
#define VKF_GLOBAL(f)
#define VKF_INSTANCE(f)
#define VKF_DEVICE(f) f = (PFN_##f)vkGetDeviceProcAddr(dev, #f);
#include "vk_funcs.inc"
#undef VKF_GLOBAL
#undef VKF_INSTANCE
#undef VKF_DEVICE
    if (!vkCmdPushDescriptorSet) vkCmdPushDescriptorSet = (PFN_vkCmdPushDescriptorSet)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
}
const char* result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default: return "VkResult(?)";
    }
}
}  // namespace vk

namespace rt {
void* platform_hwnd();
}

namespace gpu {

Device g;

// ---- memory: 256 MiB blocks per memory type, first-fit with coalescing ----------------------------
namespace {
struct Block {
    VkDeviceMemory mem;
    uint32_t type;
    VkDeviceSize size;
    std::map<VkDeviceSize, VkDeviceSize> free;     // offset -> size
    uint8_t* mapped;
};
std::mutex g_mem_lock;
std::vector<Block> g_blocks;
constexpr VkDeviceSize kBlock = 256ull << 20;

VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }
}  // namespace

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

Alloc alloc_memory(const VkMemoryRequirements& req, VkMemoryPropertyFlags want) {
    uint32_t type = memory_type(req.memoryTypeBits, want);
    if (type == ~0u) type = memory_type(req.memoryTypeBits, 0);
    if (type == ~0u) rt::fatal("Vulkan: no memory type for bits 0x%x", req.memoryTypeBits);
    std::lock_guard<std::mutex> lk(g_mem_lock);
    VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, g.props.limits.bufferImageGranularity);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t bi = 0; bi < g_blocks.size(); bi++) {
            Block& b = g_blocks[bi];
            if (b.type != type) continue;
            for (auto it = b.free.begin(); it != b.free.end(); ++it) {
                VkDeviceSize start = align_up(it->first, align), end = it->first + it->second;
                if (start + req.size > end) continue;
                VkDeviceSize off = it->first;
                b.free.erase(it);
                if (start > off) b.free[off] = start - off;
                if (start + req.size < end) b.free[start + req.size] = end - (start + req.size);
                return Alloc{b.mem, start, req.size, (int)bi, b.mapped ? b.mapped + start : nullptr};
            }
        }
        // new block (dedicated if larger than a block)
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = std::max(kBlock, align_up(req.size, 1 << 20));
        ai.memoryTypeIndex = type;
        VkDeviceMemory mem;
        VK_CHECK(vk::vkAllocateMemory(g.dev, &ai, nullptr, &mem));
        Block b{mem, type, ai.allocationSize, {}, nullptr};
        if (g.mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            VK_CHECK(vk::vkMapMemory(g.dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.mapped));
        b.free[0] = ai.allocationSize;
        g_blocks.push_back(std::move(b));
    }
    rt::fatal("Vulkan: allocation of %llu bytes failed", (unsigned long long)req.size);
}

void free_memory(const Alloc& a) {
    if (a.block < 0) return;
    std::lock_guard<std::mutex> lk(g_mem_lock);
    auto& fl = g_blocks[(size_t)a.block].free;
    VkDeviceSize off = a.offset, size = a.size;
    auto next = fl.lower_bound(off);
    if (next != fl.end() && next->first == off + size) {
        size += next->second;
        next = fl.erase(next);
    }
    if (next != fl.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            prev->second += size;
            return;
        }
    }
    fl[off] = size;
}

// ---- guest memory pools ---------------------------------------------------------------------------
namespace {
std::mutex g_pool_lock;
struct PoolRec {
    uint64_t size;
    uint32_t flags;
    HostBuffer hb;
};
std::map<uint64_t, PoolRec> g_pools;      // base -> pool

bool import_pool(uint64_t base, PoolRec& p) {
    if (!g.host_import) return false;
    if ((base & (g.import_align - 1)) || (p.size & (g.import_align - 1))) {
        rt::log("gpu: pool 0x%llx (+0x%llx) is not aligned for import", (unsigned long long)base, (unsigned long long)p.size);
        return false;
    }
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vk::vkGetMemoryHostPointerPropertiesEXT(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                (void*)(uintptr_t)base, &hp) != VK_SUCCESS)
        return false;
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb};
    bi.size = p.size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (g.bda) bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VK_CHECK(vk::vkCreateBuffer(g.dev, &bi, nullptr, &p.hb.buf));
    VkMemoryRequirements req;
    vk::vkGetBufferMemoryRequirements(g.dev, p.hb.buf, &req);
    uint32_t type = memory_type(req.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == ~0u) {
        vk::vkDestroyBuffer(g.dev, p.hb.buf, nullptr);
        p.hb.buf = VK_NULL_HANDLE;
        return false;
    }
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = (void*)(uintptr_t)base;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &imp};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, g.bda ? (void*)&fl : (void*)&imp};
    ai.allocationSize = p.size;
    ai.memoryTypeIndex = type;
    VkResult r = vk::vkAllocateMemory(g.dev, &ai, nullptr, &p.hb.mem);
    if (r != VK_SUCCESS) {
        rt::log("gpu: importing pool 0x%llx (+0x%llx) failed: %s", (unsigned long long)base, (unsigned long long)p.size, vk::result_name(r));
        vk::vkDestroyBuffer(g.dev, p.hb.buf, nullptr);
        p.hb.buf = VK_NULL_HANDLE;
        return false;
    }
    VK_CHECK(vk::vkBindBufferMemory(g.dev, p.hb.buf, p.hb.mem, 0));
    p.hb.base = base;
    p.hb.size = p.size;
    if (g.bda) {
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = p.hb.buf;
        p.hb.gpu = vk::vkGetBufferDeviceAddress(g.dev, &da);
    }
    return true;
}
}  // namespace

void pool_register(uint64_t base, uint64_t size, uint32_t flags) {
    std::lock_guard<std::mutex> lk(g_pool_lock);
    g_pools[base] = PoolRec{size, flags, {}};
}

void pool_unregister(uint64_t base) {
    std::lock_guard<std::mutex> lk(g_pool_lock);
    auto it = g_pools.find(base);
    if (it == g_pools.end()) return;
    if (it->second.hb.buf) {
        // the GPU may still read it: wait for everything submitted so far
        wait_value(g.submitted);
        vk::vkDestroyBuffer(g.dev, it->second.hb.buf, nullptr);
        vk::vkFreeMemory(g.dev, it->second.hb.mem, nullptr);
    }
    g_pools.erase(it);
}

const HostBuffer* host_buffer(uint64_t addr, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_pool_lock);
    auto it = g_pools.upper_bound(addr);
    if (it == g_pools.begin()) return nullptr;
    --it;
    PoolRec& p = it->second;
    if (addr + size > it->first + p.size) return nullptr;
    if (!p.hb.buf && !import_pool(it->first, p)) return nullptr;
    return &p.hb;
}

// ---- scratch ring ---------------------------------------------------------------------------------
namespace {
constexpr VkDeviceSize kRing = 256ull << 20;
VkBuffer g_ring_buf;
VkDeviceMemory g_ring_mem;
uint8_t* g_ring_ptr;
VkDeviceSize g_ring_head, g_ring_tail;              // allocate at head; [tail, head) may be in use
bool g_ring_empty = true;
std::deque<std::pair<VkDeviceSize, uint64_t>> g_ring_marks;   // (head at submit, timeline value)
std::mutex g_ring_lock;

void ring_init() {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kRing;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VK_CHECK(vk::vkCreateBuffer(g.dev, &bi, nullptr, &g_ring_buf));
    VkMemoryRequirements req;
    vk::vkGetBufferMemoryRequirements(g.dev, g_ring_buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vk::vkAllocateMemory(g.dev, &ai, nullptr, &g_ring_mem));
    VK_CHECK(vk::vkBindBufferMemory(g.dev, g_ring_buf, g_ring_mem, 0));
    VK_CHECK(vk::vkMapMemory(g.dev, g_ring_mem, 0, VK_WHOLE_SIZE, 0, (void**)&g_ring_ptr));
}

// release the space of finished submissions; with `wait`, block for the oldest one first
void ring_retire(bool wait) {
    while (!g_ring_marks.empty()) {
        auto [head, value] = g_ring_marks.front();
        if (completed_value() < value) {
            if (!wait) break;
            wait_value(value);
            wait = false;
        }
        g_ring_tail = head;
        g_ring_marks.pop_front();
        if (g_ring_tail == g_ring_head && g_ring_marks.empty()) {
            g_ring_empty = true;
            g_ring_head = g_ring_tail = 0;
        }
    }
}
}  // namespace

Scratch scratch(VkDeviceSize size, VkDeviceSize align) {
    if (size > kRing / 4) rt::fatal("gpu: scratch request of %llu bytes", (unsigned long long)size);
    std::lock_guard<std::mutex> lk(g_ring_lock);
    for (;;) {
        ring_retire(false);
        VkDeviceSize start = align_up(g_ring_head, align);
        bool ok = false;
        if (g_ring_empty) {
            if (start + size > kRing) start = 0;
            ok = true;
        } else if (g_ring_head > g_ring_tail) {
            if (start + size <= kRing) ok = true;
            else if (size <= g_ring_tail) { start = 0; ok = true; }
        } else {
            ok = start + size <= g_ring_tail;          // head < tail, or full when equal
        }
        if (ok) {
            g_ring_head = start + size;
            g_ring_empty = false;
            return Scratch{g_ring_buf, start, g_ring_ptr + start};
        }
        // the space belongs to submitted work: wait for the oldest submission. If nothing was
        // submitted, the command buffer being recorded alone filled the ring.
        if (g_ring_marks.empty()) rt::fatal("gpu: per-draw scratch ring exhausted within one submission");
        ring_retire(true);
    }
}

Staging staging_alloc(VkDeviceSize size) {
    Staging s;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(size, 16);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vk::vkCreateBuffer(g.dev, &bi, nullptr, &s.buf));
    VkMemoryRequirements req;
    vk::vkGetBufferMemoryRequirements(g.dev, s.buf, &req);
    s.mem = alloc_memory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vk::vkBindBufferMemory(g.dev, s.buf, s.mem.mem, s.mem.offset));
    s.ptr = s.mem.ptr;
    return s;
}

void staging_free(const Staging& s) {
    // called while recording the command buffer that reads it: free after that submission
    retire(g.submitted + 1, [s] {
        vk::vkDestroyBuffer(g.dev, s.buf, nullptr);
        free_memory(s.mem);
    });
}

// ---- deferred work and retirement -------------------------------------------------------------------
namespace {
std::mutex g_defer_lock;
std::vector<std::function<void(VkCommandBuffer)>> g_deferred;
std::deque<std::pair<uint64_t, std::function<void()>>> g_retire;
VkBuffer g_zero;
uint64_t g_zero_addr;

void run_retirements() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lk(g_defer_lock);
        uint64_t done = completed_value();
        while (!g_retire.empty() && g_retire.front().first <= done) {
            ready.push_back(std::move(g_retire.front().second));
            g_retire.pop_front();
        }
    }
    for (auto& f : ready) f();
}
}  // namespace

void defer(std::function<void(VkCommandBuffer)> fn) {
    std::lock_guard<std::mutex> lk(g_defer_lock);
    g_deferred.push_back(std::move(fn));
}

void retire(uint64_t after, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_defer_lock);
    g_retire.push_back({after, std::move(fn)});
}

VkBuffer zero_buffer() { return g_zero; }
uint64_t zero_address() { return g_zero_addr; }

// ---- submission -----------------------------------------------------------------------------------
namespace {
constexpr int kSlots = 16;
struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t value = 0;
};
Slot g_slots[kSlots];
int g_next_slot;
}  // namespace

Cmd begin_cmd() {
    int s = g_next_slot;
    g_next_slot = (g_next_slot + 1) % kSlots;
    Slot& sl = g_slots[s];
    if (!sl.pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = g.qfam;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VK_CHECK(vk::vkCreateCommandPool(g.dev, &pi, nullptr, &sl.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = sl.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vk::vkAllocateCommandBuffers(g.dev, &ai, &sl.cb));
    }
    wait_value(sl.value);
    VK_CHECK(vk::vkResetCommandPool(g.dev, sl.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vk::vkBeginCommandBuffer(sl.cb, &bi));
    run_retirements();
    std::vector<std::function<void(VkCommandBuffer)>> work;
    {
        std::lock_guard<std::mutex> lk(g_defer_lock);
        work.swap(g_deferred);
    }
    for (auto& f : work) f(sl.cb);
    if (!work.empty()) full_barrier(sl.cb);
    return Cmd{sl.cb, s};
}

uint64_t submit_cmd(Cmd& c, VkSemaphore wait, VkSemaphore signal) {
    VK_CHECK(vk::vkEndCommandBuffer(c.cb));
    uint64_t value = ++g.submitted;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = c.cb;
    VkSemaphoreSubmitInfo sig[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    sig[0].semaphore = g.timeline;
    sig[0].value = value;
    sig[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sig[1].semaphore = signal;
    sig[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo wt{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wt.semaphore = wait;
    wt.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = signal ? 2 : 1;
    si.pSignalSemaphoreInfos = sig;
    si.waitSemaphoreInfoCount = wait ? 1 : 0;
    si.pWaitSemaphoreInfos = &wt;
    VK_CHECK(vk::vkQueueSubmit2(g.queue, 1, &si, VK_NULL_HANDLE));
    g_slots[c.slot].value = value;
    {
        std::lock_guard<std::mutex> lk(g_ring_lock);
        if (!g_ring_empty) g_ring_marks.push_back({g_ring_head, value});
    }
    return value;
}

uint64_t completed_value() {
    uint64_t v = 0;
    vk::vkGetSemaphoreCounterValue(g.dev, g.timeline, &v);
    return v;
}

void wait_value(uint64_t value) {
    if (!value || completed_value() >= value) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g.timeline;
    wi.pValues = &value;
    VkResult r = vk::vkWaitSemaphores(g.dev, &wi, 10ull * 1000 * 1000 * 1000);
    if (r == VK_TIMEOUT) rt::fatal("gpu: the GPU did not finish work %llu within 10 s (hang)", (unsigned long long)value);
    if (r != VK_SUCCESS) rt::fatal("gpu: waiting for the GPU failed: %s", vk::result_name(r));
}

void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vk::vkCmdPipelineBarrier2(cb, &di);
}

// ---- swapchain ------------------------------------------------------------------------------------
namespace {
VkSurfaceKHR g_surface;
VkSwapchainKHR g_swapchain;
bool g_swap_srgb;                           // the swapchain encodes sRGB (matches the game's window textures)
VkExtent2D g_swap_extent;
std::vector<VkImage> g_swap_images;
std::vector<VkSemaphore> g_render_done;     // per swapchain image
VkSemaphore g_acquire[kSlots];
int g_acquire_next;

void destroy_swapchain() {
    if (!g_swapchain) return;
    vk::vkDeviceWaitIdle(g.dev);
    for (auto s : g_render_done) vk::vkDestroySemaphore(g.dev, s, nullptr);
    g_render_done.clear();
    vk::vkDestroySwapchainKHR(g.dev, g_swapchain, nullptr);
    g_swapchain = VK_NULL_HANDLE;
}

bool create_swapchain(bool srgb) {
    HWND hwnd = (HWND)rt::platform_hwnd();
    if (!hwnd) return false;
    if (!g_surface) {
        VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        si.hinstance = GetModuleHandleW(nullptr);
        si.hwnd = hwnd;
        VK_CHECK(vk::vkCreateWin32SurfaceKHR(g.instance, &si, nullptr, &g_surface));
        VkBool32 ok = VK_FALSE;
        vk::vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.qfam, g_surface, &ok);
        if (!ok) rt::fatal("Vulkan: the graphics queue can't present to the window");
        for (auto& s : g_acquire) {
            VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vk::vkCreateSemaphore(g.dev, &ci, nullptr, &s));
        }
    }
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vk::vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g_surface, &caps));
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;   // minimized
    uint32_t n = 0;
    vk::vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vk::vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g_surface, &n, fmts.data());
    // a blit converts between the source's and the swapchain's encodings, so the swapchain matches
    // the game's: an sRGB window texture into an sRGB swapchain keeps its colors
    VkSurfaceFormatKHR fmt = fmts[0];
    bool found = false;
    for (VkFormat want : srgb ? std::initializer_list<VkFormat>{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}
                              : std::initializer_list<VkFormat>{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
        for (auto& f : fmts)
            if (!found && f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f, found = true;
    if (!found) rt::log("gpu: no %s swapchain format, colors may be off", srgb ? "sRGB" : "UNORM");
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swapchain;
    VkSwapchainKHR sc;
    VK_CHECK(vk::vkCreateSwapchainKHR(g.dev, &ci, nullptr, &sc));
    destroy_swapchain();
    g_swapchain = sc;
    g_swap_srgb = srgb;
    g_swap_extent = caps.currentExtent;
    vk::vkGetSwapchainImagesKHR(g.dev, sc, &n, nullptr);
    g_swap_images.resize(n);
    vk::vkGetSwapchainImagesKHR(g.dev, sc, &n, g_swap_images.data());
    g_render_done.resize(n);
    for (auto& s : g_render_done) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vk::vkCreateSemaphore(g.dev, &sci, nullptr, &s));
    }
    rt::log("gpu: swapchain %ux%u, %u images, %s", g_swap_extent.width, g_swap_extent.height, n, srgb ? "sRGB" : "UNORM");
    return true;
}

void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vk::vkCmdPipelineBarrier2(cb, &di);
}
}  // namespace

void present(VkImage image, bool srgb, int32_t x, int32_t y, uint32_t w, uint32_t h, bool flip_y) {
    std::lock_guard<std::recursive_mutex> lk(g.queue_lock);
    if ((!g_swapchain || g_swap_srgb != srgb) && !create_swapchain(srgb)) return;
    VkSemaphore acq = g_acquire[g_acquire_next];
    g_acquire_next = (g_acquire_next + 1) % kSlots;
    uint32_t idx = 0;
    VkResult r = vk::vkAcquireNextImageKHR(g.dev, g_swapchain, 1000000000ull, acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { create_swapchain(srgb); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { rt::log("gpu: acquire failed: %s", vk::result_name(r)); return; }
    Cmd c = begin_cmd();
    VkImage dst = g_swap_images[idx];
    image_barrier(c.cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::vkCmdClearColorImage(c.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    // letterbox to the window, keeping the game's aspect ratio
    double sx = (double)g_swap_extent.width / w, sy = (double)g_swap_extent.height / h, s = std::min(sx, sy);
    int32_t dw = (int32_t)(w * s), dh = (int32_t)(h * s);
    int32_t dx = ((int32_t)g_swap_extent.width - dw) / 2, dy = ((int32_t)g_swap_extent.height - dh) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {x, flip_y ? y + (int32_t)h : y, 0};
    blit.srcOffsets[1] = {x + (int32_t)w, flip_y ? y : y + (int32_t)h, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
    full_barrier(c.cb);
    vk::vkCmdBlitImage(c.cb, image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    image_barrier(c.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    submit_cmd(c, acq, g_render_done[idx]);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_render_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &idx;
    r = vk::vkQueuePresentKHR(g.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) create_swapchain(srgb);
}

// ---- init -----------------------------------------------------------------------------------------
namespace {
bool has_ext(const std::vector<VkExtensionProperties>& v, const char* name) {
    for (auto& e : v)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

VkBool32 VKAPI_PTR debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                            const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    static std::atomic<int> n{0};
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) rt::log("vulkan: %s", d->pMessage);
    return VK_FALSE;
}
}  // namespace

bool init() {
    if (!vk::load_global()) {
        rt::log("gpu: vulkan-1.dll not found (no Vulkan driver)");
        return false;
    }
    bool validation = getenv("LETSGO_VK_VALIDATION") != nullptr;
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;
    if (validation) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "letsgo-pc";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    VkResult r = vk::vkCreateInstance(&ici, nullptr, &g.instance);
    if (r != VK_SUCCESS && validation) {
        rt::log("gpu: validation layer unavailable (%s), continuing without", vk::result_name(r));
        exts.pop_back();
        ici.enabledExtensionCount = (uint32_t)exts.size();
        ici.enabledLayerCount = 0;
        validation = false;
        r = vk::vkCreateInstance(&ici, nullptr, &g.instance);
    }
    if (r != VK_SUCCESS) {
        rt::log("gpu: vkCreateInstance failed: %s", vk::result_name(r));
        return false;
    }
    vk::load_instance(g.instance);
    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT di{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        di.pfnUserCallback = debug_cb;
        VkDebugUtilsMessengerEXT m;
        vk::vkCreateDebugUtilsMessengerEXT(g.instance, &di, nullptr, &m);
    }
    // physical device: the first discrete GPU with what we need, else the first that qualifies
    uint32_t n = 0;
    vk::vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vk::vkEnumeratePhysicalDevices(g.instance, &n, pds.data());
    std::vector<VkExtensionProperties> dev_exts;
    for (int pass = 0; pass < 2 && !g.phys; pass++)
        for (auto pd : pds) {
            VkPhysicalDeviceProperties p;
            vk::vkGetPhysicalDeviceProperties(pd, &p);
            if (p.apiVersion < VK_API_VERSION_1_3) continue;
            if (pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            uint32_t ne = 0;
            vk::vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> e(ne);
            vk::vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, e.data());
            if (!has_ext(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has_ext(e, VK_EXT_SHADER_OBJECT_EXTENSION_NAME)) continue;
            if (p.apiVersion < VK_API_VERSION_1_4 && !has_ext(e, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
            g.phys = pd;
            g.props = p;
            dev_exts = e;
            break;
        }
    if (!g.phys) {
        rt::log("gpu: no Vulkan 1.3 GPU with VK_EXT_shader_object and push descriptors");
        return false;
    }
    vk::vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    // queue family with graphics + compute
    vk::vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vk::vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &n, qf.data());
    for (uint32_t i = 0; i < n; i++)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            g.qfam = i;
            break;
        }
    // features: query, then enable what we use
    VkPhysicalDeviceShaderObjectFeaturesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, &so};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f14};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    bool v14 = g.props.apiVersion >= VK_API_VERSION_1_4;
    if (!v14) f13.pNext = &so;
    vk::vkGetPhysicalDeviceFeatures2(g.phys, &f2);
    if (!so.shaderObject || !f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore ||
        !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingSampledImageUpdateAfterBind) {
        rt::log("gpu: required Vulkan features missing");
        return false;
    }
    VkPhysicalDeviceShaderObjectFeaturesEXT so_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    so_on.shaderObject = VK_TRUE;
    VkPhysicalDeviceVulkan14Features f14_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, &so_on};
    f14_on.pushDescriptor = f14.pushDescriptor;
    f14_on.indexTypeUint8 = f14.indexTypeUint8;
    VkPhysicalDeviceVulkan13Features f13_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, v14 ? (void*)&f14_on : (void*)&so_on};
    f13_on.dynamicRendering = VK_TRUE;
    f13_on.synchronization2 = VK_TRUE;
    f13_on.maintenance4 = f13.maintenance4;
    VkPhysicalDeviceVulkan12Features f12_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13_on};
    f12_on.descriptorIndexing = VK_TRUE;
    f12_on.runtimeDescriptorArray = VK_TRUE;
    f12_on.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
    f12_on.descriptorBindingPartiallyBound = VK_TRUE;
    f12_on.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12_on.descriptorBindingUpdateUnusedWhilePending = f12.descriptorBindingUpdateUnusedWhilePending;
    f12_on.timelineSemaphore = VK_TRUE;
    f12_on.samplerFilterMinmax = f12.samplerFilterMinmax;
    f12_on.hostQueryReset = f12.hostQueryReset;
    f12_on.bufferDeviceAddress = f12.bufferDeviceAddress;
    g.bda = f12.bufferDeviceAddress;
    VkPhysicalDeviceVulkan11Features f11_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12_on};
    f11_on.shaderDrawParameters = f11.shaderDrawParameters;
    VkPhysicalDeviceFeatures2 f2_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11_on};
    auto& want = f2_on.features;
    const auto& have = f2.features;
    want.samplerAnisotropy = have.samplerAnisotropy;
    want.depthClamp = have.depthClamp;
    want.depthBounds = have.depthBounds;
    want.wideLines = have.wideLines;
    want.fillModeNonSolid = have.fillModeNonSolid;
    want.independentBlend = have.independentBlend;
    want.dualSrcBlend = have.dualSrcBlend;
    want.logicOp = have.logicOp;
    want.textureCompressionBC = have.textureCompressionBC;
    want.multiViewport = have.multiViewport;
    want.imageCubeArray = have.imageCubeArray;
    want.shaderImageGatherExtended = have.shaderImageGatherExtended;
    want.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    want.shaderInt64 = have.shaderInt64;
    std::vector<const char*> dext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SHADER_OBJECT_EXTENSION_NAME};
    if (!v14) dext.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    g.host_import = has_ext(dev_exts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (g.host_import) dext.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g.qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2_on};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)dext.size();
    dci.ppEnabledExtensionNames = dext.data();
    r = vk::vkCreateDevice(g.phys, &dci, nullptr, &g.dev);
    if (r != VK_SUCCESS) {
        rt::log("gpu: vkCreateDevice failed: %s", vk::result_name(r));
        return false;
    }
    vk::load_device(g.dev);
    vk::vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);
    if (g.host_import) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp};
        vk::vkGetPhysicalDeviceProperties2(g.phys, &p2);
        g.import_align = hp.minImportedHostPointerAlignment;
    }
    // timeline
    VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &st};
    VK_CHECK(vk::vkCreateSemaphore(g.dev, &sci, nullptr, &g.timeline));
    // descriptor layouts: set 0 bindless (textures by NVN pool id), set 1 pushed per draw
    {
        // 0..2 texture2D (float, uint, int), 3 samplers, 4 texture2DArray (every texture as an array view)
        VkDescriptorSetLayoutBinding b[5] = {};
        VkDescriptorBindingFlags bf[5];
        for (int i = 0; i < 5; i++) {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            b[i].descriptorCount = i == 3 ? kMaxSamplers : kMaxTextures;
            b[i].stageFlags = VK_SHADER_STAGE_ALL;
            bf[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                    (f12.descriptorBindingUpdateUnusedWhilePending ? VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT : 0);
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        fi.bindingCount = 5;
        fi.pBindingFlags = bf;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        li.bindingCount = 5;
        li.pBindings = b;
        VK_CHECK(vk::vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &g.set0_layout));
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * kMaxTextures}, {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pi.maxSets = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = ps;
        VK_CHECK(vk::vkCreateDescriptorPool(g.dev, &pi, nullptr, &g.dpool));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g.dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g.set0_layout;
        VK_CHECK(vk::vkAllocateDescriptorSets(g.dev, &ai, &g.set0));
    }
    {
        // set 1: constant buffers 0..15 of the vertex (or compute) stage at 0..15, of the fragment
        // stage at 16..31; storage buffers at 32..39 (compute)
        std::vector<VkDescriptorSetLayoutBinding> b;
        for (uint32_t i = 0; i < 16; i++)
            b.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        for (uint32_t i = 0; i < 16; i++) b.push_back({16 + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        li.bindingCount = (uint32_t)b.size();
        li.pBindings = b.data();
        VK_CHECK(vk::vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &g.set1_layout));
        VkDescriptorSetLayout sets[2] = {g.set0_layout, g.set1_layout};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = sets;
        VK_CHECK(vk::vkCreatePipelineLayout(g.dev, &pli, nullptr, &g.layout));
    }
    ring_init();
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 65536;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (g.bda) bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VK_CHECK(vk::vkCreateBuffer(g.dev, &bi, nullptr, &g_zero));
        VkMemoryRequirements req;
        vk::vkGetBufferMemoryRequirements(g.dev, g_zero, &req);
        // its own allocation: device addresses need memory allocated with that flag
        VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, g.bda ? &fl : nullptr};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory zm;
        VK_CHECK(vk::vkAllocateMemory(g.dev, &ai, nullptr, &zm));
        VK_CHECK(vk::vkBindBufferMemory(g.dev, g_zero, zm, 0));
        if (g.bda) {
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = g_zero;
            g_zero_addr = vk::vkGetBufferDeviceAddress(g.dev, &da);
        }
        defer([](VkCommandBuffer cb) { vk::vkCmdFillBuffer(cb, g_zero, 0, VK_WHOLE_SIZE, 0); });
    }
    rt::log("gpu: %s, Vulkan %u.%u.%u, driver 0x%x%s%s", g.props.deviceName, VK_API_VERSION_MAJOR(g.props.apiVersion),
            VK_API_VERSION_MINOR(g.props.apiVersion), VK_API_VERSION_PATCH(g.props.apiVersion), g.props.driverVersion,
            g.host_import ? ", host memory import" : ", NO host memory import", g.bda ? "" : ", NO buffer device address");
    return true;
}

}  // namespace gpu
