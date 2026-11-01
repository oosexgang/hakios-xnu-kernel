// hakios-metal: textures and samplers as the compute dispatch binds them.
//
// SPDX-License-Identifier: MIT
//
// A Texture is GPU memory in the layout the TP (the texture pipe) samples and the stib/ldib path stores and loads:
// linear, one level, one layer, rows of bytesPerRow() bytes, as Mesa's fdl6_layout lays an a6xx image out when the
// driver chooses the pitch (src/freedreno/fdl/fd6_layout.c:181-201): pitch = align(width * cpp, 64 * cpp), and
// align(align(height, 4) * pitch, 4096) bytes behind it (the last level's height is rounded to 4: "over-fetch on the
// last level", fd6_layout.c:249), the base aligned to 64. The descriptor a sam/isaml reads it through and the one a
// stib/ldib writes it through are made from the same fields (fd6_view.c: fdl6_view_init, the texture descriptor at
// 232-250 and the storage one at 383-397; they differ in PITCHALIGN, MIPLVLS and SAMPLES, which the storage one lacks).
//
// Why the core converts the caller's rows: Metal's -replaceRegion:... and -getBytes:... take the CALLER's stride for
// the caller's memory; the GPU's pitch is this layout's. Copying row by row here keeps the plug-in free of the layout,
// and the write-combined mapping is written in order, one row at a time.
//
// Metal's own numbers name types, formats and sampler fields (MTLTextureType, MTLPixelFormat, MTLSamplerMinMagFilter,
// MTLSamplerAddressMode, MTLCompareFunction), as hakios_metal.h copies MTLCommandBufferStatus: the plug-in passes them
// through and a test reads the same numbers the SDK headers show.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/kernel.hpp"

