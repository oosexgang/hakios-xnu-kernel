// The render path against the fake GPU: render::encode's refusals, each on its sentence with nothing reaching the GPU;
// the words it writes for the first draw's case (a 64x64 RGBA8Unorm target, tri_flat and tri_color), each held to the
// literal worked out by hand in a6xx-draw.md and tests/a6xx_test.cpp; the Options switches, each shown to remove or
// change exactly its registers; then draws that complete on the fake, whose checks are read back through lastDraw, and
// for every one of the fake's checks a mutation twin it refuses, by its sentence (compute_test's Encoded/Raw shape).
// The fake rasterizes nothing: coverage, the edge rule, the y orientation and the bytes are the phone's
// (hakios-metal-probe stage 9).
//
// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"
#include "fixtures/end_only.hpp"
#include "fixtures/tri_attr_vs_air.hpp"
#include "fixtures/tri_cflat_fs_air.hpp"
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_flat_vs_air.hpp"
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
    std::unique_ptr<CommandQueue> queue;
};

std::shared_ptr<Texture> target(Rig& r, uint32_t w = 64, uint32_t h = 64, uint32_t usage = kTextureUsageRenderTarget | kTextureUsageRead,
                                PixelFormat format = PixelFormat::kRGBA8Unorm) {
    TextureDesc d;
    d.format = format;
    d.width = w;
    d.height = h;
    d.usage = usage;
    std::string why;
    auto t = Texture::make(*r.gpu, d, &why);
    CHECK(t != nullptr);
    return t;
}

constexpr uint32_t regid(uint32_t n, uint32_t c) { return n * 4 + c; }
constexpr uint8_t kSlotPos = 0, kSlotVar0 = 32, kSlotVertexId = 9, kSlotBaryPerspPixel = 56, kFragData0 = 4;

// A stage with no register read: every regid field kRegidNone, nothing else set. A zero-initialized Stage would name
// r0.x everywhere (regid 0 is a real register), which is why the fixture writer and this helper spell 0xfc out.
Stage emptyStage(uint8_t kind, const Kernel& k) {
    Stage s{};
    s.k = k;
    s.kind = kind;
    s.vertexIdRegid = s.instanceIdRegid = s.positionRegid = s.pointSizeRegid = a6xx::kRegidNone;
    s.fragCoordRegid = s.faceRegid = s.sampleIdRegid = s.sampleMaskInRegid = a6xx::kRegidNone;
    for (uint32_t& ij : s.ijRegid) ij = a6xx::kRegidNone;
    for (uint32_t& c : s.colorRegid) c = a6xx::kRegidNone;
    s.depthRegid = s.sampleMaskRegid = s.stencilRefRegid = a6xx::kRegidNone;
    return s;
}

// The first draw's pair, built by hand from the ir3 listings in air-vs-fs.md (the shape, not the fixtures' words: the
// program is end_only's 32 dwords, which the fake does not run): a VS reading [[vertex_id]] in r0.x and writing its
// position in r0.y (constlen 8, ir3's floor for a VS), an FS writing [[color(0)]] from r0.x with no varying (constlen 0,
// an unused perspective-pixel barycentric entry at r63.x as ir3 leaves it).
Stage handVs() {
    Kernel k = kernels::end_only;
    k.name = "hand_vs";
    k.constlen = 8;
    k.maxReg = 1;
    k.pvtmemPerWave = false;
    k.localSize[0] = k.localSize[1] = k.localSize[2] = 0;
    Stage s = emptyStage(kStageVertex, k);
    s.inputs[0] = StageInput{kSlotVertexId, uint8_t(regid(0, 0)), 0x1, 0, 1, 0, 0, 0};
    s.numInputs = 1;
    s.outputs[0] = StageOutput{kSlotPos, uint8_t(regid(0, 1)), 0, 0};
    s.numOutputs = 1;
    s.vertexIdRegid = regid(0, 0);
    s.positionRegid = regid(0, 1);
    s.writesPos = 1;
    return s;
}
Stage handFs() {
    Kernel k = kernels::end_only;
    k.name = "hand_fs";
    k.maxReg = 0;
    k.pvtmemPerWave = false;
    k.localSize[0] = k.localSize[1] = k.localSize[2] = 0;
    Stage s = emptyStage(kStageFragment, k);
    s.inputs[0] = StageInput{kSlotBaryPerspPixel, a6xx::kRegidNone, 0x3, 0, 1, 0, 0, 0};
    s.numInputs = 1;
    s.outputs[0] = StageOutput{kFragData0, uint8_t(regid(0, 0)), 0, kElemFloat};
    s.numOutputs = 1;
    s.colorRegid[0] = regid(0, 0);
    s.numColorOutputs = 1;
    return s;
}
Program handProgram() { return Program{handVs(), handFs(), Linkage{}, "hand_vs+hand_fs"}; }
Program flatProgram() { return Program{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"}; }
Program colorProgram() { return Program{kernels::tri_color_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_color"}; }

Draw wholeDraw(uint32_t vertexCount = 3, uint32_t w = 64, uint32_t h = 64) {
    Draw d;
    d.vertexCount = vertexCount;
    d.viewport = Viewport{0, 0, double(w), double(h), 0, 1};
    return d;
}

// a stream built by hand or mutated, run through the raw path: what the fake says about it
struct Raw {
    explicit Raw(Rig& r) : rig(r) {
        int e = 0;
        bo = r.gpu->newBo(65536, 0, &e);
        CHECK(bo != nullptr);
    }
    int submit(const std::vector<uint32_t>& words) {
        CHECK(4 * words.size() <= bo->size());
        memcpy(bo->cpu(), words.data(), 4 * words.size());
        uint32_t fence = 0;
        int e = rig.gpu->submit(0, *bo, 0, uint32_t(words.size()), {}, &fence);
        if (e != 0) return e;
        return rig.gpu->wait(0, fence, 1000000000);
    }
    Rig& rig;
    std::unique_ptr<Bo> bo;
};

// the draw's own words, as render::encode writes them over buffers of this rig, for a test to read or mutate
struct Encoded {
    Encoded(Rig& r, const Program& p, const RenderPass& pass, const Draw& d, const render::Options& o = render::Options{}, const Bindings& vsb = Bindings{},
            const Bindings& fsb = Bindings{}) {
        std::string why;
        vsProgram = newBuffer(*r.gpu, 4ull * p.vs.k.numWords, &why);
        fsProgram = newBuffer(*r.gpu, 4ull * p.fs.k.numWords, &why);
        table = newBuffer(*r.gpu, render::kTableBytes, &why);
        CHECK(vsProgram != nullptr && fsProgram != nullptr && table != nullptr);
        memcpy(vsProgram->contents(), p.vs.k.words, 4ull * p.vs.k.numWords);
        memcpy(fsProgram->contents(), p.fs.k.words, 4ull * p.fs.k.numWords);
        memset(table->contents(), 0xee, render::kTableBytes);   // the encoder must zero what the GPU reads of it
        ok = render::encode(stream, p, pass, d, vsb, fsb, vsProgram->gpuAddress(), fsProgram->gpuAddress(), table->gpuAddress(),
                            static_cast<uint8_t*>(table->contents()), &why, o);
        error = why;
    }
    // the index of the first type-7 header of OPCODE, or -1
    size_t find(uint32_t opcode, size_t from = 0) const {
        const auto& w = stream.words();
        for (size_t i = from; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kCommand && h.opcode == opcode) return i;
            i += 1 + h.count;
        }
        return size_t(-1);
    }
    // the index of the VALUE word of the LAST type-4 write that covers register REG, or -1 (the one the GPU keeps: the
    // batch start zeroes the 32 fetch sizes before the draw writes the ones it uses)
    size_t findLastReg(uint32_t reg) const {
        const auto& w = stream.words();
        size_t found = size_t(-1);
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kRegisters && reg >= h.reg && reg < h.reg + h.count) found = i + 1 + (reg - h.reg);
            i += 1 + h.count;
        }
        return found;
    }
    // the index of the VALUE word of the first type-4 write that covers register REG, or -1
    size_t findReg(uint32_t reg) const {
        const auto& w = stream.words();
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kRegisters && reg >= h.reg && reg < h.reg + h.count) return i + 1 + (reg - h.reg);
            i += 1 + h.count;
        }
        return size_t(-1);
    }
    // the last value written to REG (the registers hold the last write), or NONE when it is never written
    static constexpr uint32_t kNone = 0xdeadbeef;
    uint32_t reg(uint32_t r) const {
        const auto& w = stream.words();
        uint32_t v = kNone;
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kRegisters && r >= h.reg && r < h.reg + h.count) v = w[i + 1 + (r - h.reg)];
            i += 1 + h.count;
        }
        return v;
    }
    // how many type-4 writes cover REG
    uint32_t writes(uint32_t r) const {
        const auto& w = stream.words();
        uint32_t count = 0;
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kRegisters && r >= h.reg && r < h.reg + h.count) count++;
            i += 1 + h.count;
        }
        return count;
    }
    // the first CP_EVENT_WRITE of EVENT: its header index, or -1
    size_t findEvent(uint32_t event) const {
        const auto& w = stream.words();
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kCommand && h.opcode == a6xx::CP_EVENT_WRITE && h.count >= 1 &&
                (w[i + 1] & a6xx::CP_EVENT_WRITE_0_EVENT__MASK) == event) {
                return i;
            }
            i += 1 + h.count;
        }
        return size_t(-1);
    }
    // the first CP_LOAD_STATE6* header of OPCODE whose word 0 names TYPE into BLOCK, or -1
    size_t findLoad(uint32_t opcode, a6xx::a6xx_state_type type, a6xx::a6xx_state_block block) const {
        const auto& w = stream.words();
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kCommand && h.opcode == opcode && h.count >= 1) {
                uint32_t w0 = w[i + 1];
                if (((w0 & a6xx::CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> a6xx::CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) == type &&
                    ((w0 & a6xx::CP_LOAD_STATE6_0_STATE_BLOCK__MASK) >> a6xx::CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT) == block) {
                    return i;
                }
            }
            i += 1 + h.count;
        }
        return size_t(-1);
    }
    uint32_t* tableWords() const { return static_cast<uint32_t*>(table->contents()); }
    std::shared_ptr<Buffer> vsProgram, fsProgram, table;
    pm4::Stream stream;
    bool ok = false;
    std::string error;
};

void expectRefusal(const std::string& got, const char* want) {
    CHECK(got == want);
    if (got != want) std::fprintf(stderr, "  got      \"%s\"\n  expected \"%s\"\n", got.c_str(), want);
}

// encode refuses with exactly WANT, adds nothing to the stream, and a command buffer carrying the same draw ends with
// that error and nothing submitted
void refused(Rig& r, const Program& p, const RenderPass& pass, const Draw& d, const char* want, const render::Options& o = render::Options{}) {
    Encoded e(r, p, pass, d, o);
    CHECK(!e.ok);
    expectRefusal(e.error, want);
    CHECK_EQ(e.stream.size(), size_t(0));
    auto cb = r.queue->commandBuffer();
    CHECK(!cb->draw(p, pass, d, Bindings{}, Bindings{}, o));
    expectRefusal(cb->error(), want);
    CHECK_EQ(r.fake->stats().submits, 0u);
}
}  // namespace

// --- the refusals, each by its sentence, nothing reaching the GPU -------------------------------------------------------

