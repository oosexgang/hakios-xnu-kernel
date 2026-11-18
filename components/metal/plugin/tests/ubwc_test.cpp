// Compressed render targets (TextureDesc::compressed with render-target usage: UBWC, Mesa's fdl6 layout) against the
// fake GPU, which keeps a UBWC image's pixels masked by their place and holds every register naming the image to the
// flag buffer its first writer named (fake_gpu.hpp says how). What is shown here: the 2D engine's copies in and out,
// a GMEM pass's loads and stores, a Clear in sysmem and in the tiles, draws into one and samples and framebuffer
// fetches of one, the flag buffer the driver clears at creation, and what such a texture refuses. Each against its
// linear twin's pixels, or a sentence. Without Mesa a compressed texture is refused at creation, and these say so.
//
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_fb_fs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_flat_vs_air.hpp"
#include "fixtures/tri_mrt_fs_air.hpp"
#include "fixtures/tri_tex_fs_air.hpp"

#include "test.hpp"

using namespace hakios::metal;
namespace kernels = hakios::metal::kernels;

namespace {
struct Rig {
    Rig() {
        auto f = std::make_unique<FakeGpu>();
        fake = f.get();
        gpu = std::make_unique<Gpu>(std::move(f));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
    FakeGpu* fake;
    std::unique_ptr<Gpu> gpu;
    std::shared_ptr<CommandQueue> queue;
};

bool noMesa(const std::string& why) { return why.find("this build has none") != std::string::npos; }

// a compressed render target, or nullptr in a build without Mesa (said once per test)
std::shared_ptr<Texture> compressed(Rig& r, uint32_t w, uint32_t h, PixelFormat f, std::string* why) {
    TextureDesc d;
    d.format = f;
    d.width = w;
    d.height = h;
    d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    d.compressed = true;
    return Texture::make(*r.gpu, d, why);
}

std::shared_ptr<Texture> linear(Rig& r, uint32_t w, uint32_t h, PixelFormat f, uint32_t usage = kTextureUsageRenderTarget | kTextureUsageRead) {
    TextureDesc d;
    d.format = f;
    d.width = w;
    d.height = h;
    d.usage = usage;
    std::string why;
    auto t = Texture::make(*r.gpu, d, &why);
    CHECK(t != nullptr);
    return t;
}

// every pixel its own word: (y << 16 | x) ^ SALT, so that a pixel moved, swapped or masked shows
void fillCoords(Texture& t, uint32_t salt = 0) {
    std::vector<uint32_t> rows(uint64_t(t.width()) * t.height());
    for (uint32_t y = 0; y < t.height(); y++) {
        for (uint32_t x = 0; x < t.width(); x++) rows[uint64_t(y) * t.width() + x] = (y << 16 | x) ^ salt;
    }
    std::string why;
    CHECK(t.replace(0, 0, t.width(), t.height(), rows.data(), uint64_t(t.width()) * 4, &why));
}
uint32_t coordsWrong(const Texture& t, uint32_t salt = 0) {
    std::vector<uint32_t> rows(uint64_t(t.width()) * t.height());
    std::string why;
    if (!t.read(0, 0, t.width(), t.height(), rows.data(), uint64_t(t.width()) * 4, &why)) return ~0u;
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < t.height(); y++) {
        for (uint32_t x = 0; x < t.width(); x++) wrong += rows[uint64_t(y) * t.width() + x] != ((y << 16 | x) ^ salt);
    }
    return wrong;
}

bool run(CommandBuffer& cb) {
    cb.commit();
    cb.waitUntilCompleted();
    return cb.status() == CommandBuffer::kCompleted;
}

render::BlitRect whole(const Texture& t) { return render::BlitRect{0, 0, t.width(), t.height()}; }

Program flatProgram() { return Program{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"}; }
Program texProgram() { return Program{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"}; }
Program fbProgram() { return Program{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"}; }

Draw wholeDraw(uint32_t vertexCount, uint32_t w, uint32_t h) {
    Draw d;
    d.vertexCount = vertexCount;
    d.viewport = Viewport{0, 0, double(w), double(h), 0, 1};
    return d;
}

void lastFault(Rig& r) {
    if (r.fake->stats().faults != 0) std::printf("       fault: %s\n", r.fake->stats().lastFault.c_str());
}
}  // namespace

// The 2D engine's copy into a compressed target and back out of it gives the rows back, RGBA8 and BGRA8 (whose linear
// rows are WXYZ and whose tiles are WZYX: the engine reorders both ways), at sizes that are and are not whole UBWC
// blocks; and what the compressed texture's memory holds is not those rows (the model masks it, as compression would
// change it) -- so the rows surviving is the two copies' doing
TEST(a_compressed_target_gives_back_through_copies_what_was_copied_in) {
    for (const PixelFormat f : {PixelFormat::kRGBA8Unorm, PixelFormat::kBGRA8Unorm}) {
        for (const auto& [w, h] : {std::pair<uint32_t, uint32_t>{64, 64}, {301, 70}}) {
            Rig r;
            std::string why;
            auto c = compressed(r, w, h, f, &why);
            if (c == nullptr && noMesa(why)) {
                std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
                return;
            }
            CHECK(c != nullptr);
            if (c == nullptr) return;
            CHECK(c->compressed() && c->contents() == nullptr && c->staging() == nullptr);
            auto a = linear(r, w, h, f), b = linear(r, w, h, f);
            fillCoords(*a);
            auto cb = r.queue->commandBuffer();
            CHECK(cb->blit(*a, 0, whole(*a), *c, 0, whole(*c), false));
            CHECK(cb->blit(*c, 0, whole(*c), *b, 0, whole(*b), false));
            CHECK(run(*cb));
            CHECK_EQ(r.fake->stats().faults, 0u);
            lastFault(r);
            CHECK_EQ(coordsWrong(*b), 0u);
            CHECK(r.fake->stats().ubwcCopies == 1 && r.fake->stats().ubwcReads == 1);
            // (the first row of the compressed texture's memory is not the first row of what went in)
            const std::vector<uint8_t> raw = r.fake->memory(c->gpuAddress(), uint64_t(w) * 4);
            std::vector<uint32_t> first(w);
            std::string rwhy;
            CHECK(a->read(0, 0, w, 1, first.data(), uint64_t(w) * 4, &rwhy));
            CHECK(raw.size() == uint64_t(w) * 4 && std::memcmp(raw.data(), first.data(), raw.size()) != 0);
        }
    }
}

// A GMEM pass over a compressed attachment loads each tile from the UBWC image and stores it back (the event blit told
// the layout and the flag buffer): what was copied in comes out after two draws the fake does not rasterize -- binned
// too (the binning pass first, each tile given its streams), as the phone runs CoreAnimation's passes. Its twin, the
// same pass without its loads (gmemLoad off), stores what GMEM held instead, and every pixel is wrong
TEST(a_gmem_pass_loads_and_stores_a_compressed_attachment) {
    for (const PixelFormat f : {PixelFormat::kRGBA8Unorm, PixelFormat::kBGRA8Unorm}) {
        for (const auto& [w, h, binned] : {std::tuple<uint32_t, uint32_t, bool>{301, 70, false}, {832, 1792, false}, {832, 1792, true}}) {
            for (int loads = 1; loads >= 0; loads--) {
                Rig r;
                std::string why;
                auto c = compressed(r, w, h, f, &why);
                if (c == nullptr && noMesa(why)) {
                    std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
                    return;
                }
                CHECK(c != nullptr);
                if (c == nullptr) return;
                auto a = linear(r, w, h, f), b = linear(r, w, h, f);
                fillCoords(*a);
                RenderPass pass;
                pass.color0 = c.get();
                render::GmemPlan plan;
                CHECK(render::gmemPlan(pass, &plan, &why));
                render::Options o;
                o.gmemLoad = loads != 0;
                o.gmemBinning = binned;
                auto cb = r.queue->commandBuffer();
                CHECK(cb->blit(*a, 0, whole(*a), *c, 0, whole(*c), false));
                CHECK(cb->draw(flatProgram(), pass, wholeDraw(3, w, h), Bindings{}, Bindings{}, o));
                CHECK(cb->draw(flatProgram(), pass, wholeDraw(3, w, h), Bindings{}, Bindings{}, o));
                CHECK(cb->blit(*c, 0, whole(*c), *b, 0, whole(*b), false));
                CHECK(run(*cb));
                const auto s = r.fake->stats();
                CHECK_EQ(s.faults, 0u);
                lastFault(r);
                CHECK_EQ(cb->counts().gmemPasses, 1u);
                CHECK_EQ(cb->counts().binnedPasses, binned ? 1u : 0u);
                CHECK_EQ(s.ubwcLoads, loads ? plan.tiles() : 0u);
                CHECK_EQ(s.ubwcStores, plan.tiles());
                CHECK_EQ(s.ubwcDraws, 2 * plan.tiles());
                CHECK_EQ(coordsWrong(*b), loads ? 0u : w * h);
            }
        }
    }
}

// A Clear of a compressed target: in sysmem the 2D engine's fill through the flag buffer, in the tiles the event blit's
// clear and the store; either way the pixels copied out are the linear twin's, byte for byte, the draw over a corner
// notwithstanding
TEST(a_clear_of_a_compressed_target_comes_out_as_a_linear_ones) {
    for (const PixelFormat f : {PixelFormat::kRGBA8Unorm, PixelFormat::kBGRA8Unorm}) {
        for (int gm = 0; gm < 2; gm++) {
            std::vector<uint32_t> got[2];
            for (int comp = 0; comp < 2; comp++) {
                Rig r;
                std::string why;
                std::shared_ptr<Texture> t = comp ? compressed(r, 100, 37, f, &why) : linear(r, 100, 37, f);
                if (t == nullptr && noMesa(why)) {
                    std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
                    return;
                }
                CHECK(t != nullptr);
                if (t == nullptr) return;
                auto b = linear(r, 100, 37, f);
                fillCoords(*b, 0x5a5a5a5a);
                RenderPass pass;
                pass.color0 = t.get();
                pass.load = LoadAction::kClear;
                const double colour[4] = {0.25, 0.5, 0.75, 0.6};
                for (int k = 0; k < 4; k++) pass.clearColor[k] = colour[k];
                render::Options o;
                o.gmem = gm != 0;
                Draw d = wholeDraw(3, 100, 37);
                d.viewport = Viewport{40, 10, 30, 12, 0, 1};
                auto cb = r.queue->commandBuffer();
                CHECK(cb->draw(flatProgram(), pass, d, Bindings{}, Bindings{}, o));
                CHECK(cb->blit(*t, 0, whole(*t), *b, 0, whole(*b), false));
                CHECK(run(*cb));
                CHECK_EQ(r.fake->stats().faults, 0u);
                lastFault(r);
                if (comp) {
                    CHECK_EQ(r.fake->stats().ubwcFills, gm ? 0u : 1u);
                    CHECK(gm ? r.fake->stats().tileClears != 0 && r.fake->stats().ubwcStores != 0 : r.fake->stats().tiles == 0);
                }
                got[comp].resize(100 * 37);
                CHECK(b->read(0, 0, 100, 37, got[comp].data(), 400, &why));
            }
            CHECK(got[0] == got[1]);
            CHECK(got[0][0] != ((0u << 16 | 0u) ^ 0x5a5a5a5au));   // (cleared, not what the copy's destination held)
        }
    }
}

// Draws into a compressed target in sysmem and in the tiles (RB_MRT TILE6_3 and WZYX whatever the format's own swap,
// its flag buffer on in FLAG_MRTS), a draw that samples it after, and a draw that reads it through framebuffer fetch
// in sysmem (the attachment's UBWC view: linear rows over it would read noise, and the fake faults on them) and in the
// tiles (its bin in GMEM, as for any attachment)
TEST(a_compressed_target_is_drawn_into_sampled_and_fetched) {
    using namespace a6xx;
    for (const PixelFormat f : {PixelFormat::kRGBA8Unorm, PixelFormat::kBGRA8Unorm}) {
        for (int gm = 0; gm < 2; gm++) {
            Rig r;
            std::string why;
            auto c = compressed(r, 128, 96, f, &why);
            if (c == nullptr && noMesa(why)) {
                std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
                return;
            }
            CHECK(c != nullptr);
            if (c == nullptr) return;
            auto out = linear(r, 128, 96, PixelFormat::kRGBA8Unorm);
            render::Options o;
            o.gmem = gm != 0;
            RenderPass pass;
            pass.color0 = c.get();
            pass.load = LoadAction::kClear;
            {
                // (a Clear and a draw, then a draw that loads, which joins the pass: a second draw asking for a Clear
                // would open a pass of its own)
                RenderPass loads = pass;
                loads.load = LoadAction::kLoad;
                auto cb = r.queue->commandBuffer();
                CHECK(cb->draw(flatProgram(), pass, wholeDraw(3, 128, 96), Bindings{}, Bindings{}, o));
                CHECK(cb->draw(flatProgram(), loads, wholeDraw(3, 128, 96), Bindings{}, Bindings{}, o));
                CHECK(run(*cb));
                CHECK_EQ(cb->counts().gmemPasses, gm ? 1u : 0u);
            }
            const auto drawn = r.fake->stats().lastDraw;
            CHECK_EQ(drawn.mrtTileMode, uint32_t(TILE6_3));
            CHECK_EQ(drawn.mrtSwap, uint32_t(WZYX));
            CHECK(r.fake->stats().ubwcDraws != 0);
            {
                Sampler smp;
                CHECK(makeSampler(SamplerDesc{}, &smp, &why));
                Bindings fsb;
                fsb.textures = {c.get()};
                fsb.samplers = {&smp};
                RenderPass into;
                into.color0 = out.get();
                render::Options sys;
                sys.gmem = false;   // (one draw, one sample of it read: a tile would read it once a tile)
                auto cb = r.queue->commandBuffer();
                CHECK(cb->draw(texProgram(), into, wholeDraw(6, 128, 96), Bindings{}, fsb, sys));
                CHECK(run(*cb));
                CHECK_EQ(r.fake->stats().ubwcSamples, 1u);
            }
            {
                RenderPass again;
                again.color0 = c.get();
                const uint32_t samples = r.fake->stats().ubwcSamples;
                auto cb = r.queue->commandBuffer();
                CHECK(cb->draw(fbProgram(), again, wholeDraw(6, 128, 96), Bindings{}, Bindings{}, o));
                CHECK(cb->draw(fbProgram(), again, wholeDraw(6, 128, 96), Bindings{}, Bindings{}, o));
                CHECK(run(*cb));
                CHECK_EQ(cb->counts().gmemPasses, gm ? 1u : 0u);
                // (in sysmem each fetch reads the UBWC view; in a tile the bin, which is no texture of memory)
                CHECK_EQ(r.fake->stats().ubwcSamples - samples, gm ? 0u : 2u);
            }
            CHECK_EQ(r.fake->stats().faults, 0u);
            lastFault(r);
        }
    }
}

// A pass of a linear attachment and a compressed one (the screen beside an offscreen target), the FS writing both: each
// slot's own words -- FLAG_MRTS for the compressed one's bit alone, the linear one's load and store linear -- in the
// tiles and in sysmem; what both held comes back where the draws (which the fake does not rasterize) leave it
TEST(a_pass_draws_into_a_linear_and_a_compressed_attachment_together) {
    using namespace a6xx;
    const Program mrt{kernels::tri_flat_vs_air, kernels::tri_mrt_fs_air, Linkage{}, "tri_mrt"};
    for (int gm = 0; gm < 2; gm++) {
        for (int first = 0; first < 2; first++) {
            Rig r;
            std::string why;
            auto c = compressed(r, 320, 200, PixelFormat::kBGRA8Unorm, &why);
            if (c == nullptr && noMesa(why)) {
                std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
                return;
            }
            CHECK(c != nullptr);
            if (c == nullptr) return;
            auto l = linear(r, 320, 200, PixelFormat::kBGRA8Unorm), a = linear(r, 320, 200, PixelFormat::kBGRA8Unorm),
                 b = linear(r, 320, 200, PixelFormat::kBGRA8Unorm);
            fillCoords(*l, 0x80000000u);
            fillCoords(*a);
            RenderPass pass;
            pass.color0 = first ? c.get() : l.get();
            pass.more[0].texture = first ? l.get() : c.get();
            pass.more[0].load = LoadAction::kLoad;
            render::Options o;
            o.gmem = gm != 0;
            auto cb = r.queue->commandBuffer();
            CHECK(cb->blit(*a, 0, whole(*a), *c, 0, whole(*c), false));
            CHECK(cb->draw(mrt, pass, wholeDraw(3, 320, 200), Bindings{}, Bindings{}, o));
            CHECK(cb->draw(mrt, pass, wholeDraw(3, 320, 200), Bindings{}, Bindings{}, o));
            CHECK(cb->blit(*c, 0, whole(*c), *b, 0, whole(*b), false));
            CHECK(run(*cb));
            CHECK_EQ(r.fake->stats().faults, 0u);
            lastFault(r);
            CHECK_EQ(cb->counts().gmemPasses, gm ? 1u : 0u);
            const auto d = r.fake->stats().lastDraw;
            CHECK(d.mrtCount == 2 && d.mrtBases[first ? 0 : 1] == c->gpuAddress() && d.mrtBases[first ? 1 : 0] == l->gpuAddress());
            CHECK_EQ(coordsWrong(*b), 0u);
            CHECK_EQ(coordsWrong(*l, 0x80000000u), 0u);
        }
    }
}

// A multisample attachment resolved into a compressed texture (the 2D engine's average, written through the flag
// buffer) comes out as its resolve into linear rows does: a Clear of every sample, then four different samples in one
// pixel, whose mean only the resolve makes
TEST(a_multisample_attachment_resolves_into_a_compressed_texture) {
    Rig r;
    std::string why;
    auto c = compressed(r, 64, 64, PixelFormat::kRGBA8Unorm, &why);
    if (c == nullptr && noMesa(why)) {
        std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
        return;
    }
    CHECK(c != nullptr);
    if (c == nullptr) return;
    TextureDesc md;
    md.type = TextureType::k2DMultisample;
    md.samples = 4;
    md.width = md.height = 64;
    md.usage = kTextureUsageRenderTarget;
    auto ms = Texture::make(*r.gpu, md, &why);
    CHECK(ms != nullptr);
    if (ms == nullptr) return;
    auto direct = linear(r, 64, 64, PixelFormat::kRGBA8Unorm), out = linear(r, 64, 64, PixelFormat::kRGBA8Unorm);
    RenderPass pass;
    pass.color0 = ms.get();
    pass.load = LoadAction::kClear;
    const double colour[4] = {0.25, 0.5, 0.75, 1.0};
    for (int i = 0; i < 4; i++) pass.clearColor[i] = colour[i];
    {
        auto cb = r.queue->commandBuffer();
        CHECK(cb->clear(pass));
        CHECK(run(*cb));
    }
    auto* raw = static_cast<uint8_t*>(ms->buffer()->contents()) + 5 * ms->bytesPerRow() + 3 * 16;
    const uint8_t samples[4][4] = {{0, 0, 0, 0}, {100, 0, 0, 255}, {200, 4, 0, 255}, {101, 0, 3, 255}};
    memcpy(raw, samples, 16);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->blit(*ms, 0, whole(*ms), *direct, 0, whole(*direct), false));
    CHECK(cb->blit(*ms, 0, whole(*ms), *c, 0, whole(*c), false));
    CHECK(cb->blit(*c, 0, whole(*c), *out, 0, whole(*out), false));
    CHECK(run(*cb));
    CHECK_EQ(r.fake->stats().faults, 0u);
    lastFault(r);
    std::vector<uint32_t> a(64 * 64), b(64 * 64);
    CHECK(direct->read(0, 0, 64, 64, a.data(), 256, &why) && out->read(0, 0, 64, 64, b.data(), 256, &why));
    CHECK(a == b);
    const uint8_t want[4] = {100, 1, 1, 191};   // (as render_test's resolve: the mean of the four, rounded)
    CHECK(std::memcmp(&b[5 * 64 + 3], want, 4) == 0);
    CHECK(r.fake->stats().ubwcCopies == 1 && r.fake->stats().ubwcReads == 1);
}

// The flag buffer is cleared when the texture is made: over memory that held something else (the fake's next buffer
// dirtied, as a kernel's recycled one may be), the flags read zero and the data does not, and the first copy into it
// finds the flag buffer as freedreno leaves one (the fake faults on any other)
TEST(a_compressed_target_is_made_with_its_flag_buffer_cleared) {
    Rig r;
    r.fake->dirtyNextBuffers(1);
    std::string why;
    auto c = compressed(r, 301, 70, PixelFormat::kBGRA8Unorm, &why);
    if (c == nullptr && noMesa(why)) {
        std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
        return;
    }
    CHECK(c != nullptr);
    if (c == nullptr) return;
    const uint64_t start = c->buffer()->gpuAddress(), data = c->gpuAddress();
    CHECK(c->flagAddress() == start && data > start);
    const std::vector<uint8_t> flags = r.fake->memory(start, data - start), rows = r.fake->memory(data, 64);
    bool zero = !flags.empty(), dirty = !rows.empty();
    for (uint8_t b : flags) zero = zero && b == 0;
    for (uint8_t b : rows) dirty = dirty && b == 0xcd;
    CHECK(zero && dirty);
    auto a = linear(r, 301, 70, PixelFormat::kBGRA8Unorm), b = linear(r, 301, 70, PixelFormat::kBGRA8Unorm);
    fillCoords(*a);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->blit(*a, 0, whole(*a), *c, 0, whole(*c), false));
    CHECK(cb->blit(*c, 0, whole(*c), *b, 0, whole(*b), false));
    CHECK(run(*cb));
    CHECK_EQ(r.fake->stats().faults, 0u);
    lastFault(r);
    CHECK_EQ(coordsWrong(*b), 0u);
}

// What a compressed render target is not: rows for this processor (replace, read and contents refuse, by name), a
// view in another format or of part of it, a texture a kernel stores into, the source or the destination of a scaled,
// filtered or offset-level copy. Its whole self as a view, and a swizzled view, stay compressed
TEST(a_compressed_target_refuses_what_its_layout_cannot_serve) {
    Rig r;
    std::string why;
    auto c = compressed(r, 128, 64, PixelFormat::kRGBA8Unorm, &why);
    if (c == nullptr && noMesa(why)) {
        std::printf("       (no Mesa in this build: %s; not run)\n", why.c_str());
        return;
    }
    CHECK(c != nullptr);
    if (c == nullptr) return;
    std::vector<uint32_t> rows(128 * 64);
    CHECK(!c->replace(0, 0, 128, 64, rows.data(), 512, &why) && why.find("a compressed render target") != std::string::npos);
    why.clear();
    CHECK(!c->read(0, 0, 128, 64, rows.data(), 512, &why) && why.find("a compressed render target") != std::string::npos);
    CHECK(c->contents() == nullptr);
    auto same = Texture::view(c, PixelFormat::kRGBA8Unorm, TextureType::k2D, 0, 1, 0, 1, &why);
    CHECK(same != nullptr && same->compressed() && same->flagAddress() == c->flagAddress() && same->gpuAddress() == c->gpuAddress());
    why.clear();
    CHECK(Texture::view(c, PixelFormat::kBGRA8Unorm, TextureType::k2D, 0, 1, 0, 1, &why) == nullptr && why.find("compressed") != std::string::npos);
    // (its sRGB pair: the same tiles and flags, read with the sRGB conversion)
    auto srgb = Texture::view(c, PixelFormat::kRGBA8UnormSrgb, TextureType::k2D, 0, 1, 0, 1, &why);
    CHECK(srgb != nullptr && srgb->compressed() && srgb->info().srgb);
    if (srgb != nullptr) {
        const a6xx::TexConst d = srgb->sampledDescriptor(), e = c->sampledDescriptor();
        CHECK(d.dword[0] == (e.dword[0] | a6xx::A6XX_TEX_CONST_0_SRGB) && d.dword[3] == e.dword[3] && d.dword[7] == e.dword[7] && d.dword[10] == e.dword[10]);
    }
    const uint8_t swz[4] = {4, 3, 2, 5};
    auto swizzled = Texture::swizzled(c, swz, &why);
    CHECK(swizzled != nullptr && swizzled->compressed());
    if (swizzled != nullptr) {
        // (the descriptor of a swizzled view reads the same UBWC image: tile mode, flag bit and flag buffer)
        const a6xx::TexConst d = swizzled->sampledDescriptor(), e = c->sampledDescriptor();
        CHECK(d.dword[3] == e.dword[3] && d.dword[7] == e.dword[7] && d.dword[10] == e.dword[10] && d.dword[0] != e.dword[0]);
    }
    {
        TextureDesc d;
        d.format = PixelFormat::kRGBA8Unorm;
        d.width = 128;
        d.height = 64;
        d.usage = kTextureUsageRenderTarget | kTextureUsageRead | kTextureUsageWrite;
        d.compressed = true;
        why.clear();
        CHECK(Texture::make(*r.gpu, d, &why) == nullptr && why.find("shader-write") != std::string::npos);
    }
    auto l = linear(r, 128, 64, PixelFormat::kRGBA8Unorm);
    auto half = linear(r, 64, 32, PixelFormat::kRGBA8Unorm);
    for (const auto& [from, to, linearFilter] :
         {std::tuple<const Texture*, const Texture*, bool>{c.get(), half.get(), false}, {half.get(), c.get(), false}, {c.get(), l.get(), true}}) {
        auto cb = r.queue->commandBuffer();
        const render::BlitRect s{0, 0, from->width(), from->height()}, d{0, 0, to->width(), to->height()};
        CHECK(!cb->blit(*from, 0, s, *to, 0, d, linearFilter));
        CHECK(cb->error().find("compressed texture") != std::string::npos);
    }
    CHECK_EQ(r.fake->stats().faults, 0u);
}
