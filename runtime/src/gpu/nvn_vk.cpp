// NVN on Vulkan (milestone 3).
//
// Objects: NVN structs are game-owned; builders and state objects hold our packed
// state directly, real objects (textures, samplers, programs, command buffers) keep
// a pointer to a host object. GPU addresses are guest addresses (gpu::host_buffer).
// Commands: nvnCommandBuffer* calls record closures; nvnQueueSubmitCommands replays
// them into a Vulkan command buffer. GPU state persists across command buffers and
// submissions, as it does on the hardware.
// Conventions: NVN's default lower-left window origin and [-w, w] clip depth are
// kept. Framebuffer rows are the same as NVN's, so only presentation flips; the
// translated vertex shaders remap depth (gpu/nvasm.h).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../hle.h"
#include "gpu.h"
#include "maxwell.h"
#include "nvasm.h"
#include "shaders.h"
#include "../perf.h"
#include "texture.h"

namespace rt::nvn {
void set_proc(const char* name, rc_fn fn);
void frame_pace(uint64_t window_interval);
void frame_done();
uint32_t window_interval(uint64_t window);
}  // namespace rt::nvn

namespace gpu {
namespace {
using rt::log;

template <class T> T* G(uint64_t p) { return (T*)(uintptr_t)p; }
float F(Ctx* c, int i) { return c->v[i].f[0]; }
void ret_ok(Ctx* c) { c->x[0] = 1; }

// ---- packed state kept in the game's NVN structs ----------------------------------------------------
struct GPoolB { uint64_t mem, size; uint32_t flags; };
struct GPool { uint64_t mem, size; uint32_t flags; };
struct GBufB { uint64_t pool, offset, size; };
struct GBuf { uint64_t addr, size; };
struct GTexB {
    uint32_t w, h, d, levels, format, target, samples, flags;
    uint64_t pool, offset;
    uint32_t stride, layout_set;
    uint8_t layout[8];
};
struct Texture;
struct GTex {
    uint32_t w, h, d, levels, format, target, samples, flags;
    uint64_t addr;
    Texture* host;
};
enum { V_LEVELS = 1, V_LAYERS = 2, V_FORMAT = 4, V_SWIZZLE = 8, V_DSMODE = 16, V_TARGET = 32 };
struct GView {
    uint16_t set;
    uint8_t target, dsmode;
    uint32_t format;
    uint16_t base_level, num_levels;
    uint32_t base_layer, num_layers;
    uint8_t swz[4];
};
struct GSmpB {
    uint32_t min, mag, wrap[3], cmp_mode, cmp_func, reduction;
    float lod_min, lod_max, lod_bias, aniso;
    float border[4];
};
struct GSmp { VkSampler s; };
struct GBlend { uint8_t target, src, dst, srca, dsta, eq, eqa, pad; };
struct GColor { uint8_t blend_mask, logic_op, alpha_test, pad; };
struct GChannel { uint32_t disabled; };     // 4 bits per render target: channel writes disabled
struct GDepth { uint8_t flags, func, sfunc, sop_f0, sop_f1, sop_b0, sop_b1, pad; };   // flags: 1 test 2 write 4 stencil
struct GMs { uint8_t enable, samples, a2c, a2c_dither; };
struct GPoly { uint8_t cull, front, mode, offset; };
struct GAttrib { uint8_t format, stream; uint16_t offset; };
struct GStream { uint32_t stride, divisor; };
struct GWinB { uint32_t count, interval; uint64_t textures; };
struct GWin { uint32_t count, next, interval, pad; uint64_t tex[8]; int32_t crop[4]; };   // crop w 0: whole texture
struct GSync { uint64_t value; };

static_assert(sizeof(GView) <= 0x28 && sizeof(GTexB) <= 0x80 && sizeof(GTex) <= 0xC0 && sizeof(GSmpB) <= 0x60, "struct sizes");
static_assert(sizeof(GAttrib) == 4 && sizeof(GStream) == 8 && sizeof(GDepth) == 8 && sizeof(GBlend) == 8, "state sizes");
static_assert(sizeof(GWin) <= 384 && sizeof(GMs) <= 0x18, "struct sizes");

bool g_ready = false;
std::once_flag g_init_once;

// ---- textures --------------------------------------------------------------------------------------
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    Alloc mem;
    const FormatInfo* fi = nullptr;
    uint32_t nvn_format = 0;
    uint32_t w = 1, h = 1, d = 1, levels = 1, layers = 1, target = 1, samples = 1, flags = 0;
    uint64_t addr = 0;
    bool cpu_visible = false;
    bool watched = false;                     // in a CPU-writable pool: re-uploaded when the CPU writes it
    TexLayout layout;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    std::mutex lock;
    std::unordered_map<uint64_t, VkImageView> views;
};

std::mutex g_desc_lock;

// textures the CPU may write directly (glyph caches and the like): checked before every submission
std::mutex g_watch_lock;
std::vector<Texture*> g_watched;

bool format_supports(VkFormat f, VkFormatFeatureFlags feat) {
    VkFormatProperties p;
    vk::vkGetPhysicalDeviceFormatProperties(g.phys, f, &p);
    return (p.optimalTilingFeatures & feat) == feat;
}

VkImageAspectFlags aspect_of(const FormatInfo& f) {
    switch (f.aspect) {
    case 1: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case 2: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case 3: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

void transition_general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = img;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vk::vkCmdPipelineBarrier2(cb, &di);
}

// stage every level/layer of `t` from its guest storage (deswizzled, converted) and queue the copy
void upload_from_guest(Texture* t) {
    const FormatInfo& f = *t->fi;
    if (!f.bpb || t->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !t->addr) return;
    rt::perf::Scope timed(rt::perf::TextureUpload);
    std::vector<uint8_t> tmp;
    std::vector<uint8_t> all;
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t layer = 0; layer < t->layers; layer++)
        for (uint32_t lv = 0; lv < t->levels; lv++) {
            size_t n = read_level(f, t->layout, (const uint8_t*)(uintptr_t)t->addr, layer, lv, tmp);
            size_t off = (all.size() + 15) & ~(size_t)15;
            all.resize(off + n);
            memcpy(all.data() + off, tmp.data(), n);
            const LevelLayout& l = t->layout.levels[lv];
            VkBufferImageCopy r{};
            r.bufferOffset = off;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1};
            r.imageExtent = {l.w, l.h, 1};
            regions.push_back(r);
        }
    Staging st = staging_alloc(all.size());
    memcpy(st.ptr, all.data(), all.size());
    VkImage img = t->image;
    defer([st, img, regions](VkCommandBuffer cb) {
        vk::vkCmdCopyBufferToImage(cb, st.buf, img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
        staging_free(st);
    });
}

uint64_t view_key(uint32_t fmt, uint32_t bl, uint32_t nl, uint32_t blay, uint32_t swz, uint32_t mode) {
    return (uint64_t)fmt | (uint64_t)bl << 8 | (uint64_t)nl << 13 | (uint64_t)blay << 18 | (uint64_t)swz << 30 | (uint64_t)mode << 46;
}

VkComponentSwizzle swz_of(uint32_t s) {
    switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_ZERO;
    case 1: return VK_COMPONENT_SWIZZLE_ONE;
    case 2: return VK_COMPONENT_SWIZZLE_R;
    case 3: return VK_COMPONENT_SWIZZLE_G;
    case 4: return VK_COMPONENT_SWIZZLE_B;
    case 5: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

// the image view NVN means by (texture, view); render targets select one level and layer
// `array`: a 2D-array view of all the view's layers (set-0 binding 4) instead of a single layer
VkImageView get_view(Texture* t, const GView* v, bool rt, uint32_t* out_level = nullptr, bool array = false) {
    uint32_t fmt = v && (v->set & V_FORMAT) ? v->format : t->nvn_format;
    const FormatInfo& vf = format_info(fmt);
    if (vf.image == VK_FORMAT_UNDEFINED || image_bytes_per_block(vf) != image_bytes_per_block(*t->fi) ||
        vf.aspect != t->fi->aspect || vf.bw != t->fi->bw)
        fmt = t->nvn_format;                       // incompatible reinterpretation: keep the image format
    uint32_t bl = v && (v->set & V_LEVELS) ? v->base_level : 0;
    bl = std::min(bl, t->levels - 1);
    uint32_t nl = v && (v->set & V_LEVELS) && v->num_levels ? v->num_levels : t->levels - bl;
    if (rt) nl = 1;
    nl = std::max(1u, std::min(nl, t->levels - bl));
    uint32_t blay = v && (v->set & V_LAYERS) ? std::min<uint32_t>(v->base_layer, t->layers - 1) : 0;
    uint32_t swz = v && (v->set & V_SWIZZLE) && !rt ? (uint32_t)v->swz[0] | v->swz[1] << 4 | v->swz[2] << 8 | v->swz[3] << 12 : 0xFFFF;
    uint32_t mode = (v && (v->set & V_DSMODE) && v->dsmode == 1) ? 1 : 0;
    if (out_level) *out_level = bl;
    uint32_t nlay = 1;
    if (array) nlay = v && (v->set & V_LAYERS) && v->num_layers ? std::min<uint32_t>(v->num_layers, t->layers - blay) : t->layers - blay;
    uint64_t key = view_key(fmt, bl, nl, blay, swz, mode | (rt ? 2 : 0) | (array ? 4 : 0));
    std::lock_guard<std::mutex> lk(t->lock);
    auto it = t->views.find(key);
    if (it != t->views.end()) return it->second;
    const FormatInfo& ff = format_info(fmt);
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = t->image;
    ci.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : t->target == 2 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    ci.format = ff.image;
    if (swz != 0xFFFF)
        ci.components = {swz_of(swz & 15), swz_of((swz >> 4) & 15), swz_of((swz >> 8) & 15), swz_of((swz >> 12) & 15)};
    if (!rt && ff.force_alpha_one) ci.components.a = VK_COMPONENT_SWIZZLE_ONE;
    VkImageAspectFlags asp = t->aspect;
    if (!rt && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) asp = mode ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    ci.subresourceRange = {asp, bl, nl, blay, std::max(1u, nlay)};
    VkImageView iv;
    VK_CHECK(vk::vkCreateImageView(g.dev, &ci, nullptr, &iv));
    t->views[key] = iv;
    return iv;
}

Texture* create_texture(const GTexB& b) {
    auto* t = new Texture;
    t->nvn_format = b.format;
    t->fi = &format_info(b.format);
    if (t->fi->image == VK_FORMAT_UNDEFINED) {
        log("gpu: texture format 0x%x (%s) has no Vulkan equivalent; using RGBA8", b.format, t->fi->name);
        t->nvn_format = 0x25;
        t->fi = &format_info(0x25);
    }
    t->w = std::max(1u, b.w);
    t->h = std::max(1u, b.h);
    t->target = b.target;
    bool array = b.target == 3 || b.target == 4 || b.target == 6;
    bool cube = b.target == 8 || b.target == 9;
    bool is3d = b.target == 2;
    t->d = is3d ? std::max(1u, b.d) : 1;
    t->layers = array ? std::max(1u, b.d) : cube ? 6 : 1;
    if (b.target == 9) t->layers = std::max(1u, b.d) * 6;
    t->levels = std::max(1u, b.levels);
    t->samples = std::max(1u, b.samples);
    t->flags = b.flags;
    t->aspect = aspect_of(*t->fi);
    GPool* pool = G<GPool>(b.pool);
    t->addr = (pool ? pool->mem : 0) + b.offset;
    t->cpu_visible = pool && !(pool->flags & 1);
    uint32_t bh = b.layout_set ? (b.layout[0] & 7) : default_block_height_log2((t->h + t->fi->bh - 1) / t->fi->bh);
    t->layout = compute_layout(*t->fi, t->w, t->h, t->d, t->levels, t->layers, bh, (b.flags & 0x10) != 0, b.stride);

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = t->fi->image;
    ci.extent = {t->w, t->h, t->d};
    ci.mipLevels = t->levels;
    ci.arrayLayers = t->layers;
    ci.samples = (VkSampleCountFlagBits)(t->samples >= 8 ? 8 : t->samples >= 4 ? 4 : t->samples >= 2 ? 2 : 1);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(ci.format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (t->aspect == VK_IMAGE_ASPECT_COLOR_BIT) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (cube && t->w == t->h && t->layers % 6 == 0) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vk::vkCreateImage(g.dev, &ci, nullptr, &t->image));
    VkMemoryRequirements req;
    vk::vkGetImageMemoryRequirements(g.dev, t->image, &req);
    t->mem = alloc_memory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vk::vkBindImageMemory(g.dev, t->image, t->mem.mem, t->mem.offset));
    VkImage img = t->image;
    VkImageAspectFlags asp = t->aspect;
    defer([img, asp](VkCommandBuffer cb) { transition_general(cb, img, asp); });
    // textures start from their guest storage: packaged textures are loaded into pools before the
    // pool exists (CPU_NO_ACCESS pools included); render targets (DISPLAY, COMPRESSIBLE) are skipped
    bool up = pool && !(b.flags & (0x1 | 0x8));
    static std::atomic<int> nlog{0};
    bool movie_tex = ((t->w == 1920 && t->h == 1080) || (t->w == 960 && t->h == 540)) && t->fi->bpb <= 2;
    if (getenv("LETSGO_TEX_LOG") && (nlog++ < 400 || movie_tex))
        log("gpu: texture %p %ux%ux%u %s target %u levels %u flags 0x%x pool flags 0x%x addr 0x%llx upload %d", (void*)t, t->w, t->h,
            t->layers, t->fi->name, b.target, t->levels, b.flags, pool ? pool->flags : 0, (unsigned long long)t->addr, up);
    if (up) {
        upload_from_guest(t);
        if (t->cpu_visible && t->layout.total) {
            rt::mem_written(t->addr, t->layout.total);         // the upload covers what was written so far
            t->watched = true;
            std::lock_guard<std::mutex> lk(g_watch_lock);
            g_watched.push_back(t);
        }
    }
    return t;
}

// texture pool id -> texture, for debug output
std::mutex g_tex_ids_lock;
std::unordered_map<uint32_t, Texture*> g_tex_ids;

void refresh_watched() {
    std::lock_guard<std::mutex> lk(g_watch_lock);
    for (Texture* t : g_watched)
        if (rt::mem_written(t->addr, t->layout.total)) upload_from_guest(t);
}

void destroy_texture(Texture* t) {
    {
        std::lock_guard<std::mutex> lk(g_tex_ids_lock);
        for (auto it = g_tex_ids.begin(); it != g_tex_ids.end();) it = it->second == t ? g_tex_ids.erase(it) : std::next(it);
    }
    if (t->watched) {
        std::lock_guard<std::mutex> lk(g_watch_lock);
        g_watched.erase(std::remove(g_watched.begin(), g_watched.end(), t), g_watched.end());
    }
    // its first layout change and uploads may still be waiting in the deferred work, which goes into the next command buffer (after
    // the retirements that are due): it goes once that command buffer is done, never before (a texture made and dropped between two
    // command buffers was destroyed and then copied into, which crashed the driver)
    defer([t](VkCommandBuffer) {
        retire(g.submitted + 1, [t] {
            for (auto& [k, v] : t->views) vk::vkDestroyImageView(g.dev, v, nullptr);
            vk::vkDestroyImage(g.dev, t->image, nullptr);
            free_memory(t->mem);
            delete t;
        });
    });
}

// ---- samplers ----------------------------------------------------------------------------------------
VkSamplerAddressMode wrap_of(uint32_t w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
VkCompareOp cmp_of(uint32_t f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }

VkSampler make_sampler(const GSmpB& b) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = b.mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    bool min_lin = b.min == 1 || b.min == 3 || b.min == 5;
    ci.minFilter = min_lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (b.min == 4 || b.min == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_of(b.wrap[0]);
    ci.addressModeV = wrap_of(b.wrap[1]);
    ci.addressModeW = wrap_of(b.wrap[2]);
    ci.mipLodBias = b.lod_bias;
    ci.anisotropyEnable = b.aniso > 1.0f ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = std::min(b.aniso, g.props.limits.maxSamplerAnisotropy);
    ci.compareEnable = b.cmp_mode == 1;
    ci.compareOp = cmp_of(b.cmp_func);
    ci.minLod = b.lod_min;
    ci.maxLod = b.min <= 1 ? std::min(b.lod_min + 0.25f, b.lod_max) : b.lod_max;   // no mipmapping: base level only
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    bool white = b.border[0] > 0.5f && b.border[1] > 0.5f && b.border[2] > 0.5f;
    ci.borderColor = b.border[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                     : white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSamplerReductionModeCreateInfo rm{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    if (b.reduction) {
        rm.reductionMode = b.reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        ci.pNext = &rm;
    }
    VkSampler s;
    VK_CHECK(vk::vkCreateSampler(g.dev, &ci, nullptr, &s));
    return s;
}

void write_texture_descriptor(uint32_t id, Texture* t, const GView* v) {
    if (id >= kMaxTextures) {
        log("gpu: texture id %u beyond the bindless table", id);
        return;
    }
    VkImageView iv = get_view(t, v, false);
    uint32_t fmt = v && (v->set & V_FORMAT) ? v->format : t->nvn_format;
    int kind = format_info(fmt).kind;
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, iv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ai{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    // binding 0 always (size queries use it); the integer views also go to their typed table, and
    // 2D textures (array or not) to the array table
    int n = 0;
    auto add = [&](uint32_t binding, VkDescriptorImageInfo* info) {
        VkWriteDescriptorSet& x = w[n++];
        x.dstSet = g.set0;
        x.dstBinding = binding;
        x.dstArrayElement = id;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        x.pImageInfo = info;
    };
    add(0, &ii);
    if (kind) add(kind == 1 ? 1 : 2, &ii);
    if (t->target != 2 && t->samples <= 1) {
        ai.imageView = get_view(t, v, false, nullptr, true);
        add(4, &ai);
    }
    std::lock_guard<std::mutex> lk(g_desc_lock);
    vk::vkUpdateDescriptorSets(g.dev, (uint32_t)n, w, 0, nullptr);
}

void write_sampler_descriptor(uint32_t id, VkSampler s) {
    if (id >= kMaxSamplers || !s) return;
    VkDescriptorImageInfo ii{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = g.set0;
    w.dstBinding = 3;
    w.dstArrayElement = id;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    std::lock_guard<std::mutex> lk(g_desc_lock);
    vk::vkUpdateDescriptorSets(g.dev, 1, &w, 0, nullptr);
}

// ---- programs ----------------------------------------------------------------------------------------
// The shader objects belong to shaders.cpp, shared by every program with the same code
struct StageShader {
    bool present = false;
    bool maxwell = false;                                 // translated from GPU code (no assembly)
    nvasm::Reflection refl;
    std::shared_ptr<ShaderSource> src;
    std::string glsl;                                     // the variant last used (kept for frame dumps)
    std::unordered_map<uint32_t, VkShaderEXT> variants;   // vertex: by the integer inputs' signedness
};
struct Program {
    StageShader st[3];                                    // vertex, fragment, compute
    std::mutex lock;
};
std::atomic<int> g_programs;
extern uint64_t g_dump_frame;

// `types`: bit i = vertex attribute i has an unsigned-integer format, bit 16 + i a signed one.
// The variant's key: assembly programs: the integer inputs with a signed format; GPU-code
// programs: the inputs with an unsigned (bits 0..15) or signed (bits 16..31) integer format
VkShaderEXT shader_variant(Program* p, int stage, uint32_t types) {
    StageShader& s = p->st[stage];
    if (!s.present) return VK_NULL_HANDLE;
    uint32_t key = s.maxwell ? types & (s.refl.inputs & 0xFFFF) * 0x10001u : (types >> 16) & s.refl.int_inputs;
    std::lock_guard<std::mutex> lk(p->lock);
    auto it = s.variants.find(key);
    if (it != s.variants.end()) return it->second;
    auto b = shader_build(s.src, key);
    VkShaderEXT sh = shader_object(*b);                   // usually built by now (ProgramSetShaders queued it)
    if (g_dump_frame != ~0ull) s.glsl = shader_glsl(*b);
    s.variants[key] = sh;
    return sh;
}

// ---- command recording -------------------------------------------------------------------------------
struct Exec;
using Op = std::function<void(Exec&)>;
using OpList = std::vector<Op>;
struct Recorder {
    std::shared_ptr<OpList> cur;
    bool recording = false;
};
std::mutex g_handle_lock;
std::unordered_map<uint64_t, std::shared_ptr<OpList>> g_handles;
uint64_t g_next_handle = 0x1000;

Recorder* rec(Ctx* c) { return *G<Recorder*>(c->x[0]); }
void record(Ctx* c, Op op) {
    Recorder* r = rec(c);
    if (!r || !r->recording) return;
    r->cur->push_back(std::move(op));
}

// ---- execution state ---------------------------------------------------------------------------------
struct Bound {
    uint64_t addr = 0, size = 0;
};
struct RT {
    Texture* tex = nullptr;
    GView view{};
};
struct Exec {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    // render targets, and the open dynamic-rendering scope
    RT color[8];
    int ncolor = 0;
    RT depth;
    bool rt_dirty = true, rendering = false;
    uint32_t rt_w = 1, rt_h = 1;
    VkSampleCountFlagBits rt_samples = VK_SAMPLE_COUNT_1_BIT;
    int active_colors = 0;
    // program and resources
    // programs bound per stage (NVN programs are separable: a fragment-only program can pair with
    // another program's vertex stage)
    Program* vprog = nullptr;
    Program* fprog = nullptr;
    Program* cprog = nullptr;
    VkShaderEXT bound_vs = VK_NULL_HANDLE, bound_fs = VK_NULL_HANDLE;
    bool set0_bound = false;
    Bound ubo[6][16];
    Bound ssbo[6][16];
    uint64_t tex[6][32] = {};
    Bound vb[16];
    GAttrib attrib[16] = {};
    int nattrib = 0;
    GStream stream[16] = {};
    int nstream = 0;
    // fixed function
    GBlend blend[8];
    GColor color_state{0, 3, 8, 0};
    GChannel channel{0};
    GDepth ds{2, 2, 0x88, 0x11, 0x01, 0x11, 0x01, 0};
    GMs ms{0, 1, 0, 0};
    GPoly poly{0, 1, 2, 0};
    uint32_t sample_mask = ~0u;
    float vp[16][4] = {};
    float dr[16][2] = {};
    int32_t sc[16][4] = {};
    int nvp = 1, nsc = 1;
    float blend_color[4] = {0, 0, 0, 0};
    uint32_t stencil_ref[2] = {0, 0}, stencil_wmask[2] = {0xFF, 0xFF}, stencil_vmask[2] = {0xFF, 0xFF};
    float line_width = 1;
    bool depth_bounds_en = false;
    float depth_bounds[2] = {0, 1};
    bool depth_clamp = false, raster_discard = false;
    float poly_offset[3] = {0, 0, 0};

    Exec() {
        for (auto& b : blend) b = GBlend{0, 2, 1, 2, 1, 1, 1, 0};
        for (int i = 0; i < 16; i++) {
            dr[i][0] = 0;
            dr[i][1] = 1;
            sc[i][2] = sc[i][3] = 0x7FFFFFFF;
        }
    }
    void new_cb(VkCommandBuffer c) {
        cb = c;
        rendering = false;
        rt_dirty = true;
        set0_bound = false;
        bound_vs = bound_fs = VK_NULL_HANDLE;
    }
    void end_rendering() {
        if (rendering) vk::vkCmdEndRendering(cb);
        rendering = false;
        rt_dirty = true;
    }
    void ensure_rendering();
    void draw_setup(uint32_t topology_nvn);
    void set_dynamic_state(uint32_t topology_nvn);
    void push_descriptors(int stage, const nvasm::Reflection& r);
    uint32_t attrib_types() const;
    void set_vertex_input(const nvasm::Reflection& vr);
};

Exec g_exec;

void Exec::ensure_rendering() {
    if (rendering && !rt_dirty) return;
    end_rendering();
    VkRenderingAttachmentInfo ca[8];
    uint32_t w = 0x7FFFFFFF, h = 0x7FFFFFFF;
    int n = 0;
    rt_samples = VK_SAMPLE_COUNT_1_BIT;
    for (int i = 0; i < ncolor; i++) {
        ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (Texture* t = color[i].tex) {
            uint32_t lv = 0;
            ca[i].imageView = get_view(t, &color[i].view, true, &lv);
            w = std::min(w, std::max(1u, t->w >> lv));
            h = std::min(h, std::max(1u, t->h >> lv));
            rt_samples = (VkSampleCountFlagBits)std::max<uint32_t>(1, t->samples);
        }
        n = i + 1;
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool has_depth = false, has_stencil = false;
    if (Texture* t = depth.tex) {
        uint32_t lv = 0;
        VkImageView iv = get_view(t, &depth.view, true, &lv);
        w = std::min(w, std::max(1u, t->w >> lv));
        h = std::min(h, std::max(1u, t->h >> lv));
        rt_samples = (VkSampleCountFlagBits)std::max<uint32_t>(1, t->samples);
        for (auto* a : {&da, &sa}) {
            a->imageView = iv;
            a->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            a->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        has_depth = (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        has_stencil = (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    }
    if (w == 0x7FFFFFFF) {
        w = 1;
        h = 1;
    }
    rt_w = w;
    rt_h = h;
    active_colors = n;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = (uint32_t)n;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = has_depth ? &da : nullptr;
    ri.pStencilAttachment = has_stencil ? &sa : nullptr;
    vk::vkCmdBeginRendering(cb, &ri);
    rendering = true;
    rt_dirty = false;
}

VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
VkPrimitiveTopology topology_of(uint32_t p) {
    switch (p) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

void Exec::set_dynamic_state(uint32_t prim) {
    int n = std::max(nvp, nsc);
    VkViewport vps[16];
    VkRect2D scs[16];
    for (int i = 0; i < n; i++) {
        vps[i] = {vp[i][0], vp[i][1], vp[i][2], vp[i][3], dr[i][0], dr[i][1]};
        if (vps[i].width <= 0) vps[i].width = (float)rt_w;
        if (vps[i].height == 0) vps[i].height = (float)rt_h;
        int64_t x0 = std::max<int64_t>(0, sc[i][0]), y0 = std::max<int64_t>(0, sc[i][1]);
        int64_t x1 = std::min<int64_t>((int64_t)sc[i][0] + sc[i][2], rt_w), y1 = std::min<int64_t>((int64_t)sc[i][1] + sc[i][3], rt_h);
        scs[i] = {{(int32_t)std::min<int64_t>(x0, rt_w), (int32_t)std::min<int64_t>(y0, rt_h)},
                  {(uint32_t)std::max<int64_t>(0, x1 - x0), (uint32_t)std::max<int64_t>(0, y1 - y0)}};
    }
    vk::vkCmdSetViewportWithCount(cb, (uint32_t)n, vps);
    vk::vkCmdSetScissorWithCount(cb, (uint32_t)n, scs);
    vk::vkCmdSetRasterizerDiscardEnable(cb, raster_discard);
    vk::vkCmdSetCullMode(cb, (VkCullModeFlags)(poly.cull & 3));
    // NVN's lower-left origin is kept in Vulkan's y-down framebuffer (same rows in memory), which
    // flips the sign of a triangle's area: NVN's counter-clockwise is Vulkan's clockwise
    vk::vkCmdSetFrontFace(cb, poly.front == 0 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    VkPolygonMode pm = poly.mode == 0 ? VK_POLYGON_MODE_POINT : poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    vk::vkCmdSetPolygonModeEXT(cb, pm);
    vk::vkCmdSetRasterizationSamplesEXT(cb, rt_samples);
    VkSampleMask smask = sample_mask;
    vk::vkCmdSetSampleMaskEXT(cb, rt_samples, &smask);
    vk::vkCmdSetAlphaToCoverageEnableEXT(cb, ms.a2c);
    bool dtest = ds.flags & 1, dwrite = (ds.flags & 2) != 0, stest = (ds.flags & 4) != 0;
    bool has_depth = depth.tex && (depth.tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    bool has_stencil = depth.tex && (depth.tex->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    vk::vkCmdSetDepthTestEnable(cb, dtest && has_depth);
    vk::vkCmdSetDepthWriteEnable(cb, dwrite && has_depth);
    vk::vkCmdSetDepthCompareOp(cb, cmp_of(ds.func));
    vk::vkCmdSetDepthBoundsTestEnable(cb, depth_bounds_en && has_depth);
    vk::vkCmdSetDepthBounds(cb, depth_bounds[0], depth_bounds[1]);
    uint32_t offset_bit = poly.mode == 0 ? 1 : poly.mode == 1 ? 2 : 4;
    bool bias = (poly.offset & offset_bit) != 0;
    vk::vkCmdSetDepthBiasEnable(cb, bias);
    vk::vkCmdSetDepthBias(cb, poly_offset[1], poly_offset[2], poly_offset[0]);
    vk::vkCmdSetDepthClampEnableEXT(cb, depth_clamp);
    vk::vkCmdSetStencilTestEnable(cb, stest && has_stencil);
    vk::vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, (VkStencilOp)((ds.sop_f0 & 15) - 1), (VkStencilOp)((ds.sop_f1 & 15) - 1),
                          (VkStencilOp)((ds.sop_f0 >> 4) - 1), cmp_of(ds.sfunc & 15));
    vk::vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, (VkStencilOp)((ds.sop_b0 & 15) - 1), (VkStencilOp)((ds.sop_b1 & 15) - 1),
                          (VkStencilOp)((ds.sop_b0 >> 4) - 1), cmp_of(ds.sfunc >> 4));
    vk::vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_vmask[0]);
    vk::vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, stencil_vmask[1]);
    vk::vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_wmask[0]);
    vk::vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, stencil_wmask[1]);
    vk::vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_ref[0]);
    vk::vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, stencil_ref[1]);
    vk::vkCmdSetPrimitiveTopology(cb, topology_of(prim));
    vk::vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vk::vkCmdSetLineWidth(cb, std::max(1.0f, std::min(line_width, g.props.limits.lineWidthRange[1])));
    vk::vkCmdSetBlendConstants(cb, blend_color);
    vk::vkCmdSetLogicOpEnableEXT(cb, VK_FALSE);
    if (active_colors) {
        VkBool32 en[8];
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8];
        for (int i = 0; i < active_colors; i++) {
            en[i] = (color_state.blend_mask >> i) & 1;
            const GBlend& b = blend[i];
            eq[i] = {blend_factor(b.src), blend_factor(b.dst), blend_op(b.eq), blend_factor(b.srca), blend_factor(b.dsta), blend_op(b.eqa)};
            wm[i] = (~(channel.disabled >> (4 * i))) & 15;
            if (!color[i].tex) wm[i] = 0;
        }
        vk::vkCmdSetColorBlendEnableEXT(cb, 0, (uint32_t)active_colors, en);
        vk::vkCmdSetColorBlendEquationEXT(cb, 0, (uint32_t)active_colors, eq);
        vk::vkCmdSetColorWriteMaskEXT(cb, 0, (uint32_t)active_colors, wm);
    }
}

void Exec::push_descriptors(int stage, const nvasm::Reflection& r) {
    VkDescriptorBufferInfo bi[16];
    VkWriteDescriptorSet w[16];
    int n = 0;
    int base = nvasm::kStageBase[stage];
    for (int k = 0; k < 16; k++) {
        if (!(r.cbufs & (1u << k))) continue;
        if (k == 14) {
            Scratch s = scratch(256, g.props.limits.minUniformBufferOffsetAlignment);
            memcpy(s.ptr, tex[stage], 256);
            bi[n] = {s.buf, s.offset, 256};
        } else if (k == 15) {
            // NVN driver data (GPU-code programs): storage buffer i's device address and size at 0x110 + 16i
            Scratch s = scratch(0x210, g.props.limits.minUniformBufferOffsetAlignment);
            memset(s.ptr, 0, 0x210);
            for (int i = 0; i < 16; i++) {
                uint64_t addr = zero_address(), size = 65536;
                const Bound& b = ssbo[stage][i];
                const HostBuffer* hb = b.addr ? host_buffer(b.addr, std::max<uint64_t>(b.size, 1)) : nullptr;
                if (hb && hb->gpu) {
                    addr = hb->gpu + (b.addr - hb->base);
                    size = b.size;
                }
                uint32_t size32 = (uint32_t)std::min<uint64_t>(size, 0xFFFFFFFFu);
                memcpy(s.ptr + 0x110 + 16 * i, &addr, 8);
                memcpy(s.ptr + 0x118 + 16 * i, &size32, 4);
            }
            bi[n] = {s.buf, s.offset, 0x210};
        } else {
            const Bound& b = ubo[stage][k];
            const HostBuffer* hb = b.addr ? host_buffer(b.addr, std::max<uint64_t>(b.size, 1)) : nullptr;
            if (hb) bi[n] = {hb->buf, b.addr - hb->base, std::min<uint64_t>(std::max<uint64_t>(b.size, 16), 65536)};
            else bi[n] = {zero_buffer(), 0, 65536};
        }
        w[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstBinding = (uint32_t)(base + k);
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vk::vkCmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 1, (uint32_t)n, w);
}

void Exec::set_vertex_input(const nvasm::Reflection& vr) {
    VkVertexInputBindingDescription2EXT bd[17];
    VkVertexInputAttributeDescription2EXT ad[32];
    uint32_t nb = 0, na = 0;
    uint32_t used_streams = 0;
    bool need_zero = false;
    for (int i = 0; i < 32; i++) {
        if (!(vr.inputs & (1u << i))) continue;
        ad[na] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        ad[na].location = (uint32_t)i;
        const FormatInfo* f = i < nattrib && attrib[i].format ? &format_info(attrib[i].format) : nullptr;
        if (f && f->vertex != VK_FORMAT_UNDEFINED && attrib[i].stream < 16) {
            ad[na].binding = attrib[i].stream;
            ad[na].format = f->vertex;
            ad[na].offset = attrib[i].offset;
            used_streams |= 1u << attrib[i].stream;
        } else {
            if (f) {
                // a format with no Vulkan vertex equivalent reads as zeros: say so once per format
                static std::mutex lk;
                static std::set<uint32_t> told;
                std::lock_guard<std::mutex> g(lk);
                if (told.insert(attrib[i].format).second)
                    log("gpu: vertex attribute %d: format 0x%x (%s) has no Vulkan vertex format, reads as zero", i,
                        (unsigned)attrib[i].format, f->name);
            }
            ad[na].binding = 16;
            ad[na].format = (vr.int_inputs & (1u << i)) ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            need_zero = true;
        }
        na++;
    }
    for (uint32_t s = 0; s < 16; s++) {
        if (!(used_streams & (1u << s))) continue;
        bd[nb] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        bd[nb].binding = s;
        bd[nb].stride = s < (uint32_t)nstream ? stream[s].stride : 0;
        bd[nb].inputRate = s < (uint32_t)nstream && stream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        bd[nb].divisor = s < (uint32_t)nstream && stream[s].divisor ? stream[s].divisor : 1;
        nb++;
        const Bound& b = vb[s];
        const HostBuffer* hb = b.addr ? host_buffer(b.addr, 1) : nullptr;
        VkBuffer buf = hb ? hb->buf : zero_buffer();
        VkDeviceSize off = hb ? b.addr - hb->base : 0;
        VkDeviceSize size = hb ? std::min<uint64_t>(b.size ? b.size : VK_WHOLE_SIZE, hb->base + hb->size - b.addr) : VK_WHOLE_SIZE;
        vk::vkCmdBindVertexBuffers2(cb, s, 1, &buf, &off, &size, nullptr);
    }
    if (need_zero) {
        bd[nb] = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        bd[nb].binding = 16;
        bd[nb].stride = 0;
        bd[nb].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bd[nb].divisor = 1;
        nb++;
        VkBuffer zb = zero_buffer();
        VkDeviceSize zo = 0;
        vk::vkCmdBindVertexBuffers2(cb, 16, 1, &zb, &zo, nullptr, nullptr);
    }
    vk::vkCmdSetVertexInputEXT(cb, nb, bd, na, ad);
}

std::atomic<int> g_draw_skips;

extern uint64_t g_dump_frame;              // frame dump (LETSGO_DUMP_FRAME), defined below
int dump_note_draw(Texture* t);
void dump_note(const std::string& line, Program* p);
extern std::vector<std::string> g_dump_lines, g_dump_data;
extern std::vector<uint32_t> g_dump_tex_ids;

void Exec::draw_setup(uint32_t prim) {
    ensure_rendering();
    if (g_dump_frame != ~0ull && rt::nvn_frames() == g_dump_frame) {
        char b[512];
        int rt = ncolor && color[0].tex ? dump_note_draw(color[0].tex) : -1;
        for (int i = 1; i < ncolor; i++) dump_note_draw(color[i].tex);
        std::string line;
        snprintf(b, sizeof b, "rt%d prog %p/%p (%s/%s) prim %u vp (%g %g %g %g) sc (%d %d %d %d) blend %d [%u %u %u / %u %u %u] mask %x",
                 rt, (void*)vprog, (void*)fprog, vprog && vprog->st[0].maxwell ? "gpu-code" : "asm",
                 fprog && fprog->st[1].maxwell ? "gpu-code" : "asm",
                 prim, vp[0][0], vp[0][1], vp[0][2], vp[0][3], sc[0][0], sc[0][1], sc[0][2], sc[0][3], color_state.blend_mask & 1,
                 blend[0].src, blend[0].dst, blend[0].eq, blend[0].srca, blend[0].dsta, blend[0].eqa, (~channel.disabled) & 15);
        line = b;
        snprintf(b, sizeof b, " cull %u front %u depth %x/%u", poly.cull, poly.front, ds.flags, ds.func);
        line += b;
        // vertex inputs the program reads: format, stream, offset (stride)
        for (int i = 0; vprog && i < 32; i++) {
            if (!(vprog->st[0].refl.inputs & (1u << i))) continue;
            if (i < nattrib && attrib[i].format) {
                const GAttrib& a = attrib[i];
                snprintf(b, sizeof b, " a%d=%s@s%u+%u/%u", i, format_info(a.format).name, a.stream, a.offset,
                         a.stream < (uint32_t)nstream ? stream[a.stream].stride : 0);
            } else {
                snprintf(b, sizeof b, " a%d=none", i);
            }
            line += b;
        }
        for (int i = 0; i < 32; i++) {
            if (!tex[1][i]) continue;
            uint32_t id = (uint32_t)tex[1][i] & 0xFFFFF;
            if (std::find(g_dump_tex_ids.begin(), g_dump_tex_ids.end(), id) == g_dump_tex_ids.end()) g_dump_tex_ids.push_back(id);
            Texture* t = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_tex_ids_lock);
                auto it = g_tex_ids.find(id);
                if (it != g_tex_ids.end()) t = it->second;
            }
            if (t) snprintf(b, sizeof b, " t%d=%u:%ux%ux%u/%s", i, id, t->w, t->h, t->layers, t->fi->name);
            else snprintf(b, sizeof b, " t%d=%u:?", i, id);
            line += b;
        }
        dump_note(line, vprog);
        if (fprog != vprog) dump_note("", fprog);
        // the data the vertex stage reads: its first uniform buffers (as floats) and the first vertex
        std::string d = "draw " + std::to_string(g_dump_lines.size() - 1) + "\n";
        for (int k = 0; k < 3; k++) {
            const Bound& u = ubo[0][k];
            if (!u.addr) continue;
            snprintf(b, sizeof b, "  vs ubo%d @%llx size %llu:", k, (unsigned long long)u.addr, (unsigned long long)u.size);
            d += b;
            uint64_t n = std::min<uint64_t>(u.size / 4, k == 0 ? 96 : 48);
            for (uint64_t j = 0; j < n; j++) {
                if (j % 4 == 0) d += j % 16 == 0 ? "\n    " : " |";
                snprintf(b, sizeof b, " %g", (double)((const float*)(uintptr_t)u.addr)[j]);
                d += b;
            }
            d += "\n";
        }
        if (vb[0].addr && nstream > 0) {
            snprintf(b, sizeof b, "  vertex 0 @%llx stride %u:", (unsigned long long)vb[0].addr, stream[0].stride);
            d += b;
            const uint8_t* v = (const uint8_t*)(uintptr_t)vb[0].addr;
            for (uint32_t j = 0; j < std::min<uint32_t>(stream[0].stride, 64); j++) {
                snprintf(b, sizeof b, "%s%02x", j % 4 ? "" : " ", v[j]);
                d += b;
            }
            d += "\n";
        }
        g_dump_data.push_back(d);
    }
    if (!set0_bound) {
        vk::vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set0, 0, nullptr);
        set0_bound = true;
    }
    VkShaderEXT vs = vprog ? shader_variant(vprog, 0, attrib_types()) : VK_NULL_HANDLE;
    VkShaderEXT fs = fprog ? shader_variant(fprog, 1, 0) : VK_NULL_HANDLE;
    if (vs != bound_vs || fs != bound_fs) {
        VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        VkShaderEXT sh[2] = {vs, fs};
        vk::vkCmdBindShadersEXT(cb, 2, stages, sh);
        bound_vs = vs;
        bound_fs = fs;
    }
    set_dynamic_state(prim);
    push_descriptors(0, vprog->st[0].refl);
    if (fprog && fprog->st[1].present) push_descriptors(1, fprog->st[1].refl);
    set_vertex_input(vprog->st[0].refl);
}

uint32_t Exec::attrib_types() const {
    uint32_t t = 0;
    for (int i = 0; i < nattrib && i < 16; i++) {
        if (!attrib[i].format) continue;
        int k = format_info(attrib[i].format).kind;
        if (k == 1) t |= 1u << i;
        else if (k == 2) t |= 1u << (16 + i);
    }
    return t;
}

bool can_draw(Exec& ex) {
    if (ex.vprog && ex.vprog->st[0].present && shader_variant(ex.vprog, 0, ex.attrib_types())) return true;
    if (g_dump_frame != ~0ull && rt::nvn_frames() == g_dump_frame) {
        char b[160];
        snprintf(b, sizeof b, "SKIPPED draw: vertex program %p (vertex stage %s)", (void*)ex.vprog,
                 !ex.vprog ? "-" : ex.vprog->st[0].present ? "failed to build" : "missing");
        dump_note(b, nullptr);
    }
    if (g_draw_skips++ < 10)
        log("gpu: draw skipped (no usable vertex program: program %p, vertex stage %s, fragment stage %s, attribute types 0x%x)",
            (void*)ex.vprog, !ex.vprog ? "-" : !ex.vprog->st[0].present ? "missing" : "failed to build",
            !ex.fprog ? "-" : ex.fprog->st[1].present ? "present" : "missing", ex.attrib_types());
    return false;
}

VkIndexType index_type(uint32_t t) { return t == 0 ? VK_INDEX_TYPE_UINT8 : t == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32; }

void draw_elements(Exec& ex, uint32_t prim, uint32_t type, int count, uint64_t addr, int base_vertex, int base_instance,
                   int instances) {
    if (!can_draw(ex) || count <= 0 || instances <= 0) return;
    const HostBuffer* hb = host_buffer(addr, 1);
    if (!hb) {
        if (g_draw_skips++ < 10) log("gpu: index buffer 0x%llx is not in a memory pool", (unsigned long long)addr);
        return;
    }
    ex.draw_setup(prim);
    vk::vkCmdBindIndexBuffer(ex.cb, hb->buf, addr - hb->base, index_type(type));
    vk::vkCmdDrawIndexed(ex.cb, (uint32_t)count, (uint32_t)instances, 0, base_vertex, (uint32_t)base_instance);
}

void draw_arrays(Exec& ex, uint32_t prim, int first, int count, int base_instance, int instances) {
    if (!can_draw(ex) || count <= 0 || instances <= 0) return;
    ex.draw_setup(prim);
    vk::vkCmdDraw(ex.cb, (uint32_t)count, (uint32_t)instances, (uint32_t)first, (uint32_t)base_instance);
}

// clear rectangle: the scissor of viewport 0 inside the render area
VkClearRect clear_rect(Exec& ex) {
    int64_t x0 = std::max<int64_t>(0, ex.sc[0][0]), y0 = std::max<int64_t>(0, ex.sc[0][1]);
    int64_t x1 = std::min<int64_t>((int64_t)ex.sc[0][0] + ex.sc[0][2], ex.rt_w), y1 = std::min<int64_t>((int64_t)ex.sc[0][1] + ex.sc[0][3], ex.rt_h);
    VkClearRect r{};
    r.rect = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)std::max<int64_t>(0, x1 - x0), (uint32_t)std::max<int64_t>(0, y1 - y0)}};
    r.layerCount = 1;
    return r;
}