TEST(render_refuses_what_the_first_draw_does_not_serve_by_name) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Draw d = wholeDraw();
    // the stages' kinds
    {
        Program p = handProgram();
        p.vs = handFs();
        refused(r, p, pass, d, "hand_fs is not a vertex stage");
        p = handProgram();
        p.fs = handVs();
        refused(r, p, pass, d, "hand_vs is not a fragment stage");
    }
    // what a stage shares with a compute kernel: compute.cpp's refusals, the stage's name in them
    {
        Program p = handProgram();
        p.vs.k.instrlen = 2;   // 32 words hold one unit
        refused(r, p, pass, d, "hand_vs: 32 words do not hold its instrlen of 2 units");
        p = handProgram();
        static const std::vector<uint32_t> longWords(128 * 32, 0);   // 128 units of nothing: long enough to hold the instrlen
        p.fs.k.words = longWords.data();
        p.fs.k.numWords = uint32_t(longWords.size());
        p.fs.k.instrlen = 128;
        refused(r, p, pass, d, "hand_fs is 128 units long: more than the 127 the instruction cache preloads, and the longer-program path is not written here");
        p = handProgram();
        p.vs.k.pvtmemSize = 64;
        refused(r, p, pass, d, "hand_vs wants 64 bytes of private memory a fiber: not served yet");
        p = handProgram();
        p.fs.k.numSamp = 32;
        refused(r, p, pass, d, "hand_fs: numSamp 32 does not fit SP_xS_CONFIG.NSAMP's 5 bits (31 at most)");
        p = handProgram();
        p.vs.k.numSsbos = 1;
        p.vs.k.numIbos = 1;
        refused(r, p, pass, d, "hand_vs binds 1 device buffer(s), 0 constant buffer(s) it indexes and 1 IBO(s): only constant buffers read at fixed offsets (pushed ranges) are served in a draw yet");
        p = handProgram();
        p.fs.k.numUbos = 1;
        refused(r, p, pass, d, "hand_fs binds 0 device buffer(s), 1 constant buffer(s) it indexes and 0 IBO(s): only constant buffers read at fixed offsets (pushed ranges) are served in a draw yet");
        p = handProgram();
        p.fs.k.numUboRanges = 1;   // a pushed range, served -- but nothing is bound at its block
        refused(r, p, pass, d, "hand_fs: no buffer bound at index 0 (its constant buffer)");
        p = handProgram();
        p.fs.k.textures[0].kind = kTexSampled;
        refused(r, p, pass, d, "hand_fs samples or stores through textures: textures and samplers in a draw are step two");
        p = handProgram();
        p.vs.k.driverParamsOffsetVec4 = 1;
        refused(r, p, pass, d, "hand_vs reads driver params at vec4 1: not served in the first draw");
        p = handProgram();
        p.fs.k.constantDataSize = 16;
        refused(r, p, pass, d, "hand_fs carries 16 bytes of constant data: not uploaded yet");
        p = handProgram();
        static const uint32_t imm[4] = {1, 2, 3, 4};
        p.fs.k.immediates = imm;
        p.fs.k.numImmediates = 4;   // one vec4 at 0 of a constlen of 0
        refused(r, p, pass, d, "hand_fs: 1 vec4 of constants at 0 run past its constlen of 0");
        p = handProgram();
        p.vs.k.constlen = 400;
        p.fs.k.constlen = 116;
        refused(r, p, pass, d, "the two stages need 516 vec4 of constants together (400 + 116): the pipeline has 512 (ir3_compiler.c max_const_pipeline)");
    }
    // the vertex stage's shape
    {
        Program p = handProgram();
        p.vs.inputs[p.vs.numInputs++] = StageInput{15, regid(2, 0), 0x3, 0, 0, 0, 0, 0};   // VERT_ATTRIB_GENERIC0: [[attribute(0)]]
        p.vs.numAttributes = 1;
        refused(r, p, pass, d, "hand_vs reads [[attribute(0)]], which the vertex descriptor does not describe");
        p = handProgram();
        p.vs.pointSizeRegid = regid(1, 0);
        refused(r, p, pass, d, "hand_vs writes a point size: points are not served");
        p = handProgram();
        p.vs.clipMask = 1;
        refused(r, p, pass, d, "hand_vs writes clip or cull distances: not served");
        p = handProgram();
        p.vs.writesLayer = 1;
        refused(r, p, pass, d, "hand_vs writes a layer or viewport index: not served");
        p = handProgram();
        p.vs.positionRegid = a6xx::kRegidNone;
        refused(r, p, pass, d, "hand_vs writes no position: nothing to rasterize");   // render::link's sentence
    }
    // the fragment stage's shape
    {
        Program p = handProgram();
        p.fs.numColorOutputs = 2;
        p.fs.colorRegid[1] = regid(1, 0);
        refused(r, p, pass, d, "hand_fs writes 2 colour output(s): one target, [[color(0)]], is served");
        p = handProgram();
        p.fs.colorRegid[0] = a6xx::kRegidNone;
        refused(r, p, pass, d, "hand_fs writes 1 colour output(s) and not [[color(0)]]: one target, [[color(0)]], is served");
        p = handProgram();
        p.fs.depthRegid = regid(1, 0);
        refused(r, p, pass, d, "hand_fs writes depth: no depth attachment is served");
        p = handProgram();
        p.fs.writesStencilref = 1;
        refused(r, p, pass, d, "hand_fs writes a stencil reference: no stencil attachment is served");
        p = handProgram();
        p.fs.sampleMaskRegid = regid(1, 0);
        refused(r, p, pass, d, "hand_fs writes a sample mask: no multisampling is served");
        p = handProgram();
        p.fs.sampleIdRegid = regid(1, 0);
        refused(r, p, pass, d, "hand_fs reads the sample id or the sample mask: no multisampling is served");
        p = handProgram();
        p.fs.ijRegid[a6xx::IJ_PERSP_CENTROID] = regid(1, 0);
        refused(r, p, pass, d, "hand_fs reads centroid or per-sample barycentrics: no multisampling is served");
        p = handProgram();
        p.fs.perSamp = 1;
        refused(r, p, pass, d, "hand_fs runs per sample: no multisampling is served");
        p = handProgram();
        p.fs.fbRead = 1;
        refused(r, p, pass, d, "hand_fs reads its render target ([[color(n)]] as an argument, framebuffer fetch): not served");
        p = handProgram();
        p.fs.dualSrcBlend = 1;
        refused(r, p, pass, d, "hand_fs writes a dual-source blend output: blending is off in the first draw");
        p = handProgram();
        p.fs.readsPrimid = 1;
        refused(r, p, pass, d, "hand_fs reads the primitive id: not served");
        p = handProgram();
        p.fs.numSamplerPrefetch = 1;
        refused(r, p, pass, d, "hand_fs has 1 texture prefetch(es): the draw does not emit SP_FS_PREFETCH_CMD yet");
        // a varying the vertex stage does not write: render::link's sentence
        p = handProgram();
        p.fs.inputs[1] = StageInput{kSlotVar0, uint8_t(regid(0, 2)), 0xf, 0, 0, 1, 0, 0};
        p.fs.numInputs = 2;
        p.fs.totalIn = 4;
        refused(r, p, pass, d, "the fragment stage reads varying slot 32 (VAR0), which the vertex stage does not write");
    }
    // the target
    {
        Program p = handProgram();
        RenderPass none;
        refused(r, p, none, d, "a render pass with no colour attachment: one target is what the first draw serves");
        auto plain = target(r, 64, 64, kTextureUsageRead | kTextureUsageWrite);
        RenderPass notRt;
        notRt.color0 = plain.get();
        refused(r, p, notRt, d, "the 64 x 64 RGBA8Unorm texture was made without render-target usage (MTLTextureUsageRenderTarget, 4); a draw needs it");
        auto uint32 = target(r, 64, 64, kTextureUsageRenderTarget, PixelFormat::kR32Uint);
        RenderPass ints;
        ints.color0 = uint32.get();
        refused(r, p, ints, d, "hand_fs returns floats into R32Uint, which holds integers");
        Program ip = handProgram();
        ip.fs.outputs[0].pad = kElemUint;
        refused(r, ip, pass, d, "hand_fs returns integers into RGBA8Unorm, which holds floats");
        RenderPass clear = pass;
        clear.load = LoadAction::kClear;
        refused(r, p, clear, d, "load action Clear: a clear is a draw in turnip's r3d path or a 2D blit, and neither is written here; the first draw's target is filled by the CPU");
        RenderPass odd = pass;
        odd.load = LoadAction(7);
        refused(r, p, odd, d, "load action 7: MTLLoadAction is DontCare (0), Load (1) or Clear (2)");
        RenderPass dontStore = pass;
        dontStore.store = StoreAction::kDontCare;
        refused(r, p, dontStore, d, "store action 0: only Store (1) is served");
    }
    // the draw
    {
        Program p = handProgram();
        Draw x = d;
        x.primitive = Primitive::kLine;
        refused(r, p, pass, x, "primitive 1: triangles (3) and triangle strips (4) are what the first draw serves");
        x = d;
        x.vertexCount = 0;
        refused(r, p, pass, x, "a draw of 0 vertices");
        x = d;
        x.instanceCount = 2;
        refused(r, p, pass, x, "2 instances: the first draw runs exactly 1");
        x = d;
        x.viewport.width = NAN;
        refused(r, p, pass, x, "a viewport with a non-finite value");
        x = d;
        x.viewport.width = 0;
        refused(r, p, pass, x, "a viewport of 0 x 64 pixels: both sides must be positive");
        x = d;
        x.viewport.x = 1;
        refused(r, p, pass, x, "viewport (1, 0, 64, 64) lies outside the 64 x 64 target");
        x = d;
        x.viewport.y = -1;
        refused(r, p, pass, x, "viewport (0, 0, 64, 64) lies outside the 64 x 64 target");
        x = d;
        x.scissor = Scissor{32, 0, 33, 64};
        refused(r, p, pass, x, "scissor (32, 0, 33, 64) lies outside the 64 x 64 target");
        x = d;
        x.scissor = Scissor{0, 0, 5, 0};
        refused(r, p, pass, x, "a scissor of 5 x 0 pixels: an empty scissor draws nothing (width 0 means the whole target)");
    }
    CHECK_EQ(r.fake->stats().submits, 0u);
}

// --- the words, against the literals worked out by hand (a6xx-draw.md; tests/a6xx_test.cpp) ---------------------------------

