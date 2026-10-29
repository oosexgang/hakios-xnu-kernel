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

// The sixteen formats served, each with what fd6_format_table.c says of it: the .tex column (which is also the .rb one
// for every format here, so one FMT serves the sampled and the storage descriptor), the swap (fd6_pipe2swap: WXYZ for
// the BGRA pair at :174, :176; WZYX for the rest), and whether it has alpha (fdl6_format_swiz reads W as ONE without
// one, fd6_view.c:117-120). The sRGB pair is the UNORM format plus TEX_CONST_0_SRGB (:165, :176). The element class
// is Metal's rule for texture2d<T>: unorm, float and sRGB formats are read as float (or half), *Uint as uint.
constexpr FormatInfo kFormats[] = {
    // A8: a format of its own to the TP and the RB, which put the one channel in alpha themselves, so the identity
    // swizzle reads (0, 0, 0, a) as Metal's A8Unorm does (fd6_format_table.c:53 _TC(A8_UNORM, A8_UNORM, WZYX))
    {PixelFormat::kA8Unorm, FMT6_A8_UNORM, 1, WZYX, true, false, kElemFloat, "A8Unorm"},
    {PixelFormat::kR8Unorm, FMT6_8_UNORM, 1, WZYX, false, false, kElemFloat, "R8Unorm"},
    {PixelFormat::kR16Float, FMT6_16_FLOAT, 2, WZYX, false, false, kElemFloat, "R16Float"},
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
    uint64_t unit = 64ull * format.bytesPerPixel;
    return (uint64_t(width) * format.bytesPerPixel + unit - 1) / unit * unit;
}

uint64_t textureBytes(const FormatInfo& format, uint32_t width, uint32_t height) {
    if (format.depth) {
        // "Linear D/S is not supported by HW" (fd6_layout.c:157-164): a depth format is laid out TILE6_3 on every level, its
        // rows counted to the tiled height alignment of 16 for cpp 4 (fdl6_tile_alignment, :81-95; the pitch is
        // texturePitch's, the same 64 * cpp), the layer 4096-aligned, and so is the base (base_align, :102-104: the
        // buffer is whole pages)
        uint64_t rows = (uint64_t(height) + 15) / 16 * 16;
        return (rows * texturePitch(format, width) + 4095) / 4096 * 4096;
    }
    return layerBytes2d(height, texturePitch(format, width));
}

std::shared_ptr<Texture> Texture::make(Gpu& gpu, const TextureDesc& desc, std::string* why) {
    if (desc.type != TextureType::k2D) {
        refuse(why, std::string("texture type ") + n(uint32_t(desc.type)) + " (" + typeName(desc.type) + "): only 2D (2) is served yet");
        return nullptr;
    }
    const FormatInfo* info = formatInfo(desc.format);
    if (info == nullptr) {
        refuse(why, "pixel format " + n(uint32_t(desc.format)) + ": not one of the sixteen served (texture.hpp: PixelFormat)");
        return nullptr;
    }
    // TEX_CONST_1's WIDTH and HEIGHT are 15 bits each (a6xx.hpp: A6XX_TEX_CONST_1_WIDTH__MASK)
    if (desc.width == 0 || desc.width > 32767 || desc.height == 0 || desc.height > 32767) {
        refuse(why, "a texture of " + n(desc.width) + " x " + n(desc.height) + ": each side is 1 to 32767 (the descriptor's fields are 15 bits)");
        return nullptr;
    }
    // MTLTextureUsageRenderTarget (4) is taken alone or beside the shader bits: a draw's colour target has the same linear
    // layout as a sampled or stored texture (render.cpp reads RB_MRT_BUF_INFO/PITCH/ARRAY_PITCH/BASE off the same fields,
    // as fdl6_view_init does for both views, fd6_view.c:232-250 and 408-413); what differs is only who writes it
    if ((desc.usage & ~(kTextureUsageRead | kTextureUsageWrite | kTextureUsageRenderTarget)) != 0) {
        refuse(why, "texture usage " + n(desc.usage) + " has bits other than shader read (1), shader write (2) and render target (4): not served");
        return nullptr;
    }
    if (info->depth && desc.usage != kTextureUsageRenderTarget) {
        // the tiled layout has no reader here yet: a depth texture is an attachment the RB writes and reads, nothing else
        refuse(why, std::string("a ") + info->name + " texture with usage " + n(desc.usage) +
                        ": a depth texture is tiled (TILE6_3) and is served as a render target alone (4), not sampled or stored");
        return nullptr;
    }
    uint64_t pitch = texturePitch(*info, desc.width);
    std::shared_ptr<Buffer> buffer = newBuffer(gpu, textureBytes(*info, desc.width, desc.height), why);
    if (buffer == nullptr) {
        return nullptr;
    }
    return std::shared_ptr<Texture>(new Texture(std::move(buffer), desc, info, pitch));
}

