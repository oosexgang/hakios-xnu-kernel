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
    // a format outside the eleven is nobody's
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
    // the refusals, each on its sentence
    TextureDesc d;
    d.type = TextureType::k2DArray;
    CHECK(has(refusedMake(r, d), "texture type 3 (2DArray): only 2D (2) is served yet"));
    d.type = TextureType::k3D;
    CHECK(has(refusedMake(r, d), "texture type 7 (3D): only 2D"));
    d = TextureDesc{};
    d.format = PixelFormat(13);
    CHECK(has(refusedMake(r, d), "pixel format 13: not one of the eleven served"));
    d = TextureDesc{};
    d.width = 0;
    CHECK(has(refusedMake(r, d), "a texture of 0 x 1: each side is 1 to 32767"));
    d.width = 32768;
    CHECK(has(refusedMake(r, d), "a texture of 32768 x 1: each side is 1 to 32767"));
    d.width = 32767;
    d.height = 0;
    CHECK(has(refusedMake(r, d), "32767 x 0: each side"));
    d = TextureDesc{};
    d.usage = 4;   // MTLTextureUsageRenderTarget
    CHECK(has(refusedMake(r, d), "texture usage 4 has bits other than shader read (1) and shader write (2)"));
    d.usage = 0x13;
    CHECK(has(refusedMake(r, d), "texture usage 19 has bits other than"));
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
    d.minFilter = 2;
    CHECK(has(refusedSampler(d), "sampler filters min 2 mag 0: MTLSamplerMinMagFilter is Nearest (0) or Linear (1)"));
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
