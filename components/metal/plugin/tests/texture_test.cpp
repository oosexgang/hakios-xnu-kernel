// Textures and samplers as objects: the layout rule (fdl6_layout's pitch and size for a linear 2D image), the row
// conversion between the caller's stride and the GPU's, the two descriptors against literals, the sampler words
// against literals, and every refusal by its sentence. Nothing here reaches a dispatch; compute_test does that.
#include <cmath>
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
    CHECK(formatInfo(PixelFormat(93)) == nullptr);   // RGB9E5Float
    // the integer ones, each its FMT and class: what tex_copy's lanes measured on the phone (probe 7g) rests on these
    CHECK(formatInfo(PixelFormat::kR8Uint)->fmt6 == a6xx::FMT6_8_UINT && formatInfo(PixelFormat::kR8Uint)->element == kElemUint);
    CHECK(formatInfo(PixelFormat::kRG16Sint)->fmt6 == a6xx::FMT6_16_16_SINT && formatInfo(PixelFormat::kRG16Sint)->element == kElemInt);
    CHECK(formatInfo(PixelFormat::kRGBA8Snorm)->fmt6 == a6xx::FMT6_8_8_8_8_SNORM && formatInfo(PixelFormat::kRGBA8Snorm)->element == kElemFloat);
    CHECK_EQ(formatInfo(PixelFormat::kRGBA16Uint)->bytesPerPixel, 8u);
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
    d.type = TextureType::kCubeArray;   // one face: a cube array is six a cube
    CHECK(has(refusedMake(r, d), "square faces, 6 a cube"));
    d.type = TextureType(8);
    CHECK(has(refusedMake(r, d), "MTLTextureType is 0 to 7"));
    d.type = TextureType::k3D;
    d.mipLevels = 2;
    d.width = d.height = 4;
    CHECK(has(refusedMake(r, d), "a 3D texture 1 deep of 2 levels: 1 to 2048 slices are served, of one level for a 3D texture"));
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
    d.format = PixelFormat(93);
    CHECK(has(refusedMake(r, d), "pixel format 93: not one of the formats served"));
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
    // MirrorClampToEdge is the TP's MIRROR_CLAMP (4); a border colour is its entry in the table (BCOLOR 1, 2), and
    // ClampToZero stays entry 0 whatever the colour
    {
        using namespace a6xx;
        d = SamplerDesc{};
        d.sAddress = 1;
        CHECK(makeSampler(d, &s, &why));
        CHECK_EQ((s.words[0] & A6XX_TEX_SAMP_0_WRAP_S__MASK) >> A6XX_TEX_SAMP_0_WRAP_S__SHIFT, uint32_t(A6XX_TEX_MIRROR_CLAMP));
        for (uint32_t colour = 0; colour < 3; colour++) {
            d = SamplerDesc{};
            d.tAddress = 5;
            d.borderColor = colour;
            CHECK(makeSampler(d, &s, &why));
            CHECK_EQ((s.words[2] & A6XX_TEX_SAMP_2_BCOLOR__MASK) >> A6XX_TEX_SAMP_2_BCOLOR__SHIFT, colour);
            d.tAddress = 4;
            CHECK(makeSampler(d, &s, &why));
            CHECK_EQ((s.words[2] & A6XX_TEX_SAMP_2_BCOLOR__MASK) >> A6XX_TEX_SAMP_2_BCOLOR__SHIFT, 0u);
        }
        // the twin: one axis to zero, another to white -- one BCOLOR cannot be both
        d = SamplerDesc{};
        d.sAddress = 4;
        d.tAddress = 5;
        d.borderColor = 2;
        CHECK(has(refusedSampler(d), "its one BCOLOR entry cannot be both"));
        d.borderColor = 3;
        CHECK(has(refusedSampler(d), "sampler border colour 3: MTLSamplerBorderColor is 0, 1 or 2"));
        // the table those entries index: transparent black, opaque black, opaque white, in each format family's field
        uint8_t table[384];
        writeBorderColors(table);
        uint32_t fp[3][4];
        for (uint32_t e = 0; e < 3; e++) memcpy(fp[e], table + 128 * e, 16);
        CHECK(fp[0][0] == 0 && fp[0][3] == 0 && fp[1][0] == 0 && fp[1][3] == 0x3f800000u && fp[2][0] == 0x3f800000u && fp[2][3] == 0x3f800000u);
        CHECK(table[128 + 48] == 0 && table[128 + 51] == 0xff && table[256 + 48] == 0xff);   // unorm8: black's alpha, white's red
        uint16_t w565 = 0;
        memcpy(&w565, table + 256 + 40, 2);
        CHECK_EQ(w565, 0xffffu);
    }
    // the refusals, each on its sentence
    d = SamplerDesc{};
    d.rAddress = 6;
    CHECK(has(refusedSampler(d), "sampler r address mode 6: MTLSamplerAddressMode is 0 to 5"));
    d = SamplerDesc{};
    d.maxAnisotropy = 17;
    CHECK(has(refusedSampler(d), "sampler anisotropy 17: MTLSamplerDescriptor.maxAnisotropy is 1 to 16"));
    d = SamplerDesc{};
    d.compareFunction = 8;
    CHECK(has(refusedSampler(d), "sampler compare function 8: MTLCompareFunction is 0 to 7"));
    // anisotropy as fd6_sampler_state_create: ANISO = last_bit(min(n >> 1, 8)), a linear filter made the anisotropic
    // one; a nearest one stays nearest. 1 is none (the control)
    {
        using namespace a6xx;
        auto words = [](uint32_t n, uint32_t filter) {
            SamplerDesc a;
            a.maxAnisotropy = n;
            a.minFilter = a.magFilter = filter;
            Sampler out;
            std::string w;
            CHECK(makeSampler(a, &out, &w));
            return out;
        };
        auto aniso = [](const Sampler& x) { return (x.words[0] & A6XX_TEX_SAMP_0_ANISO__MASK) >> A6XX_TEX_SAMP_0_ANISO__SHIFT; };
        auto minF = [](const Sampler& x) { return (x.words[0] & A6XX_TEX_SAMP_0_XY_MIN__MASK) >> A6XX_TEX_SAMP_0_XY_MIN__SHIFT; };
        CHECK_EQ(aniso(words(1, 1)), 0u);
        CHECK_EQ(minF(words(1, 1)), uint32_t(A6XX_TEX_LINEAR));
        CHECK_EQ(aniso(words(2, 1)), 1u);
        CHECK_EQ(aniso(words(4, 1)), 2u);
        CHECK_EQ(aniso(words(8, 1)), 3u);
        CHECK_EQ(aniso(words(16, 1)), 4u);
        CHECK_EQ(minF(words(16, 1)), uint32_t(A6XX_TEX_ANISO));
        CHECK_EQ(minF(words(16, 0)), uint32_t(A6XX_TEX_NEAREST));
        // a compare function: COMPARE_FUNC its number in dword 1 (Less 1, GreaterEqual 6); Never leaves it 0
        auto cmp = [](uint32_t f) {
            SamplerDesc a;
            a.compareFunction = f;
            Sampler out;
            std::string w;
            CHECK(makeSampler(a, &out, &w));
            return (out.words[1] & A6XX_TEX_SAMP_1_COMPARE_FUNC__MASK) >> A6XX_TEX_SAMP_1_COMPARE_FUNC__SHIFT;
        };
        CHECK_EQ(cmp(0), 0u);
        CHECK_EQ(cmp(1), 1u);
        CHECK_EQ(cmp(6), 6u);
    }
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
    d.maxAnisotropy = 17;
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
    CHECK(t->replaceSlices(0, 0, 0, 0, 4, 4, 3, in, 16, 64, &why));
    const auto* mem = static_cast<const uint8_t*>(t->contents());
    uint32_t word;
    memcpy(&word, mem + 2 * 4096 + 3 * 256 + 1 * 4, 4);   // slice 2, row 3, column 1, where the layout puts it
    CHECK_EQ(word, 0xff020301u);
    memset(out, 0, sizeof out);
    CHECK(t->readSlices(0, 0, 0, 0, 4, 4, 3, out, 16, 64, &why));
    CHECK(memcmp(in, out, sizeof in) == 0);
    CHECK(!t->replaceSlices(0, 0, 0, 2, 4, 4, 2, in, 16, 64, &why));
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
    CHECK(t->replaceSlices(0, 0, 0, 1, 4, 4, 1, layer1, 16, 64, &why));
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