// read level 0, layer 0 of `t` back as 8-bit RGBA in display order (rows flipped); float formats are
// shown through the sRGB curve. False for formats this debug path doesn't convert.
float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;
    if (e == 0) {
        if (!m) bits = sign;
        else {
            e = 113;
            while (!(m & 1024)) m <<= 1, e--;
            bits = sign | e << 23 | (m & 1023) << 13;
        }
    } else if (e == 31) bits = sign | 0x7F800000u | m << 13;
    else bits = sign | (e + 112) << 23 | m << 13;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
float uf11(uint32_t v, int mbits) {          // unsigned 11/10-bit float: 5-bit exponent
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return std::ldexp((float)m, -14 - mbits);
    if (e == 31) return 65504.0f;
    return std::ldexp(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}
uint8_t to8(float f, bool srgb) {
    if (!(f > 0)) return 0;
    if (f >= 1) return 255;
    if (srgb) f = f <= 0.0031308f ? f * 12.92f : 1.055f * std::pow(f, 1 / 2.4f) - 0.055f;
    return (uint8_t)std::lround(f * 255);
}

bool read_rgba8(Texture* t, std::vector<uint8_t>& rgba) {
    VkFormat f = t->fi->image;
    uint32_t bpp = 0;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: bpp = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
    case VK_FORMAT_R8_UNORM: bpp = 1; break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: bpp = 2; break;
    default: return false;
    }
    if (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT || t->samples > 1) return false;
    uint32_t w = t->w, h = t->h;
    Staging st = staging_alloc((VkDeviceSize)w * h * bpp);
    {
        std::lock_guard<std::recursive_mutex> lk(g.queue_lock);
        Cmd c = begin_cmd();
        full_barrier(c.cb);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vk::vkCmdCopyImageToBuffer(c.cb, t->image, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &r);
        wait_value(submit_cmd(c));
    }
    rgba.assign((size_t)w * h * 4, 255);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = st.ptr + (size_t)(h - 1 - y) * w * bpp;
        uint8_t* dst = rgba.data() + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, src += bpp, dst += 4) {
            uint32_t v32 = 0;
            memcpy(&v32, src, std::min<uint32_t>(bpp, 4));
            switch (f) {
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: memcpy(dst, src, 4); break;
            case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
                dst[0] = src[2], dst[1] = src[1], dst[2] = src[0], dst[3] = src[3];
                break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                dst[0] = (uint8_t)((v32 & 1023) >> 2), dst[1] = (uint8_t)((v32 >> 12) & 255), dst[2] = (uint8_t)((v32 >> 22) & 255);
                dst[3] = (uint8_t)((v32 >> 30) * 85);
                break;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                dst[0] = to8(uf11(v32 & 2047, 6), true), dst[1] = to8(uf11((v32 >> 11) & 2047, 6), true);
                dst[2] = to8(uf11(v32 >> 22, 5), true);
                break;
            case VK_FORMAT_R16G16B16A16_SFLOAT: {
                uint16_t hv[4];
                memcpy(hv, src, 8);
                for (int k = 0; k < 3; k++) dst[k] = to8(half_to_float(hv[k]), true);
                dst[3] = to8(half_to_float(hv[3]), false);
                break;
            }
            case VK_FORMAT_R16G16_SFLOAT: {
                uint16_t hv[2];
                memcpy(hv, src, 4);
                dst[0] = to8(half_to_float(hv[0]), true), dst[1] = to8(half_to_float(hv[1]), true), dst[2] = 0;
                break;
            }
            case VK_FORMAT_R16_SFLOAT: dst[0] = dst[1] = dst[2] = to8(half_to_float((uint16_t)v32), true); break;
            case VK_FORMAT_R32_SFLOAT: {
                float fv;
                memcpy(&fv, src, 4);
                dst[0] = dst[1] = dst[2] = to8(fv, false);
                break;
            }
            case VK_FORMAT_R8_UNORM: dst[0] = dst[1] = dst[2] = src[0]; break;
            case VK_FORMAT_R8G8_UNORM: dst[0] = src[0], dst[1] = src[1], dst[2] = 0; break;
            default: break;
            }
        }
    }
    vk::vkDestroyBuffer(g.dev, st.buf, nullptr);
    free_memory(st.mem);
    return true;
}

