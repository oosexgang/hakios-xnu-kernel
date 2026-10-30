// Textures and samplers as objects: the layout rule (fdl6_layout's pitch and size for a linear 2D image), the row
// conversion between the caller's stride and the GPU's, the two descriptors against literals, the sampler words
// against literals, and every refusal by its sentence. Nothing here reaches a dispatch; compute_test does that.
#include <cstring>
#include <vector>

#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/texture.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
struct Rig {
    Rig() : gpu(std::make_unique<FakeGpu>()) {}
    Gpu gpu;
};

std::shared_ptr<Texture> make(Rig& r, PixelFormat f, uint32_t w, uint32_t h, uint32_t usage = kTextureUsageRead | kTextureUsageWrite) {
    TextureDesc d;
    d.format = f;
    d.width = w;
    d.height = h;
    d.usage = usage;
    std::string why;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    return t;
}

std::string refusedMake(Rig& r, const TextureDesc& d) {
    std::string why;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t == nullptr);
    return why;
}

std::string refusedSampler(const SamplerDesc& d) {
    Sampler s;
    std::string why;
    CHECK(!makeSampler(d, &s, &why));
    return why;
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}  // namespace

TEST(texture_pitch_is_mesas_layout_rule) {
    // pitch = align(width * cpp, 64 * cpp) (fd6_layout.c:181-201 with pitchalign = cpp_shift): a row is a whole number
    // of 64 pixels, whatever the pixel's size
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kR8Unorm), 100), 128u);        // align(100, 64)
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kRGBA8Unorm), 100), 512u);     // align(400, 256)
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kRGBA16Float), 7), 512u);      // align(56, 512)
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kRGBA32Float), 100), 2048u);   // align(1600, 1024)
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kRG8Unorm), 64), 128u);        // exactly one unit
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kR32Uint), 64), 256u);
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kR32Uint), 65), 512u);
    // bytes = align(align(height, 4) * pitch, 4096) (fd6_layout.c:249-250, 284, 304-305)
    CHECK_EQ(textureBytes(*formatInfo(PixelFormat::kR32Uint), 64, 32), 8192u);      // 32 * 256
    CHECK_EQ(textureBytes(*formatInfo(PixelFormat::kRGBA8Unorm), 100, 7), 4096u);   // align(8 * 512, 4096)
    CHECK_EQ(textureBytes(*formatInfo(PixelFormat::kRGBA8Unorm), 64, 64), 16384u);
    CHECK_EQ(textureBytes(*formatInfo(PixelFormat::kRGBA8Unorm), 1, 1), 4096u);     // 4 rows of 256, padded to a page
    // the table: every served format's FMT, pixel size, swap, alpha, sRGB and element class
    const FormatInfo* f = formatInfo(PixelFormat::kBGRA8UnormSrgb);
    CHECK(f != nullptr);
    CHECK_EQ(uint32_t(f->fmt6), 48u);
    CHECK_EQ(f->bytesPerPixel, 4u);
    CHECK_EQ(uint32_t(f->swap), uint32_t(a6xx::WXYZ));
    CHECK(f->hasAlpha && f->srgb && f->element == kElemFloat);
    CHECK(strcmp(f->name, "BGRA8Unorm_sRGB") == 0);
    f = formatInfo(PixelFormat::kR32Uint);
    CHECK(f != nullptr && uint32_t(f->fmt6) == 75 && f->bytesPerPixel == 4 && f->swap == a6xx::WZYX && !f->hasAlpha && !f->srgb && f->element == kElemUint);
    f = formatInfo(PixelFormat::kRGBA32Uint);
    CHECK(f != nullptr && uint32_t(f->fmt6) == 131 && f->bytesPerPixel == 16 && f->hasAlpha && f->element == kElemUint);
    f = formatInfo(PixelFormat::kRGBA16Float);
    CHECK(f != nullptr && uint32_t(f->fmt6) == 98 && f->bytesPerPixel == 8 && f->element == kElemFloat);
    CHECK(formatInfo(PixelFormat::kR8Unorm)->fmt6 == a6xx::FMT6_8_UNORM && formatInfo(PixelFormat::kRG8Unorm)->fmt6 == a6xx::FMT6_8_8_UNORM);
    CHECK(formatInfo(PixelFormat::kR32Float)->fmt6 == a6xx::FMT6_32_FLOAT && formatInfo(PixelFormat::kRGBA32Float)->fmt6 == a6xx::FMT6_32_32_32_32_FLOAT);
    CHECK(formatInfo(PixelFormat::kRGBA8UnormSrgb)->srgb && !formatInfo(PixelFormat::kRGBA8Unorm)->srgb);
    CHECK(formatInfo(PixelFormat::kBGRA8Unorm)->swap == a6xx::WXYZ && formatInfo(PixelFormat::kRGBA8Unorm)->swap == a6xx::WZYX);
    // a format outside them is nobody's
    CHECK(formatInfo(PixelFormat(13)) == nullptr);   // R8Uint
    CHECK(formatInfo(PixelFormat(0)) == nullptr);
}