TEST(render_encodes_the_first_triangles_registers) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Encoded e(r, flatProgram(), pass, wholeDraw());
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    using namespace a6xx;
    // the table: the timestamps zeroed, the border colour entry zeroed, the hole zeroed; the rest untouched
    const uint32_t* tw = e.tableWords();
    for (uint32_t i = 0; i < 4; i++) CHECK_EQ(tw[render::kTableTimestampsAt / 4 + i], 0u);
    for (uint32_t i = 0; i < 32; i++) CHECK_EQ(tw[render::kTableBorderColorAt / 4 + i], 0u);
    CHECK_EQ(tw[4], 0xeeeeeeeeu);
    // the batch start: CP_SET_MODE first, the three invalidates, HLSQ_INVALIDATE_CMD all, a WFI, the static table...
    const auto& w = e.stream.words();
    CHECK_EQ(e.find(CP_SET_MODE), size_t(0));
    CHECK_EQ(w[1], 0u);
    CHECK_EQ(e.findEvent(PC_CCU_INVALIDATE_COLOR), size_t(2));
    CHECK_EQ(e.findEvent(PC_CCU_INVALIDATE_DEPTH), size_t(4));
    CHECK_EQ(e.findEvent(CACHE_INVALIDATE), size_t(6));
    CHECK_EQ(e.reg(REG_A6XX_SP_FLOAT_CNTL), uint32_t(A6XX_SP_FLOAT_CNTL_F16_NO_INF));   // fd6's value (turnip's is 0): a bisect item
    CHECK_EQ(e.reg(REG_A6XX_GRAS_UNKNOWN_8110), 2u);
    CHECK_EQ(e.reg(REG_A6XX_VFD_ADD_OFFSET), uint32_t(A6XX_VFD_ADD_OFFSET_VERTEX));
    CHECK_EQ(e.reg(REG_A6XX_VPC_SO_DISABLE), 1u);   // turnip's static 1 (fd6 writes 0 in its sysmem prep): not switchable yet
    CHECK_EQ(e.reg(REG_A6XX_RB_ALPHA_CONTROL), 0u);
    CHECK_EQ(e.writes(REG_A6XX_RB_ALPHA_CONTROL), 2u);   // the static tail's and blendZsa's
    for (uint32_t i = 0; i < kVfdFetchCount; i++) CHECK_EQ(e.reg(vfdFetchSize(i)), 0u);
    {
        size_t ds = e.find(CP_SET_DRAW_STATE);
        CHECK(ds != size_t(-1));
        CHECK_EQ(w[ds + 1], uint32_t(CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS));
        CHECK_EQ(w[ds + 2], 0u);
        CHECK_EQ(w[ds + 3], 0u);
    }
    uint64_t border = e.table->gpuAddress() + render::kTableBorderColorAt;
    CHECK_EQ(e.reg(REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR), uint32_t(border));
    CHECK_EQ(e.reg(REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR + 1), uint32_t(border >> 32));
    CHECK_EQ(e.reg(REG_A6XX_SP_PS_TP_BORDER_COLOR_BASE_ADDR), uint32_t(border));
    // RB_CCU_CNTL 0x10000000 right after a WFI
    {
        size_t ccu = e.findReg(REG_A6XX_RB_CCU_CNTL);
        CHECK(ccu != size_t(-1));
        CHECK_EQ(w[ccu], 0x10000000u);
        pm4::Header before = pm4::decode(w[ccu - 2]);
        CHECK(before.kind == pm4::Header::kCommand && before.opcode == pm4::kWaitForIdle && before.count == 0);
    }
    // the pass begin
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR), 0x003f003fu);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_2D_RESOLVE_CNTL_2), 0x003f003fu);
    CHECK_EQ(e.reg(REG_A6XX_RB_WINDOW_OFFSET), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_TP_WINDOW_OFFSET), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_BIN_CONTROL), 0x00c00000u);
    CHECK_EQ(e.reg(REG_A6XX_RB_BIN_CONTROL), 0x00c00000u);
    CHECK_EQ(e.reg(REG_A6XX_RB_BIN_CONTROL2), 0u);
    {
        size_t marker = e.find(CP_SET_MARKER);
        CHECK(marker != size_t(-1));
        CHECK_EQ(w[marker + 1], 1u);   // RM6_DIRECT_RENDER
        CHECK(e.find(CP_SKIP_IB2_ENABLE_GLOBAL) > marker);
        CHECK_EQ(w[e.find(CP_SKIP_IB2_ENABLE_GLOBAL) + 1], 0u);
        CHECK_EQ(w[e.find(CP_SET_VISIBILITY_OVERRIDE) + 1], 1u);
        CHECK(e.find(CP_SET_MODE, marker) != size_t(-1));
    }
    // the begin LRZ_FLUSH (fd6's), the depth-less zs, the target, no MSAA, RB_RENDER_CNTL, LRZ off
    CHECK(e.findEvent(LRZ_FLUSH) != size_t(-1));
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_BUFFER_INFO), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_BUFFER_INFO + 5), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCIL_INFO), 0u);
    {
        size_t mrt = e.findReg(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 0));
        CHECK(mrt != size_t(-1));
        CHECK_EQ(pm4::decode(w[mrt - 1]).count, 6u);   // six registers, not seven (a seventh is RB_MRT_CONTROL[1])
        CHECK_EQ(w[mrt], 0x30u);
        CHECK_EQ(w[mrt + 1], 4u);
        CHECK_EQ(w[mrt + 2], 0x100u);
        CHECK_EQ(w[mrt + 3], uint32_t(t->gpuAddress()));
        CHECK_EQ(w[mrt + 4], uint32_t(t->gpuAddress() >> 32));
        CHECK_EQ(w[mrt + 5], 0u);
    }
    CHECK_EQ(e.reg(regSpFsMrtReg(0)), 0x30u);
    CHECK_EQ(e.reg(regRbMrtFlagBuffer(REG_A6XX_RB_MRT_FLAG_BUFFER_ADDR, 0)), 0u);
    CHECK_EQ(e.reg(regRbMrtFlagBuffer(REG_A6XX_RB_MRT_FLAG_BUFFER_PITCH, 0)), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0), 0x30u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DITHER_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_SRGB_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_SRGB_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_MAX_LAYER_INDEX), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_TP_RAS_MSAA_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_TP_DEST_MSAA_CNTL), 4u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_DEST_MSAA_CNTL), 4u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEST_MSAA_CNTL), 4u);
    CHECK_EQ(e.reg(REG_A6XX_RB_BLIT_GMEM_MSAA_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_CNTL), 0x10u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_LRZ_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_LRZ_CNTL), 0u);
    CHECK_EQ(e.writes(REG_A6XX_GRAS_LRZ_CNTL), 3u);   // the static table's two and lrzOff's
    CHECK_EQ(e.reg(REG_A6XX_GRAS_LRZ_BUFFER_PITCH), 0u);
    // the program config: VS and FS enabled, HS/DS/GS off, the constlens in units of 4 vec4
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_CONFIG), 0x100u);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_VS_CNTL), 0x102u);
    CHECK_EQ(e.reg(REG_A6XX_SP_HS_CONFIG), 0u);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_GS_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_CONFIG), 0x100u);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_FS_CNTL), 0x100u);
    CHECK_EQ(e.reg(REG_A6XX_SP_IBO_COUNT), 0u);
    // the two programs: tri_flat_vs has maxReg 2 (3 full registers), tri_flat_fs maxReg 0 (1)
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_CTRL_REG0), 0x00100180u);   // FULLREGFOOTPRINT 3 << 7 | MERGEDREGS bit 20
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_INSTRLEN), 1u);
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_OBJ_START), uint32_t(e.vsProgram->gpuAddress()));
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_OBJ_START + 1), uint32_t(e.vsProgram->gpuAddress() >> 32));
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_PVT_MEM_PARAM), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_CTRL_REG0), 0x81000080u);   // FULLREGFOOTPRINT 1 << 7 | THREAD64 | UNK24 | MERGEDREGS bit 31
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_INSTRLEN), 1u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_OBJ_START), uint32_t(e.fsProgram->gpuAddress()));
    {
        size_t vsLoad = e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER);
        size_t fsLoad = e.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_FS_SHADER);
        CHECK(vsLoad != size_t(-1) && fsLoad != size_t(-1));
        CHECK_EQ(w[vsLoad + 1], loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_VS_SHADER, 1));
        CHECK_EQ(w[vsLoad + 2], uint32_t(e.vsProgram->gpuAddress()));
        CHECK_EQ(w[fsLoad + 2], uint32_t(e.fsProgram->gpuAddress()));
        CHECK_EQ(e.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_VS_SHADER), size_t(-1));   // never the FS opcode for the VS
        // tri_flat_vs's two immediates at vec4 1 (c0 is PRIMITIVE_PARAM), padded to one vec4, direct, by the VS opcode
        size_t imm = e.findLoad(CP_LOAD_STATE6_GEOM, ST6_CONSTANTS, SB6_VS_SHADER);
        CHECK(imm != size_t(-1));
        CHECK_EQ(w[imm + 1], loadState6_0(1, ST6_CONSTANTS, SS6_DIRECT, SB6_VS_SHADER, 1));
        CHECK_EQ(pm4::decode(w[imm]).count, 7u);
        CHECK_EQ(w[imm + 4], 0x3f800000u);
        CHECK_EQ(w[imm + 5], 0xbf800000u);
        CHECK_EQ(w[imm + 7], 0u);
        CHECK_EQ(e.findLoad(CP_LOAD_STATE6_FRAG, ST6_CONSTANTS, SB6_FS_SHADER), size_t(-1));   // the FS has none
    }
    // the vertex fetch: no attributes, vertex id in r0.x, the tess/GS registers none, index offset 0
    CHECK_EQ(e.reg(REG_A6XX_PC_MULTIVIEW_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_0), 0u);
    CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_1), 0xfcfcfc00u);
    CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_2), 0xfcfcu);
    CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_3), 0xfcfcfcfcu);
    CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_4), 0xfcu);
    CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_5), 0xfcfcu);
    CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_6), 0u);
    CHECK_EQ(e.reg(REG_A6XX_VFD_INDEX_OFFSET), 0u);
    CHECK_EQ(e.reg(REG_A6XX_VFD_INSTANCE_START_OFFSET), 0u);
    CHECK_EQ(e.reg(REG_A6XX_PC_RESTART_INDEX), 0xffffffffu);
    // the link, position only: r1.z (regid 6) at location 0, stride 4
    CHECK_EQ(e.reg(regSpVsOutReg(0)), 0x00000f06u);
    CHECK_EQ(e.reg(regSpVsVpcDstReg(0)), 0u);
    CHECK_EQ(e.reg(REG_A6XX_VPC_VS_PACK), 0x00ff0004u);
    CHECK_EQ(e.reg(REG_A6XX_VPC_VS_CLIP_CNTL), 0x00ffff00u);
    CHECK_EQ(e.reg(REG_A6XX_VPC_VS_CLIP_CNTL_V2), 0x00ffff00u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_VS_CL_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_PC_VS_OUT_CNTL), 4u);
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_PRIMITIVE_CNTL), 1u);
    CHECK_EQ(e.reg(REG_A6XX_VPC_VS_LAYER_CNTL), 0x00ffffffu);
    CHECK_EQ(e.reg(REG_A6XX_VPC_VS_LAYER_CNTL_V2), 0x00ffffffu);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_VS_LAYER_CNTL), 0u);
    for (uint32_t i = 0; i < 8; i++) {
        CHECK_EQ(e.reg(regVpcVaryingInterpMode(i)), 0u);
        CHECK_EQ(e.reg(regVpcVaryingPsReplMode(i)), 0u);
    }
    CHECK_EQ(e.reg(REG_A6XX_PC_PS_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_VPC_SO_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_PC_SO_STREAM_CNTL), 0u);
    // the FS's inputs: nothing read, so IJ_WRITE_DISABLE, every regid none, no varying anywhere
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_PREFETCH_CNTL), 0x8u);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_CONTROL_1_REG), 7u);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_CONTROL_2_REG), 0xfcfcfcfcu);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_CONTROL_3_REG), 0xfcfcfcfcu);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_CONTROL_4_REG), 0xfcfcfcfcu);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_CONTROL_5_REG), 0xfcfcu);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_FS_CNTL_0), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_CONTROL0), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_CONTROL1), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_SAMPLE_CNTL), 0u);
    for (uint32_t i = 0; i < 4; i++) CHECK_EQ(e.reg(regVpcVarDisable(i)), 0xffffffffu);
    CHECK_EQ(e.reg(REG_A6XX_VPC_CNTL_0), 0xff00ff00u);
    // the FS's outputs: colour in r0.x, one target, all four components
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_OUTPUT_CNTL0), 0xfcfcfc00u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_OUTPUT_CNTL1), 1u);
    CHECK_EQ(e.reg(regSpFsOutputReg(0)), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_RENDER_COMPONENTS), 0xfu);
    CHECK_EQ(e.reg(REG_A6XX_RB_FS_OUTPUT_CNTL0), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_FS_OUTPUT_CNTL1), 1u);
    CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_COMPONENTS), 0xfu);
    // the rasterizer
    CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_CNTL), 0xc0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_CNTL), 0x2010u);
    CHECK_EQ(e.reg(REG_A6XX_VPC_POLYGON_MODE), 3u);
    CHECK_EQ(e.reg(REG_A6XX_PC_POLYGON_MODE), 3u);
    CHECK_EQ(e.reg(REG_A6XX_PC_RASTER_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_POINT_MINMAX), 0xffc00001u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_POINT_SIZE), 0x10u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_POLY_OFFSET_OFFSET_CLAMP), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_UNKNOWN_8A30), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_CNTL), 2u);
    CHECK_EQ(e.reg(REG_A6XX_PC_PRIMITIVE_CNTL_0), 0u);
    // the viewport: offsets 32, x scale 32, y scale MINUS 32 (Metal's orientation: the probe's orientation line decides),
    // depth 0..1; the two scissors the whole target; the guardband at its maximum
    {
        size_t vp = e.findReg(REG_A6XX_GRAS_CL_VPORT_XOFFSET);
        CHECK(vp != size_t(-1));
        CHECK_EQ(pm4::decode(w[vp - 1]).count, 6u);
        CHECK_EQ(w[vp], 0x42000000u);
        CHECK_EQ(w[vp + 1], 0x42000000u);
        CHECK_EQ(w[vp + 2], 0x42000000u);
        CHECK_EQ(w[vp + 3], 0xc2000000u);
        CHECK_EQ(w[vp + 4], 0u);
        CHECK_EQ(w[vp + 5], 0x3f800000u);
    }
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL + 1), 0x003f003fu);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_Z_CLAMP_MIN), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_Z_CLAMP_MIN + 1), 0x3f800000u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ), 0x0007fdffu);
    CHECK_EQ(e.reg(REG_A6XX_RB_Z_CLAMP_MIN), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_Z_CLAMP_MAX), 0x3f800000u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL + 1), 0x003f003fu);
    // blending, depth and stencil off
    CHECK_EQ(e.reg(REG_A6XX_SP_BLEND_CNTL), 0x100u);
    CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_CNTL), 0xffff0100u);
    CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0x780u);
    CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, 0)), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_RED_F32), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_ALPHA_F32), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_PLANE_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_DEPTH_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCIL_CONTROL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_STENCIL_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCILREF), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCILWRMASK), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_Z_BOUNDS_MIN), 0u);
    CHECK_EQ(e.reg(REG_A6XX_RB_Z_BOUNDS_MAX), 0x3f800000u);
    // the draw: TRILIST, AUTO_INDEX, IGNORE_VISIBILITY, one instance, three vertices
    {
        size_t dr = e.find(CP_DRAW_INDX_OFFSET);
        CHECK(dr != size_t(-1));
        CHECK_EQ(pm4::decode(w[dr]).count, 3u);
        CHECK_EQ(w[dr + 1], 0x84u);
        CHECK_EQ(w[dr + 2], 1u);
        CHECK_EQ(w[dr + 3], 3u);
        // the tail after it: a WFI, the CCU colour flush writing 1 to the table (no TIMESTAMP bit) and its wait, LRZ_FLUSH,
        // the cache flush writing 2 and its wait; no RB_DONE_TS
        CHECK(pm4::decode(w[dr + 4]).opcode == pm4::kWaitForIdle);
        size_t ccu = e.findEvent(PC_CCU_FLUSH_COLOR_TS);
        CHECK(ccu > dr);
        CHECK_EQ(pm4::decode(w[ccu]).count, 4u);
        CHECK_EQ(w[ccu + 1], uint32_t(PC_CCU_FLUSH_COLOR_TS));
        CHECK_EQ(w[ccu + 2], uint32_t(e.table->gpuAddress() + render::kTableTimestampsAt));
        CHECK_EQ(w[ccu + 4], 1u);
        size_t wait = e.find(CP_WAIT_REG_MEM, ccu);
        CHECK_EQ(wait, ccu + 5);
        CHECK_EQ(w[wait + 4], 1u);
        size_t lrz = e.findEvent(LRZ_FLUSH);
        while (lrz != size_t(-1) && lrz < dr) {
            size_t next = size_t(-1);
            for (size_t i = lrz + 1 + pm4::decode(w[lrz]).count; i < w.size() && next == size_t(-1);) {
                pm4::Header h = pm4::decode(w[i]);
                if (h.kind == pm4::Header::kCommand && h.opcode == CP_EVENT_WRITE && (w[i + 1] & CP_EVENT_WRITE_0_EVENT__MASK) == LRZ_FLUSH) next = i;
                i += 1 + h.count;
            }
            lrz = next;
        }
        CHECK(lrz != size_t(-1) && lrz > wait);   // the tail's LRZ_FLUSH, beside the begin's
        size_t cache = e.findEvent(CACHE_FLUSH_TS);
        CHECK(cache > lrz);
        CHECK_EQ(w[cache + 2], uint32_t(e.table->gpuAddress() + render::kTableTimestampsAt + 4));
        CHECK_EQ(w[cache + 4], 2u);
        CHECK_EQ(e.find(CP_WAIT_MEM_GTE, cache), cache + 5);
        CHECK_EQ(e.findEvent(RB_DONE_TS), size_t(-1));
        CHECK_EQ(e.find(CP_WAIT_MEM_GTE, cache) + 5, w.size());   // and nothing after the last wait
    }
}

