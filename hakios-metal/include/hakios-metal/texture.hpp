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
    kR8Unorm = 10,
    kRG8Unorm = 30,
    kR32Uint = 53,
    kR32Float = 55,
    kRGBA8Unorm = 70,
    kRGBA8UnormSrgb = 71,
    kBGRA8Unorm = 80,
    kBGRA8UnormSrgb = 81,
    kRGBA16Float = 115,
    kRGBA32Uint = 123,
    kRGBA32Float = 125,
};

// MTLTextureType (MTLTexture.h). Only k2D is served; the others are named here so a refusal can name them back.
enum class TextureType : uint32_t { k1D = 0, k1DArray = 1, k2D = 2, k2DArray = 3, k2DMultisample = 4, kCube = 5, kCubeArray = 6, k3D = 7 };

// MTLTextureUsage bits
constexpr uint32_t kTextureUsageRead = 1;
constexpr uint32_t kTextureUsageWrite = 2;

struct FormatInfo {
    PixelFormat format;
    a6xx::a6xx_format fmt6;    // TEX_CONST_0.FMT: fd6_format_table.c's .tex column, which is also its storage format for every format here
    uint32_t bytesPerPixel;
    a6xx::a3xx_color_swap swap; // WZYX for RGBA-ordered formats, WXYZ for BGRA (fd6_format_table.c:163, 174)
    bool hasAlpha;             // without one SWIZ_W reads ONE (fdl6_format_swiz, fd6_view.c:117-120): Metal's (x, y, z, 1) for such formats
    bool srgb;                 // TEX_CONST_0.SRGB
    KernelElement element;     // the class a kernel may address it as: float for unorm/snorm/float/sRGB, uint for *Uint, int for *Sint
    const char* name;          // Metal's name, for messages
};
// nullptr when the format is not served
const FormatInfo* formatInfo(PixelFormat format);

// the layout rule in one place, for the tests and for the plug-in's bufferBytesPerRow
uint64_t texturePitch(const FormatInfo& format, uint32_t width);                    // align(width * cpp, 64 * cpp)
uint64_t textureBytes(const FormatInfo& format, uint32_t width, uint32_t height);   // align(align(height, 4) * pitch, 4096)

struct TextureDesc {
    TextureType type = TextureType::k2D;
    PixelFormat format = PixelFormat::kRGBA8Unorm;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t usage = kTextureUsageRead;   // kTextureUsage* bits; a kernel that writes a texture made without kTextureUsageWrite is refused
};

class Texture {
public:
    // nullptr with *why: a type, format or usage not served, a width or height outside 1..32767 (TEX_CONST_1's fields
    // are 15 bits), or no memory. The memory is a Buffer of textureBytes() bytes, so CommandBuffer::use()/keep() apply.
    static std::shared_ptr<Texture> make(Gpu& gpu, const TextureDesc& desc, std::string* why);

    const TextureDesc& desc() const { return desc_; }
    uint32_t width() const { return desc_.width; }
    uint32_t height() const { return desc_.height; }
    PixelFormat format() const { return desc_.format; }
    TextureType type() const { return desc_.type; }
    uint32_t usage() const { return desc_.usage; }
    const FormatInfo& info() const { return *info_; }
    uint64_t bytesPerRow() const { return pitch_; }                  // the GPU's pitch
    const std::shared_ptr<Buffer>& buffer() const { return buffer_; }
    uint64_t gpuAddress() const { return buffer_->gpuAddress(); }
    void* contents() const { return buffer_->contents(); }           // the rows, write-combined

    // -replaceRegion:mipmapLevel:withBytes:bytesPerRow: for level 0: H rows of W pixels from BYTES, the caller's rows
    // BYTES_PER_ROW apart, into the texture's rows at (X, Y). False with *why when the region lies outside the texture
    // or the stride is shorter than W pixels.
    bool replace(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* bytes, uint64_t bytesPerRow, std::string* why);
    // -getBytes:bytesPerRow:fromRegion:mipmapLevel:, the other way (the write-combined rows are read once each)
    bool read(uint32_t x, uint32_t y, uint32_t w, uint32_t h, void* bytes, uint64_t bytesPerRow, std::string* why) const;

    // the descriptor a sam/isaml reads this texture through: TYPE 2D, PITCHALIGN log2(cpp), MIPLVLS 0, SAMPLES one,
    // DEPTH 1, ARRAY_PITCH the layer's bytes, the format's swap and swizzle (fd6_view.c:232-250)
    a6xx::TexConst sampledDescriptor() const;
    // the descriptor a stib/ldib addresses it through, at IBO slot numSsbos + index: the same without PITCHALIGN,
    // MIPLVLS and SAMPLES (fd6_view.c:383-397)
    a6xx::TexConst storageDescriptor() const;

private:
    Texture(std::shared_ptr<Buffer> buffer, const TextureDesc& desc, const FormatInfo* info, uint64_t pitch)
        : buffer_(std::move(buffer)), desc_(desc), info_(info), pitch_(pitch) {}
    std::shared_ptr<Buffer> buffer_;
    TextureDesc desc_;
    const FormatInfo* info_;
    uint64_t pitch_;
};

// MTLSamplerDescriptor's fields in Metal's numbering (MTLSampler.h), with Metal's defaults
struct SamplerDesc {
    uint32_t minFilter = 0;              // MTLSamplerMinMagFilter: Nearest 0, Linear 1
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
};

// Metal's descriptor as the a6xx sampler words, as Gallium's fd6_sampler_state_create encodes PIPE's (fd6_texture.cc:250-306):
// NotMipmapped sets MIPFILTER_LINEAR_FAR as Gallium does for PIPE_TEX_MIPFILTER_NONE; ClampToZero and ClampToBorderColor
// with TransparentBlack are CLAMP_TO_BORDER with border colour 0, the zero entry of the table every dispatch points the TP
// at (compute.hpp: kTableBorderColorAt), which with the alpha-less formats' SWIZ_W = ONE is exactly Metal's "transparent
// zero for formats with alpha, opaque zero without". False with *why for what is not served yet: OpaqueBlack/OpaqueWhite
// borders, MirrorClampToEdge (power-of-two sizes only on a6xx, and the sampler does not know the texture), anisotropy
// above 1, a compare function other than Never.
bool makeSampler(const SamplerDesc& desc, Sampler* out, std::string* why);

}  // namespace hakios::metal