TEST(texture_make_lays_the_memory_out_and_refuses_by_name) {
    Rig r;
    auto t = make(r, PixelFormat::kRGBA8Unorm, 100, 7);
    CHECK_EQ(t->width(), 100u);
    CHECK_EQ(t->height(), 7u);
    CHECK_EQ(t->bytesPerRow(), 512u);
    CHECK_EQ(t->buffer()->length(), 4096u);
    CHECK_EQ(t->gpuAddress(), t->buffer()->gpuAddress());
    CHECK_EQ(t->gpuAddress() % 64, 0u);
    CHECK(t->contents() == t->buffer()->contents());
    CHECK(t->type() == TextureType::k2D && t->format() == PixelFormat::kRGBA8Unorm && t->usage() == 3u);
    CHECK(strcmp(t->info().name, "RGBA8Unorm") == 0);
    // a texture over a buffer: the buffer's own bytes from the offset, at the pitch given, and refused off 64 bytes
    {
        std::string w;
        auto b = newBuffer(r.gpu, 64 * 1024, &w);
        TextureDesc bd;
        bd.format = PixelFormat::kRGBA8Unorm;
        bd.width = 30;
        bd.height = 10;
        bd.usage = kTextureUsageRead;
        auto v = Texture::overBuffer(b, 128, 192, bd, &w);
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK_EQ(v->gpuAddress(), b->gpuAddress() + 128);
            CHECK_EQ(v->bytesPerRow(), 192u);
            CHECK(v->buffer() == b);
        }
        CHECK(Texture::overBuffer(b, 128, 120, bd, &w) == nullptr);
        CHECK(has(w, "multiples of 64 bytes"));
        CHECK(Texture::overBuffer(b, 64 * 1024 - 192, 192, bd, &w) == nullptr);
        CHECK(has(w, "the rows inside the buffer"));
    }
    // the refusals, each on its sentence
    TextureDesc d;
    d.type = TextureType::kCubeArray;
    CHECK(has(refusedMake(r, d), "texture type 6 (CubeArray): 1D (0), 2D (2), 2D array (3), cube (5) and 3D (7) are served"));
    d.type = TextureType::k3D;
    d.mipLevels = 2;
    d.width = d.height = 4;
    CHECK(has(refusedMake(r, d), "a 3D texture 1 deep of 2 levels: 1 to 2048 slices of one level are served"));
    d = TextureDesc{};
    d.depth = 2;
    CHECK(has(refusedMake(r, d), "a depth of 2 for a texture of type 2: only a 3D texture or a 2D array has slices"));
    // a 1D texture is one row, under the TP's 1D type
    d = TextureDesc{};
    d.type = TextureType::k1D;
    d.width = 256;
    d.height = 4;
    CHECK(has(refusedMake(r, d), "a 1D texture 4 high: a 1D texture is one row"));
    d.height = 1;
    {
        std::string w;
        auto one = Texture::make(r.gpu, d, &w);
        CHECK(one != nullptr);
        if (one != nullptr) {
            const a6xx::TexConst c = one->sampledDescriptor();
            CHECK_EQ((c.dword[2] & a6xx::A6XX_TEX_CONST_2_TYPE__MASK) >> a6xx::A6XX_TEX_CONST_2_TYPE__SHIFT, uint32_t(a6xx::A6XX_TEX_1D));
            CHECK_EQ((c.dword[1] & a6xx::A6XX_TEX_CONST_1_WIDTH__MASK) >> a6xx::A6XX_TEX_CONST_1_WIDTH__SHIFT, 256u);
        }
    }
    d = TextureDesc{};
    d.format = PixelFormat(13);
    CHECK(has(refusedMake(r, d), "pixel format 13: not one of the formats served"));
    d = TextureDesc{};
    d.width = 0;
    CHECK(has(refusedMake(r, d), "a texture of 0 x 1: each side is 1 to 32767"));
    d.width = 32768;
    CHECK(has(refusedMake(r, d), "a texture of 32768 x 1: each side is 1 to 32767"));
    d.width = 32767;
    d.height = 0;
    CHECK(has(refusedMake(r, d), "32767 x 0: each side"));
    d = TextureDesc{};
    d.usage = 0x13;   // MTLTextureUsagePixelFormatView (0x10) beside read and write
    CHECK(has(refusedMake(r, d), "texture usage 19 has bits other than shader read (1), shader write (2) and render target (4): not served"));
    d.usage = 8;
    CHECK(has(refusedMake(r, d), "texture usage 8 has bits other than"));
    // and the largest side the fields hold is made
    d = TextureDesc{};
    d.format = PixelFormat::kR8Unorm;
    d.width = 32767;
    d.height = 1;
    std::string why;
    auto wide = Texture::make(r.gpu, d, &why);
    CHECK(wide != nullptr);
    CHECK_EQ(wide->bytesPerRow(), 32768u);
}

