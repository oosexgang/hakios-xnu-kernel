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

TextureLevel textureLevel(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t levels, uint32_t level) {
    const uint64_t align = 64ull * format.bytesPerPixel;   // 1 << pitchalign, pitchalign = cpp_shift + 6
    const uint64_t pitch0 = texturePitch(format, width);
    uint64_t offset = 0;
    TextureLevel out{};
    for (uint32_t l = 0; l <= level; l++) {
        const uint64_t pitch = (std::max<uint64_t>(1, pitch0 >> l) + align - 1) / align * align;
        const uint32_t h = std::max<uint32_t>(1, height >> l);
        const uint64_t rows = l == levels - 1 ? (uint64_t(h) + 3) / 4 * 4 : h;
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
uint64_t s8Bytes(uint32_t width, uint32_t height) { return ((uint64_t(height) + 31) / 32 * 32 * s8Pitch(width) + 4095) / 4096 * 4096; }
}  // namespace

uint64_t textureBytes(const FormatInfo& format, uint32_t width, uint32_t height, uint32_t levels) {
    if (format.stencil) {
        return depthBytes(format, width, height) + s8Bytes(width, height);
    }
    if (levels > 1 && !format.depth) {
        const TextureLevel last = textureLevel(format, width, height, levels, levels - 1);
        return (last.offset + (uint64_t(last.height) + 3) / 4 * 4 * last.pitch + 4095) / 4096 * 4096;
    }
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
    // 1D is a 2D layout one row high under the TP's 1D type (fd6_view.c:247, fdl6_tex_type): CoreAnimation keeps its
    // gamma tables in them
    if (desc.type != TextureType::k2D && desc.type != TextureType::k1D) {
        refuse(why, std::string("texture type ") + n(uint32_t(desc.type)) + " (" + typeName(desc.type) + "): 1D (0) and 2D (2) are served");
        return nullptr;
    }
    if (desc.type == TextureType::k1D && desc.height != 1) {
        refuse(why, "a 1D texture " + n(desc.height) + " high: a 1D texture is one row");
        return nullptr;
    }
    const FormatInfo* info = formatInfo(desc.format);
    if (info == nullptr) {
        refuse(why, "pixel format " + n(uint32_t(desc.format)) + ": not one of the formats served (texture.hpp: PixelFormat)");
        return nullptr;
    }
    if (info->twoPlane) {
        refuse(why, std::string("a ") + info->name + " texture of its own memory: the format is served over a two-plane IOSurface alone");
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
    uint32_t most = 1;
    while ((std::max(desc.width, desc.height) >> most) != 0) most++;   // floor(log2(max(w, h))) + 1
    if (desc.mipLevels == 0 || desc.mipLevels > most || desc.mipLevels > 16) {
        refuse(why, "a " + n(desc.width) + " x " + n(desc.height) + " texture of " + n(desc.mipLevels) + " mip levels: 1 to " + n(std::min<uint32_t>(most, 16)) +
                        " (down to 1 x 1, and MIPLVLS holds 16)");
        return nullptr;
    }
    if (info->depth && desc.mipLevels != 1) {
        refuse(why, std::string("a ") + info->name + " texture of " + n(desc.mipLevels) + " mip levels: a depth texture is tiled, one level is served");
        return nullptr;
    }
    if (info->depth && desc.usage != kTextureUsageRenderTarget) {
        // the tiled layout has no reader here yet: a depth texture is an attachment the RB writes and reads, nothing else
        refuse(why, std::string("a ") + info->name + " texture with usage " + n(desc.usage) +
                        ": a depth texture is tiled (TILE6_3) and is served as a render target alone (4), not sampled or stored");
        return nullptr;
    }
    uint64_t pitch = texturePitch(*info, desc.width);
    std::shared_ptr<Buffer> buffer = newBuffer(gpu, textureBytes(*info, desc.width, desc.height, desc.mipLevels), why);
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
    uint64_t row = uint64_t(w) * t.info().bytesPerPixel;
    if (h > 1 && bytesPerRow < row) {
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
    return textureLevel(*info_, desc_.width, desc_.height, desc_.mipLevels, l);
}

bool Texture::replace(uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* bytes, uint64_t bytesPerRow, std::string* why) {
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
    uint64_t row = uint64_t(w) * info_->bytesPerPixel;
    auto* dst = static_cast<uint8_t*>(contents()) + L.offset + uint64_t(y) * L.pitch + uint64_t(x) * info_->bytesPerPixel;
    auto* src = static_cast<const uint8_t*>(bytes);
    // one row at a time, in order: the mapping is write-combined, and a row is the longest run that is contiguous in both
    for (uint32_t r = 0; r < h; r++) {
        memcpy(dst + uint64_t(r) * L.pitch, src + uint64_t(r) * bytesPerRow, row);
    }
    return true;
}

bool Texture::read(uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void* bytes, uint64_t bytesPerRow, std::string* why) const {
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
    uint64_t row = uint64_t(w) * info_->bytesPerPixel;
    auto* src = static_cast<const uint8_t*>(contents()) + L.offset + uint64_t(y) * L.pitch + uint64_t(x) * info_->bytesPerPixel;
    auto* dst = static_cast<uint8_t*>(bytes);
    for (uint32_t r = 0; r < h; r++) {
        memcpy(dst + uint64_t(r) * bytesPerRow, src + uint64_t(r) * L.pitch, row);
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
    return image2d(gpuAddress(), desc_.width, desc_.height, uint32_t(pitch_), info_->fmt6, info_->swap, info_->hasAlpha, info_->srgb);
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
    if (info == nullptr || info->depth || info->twoPlane) {
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