TEST(render_encodes_the_varying_link_of_tri_color) {
    // pair 2's numbers from air-vs-fs.md worked by hand over the fixtures' tables: the FS's VAR0 at inloc 0 takes the VS's
    // r4.y (0x11), the position r3.y (0x0d) is appended at location 4, stride 8; the FS reads the perspective pixel
    // barycentrics in r0.x and writes r0.z
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Encoded e(r, colorProgram(), pass, wholeDraw(6));
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    using namespace a6xx;
    CHECK_EQ(e.reg(regSpVsOutReg(0)), 0x0f0d0f11u);
    CHECK_EQ(e.reg(regSpVsVpcDstReg(0)), 0x00000400u);
    CHECK_EQ(e.reg(REG_A6XX_VPC_VS_PACK), 0x00ff0408u);
    CHECK_EQ(e.reg(REG_A6XX_PC_VS_OUT_CNTL), 8u);
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_PRIMITIVE_CNTL), 2u);
    CHECK_EQ(e.reg(regVpcVarDisable(0)), 0xfffffff0u);
    CHECK_EQ(e.reg(regVpcVarDisable(1)), 0xffffffffu);
    CHECK_EQ(e.reg(REG_A6XX_VPC_CNTL_0), 0xff01ff04u);
    CHECK_EQ(e.reg(regVpcVaryingInterpMode(0)), 0u);   // smooth
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_PREFETCH_CNTL), 0u);   // ij_pixel is read: no IJ_WRITE_DISABLE
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_CONTROL_3_REG), 0xfcfcfc00u);
    CHECK_EQ(e.reg(REG_A6XX_HLSQ_FS_CNTL_0), 2u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_CNTL), 1u);
    CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_CONTROL0), 0x401u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_CTRL_REG0), 0x81400100u);   // FULLREGFOOTPRINT 2 | VARYING | UNK24 | MERGEDREGS
    CHECK_EQ(e.reg(regSpFsOutputReg(0)), 2u);
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_CTRL_REG0), 0x00100300u);   // maxReg 5: 6 full registers
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_INSTRLEN), 2u);
    CHECK_EQ(e.stream.words()[e.find(CP_DRAW_INDX_OFFSET) + 3], 6u);
    CHECK_EQ(e.stream.words()[e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER) + 1], loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_VS_SHADER, 2));
    // and the hand-built pair encodes the same shape as tri_flat (position r0.y, vertex id r0.x, colour r0.x)
    Encoded h(r, handProgram(), pass, wholeDraw());
    CHECK(h.ok);
    CHECK_EQ(h.reg(regSpVsOutReg(0)), 0x00000f01u);
    CHECK_EQ(h.reg(REG_A6XX_VPC_VS_PACK), 0x00ff0004u);
    CHECK_EQ(h.reg(REG_A6XX_VFD_CONTROL_1), 0xfcfcfc00u);
    CHECK_EQ(h.reg(REG_A6XX_SP_VS_CTRL_REG0), 0x00100100u);
    CHECK_EQ(h.reg(REG_A6XX_HLSQ_VS_CNTL), 0x102u);
    CHECK_EQ(h.reg(regSpFsOutputReg(0)), 0u);
}

TEST(render_options_switch_each_group_and_each_alternative) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Draw d = wholeDraw();
    Program p = flatProgram();
    using namespace a6xx;
    const Encoded whole(r, p, pass, d);
    CHECK(whole.ok);
    auto with = [&](void (*change)(render::Options&)) {
        render::Options o;
        change(o);
        return Encoded(r, p, pass, d, o);
    };
    // each group off removes its registers and packets, and only its
    {
        Encoded e = with([](render::Options& o) { o.batchStart = false; });
        CHECK(e.ok);
        CHECK_EQ(e.reg(REG_A6XX_SP_FLOAT_CNTL), Encoded::kNone);
        CHECK_EQ(e.find(CP_SET_DRAW_STATE), size_t(-1));
        CHECK_EQ(e.reg(REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR), Encoded::kNone);
        CHECK_EQ(e.writes(REG_A6XX_RB_ALPHA_CONTROL), 1u);   // blendZsa's alone
        CHECK_EQ(e.reg(REG_A6XX_RB_CCU_CNTL), 0x10000000u);   // ccuCntl is its own group
        CHECK(e.stream.size() < whole.stream.size());
    }
    {
        Encoded e = with([](render::Options& o) { o.ccuCntl = false; });
        CHECK(e.ok);
        CHECK_EQ(e.reg(REG_A6XX_RB_CCU_CNTL), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.ccuConcurrentResolve = true; });
        CHECK_EQ(e.reg(REG_A6XX_RB_CCU_CNTL), 0x10000004u);
    }
    {
        Encoded e = with([](render::Options& o) { o.passBegin = false; });
        CHECK(e.ok);
        CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_GRAS_BIN_CONTROL), Encoded::kNone);
        CHECK_EQ(e.find(CP_SET_VISIBILITY_OVERRIDE), size_t(-1));
        CHECK_EQ(e.find(CP_SKIP_IB2_ENABLE_GLOBAL), size_t(-1));
        CHECK(e.find(CP_SET_MARKER) != size_t(-1));   // the marker is its own switch
    }
    {
        Encoded e = with([](render::Options& o) { o.binControlLrzFeedback = true; });
        CHECK_EQ(e.reg(REG_A6XX_GRAS_BIN_CONTROL), 0x03c00000u);
        CHECK_EQ(e.reg(REG_A6XX_RB_BIN_CONTROL), 0x03c00000u);
    }
    {
        Encoded e = with([](render::Options& o) { o.marker = false; });
        CHECK_EQ(e.find(CP_SET_MARKER), size_t(-1));
        CHECK(e.find(CP_SET_VISIBILITY_OVERRIDE) != size_t(-1));
    }
    {
        Encoded e = with([](render::Options& o) { o.zsNone = false; });
        CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_BUFFER_INFO), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_RB_STENCIL_INFO), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.mrt = false; });
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 0)), Encoded::kNone);
        CHECK_EQ(e.reg(regSpFsMrtReg(0)), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_RB_SRGB_CNTL), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.msaaOff = false; });
        CHECK_EQ(e.reg(REG_A6XX_RB_DEST_MSAA_CNTL), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_RB_BLIT_GMEM_MSAA_CNTL), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.renderCntl = false; });
        CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_CNTL), Encoded::kNone);
        Encoded z = with([](render::Options& o) { o.renderCntlZero = true; });
        CHECK_EQ(z.reg(REG_A6XX_RB_RENDER_CNTL), 0u);
    }
    {
        Encoded e = with([](render::Options& o) { o.lrzOff = false; });
        CHECK_EQ(e.writes(REG_A6XX_GRAS_LRZ_CNTL), 2u);   // the static table's two remain
        CHECK_EQ(e.reg(REG_A6XX_GRAS_LRZ_BUFFER_PITCH), Encoded::kNone);
        size_t lrz = e.findEvent(LRZ_FLUSH);
        CHECK(lrz > e.find(CP_DRAW_INDX_OFFSET));   // only the tail's LRZ_FLUSH is left
    }
    {
        Encoded e = with([](render::Options& o) { o.programConfig = false; });
        CHECK_EQ(e.reg(REG_A6XX_HLSQ_VS_CNTL), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_SP_FS_CONFIG), Encoded::kNone);
        CHECK_EQ(e.writes(REG_A6XX_HLSQ_INVALIDATE_CMD), 1u);   // the batch start's alone
    }
    {
        Encoded e = with([](render::Options& o) { o.programVs = false; });
        CHECK_EQ(e.reg(REG_A6XX_SP_VS_CTRL_REG0), Encoded::kNone);
        CHECK_EQ(e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER), size_t(-1));
        CHECK(e.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_FS_SHADER) != size_t(-1));
        Encoded f = with([](render::Options& o) { o.programFs = false; });
        CHECK_EQ(f.reg(REG_A6XX_SP_FS_CTRL_REG0), Encoded::kNone);
        CHECK_EQ(f.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_FS_SHADER), size_t(-1));
    }
    {
        Encoded e = with([](render::Options& o) { o.vfd = false; });
        CHECK_EQ(e.reg(REG_A6XX_VFD_CONTROL_1), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_PC_RESTART_INDEX), Encoded::kNone);
        CHECK_EQ(e.writes(REG_A6XX_PC_MULTIVIEW_CNTL), 1u);   // the static table's
    }
    {
        Encoded e = with([](render::Options& o) { o.vpc = false; });
        CHECK_EQ(e.reg(REG_A6XX_VPC_VS_PACK), Encoded::kNone);
        CHECK_EQ(e.reg(regSpVsOutReg(0)), Encoded::kNone);
        CHECK_EQ(e.reg(regVpcVaryingInterpMode(7)), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.fsInputs = false; });
        CHECK_EQ(e.reg(REG_A6XX_HLSQ_CONTROL_3_REG), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_VPC_CNTL_0), Encoded::kNone);
        CHECK_EQ(e.reg(regVpcVarDisable(3)), Encoded::kNone);
        CHECK_EQ(e.writes(REG_A6XX_HLSQ_CONTROL_5_REG), 1u);   // the static table's
    }
    {
        Encoded e = with([](render::Options& o) { o.fsOutputs = false; });
        CHECK_EQ(e.reg(regSpFsOutputReg(0)), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_COMPONENTS), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.rast = false; });
        CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_CNTL), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_CNTL), Encoded::kNone);
        CHECK_EQ(e.writes(REG_A6XX_GRAS_SC_CNTL), 1u);   // the static table's
    }
    {
        Encoded e = with([](render::Options& o) { o.viewportScissor = false; });
        CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_VPORT_XOFFSET), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.blendZsa = false; });
        CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_CNTL), Encoded::kNone);
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), Encoded::kNone);
        CHECK_EQ(e.reg(REG_A6XX_RB_Z_BOUNDS_MAX), Encoded::kNone);
    }
    {
        Encoded e = with([](render::Options& o) { o.visCullUse = true; });
        CHECK_EQ(e.stream.words()[e.find(CP_DRAW_INDX_OFFSET) + 1], 0x184u);
    }
    {
        Encoded e = with([](render::Options& o) { o.tailCcuFlush = false; });
        CHECK_EQ(e.findEvent(PC_CCU_FLUSH_COLOR_TS), size_t(-1));
        CHECK_EQ(e.find(CP_WAIT_REG_MEM), size_t(-1));
        CHECK(e.findEvent(CACHE_FLUSH_TS) != size_t(-1));
    }
    {
        Encoded e = with([](render::Options& o) { o.tailLrzFlush = false; });
        CHECK(e.findEvent(LRZ_FLUSH) < e.find(CP_DRAW_INDX_OFFSET));   // the begin's stays, the tail's goes
        size_t dr = e.find(CP_DRAW_INDX_OFFSET);
        bool after = false;
        for (size_t i = dr; i < e.stream.size();) {
            pm4::Header h = pm4::decode(e.stream.words()[i]);
            after = after || (h.kind == pm4::Header::kCommand && h.opcode == CP_EVENT_WRITE && (e.stream.words()[i + 1] & CP_EVENT_WRITE_0_EVENT__MASK) == LRZ_FLUSH);
            i += 1 + h.count;
        }
        CHECK(!after);
    }
    {
        Encoded e = with([](render::Options& o) { o.tailCacheFlush = false; });
        CHECK_EQ(e.findEvent(CACHE_FLUSH_TS), size_t(-1));
        CHECK_EQ(e.find(CP_WAIT_MEM_GTE), size_t(-1));
    }
    {
        Encoded e = with([](render::Options& o) { o.tailRbDone = true; });
        size_t rb = e.findEvent(RB_DONE_TS);
        CHECK(rb != size_t(-1));
        CHECK_EQ(e.stream.words()[rb + 2], uint32_t(e.table->gpuAddress() + render::kTableTimestampsAt + 8));
        CHECK_EQ(e.stream.words()[rb + 4], 3u);
        CHECK(e.findEvent(PC_CCU_FLUSH_COLOR_TS) < rb && rb < e.findEvent(CACHE_FLUSH_TS));
    }
    {
        Encoded e = with([](render::Options& o) { o.tailTimestampBit = true; });
        size_t ccu = e.findEvent(PC_CCU_FLUSH_COLOR_TS);
        CHECK((e.stream.words()[ccu + 1] & CP_EVENT_WRITE_0_TIMESTAMP) != 0);
        CHECK((whole.stream.words()[whole.findEvent(PC_CCU_FLUSH_COLOR_TS) + 1] & CP_EVENT_WRITE_0_TIMESTAMP) == 0);
    }
    // the draw's own knobs: a half viewport, a scissor, a vertex start, a strip, a 48x20 target
    {
        Draw half = d;
        half.viewport = Viewport{0, 0, 32, 64, 0, 1};
        Encoded e(r, p, pass, half);
        CHECK(e.ok);
        size_t vp = e.findReg(REG_A6XX_GRAS_CL_VPORT_XOFFSET);
        CHECK_EQ(e.stream.words()[vp], 0x41800000u);       // x offset 16
        CHECK_EQ(e.stream.words()[vp + 1], 0x41800000u);   // x scale 16
        CHECK_EQ(e.stream.words()[vp + 2], 0x42000000u);   // y offset 32
        CHECK_EQ(e.stream.words()[vp + 3], 0xc2000000u);   // y scale -32
        CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL + 1), 0x003f001fu);
        CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL + 1), 0x003f003fu);   // the scissor stays the whole target
        CHECK_EQ(e.reg(REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ), 0x0007fdffu);       // 16/16: still MAX_GB (exponent 11)
        Draw sc = d;
        sc.scissor = Scissor{8, 4, 16, 32};
        Encoded s(r, p, pass, sc);
        CHECK(s.ok);
        CHECK_EQ(s.reg(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL), 0x00040008u);
        CHECK_EQ(s.reg(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL + 1), 0x00230017u);
        Draw from3 = d;
        from3.vertexStart = 3;
        Encoded f(r, p, pass, from3);
        CHECK_EQ(f.reg(REG_A6XX_VFD_INDEX_OFFSET), 3u);
        Draw strip = d;
        strip.primitive = Primitive::kTriangleStrip;
        strip.vertexCount = 4;
        Encoded st(r, p, pass, strip);
        CHECK_EQ(st.stream.words()[st.find(CP_DRAW_INDX_OFFSET) + 1], 0x86u);   // DI_PT_TRISTRIP 6
        CHECK_EQ(st.stream.words()[st.find(CP_DRAW_INDX_OFFSET) + 3], 4u);
        auto small = target(r, 48, 20);
        RenderPass sp;
        sp.color0 = small.get();
        Encoded o(r, p, sp, wholeDraw(3, 48, 20));
        CHECK(o.ok);
        CHECK_EQ(o.reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR), 0x0013002fu);
        CHECK_EQ(o.reg(regRbMrt(REG_A6XX_RB_MRT_PITCH, 0)), 4u);
        CHECK_EQ(o.reg(regRbMrt(REG_A6XX_RB_MRT_ARRAY_PITCH, 0)), 0x80u);
        size_t ovp = o.findReg(REG_A6XX_GRAS_CL_VPORT_XOFFSET);
        CHECK_EQ(o.stream.words()[ovp], 0x41c00000u);       // 24
        CHECK_EQ(o.stream.words()[ovp + 2], 0x41200000u);   // 10
        CHECK_EQ(o.stream.words()[ovp + 3], 0xc1200000u);   // -10
        // an sRGB target sets both sRGB bits; a BGRA one the swap
        auto srgb = target(r, 64, 64, kTextureUsageRenderTarget, PixelFormat::kBGRA8UnormSrgb);
        RenderPass sg;
        sg.color0 = srgb.get();
        Encoded g(r, p, sg, d);
        CHECK(g.ok);
        CHECK_EQ(g.reg(REG_A6XX_RB_SRGB_CNTL), 1u);
        CHECK_EQ(g.reg(REG_A6XX_SP_SRGB_CNTL), 1u);
        CHECK_EQ(g.reg(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 0)), 0x2030u);
    }
}