TEST(texture_replace_and_read_convert_the_callers_stride) {
    // 100 x 7 RGBA8: the GPU's rows are 512 bytes, the caller's 480 (wider than the 400-byte row, narrower than the
    // pitch). Every pixel distinct, so a row landing in the wrong place cannot read back right.
    Rig r;
    auto t = make(r, PixelFormat::kRGBA8Unorm, 100, 7);
    std::vector<uint8_t> in(480 * 7, 0xee);
    for (uint32_t y = 0; y < 7; y++) {
        for (uint32_t x = 0; x < 100; x++) {
            uint8_t* p = &in[480 * y + 4 * x];
            p[0] = uint8_t(x); p[1] = uint8_t(y); p[2] = uint8_t(x ^ y); p[3] = uint8_t(0x80 + y);
        }
    }
    std::string why;
    CHECK(t->replace(0, 0, 100, 7, in.data(), 480, &why));
    // the GPU's rows hold the pixels at the pitch, not at the caller's stride
    auto* rows = static_cast<const uint8_t*>(t->contents());
    CHECK_EQ(rows[512 * 3 + 4 * 17 + 0], 17u);
    CHECK_EQ(rows[512 * 3 + 4 * 17 + 1], 3u);
    CHECK_EQ(rows[512 * 6 + 4 * 99 + 3], 0x86u);
    // and read back at a third stride, row for row equal to what went in
    std::vector<uint8_t> out(416 * 7, 0);
    CHECK(t->read(0, 0, 100, 7, out.data(), 416, &why));
    bool same = true;
    for (uint32_t y = 0; y < 7; y++) {
        same = same && memcmp(&out[416 * y], &in[480 * y], 400) == 0;
    }
    CHECK(same);
    // a tight stride (exactly a row) is allowed both ways
    std::vector<uint8_t> tight(400 * 7, 0);
    CHECK(t->read(0, 0, 100, 7, tight.data(), 400, &why));
    CHECK(memcmp(tight.data(), out.data(), 400) == 0);
    CHECK(t->replace(0, 0, 100, 7, tight.data(), 400, &why));
}

TEST(texture_replace_of_a_sub_region_leaves_everything_else_alone) {
    // the whole object canary-filled, 5 x 3 pixels written at (3, 2): the bytes outside the region, the row padding
    // between the 400th byte and the 512-byte pitch, and the rows past the 7th are canary afterwards, byte for byte
    Rig r;
    auto t = make(r, PixelFormat::kRGBA8Unorm, 100, 7);
    auto* mem = static_cast<uint8_t*>(t->contents());
    memset(mem, 0xab, size_t(t->buffer()->length()));
    uint8_t pixels[3][5][4];
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 5; x++) {
            pixels[y][x][0] = uint8_t(0x10 + x); pixels[y][x][1] = uint8_t(0x20 + y); pixels[y][x][2] = 0x33; pixels[y][x][3] = 0x44;
        }
    }
    std::string why;
    CHECK(t->replace(3, 2, 5, 3, pixels, 20, &why));
    uint32_t inside = 0, canary = 0, wrong = 0;
    for (uint64_t i = 0; i < t->buffer()->length(); i++) {
        uint64_t row = i / 512, col = i % 512;
        bool in = row >= 2 && row < 5 && col >= 12 && col < 32;
        if (in) {
            uint64_t py = row - 2, px = (col - 12) / 4, c = col % 4;
            if (mem[i] == pixels[py][px][c]) inside++; else wrong++;
        } else {
            if (mem[i] == 0xab) canary++; else wrong++;
        }
    }
    CHECK_EQ(inside, 60u);
    CHECK_EQ(wrong, 0u);
    CHECK_EQ(canary, 4096u - 60u);
    // read of the same region, at a wider stride, gives the pixels back and touches nothing beyond each row
    uint8_t back[3][8][4];
    memset(back, 0xcd, sizeof back);
    CHECK(t->read(3, 2, 5, 3, back, 32, &why));
    CHECK(memcmp(back[0], pixels[0], 20) == 0 && memcmp(back[1], pixels[1], 20) == 0 && memcmp(back[2], pixels[2], 20) == 0);
    CHECK_EQ(back[0][5][0], 0xcdu);
    CHECK_EQ(back[2][7][3], 0xcdu);
    // the refusals: a region past either edge, a stride shorter than the row
    CHECK(!t->replace(96, 0, 5, 1, pixels, 20, &why));
    CHECK(has(why, "region 5 x 1 at (96, 0) lies outside the 100 x 7 texture"));
    CHECK(!t->read(0, 5, 1, 3, back, 32, &why));
    CHECK(has(why, "region 1 x 3 at (0, 5) lies outside the 100 x 7 texture"));
    CHECK(!t->replace(0, 0, 5, 3, pixels, 16, &why));
    CHECK(has(why, "bytesPerRow 16 is shorter than a row of 5 pixels (20 bytes)"));
    CHECK(!t->read(0, 0, 100, 2, back, 399, &why));
    CHECK(has(why, "bytesPerRow 399 is shorter than a row of 100 pixels (400 bytes)"));
    // and none of those touched the memory
    CHECK_EQ(mem[0], 0xabu);
    CHECK_EQ(mem[512 * 2 + 12], 0x10u);
}