// A multisample texture is the memory of a texture SAMPLES times as wide (fdl6_layout's cpp times the count), its
// logical size the pixels: the single-sample texture of the same size is the control, a quarter of the pitch.
TEST(a_multisample_texture_is_its_samples_side_by_side) {
    Rig r;
    TextureDesc one;
    one.width = 100;
    one.height = 10;
    one.usage = kTextureUsageRenderTarget;
    TextureDesc four = one;
    four.type = TextureType::k2DMultisample;
    four.samples = 4;
    std::string why;
    auto a = Texture::make(r.gpu, one, &why);
    auto m = Texture::make(r.gpu, four, &why);
    CHECK(a != nullptr && m != nullptr);
    if (a == nullptr || m == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK_EQ(m->width(), 100u);
    CHECK_EQ(m->samples(), 4u);
    CHECK_EQ(a->bytesPerRow(), uint64_t(texturePitch(*formatInfo(PixelFormat::kRGBA8Unorm), 100)));
    CHECK_EQ(m->bytesPerRow(), uint64_t(texturePitch(*formatInfo(PixelFormat::kRGBA8Unorm), 400)));
    CHECK(m->buffer()->length() >= 10u * m->bytesPerRow());
    // the CPU cannot address samples, as Metal says
    uint8_t px[4] = {};
    CHECK(!m->replace(0, 0, 0, 1, 1, px, 4, &why));
    CHECK(has(why, "a multisample texture's samples are the GPU's alone"));
    // the refusals: a sample count of 3, a count on a 2D texture, a multisample texture that is not a render target
    TextureDesc bad = four;
    bad.samples = 3;
    CHECK(has(refusedMake(r, bad), "with 3 samples: a 2D multisample texture (4) of 2 or 4, every other type of 1"));
    bad = one;
    bad.samples = 4;
    CHECK(has(refusedMake(r, bad), "a texture of type 2 with 4 samples"));
    bad = four;
    bad.usage = kTextureUsageRead;
    CHECK(has(refusedMake(r, bad), "a render target (4) alone, is served"));
}

// A multisample Depth32Float is fdl6's tiled layout at cpp 4 * samples: its pitch aligned to 64 such texels and its rows
// counted to 16. The single-sample depth of the same size is the control (cpp 4, 64 texels of 4 bytes).
TEST(a_multisample_depth_is_tiled_at_its_samples_cpp) {
    Rig r;
    TextureDesc one;
    one.format = PixelFormat::kDepth32Float;
    one.width = 100;
    one.height = 20;
    one.usage = kTextureUsageRenderTarget;
    TextureDesc four = one;
    four.type = TextureType::k2DMultisample;
    four.samples = 4;
    std::string why;
    auto a = Texture::make(r.gpu, one, &why);
    auto m = Texture::make(r.gpu, four, &why);
    CHECK(a != nullptr && m != nullptr);
    if (a == nullptr || m == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK_EQ(a->bytesPerRow(), 512u);    // 100 * 4 to 256
    CHECK_EQ(m->bytesPerRow(), 2048u);   // 100 * 16 to 1024
    CHECK_EQ(m->buffer()->length(), 32u * 2048);   // 20 rows to 32, 64 KiB
    // a stencil plane is not served multisampled, nor a multisample depth that is stored into
    TextureDesc bad = four;
    bad.format = PixelFormat::kDepth32FloatStencil8;
    CHECK(has(refusedMake(r, bad), "a multisample Depth32Float_Stencil8 texture"));
    bad = four;
    bad.usage = kTextureUsageRenderTarget | kTextureUsageWrite;   // (read beside render target is taken, as sokol asks)
    CHECK(has(refusedMake(r, bad), "sampled too (5), not stored"));
    TextureDesc readToo = four;
    readToo.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    CHECK(Texture::make(r.gpu, readToo, &why) != nullptr);
}

// A view is the parent's memory under other names: its rows start where the parent's level or slice does, at that
// level's pitch, and a write through one is in the other's bytes. The twins are what is not a view of those bytes --
// another size a pixel, a level run that does not start at 0 (its layout is not the parent's), a range outside.
TEST(a_texture_view_names_a_level_or_a_slice_of_its_parents_memory) {
    Rig r;
    std::string why;
    TextureDesc d;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = 64;
    d.height = 48;
    d.mipLevels = 4;
    auto mip = Texture::make(r.gpu, d, &why);
    CHECK(mip != nullptr);
    if (mip == nullptr) return;
    for (uint32_t l = 0; l < 4; l++) {
        auto v = Texture::view(mip, PixelFormat::kRGBA8UnormSrgb, TextureType::k2D, l, 1, 0, 1, &why);
        CHECK(v != nullptr);
        if (v == nullptr) {
            std::printf("       level %u: %s\n", l, why.c_str());
            continue;
        }
        const TextureLevel L = mip->level(l);
        CHECK_EQ(v->width(), L.width);
        CHECK_EQ(v->height(), L.height);
        CHECK_EQ(v->levels(), 1u);
        CHECK_EQ(v->bytesPerRow(), L.pitch);
        CHECK_EQ(v->gpuAddress(), mip->levelAddress(l));
        CHECK(v->buffer() == mip->buffer());
        CHECK(v->info().srgb);
        // a write through the view is in the parent's level l
        const uint32_t word = 0x11223344u + l;
        CHECK(v->replace(1, 1, 1, 1, &word, 4, &why));
        uint32_t back = 0;
        CHECK(mip->read(l, 1, 1, 1, 1, &back, 4, &why));
        CHECK_EQ(back, word);
    }
    auto prefix = Texture::view(mip, PixelFormat::kRGBA8Unorm, TextureType::k2D, 0, 2, 0, 1, &why);
    CHECK(prefix != nullptr);
    if (prefix != nullptr) CHECK_EQ(prefix->levelAddress(1), mip->levelAddress(1));
    CHECK(Texture::view(mip, PixelFormat::kRG8Unorm, TextureType::k2D, 0, 1, 0, 1, &why) == nullptr);
    CHECK(has(why, "as many bytes a pixel"));
    CHECK(Texture::view(mip, PixelFormat::kRGBA8Unorm, TextureType::k2D, 1, 2, 0, 1, &why) == nullptr);
    CHECK(has(why, "served are"));
    CHECK(Texture::view(mip, PixelFormat::kRGBA8Unorm, TextureType::k2D, 3, 2, 0, 1, &why) == nullptr);
    CHECK(has(why, "outside the texture"));

    TextureDesc a;
    a.type = TextureType::k2DArray;
    a.format = PixelFormat::kRGBA16Float;
    a.width = a.height = 16;
    a.depth = 6;
    auto arr = Texture::make(r.gpu, a, &why);
    CHECK(arr != nullptr);
    if (arr == nullptr) return;
    auto s2 = Texture::view(arr, PixelFormat::kRGBA16Float, TextureType::k2D, 0, 1, 2, 1, &why);
    CHECK(s2 != nullptr);
    if (s2 != nullptr) {
        CHECK_EQ(s2->gpuAddress(), arr->gpuAddress() + 2 * arr->sliceBytes());
        CHECK_EQ(s2->bytesPerRow(), arr->bytesPerRow());
        CHECK(s2->type() == TextureType::k2D);
    }
    auto run = Texture::view(arr, PixelFormat::kRGBA16Float, TextureType::k2DArray, 0, 1, 3, 2, &why);
    CHECK(run != nullptr);
    if (run != nullptr) {
        CHECK_EQ(run->depth(), 2u);
        CHECK_EQ(run->gpuAddress(), arr->gpuAddress() + 3 * arr->sliceBytes());
        CHECK_EQ(run->sliceBytes(), arr->sliceBytes());
    }
    auto cube = Texture::view(arr, PixelFormat::kRGBA16Float, TextureType::kCube, 0, 1, 0, 6, &why);
    CHECK(cube != nullptr);
    CHECK(Texture::view(arr, PixelFormat::kRGBA16Float, TextureType::kCube, 0, 1, 1, 5, &why) == nullptr);
    CHECK(Texture::view(arr, PixelFormat::kRGBA16Float, TextureType::k2D, 0, 1, 6, 1, &why) == nullptr);
    CHECK(has(why, "outside the texture"));
}

// plan is make without the memory: for every shape make serves, the bytes it plans are the bytes the texture's Buffer
// has -- the promise a heap sized by -heapTextureSizeAndAlignWithDescriptor: rests on -- and it refuses what make does
TEST(a_textures_plan_is_the_memory_make_gives_it) {
    Rig r;
    auto shaped = [](TextureType type, PixelFormat f, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t samples, uint32_t usage) {
        TextureDesc d;
        d.type = type;
        d.format = f;
        d.width = w;
        d.height = h;
        d.depth = depth;
        d.mipLevels = levels;
        d.samples = samples;
        d.usage = usage;
        return d;
    };
    const TextureDesc descs[] = {
        shaped(TextureType::k2D, PixelFormat::kRGBA8Unorm, 100, 37, 1, 1, 1, kTextureUsageRead),
        shaped(TextureType::k2D, PixelFormat::kRGBA16Float, 64, 64, 1, 7, 1, kTextureUsageRead),
        shaped(TextureType::k2DArray, PixelFormat::kR8Uint, 33, 17, 5, 1, 1, kTextureUsageRead | kTextureUsageWrite),
        shaped(TextureType::k3D, PixelFormat::kRG16Float, 16, 16, 9, 1, 1, kTextureUsageRead),
        shaped(TextureType::k2DMultisample, PixelFormat::kBGRA8Unorm, 50, 40, 1, 1, 4, kTextureUsageRenderTarget),
        shaped(TextureType::k2D, PixelFormat::kDepth32Float, 70, 30, 1, 1, 1, kTextureUsageRenderTarget | kTextureUsageRead),
    };
    for (const TextureDesc& d : descs) {
        const FormatInfo* info = nullptr;
        uint64_t pitch = 0, bytes = 0;
        std::string why;
        CHECK(Texture::plan(d, &info, &pitch, &bytes, &why));
        auto t = Texture::make(r.gpu, d, &why);
        CHECK(t != nullptr);
        if (t == nullptr) continue;
        CHECK_EQ(bytes, t->buffer()->length());
        CHECK_EQ(pitch, t->bytesPerRow());
    }
    // what make refuses, plan refuses in the same words
    TextureDesc bad = shaped(TextureType::k2D, PixelFormat(93), 4, 4, 1, 1, 1, kTextureUsageRead);
    const FormatInfo* info = nullptr;
    uint64_t pitch = 0, bytes = 0;
    std::string why;
    CHECK(!Texture::plan(bad, &info, &pitch, &bytes, &why));
    CHECK(has(why, "pixel format 93: not one of the formats served"));
}

// A 1D array a kernel writes (Resident Evil 4 asks for a 512 x 1 R16Float one with usage 3): made, and stored through
// a 2D view whose rows are its layers -- ir3 addresses it by (x, layer), two coordinates -- a slice apart. The twin: a
// 2D array keeps its layered view (TYPE 2D, ARRAY_PITCH), so the rows-for-layers form is the 1D array's alone.
TEST(a_1d_array_is_stored_through_the_2d_image_of_its_layers) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc d;
    d.type = TextureType::k1DArray;
    d.format = PixelFormat::kR16Float;
    d.width = 512;
    d.height = 1;
    d.depth = 4;
    d.usage = kTextureUsageRead | kTextureUsageWrite;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    const TexConst s = t->storageDescriptor();
    CHECK_EQ(s.dword[1] & A6XX_TEX_CONST_1_WIDTH__MASK, 512u);
    CHECK_EQ((s.dword[1] & A6XX_TEX_CONST_1_HEIGHT__MASK) >> A6XX_TEX_CONST_1_HEIGHT__SHIFT, 4u);
    CHECK_EQ(s.dword[2] & A6XX_TEX_CONST_2_PITCH__MASK, A6XX_TEX_CONST_2_PITCH(uint32_t(t->sliceBytes())));
    TextureDesc a = d;
    a.type = TextureType::k2DArray;
    auto t2 = Texture::make(r.gpu, a, &why);
    CHECK(t2 != nullptr);
    if (t2 == nullptr) return;
    const TexConst s2 = t2->storageDescriptor();
    CHECK_EQ((s2.dword[1] & A6XX_TEX_CONST_1_HEIGHT__MASK) >> A6XX_TEX_CONST_1_HEIGHT__SHIFT, 1u);
    CHECK(s2.dword[3] != 0);   // ARRAY_PITCH: the layered view
}

// The views Resident Evil 4 makes: a one-level 2D target as a one-slice 2D array, and a cube as a cube array of one
// cube. Each reads the parent's own memory -- the same address, the same pitch -- and a cube array takes whole sixes.
TEST(a_2d_texture_views_as_an_array_and_a_cube_as_a_cube_array) {
    Rig r;
    std::string why;
    TextureDesc d;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = 64;
    d.height = 48;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto flat = Texture::make(r.gpu, d, &why);
    CHECK(flat != nullptr);
    if (flat == nullptr) return;
    auto arr = Texture::view(flat, PixelFormat::kRGBA8Unorm, TextureType::k2DArray, 0, 1, 0, 1, &why);
    CHECK(arr != nullptr);
    if (arr != nullptr) {
        CHECK(arr->type() == TextureType::k2DArray);
        CHECK_EQ(arr->depth(), 1u);
        CHECK_EQ(arr->gpuAddress(), flat->gpuAddress());
        CHECK_EQ(arr->level(0).pitch, flat->level(0).pitch);
    }
    TextureDesc c = d;
    c.type = TextureType::kCube;
    c.width = c.height = 32;
    c.depth = 6;
    c.usage = kTextureUsageRead;
    auto cube = Texture::make(r.gpu, c, &why);
    CHECK(cube != nullptr);
    if (cube == nullptr) return;
    auto cubes = Texture::view(cube, PixelFormat::kRGBA8Unorm, TextureType::kCubeArray, 0, 1, 0, 6, &why);
    CHECK(cubes != nullptr);
    if (cubes != nullptr) {
        CHECK(cubes->type() == TextureType::kCubeArray);
        CHECK_EQ(cubes->depth(), 6u);
        CHECK_EQ(cubes->gpuAddress(), cube->gpuAddress());
    }
    CHECK(Texture::view(cube, PixelFormat::kRGBA8Unorm, TextureType::kCubeArray, 0, 1, 1, 5, &why) == nullptr);
}

// ETC2 and EAC, the other block formats the TP samples: 4 x 4 blocks of 8 bytes (RGB8, RGB8A1, R11) or 16 (RGBA8, RG11),
// laid out as ASTC is -- 100 texels across are 25 blocks, 200 bytes for an 8-byte block, aligned to 64 blocks' worth
// (512); 400 for a 16-byte one (1024). Resident Evil 4 probes ETC2_RGB8A1 with a 4 x 4 texture; sampled alone.
TEST(an_etc2_texture_is_laid_out_in_blocks_of_its_size) {
    const FormatInfo& rgb8a1 = *formatInfo(PixelFormat::kETC2_RGB8A1);
    CHECK_EQ(rgb8a1.blockW, 4u);
    CHECK_EQ(rgb8a1.bytesPerPixel, 8u);
    CHECK_EQ(texturePitch(rgb8a1, 100), 512u);
    CHECK_EQ(texturePitch(*formatInfo(PixelFormat::kEAC_RGBA8), 100), 1024u);
    CHECK(formatInfo(PixelFormat::kETC2_RGB8_sRGB)->srgb);
    CHECK(!formatInfo(PixelFormat::kETC2_RGB8)->hasAlpha);
    Rig r;
    TextureDesc d;
    d.format = PixelFormat::kETC2_RGB8A1;
    d.width = d.height = 4;
    d.usage = kTextureUsageRead;
    std::string why;
    CHECK(Texture::make(r.gpu, d, &why) != nullptr);
    d.usage = kTextureUsageRead | kTextureUsageWrite;
    CHECK(Texture::make(r.gpu, d, &why) == nullptr);
}

// Arrays and cubes with mip levels, as fdl6 lays them (layer_first): each layer the 2D chain of its levels, the layers
// that chain's size apart, 4096-aligned -- so level L of layer z is z layers and L's own offset in. Resident Evil 4
// makes a 5-level cube (its reflection probe, RG11B10Float) and views of each level as a cube, and 2-level arrays of 64
// layers. The checks: the layer size, a level view's address and its layer pitch kept from the parent (not one level's
// own size), and bytes written at level 2 of face 3 found there and nowhere else.
TEST(an_array_or_a_cube_with_mip_levels_is_layer_after_layer) {
    Rig r;
    std::string why;
    TextureDesc d;
    d.type = TextureType::kCube;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = d.height = 32;
    d.depth = 6;
    d.mipLevels = 5;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto cube = Texture::make(r.gpu, d, &why);
    CHECK(cube != nullptr);
    if (cube == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    const FormatInfo& f = *formatInfo(PixelFormat::kRGBA8Unorm);
    const uint64_t layer = textureBytes(f, 32, 32, 5);
    CHECK_EQ(cube->sliceBytes(), layer);
    CHECK(cube->buffer()->length() >= layer * 6);
    auto level4 = Texture::view(cube, PixelFormat::kRGBA8Unorm, TextureType::kCube, 4, 1, 0, 6, &why);
    CHECK(level4 != nullptr);
    if (level4 != nullptr) {
        CHECK_EQ(level4->gpuAddress(), cube->gpuAddress() + cube->level(4).offset);
        CHECK_EQ(level4->sliceBytes(), layer);
        CHECK_EQ(level4->width(), 2u);
    }
    auto all = Texture::view(cube, PixelFormat::kRGBA8Unorm, TextureType::kCubeArray, 0, 5, 0, 6, &why);
    CHECK(all != nullptr && all->sliceBytes() == layer);
    // 8 x 8 at level 2 of face 3, read back from the same place, and the bytes there in the layout
    std::vector<uint32_t> in(64), out(64, 0);
    for (uint32_t i = 0; i < 64; i++) in[i] = 0x01020304u * (i + 1);
    CHECK(cube->replaceSlices(2, 0, 0, 3, 8, 8, 1, in.data(), 32, 256, &why));
    CHECK(cube->readSlices(2, 0, 0, 3, 8, 8, 1, out.data(), 32, 256, &why));
    CHECK(in == out);
    const TextureLevel L2 = cube->level(2);
    const auto* at = static_cast<const uint8_t*>(cube->contents()) + 3 * layer + L2.offset + L2.pitch;   // row 1 of it
    CHECK(memcmp(at, &in[8], 32) == 0);
    // the game's arrays: 64 layers of 2 levels
    TextureDesc a;
    a.type = TextureType::k2DArray;
    a.format = PixelFormat::kR8Unorm;
    a.width = a.height = 32;
    a.depth = 64;
    a.mipLevels = 2;
    a.usage = kTextureUsageRead;
    CHECK(Texture::make(r.gpu, a, &why) != nullptr);
}

// A whole 2D chain (and a depth buffer) as a one-slice 2D array, which Resident Evil 4 asks of its targets with 4, 5 and
// 8 levels: the same address, every level, one layer; the depth view keeps its tiling (TILE6_3) through the array's
// descriptor. A view starting past level 0 of more than one level is still refused -- its chain would not be the
// parent's.
TEST(a_2d_chain_and_a_depth_buffer_view_as_one_slice_arrays) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc d;
    d.format = PixelFormat::kRGBA16Float;
    d.width = 64;
    d.height = 32;
    d.mipLevels = 5;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    auto a = Texture::view(t, PixelFormat::kRGBA16Float, TextureType::k2DArray, 0, 5, 0, 1, &why);
    CHECK(a != nullptr);
    if (a != nullptr) {
        CHECK_EQ(a->gpuAddress(), t->gpuAddress());
        CHECK_EQ(a->levels(), 5u);
        CHECK_EQ(a->depth(), 1u);
        CHECK_EQ(a->level(3).offset, t->level(3).offset);
    }
    CHECK(Texture::view(t, PixelFormat::kRGBA16Float, TextureType::k2DArray, 1, 2, 0, 1, &why) == nullptr);
    TextureDesc z;
    z.format = PixelFormat::kDepth32Float;
    z.width = 64;
    z.height = 32;
    z.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto depth = Texture::make(r.gpu, z, &why);
    CHECK(depth != nullptr);
    if (depth == nullptr) return;
    auto za = Texture::view(depth, PixelFormat::kDepth32Float, TextureType::k2DArray, 0, 1, 0, 1, &why);
    CHECK(za != nullptr);
    if (za != nullptr) {
        const TexConst s = za->sampledDescriptor();
        CHECK_EQ((s.dword[0] & A6XX_TEX_CONST_0_TILE_MODE__MASK) >> A6XX_TEX_CONST_0_TILE_MODE__SHIFT, uint32_t(TILE6_3));
        CHECK_EQ(za->gpuAddress(), depth->gpuAddress());
    }
}

// One layer of a depth array as 2D: the parent's tiled image a layer on, at its pitch, as a pass with depth_slice draws
// it; a colour format over the depth bytes and a layer past the array are still refused.
// PVRTC, which no A6xx unit reads, decoded by the processor into RGBA8. Blocks whose colour A is opaque red and B
// opaque blue: modulation 0 everywhere is red, 3 blue, 1 three eighths of the way (5-bit 19.375 red, 11.625 blue: bytes
// 159 and 96); the punch-through mode's 2 is half way with alpha 0. Then the order of the blocks: a 16 x 16 texture
// whose blocks each have a colour of their own, read at their centres, where a texel is its own block's colour alone;
// the place of block (bx, by) written out bit by bit -- y's low bit, x's, y's next, x's -- not by the decoder's function.
TEST(pvrtc_decodes_its_two_colours_its_modulation_and_its_block_order) {
    auto blocks = [](uint32_t count, uint32_t mod, uint32_t colour) {
        std::vector<uint8_t> d(8 * count);
        for (uint32_t i = 0; i < count; i++) {
            memcpy(d.data() + 8 * i, &mod, 4);
            memcpy(d.data() + 8 * i + 4, &colour, 4);
        }
        return d;
    };
    const uint32_t redBlue = 0x80000000u | (31u << 16) | 0x8000u | (31u << 10);
    struct Case { uint32_t mod, colour; uint8_t want[4]; };
    const Case cases[] = {{0x00000000u, redBlue, {255, 0, 0, 255}},
                          {0xffffffffu, redBlue, {0, 0, 255, 255}},
                          {0x55555555u, redBlue, {159, 0, 96, 255}},
                          {0xaaaaaaaau, redBlue | 1u, {128, 0, 128, 0}}};
    for (const Case& c : cases) {
        std::vector<uint8_t> data = blocks(4, c.mod, c.colour), rgba(8 * 8 * 4);
        decodePvrtc(data.data(), 8, 8, 4, rgba.data());
        uint32_t right = 0;
        for (uint32_t i = 0; i < 64; i++) right += memcmp(rgba.data() + 4 * i, c.want, 4) == 0;
        CHECK_EQ(right, 64u);
        if (right != 64) std::printf("       mod %#x: first texel %u %u %u %u\n", c.mod, rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    std::vector<uint8_t> data(16 * 8), rgba(16 * 16 * 4);
    for (uint32_t by = 0; by < 4; by++) {
        for (uint32_t bx = 0; bx < 4; bx++) {
            const uint32_t at = (by & 1) | (bx & 1) << 1 | (by >> 1 & 1) << 2 | (bx >> 1 & 1) << 3;
            const uint32_t r = bx * 7 + 3, g = by * 7 + 3;
            const uint32_t colour = 0x80000000u | r << 26 | g << 21 | 0x8000u | r << 10 | g << 5, mod = 0;
            memcpy(data.data() + 8 * at, &mod, 4);
            memcpy(data.data() + 8 * at + 4, &colour, 4);
        }
    }
    decodePvrtc(data.data(), 16, 16, 4, rgba.data());
    uint32_t centres = 0;
    for (uint32_t by = 0; by < 4; by++) {
        for (uint32_t bx = 0; bx < 4; bx++) {
            const uint8_t* t = rgba.data() + ((by * 4 + 2) * 16 + bx * 4 + 2) * 4;
            centres += t[0] == uint8_t(std::lround((bx * 7 + 3) * 255.0 / 31)) && t[1] == uint8_t(std::lround((by * 7 + 3) * 255.0 / 31));
        }
    }
    CHECK_EQ(centres, 16u);
    // through a texture: an RGBA4BPP_sRGB one holds the RGBA8 it decodes to, sRGB in its descriptor, and refuses a part
    Rig r;
    std::string why;
    TextureDesc d;
    d.format = PixelFormat::kPVRTC_RGBA_4BPP_sRGB;
    d.width = 16;
    d.height = 16;
    d.usage = kTextureUsageRead;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    CHECK(t->info().srgb);
    CHECK(t->replace(0, 0, 0, 16, 16, data.data(), 0, &why));
    const uint8_t* stored = static_cast<const uint8_t*>(t->contents()) + t->level(0).offset + 2 * t->level(0).pitch + 2 * 4;
    CHECK(memcmp(stored, rgba.data() + (2 * 16 + 2) * 4, 4) == 0);
    CHECK(!t->replace(0, 0, 0, 8, 8, data.data(), 0, &why));
}

TEST(a_layer_of_a_depth_array_views_as_2d) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc z;
    z.type = TextureType::k2DArray;
    z.format = PixelFormat::kDepth32Float;
    z.width = 96;
    z.height = 40;
    z.depth = 32;
    z.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto arr = Texture::make(r.gpu, z, &why);
    CHECK(arr != nullptr);
    if (arr == nullptr) return;
    for (uint32_t k : {0u, 3u, 31u}) {
        auto v = Texture::view(arr, PixelFormat::kDepth32Float, TextureType::k2D, 0, 1, k, 1, &why);
        CHECK(v != nullptr);
        if (v == nullptr) {
            std::printf("       %s\n", why.c_str());
            continue;
        }
        CHECK_EQ(v->gpuAddress(), arr->gpuAddress() + uint64_t(k) * arr->sliceBytes());
        CHECK_EQ(v->level(0).pitch, arr->level(0).pitch);
        CHECK_EQ(v->depth(), 1u);
        const TexConst s = v->sampledDescriptor(), ps = arr->sampledDescriptor();
        CHECK_EQ((s.dword[0] & A6XX_TEX_CONST_0_TILE_MODE__MASK) >> A6XX_TEX_CONST_0_TILE_MODE__SHIFT, uint32_t(TILE6_3));
        CHECK_EQ(s.dword[2], ps.dword[2]);   // the pitch word, the parent's
    }
    CHECK(Texture::view(arr, PixelFormat::kR32Float, TextureType::k2D, 0, 1, 3, 1, &why) == nullptr);
    CHECK(Texture::view(arr, PixelFormat::kDepth32Float, TextureType::k2D, 0, 1, 32, 1, &why) == nullptr);
}

// A swizzled view reads its components through the parent's: RG11B10Float has no alpha (SWIZ_W is ONE), so a view of
// (alpha, red, zero, one) reads X = ONE, Y = X, Z = ZERO, W = ONE; a view of that view naming its (green, green, red,
// alpha) reads X, X, ONE, ONE. The parent's descriptor is untouched.
TEST(a_swizzled_view_composes_with_what_its_parent_reads) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc d;
    d.format = PixelFormat::kRG11B10Float;
    d.width = d.height = 8;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    auto fields = [](const TexConst& c) {
        std::vector<uint32_t> v;
        for (int i = 0; i < 4; i++) v.push_back((c.dword[0] >> (A6XX_TEX_CONST_0_SWIZ_X__SHIFT + 3 * i)) & 7);
        return v;
    };
    const uint8_t s1[4] = {5, 2, 0, 1};
    auto v1 = Texture::swizzled(t, s1, &why);
    CHECK(v1 != nullptr);
    if (v1 == nullptr) return;
    CHECK(fields(v1->sampledDescriptor()) == (std::vector<uint32_t>{A6XX_TEX_ONE, A6XX_TEX_X, A6XX_TEX_ZERO, A6XX_TEX_ONE}));
    const uint8_t s2[4] = {3, 3, 2, 5};
    auto v2 = Texture::swizzled(v1, s2, &why);
    CHECK(v2 != nullptr);
    if (v2 != nullptr) CHECK(fields(v2->sampledDescriptor()) == (std::vector<uint32_t>{A6XX_TEX_X, A6XX_TEX_X, A6XX_TEX_ONE, A6XX_TEX_ONE}));
    CHECK(fields(t->sampledDescriptor()) == (std::vector<uint32_t>{A6XX_TEX_X, A6XX_TEX_Y, A6XX_TEX_Z, A6XX_TEX_ONE}));
    const uint8_t bad[4] = {6, 2, 3, 4};
    CHECK(Texture::swizzled(t, bad, &why) == nullptr);
}

// A 1D texture as a one-slice 1D array (Resident Evil 4 makes one of an RGBA32Float table): the same memory, TYPE 1D
// with DEPTH 1 in the array's descriptor.
TEST(a_1d_texture_views_as_a_one_slice_1d_array) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc d;
    d.type = TextureType::k1D;
    d.format = PixelFormat::kRGBA32Float;
    d.width = 256;
    d.height = 1;
    auto t = Texture::make(r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    auto a = Texture::view(t, PixelFormat::kRGBA32Float, TextureType::k1DArray, 0, 1, 0, 1, &why);
    CHECK(a != nullptr);
    if (a == nullptr) return;
    CHECK_EQ(a->gpuAddress(), t->gpuAddress());
    const TexConst s = a->sampledDescriptor();
    CHECK_EQ((s.dword[2] & A6XX_TEX_CONST_2_TYPE__MASK) >> A6XX_TEX_CONST_2_TYPE__SHIFT, uint32_t(A6XX_TEX_1D));
    CHECK_EQ((s.dword[5] & A6XX_TEX_CONST_5_DEPTH__MASK) >> A6XX_TEX_CONST_5_DEPTH__SHIFT, 1u);
}

// The stencil plane of a Depth32Float_Stencil8 texture as an X32_Stencil8 view: FMT6_8_UINT at the S8 plane's address
// (past the depth plane), its 128-aligned pitch, tiled, PITCHALIGN 1. Of a Depth32Float texture without one, refused.
TEST(a_stencil_view_reads_the_s8_plane) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc d;
    d.format = PixelFormat::kDepth32FloatStencil8;
    d.width = 100;
    d.height = 40;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto ds = Texture::make(r.gpu, d, &why);
    CHECK(ds != nullptr);
    if (ds == nullptr) return;
    auto s = Texture::view(ds, PixelFormat::kX32Stencil8, TextureType::k2D, 0, 1, 0, 1, &why);
    CHECK(s != nullptr);
    if (s == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK_EQ(s->gpuAddress(), ds->gpuAddress() + ds->stencilOffset());
    const TexConst c = s->sampledDescriptor();
    CHECK_EQ((c.dword[0] & A6XX_TEX_CONST_0_FMT__MASK) >> A6XX_TEX_CONST_0_FMT__SHIFT, uint32_t(FMT6_8_UINT));
    CHECK_EQ((c.dword[0] & A6XX_TEX_CONST_0_TILE_MODE__MASK) >> A6XX_TEX_CONST_0_TILE_MODE__SHIFT, uint32_t(TILE6_3));
    CHECK_EQ(c.dword[2] & A6XX_TEX_CONST_2_PITCH__MASK, A6XX_TEX_CONST_2_PITCH(128));
    CHECK_EQ((c.dword[2] & A6XX_TEX_CONST_2_PITCHALIGN__MASK) >> A6XX_TEX_CONST_2_PITCHALIGN__SHIFT, 1u);
    TextureDesc z = d;
    z.format = PixelFormat::kDepth32Float;
    auto depthOnly = Texture::make(r.gpu, z, &why);
    CHECK(depthOnly != nullptr && Texture::view(depthOnly, PixelFormat::kX32Stencil8, TextureType::k2D, 0, 1, 0, 1, &why) == nullptr);
}