// --screenshot: read the presented rectangle back and save a PNG
void capture_if_requested(Texture* t, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    std::string path;
    if (!rt::platform_screenshot_due(path)) return;
    std::vector<uint8_t> rgba;
    if (!read_rgba8(t, rgba)) {
        log("gpu: screenshot of a %s texture is not supported", t->fi->name);
        return;
    }
    // rgba is in display order (row 0 = the texture's last row)
    std::vector<uint8_t> crop((size_t)w * h * 4);
    for (uint32_t r = 0; r < h; r++)
        memcpy(crop.data() + (size_t)r * w * 4, rgba.data() + ((size_t)(t->h - (uint32_t)y - h + r) * t->w + (uint32_t)x) * 4, (size_t)w * 4);
    // writing the PNG takes longer than a frame, so it happens on a thread of its own: the game doesn't slow down while
    // a test takes shots
    std::thread([path, w, h, crop = std::move(crop)] { rt::platform_save_png(path, w, h, crop.data()); }).detach();
}

// LETSGO_DUMP_FRAME=N: save every color render target of frame N (and the presented image) as
// <LETSGO_DUMP_DIR or build/frame>/NN_<w>x<h>_<format>_<draws>draws.png, in first-use order, with
// draws.txt (one line per draw) and the GLSL of the programs used. tS instead of N: the first
// frame that starts S seconds after launch (for runs whose frame numbers vary, like LETSGO_TURBO).
uint64_t g_dump_frame = ~0ull;                         // the next frame to dump
std::vector<uint64_t> g_dump_frames;                   // LETSGO_DUMP_FRAME=N[,M...]
std::vector<double> g_dump_times;                      // LETSGO_DUMP_FRAME=tS[,...]
bool g_dump_multi = false;                             // several dumps: one folder per frame
// at a present: a time trigger that has come turns into the next frame's number
void dump_time_check() {
    if (g_dump_times.empty() || rt::uptime() < g_dump_times.front()) return;
    g_dump_times.erase(g_dump_times.begin());
    g_dump_frames.insert(std::upper_bound(g_dump_frames.begin(), g_dump_frames.end(), rt::nvn_frames() + 1), rt::nvn_frames() + 1);
    g_dump_frame = g_dump_frames.front();
}
std::vector<std::pair<Texture*, int>> g_dump_rts;      // render target, draws into it
std::vector<std::string> g_dump_lines;
std::vector<std::string> g_dump_data;                  // data.txt: per draw, vertex-stage uniform buffers and vertex 0
std::vector<Program*> g_dump_progs;
std::vector<uint32_t> g_dump_tex_ids;                  // textures sampled by the fragment stage