namespace hakios::metal {

// The MTLPixelFormat values served (MTLPixelFormat.h); every other value is refused by name at creation.
enum class PixelFormat : uint32_t {
    kA8Unorm = 1,
    kB5G6R5Unorm = 40,     // packed in 16 bits, named from the low bits as Gallium names them: B in 0..4, R in 11..15
    kBGR5A1Unorm = 43,     // B in 0..4, G 5..9, R 10..14, A 15 (CoreAnimation backs some layers with it)
    kR8Unorm = 10,
    // the integer and signed-normalised colour formats, one to four channels: each one FMT6 to the TP, the RB and the
    // storage path alike (fd6_format_table.c's VTC rows), WZYX; an integer one is read as texture2d<uint> or <int>
    kR8Snorm = 12,
    kR8Uint = 13,
    kR8Sint = 14,
    kR16Snorm = 22,
    kRG8Snorm = 32,
    kRG8Uint = 33,
    kRG8Sint = 34,
    kR32Sint = 54,
    kRG16Unorm = 60,
    kRG16Snorm = 62,
    kRG16Uint = 63,
    kRG16Sint = 64,
    kRGBA8Snorm = 72,
    kRGBA8Uint = 73,
    kRGBA8Sint = 74,
    kRG32Uint = 103,
    kRG32Sint = 104,
    kRGBA16Unorm = 110,
    kRGBA16Snorm = 112,
    kRGBA16Uint = 113,
    kRGBA16Sint = 114,
    kRGBA32Sint = 124,
    kR16Float = 25,
    kR16Unorm = 20,
    kR16Uint = 23,         // CoreAnimation's 1D gamma tables (3072 x 1, read alone)
    kR16Sint = 24,
    kRG8Unorm = 30,
    kRG16Float = 65,
    kR32Uint = 53,
    kR32Float = 55,
    kRGBA8Unorm = 70,
    kRGBA8UnormSrgb = 71,
    kBGRA8Unorm = 80,
    kBGRA8UnormSrgb = 81,
    kRG32Float = 105,
    kRGBA16Float = 115,
    kRG11B10Float = 92,
    kRGB10A2Unorm = 90,    // red in the low 10 bits, alpha in the top 2 (Gallium's R10G10B10A2_UNORM)
    kBGR10A2Unorm = 94,    // blue in the low 10 bits (B10G10R10A2_UNORM): the same layout, the swap WXYZ    // three unsigned floats packed in 32 bits, red in the low 11 (Gallium's R11G11B10_FLOAT): an HDR target
    kRGBA32Uint = 123,
    kRGBA32Float = 125,
    kDepth32Float = 252,   // a depth attachment only: tiled (TILE6_3), since the hardware has no linear depth buffer
    kDepth32FloatStencil8 = 260,
    kX32Stencil8 = 261,    // a view of a Depth32Float_Stencil8 texture's S8 plane, sampled as a uint: Texture::view alone   // the same depth plane and a separate S8 plane after it (turnip's D32_SFLOAT_S8_UINT)
    // Metal's private 500, the format CoreAnimation asks for over a '420f' or '420v' IOSurface (a decoded photo): both
    // planes as one texture, sampled as (Y, Cb, Cr) with no conversion -- the fragment function applies its own
    // ycbcr_matrix (fixed_frag's fc_has_ycbcr_matrix). The TP's NV12 format reads exactly that. Over a surface alone,
    // and for reading alone.
    kYCbCr8_420_2P = 500,
    // ASTC, 128 bits a block of W x H texels: what an asset catalogue's images are on Apple's GPUs (ImageIO stages them
    // through -newTextureLayoutWithDescriptor:). Sampled alone; written by -replaceRegion: in whole blocks.
    // ETC2 and EAC, 4 x 4 blocks of 8 bytes (one or three channels, a 1-bit alpha) or 16 (two channels, or RGBA): sampled
    // alone, as ASTC is
    kEAC_R11Unorm = 170,
    kEAC_R11Snorm = 172,
    kEAC_RG11Unorm = 174,
    kEAC_RG11Snorm = 176,
    kEAC_RGBA8 = 178,
    kEAC_RGBA8_sRGB = 179,
    kETC2_RGB8 = 180,
    kETC2_RGB8_sRGB = 181,
    kETC2_RGB8A1 = 182,
    kETC2_RGB8A1_sRGB = 183,
    kASTC_4x4_sRGB = 186,
    kASTC_5x4_sRGB = 187,
    kASTC_5x5_sRGB = 188,
    kASTC_6x5_sRGB = 189,
    kASTC_6x6_sRGB = 190,
    kASTC_8x5_sRGB = 192,
    kASTC_8x6_sRGB = 193,
    kASTC_8x8_sRGB = 194,
    kASTC_10x5_sRGB = 195,
    kASTC_10x6_sRGB = 196,
    kASTC_10x8_sRGB = 197,
    kASTC_10x10_sRGB = 198,
    kASTC_12x10_sRGB = 199,
    kASTC_12x12_sRGB = 200,
    kASTC_4x4_LDR = 204,
    kASTC_5x4_LDR = 205,
    kASTC_5x5_LDR = 206,
    kASTC_6x5_LDR = 207,
    kASTC_6x6_LDR = 208,
    kASTC_8x5_LDR = 210,
    kASTC_8x6_LDR = 211,
    kASTC_8x8_LDR = 212,
    kASTC_10x5_LDR = 213,
    kASTC_10x6_LDR = 214,
    kASTC_10x8_LDR = 215,
    kASTC_10x10_LDR = 216,
    kASTC_12x10_LDR = 217,
    kASTC_12x12_LDR = 218,
    // the HDR forms: the same blocks and the same FMT6_ASTC_* (the probe measures whether the texture unit decodes HDR)
    // PVRTC, which the A6xx texture unit has no decoder for: stored as RGBA8 (Texture::replace decodes a whole level)
    kPVRTC_RGB_2BPP = 160,
    kPVRTC_RGB_2BPP_sRGB = 161,
    kPVRTC_RGB_4BPP = 162,
    kPVRTC_RGB_4BPP_sRGB = 163,
    kPVRTC_RGBA_2BPP = 164,
    kPVRTC_RGBA_2BPP_sRGB = 165,
    kPVRTC_RGBA_4BPP = 166,
    kPVRTC_RGBA_4BPP_sRGB = 167,
    kASTC_4x4_HDR = 222,
    kASTC_5x4_HDR = 223,
    kASTC_5x5_HDR = 224,
    kASTC_6x5_HDR = 225,
    kASTC_6x6_HDR = 226,
    kASTC_8x5_HDR = 228,
    kASTC_8x6_HDR = 229,
    kASTC_8x8_HDR = 230,
    kASTC_10x5_HDR = 231,
    kASTC_10x6_HDR = 232,
    kASTC_10x8_HDR = 233,
    kASTC_10x10_HDR = 234,
    kASTC_12x10_HDR = 235,
    kASTC_12x12_HDR = 236,
};

// MTLTextureType (MTLTexture.h). Only k2D is served; the others are named here so a refusal can name them back.
enum class TextureType : uint32_t { k1D = 0, k1DArray = 1, k2D = 2, k2DArray = 3, k2DMultisample = 4, kCube = 5, kCubeArray = 6, k3D = 7 };

// MTLTextureUsage bits
constexpr uint32_t kTextureUsageRead = 1;
constexpr uint32_t kTextureUsageWrite = 2;
constexpr uint32_t kTextureUsageRenderTarget = 4;   // MTLTextureUsageRenderTarget: a draw's colour target (render.hpp); the layout is the same linear one

struct FormatInfo {
    PixelFormat format;
    a6xx::a6xx_format fmt6;    // TEX_CONST_0.FMT: fd6_format_table.c's .tex column, which is also its storage format for every format here
    uint32_t bytesPerPixel;
    a6xx::a3xx_color_swap swap; // WZYX for RGBA-ordered formats, WXYZ for BGRA (fd6_format_table.c:163, 174)
    bool hasAlpha;             // without one SWIZ_W reads ONE (fdl6_format_swiz, fd6_view.c:117-120): Metal's (x, y, z, 1) for such formats
    bool srgb;                 // TEX_CONST_0.SRGB
    KernelElement element;     // the class a kernel may address it as: float for unorm/snorm/float/sRGB, uint for *Uint, int for *Sint
    const char* name;          // Metal's name, for messages
    bool depth = false;        // a depth format: TILE6_3, render-target usage alone, no CPU access (texture.cpp says why)
    bool stencil = false;      // ...with a stencil plane of its own (Texture::stencilPlane)
    bool twoPlane = false;     // luma and an interleaved half-size chroma plane, each with its own address (TEX_CONST_6..8)
    // (blockW and blockH below)
    // a block-compressed format: BLOCK_W x BLOCK_H texels in BYTES_PER_PIXEL bytes (a block's, 16 for ASTC), laid out as
    // fdl6 lays a block format: rows of blocks, the pitch the blocks across times cpp (fdl_set_pitchalign)
    uint32_t blockW = 1, blockH = 1;
    // what the RB and the 2D engine's destination are given where it is not FMT: fd6_format_table.c's colour column
    // (10_10_10_2_UNORM_DEST for RGB10A2); FMT6_NONE when it is the same
    a6xx::a6xx_format rbFmt = a6xx::FMT6_NONE;
    // PVRTC1 at 2 or 4 bits a texel: stored as the RGBA8 it decodes to, the processor decoding a whole level when it is
    // written (decodePvrtc), since no A6xx unit reads it; 0 for every other format
    uint32_t pvrtcBpp = 0;
};
// PVRTC1 data of a W x H level (BPP 2 or 4) into W x H RGBA8 texels, row after row
void decodePvrtc(const uint8_t* data, uint32_t w, uint32_t h, uint32_t bpp, uint8_t* rgba);
// how many bytes of PVRTC1 a W x H level is: blocks of 4 x 4 (8 x 4 at 2 bpp), at least two each way, eight bytes each
uint64_t pvrtcBytes(uint32_t w, uint32_t h, uint32_t bpp);
inline uint32_t blocksAcross(const FormatInfo& f, uint32_t width) { return (width + f.blockW - 1) / f.blockW; }
inline uint32_t blocksDown(const FormatInfo& f, uint32_t height) { return (height + f.blockH - 1) / f.blockH; }
// nullptr when the format is not served
const FormatInfo* formatInfo(PixelFormat format);

// the layout rule in one place, for the tests and for the plug-in's bufferBytesPerRow
uint64_t texturePitch(const FormatInfo& format, uint32_t width);                    // align(width * cpp, 64 * cpp)
uint64_t textureBytes(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t levels = 1);   // align(align(height, 4) * pitch, 4096) for one level

// Level L of a linear 2D texture as fdl6_layout lays it out (fd6_layout.c:181-201, 233-283; freedreno_layout.h:143
// fdl_pitch): the pitch the first level's pitch minified and aligned again to 64 * cpp, the rows the level's height,
// the last level's counted to a multiple of 4, the levels one after another, the whole layer 4096-aligned. The TP
// finds every level from the first one's pitch by this rule, so it is the rule here too, not a choice.
struct TextureLevel {
    uint64_t offset, pitch;
    uint32_t width, height;
};
TextureLevel textureLevel(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t levels, uint32_t level);
// a depth texture's mip level: each level its own tiled layout, one after another (texture.cpp says why)
TextureLevel depthLevel(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t level);

struct TextureDesc {
    TextureType type = TextureType::k2D;
    PixelFormat format = PixelFormat::kRGBA8Unorm;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipLevels = 1;               // MTLTextureDescriptor.mipmapLevelCount: 1 to floor(log2(max(width, height))) + 1
    uint32_t usage = kTextureUsageRead;   // kTextureUsage* bits; a kernel that writes a texture made without kTextureUsageWrite is refused, a draw into one made without kTextureUsageRenderTarget too
    uint32_t depth = 1;                   // a 3D texture's slices (TextureType::k3D, one level); 1 for every other type
    uint32_t samples = 1;                 // MTLTextureDescriptor.sampleCount: 2 or 4 for TextureType::k2DMultisample, 1 for every other type
};

class Texture {
public:
    // nullptr with *why: a type, format or usage not served, a width or height outside 1..32767 (TEX_CONST_1's fields
    // are 15 bits), or no memory. The memory is a Buffer of textureBytes() bytes, so CommandBuffer::use()/keep() apply.
    static std::shared_ptr<Texture> make(Gpu& gpu, const TextureDesc& desc, std::string* why);
    // what make would lay DESC out as, allocating nothing: its format, pitch and bytes (the memory make asks for), or
    // false with make's own refusal. -heapTextureSizeAndAlignWithDescriptor: answers with these bytes so that a heap
    // sized from it holds the texture make then gives
    static bool plan(const TextureDesc& desc, const FormatInfo** info, uint64_t* pitch, uint64_t* bytes, std::string* why);
    // -newTextureWithDescriptor:iosurface:plane: for plane 0: a texture whose rows are IOSurface SURFACE_ID's pixels (the
    // kernel's surface buffer, command.hpp: surfaceBuffer), DESC's format, size and usage. nullptr with *why: no such
    // surface, more than one plane, a size past the surface's, rows the GPU cannot take (a pitch or a start that is not
    // a multiple of 64 bytes, or rows shorter than the width), a FourCC that is not the format's ('BGRA' for the BGRA8
    // pair, 'RGBA' for the RGBA8 pair; 0 is taken as any), a depth format.
    // A two-plane format (YCbCr8_420_2P) takes the surface's second plane too: CHROMA its offset in the surface's memory
    // and its row bytes, which the caller reads off the surface (the kernel describes plane 0 alone).
    struct SurfacePlane {
        uint64_t offset;
        uint32_t rowBytes;
    };
    static std::shared_ptr<Texture> wrapSurface(Gpu& gpu, uint32_t surfaceId, const TextureDesc& desc, std::string* why,
                                                const SurfacePlane* chroma = nullptr);
    // -[MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:]: a texture whose rows are BUFFER's bytes from OFFSET,
    // BYTES_PER_ROW apart -- the same memory, so a write through either is seen through the other. One level of a
    // colour format, 1D or 2D; the rows the TP takes (a pitch and a start that are multiples of 64 bytes, as for a
    // surface), inside the buffer. nullptr with *why.
    static std::shared_ptr<Texture> overBuffer(std::shared_ptr<Buffer> buffer, uint64_t offset, uint64_t bytesPerRow, const TextureDesc& desc, std::string* why);
    // -newTextureViewWithPixelFormat:textureType:levels:slices:: a texture over PARENT's memory (a write through either
    // is seen through the other), in FORMAT -- PARENT's, or another colour format of the same bytes a pixel -- of TYPE,
    // levels BASE_LEVEL .. + LEVELS - 1 and slices BASE_SLICE .. + SLICES - 1 of PARENT. Served: every level from 0 in
    // PARENT's type (a prefix of its layout); one level L as a 2D texture (its rows are a 2D layout of their own); of
    // a one-level 2D array or cube, a slice as 2D, or a run of slices as a 2D array, or six as a cube. nullptr with *why.
    static std::shared_ptr<Texture> view(const std::shared_ptr<Texture>& parent, PixelFormat format, TextureType type, uint32_t baseLevel, uint32_t levels,
                                         uint32_t baseSlice, uint32_t slices, std::string* why);
    uint32_t surface() const { return surface_; }   // the IOSurface's ID, 0 for a texture of its own memory
    // what an argument buffer encodes for this texture (gpuResourceID), never 0, and the texture back from it while it
    // lives (nullptr after, or for an id no texture had)
    uint64_t resourceId() const { return id_; }
    static std::shared_ptr<Texture> byResourceId(uint64_t id);
    // -newTextureViewWithPixelFormat:...swizzle:: TEXTURE read through SWIZZLE (Metal's MTLTextureSwizzle numbers, red
    // to alpha: 0 zero, 1 one, 2 red, 3 green, 4 blue, 5 alpha), over what it already reads -- the format's own swizzle
    // and any of a view it is. Sampling alone: a store is not swizzled, in Metal either. nullptr with *why for a number
    // past 5.
    static std::shared_ptr<Texture> swizzled(const std::shared_ptr<Texture>& texture, const uint8_t swizzle[4], std::string* why);