TEST(texture_descriptors_are_the_worked_example_at_the_textures_address) {
    // RGBA8Unorm 64 x 64: a6xx_test's worked example (0x0c006880, 0x00200040, 0x20008002, 4, base, DEPTH 1), the base
    // being wherever the fake put the buffer
    Rig r;
    auto t = make(r, PixelFormat::kRGBA8Unorm, 64, 64);
    uint64_t base = t->gpuAddress();
    a6xx::TexConst d = t->sampledDescriptor();
    CHECK_EQ(d.dword[0], 0x0c006880u);
    CHECK_EQ(d.dword[1], 0x00200040u);
    CHECK_EQ(d.dword[2], 0x20008002u);
    CHECK_EQ(d.dword[3], 0x4u);
    CHECK_EQ(d.dword[4], uint32_t(base));
    CHECK_EQ(d.dword[5], uint32_t(base >> 32) | 0x00020000u);
    for (int i = 6; i < 16; i++) CHECK_EQ(d.dword[i], 0u);
    a6xx::TexConst s = t->storageDescriptor();
    CHECK_EQ(s.dword[0], 0x0c006880u);
    CHECK_EQ(s.dword[1], 0x00200040u);
    CHECK_EQ(s.dword[2], 0x20008000u);   // no PITCHALIGN in the storage view
    CHECK_EQ(s.dword[3], 0x4u);
    CHECK_EQ(s.dword[4], uint32_t(base));
    CHECK_EQ(s.dword[5], uint32_t(base >> 32) | 0x00020000u);
    // R32Uint 64 x 32: FMT 75, W reads ONE (SWIZ 0xa880), PITCHALIGN 2 with a 256-byte pitch, 8192 bytes = 2 pages
    auto u = make(r, PixelFormat::kR32Uint, 64, 32);
    d = u->sampledDescriptor();
    CHECK_EQ(d.dword[0], 0x12c0a880u);
    CHECK_EQ(d.dword[1], 0x00100040u);
    CHECK_EQ(d.dword[2], 0x20008002u);
    CHECK_EQ(d.dword[3], 0x2u);
    // RGBA16Float 7 x 7: pitch 512, PITCHALIGN 3 (8-byte pixels), 8 rows counted, one page
    auto h = make(r, PixelFormat::kRGBA16Float, 7, 7);
    d = h->sampledDescriptor();
    CHECK_EQ(d.dword[0], 0x18806880u);
    CHECK_EQ(d.dword[1], 0x00038007u);
    CHECK_EQ(d.dword[2], 0x20010003u);
    CHECK_EQ(d.dword[3], 0x1u);
    // BGRA8Unorm_sRGB: the RGBA8 format with SWAP WXYZ (bit 30) and SRGB (bit 2)
    auto b = make(r, PixelFormat::kBGRA8UnormSrgb, 64, 64);
    CHECK_EQ(b->sampledDescriptor().dword[0], 0x4c006884u);
    CHECK_EQ(b->storageDescriptor().dword[0], 0x4c006884u);
}

TEST(texture_takes_the_render_target_usage_and_its_fields_give_the_mrt_words) {
    // MTLTextureUsageRenderTarget (4): alone, with read, with read and write -- the layout is the one linear layout,
    // so make() takes it as it takes the shader bits (texture.cpp says why)
    Rig r;
    auto rt = make(r, PixelFormat::kRGBA8Unorm, 64, 64, kTextureUsageRenderTarget);
    CHECK_EQ(rt->usage(), 4u);
    CHECK_EQ(make(r, PixelFormat::kRGBA8Unorm, 64, 64, kTextureUsageRenderTarget | kTextureUsageRead)->usage(), 5u);
    CHECK_EQ(make(r, PixelFormat::kRGBA8Unorm, 64, 64, kTextureUsageRenderTarget | kTextureUsageRead | kTextureUsageWrite)->usage(), 7u);
    // and the twin: a bit above it is still refused by name
    TextureDesc d;
    d.usage = kTextureUsageRenderTarget | 0x10;
    CHECK(has(refusedMake(r, d), "texture usage 20 has bits other than shader read (1), shader write (2) and render target (4): not served"));
    // The five RB_MRT[0] words of this 64 x 64 RGBA8Unorm target, off the texture's own fields through a6xx.hpp's encoders
    // (render.cpp's mrtWords reads the same fields through the same encoders; render_test holds the stream to these):
    // BUF_INFO 0x30 (FMT6_8_8_8_8_UNORM 48, TILE6_LINEAR, WZYX: fd6_view.c:408-413), PITCH 256 >> 6 = 4, ARRAY_PITCH
    // 16384 >> 6 = 0x100 (the layer's bytes, a6xx.xml.h:5824-5828), BASE the buffer's address, BASE_GMEM 0 in sysmem;
    // SP_FS_MRT_REG 0x30 with neither integer bit for a unorm format
    const FormatInfo& f = rt->info();
    CHECK_EQ(a6xx::rbMrtBufInfo(f.fmt6, a6xx::TILE6_LINEAR, f.swap), 0x30u);
    CHECK(a6xx::rbMrtPitchValid(uint32_t(rt->bytesPerRow())));
    CHECK_EQ(a6xx::rbMrtPitch(uint32_t(rt->bytesPerRow())), 4u);
    CHECK_EQ(rt->buffer()->length(), 16384u);
    CHECK_EQ(a6xx::rbMrtArrayPitch(uint32_t(rt->buffer()->length())), 0x100u);
    CHECK_EQ(rt->gpuAddress() % a6xx::kMrtBaseAlign, 0u);
    CHECK_EQ(a6xx::spFsMrtReg(f.fmt6, f.element == kElemInt, f.element == kElemUint), 0x30u);
    CHECK(!f.srgb);
    // a twin per word: the pitch left in bytes is not the encoded word; a uint format sets COLOR_UINT; the sRGB pair says so
    CHECK(a6xx::rbMrtPitch(uint32_t(rt->bytesPerRow())) != 256u);
    const FormatInfo& u = *formatInfo(PixelFormat::kR32Uint);
    CHECK_EQ(a6xx::spFsMrtReg(u.fmt6, u.element == kElemInt, u.element == kElemUint), 0x24bu);   // FMT6_32_UINT 75 | COLOR_UINT 0x200
    CHECK_EQ(a6xx::rbMrtBufInfo(formatInfo(PixelFormat::kBGRA8Unorm)->fmt6, a6xx::TILE6_LINEAR, formatInfo(PixelFormat::kBGRA8Unorm)->swap), 0x2030u);   // COLOR_SWAP WXYZ (1) at bit 13
    CHECK(formatInfo(PixelFormat::kRGBA8UnormSrgb)->srgb);
    // a 48 x 20 target: pitch 256 (48 * 4 rounded to 64 pixels), 8192 bytes (20 rows counted to 24, then a page)
    auto odd = make(r, PixelFormat::kRGBA8Unorm, 48, 20, kTextureUsageRenderTarget);
    CHECK_EQ(a6xx::rbMrtPitch(uint32_t(odd->bytesPerRow())), 4u);
    CHECK_EQ(a6xx::rbMrtArrayPitch(uint32_t(odd->buffer()->length())), 0x80u);
}