// formats whose colors are sRGB-encoded
bool is_srgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8_SRGB: case VK_FORMAT_B8G8R8_SRGB:
        return true;
    default:
        return false;
    }
}

// level 0, layer `layer` of a block-compressed texture as a .dds file (DX10 header)
uint32_t dxgi_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 71;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return 72;
    case VK_FORMAT_BC2_UNORM_BLOCK: return 74;
    case VK_FORMAT_BC2_SRGB_BLOCK: return 75;
    case VK_FORMAT_BC3_UNORM_BLOCK: return 77;
    case VK_FORMAT_BC3_SRGB_BLOCK: return 78;
    case VK_FORMAT_BC4_UNORM_BLOCK: return 80;
    case VK_FORMAT_BC4_SNORM_BLOCK: return 81;
    case VK_FORMAT_BC5_UNORM_BLOCK: return 83;
    case VK_FORMAT_BC5_SNORM_BLOCK: return 84;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return 95;
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return 96;
    case VK_FORMAT_BC7_UNORM_BLOCK: return 98;
    case VK_FORMAT_BC7_SRGB_BLOCK: return 99;
    default: return 0;
    }
}
bool save_dds(Texture* t, uint32_t layer, const std::string& path) {
    uint32_t dxgi = dxgi_of(t->fi->image);
    if (!dxgi) return false;
    uint32_t bpb = (dxgi == 71 || dxgi == 72 || dxgi == 80 || dxgi == 81) ? 8 : 16;
    uint32_t w = t->w, h = t->h, bw = (w + 3) / 4, bh = (h + 3) / 4;
    VkDeviceSize size = (VkDeviceSize)bw * bh * bpb;
    Staging st = staging_alloc(size);
    {
        std::lock_guard<std::recursive_mutex> lk(g.queue_lock);
        Cmd c = begin_cmd();
        full_barrier(c.cb);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        r.imageExtent = {w, h, 1};
        vk::vkCmdCopyImageToBuffer(c.cb, t->image, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &r);
        wait_value(submit_cmd(c));
    }
    uint32_t hdr[32 + 5] = {};
    hdr[0] = 124;                                   // dwSize
    hdr[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;    // CAPS HEIGHT WIDTH PIXELFORMAT LINEARSIZE
    hdr[2] = h;
    hdr[3] = w;
    hdr[4] = (uint32_t)size;
    hdr[6] = 1;                                     // mip count
    hdr[18] = 32;                                   // pixel format: size
    hdr[19] = 0x4;                                  // FOURCC
    memcpy(&hdr[20], "DX10", 4);
    hdr[26] = 0x1000;                               // caps: TEXTURE
    hdr[31] = dxgi;                                 // DX10 header
    hdr[32] = 3;                                    // TEXTURE2D
    hdr[34] = 1;                                    // array size
    bool ok = false;
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite("DDS ", 1, 4, f);
        fwrite(hdr, 4, 31, f);
        fwrite(&hdr[31], 4, 5, f);
        fwrite(st.ptr, 1, (size_t)size, f);
        fclose(f);
        ok = true;
    }
    vk::vkDestroyBuffer(g.dev, st.buf, nullptr);
    free_memory(st.mem);
    return ok;
}
int dump_note_draw(Texture* t) {
    if (rt::nvn_frames() != g_dump_frame || !t) return -1;
    for (size_t i = 0; i < g_dump_rts.size(); i++)
        if (g_dump_rts[i].first == t) {
            g_dump_rts[i].second++;
            return (int)i;
        }
    g_dump_rts.push_back({t, 1});
    return (int)g_dump_rts.size() - 1;
}
void dump_note(const std::string& line, Program* p) {
    if (!line.empty()) g_dump_lines.push_back(line);
    if (p && std::find(g_dump_progs.begin(), g_dump_progs.end(), p) == g_dump_progs.end()) g_dump_progs.push_back(p);
}
void dump_frame(Texture* presented) {
    if (rt::nvn_frames() != g_dump_frame) return;
    const char* d = getenv("LETSGO_DUMP_DIR");
    std::string dir = d ? d : "build/frame";
    CreateDirectoryA(dir.c_str(), nullptr);
    if (g_dump_multi) {
        dir += "/" + std::to_string(g_dump_frame);
        CreateDirectoryA(dir.c_str(), nullptr);
    }
    bool seen = false;
    for (auto& [t, n] : g_dump_rts) seen |= t == presented;
    if (!seen) g_dump_rts.push_back({presented, 0});
    int k = 0;
    for (auto& [t, n] : g_dump_rts) {
        std::vector<uint8_t> rgba;
        char name[160];
        snprintf(name, sizeof name, "%s/%02d_%ux%u_%s_%ddraws%s.png", dir.c_str(), k++, t->w, t->h, t->fi->name, n,
                 t == presented ? "_presented" : "");
        if (read_rgba8(t, rgba)) rt::platform_save_png(name, t->w, t->h, rgba.data());
        else log("gpu: frame dump: %s texture not converted", t->fi->name);
    }
    for (uint32_t id : g_dump_tex_ids) {
        Texture* t = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_tex_ids_lock);
            auto it = g_tex_ids.find(id);
            if (it != g_tex_ids.end()) t = it->second;
        }
        if (!t) continue;
        char name[160];
        snprintf(name, sizeof name, "%s/tex_%u_%ux%u_%s", dir.c_str(), id, t->w, t->h, t->fi->name);
        std::vector<uint8_t> rgba;
        if (dxgi_of(t->fi->image)) save_dds(t, 0, std::string(name) + ".dds");
        else if (read_rgba8(t, rgba)) rt::platform_save_png(std::string(name) + ".png", t->w, t->h, rgba.data());
    }
    g_dump_tex_ids.clear();
    if (FILE* f = fopen((dir + "/draws.txt").c_str(), "w")) {
        for (size_t i = 0; i < g_dump_lines.size(); i++) fprintf(f, "%3zu %s\n", i, g_dump_lines[i].c_str());
        fclose(f);
    }
    if (FILE* f = fopen((dir + "/data.txt").c_str(), "w")) {
        for (auto& d : g_dump_data) fputs(d.c_str(), f);
        fclose(f);
    }
    g_dump_data.clear();
    for (Program* p : g_dump_progs)
        for (int st = 0; st < 2; st++) {
            char name[160];
            snprintf(name, sizeof name, "%s/prog_%p_%s.glsl", dir.c_str(), (void*)p, st ? "fs" : "vs");
            if (FILE* f = fopen(name, "w")) {
                fputs(p->st[st].glsl.c_str(), f);
                fclose(f);
            }
        }
    g_dump_rts.clear();
    g_dump_lines.clear();
    g_dump_progs.clear();
    g_dump_frames.erase(g_dump_frames.begin());
    g_dump_frame = g_dump_frames.empty() ? ~0ull - 1 : g_dump_frames.front();   // stays != ~0: GLSL is still kept
    if (g_dump_frames.size() + g_dump_times.size() == 0) log("gpu: frame dumps done");
}

// ---- NVN entry points ----------------------------------------------------------------------------------
#define P(name) static void name(Ctx* c)

// device
P(DeviceInitialize) {
    std::call_once(g_init_once, [] {
        if (const char* f = getenv("LETSGO_DUMP_FRAME"))
            for (const char* q = f; *q; q += strcspn(q, ","), q += *q == ',') {
                if (*q == 't') g_dump_times.push_back(strtod(q + 1, nullptr));
                else g_dump_frames.push_back(strtoull(q, nullptr, 10));
            }
        std::sort(g_dump_frames.begin(), g_dump_frames.end());
        std::sort(g_dump_times.begin(), g_dump_times.end());
        g_dump_multi = g_dump_frames.size() + g_dump_times.size() > 1;
        if (!g_dump_frames.empty()) g_dump_frame = g_dump_frames.front();
        else if (!g_dump_times.empty()) g_dump_frame = ~0ull - 1;              // dump mode: keep the GLSL
        if (g_dump_frame != ~0ull) shaders_keep_glsl();
        g_ready = init();
        if (!g_ready) rt::fatal("Vulkan could not be initialized (see letsgo.log). Run with --no-gpu for the logging-only renderer.");
        // sampler 0 until the game registers its own: texel-fetch handles carry no sampler
        GSmpB b{};
        b.min = b.mag = 1;
        write_sampler_descriptor(0, make_sampler(b));
    });
    c->x[0] = 1;
}
P(DeviceGetTextureHandle) { c->x[0] = (c->x[2] << 20) | (c->x[1] & 0xFFFFF); }
// a handle for texel fetches only: the texture with no sampler (sampler slot 0)
P(DeviceGetTexelFetchHandle) { c->x[0] = c->x[1] & 0xFFFFF; }
P(DeviceFinalizeCommandHandle) {
    std::lock_guard<std::mutex> lk(g_handle_lock);
    g_handles.erase(c->x[1]);
}