    const TextureDesc& desc() const { return desc_; }
    uint32_t width() const { return desc_.width; }
    uint32_t height() const { return desc_.height; }
    uint32_t levels() const { return desc_.mipLevels; }
    // level L's place in the texture's memory (L < levels()); for an IOSurface's texture, its one level
    TextureLevel level(uint32_t l) const;
    uint64_t levelAddress(uint32_t l) const { return gpuAddress() + level(l).offset; }
    // a depth format's planes: the depth plane's bytes (its layer), and for Depth32Float_Stencil8 the S8 plane after it,
    // TILE6_3 as the depth one: fdl6_tile_alignment for cpp 1 (pitch to 128 bytes, rows to 32), 4096-aligned
    uint64_t depthPlaneBytes() const;
    uint64_t stencilOffset() const;
    uint64_t stencilPitch() const;
    uint64_t stencilPlaneBytes() const;
    PixelFormat format() const { return desc_.format; }
    TextureType type() const { return desc_.type; }
    uint32_t samples() const { return desc_.samples; }   // 1, or a multisample texture's 2 or 4 (each pixel's samples side by side)
    uint32_t depth() const { return desc_.depth; }
    uint32_t usage() const { return desc_.usage; }
    const FormatInfo& info() const { return *info_; }
    uint64_t bytesPerRow() const { return pitch_; }                  // the GPU's pitch
    const std::shared_ptr<Buffer>& buffer() const { return buffer_; }
    uint64_t gpuAddress() const { return buffer_->gpuAddress() + offset_; }
    void* contents() const {                                          // the rows, write-combined (nullptr for a surface the driver maps nowhere)
        return buffer_->contents() != nullptr ? static_cast<uint8_t*>(buffer_->contents()) + offset_ : nullptr;
    }