TEST(sampler_words_follow_galliums_encoding_of_metals_descriptor) {
    Sampler s;
    std::string why;
    // Metal's defaults: nearest, clamp to edge, not mipmapped, normalized, lod 0..FLT_MAX, anisotropy 1, never
    CHECK(makeSampler(SamplerDesc{}, &s, &why));
    CHECK_EQ(s.words[0], 0x920u);
    CHECK_EQ(s.words[1], 0x000fff40u);
    CHECK_EQ(s.words[2], 0u);
    CHECK_EQ(s.words[3], 0u);
    // linear min and mag: XY_MAG | XY_MIN = 0xa, and CHROMA_LINEAR in word 2
    SamplerDesc d;
    d.minFilter = d.magFilter = 1;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[0], 0x92au);
    CHECK_EQ(s.words[1], 0x000fff40u);
    CHECK_EQ(s.words[2], 0x20u);
    // linear mag alone: no CHROMA_LINEAR
    d = SamplerDesc{};
    d.magFilter = 1;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[0], 0x922u);
    CHECK_EQ(s.words[2], 0u);
    // coord::pixel
    d = SamplerDesc{};
    d.normalizedCoordinates = false;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[1], 0x000fff60u);
    // clamp to zero on every axis: CLAMP_TO_BORDER (3) at shifts 5, 8, 11 = 0x1b60, BCOLOR 0 (entry 0 of the zero table)
    d = SamplerDesc{};
    d.sAddress = d.tAddress = d.rAddress = 4;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[0], 0x1b60u);
    CHECK_EQ(s.words[2], 0u);
    // ... and ClampToBorderColor with TransparentBlack is the same words
    d.sAddress = d.tAddress = d.rAddress = 5;
    d.borderColor = 0;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[0], 0x1b60u);
    // repeat and mirrored repeat: 0 and 2
    d = SamplerDesc{};
    d.sAddress = 2;
    d.tAddress = 3;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[0], 0x00000a00u);   // S REPEAT 0 | T MIRROR_REPEAT 2 << 8 | R CLAMP_TO_EDGE 1 << 11
    // the mip filters: nearest sets neither bit, linear sets MIPFILTER_LINEAR_NEAR (bit 0 of word 0)
    d = SamplerDesc{};
    d.mipFilter = 1;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[0], 0x920u);
    CHECK_EQ(s.words[1], 0x000fff00u);
    d.mipFilter = 2;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[0], 0x921u);
    CHECK_EQ(s.words[1], 0x000fff00u);
    // the LOD clamps in 4.8 fixed point: 1.5 -> 0x180; 2.0 -> 0x200; FLT_MAX and anything past 4095/256 -> 0xfff; a
    // negative minimum -> 0
    d = SamplerDesc{};
    d.lodMinClamp = 1.5f;
    d.lodMaxClamp = 2.0f;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[1], 0x18020040u);
    d.lodMinClamp = -3.0f;
    d.lodMaxClamp = 16.0f;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[1], 0x000fff40u);
    d.lodMaxClamp = 15.99609375f;   // 4095/256 exactly: the largest value that is its own encoding
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[1], 0x000fff40u);
    d.lodMaxClamp = 15.99f;
    CHECK(makeSampler(d, &s, &why));
    CHECK_EQ(s.words[1], 0x000ffd40u);   // 15.99 * 256 = 4093.44 -> 0xffd, truncated as Mesa casts
    // the refusals, each on its sentence
    d = SamplerDesc{};
    d.sAddress = 1;
    CHECK(has(refusedSampler(d), "sampler s address mode 1 (MirrorClampToEdge): power-of-two textures only on a6xx, not served yet"));
    d = SamplerDesc{};
    d.tAddress = 5;
    d.borderColor = 1;
    CHECK(has(refusedSampler(d), "sampler border colour 1 (OpaqueBlack): only TransparentBlack (0) has an entry in the border table yet"));
    d.borderColor = 2;
    CHECK(has(refusedSampler(d), "sampler border colour 2 (OpaqueWhite)"));
    d = SamplerDesc{};
    d.rAddress = 6;
    CHECK(has(refusedSampler(d), "sampler r address mode 6: MTLSamplerAddressMode is 0 to 5"));
    d = SamplerDesc{};
    d.maxAnisotropy = 2;
    CHECK(has(refusedSampler(d), "sampler anisotropy 2: only 1 is served yet"));
    d = SamplerDesc{};
    d.compareFunction = 1;
    CHECK(has(refusedSampler(d), "sampler compare function 1: only Never (0) is served yet"));
    d = SamplerDesc{};
    d.minFilter = 3;
    CHECK(has(refusedSampler(d), "sampler filters min 3 mag 0: Nearest (0), Linear (1) or bicubic (2)"));
    // bicubic: the TP's cubic filter in both fields, and no CHROMA_LINEAR (that is for linear alone)
    {
        using namespace a6xx;
        SamplerDesc c;
        c.minFilter = c.magFilter = 2;
        Sampler cs;
        std::string cwhy;
        CHECK(makeSampler(c, &cs, &cwhy));
        CHECK_EQ((cs.words[0] & A6XX_TEX_SAMP_0_XY_MIN__MASK) >> A6XX_TEX_SAMP_0_XY_MIN__SHIFT, uint32_t(A6XX_TEX_CUBIC));
        CHECK_EQ((cs.words[0] & A6XX_TEX_SAMP_0_XY_MAG__MASK) >> A6XX_TEX_SAMP_0_XY_MAG__SHIFT, uint32_t(A6XX_TEX_CUBIC));
    }
    d = SamplerDesc{};
    d.mipFilter = 3;
    CHECK(has(refusedSampler(d), "sampler mip filter 3: MTLSamplerMipFilter is 0, 1 or 2"));
    // a refusal leaves the words alone
    Sampler untouched;
    untouched.words[0] = 0x12345678;
    d = SamplerDesc{};
    d.maxAnisotropy = 16;
    CHECK(!makeSampler(d, &untouched, &why));
    CHECK_EQ(untouched.words[0], 0x12345678u);
}