// memory pools and buffers
P(MemoryPoolBuilderSetDefaults) { memset(G<GPoolB>(c->x[0]), 0, sizeof(GPoolB)); }
P(MemoryPoolBuilderSetStorage) { auto* b = G<GPoolB>(c->x[0]); b->mem = c->x[1]; b->size = c->x[2]; }
P(MemoryPoolBuilderSetFlags) { G<GPoolB>(c->x[0])->flags = (uint32_t)c->x[1]; }
P(MemoryPoolInitialize) {
    auto* p = G<GPool>(c->x[0]);
    auto* b = G<GPoolB>(c->x[1]);
    *p = {b->mem, b->size, b->flags};
    pool_register(p->mem, p->size, p->flags);
    c->x[0] = 1;
}
P(MemoryPoolFinalize) { pool_unregister(G<GPool>(c->x[0])->mem); }
P(MemoryPoolMap) { c->x[0] = G<GPool>(c->x[0])->mem; }
P(MemoryPoolGetFlags) { c->x[0] = G<GPool>(c->x[0])->flags; }
P(MemoryPoolGetSize) { c->x[0] = G<GPool>(c->x[0])->size; }
P(BufferBuilderSetDefaults) { memset(G<GBufB>(c->x[0]), 0, sizeof(GBufB)); }
P(BufferBuilderSetStorage) { auto* b = G<GBufB>(c->x[0]); b->pool = c->x[1]; b->offset = c->x[2]; b->size = c->x[3]; }
P(BufferInitialize) {
    auto* buf = G<GBuf>(c->x[0]);
    auto* b = G<GBufB>(c->x[1]);
    buf->addr = (b->pool ? G<GPool>(b->pool)->mem : 0) + b->offset;
    buf->size = b->size;
    c->x[0] = 1;
}
P(BufferGetAddress) { c->x[0] = G<GBuf>(c->x[0])->addr; }
P(BufferGetSize) { c->x[0] = G<GBuf>(c->x[0])->size; }

// textures
P(TextureBuilderSetDefaults) {
    auto* b = G<GTexB>(c->x[0]);
    memset(b, 0, sizeof *b);
    b->w = b->h = b->d = b->levels = b->samples = 1;
    b->target = 1;
}
P(TextureBuilderSetFlags) { G<GTexB>(c->x[0])->flags = (uint32_t)c->x[1]; }
P(TextureBuilderSetTarget) { G<GTexB>(c->x[0])->target = (uint32_t)c->x[1]; }
P(TextureBuilderSetWidth) { G<GTexB>(c->x[0])->w = (uint32_t)c->x[1]; }
P(TextureBuilderSetHeight) { G<GTexB>(c->x[0])->h = (uint32_t)c->x[1]; }
P(TextureBuilderSetDepth) { G<GTexB>(c->x[0])->d = (uint32_t)c->x[1]; }
P(TextureBuilderSetSize1D) { auto* b = G<GTexB>(c->x[0]); b->w = (uint32_t)c->x[1]; b->h = b->d = 1; }
P(TextureBuilderSetSize2D) { auto* b = G<GTexB>(c->x[0]); b->w = (uint32_t)c->x[1]; b->h = (uint32_t)c->x[2]; b->d = 1; }
P(TextureBuilderSetSize3D) { auto* b = G<GTexB>(c->x[0]); b->w = (uint32_t)c->x[1]; b->h = (uint32_t)c->x[2]; b->d = (uint32_t)c->x[3]; }
P(TextureBuilderSetLevels) { G<GTexB>(c->x[0])->levels = (uint32_t)c->x[1]; }
P(TextureBuilderSetFormat) { G<GTexB>(c->x[0])->format = (uint32_t)c->x[1]; }
P(TextureBuilderSetSamples) { G<GTexB>(c->x[0])->samples = std::max(1u, (uint32_t)c->x[1]); }
P(TextureBuilderSetStride) { G<GTexB>(c->x[0])->stride = (uint32_t)c->x[1]; }
P(TextureBuilderSetStorage) { auto* b = G<GTexB>(c->x[0]); b->pool = c->x[1]; b->offset = c->x[2]; }
P(TextureBuilderSetPackagedTextureLayout) {
    auto* b = G<GTexB>(c->x[0]);
    if (c->x[1]) {
        memcpy(b->layout, G<uint8_t>(c->x[1]), 8);
        b->layout_set = 1;
    } else {
        b->layout_set = 0;
    }
}
uint64_t builder_size(const GTexB& b) {
    const FormatInfo& f = format_info(b.format);
    if (!f.bpb) return 0x10000;
    bool array = b.target == 3 || b.target == 4 || b.target == 6;
    uint32_t layers = array ? std::max(1u, b.d) : (b.target == 8 ? 6 : b.target == 9 ? 6 * std::max(1u, b.d) : 1);
    uint32_t bh = b.layout_set ? (b.layout[0] & 7) : default_block_height_log2((std::max(1u, b.h) + f.bh - 1) / f.bh);
    TexLayout L = compute_layout(f, std::max(1u, b.w), std::max(1u, b.h), b.target == 2 ? std::max(1u, b.d) : 1,
                                 std::max(1u, b.levels), layers, bh, (b.flags & 0x10) != 0, b.stride);
    return (L.total * std::max(1u, b.samples) + 511) & ~511ull;
}
P(TextureBuilderGetStorageSize) { c->x[0] = builder_size(*G<GTexB>(c->x[0])); }
P(TextureBuilderGetStorageAlignment) { c->x[0] = 512; }
P(TextureInitialize) {
    auto* t = G<GTex>(c->x[0]);
    auto* b = G<GTexB>(c->x[1]);
    Texture* h = create_texture(*b);
    *t = {h->w, h->h, b->target == 2 ? h->d : (b->target == 3 || b->target == 4 || b->target == 6 ? h->layers : 1), h->levels,
          b->format, b->target, h->samples, h->flags, h->addr, h};
    c->x[0] = 1;
}
P(TextureFinalize) {
    auto* t = G<GTex>(c->x[0]);
    if (t->host) destroy_texture(t->host);
    t->host = nullptr;
}
P(TextureGetWidth) { c->x[0] = G<GTex>(c->x[0])->w; }
P(TextureGetHeight) { c->x[0] = G<GTex>(c->x[0])->h; }
P(TextureGetDepth) { c->x[0] = G<GTex>(c->x[0])->d; }
P(TextureGetLevels) { c->x[0] = G<GTex>(c->x[0])->levels; }
P(TextureGetFormat) { c->x[0] = G<GTex>(c->x[0])->format; }
P(TextureGetTarget) { c->x[0] = G<GTex>(c->x[0])->target; }
P(TextureGetSamples) { c->x[0] = G<GTex>(c->x[0])->samples; }
P(TextureGetFlags) { c->x[0] = G<GTex>(c->x[0])->flags; }
P(TextureGetTextureAddress) { c->x[0] = G<GTex>(c->x[0])->addr; }
// WriteTexels(texture, view, region, data): tightly packed texels in the texture's format
P(TextureWriteTexels) {
    Texture* t = G<GTex>(c->x[0])->host;
    const GView* v = G<GView>(c->x[1]);
    const int32_t* reg = G<int32_t>(c->x[2]);
    if (!t || !reg) return;
    uint32_t level = v && (v->set & V_LEVELS) ? v->base_level : 0;
    uint32_t w = (uint32_t)reg[3], h = (uint32_t)reg[4], d = std::max(1, reg[5]);
    const FormatInfo& f = *t->fi;
    uint64_t bytes = (uint64_t)((w + f.bw - 1) / f.bw) * ((h + f.bh - 1) / f.bh) * f.bpb * d;
    std::vector<uint8_t> conv;
    convert_linear(f, G<uint8_t>(c->x[3]), bytes, w, h * d, conv);
    Staging st = staging_alloc(conv.size());
    memcpy(st.ptr, conv.data(), conv.size());
    VkBufferImageCopy bc{};
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, t->target == 2 ? 0u : (uint32_t)reg[2], t->target == 2 ? 1u : (uint32_t)d};
    bc.imageOffset = {reg[0], reg[1], t->target == 2 ? reg[2] : 0};
    bc.imageExtent = {w, h, t->target == 2 ? (uint32_t)d : 1u};
    VkImage img = t->image;
    defer([st, img, bc](VkCommandBuffer cb) {
        vk::vkCmdCopyBufferToImage(cb, st.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        staging_free(st);
    });
}
P(TextureFlushTexels) {
    Texture* t = G<GTex>(c->x[0])->host;
    if (t && t->cpu_visible) upload_from_guest(t);
}

// texture views
P(TextureViewSetDefaults) { memset(G<GView>(c->x[0]), 0, sizeof(GView)); }
P(TextureViewSetLevels) { auto* v = G<GView>(c->x[0]); v->set |= V_LEVELS; v->base_level = (uint16_t)c->x[1]; v->num_levels = (uint16_t)c->x[2]; }
P(TextureViewSetLayers) { auto* v = G<GView>(c->x[0]); v->set |= V_LAYERS; v->base_layer = (uint32_t)c->x[1]; v->num_layers = (uint32_t)c->x[2]; }
P(TextureViewSetFormat) { auto* v = G<GView>(c->x[0]); v->set |= V_FORMAT; v->format = (uint32_t)c->x[1]; }
P(TextureViewSetSwizzle) {
    auto* v = G<GView>(c->x[0]);
    v->set |= V_SWIZZLE;
    for (int i = 0; i < 4; i++) v->swz[i] = (uint8_t)c->x[1 + i];
}
P(TextureViewSetDepthStencilMode) { auto* v = G<GView>(c->x[0]); v->set |= V_DSMODE; v->dsmode = (uint8_t)c->x[1]; }
P(TextureViewSetTarget) { auto* v = G<GView>(c->x[0]); v->set |= V_TARGET; v->target = (uint8_t)c->x[1]; }
P(TextureViewGetLevels) {
    auto* v = G<GView>(c->x[0]);
    bool set = v->set & V_LEVELS;
    if (c->x[1]) *G<int32_t>(c->x[1]) = set ? v->base_level : 0;
    if (c->x[2]) *G<int32_t>(c->x[2]) = set ? v->num_levels : 0;
    c->x[0] = set;
}
P(TextureViewGetLayers) {
    auto* v = G<GView>(c->x[0]);
    bool set = v->set & V_LAYERS;
    if (c->x[1]) *G<int32_t>(c->x[1]) = set ? (int32_t)v->base_layer : 0;
    if (c->x[2]) *G<int32_t>(c->x[2]) = set ? (int32_t)v->num_layers : 0;
    c->x[0] = set;
}
P(TextureViewGetTarget) {
    auto* v = G<GView>(c->x[0]);
    bool set = v->set & V_TARGET;
    if (c->x[1]) *G<int32_t>(c->x[1]) = set ? v->target : 0;
    c->x[0] = set;
}
P(TextureViewGetFormat) {
    auto* v = G<GView>(c->x[0]);
    bool set = v->set & V_FORMAT;
    if (c->x[1]) *G<int32_t>(c->x[1]) = set ? (int32_t)v->format : 0;
    c->x[0] = set;
}

// samplers and pools
P(SamplerBuilderSetDefaults) {
    auto* b = G<GSmpB>(c->x[0]);
    memset(b, 0, sizeof *b);
    b->min = 1;          // NVN default: linear, no mipmapping
    b->mag = 1;
    b->wrap[0] = b->wrap[1] = b->wrap[2] = 1;
    b->lod_max = 1000.0f;
    b->aniso = 1.0f;
    b->cmp_func = 2;
}
P(SamplerBuilderSetMinMagFilter) { auto* b = G<GSmpB>(c->x[0]); b->min = (uint32_t)c->x[1]; b->mag = (uint32_t)c->x[2]; }
P(SamplerBuilderSetWrapMode) {
    auto* b = G<GSmpB>(c->x[0]);
    for (int i = 0; i < 3; i++) b->wrap[i] = (uint32_t)c->x[1 + i];
}
P(SamplerBuilderSetLodClamp) { auto* b = G<GSmpB>(c->x[0]); b->lod_min = F(c, 0); b->lod_max = F(c, 1); }
P(SamplerBuilderSetLodBias) { G<GSmpB>(c->x[0])->lod_bias = F(c, 0); }
P(SamplerBuilderSetMaxAnisotropy) { G<GSmpB>(c->x[0])->aniso = F(c, 0); }
P(SamplerBuilderSetCompare) { auto* b = G<GSmpB>(c->x[0]); b->cmp_mode = (uint32_t)c->x[1]; b->cmp_func = (uint32_t)c->x[2]; }
P(SamplerBuilderSetReductionFilter) { G<GSmpB>(c->x[0])->reduction = (uint32_t)c->x[1]; }
P(SamplerBuilderSetBorderColor) { memcpy(G<GSmpB>(c->x[0])->border, G<float>(c->x[1]), 16); }
P(SamplerInitialize) {
    G<GSmp>(c->x[0])->s = make_sampler(*G<GSmpB>(c->x[1]));
    c->x[0] = 1;
}
P(SamplerFinalize) {
    VkSampler s = G<GSmp>(c->x[0])->s;
    retire(g.submitted, [s] { vk::vkDestroySampler(g.dev, s, nullptr); });
}
P(TexturePoolInitialize) { c->x[0] = 1; }
P(SamplerPoolInitialize) { c->x[0] = 1; }
P(TexturePoolRegisterTexture) {
    auto* t = G<GTex>(c->x[2]);
    static std::atomic<int> nlog{0};
    bool movie_tex = t && t->host && ((t->w == 1920 && t->h == 1080) || (t->w == 960 && t->h == 540)) && t->host->fi->bpb <= 2;
    if (getenv("LETSGO_TEX_LOG") && (nlog++ < 300 || movie_tex))
        log("gpu: register texture id %u: %p %ux%u %s", (uint32_t)c->x[1], t ? (void*)t->host : nullptr, t ? t->w : 0, t ? t->h : 0,
            t && t->host ? t->host->fi->name : "-");
    if (t && t->host) {
        std::lock_guard<std::mutex> lk(g_tex_ids_lock);
        g_tex_ids[(uint32_t)c->x[1]] = t->host;
    }
    if (t && t->host) write_texture_descriptor((uint32_t)c->x[1], t->host, G<GView>(c->x[3]));
}
P(SamplerPoolRegisterSampler) {
    if (auto* s = G<GSmp>(c->x[2])) write_sampler_descriptor((uint32_t)c->x[1], s->s);
}

