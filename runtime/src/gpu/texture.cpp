// NVN formats and Tegra X1 block-linear layout (see texture.h).
// Block-linear: 64-byte x 8-row GOBs (512 bytes), stacked `gob_bh` high into blocks;
// smaller mip levels shrink the block height (the rule Ryujinx documents for Maxwell).
#include "texture.h"

#include <algorithm>
#include <cstring>

#include "../perf.h"
#include "../rt.h"

namespace gpu {

namespace {
constexpr uint32_t kGobW = 64, kGobH = 8, kGobSize = 512;

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

FormatInfo F(const char* n, VkFormat img, VkFormat vtx, uint8_t bpb, uint8_t kind = 0, uint8_t aspect = 0,
             Conv conv = Conv::None, bool x = false, uint8_t bw = 1, uint8_t bh = 1) {
    FormatInfo f;
    f.name = n;
    f.image = img;
    f.vertex = vtx;
    f.bpb = bpb;
    f.kind = kind;
    f.aspect = aspect;
    f.conv = conv;
    f.force_alpha_one = x;
    f.bw = bw;
    f.bh = bh;
    return f;
}

std::vector<FormatInfo> build_table() {
    std::vector<FormatInfo> t(0x9D);
    auto U = VK_FORMAT_UNDEFINED;
    t[0x01] = F("R8", VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1);
    t[0x02] = F("R8SN", VK_FORMAT_R8_SNORM, VK_FORMAT_R8_SNORM, 1);
    t[0x03] = F("R8UI", VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 1, 1);
    t[0x04] = F("R8I", VK_FORMAT_R8_SINT, VK_FORMAT_R8_SINT, 1, 2);
    t[0x05] = F("R16F", VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, 2);
    t[0x06] = F("R16", VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 2);
    t[0x07] = F("R16SN", VK_FORMAT_R16_SNORM, VK_FORMAT_R16_SNORM, 2);
    t[0x08] = F("R16UI", VK_FORMAT_R16_UINT, VK_FORMAT_R16_UINT, 2, 1);
    t[0x09] = F("R16I", VK_FORMAT_R16_SINT, VK_FORMAT_R16_SINT, 2, 2);
    t[0x0A] = F("R32F", VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, 4);
    t[0x0B] = F("R32UI", VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, 4, 1);
    t[0x0C] = F("R32I", VK_FORMAT_R32_SINT, VK_FORMAT_R32_SINT, 4, 2);
    t[0x0D] = F("RG8", VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 2);
    t[0x0E] = F("RG8SN", VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_SNORM, 2);
    t[0x0F] = F("RG8UI", VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_UINT, 2, 1);
    t[0x10] = F("RG8I", VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SINT, 2, 2);
    t[0x11] = F("RG16F", VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, 4);
    t[0x12] = F("RG16", VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, 4);
    t[0x13] = F("RG16SN", VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SNORM, 4);
    t[0x14] = F("RG16UI", VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_UINT, 4, 1);
    t[0x15] = F("RG16I", VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SINT, 4, 2);
    t[0x16] = F("RG32F", VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, 8);
    t[0x17] = F("RG32UI", VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, 8, 1);
    t[0x18] = F("RG32I", VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SINT, 8, 2);
    t[0x19] = F("RGB8", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8_UNORM, 3, 0, 0, Conv::RGB_to_RGBA);
    t[0x1A] = F("RGB8SN", VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8_SNORM, 3, 0, 0, Conv::RGB_to_RGBA);
    t[0x1B] = F("RGB8UI", VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8_UINT, 3, 1, 0, Conv::RGB_to_RGBA);
    t[0x1C] = F("RGB8I", VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8_SINT, 3, 2, 0, Conv::RGB_to_RGBA);
    t[0x1D] = F("RGB16F", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, 6, 0, 0, Conv::RGB_to_RGBA);
    t[0x1E] = F("RGB16", VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16_UNORM, 6, 0, 0, Conv::RGB_to_RGBA);
    t[0x1F] = F("RGB16SN", VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16_SNORM, 6, 0, 0, Conv::RGB_to_RGBA);
    t[0x20] = F("RGB16UI", VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16_UINT, 6, 1, 0, Conv::RGB_to_RGBA);
    t[0x21] = F("RGB16I", VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16_SINT, 6, 2, 0, Conv::RGB_to_RGBA);
    t[0x22] = F("RGB32F", VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, 12, 0, 0, Conv::RGB_to_RGBA);
    t[0x23] = F("RGB32UI", VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32_UINT, 12, 1, 0, Conv::RGB_to_RGBA);
    t[0x24] = F("RGB32I", VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32_SINT, 12, 2, 0, Conv::RGB_to_RGBA);
    t[0x25] = F("RGBA8", VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4);
    t[0x26] = F("RGBA8SN", VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 4);
    t[0x27] = F("RGBA8UI", VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, 4, 1);
    t[0x28] = F("RGBA8I", VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT, 4, 2);
    t[0x29] = F("RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8);
    t[0x2A] = F("RGBA16", VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 8);
    t[0x2B] = F("RGBA16SN", VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, 8);
    t[0x2C] = F("RGBA16UI", VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT, 8, 1);
    t[0x2D] = F("RGBA16I", VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT, 8, 2);
    t[0x2E] = F("RGBA32F", VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    t[0x2F] = F("RGBA32UI", VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, 16, 1);
    t[0x30] = F("RGBA32I", VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SINT, 16, 2);
    t[0x31] = F("STENCIL8", VK_FORMAT_S8_UINT, U, 1, 1, 2);
    t[0x32] = F("DEPTH16", VK_FORMAT_D16_UNORM, U, 2, 0, 1);
    t[0x33] = F("DEPTH24", VK_FORMAT_X8_D24_UNORM_PACK32, U, 4, 0, 1);
    t[0x34] = F("DEPTH32F", VK_FORMAT_D32_SFLOAT, U, 4, 0, 1);
    t[0x35] = F("DEPTH24_STENCIL8", VK_FORMAT_D24_UNORM_S8_UINT, U, 4, 0, 3);
    t[0x36] = F("DEPTH32F_STENCIL8", VK_FORMAT_D32_SFLOAT_S8_UINT, U, 8, 0, 3);
    t[0x37] = F("RGBX8_SRGB", VK_FORMAT_R8G8B8A8_SRGB, U, 4, 0, 0, Conv::None, true);
    t[0x38] = F("RGBA8_SRGB", VK_FORMAT_R8G8B8A8_SRGB, U, 4);
    t[0x39] = F("RGBA4", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P4444);
    t[0x3A] = F("RGB5", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P5551, true);
    t[0x3B] = F("RGB5A1", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P5551);
    t[0x3C] = F("RGB565", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P565);
    t[0x3D] = F("RGB10A2", VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4);
    t[0x3E] = F("RGB10A2UI", VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 1);
    t[0x3F] = F("R11G11B10F", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4);
    t[0x40] = F("RGB9E5F", VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, U, 4);
    t[0x41] = F("RGB_DXT1", VK_FORMAT_BC1_RGB_UNORM_BLOCK, U, 8, 0, 0, Conv::None, false, 4, 4);
    t[0x42] = F("RGBA_DXT1", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, U, 8, 0, 0, Conv::None, false, 4, 4);
    t[0x43] = F("RGBA_DXT3", VK_FORMAT_BC2_UNORM_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x44] = F("RGBA_DXT5", VK_FORMAT_BC3_UNORM_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x45] = F("RGB_DXT1_SRGB", VK_FORMAT_BC1_RGB_SRGB_BLOCK, U, 8, 0, 0, Conv::None, false, 4, 4);
    t[0x46] = F("RGBA_DXT1_SRGB", VK_FORMAT_BC1_RGBA_SRGB_BLOCK, U, 8, 0, 0, Conv::None, false, 4, 4);
    t[0x47] = F("RGBA_DXT3_SRGB", VK_FORMAT_BC2_SRGB_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x48] = F("RGBA_DXT5_SRGB", VK_FORMAT_BC3_SRGB_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x49] = F("RGTC1_UNORM", VK_FORMAT_BC4_UNORM_BLOCK, U, 8, 0, 0, Conv::None, false, 4, 4);
    t[0x4A] = F("RGTC1_SNORM", VK_FORMAT_BC4_SNORM_BLOCK, U, 8, 0, 0, Conv::None, false, 4, 4);
    t[0x4B] = F("RGTC2_UNORM", VK_FORMAT_BC5_UNORM_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x4C] = F("RGTC2_SNORM", VK_FORMAT_BC5_SNORM_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x4D] = F("BPTC_UNORM", VK_FORMAT_BC7_UNORM_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x4E] = F("BPTC_UNORM_SRGB", VK_FORMAT_BC7_SRGB_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x4F] = F("BPTC_SFLOAT", VK_FORMAT_BC6H_SFLOAT_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    t[0x50] = F("BPTC_UFLOAT", VK_FORMAT_BC6H_UFLOAT_BLOCK, U, 16, 0, 0, Conv::None, false, 4, 4);
    // integer data converted to float without normalization: vertex formats only
    const VkFormat i2f[][2] = {
        {VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED}, {VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED},
        {VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT},     {VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED},
        {VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED}, {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT},
        {VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED}, {VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED},
        {VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT}, {VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED},
        {VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED}, {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT},
    };
    const uint8_t i2f_size[] = {1, 2, 4, 2, 4, 8, 3, 6, 12, 4, 8, 16};
    for (int i = 0; i < 12; i++) {
        t[(size_t)(0x51 + 2 * i)] = F("UI2F", U, i2f[i][0], i2f_size[i]);
        t[(size_t)(0x52 + 2 * i)] = F("I2F", U, i2f[i][1], i2f_size[i]);
    }
    t[0x69] = F("RGB10A2SN", VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 4);
    t[0x6A] = F("RGB10A2I", VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32, 4, 2);
    t[0x6B] = F("RGB10A2_UI2F", U, VK_FORMAT_A2B10G10R10_USCALED_PACK32, 4);
    t[0x6C] = F("RGB10A2_I2F", U, VK_FORMAT_A2B10G10R10_SSCALED_PACK32, 4);
    t[0x6D] = F("RGBX8", VK_FORMAT_R8G8B8A8_UNORM, U, 4, 0, 0, Conv::None, true);
    t[0x6E] = F("RGBX8SN", VK_FORMAT_R8G8B8A8_SNORM, U, 4, 0, 0, Conv::None, true);
    t[0x6F] = F("RGBX8UI", VK_FORMAT_R8G8B8A8_UINT, U, 4, 1, 0, Conv::None, true);
    t[0x70] = F("RGBX8I", VK_FORMAT_R8G8B8A8_SINT, U, 4, 2, 0, Conv::None, true);
    t[0x71] = F("RGBX16F", VK_FORMAT_R16G16B16A16_SFLOAT, U, 8, 0, 0, Conv::None, true);
    t[0x72] = F("RGBX16", VK_FORMAT_R16G16B16A16_UNORM, U, 8, 0, 0, Conv::None, true);
    t[0x73] = F("RGBX16SN", VK_FORMAT_R16G16B16A16_SNORM, U, 8, 0, 0, Conv::None, true);
    t[0x74] = F("RGBX16UI", VK_FORMAT_R16G16B16A16_UINT, U, 8, 1, 0, Conv::None, true);
    t[0x75] = F("RGBX16I", VK_FORMAT_R16G16B16A16_SINT, U, 8, 2, 0, Conv::None, true);
    t[0x76] = F("RGBX32F", VK_FORMAT_R32G32B32A32_SFLOAT, U, 16, 0, 0, Conv::None, true);
    t[0x77] = F("RGBX32UI", VK_FORMAT_R32G32B32A32_UINT, U, 16, 1, 0, Conv::None, true);
    t[0x78] = F("RGBX32I", VK_FORMAT_R32G32B32A32_SINT, U, 16, 2, 0, Conv::None, true);
    static const uint8_t astc_dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                             {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (int i = 0; i < 14; i++) {
        t[(size_t)(0x79 + i)] = F("ASTC", VK_FORMAT_R8G8B8A8_UNORM, U, 16, 0, 0, Conv::ASTC, false, astc_dims[i][0], astc_dims[i][1]);
        t[(size_t)(0x87 + i)] = F("ASTC_SRGB", VK_FORMAT_R8G8B8A8_SRGB, U, 16, 0, 0, Conv::ASTC, false, astc_dims[i][0], astc_dims[i][1]);
    }
    t[0x95] = F("BGR565", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P565_BGR);
    t[0x96] = F("BGR5", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P5551, true);
    t[0x97] = F("BGR5A1", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P5551);
    t[0x98] = F("A1BGR5", VK_FORMAT_R8G8B8A8_UNORM, U, 2, 0, 0, Conv::P1555);
    t[0x99] = F("BGRX8", VK_FORMAT_B8G8R8A8_UNORM, U, 4, 0, 0, Conv::None, true);
    t[0x9A] = F("BGRA8", VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4);
    t[0x9B] = F("BGRX8_SRGB", VK_FORMAT_B8G8R8A8_SRGB, U, 4, 0, 0, Conv::None, true);
    t[0x9C] = F("BGRA8_SRGB", VK_FORMAT_B8G8R8A8_SRGB, U, 4);
    // the BGR 16-bit packings swap red and blue relative to their RGB twins
    return t;
}
}  // namespace

const FormatInfo& format_info(uint32_t nvn_format) {
    static const std::vector<FormatInfo> table = build_table();
    static const FormatInfo none;
    return nvn_format < table.size() ? table[nvn_format] : none;
}

uint32_t default_block_height_log2(uint32_t height_in_blocks) {
    uint32_t gobs = div_up(height_in_blocks, kGobH);
    uint32_t log2 = 0;
    while (log2 < 4 && (1u << log2) < gobs) log2++;
    return log2;
}

TexLayout compute_layout(const FormatInfo& f, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t layers,
                         uint32_t bh_log2, bool linear, uint32_t stride) {
    TexLayout L;
    L.linear = linear;
    L.stride = stride;
    uint64_t off = 0;
    uint32_t bpb = f.bpb ? f.bpb : 4;
    for (uint32_t lv = 0; lv < std::max(levels, 1u); lv++) {
        LevelLayout l;
        l.w = std::max(1u, w >> lv);
        l.h = std::max(1u, h >> lv);
        l.d = std::max(1u, d >> lv);
        l.wb = div_up(l.w, f.bw);
        l.hb = div_up(l.h, f.bh);
        if (linear) {
            uint32_t pitch = lv == 0 && stride ? stride : l.wb * bpb;
            l.size = (uint64_t)pitch * l.hb * l.d;
        } else {
            uint32_t gbh = 1u << bh_log2;
            while (l.hb <= (gbh >> 1) * kGobH && gbh != 1) gbh >>= 1;
            l.gob_bh = gbh;
            l.gobs_x = div_up(l.wb * bpb, kGobW);
            uint64_t rows = div_up(div_up(l.hb, kGobH), gbh);
            l.size = (uint64_t)l.gobs_x * gbh * kGobSize * rows * l.d;
        }
        l.offset = off;
        off += l.size;
        L.levels.push_back(l);
    }
    if (!linear && layers > 1) {
        // layers are aligned to level 0's block of GOBs
        uint32_t gbh = 1u << bh_log2;
        uint32_t hb = div_up(h, f.bh);
        while (hb <= (gbh >> 1) * kGobH && gbh != 1) gbh >>= 1;
        uint64_t block = (uint64_t)gbh * kGobSize;
        off = (off + block - 1) / block * block;
    }
    L.layer_stride = off;
    L.total = off * std::max(layers, 1u);
    return L;
}

uint32_t image_bytes_per_block(const FormatInfo& f) {
    switch (f.conv) {
    case Conv::None: return f.bpb;
    case Conv::RGB_to_RGBA: return f.bpb / 3 * 4;
    case Conv::ASTC: return 4;                      // per texel (decoded)
    default: return 4;                              // 16-bit packings -> RGBA8
    }
}

namespace {
uint8_t ext5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
uint8_t ext6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
uint8_t ext4(uint32_t v) { return (uint8_t)((v << 4) | v); }

// guest texels (tightly packed rows, `n` texels) -> image format texels
void convert_texels(const FormatInfo& f, const uint8_t* src, uint8_t* dst, size_t n) {
    switch (f.conv) {
    case Conv::None: memcpy(dst, src, n * f.bpb); return;
    case Conv::RGB_to_RGBA: {
        uint32_t c = f.bpb / 3;                      // bytes per component
        for (size_t i = 0; i < n; i++) {
            memcpy(dst + i * 4 * c, src + i * 3 * c, 3 * c);
            uint8_t* a = dst + i * 4 * c + 3 * c;
            if (c == 1) a[0] = f.kind == 0 ? 0xFF : 1;
            else if (c == 2) { uint16_t one = f.kind ? 1 : (f.image == VK_FORMAT_R16G16B16A16_SFLOAT ? 0x3C00 : 0xFFFF); memcpy(a, &one, 2); }
            else { uint32_t one = f.kind ? 1 : 0x3F800000; memcpy(a, &one, 4); }
        }
        return;
    }
    default: break;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)src[2 * i] | ((uint32_t)src[2 * i + 1] << 8);
        uint8_t* o = dst + 4 * i;
        switch (f.conv) {
        case Conv::P5551: o[0] = ext5(v & 31); o[1] = ext5((v >> 5) & 31); o[2] = ext5((v >> 10) & 31); o[3] = (v >> 15) ? 255 : 0; break;
        case Conv::P1555: o[3] = (v & 1) ? 255 : 0; o[0] = ext5((v >> 1) & 31); o[1] = ext5((v >> 6) & 31); o[2] = ext5((v >> 11) & 31); break;
        case Conv::P565: o[0] = ext5(v & 31); o[1] = ext6((v >> 5) & 63); o[2] = ext5(v >> 11); o[3] = 255; break;
        case Conv::P565_BGR: o[2] = ext5(v & 31); o[1] = ext6((v >> 5) & 63); o[0] = ext5(v >> 11); o[3] = 255; break;
        case Conv::P4444: o[0] = ext4(v & 15); o[1] = ext4((v >> 4) & 15); o[2] = ext4((v >> 8) & 15); o[3] = ext4(v >> 12); break;
        default: break;
        }
        if (f.force_alpha_one) o[3] = 255;
    }
}

// texel block (bx, by) of a block-linear level -> byte offset in the level
inline uint64_t bl_offset(const LevelLayout& l, uint32_t xbytes, uint32_t y) {
    uint32_t gbh = l.gob_bh;
    uint64_t block_row = y / (kGobH * gbh), gob_x = xbytes / kGobW, gob_in_block = (y % (kGobH * gbh)) / kGobH;
    uint64_t off = block_row * ((uint64_t)l.gobs_x * gbh * kGobSize) + gob_x * ((uint64_t)gbh * kGobSize) + gob_in_block * kGobSize;
    uint32_t xg = xbytes % kGobW, yg = y % kGobH;
    return off + ((xg / 32) * 256) + ((yg / 2) * 64) + (((xg % 32) / 16) * 32) + ((yg % 2) * 16) + (xg % 16);
}
}  // namespace

size_t read_level(const FormatInfo& f, const TexLayout& L, const uint8_t* base, uint32_t layer, uint32_t level,
                  std::vector<uint8_t>& out) {
    const LevelLayout& l = L.levels[level];
    const uint8_t* src = base + (uint64_t)layer * L.layer_stride + l.offset;
    uint32_t row_bytes = l.wb * f.bpb;
    // guest blocks, tightly packed rows
    std::vector<uint8_t> packed((size_t)row_bytes * l.hb);
    if (L.linear) {
        uint32_t pitch = level == 0 && L.stride ? L.stride : row_bytes;
        for (uint32_t y = 0; y < l.hb; y++) memcpy(packed.data() + (size_t)y * row_bytes, src + (uint64_t)y * pitch, row_bytes);
    } else {
        for (uint32_t y = 0; y < l.hb; y++) {
            uint8_t* row = packed.data() + (size_t)y * row_bytes;
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = std::min(16u, row_bytes - x);
                memcpy(row + x, src + bl_offset(l, x, y), n);
            }
        }
    }
    rt::perf::Scope timed(rt::perf::TextureDecode);
    if (f.conv == Conv::ASTC) {
        // decode block by block into RGBA8 texels
        out.resize((size_t)l.w * l.h * 4);
        std::vector<uint8_t> tmp((size_t)f.bw * f.bh * 4);
        for (uint32_t by = 0; by < l.hb; by++)
            for (uint32_t bx = 0; bx < l.wb; bx++) {
                astc_decode_block(packed.data() + (size_t)by * row_bytes + (size_t)bx * 16, f.bw, f.bh, tmp.data(), f.bw * 4);
                for (uint32_t py = 0; py < f.bh && by * f.bh + py < l.h; py++) {
                    uint32_t px_n = std::min<uint32_t>(f.bw, l.w - bx * f.bw);
                    memcpy(out.data() + ((size_t)(by * f.bh + py) * l.w + bx * f.bw) * 4, tmp.data() + (size_t)py * f.bw * 4, (size_t)px_n * 4);
                }
            }
        return out.size();
    }
    size_t texels = (size_t)l.wb * l.hb;      // blocks (= texels for uncompressed formats)
    out.resize(texels * image_bytes_per_block(f));
    convert_texels(f, packed.data(), out.data(), texels);
    return out.size();
}

void convert_linear(const FormatInfo& f, const uint8_t* src, size_t src_size, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    rt::perf::Scope timed(rt::perf::TextureDecode);
    if (f.conv == Conv::ASTC) {
        uint32_t wb = div_up(w, f.bw), hb = div_up(h, f.bh);
        out.assign((size_t)w * h * 4, 0);
        std::vector<uint8_t> tmp((size_t)f.bw * f.bh * 4);
        for (uint32_t by = 0; by < hb; by++)
            for (uint32_t bx = 0; bx < wb; bx++) {
                size_t o = ((size_t)by * wb + bx) * 16;
                if (o + 16 > src_size) return;
                astc_decode_block(src + o, f.bw, f.bh, tmp.data(), f.bw * 4);
                for (uint32_t py = 0; py < f.bh && by * f.bh + py < h; py++)
                    memcpy(out.data() + ((size_t)(by * f.bh + py) * w + bx * f.bw) * 4, tmp.data() + (size_t)py * f.bw * 4,
                           (size_t)std::min<uint32_t>(f.bw, w - bx * f.bw) * 4);
            }
        return;
    }
    size_t blocks = (size_t)div_up(w, f.bw) * div_up(h, f.bh);
    if (blocks * f.bpb > src_size) blocks = src_size / std::max<uint8_t>(f.bpb, 1);
    out.resize(blocks * image_bytes_per_block(f));
    convert_texels(f, src, out.data(), blocks);
}

}  // namespace gpu