namespace {
// the region check both directions share: W x H pixels at (X, Y) inside the texture, and the caller's rows at least a
// row of W pixels apart (Metal's -replaceRegion: asks the same of bytesPerRow)
bool regionFits(const Texture& t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t bytesPerRow, std::string* why) {
    if (uint64_t(x) + w > t.width() || uint64_t(y) + h > t.height()) {
        return refuse(why, "region " + n(w) + " x " + n(h) + " at (" + n(x) + ", " + n(y) + ") lies outside the " + n(t.width()) + " x " +
                               n(t.height()) + " texture");
    }
    uint64_t row = uint64_t(w) * t.info().bytesPerPixel;
    if (h > 1 && bytesPerRow < row) {
        return refuse(why, "bytesPerRow " + n(bytesPerRow) + " is shorter than a row of " + n(w) + " pixels (" + n(row) + " bytes)");
    }
    return true;
}
}  // namespace

bool Texture::replace(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* bytes, uint64_t bytesPerRow, std::string* why) {
    if (info_->depth) {
        return refuse(why, std::string("a ") + info_->name + " texture is tiled (TILE6_3): its bytes are not rows the CPU can address");
    }
    if (contents() == nullptr) {
        return refuse(why, "IOSurface " + n(surface_) + "'s memory is mapped nowhere in this process by the driver: write it through the surface");
    }
    if (!regionFits(*this, x, y, w, h, bytesPerRow, why)) {
        return false;
    }
    uint64_t row = uint64_t(w) * info_->bytesPerPixel;
    auto* dst = static_cast<uint8_t*>(contents()) + uint64_t(y) * pitch_ + uint64_t(x) * info_->bytesPerPixel;
    auto* src = static_cast<const uint8_t*>(bytes);
    // one row at a time, in order: the mapping is write-combined, and a row is the longest run that is contiguous in both
    for (uint32_t r = 0; r < h; r++) {
        memcpy(dst + uint64_t(r) * pitch_, src + uint64_t(r) * bytesPerRow, row);
    }
    return true;
}

bool Texture::read(uint32_t x, uint32_t y, uint32_t w, uint32_t h, void* bytes, uint64_t bytesPerRow, std::string* why) const {
    if (info_->depth) {
        return refuse(why, std::string("a ") + info_->name + " texture is tiled (TILE6_3): its bytes are not rows the CPU can address");
    }
    if (contents() == nullptr) {
        return refuse(why, "IOSurface " + n(surface_) + "'s memory is mapped nowhere in this process by the driver: read it through the surface");
    }
    if (!regionFits(*this, x, y, w, h, bytesPerRow, why)) {
        return false;
    }
    uint64_t row = uint64_t(w) * info_->bytesPerPixel;
    auto* src = static_cast<const uint8_t*>(contents()) + uint64_t(y) * pitch_ + uint64_t(x) * info_->bytesPerPixel;
    auto* dst = static_cast<uint8_t*>(bytes);
    for (uint32_t r = 0; r < h; r++) {
        memcpy(dst + uint64_t(r) * bytesPerRow, src + uint64_t(r) * pitch_, row);
    }
    return true;
}

a6xx::TexConst Texture::sampledDescriptor() const {
    // PITCHALIGN = log2(pitch alignment / 64) = log2(cpp) for the pitch texturePitch chose (fd6_layout.c:194-201, fd6_view.c:244);
    // a surface's rows carry the alignment their pitch has, wrapSurface worked it out
    return texture2d(gpuAddress(), desc_.width, desc_.height, uint32_t(pitch_), pitchAlign_ != ~0u ? pitchAlign_ : log2u(info_->bytesPerPixel), info_->fmt6,
                     info_->swap, info_->hasAlpha, info_->srgb);
}

a6xx::TexConst Texture::storageDescriptor() const {
    return image2d(gpuAddress(), desc_.width, desc_.height, uint32_t(pitch_), info_->fmt6, info_->swap, info_->hasAlpha, info_->srgb);
}