// fixed-function state objects
P(BlendStateSetDefaults) { *G<GBlend>(c->x[0]) = GBlend{0, 2, 1, 2, 1, 1, 1, 0}; }
P(BlendStateSetBlendTarget) { G<GBlend>(c->x[0])->target = (uint8_t)c->x[1]; }
P(BlendStateSetBlendFunc) {
    auto* b = G<GBlend>(c->x[0]);
    b->src = (uint8_t)c->x[1];
    b->dst = (uint8_t)c->x[2];
    b->srca = (uint8_t)c->x[3];
    b->dsta = (uint8_t)c->x[4];
}
P(BlendStateSetBlendEquation) { auto* b = G<GBlend>(c->x[0]); b->eq = (uint8_t)c->x[1]; b->eqa = (uint8_t)c->x[2]; }
P(ColorStateSetDefaults) { *G<GColor>(c->x[0]) = GColor{0, 3, 8, 0}; }
P(ColorStateSetBlendEnable) {
    auto* s = G<GColor>(c->x[0]);
    uint8_t bit = (uint8_t)(1u << (c->x[1] & 7));
    s->blend_mask = c->x[2] & 1 ? s->blend_mask | bit : s->blend_mask & (uint8_t)~bit;
}
P(ColorStateSetLogicOp) { G<GColor>(c->x[0])->logic_op = (uint8_t)c->x[1]; }
P(ColorStateSetAlphaTest) { G<GColor>(c->x[0])->alpha_test = (uint8_t)c->x[1]; }
P(ChannelMaskStateSetDefaults) { G<GChannel>(c->x[0])->disabled = 0; }
P(ChannelMaskStateSetChannelMask) {
    auto* s = G<GChannel>(c->x[0]);
    uint32_t i = (uint32_t)c->x[1] & 7, off = 0;
    for (int k = 0; k < 4; k++)
        if (!(c->x[2 + k] & 1)) off |= 1u << k;
    s->disabled = (s->disabled & ~(15u << (4 * i))) | off << (4 * i);
}
P(DepthStencilStateSetDefaults) { *G<GDepth>(c->x[0]) = GDepth{2, 2, 0x88, 0x11, 0x01, 0x11, 0x01, 0}; }
P(DepthStencilStateSetDepthTestEnable) { auto* s = G<GDepth>(c->x[0]); s->flags = (uint8_t)((s->flags & ~1) | (c->x[1] & 1)); }
P(DepthStencilStateSetDepthWriteEnable) { auto* s = G<GDepth>(c->x[0]); s->flags = (uint8_t)((s->flags & ~2) | ((c->x[1] & 1) << 1)); }
P(DepthStencilStateSetStencilTestEnable) { auto* s = G<GDepth>(c->x[0]); s->flags = (uint8_t)((s->flags & ~4) | ((c->x[1] & 1) << 2)); }
P(DepthStencilStateSetDepthFunc) { G<GDepth>(c->x[0])->func = (uint8_t)c->x[1]; }
P(DepthStencilStateSetStencilFunc) {
    auto* s = G<GDepth>(c->x[0]);
    uint8_t f = (uint8_t)c->x[2] & 15;
    if (c->x[1] & 1) s->sfunc = (uint8_t)((s->sfunc & 0xF0) | f);
    if (c->x[1] & 2) s->sfunc = (uint8_t)((s->sfunc & 0x0F) | f << 4);
}
// SetStencilOp(face, fail, depthFail, depthPass): stored as (fail | depthPass << 4, depthFail)
P(DepthStencilStateSetStencilOp) {
    auto* s = G<GDepth>(c->x[0]);
    uint8_t a = (uint8_t)((c->x[2] & 15) | (c->x[4] & 15) << 4), b = (uint8_t)(c->x[3] & 15);
    if (c->x[1] & 1) { s->sop_f0 = a; s->sop_f1 = b; }
    if (c->x[1] & 2) { s->sop_b0 = a; s->sop_b1 = b; }
}
P(MultisampleStateSetDefaults) { memset(G<GMs>(c->x[0]), 0, 0x18); G<GMs>(c->x[0])->samples = 1; }
P(MultisampleStateSetMultisampleEnable) { G<GMs>(c->x[0])->enable = c->x[1] & 1; }
P(MultisampleStateSetSamples) { G<GMs>(c->x[0])->samples = (uint8_t)std::max<uint64_t>(1, c->x[1]); }
P(MultisampleStateSetAlphaToCoverageEnable) { G<GMs>(c->x[0])->a2c = c->x[1] & 1; }
P(PolygonStateSetDefaults) { *G<GPoly>(c->x[0]) = GPoly{0, 1, 2, 0}; }
P(PolygonStateSetCullFace) { G<GPoly>(c->x[0])->cull = (uint8_t)c->x[1]; }
P(PolygonStateSetFrontFace) { G<GPoly>(c->x[0])->front = (uint8_t)c->x[1]; }
P(PolygonStateSetPolygonMode) { G<GPoly>(c->x[0])->mode = (uint8_t)c->x[1]; }
P(PolygonStateSetPolygonOffsetEnables) { G<GPoly>(c->x[0])->offset = (uint8_t)c->x[1]; }
P(VertexAttribStateSetDefaults) { *G<GAttrib>(c->x[0]) = GAttrib{0, 0, 0}; }
P(VertexAttribStateSetFormat) { auto* a = G<GAttrib>(c->x[0]); a->format = (uint8_t)c->x[1]; a->offset = (uint16_t)c->x[2]; }
P(VertexAttribStateSetStreamIndex) { G<GAttrib>(c->x[0])->stream = (uint8_t)c->x[1]; }
P(VertexStreamStateSetDefaults) { *G<GStream>(c->x[0]) = GStream{0, 0}; }
P(VertexStreamStateSetStride) { G<GStream>(c->x[0])->stride = (uint32_t)c->x[1]; }
P(VertexStreamStateSetDivisor) { G<GStream>(c->x[0])->divisor = (uint32_t)c->x[1]; }

// programs
P(ProgramInitialize) {
    *G<Program*>(c->x[0]) = new Program;
    if (getenv("LETSGO_PROG_LOG")) log("gpu: ProgramInitialize(0x%llx)", (unsigned long long)c->x[0]);
    g_programs++;
    c->x[0] = 1;
}
P(ProgramFinalize) {
    Program* p = *G<Program*>(c->x[0]);
    if (getenv("LETSGO_PROG_LOG")) log("gpu: ProgramFinalize(0x%llx)", (unsigned long long)c->x[0]);
    if (!p) return;
    retire(g.submitted, [p] { delete p; });     // its shader objects stay with shaders.cpp (other programs share them)
    *G<Program*>(c->x[0]) = nullptr;
}
// SetShaders(program, count, NVNshaderData[] {data address, control pointer})
P(ProgramSetShaders) {
    Program* p = *G<Program*>(c->x[0]);
    int n = (int)c->x[1];
    const uint64_t* sd = G<uint64_t>(c->x[2]);
    bool ok = true;
    for (int i = 0; i < n && p; i++) {
        const uint8_t* ctrl = G<uint8_t>(sd[2 * i + 1]);
        if (!ctrl || *(const uint32_t*)ctrl != 0x98761234u) {
            log("gpu: shader %d of a program: control 0x%llx (%s) data 0x%llx, magic 0x%08x", i, (unsigned long long)sd[2 * i + 1],
                rt::describe(sd[2 * i + 1]).c_str(), (unsigned long long)sd[2 * i], ctrl ? *(const uint32_t*)ctrl : 0);
            ok = false;
            continue;
        }
        uint32_t asm_off = *(const uint32_t*)(ctrl + 0x14), asm_size = *(const uint32_t*)(ctrl + 0x18);
        uint32_t const_off = *(const uint32_t*)(ctrl + 0x20);
        const char* hdr = (const char*)ctrl + asm_off;
        // the program.local constants the text declares ("PARAM c[N] = ..."), not more: what follows
        // them differs between runs, and the copy identifies the shader in the shader cache
        uint32_t constants = 0;
        if (std::string_view text(hdr, asm_size); text.find("PARAM c[") != std::string_view::npos)
            constants = (uint32_t)strtoul(hdr + text.find("PARAM c[") + 8, nullptr, 10);
        size_t size = std::max<size_t>(asm_off + asm_size, const_off + 16 * (size_t)std::min(constants, 64u));
        int stage = !strncmp(hdr, "!!NVvp", 6) ? 0 : !strncmp(hdr, "!!NVfp", 6) ? 1 : !strncmp(hdr, "!!NVcp", 6) ? 2 : -1;
        std::shared_ptr<ShaderSource> src;
        if (stage >= 0) {
            src = make_source(stage, false, ctrl, size);
        } else {
            // no assembly: translate the GPU code (control+0x6F0: control section size)
            size_t csize = std::min<size_t>(std::max<uint32_t>(0x710, *(const uint32_t*)(ctrl + 0x6F0)), 1u << 20);
            const uint8_t* data = G<uint8_t>(sd[2 * i]);
            size_t dsize = maxwell::data_size(ctrl, csize);
            uint32_t sph = *(const uint32_t*)(ctrl + 0x708);
            uint32_t kind = data && dsize > sph + 4 ? (*(const uint32_t*)(data + sph) >> 10) & 15 : 0;
            stage = kind == 1 ? 0 : kind == 5 ? 1 : -1;
            if (stage < 0) {
                log("gpu: shader %d of a program: no NV assembly and unsupported GPU code (type %u)", i, kind);
                ok = false;
                continue;
            }
            if (const char* dir = getenv("LETSGO_DUMP_SHADERS")) {
                static std::atomic<int> dumped{0};
                std::string base = std::string(dir) + "/" + std::to_string(dumped++);
                if (FILE* f = fopen((base + ".ctrl").c_str(), "wb")) { fwrite(ctrl, 1, csize, f); fclose(f); }
                if (FILE* f = fopen((base + ".code").c_str(), "wb")) { fwrite(data, 1, dsize, f); fclose(f); }
            }
            src = make_source(stage, true, ctrl, csize, data, dsize);
        }
        // the build starts now, on a worker (the game sets programs while it loads what uses them);
        // the reflection comes from the shader cache, or from translating here
        auto b = shader_build(src, 0, true);
        StageShader& s = p->st[stage];
        std::lock_guard<std::mutex> lk(p->lock);
        std::string error;
        s.maxwell = src->maxwell;
        s.src = src;
        s.variants.clear();
        if (!shader_reflection(*b, s.refl, error)) {
            log("gpu: program stage %d%s: %s", stage, s.maxwell ? " (GPU code)" : "", error.c_str());
            s.present = false;
            ok = false;
            continue;
        }
        s.present = true;
    }
    static std::atomic<int> logged{0};
    if (!ok && logged++ < 40) log("gpu: ProgramSetShaders(%d stages): not all stages could be translated", n);
    if (getenv("LETSGO_PROG_LOG") && p) {
        std::string kinds;
        for (int i = 0; i < n; i++) {
            const uint8_t* ctrl = G<uint8_t>(sd[2 * i + 1]);
            uint32_t ao = ctrl ? *(const uint32_t*)(ctrl + 0x14) : 0;
            kinds += ctrl && ao ? std::string((const char*)ctrl + ao, strnlen((const char*)ctrl + ao, 8)) + " " : "? ";
        }
        log("gpu: program %p set with %d shaders [%s]: vertex %d fragment %d compute %d", (void*)p, n, kinds.c_str(), p->st[0].present,
            p->st[1].present, p->st[2].present);
    }
    c->x[0] = 1;                  // the game treats failure as fatal; draws with missing stages are skipped instead
}

