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

// The eleven formats served, each with what fd6_format_table.c says of it: the .tex column (which is also the .rb one
// for every format here, so one FMT serves the sampled and the storage descriptor), the swap (fd6_pipe2swap: WXYZ for
// the BGRA pair at :174, :176; WZYX for the rest), and whether it has alpha (fdl6_format_swiz reads W as ONE without
// one, fd6_view.c:117-120). The sRGB pair is the UNORM format plus TEX_CONST_0_SRGB (:165, :176). The element class
// is Metal's rule for texture2d<T>: unorm, float and sRGB formats are read as float (or half), *Uint as uint.
constexpr FormatInfo kFormats[] = {
    {PixelFormat::kR8Unorm, FMT6_8_UNORM, 1, WZYX, false, false, kElemFloat, "R8Unorm"},
    {PixelFormat::kRG8Unorm, FMT6_8_8_UNORM, 2, WZYX, false, false, kElemFloat, "RG8Unorm"},
    {PixelFormat::kR32Uint, FMT6_32_UINT, 4, WZYX, false, false, kElemUint, "R32Uint"},
    {PixelFormat::kR32Float, FMT6_32_FLOAT, 4, WZYX, false, false, kElemFloat, "R32Float"},
    {PixelFormat::kRGBA8Unorm, FMT6_8_8_8_8_UNORM, 4, WZYX, true, false, kElemFloat, "RGBA8Unorm"},
    {PixelFormat::kRGBA8UnormSrgb, FMT6_8_8_8_8_UNORM, 4, WZYX, true, true, kElemFloat, "RGBA8Unorm_sRGB"},
    {PixelFormat::kBGRA8Unorm, FMT6_8_8_8_8_UNORM, 4, WXYZ, true, false, kElemFloat, "BGRA8Unorm"},
    {PixelFormat::kBGRA8UnormSrgb, FMT6_8_8_8_8_UNORM, 4, WXYZ, true, true, kElemFloat, "BGRA8Unorm_sRGB"},
    {PixelFormat::kRGBA16Float, FMT6_16_16_16_16_FLOAT, 8, WZYX, true, false, kElemFloat, "RGBA16Float"},
    {PixelFormat::kRGBA32Uint, FMT6_32_32_32_32_UINT, 16, WZYX, true, false, kElemUint, "RGBA32Uint"},
    {PixelFormat::kRGBA32Float, FMT6_32_32_32_32_FLOAT, 16, WZYX, true, false, kElemFloat, "RGBA32Float"},
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
    return layerBytes2d(height, texturePitch(format, width));
}

std::shared_ptr<Texture> Texture::make(Gpu& gpu, const TextureDesc& desc, std::string* why) {
    if (desc.type != TextureType::k2D) {
        refuse(why, std::string("texture type ") + n(uint32_t(desc.type)) + " (" + typeName(desc.type) + "): only 2D (2) is served yet");
        return nullptr;
    }
    const FormatInfo* info = formatInfo(desc.format);
    if (info == nullptr) {
        refuse(why, "pixel format " + n(uint32_t(desc.format)) + ": not one of the eleven served (texture.hpp: PixelFormat)");
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
    // PITCHALIGN = log2(pitch alignment / 64) = log2(cpp) for the pitch texturePitch chose (fd6_layout.c:194-201, fd6_view.c:244)
    return texture2d(gpuAddress(), desc_.width, desc_.height, uint32_t(pitch_), log2u(info_->bytesPerPixel), info_->fmt6, info_->swap,
                     info_->hasAlpha, info_->srgb);
}

a6xx::TexConst Texture::storageDescriptor() const {
    return image2d(gpuAddress(), desc_.width, desc_.height, uint32_t(pitch_), info_->fmt6, info_->swap, info_->hasAlpha, info_->srgb);
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