// --- draws that complete on the fake, read back through lastDraw --------------------------------------------------------

namespace {
// one draw through CommandBuffer::draw, committed and waited for; the command buffer is returned for its status and error
std::shared_ptr<CommandBuffer> drawOnce(Rig& r, const Program& p, const RenderPass& pass, const Draw& d, const render::Options& o = render::Options{},
                                        const Bindings& vsb = Bindings{}, const Bindings& fsb = Bindings{}) {
    auto cb = r.queue->commandBuffer();
    if (cb->draw(p, pass, d, vsb, fsb, o)) {
        cb->commit();
        cb->waitUntilCompleted();
    }
    return cb;
}

bool allWords(const Texture& t, uint32_t value) {
    auto* w = static_cast<const uint32_t*>(t.contents());
    for (uint64_t i = 0; i < t.buffer()->length() / 4; i++) {
        if (w[i] != value) return false;
    }
    return true;
}

void fillWords(const Texture& t, uint32_t value) {
    auto* w = static_cast<uint32_t*>(t.contents());
    for (uint64_t i = 0; i < t.buffer()->length() / 4; i++) w[i] = value;
}
}  // namespace

TEST(a_draw_of_tri_flat_holds_together_and_the_fake_does_not_rasterize_it) {
    Rig r;
    auto t = target(r);
    fillWords(*t, 0xa5a5a5a5u);
    RenderPass pass;
    pass.color0 = t.get();
    auto cb = drawOnce(r, flatProgram(), pass, wholeDraw());
    CHECK(cb->error().empty());
    if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
    CHECK(cb->status() == CommandBuffer::kCompleted);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK_EQ(s.submits, 1u);
    CHECK_EQ(s.draws, 1u);
    CHECK_EQ(s.dispatches, 0u);
    const FakeGpu::Draw& d = s.lastDraw;
    CHECK_EQ(d.primType, 4u);        // DI_PT_TRILIST
    CHECK_EQ(d.sourceSelect, 2u);    // DI_SRC_SEL_AUTO_INDEX
    CHECK_EQ(d.visCull, 0u);         // IGNORE_VISIBILITY
    CHECK_EQ(d.numInstances, 1u);
    CHECK_EQ(d.numIndices, 3u);
    CHECK_EQ(d.vsProgram % 128, 0u);
    CHECK_EQ(d.fsProgram % 128, 0u);
    CHECK(d.vsProgram != d.fsProgram);
    CHECK_EQ(d.vsInstrlen, 1u);
    CHECK_EQ(d.fsInstrlen, 1u);
    CHECK_EQ(d.vsConstlen, 8u);
    CHECK_EQ(d.fsConstlen, 0u);
    CHECK_EQ(d.vsConsts.size(), size_t(1));   // tri_flat_vs's immediates: one vec4 at 1, direct
    CHECK_EQ(d.vsConsts[0].dstOffVec4, 1u);
    CHECK_EQ(d.vsConsts[0].units, 1u);
    CHECK(d.vsConsts[0].direct);
    CHECK_EQ(d.fsConsts.size(), size_t(0));
    // the target as the registers name it, against the Texture
    CHECK_EQ(d.mrtFormat, 48u);
    CHECK_EQ(d.mrtSwap, 0u);
    CHECK_EQ(d.mrtTileMode, 0u);
    CHECK_EQ(d.mrtPitch, uint32_t(t->bytesPerRow()));
    CHECK_EQ(d.mrtPitch, 256u);
    CHECK_EQ(d.mrtArrayPitch, uint32_t(t->buffer()->length()));
    CHECK_EQ(d.mrtBase, t->gpuAddress());
    CHECK_EQ(d.spFsMrtReg, 0x30u);
    CHECK_EQ(d.renderComponents, 0xfu);
    CHECK_EQ(d.fsRenderComponents, 0xfu);
    CHECK_EQ(d.mrtCount, 1u);
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(d.windowScissor[i], 0u);
        CHECK_EQ(d.screenScissor[i], 0u);
        CHECK_EQ(d.viewportScissor[i], 0u);
        CHECK_EQ(d.windowScissor[2 + i], 63u);
        CHECK_EQ(d.screenScissor[2 + i], 63u);
        CHECK_EQ(d.viewportScissor[2 + i], 63u);
    }
    // the viewport's six floats, the y scale NEGATIVE
    CHECK(d.viewport[0] == 32.0f && d.viewport[1] == 32.0f && d.viewport[2] == 32.0f);
    CHECK(d.viewport[3] == -32.0f);
    CHECK(d.viewport[4] == 0.0f && d.viewport[5] == 1.0f);
    CHECK_EQ(d.vertexIdRegid, 0u);   // r0.x
    CHECK_EQ(d.vfdFetchCount, 0u);
    CHECK_EQ(d.vfdDecodeCount, 0u);
    CHECK_EQ(d.vertexIndexOffset, 0u);
    CHECK_EQ(d.positionLoc, 0u);
    CHECK_EQ(d.strideInVpc, 4u);
    CHECK_EQ(d.vsOutCount, 1u);
    CHECK_EQ(d.numNonPosVar, 0u);
    CHECK_EQ(d.spFsOutputReg0, 0u);   // r0.x
    CHECK(d.ccuFlushed && d.cacheFlushed);
    // the fake has no rasterizer: it says so here, so that nobody reads "completed" as "drawn"
    CHECK(allWords(*t, 0xa5a5a5a5u));
}

TEST(a_draw_of_tri_color_and_of_the_hand_built_pair_hold_together) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    auto cb = drawOnce(r, colorProgram(), pass, wholeDraw(6));
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK_EQ(s.draws, 1u);
    CHECK_EQ(s.lastDraw.numIndices, 6u);
    CHECK_EQ(s.lastDraw.vsInstrlen, 2u);
    CHECK_EQ(s.lastDraw.positionLoc, 4u);
    CHECK_EQ(s.lastDraw.strideInVpc, 8u);
    CHECK_EQ(s.lastDraw.vsOutCount, 2u);
    CHECK_EQ(s.lastDraw.numNonPosVar, 4u);
    CHECK_EQ(s.lastDraw.spFsOutputReg0, 2u);   // r0.z
    CHECK_EQ(s.lastDraw.vsConsts.size(), size_t(1));
    // the hand-built pair, then a half viewport, a scissor, a vertex start and a strip, each its own command buffer
    auto h = drawOnce(r, handProgram(), pass, wholeDraw());
    CHECK(h->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().lastDraw.spFsOutputReg0, 0u);
    CHECK_EQ(r.fake->stats().lastDraw.vsConsts.size(), size_t(0));
    Draw half = wholeDraw();
    half.viewport = Viewport{0, 0, 32, 64, 0, 1};
    half.vertexStart = 3;
    half.scissor = Scissor{8, 4, 16, 32};
    half.primitive = Primitive::kTriangleStrip;
    half.vertexCount = 4;
    auto hv = drawOnce(r, flatProgram(), pass, half);
    CHECK(hv->status() == CommandBuffer::kCompleted);
    s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK_EQ(s.draws, 3u);
    CHECK_EQ(s.lastDraw.primType, 6u);   // DI_PT_TRISTRIP
    CHECK_EQ(s.lastDraw.numIndices, 4u);
    CHECK_EQ(s.lastDraw.vertexIndexOffset, 3u);
    CHECK(s.lastDraw.viewport[0] == 16.0f && s.lastDraw.viewport[1] == 16.0f && s.lastDraw.viewport[2] == 32.0f && s.lastDraw.viewport[3] == -32.0f);
    CHECK_EQ(s.lastDraw.viewportScissor[2], 31u);
    CHECK_EQ(s.lastDraw.viewportScissor[3], 63u);
    CHECK_EQ(s.lastDraw.screenScissor[0], 8u);
    CHECK_EQ(s.lastDraw.screenScissor[1], 4u);
    CHECK_EQ(s.lastDraw.screenScissor[2], 23u);
    CHECK_EQ(s.lastDraw.screenScissor[3], 35u);
    // two passes in one command buffer: two draws, each flushed by its own tail
    auto two = r.queue->commandBuffer();
    CHECK(two->draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{}));
    CHECK(two->draw(flatProgram(), pass, wholeDraw(6), Bindings{}, Bindings{}));
    two->commit();
    two->waitUntilCompleted();
    CHECK(two->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().draws, 5u);
    CHECK_EQ(r.fake->stats().faults, 0u);
    // a 48x20 target: 192-byte rows at a pitch of 256, 20 rows counted to 24 inside the 8192-byte buffer
    auto small = target(r, 48, 20);
    RenderPass sp;
    sp.color0 = small.get();
    auto sm = drawOnce(r, flatProgram(), sp, wholeDraw(3, 48, 20));
    CHECK(sm->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().lastDraw.windowScissor[2], 47u);
    CHECK_EQ(r.fake->stats().lastDraw.windowScissor[3], 19u);
    CHECK_EQ(r.fake->stats().lastDraw.mrtArrayPitch, 8192u);
    // the alternatives the bisect runs all hold together on the fake (what the hardware wants of them is the phone's)
    for (auto change : {+[](render::Options& o) { o.visCullUse = true; }, +[](render::Options& o) { o.binControlLrzFeedback = true; },
                        +[](render::Options& o) { o.ccuConcurrentResolve = true; }, +[](render::Options& o) { o.renderCntlZero = true; },
                        +[](render::Options& o) { o.tailRbDone = true; }, +[](render::Options& o) { o.batchStart = false; },
                        +[](render::Options& o) { o.tailLrzFlush = false; }, +[](render::Options& o) { o.lrzOff = false; },
                        +[](render::Options& o) { o.zsNone = false; }, +[](render::Options& o) { o.renderCntl = false; },
                        +[](render::Options& o) { o.rast = false; }, +[](render::Options& o) { o.vfd = false; },
                        +[](render::Options& o) { o.fsInputs = false; }}) {
        render::Options o;
        change(o);
        auto c = drawOnce(r, flatProgram(), pass, wholeDraw(), o);
        CHECK(c->status() == CommandBuffer::kCompleted);
        if (c->status() != CommandBuffer::kCompleted) std::printf("       %s\n", r.fake->stats().lastFault.c_str());
    }
    CHECK_EQ(r.fake->stats().faults, 0u);
    CHECK_EQ(r.fake->stats().lastDraw.visCull, 0u);
    auto v = drawOnce(r, flatProgram(), pass, wholeDraw(), [] { render::Options o; o.visCullUse = true; return o; }());
    CHECK(v->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().lastDraw.visCull, 1u);
}