    // -replaceRegion:mipmapLevel:withBytes:bytesPerRow: for level 0: H rows of W pixels from BYTES, the caller's rows
    // BYTES_PER_ROW apart, into the texture's rows at (X, Y). False with *why when the region lies outside the texture
    // or the stride is shorter than W pixels.
    bool replace(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* bytes, uint64_t bytesPerRow, std::string* why) {
        return replace(0, x, y, w, h, bytes, bytesPerRow, why);
    }
    // ...of mip level LEVEL, the region inside that level's size
    bool replace(uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* bytes, uint64_t bytesPerRow, std::string* why);
    // -getBytes:bytesPerRow:fromRegion:mipmapLevel:, the other way (the write-combined rows are read once each)
    bool read(uint32_t x, uint32_t y, uint32_t w, uint32_t h, void* bytes, uint64_t bytesPerRow, std::string* why) const {
        return read(0, x, y, w, h, bytes, bytesPerRow, why);
    }
    bool read(uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void* bytes, uint64_t bytesPerRow, std::string* why) const;
    // slices Z to Z + D - 1 of mip level LEVEL, each its rows (bytesPerRow apart) and the next BYTES_PER_IMAGE further on
    bool replaceSlices(uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, const void* bytes, uint64_t bytesPerRow,
                       uint64_t bytesPerImage, std::string* why);
    bool readSlices(uint32_t level, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d, void* bytes, uint64_t bytesPerRow,
                    uint64_t bytesPerImage, std::string* why) const;
    // how far apart its slices are (ARRAY_PITCH): a 3D texture's depth slice (fd6_layout.c's size0 for level 0: the rows,
    // 4096-aligned); an array's or cube's layer, which holds every level of it (fdl6's layer_first: the 2D chain of its
    // levels, 4096-aligned) -- a view keeps its parent's; 0 for any other type
    uint64_t sliceBytes() const;

