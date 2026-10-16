// NVN texture formats and the Tegra X1 block-linear layout.
#pragma once
#include <cstdint>
#include <vector>

#include "vk.h"

namespace gpu {

enum class Conv : uint8_t {
    None,
    RGB_to_RGBA,       // 3-component texel -> 4 components (alpha/1 appended)
    P5551,             // 16-bit packed, R in the low bits -> RGBA8
    P1555,             // 16-bit, A in the low bit (A1BGR5) -> RGBA8
    P565,              // 16-bit 5/6/5, R low -> RGBA8
    P565_BGR,          // 16-bit 5/6/5, B low -> RGBA8
    P4444,             // 16-bit 4/4/4/4, R low -> RGBA8
    ASTC,              // decoded to RGBA8
};

struct FormatInfo {
    const char* name = "?";
    VkFormat image = VK_FORMAT_UNDEFINED;     // Vulkan image format (after conversion)
    VkFormat vertex = VK_FORMAT_UNDEFINED;    // as a vertex attribute
    uint8_t bw = 1, bh = 1, bpb = 0;          // block size and bytes per block in guest memory
    uint8_t kind = 0;                         // 0 float/normalized, 1 uint, 2 sint
    uint8_t aspect = 0;                       // 0 color, 1 depth, 2 stencil, 3 depth+stencil
    Conv conv = Conv::None;
    bool force_alpha_one = false;             // RGBX formats
};
const FormatInfo& format_info(uint32_t nvn_format);

// Layout of one texture's data in guest memory.
struct LevelLayout {
    uint64_t offset = 0, size = 0;
    uint32_t w = 1, h = 1, d = 1;             // texels
    uint32_t wb = 1, hb = 1;                  // blocks
    uint32_t gob_bh = 1;                      // block height in GOBs used by this level
    uint32_t gobs_x = 1;
};
struct TexLayout {
    bool linear = false;
    uint32_t stride = 0;                      // linear: row pitch in bytes
    uint64_t layer_stride = 0, total = 0;
    std::vector<LevelLayout> levels;
};
// block_height_log2: GOBs per block of level 0 (from the packaged layout, or chosen by the driver)
TexLayout compute_layout(const FormatInfo& f, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t layers,
                         uint32_t block_height_log2, bool linear, uint32_t stride);
// NVN's own block height choice for a texture of `height_in_blocks`
uint32_t default_block_height_log2(uint32_t height_in_blocks);

// one level of one layer from guest memory to tightly packed texels in the image format
// (deswizzled and converted); returns the bytes written to `out`
size_t read_level(const FormatInfo& f, const TexLayout& L, const uint8_t* base, uint32_t layer, uint32_t level,
                  std::vector<uint8_t>& out);
// bytes per block / block size of the Vulkan image format the data is converted to
uint32_t image_bytes_per_block(const FormatInfo& f);

// tightly packed linear texels in the guest format (nvnTextureWriteTexels) -> image format
void convert_linear(const FormatInfo& f, const uint8_t* src, size_t src_size, uint32_t w, uint32_t h,
                    std::vector<uint8_t>& out);

// ASTC LDR block decode (astc.cpp): one block -> bw*bh RGBA8 texels
void astc_decode_block(const uint8_t block[16], int bw, int bh, uint8_t* rgba, int out_stride);

}  // namespace gpu