// --- the fake can fail: one class of fault at a time, a mutation twin per check -----------------------------------------------

namespace {
bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// WORDS run raw: the fault the fake stopped on (and a check that it did stop)
std::string faultOf(Rig& r, const std::vector<uint32_t>& words) {
    uint32_t faultsBefore = r.fake->stats().faults;
    Raw raw(r);
    int e = raw.submit(words);
    CHECK_EQ(e, ETIMEDOUT);
    CHECK_EQ(r.fake->stats().faults, faultsBefore + 1);
    return r.fake->stats().lastFault;
}

// the flat draw encoded over a fresh rig, with its words as a vector to mutate
struct Twin {
    explicit Twin(const render::Options& o = render::Options{}) : t(target(r)), pass(), e(r, flatProgram(), (pass.color0 = t.get(), pass), wholeDraw(), o) {
        CHECK(e.ok);
        words = e.stream.words();
    }
    // the value word of register REG in the stream, asserting it is there
    uint32_t& at(uint32_t reg) {
        size_t i = e.findReg(reg);
        CHECK(i != size_t(-1));
        return words[i == size_t(-1) ? 0 : i];
    }
    std::string fault() { return faultOf(r, words); }
    Rig r;
    std::shared_ptr<Texture> t;
    RenderPass pass;
    Encoded e;
    std::vector<uint32_t> words;
};
}  // namespace

TEST(the_fake_accepts_the_unmutated_flat_draw_raw) {
    // the twins below start from this stream: it must pass as it is, or every refusal below would pass for the wrong reason
    Twin w;
    Raw raw(w.r);
    CHECK_EQ(raw.submit(w.words), 0);
    CHECK_EQ(w.r.fake->stats().faults, 0u);
    CHECK_EQ(w.r.fake->stats().draws, 1u);
    // the pass's table was written by the tail's two events (the values their waits read): 1 by the CCU flush, 2 by the cache flush
    CHECK_EQ(w.e.tableWords()[0], 1u);
    CHECK_EQ(w.e.tableWords()[1], 2u);
    CHECK_EQ(w.e.tableWords()[2], 0u);   // RB_DONE_TS's word, off by default
}

TEST(the_fake_refuses_a_draw_packet_that_is_not_the_three_word_triangle_form) {
    using namespace a6xx;
    {
        Twin w;
        size_t dr = w.e.find(CP_DRAW_INDX_OFFSET);
        std::vector<uint32_t> seven = {drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_DMA, IGNORE_VISIBILITY), 1, 3, 0, 0, 0, 3};
        w.words.erase(w.words.begin() + dr, w.words.begin() + dr + 4);
        w.words.insert(w.words.begin() + dr, pm4::type7(CP_DRAW_INDX_OFFSET, 7));
        w.words.insert(w.words.begin() + dr + 1, seven.begin(), seven.end());
        CHECK(has(w.fault(), "CP_DRAW_INDX_OFFSET of 7 words: indexed draws (7 words, DI_SRC_SEL_DMA) are not what this GPU model runs yet"));
    }
    {
        Twin w;
        w.words[w.e.find(CP_DRAW_INDX_OFFSET) + 1] = drawIndxOffset0(DI_PT_POINTLIST, DI_SRC_SEL_AUTO_INDEX, IGNORE_VISIBILITY);
        CHECK(has(w.fault(), "a draw of primitive type 9: triangle lists (4) and strips (6) are what this GPU model runs"));
    }
    {
        Twin w;
        w.words[w.e.find(CP_DRAW_INDX_OFFSET) + 1] = drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_DMA, IGNORE_VISIBILITY);
        CHECK(has(w.fault(), "a draw whose indices come from source 0, not DI_SRC_SEL_AUTO_INDEX (2)"));
    }
    {
        Twin w;
        w.words[w.e.find(CP_DRAW_INDX_OFFSET) + 2] = 2;
        CHECK(has(w.fault(), "a draw of 2 instances: one is what this GPU model runs"));
    }
    {
        Twin w;
        w.words[w.e.find(CP_DRAW_INDX_OFFSET) + 3] = 0;
        CHECK(has(w.fault(), "a draw of 0 vertices"));
    }
}

TEST(the_fake_refuses_a_draw_outside_a_begun_sysmem_pass) {
    using namespace a6xx;
    {
        render::Options o;
        o.marker = false;
        Twin w(o);
        CHECK(has(w.fault(), "a draw before CP_SET_MARKER RM6_DIRECT_RENDER: the sysmem pass was not begun in this submission"));
    }
    {
        Twin w;
        w.at(REG_A6XX_GRAS_BIN_CONTROL) = binControl(RENDERING_PASS, BUFFERS_IN_GMEM, LRZ_FEEDBACK_NONE, 64, 64);
        CHECK(has(w.fault(), "GRAS_BIN_CONTROL 0x402: a draw outside a sysmem pass (BUFFERS_IN_SYSMEM, no bins, RENDERING_PASS): GMEM rendering is not what this GPU model runs"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_BIN_CONTROL) = binControl(BINNING_PASS, BUFFERS_IN_SYSMEM, LRZ_FEEDBACK_NONE);
        CHECK(has(w.fault(), "RB_BIN_CONTROL 0xc40000: a draw outside a sysmem pass"));
    }
    {
        render::Options o;
        o.passBegin = false;   // the marker alone, no BIN_CONTROL: the registers' power-up zeros say GMEM
        Twin w(o);
        CHECK(has(w.fault(), "GRAS_BIN_CONTROL 0: a draw outside a sysmem pass"));
    }
}

TEST(the_fake_refuses_a_draw_whose_stages_do_not_hold_together) {
    using namespace a6xx;
    {
        Twin w;
        w.at(REG_A6XX_HLSQ_VS_CNTL) = 0;
        CHECK(has(w.fault(), "a draw with the vertex stage not enabled in both HLSQ_xS_CNTL and SP_xS_CONFIG"));
    }
    {
        Twin w;
        w.at(REG_A6XX_SP_FS_CONFIG) = 0;
        CHECK(has(w.fault(), "a draw with the fragment stage not enabled in both HLSQ_xS_CNTL and SP_xS_CONFIG"));
    }
    {
        Twin w;
        w.at(REG_A6XX_SP_HS_CONFIG) = 0x100;
        CHECK(has(w.fault(), "a geometry or tessellation stage enabled (HLSQ_HS/DS/GS_CNTL or SP_HS/DS/GS_CONFIG not 0): not served"));
    }
    {
        render::Options o;
        o.programConfig = false;
        Twin w(o);
        CHECK(has(w.fault(), "a draw with the vertex stage not enabled"));
    }
    {
        Twin w;
        w.at(REG_A6XX_SP_VS_INSTRLEN) = 0;
        CHECK(has(w.fault(), "a draw with the vertex stage's SP_xS_INSTRLEN 0"));
    }
    {
        Twin w;
        w.at(REG_A6XX_SP_FS_OBJ_START) += 32;   // 32-aligned, as the XML allows, is not the 128 the preload unit wants
        CHECK(has(w.fault(), "the fragment program at SP_xS_OBJ_START"));
        CHECK(has(w.r.fake->stats().lastFault, "outside every buffer or not 128-byte aligned"));
    }
    {
        Twin w;
        w.at(REG_A6XX_SP_VS_OBJ_START + 1) = 0x7;   // a high word that no buffer has
        CHECK(has(w.fault(), "the vertex program at SP_xS_OBJ_START 0x7"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER);
        w.words[load + 2] += 128;   // preloaded from 128 bytes further into the same page
        CHECK(has(w.fault(), "a draw without the vertex program preloaded from where SP_xS_OBJ_START points (ST6_SHADER into its own block, by its own opcode)"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_FS_SHADER);
        w.words[load + 1] = loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_FS_SHADER, 2);
        CHECK(has(w.fault(), "a preload of 2 units for a fragment program of 1"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER);
        w.words[load] = pm4::type7(CP_LOAD_STATE6_FRAG, 3);   // the VS's program by the FS's opcode
        CHECK(has(w.fault(), "CP_LOAD_STATE6_FRAG for state block 8: the vertex stage's loads go by CP_LOAD_STATE6_GEOM (tu6_stage2opcode)"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_FS_SHADER);
        w.words[load] = pm4::type7(CP_LOAD_STATE6_GEOM, 3);
        CHECK(has(w.fault(), "CP_LOAD_STATE6_GEOM for state block 12: the fragment stage's loads go by CP_LOAD_STATE6_FRAG (tu6_stage2opcode)"));
    }
    {
        Twin w;
        size_t imm = w.e.findLoad(CP_LOAD_STATE6_GEOM, ST6_CONSTANTS, SB6_VS_SHADER);
        w.words[imm + 1] = loadState6_0(8, ST6_CONSTANTS, SS6_DIRECT, SB6_VS_SHADER, 1);   // at vec4 8 of a constlen of 8
        CHECK(has(w.fault(), "vertex constants at vec4 8, 1 of them: past the stage's constlen of 8"));
    }
    {
        Twin w;
        w.at(REG_A6XX_HLSQ_FS_CNTL_0) = hlsqFsCntl0(THREAD128, false);
        CHECK(has(w.fault(), "SP_FS_CTRL_REG0 and HLSQ_FS_CNTL_0 disagree about the fragment thread size"));
    }
    {
        render::Options o;
        o.programVs = false;
        Twin w(o);
        CHECK(has(w.fault(), "a draw with the vertex stage's SP_xS_INSTRLEN 0"));
    }
}

TEST(the_fake_refuses_a_draw_whose_target_does_not_hold_together) {
    using namespace a6xx;
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 0)) = 0x7f;
        CHECK(has(w.fault(), "RB_MRT_BUF_INFO[0]: COLOR_FORMAT 127 is not one this GPU model knows"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 0)) = rbMrtBufInfo(FMT6_8_8_8_8_UNORM, a6xx_tile_mode(3), WZYX);   // TILE6_3 (a6xx.xml.h:60), which the enum here does not name
        CHECK(has(w.fault(), "RB_MRT_BUF_INFO[0]: tile mode 3: only linear targets are what this GPU model runs"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_PITCH, 0)) = 2;   // 128 bytes
        CHECK(has(w.fault(), "RB_MRT_PITCH[0] 128 bytes for rows of 64 pixels of 4 bytes: shorter than a row"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BASE, 0)) += 32;
        CHECK(has(w.fault(), "RB_MRT[0]: 64 rows (64 counted) of 256 bytes at"));
        CHECK(has(w.r.fake->stats().lastFault, "outside every buffer or not 64-byte aligned"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BASE, 0)) += 64 * 4;   // 64 rows from 256 bytes in run past the 16384-byte buffer
        CHECK(has(w.fault(), "outside every buffer or not 64-byte aligned"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BASE, 0) + 1) = 0x7;
        CHECK(has(w.fault(), "RB_MRT[0]: 64 rows (64 counted) of 256 bytes at 0x7"));
    }
    {
        Twin w;
        w.at(regSpFsMrtReg(0)) = spFsMrtReg(FMT6_32_FLOAT, false, false);
        CHECK(has(w.fault(), "SP_FS_MRT_REG[0] names format 74, RB_MRT_BUF_INFO[0] 48"));
    }
    {
        Twin w;
        w.at(REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0) = 74;
        CHECK(has(w.fault(), "GRAS_LRZ_MRT_BUF_INFO_0 names format 74, RB_MRT_BUF_INFO[0] 48"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BASE_GMEM, 0)) = 0x1000;
        CHECK(has(w.fault(), "RB_MRT_BASE_GMEM[0] is 0x1000: a sysmem draw has no GMEM base"));
    }
    {
        render::Options o;
        o.mrt = false;
        Twin w(o);
        CHECK(has(w.fault(), "RB_MRT_BUF_INFO[0]: COLOR_FORMAT 0 is not one this GPU model knows"));
    }
    // a draw into a 48x20 target whose window scissor says 128 rows: 32768 bytes of rows run past the texture's one
    // 16 KiB page (8192 bytes of layout, rounded to the phone's page by Gpu::newBo)
    {
        Rig r;
        auto small = target(r, 48, 20);
        RenderPass sp;
        sp.color0 = small.get();
        Encoded e(r, flatProgram(), sp, wholeDraw(3, 48, 20));
        CHECK(e.ok);
        std::vector<uint32_t> words = e.stream.words();
        words[e.findReg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR)] = regXY(47, 127);
        CHECK(has(faultOf(r, words), "RB_MRT[0]: 128 rows (128 counted) of 256 bytes at"));
    }
}

