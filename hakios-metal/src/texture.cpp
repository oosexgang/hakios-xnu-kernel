#include "hakios-metal/texture.hpp"

#include <algorithm>
#include <cstring>

namespace hakios::metal {

namespace {

using namespace a6xx;

std::string n(uint64_t v) { return std::to_string(v); }

bool refuse(std::string* why, const std::string& text) {
    *why = text;
    return false;
}

// The formats served, each with what fd6_format_table.c says of it: the .tex column (which is also the .rb one
// for every format here, so one FMT serves the sampled and the storage descriptor), the swap (fd6_pipe2swap: WXYZ for
// the BGRA pair at :174, :176; WZYX for the rest), and whether it has alpha (fdl6_format_swiz reads W as ONE without
// one, fd6_view.c:117-120). The sRGB pair is the UNORM format plus TEX_CONST_0_SRGB (:165, :176). The element class
// is Metal's rule for texture2d<T>: unorm, float and sRGB formats are read as float (or half), *Uint as uint.
constexpr FormatInfo kFormats[] = {
    // A8: a format of its own to the TP and the RB, which put the one channel in alpha themselves, so the identity
    // swizzle reads (0, 0, 0, a) as Metal's A8Unorm does (fd6_format_table.c:53 _TC(A8_UNORM, A8_UNORM, WZYX))
    {PixelFormat::kA8Unorm, FMT6_A8_UNORM, 1, WZYX, true, false, kElemFloat, "A8Unorm"},
    {PixelFormat::kR8Unorm, FMT6_8_UNORM, 1, WZYX, false, false, kElemFloat, "R8Unorm"},
    {PixelFormat::kR8Snorm, FMT6_8_SNORM, 1, WZYX, false, false, kElemFloat, "R8Snorm"},
    {PixelFormat::kR8Uint, FMT6_8_UINT, 1, WZYX, false, false, kElemUint, "R8Uint"},
    {PixelFormat::kR8Sint, FMT6_8_SINT, 1, WZYX, false, false, kElemInt, "R8Sint"},
    {PixelFormat::kR16Snorm, FMT6_16_SNORM, 2, WZYX, false, false, kElemFloat, "R16Snorm"},
    {PixelFormat::kRG8Snorm, FMT6_8_8_SNORM, 2, WZYX, false, false, kElemFloat, "RG8Snorm"},
    {PixelFormat::kRG8Uint, FMT6_8_8_UINT, 2, WZYX, false, false, kElemUint, "RG8Uint"},
    {PixelFormat::kRG8Sint, FMT6_8_8_SINT, 2, WZYX, false, false, kElemInt, "RG8Sint"},
    {PixelFormat::kR32Sint, FMT6_32_SINT, 4, WZYX, false, false, kElemInt, "R32Sint"},
    {PixelFormat::kRG16Unorm, FMT6_16_16_UNORM, 4, WZYX, false, false, kElemFloat, "RG16Unorm"},
    {PixelFormat::kRG16Snorm, FMT6_16_16_SNORM, 4, WZYX, false, false, kElemFloat, "RG16Snorm"},
    {PixelFormat::kRG16Uint, FMT6_16_16_UINT, 4, WZYX, false, false, kElemUint, "RG16Uint"},
    {PixelFormat::kRG16Sint, FMT6_16_16_SINT, 4, WZYX, false, false, kElemInt, "RG16Sint"},
    {PixelFormat::kRGBA8Snorm, FMT6_8_8_8_8_SNORM, 4, WZYX, true, false, kElemFloat, "RGBA8Snorm"},
    {PixelFormat::kRGBA8Uint, FMT6_8_8_8_8_UINT, 4, WZYX, true, false, kElemUint, "RGBA8Uint"},
    {PixelFormat::kRGBA8Sint, FMT6_8_8_8_8_SINT, 4, WZYX, true, false, kElemInt, "RGBA8Sint"},
    {PixelFormat::kRG32Uint, FMT6_32_32_UINT, 8, WZYX, false, false, kElemUint, "RG32Uint"},
    {PixelFormat::kRG32Sint, FMT6_32_32_SINT, 8, WZYX, false, false, kElemInt, "RG32Sint"},
    {PixelFormat::kRGBA16Unorm, FMT6_16_16_16_16_UNORM, 8, WZYX, true, false, kElemFloat, "RGBA16Unorm"},
    {PixelFormat::kRGBA16Snorm, FMT6_16_16_16_16_SNORM, 8, WZYX, true, false, kElemFloat, "RGBA16Snorm"},
    {PixelFormat::kRGBA16Uint, FMT6_16_16_16_16_UINT, 8, WZYX, true, false, kElemUint, "RGBA16Uint"},
    {PixelFormat::kRGBA16Sint, FMT6_16_16_16_16_SINT, 8, WZYX, true, false, kElemInt, "RGBA16Sint"},
    {PixelFormat::kRGBA32Sint, FMT6_32_32_32_32_SINT, 16, WZYX, true, false, kElemInt, "RGBA32Sint"},
    // the two packed 16-bit formats: Metal names their channels from the low bits up, as Gallium does, so they are
    // Gallium's B5G6R5_UNORM and B5G5R5A1_UNORM, both WXYZ to the TP and the RB (fd6_format_table.c:108, 111)
    {PixelFormat::kB5G6R5Unorm, FMT6_5_6_5_UNORM, 2, WXYZ, false, false, kElemFloat, "B5G6R5Unorm"},
    {PixelFormat::kBGR5A1Unorm, FMT6_5_5_5_1_UNORM, 2, WXYZ, true, false, kElemFloat, "BGR5A1Unorm"},
    {PixelFormat::kR16Float, FMT6_16_FLOAT, 2, WZYX, false, false, kElemFloat, "R16Float"},
    {PixelFormat::kR16Unorm, FMT6_16_UNORM, 2, WZYX, false, false, kElemFloat, "R16Unorm"},
    {PixelFormat::kR16Uint, FMT6_16_UINT, 2, WZYX, false, false, kElemUint, "R16Uint"},
    {PixelFormat::kR16Sint, FMT6_16_SINT, 2, WZYX, false, false, kElemInt, "R16Sint"},
    {PixelFormat::kRG8Unorm, FMT6_8_8_UNORM, 2, WZYX, false, false, kElemFloat, "RG8Unorm"},
    {PixelFormat::kRG16Float, FMT6_16_16_FLOAT, 4, WZYX, false, false, kElemFloat, "RG16Float"},
    {PixelFormat::kRG32Float, FMT6_32_32_FLOAT, 8, WZYX, false, false, kElemFloat, "RG32Float"},
    {PixelFormat::kR32Uint, FMT6_32_UINT, 4, WZYX, false, false, kElemUint, "R32Uint"},
    {PixelFormat::kR32Float, FMT6_32_FLOAT, 4, WZYX, false, false, kElemFloat, "R32Float"},
    {PixelFormat::kRGBA8Unorm, FMT6_8_8_8_8_UNORM, 4, WZYX, true, false, kElemFloat, "RGBA8Unorm"},
    {PixelFormat::kRGBA8UnormSrgb, FMT6_8_8_8_8_UNORM, 4, WZYX, true, true, kElemFloat, "RGBA8Unorm_sRGB"},
    {PixelFormat::kBGRA8Unorm, FMT6_8_8_8_8_UNORM, 4, WXYZ, true, false, kElemFloat, "BGRA8Unorm"},
    {PixelFormat::kBGRA8UnormSrgb, FMT6_8_8_8_8_UNORM, 4, WXYZ, true, true, kElemFloat, "BGRA8Unorm_sRGB"},
    {PixelFormat::kRGBA16Float, FMT6_16_16_16_16_FLOAT, 8, WZYX, true, false, kElemFloat, "RGBA16Float"},
    {PixelFormat::kRGBA32Uint, FMT6_32_32_32_32_UINT, 16, WZYX, true, false, kElemUint, "RGBA32Uint"},
    {PixelFormat::kRGBA32Float, FMT6_32_32_32_32_FLOAT, 16, WZYX, true, false, kElemFloat, "RGBA32Float"},
    // Z32_FLOAT is FMT6_32_FLOAT to the TP and the 2D engine (fd6_format_table.c:330 VT(Z32_FLOAT, 32_FLOAT, WZYX)) and
    // DEPTH6_32 to the RB (fd6_pipe2depth)
    {PixelFormat::kDepth32Float, FMT6_32_FLOAT, 4, WZYX, false, false, kElemFloat, "Depth32Float", true},
    {PixelFormat::kDepth32FloatStencil8, FMT6_32_FLOAT, 4, WZYX, false, false, kElemFloat, "Depth32Float_Stencil8", true, true},
    // NV12: R8_G8B8_420_UNORM, WZYX (fd6_format_table.c:357): Y in x, Cb in y, Cr in z, and no alpha, so w reads one
    {PixelFormat::kYCbCr8_420_2P, FMT6_R8_G8B8_2PLANE_420_UNORM, 1, WZYX, false, false, kElemFloat, "YCbCr8_420_2P", false, false, true},
    // ASTC: 16 bytes a block, sampled alone, LDR and sRGB the same storage (fd6_format_table.c:324-337 _T_)
    {PixelFormat::kASTC_4x4_LDR, FMT6_ASTC_4x4, 16, WZYX, true, false, kElemFloat, "ASTC_4x4_LDR", false, false, false, 4, 4},
    {PixelFormat::kASTC_4x4_sRGB, FMT6_ASTC_4x4, 16, WZYX, true, true, kElemFloat, "ASTC_4x4_sRGB", false, false, false, 4, 4},
    {PixelFormat::kASTC_5x4_LDR, FMT6_ASTC_5x4, 16, WZYX, true, false, kElemFloat, "ASTC_5x4_LDR", false, false, false, 5, 4},
    {PixelFormat::kASTC_5x4_sRGB, FMT6_ASTC_5x4, 16, WZYX, true, true, kElemFloat, "ASTC_5x4_sRGB", false, false, false, 5, 4},
    {PixelFormat::kASTC_5x5_LDR, FMT6_ASTC_5x5, 16, WZYX, true, false, kElemFloat, "ASTC_5x5_LDR", false, false, false, 5, 5},
    {PixelFormat::kASTC_5x5_sRGB, FMT6_ASTC_5x5, 16, WZYX, true, true, kElemFloat, "ASTC_5x5_sRGB", false, false, false, 5, 5},
    {PixelFormat::kASTC_6x5_LDR, FMT6_ASTC_6x5, 16, WZYX, true, false, kElemFloat, "ASTC_6x5_LDR", false, false, false, 6, 5},
    {PixelFormat::kASTC_6x5_sRGB, FMT6_ASTC_6x5, 16, WZYX, true, true, kElemFloat, "ASTC_6x5_sRGB", false, false, false, 6, 5},
    {PixelFormat::kASTC_6x6_LDR, FMT6_ASTC_6x6, 16, WZYX, true, false, kElemFloat, "ASTC_6x6_LDR", false, false, false, 6, 6},
    {PixelFormat::kASTC_6x6_sRGB, FMT6_ASTC_6x6, 16, WZYX, true, true, kElemFloat, "ASTC_6x6_sRGB", false, false, false, 6, 6},
    {PixelFormat::kASTC_8x5_LDR, FMT6_ASTC_8x5, 16, WZYX, true, false, kElemFloat, "ASTC_8x5_LDR", false, false, false, 8, 5},
    {PixelFormat::kASTC_8x5_sRGB, FMT6_ASTC_8x5, 16, WZYX, true, true, kElemFloat, "ASTC_8x5_sRGB", false, false, false, 8, 5},
    {PixelFormat::kASTC_8x6_LDR, FMT6_ASTC_8x6, 16, WZYX, true, false, kElemFloat, "ASTC_8x6_LDR", false, false, false, 8, 6},
    {PixelFormat::kASTC_8x6_sRGB, FMT6_ASTC_8x6, 16, WZYX, true, true, kElemFloat, "ASTC_8x6_sRGB", false, false, false, 8, 6},
    {PixelFormat::kASTC_8x8_LDR, FMT6_ASTC_8x8, 16, WZYX, true, false, kElemFloat, "ASTC_8x8_LDR", false, false, false, 8, 8},
    {PixelFormat::kASTC_8x8_sRGB, FMT6_ASTC_8x8, 16, WZYX, true, true, kElemFloat, "ASTC_8x8_sRGB", false, false, false, 8, 8},
    {PixelFormat::kASTC_10x5_LDR, FMT6_ASTC_10x5, 16, WZYX, true, false, kElemFloat, "ASTC_10x5_LDR", false, false, false, 10, 5},
    {PixelFormat::kASTC_10x5_sRGB, FMT6_ASTC_10x5, 16, WZYX, true, true, kElemFloat, "ASTC_10x5_sRGB", false, false, false, 10, 5},
    {PixelFormat::kASTC_10x6_LDR, FMT6_ASTC_10x6, 16, WZYX, true, false, kElemFloat, "ASTC_10x6_LDR", false, false, false, 10, 6},
    {PixelFormat::kASTC_10x6_sRGB, FMT6_ASTC_10x6, 16, WZYX, true, true, kElemFloat, "ASTC_10x6_sRGB", false, false, false, 10, 6},
    {PixelFormat::kASTC_10x8_LDR, FMT6_ASTC_10x8, 16, WZYX, true, false, kElemFloat, "ASTC_10x8_LDR", false, false, false, 10, 8},
    {PixelFormat::kASTC_10x8_sRGB, FMT6_ASTC_10x8, 16, WZYX, true, true, kElemFloat, "ASTC_10x8_sRGB", false, false, false, 10, 8},
    {PixelFormat::kASTC_10x10_LDR, FMT6_ASTC_10x10, 16, WZYX, true, false, kElemFloat, "ASTC_10x10_LDR", false, false, false, 10, 10},
    {PixelFormat::kASTC_10x10_sRGB, FMT6_ASTC_10x10, 16, WZYX, true, true, kElemFloat, "ASTC_10x10_sRGB", false, false, false, 10, 10},
    {PixelFormat::kASTC_12x10_LDR, FMT6_ASTC_12x10, 16, WZYX, true, false, kElemFloat, "ASTC_12x10_LDR", false, false, false, 12, 10},
    {PixelFormat::kASTC_12x10_sRGB, FMT6_ASTC_12x10, 16, WZYX, true, true, kElemFloat, "ASTC_12x10_sRGB", false, false, false, 12, 10},
    {PixelFormat::kASTC_12x12_LDR, FMT6_ASTC_12x12, 16, WZYX, true, false, kElemFloat, "ASTC_12x12_LDR", false, false, false, 12, 12},
    {PixelFormat::kASTC_12x12_sRGB, FMT6_ASTC_12x12, 16, WZYX, true, true, kElemFloat, "ASTC_12x12_sRGB", false, false, false, 12, 12},
};

const char* typeName(TextureType t) {
    switch (t) {
    case TextureType::k1D: return "1D";
    case TextureType::k1DArray: return "1DArray";
    case TextureType::k2D: return "2D";
    case TextureType::k2DArray: return "2DArray";
    case TextureType::k2DMultisample: return "2DMultisample";
    case TextureType::kCube: return "Cube";
    case TextureType::kCubeArray: return "CubeArray";
    case TextureType::k3D: return "3D";
    }
    return "?";
}

uint32_t log2u(uint32_t v) {
    uint32_t r = 0;
    while ((1u << (r + 1)) <= v) r++;
    return r;
}

// a level of detail as the 12-bit unsigned 4.8 fixed point the TEX_SAMP fields hold: Mesa's encoder is uint32_t(val * 256.0),
// which for Metal's default lodMaxClamp of FLT_MAX is undefined in C, so the value is clamped to the field first, as
// turnip clamps it (tu_sampler.cc:66-67: MIN2(..., 4095/256))
uint32_t lodFixed(float lod) {
    double scaled = double(lod) * double(1u << kLodFractionBits);
    if (!(scaled > 0.0)) return 0;   // negative, zero, or NaN
    if (scaled >= double(kLodMaxFixed)) return kLodMaxFixed;
    return uint32_t(scaled);
}

}  // namespace

const FormatInfo* formatInfo(PixelFormat format) {
    for (const FormatInfo& f : kFormats) {
        if (f.format == format) return &f;
    }
    return nullptr;
}

uint64_t texturePitch(const FormatInfo& format, uint32_t width) {
    // fd6_layout.c:181-201 for a layout the driver chooses: pitchalign = cpp_shift, then pitch0 = align(width * cpp,
    // 1 << (pitchalign + 6)) = align(width * cpp, 64 * cpp); so a row is a multiple of 64 pixels
    // (a block format counts blocks across, as fdl_set_pitchalign does: nblocksx * cpp, cpp the block's bytes)
    uint64_t unit = 64ull * format.bytesPerPixel;
    return (uint64_t(blocksAcross(format, width)) * format.bytesPerPixel + unit - 1) / unit * unit;
}

TextureLevel textureLevel(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t levels, uint32_t level) {
    const uint64_t align = 64ull * format.bytesPerPixel;   // 1 << pitchalign, pitchalign = cpp_shift + 6
    const uint64_t pitch0 = texturePitch(format, width);
    uint64_t offset = 0;
    TextureLevel out{};
    for (uint32_t l = 0; l <= level; l++) {
        const uint64_t pitch = (std::max<uint64_t>(1, pitch0 >> l) + align - 1) / align * align;
        const uint32_t h = std::max<uint32_t>(1, height >> l);
        const uint64_t down = blocksDown(format, h);   // rows of blocks for a block format (fd6_layout.c's nblocksy)
        const uint64_t rows = l == levels - 1 ? (down + 3) / 4 * 4 : down;
        out = TextureLevel{offset, pitch, std::max<uint32_t>(1, width >> l), h};
        offset += rows * pitch;
    }
    return out;
}

namespace {
uint64_t depthBytes(const FormatInfo& format, uint32_t width, uint32_t height) {
    uint64_t rows = (uint64_t(height) + 15) / 16 * 16;
    return (rows * texturePitch(format, width) + 4095) / 4096 * 4096;
}
uint64_t s8Pitch(uint32_t width) { return (uint64_t(width) + 127) / 128 * 128; }
}  // namespace

// A depth texture's mip level: each level its own tiled layout -- the pitch for its width, its rows to 16, its start
// 4096-aligned -- one after another. The RB draws into a level at that base and pitch and the Clear fills it there; the
// TP is not given these offsets (it would work fdl6's out), so a sampled view of such a texture is of level 0 alone.
TextureLevel depthLevel(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t level) {
    uint64_t offset = 0;
    for (uint32_t l = 0; l < level; l++) {
        offset += depthBytes(format, std::max<uint32_t>(1, width >> l), std::max<uint32_t>(1, height >> l));
    }
    const uint32_t w = std::max<uint32_t>(1, width >> level), h = std::max<uint32_t>(1, height >> level);
    return TextureLevel{offset, texturePitch(format, w), w, h};
}

namespace {
uint64_t s8Bytes(uint32_t width, uint32_t height) { return ((uint64_t(height) + 31) / 32 * 32 * s8Pitch(width) + 4095) / 4096 * 4096; }
}  // namespace

uint64_t textureBytes(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t levels) {
    if (format.depth && levels > 1) {
        const TextureLevel last = depthLevel(format, width, height, levels - 1);
        return last.offset + depthBytes(format, last.width, last.height);
    }
    if (format.stencil) {
        return depthBytes(format, width, height) + s8Bytes(width, height);
    }
    if (levels > 1 && !format.depth) {
        const TextureLevel last = textureLevel(format, width, height, levels, levels - 1);
        return (last.offset + (uint64_t(blocksDown(format, last.height)) + 3) / 4 * 4 * last.pitch + 4095) / 4096 * 4096;
    }
    if (format.depth) {
        // "Linear D/S is not supported by HW" (fd6_layout.c:157-164): a depth format is laid out TILE6_3 on every level, its
        // rows counted to the tiled height alignment of 16 for cpp 4 (fdl6_tile_alignment, :81-95; the pitch is
        // texturePitch's, the same 64 * cpp), the layer 4096-aligned, and so is the base (base_align, :102-104: the
        // buffer is whole pages)
        uint64_t rows = (uint64_t(height) + 15) / 16 * 16;
        return (rows * texturePitch(format, width) + 4095) / 4096 * 4096;
    }
    return layerBytes2d(blocksDown(format, height), texturePitch(format, width));
}

bool Texture::plan(const TextureDesc& desc, const FormatInfo** infoOut, uint64_t* pitchOut, uint64_t* bytesOut, std::string* why) {
    // 1D is a 2D layout one row high under the TP's 1D type (fd6_view.c:247, fdl6_tex_type): CoreAnimation keeps its
    // gamma tables in them. 3D is that 2D layout once a depth slice, each slice 4096-aligned (fd6_layout.c's size0 for a
    // 3D level 0): CoreAnimation's colour lookup tables, read by fixed_frag's 3D sample.
    // A 2D array is the same slices as layers (fd6_layout.c: a level's layers each layer_size apart, one level here)
    // under the TP's 2D type with DEPTH the layer count (fdl6_view_init's array view); DEPTH carries its layers.
    // A cube is a 2D array of its 6 faces in Metal's order (+X, -X, +Y, -Y, +Z, -Z), which is the TP's layer order too,
    // under the cube type with DEPTH the layers over 6 (fd6_view.c:178-182)
    // A multisample texture is a render target whose pixels each hold SAMPLES samples side by side: fdl6_layout
    // multiplies cpp by the count (fd6_layout.c:134), so in the linear layout it is the memory of a single-sample
    // texture SAMPLES times as wide -- the RB writes it under DEST_MSAA_CNTL's count, the 2D engine resolves it reading
    // SAMPLES of them a pixel, and a Clear fills it as the wider texture it is in memory.
    const bool slices = desc.type == TextureType::k3D || desc.type == TextureType::k2DArray || desc.type == TextureType::kCube;
    const bool multisample = desc.type == TextureType::k2DMultisample;
    if (desc.type != TextureType::k2D && desc.type != TextureType::k1D && !slices && !multisample) {
        refuse(why, std::string("texture type ") + n(uint32_t(desc.type)) + " (" + typeName(desc.type) +
                        "): 1D (0), 2D (2), 2D array (3), 2D multisample (4), cube (5) and 3D (7) are served");
        return false;
    }
    if (multisample != (desc.samples != 1) || (multisample && desc.samples != 2 && desc.samples != 4)) {
        refuse(why, "a texture of type " + n(uint32_t(desc.type)) + " with " + n(desc.samples) + " samples: a 2D multisample texture (4) of 2 or 4, every other type of 1");
        return false;
    }
    if (desc.type == TextureType::kCube && (desc.depth != 6 || desc.width != desc.height)) {
        refuse(why, "a cube of " + n(desc.width) + " x " + n(desc.height) + " with " + n(desc.depth) + " faces: square faces, 6 of them");
        return false;
    }
    if (slices && (desc.depth == 0 || desc.depth > 2048 || desc.mipLevels != 1)) {
        refuse(why, std::string(desc.type == TextureType::k3D ? "a 3D texture " : "a 2D array of ") + n(desc.depth) +
                        (desc.type == TextureType::k3D ? " deep" : " layers") + " of " + n(desc.mipLevels) + " levels: 1 to 2048 slices of one level are served");
        return false;
    }
    if (!slices && desc.depth != 1) {
        refuse(why, "a depth of " + n(desc.depth) + " for a texture of type " + n(uint32_t(desc.type)) + ": only a 3D texture or a 2D array has slices");
        return false;
    }
    if (desc.type == TextureType::k1D && desc.height != 1) {
        refuse(why, "a 1D texture " + n(desc.height) + " high: a 1D texture is one row");
        return false;
    }
    const FormatInfo* info = formatInfo(desc.format);
    if (info == nullptr) {
        refuse(why, "pixel format " + n(uint32_t(desc.format)) + ": not one of the formats served (texture.hpp: PixelFormat)");
        return false;
    }
    if (info->twoPlane) {
        refuse(why, std::string("a ") + info->name + " texture of its own memory: the format is served over a two-plane IOSurface alone");
        return false;
    }
    // TEX_CONST_1's WIDTH and HEIGHT are 15 bits each (a6xx.hpp: A6XX_TEX_CONST_1_WIDTH__MASK)
    if (desc.width == 0 || desc.width > 32767 || desc.height == 0 || desc.height > 32767) {
        refuse(why, "a texture of " + n(desc.width) + " x " + n(desc.height) + ": each side is 1 to 32767 (the descriptor's fields are 15 bits)");
        return false;
    }
    // MTLTextureUsageRenderTarget (4) is taken alone or beside the shader bits: a draw's colour target has the same linear
    // layout as a sampled or stored texture (render.cpp reads RB_MRT_BUF_INFO/PITCH/ARRAY_PITCH/BASE off the same fields,
    // as fdl6_view_init does for both views, fd6_view.c:232-250 and 408-413); what differs is only who writes it
    if ((desc.usage & ~(kTextureUsageRead | kTextureUsageWrite | kTextureUsageRenderTarget)) != 0) {
        refuse(why, "texture usage " + n(desc.usage) + " has bits other than shader read (1), shader write (2) and render target (4): not served");
        return false;
    }
    uint32_t most = 1;
    while ((std::max(desc.width, desc.height) >> most) != 0) most++;   // floor(log2(max(w, h))) + 1
    if (desc.mipLevels == 0 || desc.mipLevels > most || desc.mipLevels > 16) {
        refuse(why, "a " + n(desc.width) + " x " + n(desc.height) + " texture of " + n(desc.mipLevels) + " mip levels: 1 to " + n(std::min<uint32_t>(most, 16)) +
                        " (down to 1 x 1, and MIPLVLS holds 16)");
        return false;
    }
    if (info->blockW > 1 && desc.usage != kTextureUsageRead) {
        refuse(why, std::string("a ") + info->name + " texture with usage " + n(desc.usage) + ": a compressed format is sampled alone (1), not stored or drawn into");
        return false;
    }
    if (info->depth && desc.mipLevels != 1 && (info->stencil || desc.samples != 1)) {
        refuse(why, std::string("a ") + info->name + " texture of " + n(desc.mipLevels) + " mip levels: mip levels of a depth texture with a stencil plane or "
                        "samples are not served");
        return false;
    }
    if (info->depth && (desc.usage & ~kTextureUsageRead) != kTextureUsageRenderTarget) {
        // a depth texture is an attachment the RB writes, and may be sampled (a shadow map): the TP reads the tiled layout
        // as fdl6_view_init describes it (sampledDescriptor); a store into it would need the tiled IBO, which is not here
        refuse(why, std::string("a ") + info->name + " texture with usage " + n(desc.usage) +
                        ": a depth texture is tiled (TILE6_3) and is served as a render target (4), sampled too (5), not stored");
        return false;
    }
    // (shader read is taken beside render target -- sokol asks for both on every attachment; a shader sampling one as a
    // 2D texture is refused at its draw by the type, a texture2d_ms read is served as such)
    if (multisample && (info->stencil || info->blockW > 1 || desc.mipLevels != 1 || (desc.usage & ~kTextureUsageRead) != kTextureUsageRenderTarget)) {
        refuse(why, std::string("a multisample ") + info->name + " texture of " + n(desc.mipLevels) + " levels with usage " + n(desc.usage) +
                        ": one level of an uncompressed colour format or Depth32Float, a render target (4) alone, is served (a stencil plane is "
                        "laid out apart and not for samples here)");
        return false;
    }
    // (a slice texture is sampled or stored by a kernel, and may be drawn into a slice at a time: the slices are 2D layouts
    // like any target, and a kernel's store reaches slice z through the storage view's ARRAY_PITCH. A cube is not stored:
    // a kernel addressing one is refused at its dispatch, so its usage stays what is served)
    const uint32_t sliceUsage = desc.type == TextureType::kCube ? kTextureUsageRead | kTextureUsageRenderTarget
                                                               : kTextureUsageRead | kTextureUsageWrite | kTextureUsageRenderTarget;
    if (slices && (info->depth || info->twoPlane || info->blockW > 1 || (desc.usage & (kTextureUsageRead | kTextureUsageWrite)) == 0 ||
                   (desc.usage & ~sliceUsage) != 0)) {
        refuse(why, std::string("a ") + (desc.type == TextureType::k3D ? "3D " : desc.type == TextureType::kCube ? "cube " : "2D array ") + info->name + " texture with usage " + n(desc.usage) +
                        ": one of an uncompressed colour format, sampled (1)" + (desc.type == TextureType::kCube ? "" : " or stored by a kernel (2)") +
                        ", and drawn into a slice at a time too (4), is served");
        return false;
    }
    const uint32_t memoryWidth = desc.width * desc.samples;   // the width of its memory: a multisample texture's samples side by side
    uint64_t pitch = texturePitch(*info, memoryWidth);
    uint64_t bytes = slices ? (uint64_t(blocksDown(*info, desc.height)) * pitch + 4095) / 4096 * 4096 * desc.depth
                                                    : textureBytes(*info, memoryWidth, desc.height, desc.mipLevels);
    if (info->depth && desc.samples > 1) {
        // a multisample depth layout is fdl6's tiled one at cpp 4 * samples: the pitch aligned to 64 such texels
        // (fdl6_tile_alignment's pitchalign = log2(cpp)), its rows counted to 16, the layer 4096-aligned
        const uint64_t cpp = uint64_t(info->bytesPerPixel) * desc.samples;
        pitch = (uint64_t(desc.width) * cpp + 64 * cpp - 1) / (64 * cpp) * (64 * cpp);
        bytes = ((uint64_t(desc.height) + 15) / 16 * 16 * pitch + 4095) / 4096 * 4096;
    }
    *infoOut = info;
    *pitchOut = pitch;
    *bytesOut = bytes;
    return true;
}

std::shared_ptr<Texture> Texture::make(Gpu& gpu, const TextureDesc& desc, std::string* why) {
    const FormatInfo* info = nullptr;
    uint64_t pitch = 0, bytes = 0;
    if (!plan(desc, &info, &pitch, &bytes, why)) {
        return nullptr;
    }
    std::shared_ptr<Buffer> buffer = newBuffer(gpu, bytes, why);
    if (buffer == nullptr) {
        return nullptr;
    }
    return std::shared_ptr<Texture>(new Texture(std::move(buffer), desc, info, pitch));
}

namespace {
// the region check both directions share: W x H pixels at (X, Y) inside the texture, and the caller's rows at least a
// row of W pixels apart (Metal's -replaceRegion: asks the same of bytesPerRow)
bool regionFits(const Texture& t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t bytesPerRow, std::string* why) {
    if (level >= t.levels()) {
        return refuse(why, "mip level " + n(level) + " of a texture of " + n(t.levels()));
    }
    const TextureLevel L = t.level(level);
    if (uint64_t(x) + w > L.width || uint64_t(y) + h > L.height) {
        return refuse(why, "region " + n(w) + " x " + n(h) + " at (" + n(x) + ", " + n(y) + ") lies outside the " + n(L.width) + " x " +
                               n(L.height) + (level != 0 ? " level " + n(level) + " of the" : std::string()) + " texture");
    }
    const FormatInfo& f = t.info();
    if (f.blockW > 1 && (x % f.blockW != 0 || y % f.blockH != 0 || (w % f.blockW != 0 && x + w != L.width) || (h % f.blockH != 0 && y + h != L.height))) {
        return refuse(why, "region " + n(w) + " x " + n(h) + " at (" + n(x) + ", " + n(y) + ") of a " + f.name + " texture: whole " + n(f.blockW) + " x " +
                               n(f.blockH) + " blocks (or up to the edge)");
    }
    uint64_t row = uint64_t(blocksAcross(f, w)) * f.bytesPerPixel;
    if (blocksDown(f, h) > 1 && bytesPerRow < row) {
        return refuse(why, "bytesPerRow " + n(bytesPerRow) + " is shorter than a row of " + n(w) + " pixels (" + n(row) + " bytes)");
    }
    return true;
}
}  // namespace

uint64_t Texture::depthPlaneBytes() const { return info_->depth ? depthBytes(*info_, desc_.width, desc_.height) : buffer_->length(); }
uint64_t Texture::stencilOffset() const { return info_->stencil ? depthPlaneBytes() : 0; }
uint64_t Texture::stencilPitch() const { return info_->stencil ? s8Pitch(desc_.width) : 0; }
uint64_t Texture::stencilPlaneBytes() const { return info_->stencil ? s8Bytes(desc_.width, desc_.height) : 0; }

TextureLevel Texture::level(uint32_t l) const {
    if (surface_ != 0 || desc_.mipLevels <= 1) return TextureLevel{0, pitch_, desc_.width, desc_.height};
    if (info_->depth) return depthLevel(*info_, desc_.width, desc_.height, l);
    return textureLevel(*info_, desc_.width, desc_.height, desc_.mipLevels, l);
}

bool Texture::replace(uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* bytes, uint64_t bytesPerRow, std::string* why) {
    if (desc_.samples != 1) {
        return refuse(why, "a multisample texture's samples are the GPU's alone: resolve it into a texture of one sample first (Metal refuses this too)");
    }
    if (info_->depth) {
        return refuse(why, std::string("a ") + info_->name + " texture is tiled (TILE6_3): its bytes are not rows the CPU can address");
    }
    if (info_->twoPlane) {
        return refuse(why, std::string("a ") + info_->name + " texture is two planes of different sizes: write the surface's planes through the surface");
    }
    if (contents() == nullptr) {
        return refuse(why, "IOSurface " + n(surface_) + "'s memory is mapped nowhere in this process by the driver: write it through the surface");
    }
    if (!regionFits(*this, level, x, y, w, h, bytesPerRow, why)) {
        return false;
    }
    const TextureLevel L = this->level(level);
    // (in blocks for a block format: a row is a row of blocks, the caller's bytesPerRow too, as Metal has it)
    uint64_t row = uint64_t(blocksAcross(*info_, w)) * info_->bytesPerPixel;
    auto* dst = static_cast<uint8_t*>(contents()) + L.offset + uint64_t(y / info_->blockH) * L.pitch + uint64_t(x / info_->blockW) * info_->bytesPerPixel;
    auto* src = static_cast<const uint8_t*>(bytes);
    // one row at a time, in order: the mapping is write-combined, and a row is the longest run that is contiguous in both
    for (uint32_t r = 0; r < blocksDown(*info_, h); r++) {
        memcpy(dst + uint64_t(r) * L.pitch, src + uint64_t(r) * bytesPerRow, row);
    }
    return true;
}

bool Texture::read(uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void* bytes, uint64_t bytesPerRow, std::string* why) const {
    if (desc_.samples != 1) {
        return refuse(why, "a multisample texture's samples are the GPU's alone: resolve it into a texture of one sample first (Metal refuses this too)");
    }
    if (info_->depth) {
        return refuse(why, std::string("a ") + info_->name + " texture is tiled (TILE6_3): its bytes are not rows the CPU can address");
    }
    if (info_->twoPlane) {
        return refuse(why, std::string("a ") + info_->name + " texture is two planes of different sizes: read the surface's planes through the surface");
    }
    if (contents() == nullptr) {
        return refuse(why, "IOSurface " + n(surface_) + "'s memory is mapped nowhere in this process by the driver: read it through the surface");
    }
    if (!regionFits(*this, level, x, y, w, h, bytesPerRow, why)) {
        return false;
    }
    const TextureLevel L = this->level(level);
    uint64_t row = uint64_t(blocksAcross(*info_, w)) * info_->bytesPerPixel;
    auto* src = static_cast<const uint8_t*>(contents()) + L.offset + uint64_t(y / info_->blockH) * L.pitch + uint64_t(x / info_->blockW) * info_->bytesPerPixel;
    auto* dst = static_cast<uint8_t*>(bytes);
    for (uint32_t r = 0; r < blocksDown(*info_, h); r++) {
        memcpy(dst + uint64_t(r) * bytesPerRow, src + uint64_t(r) * L.pitch, row);
    }
    return true;
}

uint64_t Texture::sliceBytes() const {
    return desc_.type == TextureType::k3D || desc_.type == TextureType::k2DArray || desc_.type == TextureType::kCube ? (uint64_t(blocksDown(*info_, desc_.height)) * pitch_ + 4095) / 4096 * 4096 : 0;
}

namespace {
bool slicesFit(const Texture& t, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint64_t bytesPerRow, uint64_t bytesPerImage,
               std::string* why) {
    if (t.type() != TextureType::k3D && t.type() != TextureType::k2DArray && t.type() != TextureType::kCube) {
        return refuse(why, "slices of a texture that is not 3D, a 2D array or a cube");
    }
    if (t.contents() == nullptr) return refuse(why, "the texture's memory is mapped nowhere in this process");
    if (uint64_t(z) + d > t.depth() || d == 0) {
        return refuse(why, n(d) + " slices from " + n(z) + " of a " + (t.type() == TextureType::k3D ? "3D texture " : "2D array ") + n(t.depth()) +
                               (t.type() == TextureType::k3D ? " deep" : " layers"));
    }
    if (d > 1 && bytesPerImage < bytesPerRow * h) {
        return refuse(why, "bytesPerImage " + n(bytesPerImage) + " is shorter than an image of " + n(h) + " rows of " + n(bytesPerRow) + " bytes");
    }
    (void)x, (void)y, (void)w;
    return true;
}
}  // namespace

bool Texture::replaceSlices(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, const void* bytes, uint64_t bytesPerRow,
                            uint64_t bytesPerImage, std::string* why) {
    if (!slicesFit(*this, x, y, z, w, h, d, bytesPerRow, bytesPerImage, why) || !regionFits(*this, 0, x, y, w, h, bytesPerRow, why)) return false;
    const uint64_t row = uint64_t(w) * info_->bytesPerPixel;
    for (uint32_t s = 0; s < d; s++) {
        auto* dst = static_cast<uint8_t*>(contents()) + (uint64_t(z) + s) * sliceBytes() + uint64_t(y) * pitch_ + uint64_t(x) * info_->bytesPerPixel;
        auto* src = static_cast<const uint8_t*>(bytes) + uint64_t(s) * bytesPerImage;
        for (uint32_t r = 0; r < h; r++) memcpy(dst + uint64_t(r) * pitch_, src + uint64_t(r) * bytesPerRow, row);
    }
    return true;
}

bool Texture::readSlices(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, void* bytes, uint64_t bytesPerRow, uint64_t bytesPerImage,
                         std::string* why) const {
    if (!slicesFit(*this, x, y, z, w, h, d, bytesPerRow, bytesPerImage, why) || !regionFits(*this, 0, x, y, w, h, bytesPerRow, why)) return false;
    const uint64_t row = uint64_t(w) * info_->bytesPerPixel;
    for (uint32_t s = 0; s < d; s++) {
        const auto* src = static_cast<const uint8_t*>(contents()) + (uint64_t(z) + s) * sliceBytes() + uint64_t(y) * pitch_ + uint64_t(x) * info_->bytesPerPixel;
        auto* dst = static_cast<uint8_t*>(bytes) + uint64_t(s) * bytesPerImage;
        for (uint32_t r = 0; r < h; r++) memcpy(dst + uint64_t(r) * bytesPerRow, src + uint64_t(r) * pitch_, row);
    }
    return true;
}

a6xx::TexConst Texture::sampledDescriptor() const {
    // PITCHALIGN = log2(pitch alignment / 64) = log2(cpp) for the pitch texturePitch chose (fd6_layout.c:194-201, fd6_view.c:244);
    // a surface's rows carry the alignment their pitch has, wrapSurface worked it out
    a6xx::TexConst d = texture2d(gpuAddress(), desc_.width, desc_.height, uint32_t(pitch_), pitchAlign_ != ~0u ? pitchAlign_ : log2u(info_->bytesPerPixel),
                                 info_->fmt6, info_->swap, info_->hasAlpha, info_->srgb, desc_.mipLevels, desc_.mipLevels > 1 ? buffer_->length() : 0);
    if (desc_.type == TextureType::k1D) {
        d.dword[2] = (d.dword[2] & ~A6XX_TEX_CONST_2_TYPE__MASK) | A6XX_TEX_CONST_2_TYPE(A6XX_TEX_1D);
    }
    if (desc_.type == TextureType::k3D) {
        // fdl6_view_init's 3D view (fd6_view.c:172-177, 247-249, 308-310): DEPTH the slices, ARRAY_PITCH one slice, and
        // MIN_LAYERSZ the last level's slice -- the one level's here
        d.dword[2] = (d.dword[2] & ~A6XX_TEX_CONST_2_TYPE__MASK) | A6XX_TEX_CONST_2_TYPE(A6XX_TEX_3D);
        d.dword[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t(sliceBytes())) | A6XX_TEX_CONST_3_MIN_LAYERSZ(uint32_t(sliceBytes()));
        d.dword[5] = (d.dword[5] & ~A6XX_TEX_CONST_5_DEPTH__MASK) | A6XX_TEX_CONST_5_DEPTH(desc_.depth);
    }
    if (desc_.type == TextureType::kCube) {
        // a cube view (fd6_view.c:178-182, 247-249): TYPE CUBE, DEPTH the layers over 6, ARRAY_PITCH one face
        d.dword[2] = (d.dword[2] & ~A6XX_TEX_CONST_2_TYPE__MASK) | A6XX_TEX_CONST_2_TYPE(A6XX_TEX_CUBE);
        d.dword[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t(sliceBytes()));
        d.dword[5] = (d.dword[5] & ~A6XX_TEX_CONST_5_DEPTH__MASK) | A6XX_TEX_CONST_5_DEPTH(desc_.depth / 6);
    }
    if (desc_.type == TextureType::k2DArray) {
        // an array view (fd6_view.c:172-177, 247-249): TYPE 2D, DEPTH the layers, ARRAY_PITCH one layer
        d.dword[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t(sliceBytes()));
        d.dword[5] = (d.dword[5] & ~A6XX_TEX_CONST_5_DEPTH__MASK) | A6XX_TEX_CONST_5_DEPTH(desc_.depth);
    }
    if (desc_.samples > 1) {
        // a multisample texture read by sample (texture2d_ms, ir3's isam for txf_ms): the pixels' size, the layout's pitch
        // (the samples side by side) and SAMPLES the log2 of the count (fdl6_view_init: A6XX_TEX_CONST_0_SAMPLES)
        d.dword[0] = (d.dword[0] & ~A6XX_TEX_CONST_0_SAMPLES__MASK) |
                     A6XX_TEX_CONST_0_SAMPLES(desc_.samples == 4 ? MSAA_FOUR : MSAA_TWO);
    }
    if (info_->depth) {
        // the depth plane as the TP reads it (fdl6_view_init over a depth layout): TILE6_3, and TILE_ALL since every level
        // of a depth layout is tiled (fd6_layout.c:157-164 "Linear D/S is not supported by HW"); FMT6_32_FLOAT, the depth
        // in the first component, as fd6_format_texture gives Z32_FLOAT. Its pitch is the layout's (PITCHALIGN log2(cpp)
        // for cpp 4, fdl6_tile_alignment)
        d.dword[0] = (d.dword[0] & ~(A6XX_TEX_CONST_0_TILE_MODE__MASK | A6XX_TEX_CONST_0_MIPLVLS__MASK)) | A6XX_TEX_CONST_0_TILE_MODE(TILE6_3);   // level 0 alone (depthLevel)
        d.dword[3] |= A6XX_TEX_CONST_3_TILE_ALL;
    }
    if (info_->twoPlane) {
        // the second plane as fdl6_view_init gives it (fd6_view.c:255-289): its pitch over MIN_LOD_CLAMP's dword and its
        // address in dwords 7 and 8; MIPLVLS 0, its bits being the chroma siting there (cosited, Metal's default)
        const uint64_t chroma = buffer_->gpuAddress() + chroma_.offset;
        d.dword[6] = A6XX_TEX_CONST_6_PLANE_PITCH(chroma_.rowBytes);
        d.dword[7] = uint32_t(chroma);
        d.dword[8] = uint32_t(chroma >> 32);
    }
    return d;
}

a6xx::TexConst Texture::levelDescriptor(uint32_t level) const {
    const TextureLevel L = this->level(level);
    return texture2d(levelAddress(level), L.width, L.height, uint32_t(L.pitch), pitchAlign_ != ~0u ? pitchAlign_ : log2u(info_->bytesPerPixel), info_->fmt6,
                     info_->swap, info_->hasAlpha, info_->srgb);
}

a6xx::TexConst Texture::storageDescriptor() const {
    a6xx::TexConst d = image2d(gpuAddress(), desc_.width, desc_.height, uint32_t(pitch_), info_->fmt6, info_->swap, info_->hasAlpha, info_->srgb);
    if (desc_.type == TextureType::k2DArray || desc_.type == TextureType::k3D || desc_.type == TextureType::kCube) {
        // fdl6_view_init's storage view (fd6_view.c:172-182, 390-395): ir3 addresses every layered image as 3D (stib.3d,
        // the layer as z), so what tells the TP where layer z lies is ARRAY_PITCH, and DEPTH bounds it. A cube is stored
        // as the 2D array of its six faces (fdl6_tex_type with storage); a 3D view keeps TYPE 3D and MIN_LAYERSZ as the
        // sampled view does
        const bool is3d = desc_.type == TextureType::k3D;
        d.dword[2] = (d.dword[2] & ~A6XX_TEX_CONST_2_TYPE__MASK) | A6XX_TEX_CONST_2_TYPE(is3d ? A6XX_TEX_3D : A6XX_TEX_2D);
        d.dword[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t(sliceBytes())) | (is3d ? A6XX_TEX_CONST_3_MIN_LAYERSZ(uint32_t(sliceBytes())) : 0u);
        d.dword[5] = (d.dword[5] & ~A6XX_TEX_CONST_5_DEPTH__MASK) | A6XX_TEX_CONST_5_DEPTH(desc_.depth);
    }
    return d;
}

bool Texture::isShaped(const KernelTexture& t) const {
    switch (desc_.type) {
    case TextureType::k1D: return t.dim == kTexDim1D && !t.isArray;
    case TextureType::k2D: return t.dim == kTexDim2D && !t.isArray;
    case TextureType::k2DArray: return t.dim == kTexDim2D && t.isArray;
    case TextureType::k3D: return t.dim == kTexDim3D;
    case TextureType::kCube: return t.dim == kTexDimCube && !t.isArray;
    case TextureType::k2DMultisample: return t.dim == kTexDimMS && !t.isArray;
    case TextureType::k1DArray:
    case TextureType::kCubeArray: return false;   // Texture::make makes neither
    }
    return false;
}

std::shared_ptr<Texture> Texture::wrapSurface(Gpu& gpu, uint32_t surfaceId, const TextureDesc& desc, std::string* why, const SurfacePlane* chroma) {
    const FormatInfo* info = formatInfo(desc.format);
    if (info == nullptr || desc.type != TextureType::k2D) {
        refuse(why, "a texture over IOSurface " + n(surfaceId) + " of type " + n(uint32_t(desc.type)) + " and pixel format " + n(uint32_t(desc.format)) +
                        ": a 2D texture of one of the served colour formats is what a surface takes");
        return nullptr;
    }
    if (info->depth) {
        refuse(why, std::string("a ") + info->name + " texture over an IOSurface: a depth texture is tiled, a surface's rows are linear");
        return nullptr;
    }
    if (info->blockW > 1) {
        refuse(why, std::string("a ") + info->name + " texture over an IOSurface: a compressed format is a texture of its own (-replaceRegion: fills it)");
        return nullptr;
    }
    if ((desc.usage & ~(kTextureUsageRead | kTextureUsageWrite | kTextureUsageRenderTarget)) != 0) {
        refuse(why, "texture usage " + n(desc.usage) + " has bits other than shader read (1), shader write (2) and render target (4): not served");
        return nullptr;
    }
    SurfaceInfo s;
    std::shared_ptr<Buffer> buffer = surfaceBuffer(gpu, surfaceId, &s, why);
    if (buffer == nullptr) {
        return nullptr;
    }
    const std::string name = "IOSurface " + n(surfaceId);
    if (info->twoPlane) {
        std::shared_ptr<Texture> t = wrapTwoPlanes(std::move(buffer), s, name, desc, info, chroma, why);
        if (t != nullptr) t->surface_ = surfaceId;
        return t;
    }
    if (s.planes > 1) {
        refuse(why, name + " has " + n(s.planes) + " planes: a single-plane surface is what is served");
        return nullptr;
    }
    if (desc.width == 0 || desc.height == 0 || desc.width > s.width || desc.height > s.height) {
        refuse(why, "a " + n(desc.width) + " x " + n(desc.height) + " texture over the " + n(s.width) + " x " + n(s.height) + " " + name);
        return nullptr;
    }
    // the FourCC of the surface against the format: Metal asks for a compatible one, and a mismatch swaps channels.
    // CoreAnimation's masks are 'LA08' (luminance and alpha, a byte each) read as RG8Unorm and 'L008' read as R8Unorm.
    const uint32_t bgra = 0x42475241u, rgba = 0x52474241u, la08 = 0x4c413038u, l008 = 0x4c303038u;   // 'BGRA', 'RGBA', 'LA08', 'L008'
    const bool bgraFormat = desc.format == PixelFormat::kBGRA8Unorm || desc.format == PixelFormat::kBGRA8UnormSrgb;
    const bool rgbaFormat = desc.format == PixelFormat::kRGBA8Unorm || desc.format == PixelFormat::kRGBA8UnormSrgb;
    if (s.pixelFormat != 0 && !((s.pixelFormat == bgra && bgraFormat) || (s.pixelFormat == rgba && rgbaFormat) ||
                                (s.pixelFormat == la08 && desc.format == PixelFormat::kRG8Unorm) || (s.pixelFormat == l008 && desc.format == PixelFormat::kR8Unorm))) {
        refuse(why, name + "'s pixel format " + n(s.pixelFormat) + " is not " + info->name + "'s: 'BGRA' (0x42475241) takes BGRA8Unorm[_sRGB], 'RGBA' (0x52474241) "
                        "RGBA8Unorm[_sRGB], 'LA08' RG8Unorm and 'L008' R8Unorm");
        return nullptr;
    }
    // the rows: RB_MRT_PITCH and TEX_CONST_2's pitch are in 64-byte units and the RB/TP want the start 64-aligned
    // (fd6_layout.c's linear base_align); rows as long as the width
    if (s.rowBytes % 64 != 0 || s.offset % 64 != 0 || uint64_t(s.rowBytes) < uint64_t(desc.width) * info->bytesPerPixel ||
        uint64_t(s.offset) + uint64_t(s.rowBytes) * desc.height > s.size) {
        refuse(why, name + "'s rows of " + n(s.rowBytes) + " bytes from offset " + n(s.offset) + " in its " + n(s.size) +
                        " bytes: the GPU wants a pitch and a start that are multiples of 64 bytes, rows that hold the width, and the rows inside the surface");
        return nullptr;
    }
    // TEX_CONST_2.PITCHALIGN: the largest k up to log2(cpp) with the pitch a multiple of 64 << k
    uint32_t pitchAlign = 0;
    while (pitchAlign < log2u(info->bytesPerPixel) && s.rowBytes % (64u << (pitchAlign + 1)) == 0) pitchAlign++;
    return std::shared_ptr<Texture>(new Texture(std::move(buffer), desc, info, s.rowBytes, s.offset, pitchAlign, surfaceId));
}

// A '420f' or '420v' surface as one YCbCr8_420_2P texture: plane 0 the luma rows the kernel describes, plane 1 the
// interleaved CbCr rows at half the size the caller read off the surface. Both must be rows the TP takes from their
// own addresses (64-byte pitches and starts, as for any linear texture here) and lie inside the surface's memory.
std::shared_ptr<Texture> Texture::wrapTwoPlanes(std::shared_ptr<Buffer> buffer, const SurfaceInfo& s, const std::string& name, const TextureDesc& desc,
                                                const FormatInfo* info, const SurfacePlane* chroma, std::string* why) {
    const uint32_t f420 = 0x34323066u, v420 = 0x34323076u;   // '420f', '420v'
    if (s.planes != 2 || chroma == nullptr || (s.pixelFormat != f420 && s.pixelFormat != v420)) {
        refuse(why, std::string("a ") + info->name + " texture over " + name + " of " + n(s.planes) + " plane(s) and pixel format " + n(s.pixelFormat) +
                        (chroma == nullptr ? " with no second plane given" : "") + ": a two-plane '420f' or '420v' surface and its second plane are what it takes");
        return nullptr;
    }
    if (desc.usage != kTextureUsageRead || desc.mipLevels != 1) {
        refuse(why, std::string("a ") + info->name + " texture of usage " + n(desc.usage) + " and " + n(desc.mipLevels) +
                        " level(s): one level, read by shaders, is what it serves (the TP reads NV12; nothing here writes it)");
        return nullptr;
    }
    if (desc.width == 0 || desc.height == 0 || desc.width > s.width || desc.height > s.height) {
        refuse(why, "a " + n(desc.width) + " x " + n(desc.height) + " texture over the " + n(s.width) + " x " + n(s.height) + " " + name);
        return nullptr;
    }
    const uint64_t cw = (uint64_t(desc.width) + 1) / 2, ch = (uint64_t(desc.height) + 1) / 2;
    if (s.rowBytes % 64 != 0 || s.offset % 64 != 0 || s.rowBytes < desc.width || uint64_t(s.offset) + uint64_t(s.rowBytes) * desc.height > s.size ||
        chroma->rowBytes % 64 != 0 || chroma->offset % 64 != 0 || chroma->rowBytes < 2 * cw || chroma->offset + uint64_t(chroma->rowBytes) * ch > s.size) {
        refuse(why, name + "'s planes, luma rows of " + n(s.rowBytes) + " bytes from " + n(s.offset) + ", chroma rows of " + n(chroma->rowBytes) + " from " +
                        n(chroma->offset) + ", in its " + n(s.size) + " bytes: each plane wants a pitch and a start that are multiples of 64 bytes, rows that "
                        "hold its width, and its rows inside the surface");
        return nullptr;
    }
    std::shared_ptr<Texture> t(new Texture(std::move(buffer), desc, info, s.rowBytes, s.offset, 0));
    t->chroma_ = *chroma;
    return t;
}

std::shared_ptr<Texture> Texture::overBuffer(std::shared_ptr<Buffer> buffer, uint64_t offset, uint64_t bytesPerRow, const TextureDesc& desc, std::string* why) {
    const FormatInfo* info = formatInfo(desc.format);
    if (info == nullptr || info->depth || info->twoPlane || info->blockW > 1) {
        refuse(why, "a texture over a buffer of pixel format " + n(uint32_t(desc.format)) + ": one of the colour formats served (texture.hpp: PixelFormat) is what a buffer takes");
        return nullptr;
    }
    if ((desc.type != TextureType::k2D && desc.type != TextureType::k1D) || desc.mipLevels != 1 || (desc.type == TextureType::k1D && desc.height != 1)) {
        refuse(why, "a texture over a buffer of type " + n(uint32_t(desc.type)) + " and " + n(desc.mipLevels) + " level(s): one level, 1D or 2D, is what a buffer takes");
        return nullptr;
    }
    if (desc.width == 0 || desc.width > 32767 || desc.height == 0 || desc.height > 32767 ||
        (desc.usage & ~(kTextureUsageRead | kTextureUsageWrite | kTextureUsageRenderTarget)) != 0) {
        refuse(why, "a " + n(desc.width) + " x " + n(desc.height) + " texture of usage " + n(desc.usage) + " over a buffer: each side 1 to 32767, usage read, write and render target");
        return nullptr;
    }
    if (bytesPerRow % 64 != 0 || offset % 64 != 0 || bytesPerRow < uint64_t(desc.width) * info->bytesPerPixel ||
        offset + bytesPerRow * (desc.height - 1) + uint64_t(desc.width) * info->bytesPerPixel > buffer->length()) {
        refuse(why, "rows of " + n(bytesPerRow) + " bytes from offset " + n(offset) + " of a " + n(buffer->length()) + "-byte buffer for a " + n(desc.width) + " x " +
                        n(desc.height) + " " + info->name + " texture: the GPU wants a pitch and a start that are multiples of 64 bytes, rows that hold the width, "
                        "and the rows inside the buffer");
        return nullptr;
    }
    uint32_t pitchAlign = 0;
    while (pitchAlign < log2u(info->bytesPerPixel) && bytesPerRow % (64u << (pitchAlign + 1)) == 0) pitchAlign++;
    return std::shared_ptr<Texture>(new Texture(std::move(buffer), desc, info, bytesPerRow, offset, pitchAlign));
}

std::shared_ptr<Texture> Texture::view(const std::shared_ptr<Texture>& parent, PixelFormat format, TextureType type, uint32_t baseLevel, uint32_t levels,
                                       uint32_t baseSlice, uint32_t slices, std::string* why) {
    const Texture& p = *parent;
    const FormatInfo* info = formatInfo(format);
    const bool layered = p.type() == TextureType::k2DArray || p.type() == TextureType::kCube;
    const uint32_t parentSlices = layered ? p.depth() : 1;
    const std::string asked = "a view in pixel format " + n(uint32_t(format)) + ", type " + n(uint32_t(type)) + ", levels " + n(baseLevel) + "+" + n(levels) +
                              ", slices " + n(baseSlice) + "+" + n(slices) + " of a " + p.info().name + " texture of type " + n(uint32_t(p.type())) + " with " +
                              n(p.levels()) + " level(s) and " + n(parentSlices) + " slice(s)";
    if (levels == 0 || slices == 0 || baseLevel + uint64_t(levels) > p.levels() || baseSlice + uint64_t(slices) > parentSlices) {
        refuse(why, asked + ": the range lies outside the texture");
        return nullptr;
    }
    // another format reads the same bytes otherwise: the same size a pixel and a plain colour layout on both sides, as
    // Metal's own rule for views has it (a depth or two-plane layout is not rows of pixels a second format could name)
    if (info == nullptr || (format != p.format() && (info->depth || info->twoPlane || info->blockW > 1 || p.info().depth || p.info().twoPlane ||
                                                      p.info().blockW > 1 || info->bytesPerPixel != p.info().bytesPerPixel))) {
        refuse(why, asked + ": a view reads the texture's own format, or another uncompressed colour format of as many bytes a pixel");
        return nullptr;
    }
    if (p.surface() != 0 || p.info().twoPlane || p.samples() != 1) {
        if (format == p.format() && type == p.type() && levels == p.levels() && slices == parentSlices) {
            std::shared_ptr<Texture> t(new Texture(p.buffer_, p.desc_, p.info_, p.pitch_, p.offset_, p.pitchAlign_, p.surface_));
            t->chroma_ = p.chroma_;
            return t;
        }
        refuse(why, asked + ": a view of an IOSurface's, a two-plane or a multisample texture is served only as the whole of it");
        return nullptr;
    }
    TextureDesc d = p.desc_;
    d.format = format;
    d.type = type;
    // every level from 0, the slices all there, the type the parent's: the same layout, its first LEVELS levels
    if (type == p.type() && baseLevel == 0 && baseSlice == 0 && slices == parentSlices) {
        d.mipLevels = levels;
        return std::shared_ptr<Texture>(new Texture(p.buffer_, d, info, p.pitch_, p.offset_, p.pitchAlign_));
    }
    // one level as a 2D texture: level L's rows, at its pitch, are a one-level 2D layout of their own (the TP reads a
    // single level by PITCH alone); of a layered parent that has the one level, slice BASE_SLICE's rows
    if (type == TextureType::k2D && levels == 1 && slices == 1 && (!layered || p.levels() == 1) && !p.info().depth &&
        (p.type() == TextureType::k2D || layered)) {
        const TextureLevel L = p.level(baseLevel);
        d.width = L.width;
        d.height = L.height;
        d.mipLevels = 1;
        d.depth = 1;
        uint32_t pitchAlign = 0;
        while (pitchAlign < log2u(info->bytesPerPixel) && L.pitch % (64u << (pitchAlign + 1)) == 0) pitchAlign++;
        return std::shared_ptr<Texture>(new Texture(p.buffer_, d, info, L.pitch, p.offset_ + L.offset + uint64_t(baseSlice) * p.sliceBytes(), pitchAlign));
    }
    // a run of a one-level layered texture's slices as a 2D array, or six as a cube: the slices sit sliceBytes apart
    // whichever of the two names them, so the run starts BASE_SLICE slices in
    if (layered && p.levels() == 1 && levels == 1 && (type == TextureType::k2DArray || (type == TextureType::kCube && slices == 6 && p.width() == p.height()))) {
        d.depth = slices;
        return std::shared_ptr<Texture>(new Texture(p.buffer_, d, info, p.pitch_, p.offset_ + uint64_t(baseSlice) * p.sliceBytes(), p.pitchAlign_));
    }
    refuse(why, asked + ": served are every level from 0 in the texture's own type, one level (or one slice of a one-level array or cube) as 2D, "
                        "and a run of a one-level array's or cube's slices as a 2D array, or six as a cube");
    return nullptr;
}

void writeBorderColors(uint8_t* table) {
    for (uint32_t e = 0; e < 3; e++) {
        // every channel 0 or 1: (0,0,0,0), (0,0,0,1), (1,1,1,1)
        const bool c[4] = {e == 2, e == 2, e == 2, e != 0};
        uint8_t* at = table + 128 * e;
        memset(at, 0, 128);
        auto put16 = [&](uint32_t off, uint16_t v) { memcpy(at + off, &v, 2); };
        auto put32 = [&](uint32_t off, uint32_t v) { memcpy(at + off, &v, 4); };
        uint16_t rgb565 = 0, rgb5a1 = 0, rgba4 = 0;
        uint32_t rgb10a2 = 0;
        for (uint32_t i = 0; i < 4; i++) {
            put32(4 * i, c[i] ? 0x3f800000u : 0);         // fp32
            put16(16 + 2 * i, c[i] ? 0xffff : 0);          // ui16, unorm
            put16(24 + 2 * i, c[i] ? 0x7fff : 0);          // si16, snorm
            put16(32 + 2 * i, c[i] ? 0x3c00 : 0);          // fp16
            at[48 + i] = c[i] ? 0xff : 0;                  // ui8, unorm
            at[52 + i] = c[i] ? 0x7f : 0;                  // si8, snorm
            put16(64 + 2 * i, c[i] ? 0x3c00 : 0);          // srgb, fp16 as turnip packs it
            static const uint32_t w565[3] = {5, 6, 5}, at565[3] = {0, 5, 11};
            if (i < 3 && c[i]) rgb565 |= uint16_t(((1u << w565[i]) - 1) << at565[i]);
            if (c[i]) rgb5a1 |= uint16_t((i < 3 ? 0x1fu : 1u) << (5 * i));
            if (c[i]) rgba4 |= uint16_t(0xfu << (4 * i));
            if (c[i]) rgb10a2 |= (i < 3 ? 0x3ffu : 3u) << (10 * i);
        }
        put16(40, rgb565);
        put16(42, rgb5a1);
        put16(44, rgba4);
        put32(56, rgb10a2);
        put32(60, c[0] ? 0xffffffu : 0);                   // z24, the depth from red
    }
}

bool makeSampler(const SamplerDesc& d, Sampler* out, std::string* why) {
    SamplerFields f;
    // MTLSamplerMinMagFilter: Nearest 0, Linear 1 (MTLSampler.h)
    if (d.minFilter > 2 || d.magFilter > 2) {
        return refuse(why, "sampler filters min " + n(d.minFilter) + " mag " + n(d.magFilter) + ": Nearest (0), Linear (1) or bicubic (2)");
    }
    // bicubic is the TP's own cubic filter, which this chip has (freedreno_devices.py a6xx_gen4 has_tex_filter_cubic;
    // turnip's VK_FILTER_CUBIC_EXT, tu6_tex_filter)
    auto filter = [](uint32_t m) { return m == 2 ? A6XX_TEX_CUBIC : m == 1 ? A6XX_TEX_LINEAR : A6XX_TEX_NEAREST; };
    f.min = filter(d.minFilter);
    f.mag = filter(d.magFilter);
    // MTLSamplerMipFilter: NotMipmapped 0 ("sampled as if it only had a single mipmap level", MTLSampler.h) is Gallium's
    // PIPE_TEX_MIPFILTER_NONE, which sets MIPFILTER_LINEAR_FAR (fd6_texture.cc:279-280; turnip: "disables mipmapping");
    // Nearest 1 sets neither bit; Linear 2 sets MIPFILTER_LINEAR_NEAR (fd6_texture.cc:265-270)
    switch (d.mipFilter) {
    case 0: f.mipLinearFar = true; break;
    case 1: break;
    case 2: f.mipLinearNear = true; break;
    default: return refuse(why, "sampler mip filter " + n(d.mipFilter) + ": MTLSamplerMipFilter is 0, 1 or 2");
    }
    // MTLSamplerAddressMode -> a6xx_tex_clamp. ClampToZero (4) and ClampToBorderColor (5) with TransparentBlack are
    // CLAMP_TO_BORDER with BCOLOR 0: entry 0 of the zero table every dispatch points the TP at, which with the alpha-less
    // formats' SWIZ_W = ONE (applied after the border, the XML) is Metal's "transparent zero with alpha, opaque zero
    // without" (MTLSampler.h:49-50). MirrorClampToEdge (1) is A6XX_TEX_MIRROR_CLAMP, which "only works for PoT" textures
    // (fd6_texture.cc:47) by Gallium's account; turnip maps Vulkan's mirror-clamp to it outright and offers the
    // extension unconditionally (tu_util.h:320, tu_device.cc:219), so it is taken, and measured on the phone over a
    // texture of no power-of-two size (probe 9mc). ClampToBorderColor names its colour's entry of the table
    // (writeBorderColors); ClampToZero is entry 0 whatever the colour, so the two with another colour on one sampler,
    // which has one BCOLOR, are refused.
    if (d.borderColor > 2) {
        return refuse(why, "sampler border colour " + n(d.borderColor) + ": MTLSamplerBorderColor is 0, 1 or 2");
    }
    bool toZero = false, toColour = false;
    auto clamp = [&](uint32_t mode, const char* axis, a6xx_tex_clamp* out_) -> bool {
        switch (mode) {
        case 0: *out_ = A6XX_TEX_CLAMP_TO_EDGE; return true;
        case 1: *out_ = A6XX_TEX_MIRROR_CLAMP; return true;
        case 2: *out_ = A6XX_TEX_REPEAT; return true;
        case 3: *out_ = A6XX_TEX_MIRROR_REPEAT; return true;
        case 4: *out_ = A6XX_TEX_CLAMP_TO_BORDER; toZero = true; return true;
        case 5: *out_ = A6XX_TEX_CLAMP_TO_BORDER; toColour = true; return true;
        default: return refuse(why, std::string("sampler ") + axis + " address mode " + n(mode) + ": MTLSamplerAddressMode is 0 to 5");
        }
    };
    if (!clamp(d.sAddress, "s", &f.wrapS) || !clamp(d.tAddress, "t", &f.wrapT) || !clamp(d.rAddress, "r", &f.wrapR)) {
        return false;
    }
    if (toZero && toColour && d.borderColor != 0) {
        return refuse(why, "a sampler clamping one axis to zero (4) and another to border colour " + n(d.borderColor) + ": its one BCOLOR entry cannot be both");
    }
    // Anisotropy as fd6_sampler_state_create takes it (fd6_texture.cc:256-273): ANISO the log2 of half the count, 8 at
    // most, and a linear filter becomes the anisotropic one under it (tex_filter)
    if (d.maxAnisotropy == 0 || d.maxAnisotropy > 16) {
        return refuse(why, "sampler anisotropy " + n(d.maxAnisotropy) + ": MTLSamplerDescriptor.maxAnisotropy is 1 to 16");
    }
    uint32_t aniso = 0;
    for (uint32_t half = std::min<uint32_t>(d.maxAnisotropy >> 1, 8); half != 0; half >>= 1) aniso++;   // util_last_bit
    f.aniso = a6xx_tex_aniso(aniso);
    if (aniso != 0) {
        if (f.min == A6XX_TEX_LINEAR) f.min = A6XX_TEX_ANISO;
        if (f.mag == A6XX_TEX_LINEAR) f.mag = A6XX_TEX_ANISO;
    }
    // MTLCompareFunction is adreno_compare_func number for number (Never 0 ... Always 7; Gallium's "maps 1:1"); the TP
    // compares only for an instruction that asks (sample_compare's shadow sam), so a plain sample through it is plain.
    // Never (0) is no comparison, as Gallium leaves COMPARE_FUNC unwritten without a compare mode.
    if (d.compareFunction > 7) {
        return refuse(why, "sampler compare function " + n(d.compareFunction) + ": MTLCompareFunction is 0 to 7");
    }
    f.compare = d.compareFunction != 0;
    f.compareFunc = adreno_compare_func(d.compareFunction);
    f.unnormCoords = !d.normalizedCoordinates;
    f.minLod88 = lodFixed(d.lodMinClamp);
    f.maxLod88 = lodFixed(d.lodMaxClamp);
    f.chromaLinear = (f.min == A6XX_TEX_LINEAR || f.min == A6XX_TEX_ANISO) && (f.mag == A6XX_TEX_LINEAR || f.mag == A6XX_TEX_ANISO);   // Gallium: both linear
    f.bcolor = toColour ? d.borderColor : 0;
    TexSamp s = texSamp(f);
    for (uint32_t i = 0; i < kTexSampDwords; i++) out->words[i] = s.dword[i];
    return true;
}

}  // namespace hakios::metal