// A 64 x 64 RGBA8Unorm texture of seven levels, laid out by fdl6_layout's rule and worked out here by hand: every pitch
// at least 64 * cpp = 256 bytes (so levels 0 to 6 all have rows of 256), the levels one after another (64, 32, 16, 8,
// 4, 2 rows), the last one's single row counted as 4, the layer 4096-aligned: offsets 0, 16384, 24576, 28672, 30720,
// 31744, 32256, 33280 bytes in all, 36864 with the alignment. A 100 x 30 R8Unorm one of three: pitch0 128 (100 to 64),
// level 1's 50 to 64, level 2's 25 to 64; rows 30, 15 and 7 counted as 8.
TEST(texture_levels_are_laid_out_as_fdl6_layout_does_it) {
    const FormatInfo& rgba = *formatInfo(PixelFormat::kRGBA8Unorm);
    const uint64_t offsets[7] = {0, 16384, 24576, 28672, 30720, 31744, 32256};
    for (uint32_t l = 0; l < 7; l++) {
        const TextureLevel L = textureLevel(rgba, 64, 64, 7, l);
        CHECK_EQ(L.offset, offsets[l]);
        CHECK_EQ(L.pitch, 256u);
        CHECK_EQ(L.width, 64u >> l);
        CHECK_EQ(L.height, 64u >> l);
    }
    CHECK_EQ(textureBytes(rgba, 64, 64, 7), 36864u);
    const FormatInfo& r8 = *formatInfo(PixelFormat::kR8Unorm);
    CHECK_EQ(textureLevel(r8, 100, 30, 3, 0).pitch, 128u);
    CHECK_EQ(textureLevel(r8, 100, 30, 3, 1).pitch, 64u);
    CHECK_EQ(textureLevel(r8, 100, 30, 3, 1).offset, 30u * 128);
    CHECK_EQ(textureLevel(r8, 100, 30, 3, 2).offset, 30u * 128 + 15u * 64);
    CHECK_EQ(textureLevel(r8, 100, 30, 3, 2).width, 25u);
    CHECK_EQ(textureLevel(r8, 100, 30, 3, 2).height, 7u);
    CHECK_EQ(textureBytes(r8, 100, 30, 3), 8192u);   // 3840 + 960 + 8 * 64 = 5312, to 8192
    // the descriptor: MIPLVLS 6, the layer the whole of them
    Rig r;
    TextureDesc d;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = d.height = 64;
    d.mipLevels = 7;
    std::string why;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    const a6xx::TexConst c = t->sampledDescriptor();
    CHECK_EQ((c.dword[0] & a6xx::A6XX_TEX_CONST_0_MIPLVLS__MASK) >> a6xx::A6XX_TEX_CONST_0_MIPLVLS__SHIFT, 6u);
    CHECK_EQ(c.dword[3], a6xx::A6XX_TEX_CONST_3_ARRAY_PITCH(36864));
    // a level past the bottom, and a level's region, refused by name
    d.mipLevels = 8;
    CHECK(Texture::make(r.gpu, d, &why) == nullptr);
    CHECK(has(why, "8 mip levels: 1 to 7"));
    const uint32_t px = 0;
    CHECK(!t->replace(3, 0, 0, 9, 1, &px, 36, &why));
    CHECK(has(why, "lies outside the 8 x 8 level 3 of the texture"));
}