TEST(the_fake_refuses_a_draw_whose_fragment_output_path_is_broken) {
    using namespace a6xx;
    {
        Twin w;
        w.at(REG_A6XX_SP_FS_OUTPUT_CNTL1) = 2;
        CHECK(has(w.fault(), "SP_FS_OUTPUT_CNTL1.MRT 2 and RB_FS_OUTPUT_CNTL1.MRT 1: one target, in both"));
    }
    {
        Twin w;
        w.at(regSpFsOutputReg(0)) = spFsOutputReg(kRegidNone, false);
        CHECK(has(w.fault(), "SP_FS_OUTPUT_REG[0] names no register: the fragment stage writes no colour"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_RENDER_COMPONENTS) = 0x7;
        CHECK(has(w.fault(), "RENDER_COMPONENTS 0xf (SP) and 0x7 (RB): RT0 needs all four components in both"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)) = 0;
        CHECK(has(w.fault(), "RB_MRT_CONTROL[0] enables no component: the draw would write nothing"));
    }
    {
        render::Options o;
        o.fsOutputs = false;
        Twin w(o);
        CHECK(has(w.fault(), "SP_FS_OUTPUT_CNTL1.MRT 0 and RB_FS_OUTPUT_CNTL1.MRT 0: one target, in both"));
    }
    {
        render::Options o;
        o.blendZsa = false;
        Twin w(o);
        CHECK(has(w.fault(), "RB_MRT_CONTROL[0] enables no component"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_DEPTH_CNTL) = A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE;
        CHECK(has(w.fault(), "depth or stencil state with no depth buffer: not what this GPU model runs"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_STENCIL_CONTROL) = A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE;
        CHECK(has(w.fault(), "depth or stencil state with no depth buffer"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_DEPTH_BUFFER_INFO) = A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_32);
        CHECK(has(w.fault(), "depth or stencil state with no depth buffer"));
    }
}

TEST(the_fake_refuses_a_draw_whose_viewport_scissors_or_fetch_are_off) {
    using namespace a6xx;
    {
        Twin w;
        w.at(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL + 1) = scissorXY(64, 63);
        CHECK(has(w.fault(), "GRAS_SC_SCREEN_SCISSOR[0] (0, 0)-(64, 63) is not inside the window scissor (63, 63)"));
    }
    {
        Twin w;
        w.at(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL) = scissorXY(5, 70);
        CHECK(has(w.fault(), "GRAS_SC_VIEWPORT_SCISSOR[0] (5, 70)-(63, 63) is not inside the window scissor (63, 63)"));
    }
    {
        Twin w;
        w.at(REG_A6XX_GRAS_CL_VPORT_XOFFSET + 3) = 0;   // y scale 0
        CHECK(has(w.fault(), "GRAS_CL_VPORT[0]: a scale of 0 or a non-finite offset or scale"));
    }
    {
        Twin w;
        w.at(REG_A6XX_GRAS_CL_VPORT_XOFFSET) = 0x7fc00000;   // a NaN x offset
        CHECK(has(w.fault(), "GRAS_CL_VPORT[0]: a scale of 0 or a non-finite offset or scale"));
    }
    {
        Twin w;
        size_t gb = w.e.findReg(REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ);
        w.words[gb - 1] = pm4::type7(pm4::kNop, 1);   // the write becomes a NOP over its word
        CHECK(has(w.fault(), "GRAS_CL_GUARDBAND_CLIP_ADJ was never written"));
    }
    {
        render::Options o;
        o.viewportScissor = false;
        Twin w(o);
        CHECK(has(w.fault(), "GRAS_CL_VPORT[0]: a scale of 0"));
    }
    {
        Twin w;
        w.at(REG_A6XX_VFD_CONTROL_0) = A6XX_VFD_CONTROL_0_FETCH_CNT(1);
        CHECK(has(w.fault(), "VFD_CONTROL_0 counts 1 fetches and 0 decodes"));
    }
    {
        Twin w;
        w.at(REG_A6XX_VFD_CONTROL_0) = A6XX_VFD_CONTROL_0_FETCH_CNT(1) | A6XX_VFD_CONTROL_0_DECODE_CNT(1);
        CHECK(has(w.fault(), "vertex attribute 0 has a destination register and no VFD_DECODE_INSTR"));
    }
}

TEST(the_fake_refuses_a_draw_whose_link_or_fs_inputs_disagree) {
    using namespace a6xx;
    {
        Twin w;
        w.at(REG_A6XX_VPC_VS_PACK) = vpcVsPack(4, kLocNone);
        CHECK(has(w.fault(), "VPC_VS_PACK names no position location"));
    }
    {
        Twin w;
        w.at(REG_A6XX_PC_VS_OUT_CNTL) = pcVsOutCntl(8);
        CHECK(has(w.fault(), "VPC_VS_PACK and PC_VS_OUT_CNTL disagree about the VPC stride"));
    }
    {
        Twin w;
        w.at(REG_A6XX_SP_VS_PRIMITIVE_CNTL) = spVsPrimitiveCntl(2);
        CHECK(has(w.fault(), "SP_VS_PRIMITIVE_CNTL.OUT 2, but 1 outputs are named in SP_VS_OUT_REG"));
    }
    {
        render::Options o;
        o.vpc = false;
        Twin w(o);
        CHECK(has(w.fault(), "SP_VS_PRIMITIVE_CNTL.OUT 0, but 0 outputs are named in SP_VS_OUT_REG"));
    }
    {
        Twin w;
        w.at(REG_A6XX_HLSQ_FS_CNTL_0) = hlsqFsCntl0(THREAD64, true);
        CHECK(has(w.fault(), "the VARYING flags disagree across VPC_CNTL_0 (NUMNONPOSVAR, VARYING), SP_FS_CTRL_REG0, HLSQ_FS_CNTL_0 and RB_RENDER_CONTROL0"));
    }
    {
        Twin w;
        w.at(REG_A6XX_VPC_CNTL_0) = vpcCntl0(4, kLocNone, true, kLocNone);
        CHECK(has(w.fault(), "the VARYING flags disagree"));
    }
    {
        Twin w;
        w.at(REG_A6XX_GRAS_CNTL) = A6XX_GRAS_CNTL_IJ_PERSP_PIXEL;
        CHECK(has(w.fault(), "the barycentric and coordinate registers disagree across HLSQ_CONTROL_3/4_REG, GRAS_CNTL and RB_RENDER_CONTROL0"));
    }
    {
        Twin w;
        w.at(REG_A6XX_HLSQ_CONTROL_3_REG) = hlsqControl3Reg(0, kRegidNone, kRegidNone, kRegidNone);   // an ij regid the bits do not announce
        CHECK(has(w.fault(), "the barycentric and coordinate registers disagree"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_RENDER_CONTROL0) = A6XX_RB_RENDER_CONTROL0_COORD_MASK(0xf);
        CHECK(has(w.fault(), "the barycentric and coordinate registers disagree"));
    }
    // tri_color, whose FS reads the barycentrics: its GRAS_CNTL bit removed is the same disagreement
    {
        Rig r;
        auto t = target(r);
        RenderPass pass;
        pass.color0 = t.get();
        Encoded e(r, colorProgram(), pass, wholeDraw(6));
        CHECK(e.ok);
        std::vector<uint32_t> words = e.stream.words();
        words[e.findReg(REG_A6XX_GRAS_CNTL)] = 0;
        CHECK(has(faultOf(r, words), "the barycentric and coordinate registers disagree"));
    }
}

TEST(the_fake_refuses_a_draw_with_msaa_without_the_ccu_cntl_or_without_its_flushes) {
    using namespace a6xx;
    {
        Twin w;
        w.at(REG_A6XX_RB_DEST_MSAA_CNTL) = 0;
        CHECK(has(w.fault(), "multisampling in a DEST_MSAA_CNTL register (0): not what this GPU model runs"));
    }
    {
        Twin w;
        w.at(REG_A6XX_GRAS_DEST_MSAA_CNTL) = A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE | A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES(MSAA_FOUR);
        CHECK(has(w.fault(), "multisampling in a DEST_MSAA_CNTL register (0x6)"));
    }
    {
        render::Options o;
        o.msaaOff = false;
        Twin w(o);
        CHECK(has(w.fault(), "multisampling in a DEST_MSAA_CNTL register (0)"));
    }
    {
        render::Options o;
        o.ccuCntl = false;
        Twin w(o);
        CHECK(has(w.fault(), "RB_CCU_CNTL (0) was not written in this submission right after a CP_WAIT_FOR_IDLE with the sysmem shape"));
    }
    {
        Twin w;
        size_t ccu = w.e.findReg(REG_A6XX_RB_CCU_CNTL);
        w.words[ccu - 2] = pm4::type7(pm4::kNop, 0);   // the WFI before it, gone
        CHECK(has(w.fault(), "RB_CCU_CNTL (0x10000000) was not written in this submission right after a CP_WAIT_FOR_IDLE"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_CCU_CNTL) = A6XX_RB_CCU_CNTL_DEPTH_OFFSET(0x20000);   // the GMEM shape: depth cache not at 0, no colour offset
        CHECK(has(w.fault(), "RB_CCU_CNTL (0x20000) was not written in this submission right after a CP_WAIT_FOR_IDLE with the sysmem shape"));
    }
    {
        render::Options o;
        o.tailCcuFlush = false;
        Twin w(o);
        CHECK(has(w.fault(), "the colour stays in the CCU: no PC_CCU_FLUSH_COLOR_TS (4-word) after the draw"));
        CHECK(!w.r.fake->stats().lastDraw.ccuFlushed);
    }
    {
        render::Options o;
        o.tailCacheFlush = false;
        Twin w(o);
        CHECK(has(w.fault(), "the colour may stay in the cache: no CACHE_FLUSH_TS (4-word) after the draw"));
        CHECK(w.r.fake->stats().lastDraw.ccuFlushed && !w.r.fake->stats().lastDraw.cacheFlushed);
    }
    {
        // a 1-word PC_CCU_FLUSH_COLOR_TS (no address) is not the flush a draw needs
        Twin w;
        size_t ccu = w.e.findEvent(PC_CCU_FLUSH_COLOR_TS);
        w.words[ccu] = pm4::type7(CP_EVENT_WRITE, 1);
        w.words.erase(w.words.begin() + ccu + 2, w.words.begin() + ccu + 5);
        CHECK(has(w.fault(), "would wait forever"));   // the wait that followed reads a word nothing wrote
    }
    {
        render::Options o;
        o.tailTimestampBit = true;   // the 28 October form: a timestamp where the wait expects 1
        Twin w(o);
        CHECK(has(w.fault(), "CP_WAIT_REG_MEM would wait forever"));
    }
    // and through the command buffer: the error is the fake's
    {
        Rig r;
        auto t = target(r);
        RenderPass pass;
        pass.color0 = t.get();
        render::Options o;
        o.tailCcuFlush = false;
        auto cb = drawOnce(r, flatProgram(), pass, wholeDraw(), o);
        CHECK(cb->status() == CommandBuffer::kError);
        CHECK_EQ(r.fake->stats().faults, 1u);
        CHECK_EQ(r.fake->stats().draws, 1u);   // the draw itself held together; the submission's end did not
    }
}

TEST(the_fake_refuses_the_packets_a_draw_stream_may_not_carry) {
    using namespace a6xx;
    {
        Twin w;
        size_t ds = w.e.find(CP_SET_DRAW_STATE);
        w.words[ds + 1] = setDrawState0(4, 0, 1);   // a group of 4 dwords at an address: applied at the draw over the direct writes
        w.words[ds + 2] = uint32_t(w.e.table->gpuAddress());
        CHECK(has(w.fault(), "draw-state groups are not what this GPU model runs: registers are written directly (only CP_SET_DRAW_STATE's 3-word disable-all form is taken)"));
    }
    {
        Twin w;
        size_t m = w.e.find(CP_SET_MODE);
        w.words[m] = pm4::type7(CP_SET_MODE, 2);   // two words: the second eats the next packet's header
        w.words.insert(w.words.begin() + m + 2, 0u);
        CHECK(has(w.fault(), "opcode 0x63 of 2 words, not 1"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER);
        w.words[load] = pm4::type7(CP_LOAD_STATE6, 3);
        w.words[load + 1] = loadState6_0(0, ST6_IBO, SS6_INDIRECT, SB6_IBO, 1);
        CHECK(has(w.fault(), "a load into the 3D IBO table (SB6_IBO): no Mesa driver drives it non-bindless, not what this GPU model runs"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER);
        w.words[load + 1] = loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_CS_SHADER, 1);   // the compute block by the VS's opcode
        CHECK(has(w.fault(), "CP_LOAD_STATE6_GEOM for state block 13: the compute stage's loads go by CP_LOAD_STATE6_FRAG"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_GEOM, ST6_SHADER, SB6_VS_SHADER);
        w.words[load + 1] = loadState6_0(0, ST6_IBO, SS6_INDIRECT, SB6_VS_SHADER, 1);
        CHECK(has(w.fault(), "an IBO load into the vertex shader block: the 3D pipeline's IBOs go through SB6_IBO, which this GPU model does not run"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_FS_SHADER);
        w.words[load + 1] = loadState6_0(0, ST6_SHADER, SS6_INDIRECT, a6xx_state_block(9), 1);   // SB6_HS_SHADER
        CHECK(has(w.fault(), "CP_LOAD_STATE6_FRAG for state block 9: not one this GPU model runs"));
    }
    {
        Twin w;
        size_t load = w.e.findLoad(CP_LOAD_STATE6_FRAG, ST6_SHADER, SB6_FS_SHADER);
        w.words[load + 1] = loadState6_0(0, ST6_IBO, SS6_INDIRECT, SB6_FS_TEX, 1);
        CHECK(has(w.fault(), "a load of type 3 into the fragment texture block (SB6_FS_TEX): not samplers (ST6_SHADER) or textures (ST6_CONSTANTS)"));
    }
}

// --- the first buffer in a draw: a fragment stage's `constant float4&`, pushed into its constant file ----------------------

TEST(a_draw_pushes_a_fragment_constant_range_out_of_the_bound_buffer) {
    // tri_cflat_fs reads its colour from [[buffer(0)]]: ir3 pushes block 0, 16 dwords (a 16-byte float4 padded to 4 vec4)
    // into c0, constlen 4 -- loaded with CP_LOAD_STATE6_FRAG ST6_CONSTANTS SS6_INDIRECT into SB6_FS_SHADER at DST_OFF 0
    // straight out of the bound buffer, as compute pushes fill_id_air's c
    Rig r;
    auto t = target(r);
    fillWords(*t, 0xa5a5a5a5u);
    RenderPass pass;
    pass.color0 = t.get();
    const Program p{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
    CHECK_EQ(p.fs.k.numUboRanges, 1u);
    CHECK_EQ(p.fs.k.uboRanges[0].sizeDwords, 16u);
    std::string why;
    auto colour = newBuffer(*r.gpu, 16, &why);
    CHECK(colour != nullptr);
    Bindings fsb;
    fsb.buffers = {Binding{colour.get(), 0}};
    Encoded e(r, p, pass, wholeDraw(), render::Options{}, Bindings{}, fsb);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    size_t at = e.findLoad(a6xx::CP_LOAD_STATE6_FRAG, a6xx::ST6_CONSTANTS, a6xx::SB6_FS_SHADER);
    CHECK(at != size_t(-1));
    if (at != size_t(-1)) {
        const auto& w = e.stream.words();
        uint32_t w0 = w[at + 1];
        CHECK_EQ((w0 & a6xx::CP_LOAD_STATE6_0_STATE_SRC__MASK) >> a6xx::CP_LOAD_STATE6_0_STATE_SRC__SHIFT, uint32_t(a6xx::SS6_INDIRECT));
        CHECK_EQ((w0 & a6xx::CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> a6xx::CP_LOAD_STATE6_0_NUM_UNIT__SHIFT, 4u);
        CHECK_EQ((w0 & a6xx::CP_LOAD_STATE6_0_DST_OFF__MASK) >> a6xx::CP_LOAD_STATE6_0_DST_OFF__SHIFT, 0u);
        CHECK_EQ(uint64_t(w[at + 2]) | uint64_t(w[at + 3]) << 32, colour->gpuAddress());
    }
    // the vertex stage's table is empty and its stage pushes nothing indirect
    CHECK(e.findLoad(a6xx::CP_LOAD_STATE6_GEOM, a6xx::ST6_CONSTANTS, a6xx::SB6_VS_SHADER) != size_t(-1));   // (its immediates, direct)
    // the whole draw on the fake: the load is inside the bound buffer and within the decoded CONSTLEN
    auto cb = drawOnce(r, p, pass, wholeDraw(), render::Options{}, Bindings{}, fsb);
    CHECK(cb->error().empty());
    if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
    CHECK(cb->status() == CommandBuffer::kCompleted);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK_EQ(s.draws, 1u);
    CHECK_EQ(s.lastDraw.fsConstlen, 4u);
    CHECK_EQ(s.lastDraw.fsConsts.size(), size_t(1));
    if (!s.lastDraw.fsConsts.empty()) {
        CHECK_EQ(s.lastDraw.fsConsts[0].units, 4u);
        CHECK(!s.lastDraw.fsConsts[0].direct);
    }
}

TEST(a_draw_refuses_a_constant_range_it_cannot_serve_by_name) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    const Program p{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
    std::string why;
    auto colour = newBuffer(*r.gpu, 16, &why);
    auto refusedWith = [&](const Program& q, const Bindings& fsb, const char* want) {
        Encoded e(r, q, pass, wholeDraw(), render::Options{}, Bindings{}, fsb);
        CHECK(!e.ok);
        expectRefusal(e.error, want);
        CHECK_EQ(e.stream.size(), size_t(0));
    };
    // nothing bound at the range's block
    refusedWith(p, Bindings{}, "tri_cflat_fs_air: no buffer bound at index 0 (its constant buffer)");
    // a binding at an offset that is not a word
    Bindings odd;
    odd.buffers = {Binding{colour.get(), 2}};
    refusedWith(p, odd, "tri_cflat_fs_air: buffer 0: offset 2 is not a multiple of 4 inside it");
    // the padded 64-byte range running off the object's pages: a buffer as long as its pages, bound 16 bytes before the end
    auto page = newBuffer(*r.gpu, colour->bo().size(), &why);
    Bindings tail;
    tail.buffers = {Binding{page.get(), page->bo().size() - 16}};
    refusedWith(p, tail, "tri_cflat_fs_air reads 64 bytes at 0 of constant buffer 0; 16 are bound and 16 mapped behind the offset");
    // a range past the stage's constlen (the twin of the constlen the fixture carries)
    Program short_ = p;
    short_.fs.k.constlen = 0;
    Bindings ok;
    ok.buffers = {Binding{colour.get(), 0}};
    refusedWith(short_, ok, "tri_cflat_fs_air: constant buffer 0's range at vec4 0 runs past its constlen of 0");
    CHECK_EQ(r.fake->stats().submits, 0u);
}

// --- the first vertex fetch: tri_attr_vs's two [[stage_in]] attributes through a vertex descriptor ------------------------

namespace {
// one interleaved buffer of 12-byte vertices: Float2 position at 0, UChar4Normalized colour at 8 ([[buffer(0)]])
Program attrProgram() {
    Program p{kernels::tri_attr_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_attr"};
    p.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
    p.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
    p.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
    return p;
}
}  // namespace

TEST(a_draw_fetches_two_attributes_through_the_vertex_descriptor) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    const Program p = attrProgram();
    CHECK_EQ(p.vs.numAttributes, 2u);
    std::string why;
    auto vb = newBuffer(*r.gpu, 6 * 12, &why);
    CHECK(vb != nullptr);
    Bindings vsb;
    vsb.buffers = {Binding{vb.get(), 0}};
    Encoded e(r, p, pass, wholeDraw(6), render::Options{}, vsb, Bindings{});
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    using namespace a6xx;
    const auto& w = e.stream.words();
    auto value = [&](uint32_t reg) { size_t i = e.findLastReg(reg); CHECK(i != size_t(-1)); return i == size_t(-1) ? 0u : w[i]; };
    // two attributes: both counts 2 (turnip's attr_count, tu_shader.cc:1592-1595)
    CHECK_EQ(value(REG_A6XX_VFD_CONTROL_0), A6XX_VFD_CONTROL_0_FETCH_CNT(2) | A6XX_VFD_CONTROL_0_DECODE_CNT(2));
    // decode 0: buffer 0, offset 0, FMT6_32_32_FLOAT 103, WZYX, UNK30, FLOAT = 0xc6700000 (a6xx_test's literal)
    CHECK_EQ(value(regVfdDecodeInstr(0)), 0xc6700000u);
    // decode 1: buffer 0, offset 8, FMT6_8_8_8_8_UNORM 48, FLOAT: 0xc3000100
    CHECK_EQ(value(regVfdDecodeInstr(1)), 0xc3000100u);
    // destinations: r0.x xy (0x3 | 0 << 4) and r1.x xyzw (0xf | 4 << 4), from the fixture's inputs
    CHECK_EQ(value(regVfdDestCntlInstr(0)), vfdDestCntlInstr(0x3, regid(0, 0)));
    CHECK_EQ(value(regVfdDestCntlInstr(1)), vfdDestCntlInstr(0xf, regid(1, 0)));
    // fetch 0 = the buffer: base, its 72 bytes, stride 12
    CHECK_EQ(uint64_t(value(regVfdFetchBase(0))) | uint64_t(w[e.findLastReg(regVfdFetchBase(0)) + 1]) << 32, vb->gpuAddress());
    CHECK_EQ(value(vfdFetchSize(0)), 72u);
    CHECK_EQ(value(regVfdFetchStride(0)), 12u);
    // the whole draw on the fake
    auto cb = drawOnce(r, p, pass, wholeDraw(6), render::Options{}, vsb, Bindings{});
    CHECK(cb->error().empty());
    if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().faults, 0u);
    CHECK_EQ(r.fake->stats().lastDraw.vfdDecodeCount, 2u);
    // the twin: the fetch's size cut below the sixth vertex's colour (5 * 12 + 8 + 4 = 72 bytes): the fake refuses it
    std::vector<uint32_t> cut = w;
    cut[e.findLastReg(vfdFetchSize(0))] = 71;
    CHECK(has(faultOf(r, cut), "vertex attribute 1: vertex 5's element ends at byte 72 of fetch 0, whose size is 71"));
    // and a decode naming a format the model does not know
    std::vector<uint32_t> fmt = w;
    fmt[e.findLastReg(regVfdDecodeInstr(0))] = vfdDecodeInstr(0, 0, false, FMT6_32_UINT, WZYX, false);
    CHECK(has(faultOf(r, fmt), "vertex attribute 0: VFD_DECODE_INSTR.FORMAT is not a vertex format this model knows"));
}

TEST(a_draw_refuses_a_vertex_fetch_it_cannot_serve_by_name) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    std::string why;
    auto vb = newBuffer(*r.gpu, 6 * 12, &why);
    Bindings vsb;
    vsb.buffers = {Binding{vb.get(), 0}};
    auto refusedWith = [&](const Program& q, const Bindings& b, uint32_t vertexCount, const char* want) {
        Encoded e(r, q, pass, wholeDraw(vertexCount), render::Options{}, b, Bindings{});
        CHECK(!e.ok);
        expectRefusal(e.error, want);
        CHECK_EQ(e.stream.size(), size_t(0));
    };
    Program p = attrProgram();
    p.vertexDescriptor.attributes[1] = VertexAttribute{};
    refusedWith(p, vsb, 6, "tri_attr_vs_air reads [[attribute(1)]], which the vertex descriptor does not describe");
    p = attrProgram();
    p.vertexDescriptor.attributes[1].format = VertexFormat(41);   // UInt1010102Normalized
    refusedWith(p, vsb, 6, "tri_attr_vs_air: [[attribute(1)]] is MTLVertexFormat 41: Float..Float4, Half2, Half4 and UChar4Normalized are served");
    p = attrProgram();
    p.vertexDescriptor.layouts[0].step = VertexStep::kPerInstance;
    refusedWith(p, vsb, 6, "tri_attr_vs_air: buffer 0 steps per instance: only per-vertex fetch is served yet");
    p = attrProgram();
    p.vertexDescriptor.layouts[0].stride = 0;
    refusedWith(p, vsb, 6, "tri_attr_vs_air: buffer 0's stride of 0: 1 to 4095 bytes (VFD_FETCH_STRIDE holds 12 bits)");
    p = attrProgram();
    p.vertexDescriptor.attributes[0].offset = 4096;
    refusedWith(p, vsb, 6, "tri_attr_vs_air: [[attribute(0)]] at offset 4096: VFD_DECODE_INSTR.OFFSET holds 12 bits");
    refusedWith(attrProgram(), Bindings{}, 6, "tri_attr_vs_air: no buffer bound at index 0 (the vertex buffer of [[attribute(0)]])");
    // seven vertices over six vertices' bytes: the seventh's position ends at byte 6 * 12 + 8 = 80 of 72
    refusedWith(attrProgram(), vsb, 7, "tri_attr_vs_air: [[attribute(0)]] of vertex 6 ends at byte 80 of buffer 0; 72 are bound");
    CHECK_EQ(r.fake->stats().submits, 0u);
}