// command buffers
P(CommandBufferInitialize) {
    *G<Recorder*>(c->x[0]) = new Recorder;
    c->x[0] = 1;
}
P(CommandBufferFinalize) {
    delete *G<Recorder*>(c->x[0]);
    *G<Recorder*>(c->x[0]) = nullptr;
}
P(CommandBufferBeginRecording) {
    Recorder* r = rec(c);
    r->cur = std::make_shared<OpList>();
    r->recording = true;
}
P(CommandBufferEndRecording) {
    Recorder* r = rec(c);
    r->recording = false;
    std::lock_guard<std::mutex> lk(g_handle_lock);
    uint64_t h = g_next_handle++;
    g_handles[h] = std::move(r->cur);
    r->cur.reset();
    c->x[0] = h;
}
P(CommandBufferIsRecording) { c->x[0] = rec(c) && rec(c)->recording; }
P(CommandBufferCallCommands) {
    int n = (int)c->x[1];
    const uint64_t* hs = G<uint64_t>(c->x[2]);
    std::vector<std::shared_ptr<OpList>> lists;
    {
        std::lock_guard<std::mutex> lk(g_handle_lock);
        for (int i = 0; i < n; i++) {
            auto it = g_handles.find(hs[i]);
            if (it != g_handles.end()) lists.push_back(it->second);
        }
    }
    record(c, [lists](Exec& ex) {
        for (auto& l : lists)
            for (auto& op : *l) op(ex);
    });
}
// BindProgram(program, stage bits): 0x1 vertex, 0x2 fragment (0x4-0x10 geometry/tessellation: not
// supported), 0x20 compute; null unbinds the stages
P(CommandBufferBindProgram) {
    Program* p = c->x[1] ? *G<Program*>(c->x[1]) : nullptr;
    uint32_t stages = (uint32_t)c->x[2];
    static std::atomic<int> nlog{0};
    if (c->x[1] && !p && nlog++ < 10)
        log("gpu: BindProgram(0x%llx): not an initialized program (%s)", (unsigned long long)c->x[1], rt::describe(c->x[1]).c_str());
    static std::atomic<int> nnull{0};
    if (!c->x[1] && getenv("LETSGO_PROG_LOG") && nnull++ < 20) log("gpu: BindProgram(null, stages 0x%x)", stages);
    record(c, [p, stages](Exec& ex) {
        if (stages & 0x01) ex.vprog = p;
        if (stages & 0x02) ex.fprog = p;
        if (stages & 0x20) ex.cprog = p;
    });
}
P(CommandBufferBindUniformBuffer) {
    uint32_t st = (uint32_t)c->x[1], i = (uint32_t)c->x[2];
    uint64_t a = c->x[3], s = c->x[4];
    if (st < 6 && i < 16) record(c, [st, i, a, s](Exec& ex) { ex.ubo[st][i] = {a, s}; });
}
P(CommandBufferBindStorageBuffer) {
    uint32_t st = (uint32_t)c->x[1], i = (uint32_t)c->x[2];
    uint64_t a = c->x[3], s = c->x[4];
    if (st < 6 && i < 16) record(c, [st, i, a, s](Exec& ex) { ex.ssbo[st][i] = {a, s}; });
}
P(CommandBufferBindTexture) {
    uint32_t st = (uint32_t)c->x[1], i = (uint32_t)c->x[2];
    uint64_t h = c->x[3];
    if (st < 6 && i < 32) record(c, [st, i, h](Exec& ex) { ex.tex[st][i] = h; });
}
P(CommandBufferBindVertexBuffer) {
    uint32_t i = (uint32_t)c->x[1];
    uint64_t a = c->x[2], s = c->x[3];
    if (i < 16) record(c, [i, a, s](Exec& ex) { ex.vb[i] = {a, s}; });
}
P(CommandBufferBindVertexBuffers) {
    uint32_t first = (uint32_t)c->x[1], n = (uint32_t)c->x[2];
    const uint64_t* r = G<uint64_t>(c->x[3]);
    std::vector<Bound> v;
    for (uint32_t i = 0; i < n; i++) v.push_back({r[2 * i], r[2 * i + 1]});
    record(c, [first, v](Exec& ex) {
        for (size_t i = 0; i < v.size() && first + i < 16; i++) ex.vb[first + i] = v[i];
    });
}
P(CommandBufferBindVertexAttribState) {
    int n = std::min<int>((int)c->x[1], 16);
    std::vector<GAttrib> a(G<GAttrib>(c->x[2]), G<GAttrib>(c->x[2]) + n);
    record(c, [a](Exec& ex) {
        ex.nattrib = (int)a.size();
        for (size_t i = 0; i < a.size(); i++) ex.attrib[i] = a[i];
    });
}
P(CommandBufferBindVertexStreamState) {
    int n = std::min<int>((int)c->x[1], 16);
    std::vector<GStream> s(G<GStream>(c->x[2]), G<GStream>(c->x[2]) + n);
    record(c, [s](Exec& ex) {
        ex.nstream = (int)s.size();
        for (size_t i = 0; i < s.size(); i++) ex.stream[i] = s[i];
    });
}
P(CommandBufferBindBlendState) {
    GBlend b = *G<GBlend>(c->x[1]);
    record(c, [b](Exec& ex) { ex.blend[b.target & 7] = b; });
}
P(CommandBufferBindColorState) {
    GColor s = *G<GColor>(c->x[1]);
    record(c, [s](Exec& ex) { ex.color_state = s; });
}
P(CommandBufferBindChannelMaskState) {
    GChannel s = *G<GChannel>(c->x[1]);
    record(c, [s](Exec& ex) { ex.channel = s; });
}
P(CommandBufferBindDepthStencilState) {
    GDepth s = *G<GDepth>(c->x[1]);
    record(c, [s](Exec& ex) { ex.ds = s; });
}
P(CommandBufferBindMultisampleState) {
    GMs s = *G<GMs>(c->x[1]);
    record(c, [s](Exec& ex) { ex.ms = s; });
}
P(CommandBufferBindPolygonState) {
    GPoly s = *G<GPoly>(c->x[1]);
    record(c, [s](Exec& ex) { ex.poly = s; });
}
P(CommandBufferSetSampleMask) {
    uint32_t m = (uint32_t)c->x[1];
    record(c, [m](Exec& ex) { ex.sample_mask = m; });
}
P(CommandBufferSetViewports) {
    int first = (int)c->x[1], n = (int)c->x[2];
    std::vector<float> v(G<float>(c->x[3]), G<float>(c->x[3]) + 4 * n);
    record(c, [first, n, v](Exec& ex) {
        for (int i = 0; i < n && first + i < 16; i++) memcpy(ex.vp[first + i], &v[(size_t)i * 4], 16);
        ex.nvp = std::max(ex.nvp, std::min(16, first + n));
    });
}
P(CommandBufferSetViewport) {
    float v[4] = {(float)(int32_t)c->x[1], (float)(int32_t)c->x[2], (float)(int32_t)c->x[3], (float)(int32_t)c->x[4]};
    record(c, [v](Exec& ex) { memcpy(ex.vp[0], v, 16); });
}
P(CommandBufferSetScissors) {
    int first = (int)c->x[1], n = (int)c->x[2];
    std::vector<int32_t> v(G<int32_t>(c->x[3]), G<int32_t>(c->x[3]) + 4 * n);
    record(c, [first, n, v](Exec& ex) {
        for (int i = 0; i < n && first + i < 16; i++) memcpy(ex.sc[first + i], &v[(size_t)i * 4], 16);
        ex.nsc = std::max(ex.nsc, std::min(16, first + n));
    });
}
P(CommandBufferSetScissor) {
    int32_t v[4] = {(int32_t)c->x[1], (int32_t)c->x[2], (int32_t)c->x[3], (int32_t)c->x[4]};
    record(c, [v](Exec& ex) { memcpy(ex.sc[0], v, 16); });
}
P(CommandBufferSetDepthRange) {
    float n = F(c, 0), f = F(c, 1);
    record(c, [n, f](Exec& ex) {
        for (auto& d : ex.dr) { d[0] = n; d[1] = f; }
    });
}
P(CommandBufferSetDepthRanges) {
    int first = (int)c->x[1], n = (int)c->x[2];
    std::vector<float> v(G<float>(c->x[3]), G<float>(c->x[3]) + 2 * n);
    record(c, [first, n, v](Exec& ex) {
        for (int i = 0; i < n && first + i < 16; i++) { ex.dr[first + i][0] = v[(size_t)2 * i]; ex.dr[first + i][1] = v[(size_t)2 * i + 1]; }
    });
}
P(CommandBufferSetBlendColor) {
    float v[4];
    memcpy(v, G<float>(c->x[1]), 16);
    record(c, [v](Exec& ex) { memcpy(ex.blend_color, v, 16); });
}
P(CommandBufferSetStencilRef) {
    uint32_t f = (uint32_t)c->x[1], v = (uint32_t)c->x[2];
    record(c, [f, v](Exec& ex) {
        if (f & 1) ex.stencil_ref[0] = v;
        if (f & 2) ex.stencil_ref[1] = v;
    });
}
P(CommandBufferSetStencilMask) {
    uint32_t f = (uint32_t)c->x[1], v = (uint32_t)c->x[2];
    record(c, [f, v](Exec& ex) {
        if (f & 1) ex.stencil_wmask[0] = v;
        if (f & 2) ex.stencil_wmask[1] = v;
    });
}
P(CommandBufferSetStencilValueMask) {
    uint32_t f = (uint32_t)c->x[1], v = (uint32_t)c->x[2];
    record(c, [f, v](Exec& ex) {
        if (f & 1) ex.stencil_vmask[0] = v;
        if (f & 2) ex.stencil_vmask[1] = v;
    });
}
P(CommandBufferSetLineWidth) {
    float w = F(c, 0);
    record(c, [w](Exec& ex) { ex.line_width = w; });
}
P(CommandBufferSetDepthBounds) {
    bool en = c->x[1] & 1;
    float a = F(c, 0), b = F(c, 1);
    record(c, [en, a, b](Exec& ex) {
        ex.depth_bounds_en = en;
        ex.depth_bounds[0] = a;
        ex.depth_bounds[1] = b;
    });
}
P(CommandBufferSetDepthClamp) {
    bool en = c->x[1] & 1;
    record(c, [en](Exec& ex) { ex.depth_clamp = en; });
}
P(CommandBufferSetRasterizerDiscard) {
    bool en = c->x[1] & 1;
    record(c, [en](Exec& ex) { ex.raster_discard = en; });
}
// SetPolygonOffsetClamp(factor, units, clamp)
P(CommandBufferSetPolygonOffsetClamp) {
    float f = F(c, 0), u = F(c, 1), cl = F(c, 2);
    record(c, [f, u, cl](Exec& ex) {
        ex.poly_offset[0] = cl;
        ex.poly_offset[1] = u;
        ex.poly_offset[2] = f;
    });
}

// SetRenderTargets(count, NVNtexture* const* colors, NVNtextureView* const* views, depth, depthView)
P(CommandBufferSetRenderTargets) {
    int n = std::min<int>((int)c->x[1], 8);
    const uint64_t* tex = G<uint64_t>(c->x[2]);
    const uint64_t* views = G<uint64_t>(c->x[3]);
    std::vector<RT> cv((size_t)std::max(n, 0));
    for (int i = 0; i < n; i++) {
        GTex* t = tex ? G<GTex>(tex[i]) : nullptr;
        cv[(size_t)i].tex = t ? t->host : nullptr;
        if (views && views[i]) cv[(size_t)i].view = *G<GView>(views[i]);
    }
    RT dep;
    if (GTex* t = G<GTex>(c->x[4])) dep.tex = t->host;
    if (c->x[5]) dep.view = *G<GView>(c->x[5]);
    record(c, [cv, dep](Exec& ex) {
        ex.end_rendering();
        ex.ncolor = (int)cv.size();
        for (size_t i = 0; i < cv.size(); i++) ex.color[i] = cv[i];
        ex.depth = dep;
    });
}