// ASTC, a block format: fdl6 counts blocks (nblocksx * cpp, cpp 16 for a 128-bit block) and aligns the pitch to 64
// blocks' worth, 1024 bytes, each level's pitch the first one's halved and aligned again, its rows the blocks down (the
// last level's to 4). Worked out by hand for a 100 x 30 ASTC 4x4 of 3 levels: 25 blocks across is 400 bytes, 1024 aligned;
// 8, 4 and 2 (to 4) block rows. -replaceRegion: and -getBytes: move whole blocks, a row of the caller's being a row of
// blocks; a region off the block grid, and a usage other than read, are refused.
TEST(an_astc_texture_is_laid_out_in_blocks) {
    const FormatInfo& astc = *formatInfo(PixelFormat::kASTC_4x4_LDR);
    CHECK_EQ(astc.blockW, 4u);
    CHECK_EQ(astc.bytesPerPixel, 16u);
    CHECK_EQ(texturePitch(astc, 100), 1024u);
    CHECK_EQ(textureLevel(astc, 100, 30, 3, 1).pitch, 1024u);
    CHECK_EQ(textureLevel(astc, 100, 30, 3, 1).offset, 8u * 1024);
    CHECK_EQ(textureLevel(astc, 100, 30, 3, 2).offset, 8u * 1024 + 4u * 1024);
    CHECK_EQ(textureLevel(astc, 100, 30, 3, 2).width, 25u);
    CHECK_EQ(textureBytes(astc, 100, 30, 3), 16384u);
    CHECK(formatInfo(PixelFormat::kASTC_4x4_sRGB)->srgb);
    CHECK_EQ(formatInfo(PixelFormat::kASTC_12x10_LDR)->blockH, 10u);

    Rig r;
    TextureDesc d;
    d.format = PixelFormat::kASTC_4x4_LDR;
    d.width = d.height = 8;
    d.usage = kTextureUsageRead;
    std::string why;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK_EQ(t->bytesPerRow(), 1024u);
    // four blocks, each its own bytes, in rows of two blocks (32 bytes)
    uint8_t blocks[64], back[64];
    for (int i = 0; i < 64; i++) blocks[i] = uint8_t(i * 7 + 3);
    CHECK(t->replace(0, 0, 0, 8, 8, blocks, 32, &why));
    const auto* mem = static_cast<const uint8_t*>(t->contents());
    CHECK(memcmp(mem, blocks, 32) == 0);
    CHECK(memcmp(mem + 1024, blocks + 32, 32) == 0);   // the second row of blocks a pitch down
    memset(back, 0, sizeof back);
    CHECK(t->read(0, 4, 4, 4, 4, back, 16, &why));    // the bottom-right block alone
    CHECK(memcmp(back, blocks + 48, 16) == 0);
    CHECK(!t->replace(0, 2, 0, 4, 4, blocks, 16, &why));
    CHECK(why.find("whole 4 x 4 blocks") != std::string::npos);
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    CHECK(Texture::make(r.gpu, d, &why) == nullptr);
    CHECK(why.find("a compressed format is sampled alone") != std::string::npos);
}