    // the descriptor a sam/isaml reads this texture through: TYPE 2D, PITCHALIGN log2(cpp), MIPLVLS 0, SAMPLES one,
    // DEPTH 1, ARRAY_PITCH the layer's bytes, the format's swap and swizzle (fd6_view.c:232-250)
    // (with mip levels: all of them, MIPLVLS levels - 1 and the layer the whole of them, fd6_view.c:232-250)
    a6xx::TexConst sampledDescriptor() const;
    // ...of mip level L alone: what a framebuffer fetch reads of an attachment drawn at that level
    a6xx::TexConst levelDescriptor(uint32_t level) const;
    // the descriptor a stib/ldib addresses it through, at IBO slot numSsbos + index: the same without PITCHALIGN,
    // MIPLVLS and SAMPLES (fd6_view.c:383-397)
    a6xx::TexConst storageDescriptor() const;
    // whether a shader that addresses a texture as T (its dim and array form) addresses this one as what it is: the
    // TP reads the descriptor's TYPE and DEPTH, not the instruction's dimension, so any other pairing reads wrong texels
    bool isShaped(const KernelTexture& t) const;

private:
    static std::shared_ptr<Texture> wrapTwoPlanes(std::shared_ptr<Buffer> buffer, const SurfaceInfo& s, const std::string& name, const TextureDesc& desc,
                                                  const FormatInfo* info, const SurfacePlane* chroma, std::string* why);
    Texture(std::shared_ptr<Buffer> buffer, const TextureDesc& desc, const FormatInfo* info, uint64_t pitch, uint64_t offset = 0,
            uint32_t pitchAlign = ~0u, uint32_t surface = 0)
        : buffer_(std::move(buffer)), desc_(desc), info_(info), pitch_(pitch), offset_(offset), pitchAlign_(pitchAlign), surface_(surface) {}
    std::shared_ptr<Buffer> buffer_;
    TextureDesc desc_;
    const FormatInfo* info_;
    uint64_t pitch_;
    uint64_t offset_;        // where the pixels start in the buffer (an IOSurface's offset; 0 for a texture of its own)
    uint32_t pitchAlign_;    // TEX_CONST_2.PITCHALIGN; ~0u: log2(cpp), the layout's own (texturePitch)
    uint32_t surface_;
    uint64_t layerPitch_ = 0;   // a view's: its parent's sliceBytes, which a level or a run of levels does not recompute
    uint8_t swizzle_[4] = {2, 3, 4, 5};   // Metal's numbers (swizzled): the identity unless a swizzled view
    uint32_t swizzleWord(uint32_t dword0) const;   // TEX_CONST_0 with this texture's swizzle over the format's
    uint64_t id_ = 0;
    static std::shared_ptr<Texture> adopt(Texture* t);   // every Texture is made through this: its id, and the registry
    SurfacePlane chroma_{0, 0};   // a two-plane format's second plane, in the buffer
};

// The border colour table a sampler's BCOLOR indexes, 128 bytes an entry: Metal's MTLSamplerBorderColor in its own
// numbering, TransparentBlack 0, OpaqueBlack 1, OpaqueWhite 2, each packed once per format family the TP may read the
// border as (fd6_bcolor_entry: fp32, unorm16, snorm16, fp16, 565, 5551, 4444, unorm8, snorm8, 10:10:10:2, z24, srgb),
// as turnip packs its built-in colours (tu_util.h: tu6_pack_border_color); the TP picks the field of the texture's format
void writeBorderColors(uint8_t* table);

// MTLSamplerDescriptor's fields in Metal's numbering (MTLSampler.h), with Metal's defaults
struct SamplerDesc {
    uint32_t minFilter = 0;              // MTLSamplerMinMagFilter: Nearest 0, Linear 1; 2 is bicubic, which Metal offers a constexpr sampler alone
    uint32_t magFilter = 0;
    uint32_t mipFilter = 0;              // MTLSamplerMipFilter: NotMipmapped 0, Nearest 1, Linear 2
    uint32_t sAddress = 0;               // MTLSamplerAddressMode: ClampToEdge 0, MirrorClampToEdge 1, Repeat 2, MirrorRepeat 3,
    uint32_t tAddress = 0;               //   ClampToZero 4, ClampToBorderColor 5
    uint32_t rAddress = 0;
    bool normalizedCoordinates = true;   // false: Metal's coord::pixel, TEX_SAMP_1.UNNORM_COORDS
    float lodMinClamp = 0.0f;
    float lodMaxClamp = 3.4028235e38f;   // FLT_MAX, Metal's default; clamped to the field's 4095/256 when encoded
    uint32_t maxAnisotropy = 1;
    uint32_t compareFunction = 0;        // MTLCompareFunction: Never 0, Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6, Always 7
    uint32_t borderColor = 0;            // MTLSamplerBorderColor: TransparentBlack 0, OpaqueBlack 1, OpaqueWhite 2
};

// The four TEX_SAMP dwords, computed once at creation: a dispatch copies them into its own table, so a Sampler has no
// life of its own on the GPU and freeing one needs no in-flight case.
struct Sampler {
    uint32_t words[4] = {};
    uint64_t id = 0;   // makeSampler's: what an argument buffer encodes for it (gpuResourceID), never 0
};
// the sampler makeSampler made with ID, as it was made (a copy kept for the process: samplers are 24 bytes and few);
// nullptr for an id makeSampler never gave
const Sampler* samplerById(uint64_t id);

// Metal's descriptor as the a6xx sampler words, as Gallium's fd6_sampler_state_create encodes PIPE's (fd6_texture.cc:250-306):
// NotMipmapped sets MIPFILTER_LINEAR_FAR as Gallium does for PIPE_TEX_MIPFILTER_NONE; ClampToZero and ClampToBorderColor
// with TransparentBlack are CLAMP_TO_BORDER with border colour 0, the zero entry of the table every dispatch points the TP
// at (compute.hpp: kTableBorderColorAt), which with the alpha-less formats' SWIZ_W = ONE is exactly Metal's "transparent
// zero for formats with alpha, opaque zero without". False with *why for what is not served yet: OpaqueBlack/OpaqueWhite
// borders, MirrorClampToEdge (power-of-two sizes only on a6xx, and the sampler does not know the texture), anisotropy
// above 1, a compare function other than Never.
bool makeSampler(const SamplerDesc& desc, Sampler* out, std::string* why);

}  // namespace hakios::metal