std::shared_ptr<Texture> Texture::wrapSurface(Gpu& gpu, uint32_t surfaceId, const TextureDesc& desc, std::string* why) {
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
    if (s.planes > 1) {
        refuse(why, name + " has " + n(s.planes) + " planes: a single-plane surface is what is served");
        return nullptr;
    }
    if (desc.width == 0 || desc.height == 0 || desc.width > s.width || desc.height > s.height) {
        refuse(why, "a " + n(desc.width) + " x " + n(desc.height) + " texture over the " + n(s.width) + " x " + n(s.height) + " " + name);
        return nullptr;
    }
    // the FourCC of the surface against the format: Metal asks for a compatible one, and a mismatch swaps channels
    const uint32_t bgra = 0x42475241u, rgba = 0x52474241u;   // 'BGRA', 'RGBA'
    const bool bgraFormat = desc.format == PixelFormat::kBGRA8Unorm || desc.format == PixelFormat::kBGRA8UnormSrgb;
    const bool rgbaFormat = desc.format == PixelFormat::kRGBA8Unorm || desc.format == PixelFormat::kRGBA8UnormSrgb;
    if (s.pixelFormat != 0 && !((s.pixelFormat == bgra && bgraFormat) || (s.pixelFormat == rgba && rgbaFormat))) {
        refuse(why, name + "'s pixel format " + n(s.pixelFormat) + " is not " + info->name + "'s: 'BGRA' (0x42475241) takes BGRA8Unorm[_sRGB], 'RGBA' (0x52474241) RGBA8Unorm[_sRGB]");
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

bool makeSampler(const SamplerDesc& d, Sampler* out, std::string* why) {
    SamplerFields f;
    // MTLSamplerMinMagFilter: Nearest 0, Linear 1 (MTLSampler.h)
    if (d.minFilter > 1 || d.magFilter > 1) {
        return refuse(why, "sampler filters min " + n(d.minFilter) + " mag " + n(d.magFilter) + ": MTLSamplerMinMagFilter is Nearest (0) or Linear (1)");
    }
    f.min = d.minFilter == 1 ? A6XX_TEX_LINEAR : A6XX_TEX_NEAREST;
    f.mag = d.magFilter == 1 ? A6XX_TEX_LINEAR : A6XX_TEX_NEAREST;
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
    // (fd6_texture.cc:47) and the sampler does not know the texture: refused.
    auto clamp = [&](uint32_t mode, const char* axis, a6xx_tex_clamp* out_) -> bool {
        switch (mode) {
        case 0: *out_ = A6XX_TEX_CLAMP_TO_EDGE; return true;
        case 2: *out_ = A6XX_TEX_REPEAT; return true;
        case 3: *out_ = A6XX_TEX_MIRROR_REPEAT; return true;
        case 4: *out_ = A6XX_TEX_CLAMP_TO_BORDER; return true;
        case 5:
            if (d.borderColor != 0) {
                return refuse(why, "sampler border colour " + n(d.borderColor) + " (" + (d.borderColor == 1 ? "OpaqueBlack" : d.borderColor == 2 ? "OpaqueWhite" : "?") +
                                       "): only TransparentBlack (0) has an entry in the border table yet");
            }
            *out_ = A6XX_TEX_CLAMP_TO_BORDER;
            return true;
        case 1: return refuse(why, std::string("sampler ") + axis + " address mode 1 (MirrorClampToEdge): power-of-two textures only on a6xx, not served yet");
        default: return refuse(why, std::string("sampler ") + axis + " address mode " + n(mode) + ": MTLSamplerAddressMode is 0 to 5");
        }
    };
    if (!clamp(d.sAddress, "s", &f.wrapS) || !clamp(d.tAddress, "t", &f.wrapT) || !clamp(d.rAddress, "r", &f.wrapR)) {
        return false;
    }
    if (d.maxAnisotropy != 1) {
        return refuse(why, "sampler anisotropy " + n(d.maxAnisotropy) + ": only 1 is served yet (MTLSamplerDescriptor.maxAnisotropy is 1 to 16)");
    }
    if (d.compareFunction != 0) {
        return refuse(why, "sampler compare function " + n(d.compareFunction) + ": only Never (0) is served yet (Metal has no compare mode to gate it; the GPU compares in its depth path)");
    }
    f.unnormCoords = !d.normalizedCoordinates;
    f.minLod88 = lodFixed(d.lodMinClamp);
    f.maxLod88 = lodFixed(d.lodMaxClamp);
    f.chromaLinear = f.min == A6XX_TEX_LINEAR && f.mag == A6XX_TEX_LINEAR;   // Gallium sets it whenever both are linear
    f.bcolor = 0;
    TexSamp s = texSamp(f);
    for (uint32_t i = 0; i < kTexSampDwords; i++) out->words[i] = s.dword[i];
    return true;
}

}  // namespace hakios::metal