// ClearColor(index, const float/int/uint* color, mask)
P(CommandBufferClearColor) {
    int idx = (int)c->x[1];
    VkClearColorValue v;
    memcpy(&v, G<uint8_t>(c->x[2]), 16);
    record(c, [idx, v](Exec& ex) {
        if (idx >= ex.ncolor || !ex.color[idx].tex) return;
        ex.ensure_rendering();
        VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)idx, {}};
        a.clearValue.color = v;
        VkClearRect r = clear_rect(ex);
        if (r.rect.extent.width && r.rect.extent.height) vk::vkCmdClearAttachments(ex.cb, 1, &a, 1, &r);
    });
}
// ClearDepthStencil(float depth, bool depthMask, int stencil, int stencilMask)
P(CommandBufferClearDepthStencil) {
    float d = F(c, 0);
    bool dm = c->x[1] & 1;
    uint32_t sv = (uint32_t)c->x[2], sm = (uint32_t)c->x[3];
    record(c, [d, dm, sv, sm](Exec& ex) {
        if (!ex.depth.tex) return;
        ex.ensure_rendering();
        VkImageAspectFlags asp = 0;
        if (dm && (ex.depth.tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (sm && (ex.depth.tex->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!asp) return;
        VkClearAttachment a{asp, 0, {}};
        a.clearValue.depthStencil = {d, sv};
        VkClearRect r = clear_rect(ex);
        if (r.rect.extent.width && r.rect.extent.height) vk::vkCmdClearAttachments(ex.cb, 1, &a, 1, &r);
    });
}
// ClearTexture(texture, view, NVNcopyRegion*, const float* color, mask)
P(CommandBufferClearTexture) {
    GTex* gt = G<GTex>(c->x[1]);
    Texture* t = gt ? gt->host : nullptr;
    GView v{};
    if (c->x[2]) v = *G<GView>(c->x[2]);
    VkClearColorValue col;
    memcpy(&col, G<uint8_t>(c->x[4]), 16);
    if (!t) return;
    record(c, [t, v, col](Exec& ex) {
        ex.end_rendering();
        uint32_t lv = (v.set & V_LEVELS) ? v.base_level : 0;
        uint32_t layer = (v.set & V_LAYERS) ? v.base_layer : 0;
        if (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearDepthStencilValue dv{col.float32[0], 0};
            VkImageSubresourceRange r{t->aspect, lv, 1, layer, 1};
            vk::vkCmdClearDepthStencilImage(ex.cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &r);
        } else {
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, layer, 1};
            vk::vkCmdClearColorImage(ex.cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &col, 1, &r);
        }
    });
}
P(CommandBufferBarrier) {
    record(c, [](Exec& ex) {
        ex.end_rendering();
        full_barrier(ex.cb);
    });
}

// draws
P(CommandBufferDrawElements) {
    uint32_t prim = (uint32_t)c->x[1], type = (uint32_t)c->x[2];
    int count = (int)c->x[3];
    uint64_t addr = c->x[4];
    record(c, [=](Exec& ex) { draw_elements(ex, prim, type, count, addr, 0, 0, 1); });
}
P(CommandBufferDrawElementsBaseVertex) {
    uint32_t prim = (uint32_t)c->x[1], type = (uint32_t)c->x[2];
    int count = (int)c->x[3];
    uint64_t addr = c->x[4];
    int bv = (int)c->x[5];
    record(c, [=](Exec& ex) { draw_elements(ex, prim, type, count, addr, bv, 0, 1); });
}
// DrawElementsInstanced(mode, type, count, addr, baseVertex, baseInstance, instanceCount)
P(CommandBufferDrawElementsInstanced) {
    uint32_t prim = (uint32_t)c->x[1], type = (uint32_t)c->x[2];
    int count = (int)c->x[3];
    uint64_t addr = c->x[4];
    int bv = (int)c->x[5], bi = (int)c->x[6], n = (int)c->x[7];
    record(c, [=](Exec& ex) { draw_elements(ex, prim, type, count, addr, bv, bi, n); });
}
P(CommandBufferDrawArrays) {
    uint32_t prim = (uint32_t)c->x[1];
    int first = (int)c->x[2], count = (int)c->x[3];
    record(c, [=](Exec& ex) { draw_arrays(ex, prim, first, count, 0, 1); });
}
// DrawArraysInstanced(mode, first, count, baseInstance, instanceCount)
P(CommandBufferDrawArraysInstanced) {
    uint32_t prim = (uint32_t)c->x[1];
    int first = (int)c->x[2], count = (int)c->x[3], bi = (int)c->x[4], n = (int)c->x[5];
    record(c, [=](Exec& ex) { draw_arrays(ex, prim, first, count, bi, n); });
}

// copies
P(CommandBufferCopyBufferToBuffer) {
    uint64_t src = c->x[1], dst = c->x[2], size = c->x[3];
    record(c, [=](Exec& ex) {
        const HostBuffer* s = host_buffer(src, size);
        const HostBuffer* d = host_buffer(dst, size);
        if (!s || !d || !size) return;
        ex.end_rendering();
        VkBufferCopy r{src - s->base, dst - d->base, size};
        vk::vkCmdCopyBuffer(ex.cb, s->buf, d->buf, 1, &r);
    });
}
// CopyBufferToTexture(src address, texture, view, region, flags): tightly packed source texels
P(CommandBufferCopyBufferToTexture) {
    uint64_t src = c->x[1];
    GTex* gt = G<GTex>(c->x[2]);
    Texture* t = gt ? gt->host : nullptr;
    GView v{};
    if (c->x[3]) v = *G<GView>(c->x[3]);
    int32_t reg[6];
    memcpy(reg, G<int32_t>(c->x[4]), 24);
    if (!t) return;
    static std::atomic<int> nlog{0};
    if (getenv("LETSGO_TEX_LOG") && nlog++ < 400)
        log("gpu: copy buffer 0x%llx -> texture %p (%s) region %d,%d,%d %dx%dx%d", (unsigned long long)src, (void*)t, t->fi->name,
            reg[0], reg[1], reg[2], reg[3], reg[4], reg[5]);
    std::vector<int32_t> rv(reg, reg + 6);
    record(c, [=](Exec& ex) {
        ex.end_rendering();
        const FormatInfo& f = *t->fi;
        uint32_t lv = (v.set & V_LEVELS) ? v.base_level : 0;
        uint32_t w = (uint32_t)rv[3], h = (uint32_t)rv[4], d = (uint32_t)std::max(1, rv[5]);
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, t->target == 2 ? 0u : (uint32_t)rv[2], t->target == 2 ? 1u : d};
        bc.imageOffset = {rv[0], rv[1], t->target == 2 ? rv[2] : 0};
        bc.imageExtent = {w, h, t->target == 2 ? d : 1u};
        uint64_t bytes = (uint64_t)((w + f.bw - 1) / f.bw) * ((h + f.bh - 1) / f.bh) * f.bpb * d;
        if (f.conv != Conv::None) {
            // the data needs converting: read it from guest memory now
            std::vector<uint8_t> conv;
            convert_linear(f, G<uint8_t>(src), bytes, w, h * d, conv);
            Scratch s = scratch(conv.size(), 16);
            memcpy(s.ptr, conv.data(), conv.size());
            bc.bufferOffset = s.offset;
            vk::vkCmdCopyBufferToImage(ex.cb, s.buf, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
            return;
        }
        const HostBuffer* hb = host_buffer(src, bytes);
        if (!hb) return;
        bc.bufferOffset = src - hb->base;
        vk::vkCmdCopyBufferToImage(ex.cb, hb->buf, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    });
}
// CopyTextureToTexture(src, srcView, srcRegion, dst, dstView, dstRegion, flags)
P(CommandBufferCopyTextureToTexture) {
    GTex* gs = G<GTex>(c->x[1]);
    GTex* gd = G<GTex>(c->x[4]);
    Texture* s = gs ? gs->host : nullptr;
    Texture* d = gd ? gd->host : nullptr;
    GView sv{}, dv{};
    if (c->x[2]) sv = *G<GView>(c->x[2]);
    if (c->x[5]) dv = *G<GView>(c->x[5]);
    std::vector<int32_t> sr(G<int32_t>(c->x[3]), G<int32_t>(c->x[3]) + 6), dr(G<int32_t>(c->x[6]), G<int32_t>(c->x[6]) + 6);
    uint32_t flags = (uint32_t)c->x[7];
    if (!s || !d) return;
    record(c, [=](Exec& ex) {
        ex.end_rendering();
        uint32_t sl = (sv.set & V_LEVELS) ? sv.base_level : 0, dl = (dv.set & V_LEVELS) ? dv.base_level : 0;
        uint32_t slay = (sv.set & V_LAYERS) ? sv.base_layer : 0, dlay = (dv.set & V_LAYERS) ? dv.base_layer : 0;
        bool same = sr[3] == dr[3] && sr[4] == dr[4] && s->fi->image == d->fi->image && !(flags & 0x1C);
        if (same) {
            VkImageCopy r{};
            r.srcSubresource = {s->aspect, sl, slay, 1};
            r.dstSubresource = {d->aspect, dl, dlay, 1};
            r.srcOffset = {sr[0], sr[1], 0};
            r.dstOffset = {dr[0], dr[1], 0};
            r.extent = {(uint32_t)sr[3], (uint32_t)sr[4], 1};
            vk::vkCmdCopyImage(ex.cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        } else {
            VkImageBlit b{};
            b.srcSubresource = {s->aspect, sl, slay, 1};
            b.dstSubresource = {d->aspect, dl, dlay, 1};
            b.srcOffsets[0] = {sr[0], sr[1], 0};
            b.srcOffsets[1] = {sr[0] + sr[3], sr[1] + sr[4], 1};
            b.dstOffsets[0] = {dr[0], dr[1], 0};
            b.dstOffsets[1] = {dr[0] + dr[3], dr[1] + dr[4], 1};
            if (flags & 4) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);
            if (flags & 8) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);
            VkFilter filt = ((flags & 1) && s->aspect == VK_IMAGE_ASPECT_COLOR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            vk::vkCmdBlitImage(ex.cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, filt);
        }
    });
}
// ReportCounter(type, address): NVNcounterData {counter, timestamp in ns}
P(CommandBufferReportCounter) {
    uint32_t type = (uint32_t)c->x[1];
    uint64_t addr = c->x[2];
    record(c, [type, addr](Exec& ex) {
        const HostBuffer* hb = host_buffer(addr, 16);
        if (!hb) return;
        ex.end_rendering();
        uint64_t data[2] = {type == 1 ? 1ull : 0ull, (uint64_t)((double)rt::ticks() * (1e9 / 19200000.0))};
        vk::vkCmdUpdateBuffer(ex.cb, hb->buf, addr - hb->base, 16, data);
    });
}
P(CommandBufferNoop) { (void)c; }

// queue, sync, window
P(QueueInitialize) { c->x[0] = 1; }
P(QueueSubmitCommands) {
    int n = (int)c->x[1];
    const uint64_t* hs = G<uint64_t>(c->x[2]);
    std::vector<std::shared_ptr<OpList>> lists;
    {
        std::lock_guard<std::mutex> lk(g_handle_lock);
        for (int i = 0; i < n; i++) {
            auto it = g_handles.find(hs[i]);
            if (it != g_handles.end()) lists.push_back(it->second);
        }
    }
    std::lock_guard<std::recursive_mutex> lk(g.queue_lock);
    refresh_watched();                         // CPU-written textures, uploaded at the start of this command buffer
    Cmd cmd = begin_cmd();
    g_exec.new_cb(cmd.cb);
    for (auto& l : lists)
        for (auto& op : *l) op(g_exec);
    g_exec.end_rendering();
    submit_cmd(cmd);
}
P(QueueFlush) { (void)c; }
P(QueueFinish) { (void)c; wait_value(g.submitted); }
P(QueueFenceSync) { G<GSync>(c->x[1])->value = g.submitted; }
P(QueueWaitSync) { c->x[0] = 1; }
P(SyncInitialize) {
    G<GSync>(c->x[0])->value = 0;
    c->x[0] = 1;
}
// SyncWait(sync, timeout ns) -> ALREADY_SIGNALED 0, CONDITION_SATISFIED 1, TIMEOUT_EXPIRED 2
P(SyncWait) {
    uint64_t v = G<GSync>(c->x[0])->value, timeout = c->x[1];
    if (completed_value() >= v) { c->x[0] = 0; return; }
    if (!timeout) { c->x[0] = 2; return; }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g.timeline;
    wi.pValues = &v;
    VkResult r = vk::vkWaitSemaphores(g.dev, &wi, timeout);
    c->x[0] = r == VK_SUCCESS ? 1 : 2;
}
P(WindowBuilderSetDefaults) {
    memset(G<GWinB>(c->x[0]), 0, sizeof(GWinB));
    G<GWinB>(c->x[0])->interval = 1;
}
P(WindowBuilderSetTextures) {
    auto* b = G<GWinB>(c->x[0]);
    b->count = (uint32_t)c->x[1];
    b->textures = c->x[2];
}
P(WindowBuilderSetPresentInterval) { G<GWinB>(c->x[0])->interval = (uint32_t)c->x[1]; }
P(WindowInitialize) {
    auto* w = G<GWin>(c->x[0]);
    auto* b = G<GWinB>(c->x[1]);
    memset(w, 0, sizeof *w);
    w->count = std::min(b->count ? b->count : 2u, 8u);
    w->interval = b->interval;
    for (uint32_t i = 0; i < w->count && b->textures; i++) w->tex[i] = G<uint64_t>(b->textures)[i];
    log("nvn: window initialized with %u textures", w->count);
    c->x[0] = 1;
}
P(WindowAcquireTexture) {
    auto* w = G<GWin>(c->x[0]);
    *G<int32_t>(c->x[2]) = (int32_t)(w->next++ % (w->count ? w->count : 2));
    if (c->x[1]) G<GSync>(c->x[1])->value = g.submitted;
    c->x[0] = 0;
}
P(WindowSetPresentInterval) { G<GWin>(c->x[0])->interval = (uint32_t)c->x[1]; }
// SetCrop(window, x, y, w, h): the part of each texture that is shown
P(WindowSetCrop) {
    auto* w = G<GWin>(c->x[0]);
    for (int i = 0; i < 4; i++) w->crop[i] = (int32_t)c->x[1 + i];
}
P(WindowGetPresentInterval) { c->x[0] = G<GWin>(c->x[0])->interval; }
// QueuePresentTexture(queue, window, index)
P(QueuePresentTexture) {
    auto* w = G<GWin>(c->x[1]);
    uint32_t idx = (uint32_t)c->x[2];
    GTex* gt = idx < w->count ? G<GTex>(w->tex[idx]) : nullptr;
    if (gt && gt->host) {
        Texture* t = gt->host;
        int32_t x = 0, y = 0, cw = (int32_t)t->w, ch = (int32_t)t->h;
        if (w->crop[2] > 0 && w->crop[3] > 0) {
            x = std::clamp(w->crop[0], 0, cw - 1), y = std::clamp(w->crop[1], 0, ch - 1);
            cw = std::min(w->crop[2], cw - x), ch = std::min(w->crop[3], ch - y);
        }
        capture_if_requested(t, x, y, (uint32_t)cw, (uint32_t)ch);
        if (g_dump_frame != ~0ull) dump_frame(t);
        dump_time_check();
        // on the frame's place on the virtual 60 Hz display, so frames reach the screen evenly spaced
        rt::nvn::frame_pace(w->interval);
        present(t->image, is_srgb(t->fi->image), x, y, (uint32_t)cw, (uint32_t)ch, true);
    } else {
        rt::nvn::frame_pace(w->interval);
    }
    rt::nvn::frame_done();
}

#undef P
}  // namespace

void register_nvn_vulkan() {
    using rt::nvn::set_proc;
#define R(name) set_proc("nvn" #name, name)
    R(DeviceInitialize); R(DeviceGetTextureHandle); R(DeviceGetTexelFetchHandle); R(DeviceFinalizeCommandHandle);
    R(MemoryPoolBuilderSetDefaults); R(MemoryPoolBuilderSetStorage); R(MemoryPoolBuilderSetFlags); R(MemoryPoolInitialize);
    R(MemoryPoolFinalize); R(MemoryPoolMap); R(MemoryPoolGetFlags); R(MemoryPoolGetSize);
    set_proc("nvnMemoryPoolGetBufferAddress", MemoryPoolMap);
    R(BufferBuilderSetDefaults); R(BufferBuilderSetStorage); R(BufferInitialize); R(BufferGetAddress); R(BufferGetSize);
    set_proc("nvnBufferMap", BufferGetAddress);
    R(TextureBuilderSetDefaults); R(TextureBuilderSetFlags); R(TextureBuilderSetTarget); R(TextureBuilderSetWidth);
    R(TextureBuilderSetHeight); R(TextureBuilderSetDepth); R(TextureBuilderSetSize1D); R(TextureBuilderSetSize2D);
    R(TextureBuilderSetSize3D); R(TextureBuilderSetLevels); R(TextureBuilderSetFormat); R(TextureBuilderSetSamples);
    R(TextureBuilderSetStride); R(TextureBuilderSetStorage); R(TextureBuilderSetPackagedTextureLayout);
    R(TextureBuilderGetStorageSize); R(TextureBuilderGetStorageAlignment); R(TextureInitialize); R(TextureFinalize);
    R(TextureGetWidth); R(TextureGetHeight); R(TextureGetDepth); R(TextureGetLevels); R(TextureGetFormat);
    R(TextureGetTarget); R(TextureGetSamples); R(TextureGetFlags); R(TextureGetTextureAddress); R(TextureWriteTexels);
    R(TextureFlushTexels);
    R(TextureViewSetDefaults); R(TextureViewSetLevels); R(TextureViewSetLayers); R(TextureViewSetFormat);
    R(TextureViewSetSwizzle); R(TextureViewSetDepthStencilMode); R(TextureViewSetTarget); R(TextureViewGetLevels);
    R(TextureViewGetLayers); R(TextureViewGetTarget); R(TextureViewGetFormat);
    R(SamplerBuilderSetDefaults); R(SamplerBuilderSetMinMagFilter); R(SamplerBuilderSetWrapMode); R(SamplerBuilderSetLodClamp);
    R(SamplerBuilderSetLodBias); R(SamplerBuilderSetMaxAnisotropy); R(SamplerBuilderSetCompare); R(SamplerBuilderSetReductionFilter);
    R(SamplerBuilderSetBorderColor); R(SamplerInitialize); R(SamplerFinalize);
    R(TexturePoolInitialize); R(SamplerPoolInitialize); R(TexturePoolRegisterTexture); R(SamplerPoolRegisterSampler);
    R(BlendStateSetDefaults); R(BlendStateSetBlendTarget); R(BlendStateSetBlendFunc); R(BlendStateSetBlendEquation);
    R(ColorStateSetDefaults); R(ColorStateSetBlendEnable); R(ColorStateSetLogicOp); R(ColorStateSetAlphaTest);
    R(ChannelMaskStateSetDefaults); R(ChannelMaskStateSetChannelMask);
    R(DepthStencilStateSetDefaults); R(DepthStencilStateSetDepthTestEnable); R(DepthStencilStateSetDepthWriteEnable);
    R(DepthStencilStateSetStencilTestEnable); R(DepthStencilStateSetDepthFunc); R(DepthStencilStateSetStencilFunc);
    R(DepthStencilStateSetStencilOp);
    R(MultisampleStateSetDefaults); R(MultisampleStateSetMultisampleEnable); R(MultisampleStateSetSamples);
    R(MultisampleStateSetAlphaToCoverageEnable);
    R(PolygonStateSetDefaults); R(PolygonStateSetCullFace); R(PolygonStateSetFrontFace); R(PolygonStateSetPolygonMode);
    R(PolygonStateSetPolygonOffsetEnables);
    R(VertexAttribStateSetDefaults); R(VertexAttribStateSetFormat); R(VertexAttribStateSetStreamIndex);
    R(VertexStreamStateSetDefaults); R(VertexStreamStateSetStride); R(VertexStreamStateSetDivisor);
    R(ProgramInitialize); R(ProgramFinalize); R(ProgramSetShaders);
    R(CommandBufferInitialize); R(CommandBufferFinalize); R(CommandBufferBeginRecording); R(CommandBufferEndRecording);
    R(CommandBufferIsRecording); R(CommandBufferCallCommands); R(CommandBufferBindProgram); R(CommandBufferBindUniformBuffer);
    R(CommandBufferBindStorageBuffer); R(CommandBufferBindTexture); R(CommandBufferBindVertexBuffer);
    R(CommandBufferBindVertexBuffers); R(CommandBufferBindVertexAttribState); R(CommandBufferBindVertexStreamState);
    R(CommandBufferBindBlendState); R(CommandBufferBindColorState); R(CommandBufferBindChannelMaskState);
    R(CommandBufferBindDepthStencilState); R(CommandBufferBindMultisampleState); R(CommandBufferBindPolygonState);
    R(CommandBufferSetSampleMask); R(CommandBufferSetViewports); R(CommandBufferSetViewport); R(CommandBufferSetScissors);
    R(CommandBufferSetScissor); R(CommandBufferSetDepthRange); R(CommandBufferSetDepthRanges); R(CommandBufferSetBlendColor);
    R(CommandBufferSetStencilRef); R(CommandBufferSetStencilMask); R(CommandBufferSetStencilValueMask);
    R(CommandBufferSetLineWidth); R(CommandBufferSetDepthBounds); R(CommandBufferSetDepthClamp);
    R(CommandBufferSetRasterizerDiscard); R(CommandBufferSetPolygonOffsetClamp); R(CommandBufferSetRenderTargets);
    R(CommandBufferClearColor); R(CommandBufferClearDepthStencil); R(CommandBufferClearTexture); R(CommandBufferBarrier);
    set_proc("nvnCommandBufferClearColori", CommandBufferClearColor);
    set_proc("nvnCommandBufferClearColorui", CommandBufferClearColor);
    set_proc("nvnCommandBufferClearTexturei", CommandBufferClearTexture);
    set_proc("nvnCommandBufferClearTextureui", CommandBufferClearTexture);
    R(CommandBufferDrawElements); R(CommandBufferDrawElementsBaseVertex); R(CommandBufferDrawElementsInstanced);
    R(CommandBufferDrawArrays); R(CommandBufferDrawArraysInstanced);
    R(CommandBufferCopyBufferToBuffer); R(CommandBufferCopyBufferToTexture); R(CommandBufferCopyTextureToTexture);
    R(CommandBufferReportCounter);
    for (const char* n : {"nvnCommandBufferAddCommandMemory", "nvnCommandBufferAddControlMemory", "nvnCommandBufferSetMemoryCallback",
                          "nvnCommandBufferSetMemoryCallbackData", "nvnCommandBufferSetTexturePool", "nvnCommandBufferSetSamplerPool",
                          "nvnCommandBufferSetConservativeRasterEnable"})
        set_proc(n, CommandBufferNoop);
    R(QueueInitialize); R(QueueSubmitCommands); R(QueueFlush); R(QueueFinish); R(QueueFenceSync); R(QueueWaitSync);
    R(SyncInitialize); R(SyncWait);
    R(WindowBuilderSetDefaults); R(WindowBuilderSetTextures); R(WindowBuilderSetPresentInterval); R(WindowInitialize);
    R(WindowAcquireTexture); R(WindowSetPresentInterval); R(WindowSetCrop); R(WindowGetPresentInterval); R(QueuePresentTexture);
#undef R
}

}  // namespace gpu