// A 3D texture, one level: each depth slice is the 2D rows, 4096-aligned (fd6_layout.c's size0 for a 3D level 0), the
// descriptor a 3D view of them (fdl6_view_init: TYPE 3D, DEPTH the slices, ARRAY_PITCH and MIN_LAYERSZ one slice). A
// 4x4x3 RGBA8: rows of 256 bytes (64 pixels' alignment), 4 of them, 1024 to 4096 a slice. Slices written and read back.
TEST(a_3d_texture_is_slices_of_a_2d_layout) {
    using namespace a6xx;
    Rig r;
    TextureDesc d;
    d.type = TextureType::k3D;
    d.width = d.height = 4;
    d.depth = 3;
    std::string why;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK_EQ(t->sliceBytes(), 4096u);
    CHECK(t->buffer()->length() >= 3u * 4096);
    const TexConst c = t->sampledDescriptor();
    CHECK_EQ((c.dword[2] & A6XX_TEX_CONST_2_TYPE__MASK) >> A6XX_TEX_CONST_2_TYPE__SHIFT, uint32_t(A6XX_TEX_3D));
    CHECK_EQ((c.dword[3] & A6XX_TEX_CONST_3_ARRAY_PITCH__MASK) >> A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT, 1u);   // 4096 >> 12
    CHECK_EQ((c.dword[3] & A6XX_TEX_CONST_3_MIN_LAYERSZ__MASK) >> A6XX_TEX_CONST_3_MIN_LAYERSZ__SHIFT, 1u);
    CHECK_EQ((c.dword[5] & A6XX_TEX_CONST_5_DEPTH__MASK) >> A6XX_TEX_CONST_5_DEPTH__SHIFT, 3u);
    // every texel its own word: slice z, row y, column x
    uint32_t in[3][4][4], out[3][4][4];
    for (uint32_t z = 0; z < 3; z++)
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++) in[z][y][x] = 0xff000000u | z << 16 | y << 8 | x;
    CHECK(t->replaceSlices(0, 0, 0, 4, 4, 3, in, 16, 64, &why));
    const auto* mem = static_cast<const uint8_t*>(t->contents());
    uint32_t word;
    memcpy(&word, mem + 2 * 4096 + 3 * 256 + 1 * 4, 4);   // slice 2, row 3, column 1, where the layout puts it
    CHECK_EQ(word, 0xff020301u);
    memset(out, 0, sizeof out);
    CHECK(t->readSlices(0, 0, 0, 4, 4, 3, out, 16, 64, &why));
    CHECK(memcmp(in, out, sizeof in) == 0);
    CHECK(!t->replaceSlices(0, 0, 2, 4, 4, 2, in, 16, 64, &why));
    CHECK(has(why, "2 slices from 2 of a 3D texture 3 deep"));
}

// A 2D array, one level: its layers laid out as a 3D texture's slices (4096-aligned each), its descriptor the TP's 2D
// type with DEPTH the layers and ARRAY_PITCH one layer, and no MIN_LAYERSZ (an array view's, fdl6_view_init)
TEST(a_2d_array_is_layers_of_a_2d_layout) {
    using namespace a6xx;
    Rig r;
    TextureDesc d;
    d.type = TextureType::k2DArray;
    d.width = d.height = 4;
    d.depth = 2;
    std::string why;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    const TexConst c = t->sampledDescriptor();
    CHECK_EQ((c.dword[2] & A6XX_TEX_CONST_2_TYPE__MASK) >> A6XX_TEX_CONST_2_TYPE__SHIFT, uint32_t(A6XX_TEX_2D));
    CHECK_EQ((c.dword[3] & A6XX_TEX_CONST_3_ARRAY_PITCH__MASK) >> A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT, 1u);
    CHECK_EQ(c.dword[3] & A6XX_TEX_CONST_3_MIN_LAYERSZ__MASK, 0u);
    CHECK_EQ((c.dword[5] & A6XX_TEX_CONST_5_DEPTH__MASK) >> A6XX_TEX_CONST_5_DEPTH__SHIFT, 2u);
    const uint32_t layer1[16] = {0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24, 0x31, 0x32, 0x33, 0x34, 0x41, 0x42, 0x43, 0x44};
    CHECK(t->replaceSlices(0, 0, 1, 4, 4, 1, layer1, 16, 64, &why));
    uint32_t word;
    memcpy(&word, static_cast<const uint8_t*>(t->contents()) + 4096 + 2 * 256 + 3 * 4, 4);
    CHECK_EQ(word, 0x34u);
}

// A cube: its 6 faces a 2D array's layers in Metal's order, the descriptor the TP's cube type with DEPTH 6 / 6 = 1 and
// ARRAY_PITCH one face (fdl6_view_init's cube view); faces not square, or not 6, refused
TEST(a_cube_is_six_faces_under_the_cube_type) {
    using namespace a6xx;
    Rig r;
    TextureDesc d;
    d.type = TextureType::kCube;
    d.width = d.height = 4;
    d.depth = 6;
    std::string why;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    const TexConst c = t->sampledDescriptor();
    CHECK_EQ((c.dword[2] & A6XX_TEX_CONST_2_TYPE__MASK) >> A6XX_TEX_CONST_2_TYPE__SHIFT, uint32_t(A6XX_TEX_CUBE));
    CHECK_EQ((c.dword[5] & A6XX_TEX_CONST_5_DEPTH__MASK) >> A6XX_TEX_CONST_5_DEPTH__SHIFT, 1u);
    CHECK_EQ((c.dword[3] & A6XX_TEX_CONST_3_ARRAY_PITCH__MASK) >> A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT, 1u);
    CHECK(t->buffer()->length() >= 6u * 4096);
    d.height = 8;
    CHECK(Texture::make(r.gpu, d, &why) == nullptr);
    CHECK(has(why, "square faces, 6 of them"));
}
