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
#include <map>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"
#include "fixtures/end_only.hpp"
#include "fixtures/tri_attr_vs_air.hpp"
#include "fixtures/tri_cflat_fs_air.hpp"
#include "fixtures/tri_tex_fs_air.hpp"
#include "fixtures/tri_null_fs_air.hpp"
#include "fixtures/tri_mrt_fs_air.hpp"
#include "fixtures/tri_fb_fs_air.hpp"
#include "fixtures/tri_mrt1_fs_air.hpp"
#include "fixtures/tri_upos_vs_air.hpp"
#include "fixtures/tri_dpos_vs_air.hpp"
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_vtex_vs_air.hpp"
#include "fixtures/tri_shadow_fs_air.hpp"
#include "fixtures/tri_arr_vs_air.hpp"
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
        // longer than the instruction cache: drawn, SP_FS_INSTRLEN the whole program, the preload capped at 127 units
        // (the fake faults on a preload of 128, which hangs this GPU, and on one longer than INSTRLEN); the rest is
        // fetched from SP_FS_OBJ_START as the shader runs, which is what turnip and Gallium leave to the hardware too
        {
            Encoded e(r, p, pass, d);
            CHECK(e.ok);
            if (!e.ok) std::printf("       %s\n", e.error.c_str());
            CHECK_EQ(e.reg(a6xx::REG_A6XX_SP_FS_INSTRLEN), 128u);
        }
        p = handProgram();
        p.fs.k.numSamp = 32;
        refused(r, p, pass, d, "hand_fs: numSamp 32 does not fit SP_xS_CONFIG.NSAMP's 5 bits (31 at most)");
        p = handProgram();
        p.vs.k.numSsbos = 1;   // a device buffer: the 3D IBO table, refused here for want of the buffer
        p.vs.k.numIbos = 1;
        refused(r, p, pass, d, "hand_vs: no buffer bound at index 0 (a device buffer it addresses)");
        p.vs.k.numIbos = 2;    // an image beside it
        refused(r, p, pass, d, "hand_vs has 2 IBO slots for 1 device buffer(s): images in a draw are not served");
        p = handProgram();
        p.fs.k.numUbos = 1;   // an indexed constant buffer: served through a UBO descriptor, refused here for want of the buffer
        refused(r, p, pass, d, "hand_fs: no buffer bound at index 0 (a constant buffer it indexes)");
        p = handProgram();
        p.fs.k.numUboRanges = 1;   // a pushed range, served -- but nothing is bound at its block
        refused(r, p, pass, d, "hand_fs: no buffer bound at index 0 (its constant buffer)");
        p = handProgram();
        p.fs.k.textures[0].kind = kTexSampled;   // ...and numTextureSlots left at 0
        refused(r, p, pass, d, "hand_fs samples texture 0 past its numTextureSlots of 0: the stage's tables disagree");
        p.fs.k.numTextureSlots = 1;
        // nothing bound at a declared slot: the encoder alone refuses it, and a command buffer binds the queue's null
        // texture there (an uber shader declares slots a draw never reaches) and draws
        {
            Encoded e(r, p, pass, d);
            CHECK(!e.ok);
            expectRefusal(e.error, "hand_fs: no texture bound at index 0");
            auto cb = r.queue->commandBuffer();
            CHECK(cb->draw(p, pass, d, Bindings{}, Bindings{}));
            if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
        }
        p = handProgram();
        p.vs.k.textures[0].kind = kTexSampled;
        p.vs.k.numTextureSlots = 1;
        // the VS's textures are served as the FS's (SB6_VS_TEX): the same refusal of an empty slot by the encoder, and the
        // same null texture from a command buffer, which then loads one descriptor into the vertex block
        {
            Encoded e(r, p, pass, d);
            CHECK(!e.ok);
            expectRefusal(e.error, "hand_vs: no texture bound at index 0");
            Rig r2;   // a rig of its own: this one submits, and the refusals around it count on none
            auto t2 = target(r2);
            RenderPass pass2 = pass;
            pass2.color0 = t2.get();
            auto drawOnce = [&](const Program& q) {
                auto cb = r2.queue->commandBuffer();
                CHECK(cb->draw(q, pass2, d, Bindings{}, Bindings{}));
                cb->commit();
                cb->waitUntilCompleted();
            };
            // the twin first: the hand stage's SP_VS_CONFIG says no texture (numSamp 0) while one is loaded, which the
            // fake must call out -- else its VS rule judges nothing
            drawOnce(p);
            CHECK_EQ(r2.fake->stats().faults, 1u);
            expectRefusal(r2.fake->stats().lastFault, "SP_VS_CONFIG names 0 textures and 0 samplers; SP_VS_TEX_COUNT says 1 (NTEX must cover it and equal NSAMP)");
            p.vs.k.numSamp = 1;
            p.vs.k.samplerMask = 1;
            drawOnce(p);
            const auto& st = r2.fake->stats();
            if (st.faults != 1) std::printf("       fake: %s\n", st.lastFault.c_str());
            CHECK_EQ(st.faults, 1u);
            CHECK_EQ(st.submits, 2u);
            CHECK_EQ(st.lastDraw.vsTextureCount, 1u);
            CHECK_EQ(st.lastDraw.vsSamplerCount, 1u);
            CHECK_EQ(st.lastDraw.fsTextureCount, 0u);
        }
        p = handProgram();
        p.fs.k.textures[2].kind = kTexImage;
        refused(r, p, pass, d, "hand_fs stores through texture 2 as an image: images in a draw (the 3D IBO table) are not served");
        p = handProgram();
        p.fs.k.samplerMask = 1u << 16;
        refused(r, p, pass, d, "hand_fs names texture or sampler indices past 15: the tables hold 16");
        p = handProgram();
        p.fs.k.driverParamsOffsetVec4 = 0;
        refused(r, p, pass, d, "hand_fs reads driver params at vec4 0: only a vertex stage's are served");
        p = handProgram();
        p.vs.k.driverParamsOffsetVec4 = 8;
        refused(r, p, pass, d, "hand_vs: its driver params at vec4 8 run past its constlen of 8");
        p = handProgram();
        p.fs.k.constantDataSize = 16;   // with no UBO of ir3's to read it through (constantDataUbo 0): nothing could read it
        refused(r, p, pass, d, "hand_fs carries 16 bytes of constant data that no UBO of ir3's reads, or outside its words");
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
        // a point size: linked after the position (VPC_VS_PACK.PSIZELOC, PC_VS_OUT_CNTL.PSIZE) whatever is drawn, and a
        // point draw is a point list of that size (CoreAnimation's fixed_vert_lph_gen writes one into its triangles too)
        {
            using namespace a6xx;
            p.link = Linkage{};
            std::string lwhy;
            CHECK(render::link(p.vs, p.fs, &p.link, &lwhy));
            CHECK(p.link.psizeLoc != 0xffu);
            Draw points = d;
            points.primitive = Primitive::kPoint;
            Encoded e(r, p, pass, points);
            CHECK(e.ok);
            if (!e.ok) std::printf("       %s\n", e.error.c_str());
            CHECK_EQ((e.reg(REG_A6XX_VPC_VS_PACK) & A6XX_VPC_VS_PACK_PSIZELOC__MASK) >> A6XX_VPC_VS_PACK_PSIZELOC__SHIFT, p.link.psizeLoc);
            CHECK(e.reg(REG_A6XX_PC_VS_OUT_CNTL) & A6XX_PC_VS_OUT_CNTL_PSIZE);
            const size_t at = e.find(CP_DRAW_INDX_OFFSET);
            CHECK(at != size_t(-1));
            if (at != size_t(-1)) CHECK_EQ(e.stream.words()[at + 1] & 0x3fu, uint32_t(DI_PT_POINTLIST));
        }
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
        // [[color(1)]] with no attachment 1, and [[color(2)]] with nothing at 1 or 2: drawn, the output written nowhere
        // (Metal discards an output whose pipeline slot has no format); a_colour_output_with_no_attachment_is_written_nowhere
        // has the registers
        {
            Encoded e(r, p, pass, d);
            CHECK(e.ok);
        }
        p.fs.numColorOutputs = 3;
        p.fs.colorRegid[1] = a6xx::kRegidNone;
        p.fs.colorRegid[2] = regid(1, 0);
        {
            Encoded e(r, p, pass, d);
            CHECK(e.ok);
        }
        p = handProgram();
        p.fs.numColorOutputs = 9;
        refused(r, p, pass, d, "hand_fs writes 9 colour output(s): 8 at most are served");
        // a stage writing depth is drawn: into a pass with no depth attachment its depth goes nowhere (no DEPTH_REGID,
        // no FRAG_WRITES_Z), and with one it is the depth the attachment takes, after the stage (LATE_Z)
        p = handProgram();
        p.fs.depthRegid = regid(1, 0);
        {
            using namespace a6xx;
            Encoded e(r, p, pass, d);
            CHECK(e.ok);
            CHECK_EQ((e.reg(REG_A6XX_SP_FS_OUTPUT_CNTL0) & A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__MASK) >> A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__SHIFT,
                     uint32_t(kRegidNone));
            CHECK_EQ(e.reg(REG_A6XX_RB_FS_OUTPUT_CNTL0) & A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_Z, 0u);
            TextureDesc zd;
            zd.format = PixelFormat::kDepth32Float;
            zd.width = 64;
            zd.height = 64;
            zd.usage = kTextureUsageRenderTarget;
            std::string zwhy;
            auto z = Texture::make(*r.gpu, zd, &zwhy);
            CHECK(z != nullptr);
            if (z != nullptr) {
                RenderPass zp = pass;
                zp.depth = z.get();
                Encoded ez(r, p, zp, d);
                CHECK(ez.ok);
                CHECK_EQ((ez.reg(REG_A6XX_SP_FS_OUTPUT_CNTL0) & A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__MASK) >> A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__SHIFT,
                         uint32_t(regid(1, 0)));
                CHECK(ez.reg(REG_A6XX_RB_FS_OUTPUT_CNTL0) & A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_Z);
                CHECK_EQ(ez.reg(REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL), A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE(A6XX_LATE_Z));
            }
        }
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
        p.fs.fbRead = 1;   // a stage that reads its target with no fetch slot of its own: it gets the barrier, nothing to refuse
        {
            Encoded fb(r, p, pass, d);
            CHECK(fb.ok);
        }
        p = handProgram();
        p.fs.dualSrcBlend = 1;   // served: its second colour (DATA1, register r1.x here) is the blend's second source
        p.fs.numColorOutputs = 2;
        p.fs.colorRegid[1] = regid(1, 0);
        {
            using namespace a6xx;
            Encoded e(r, p, pass, d);
            CHECK(e.ok);
            CHECK(e.reg(REG_A6XX_SP_FS_OUTPUT_CNTL0) & A6XX_SP_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE);
            CHECK(e.reg(REG_A6XX_RB_FS_OUTPUT_CNTL0) & A6XX_RB_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE);
            CHECK(e.reg(REG_A6XX_SP_BLEND_CNTL) & A6XX_SP_BLEND_CNTL_DUAL_COLOR_IN_ENABLE);
            CHECK(e.reg(REG_A6XX_RB_BLEND_CNTL) & A6XX_RB_BLEND_CNTL_DUAL_COLOR_IN_ENABLE);
            CHECK_EQ(e.reg(REG_A6XX_SP_FS_OUTPUT_CNTL1), A6XX_SP_FS_OUTPUT_CNTL1_MRT(1));      // one attachment
            CHECK_EQ(e.reg(regSpFsOutputReg(1)) & 0xff, uint32_t(regid(1, 0)));      // and the second source's register
        }
        p = handProgram();
        p.fs.readsPrimid = 1;
        refused(r, p, pass, d, "hand_fs reads the primitive id: not served");
        p = handProgram();
        p.fs.numSamplerPrefetch = 1;
        refused(r, p, pass, d, "hand_fs prefetches a texture and loads none: ir3's tables disagree");
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
        refused(r, p, none, d, "a render pass with no attachment at all: nothing to draw into");
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
        RenderPass nan = pass;
        nan.load = LoadAction::kClear;
        nan.clearColor[1] = std::nan("");
        refused(r, p, nan, d, "a clear colour with a non-finite channel (channel 1) into RGBA8Unorm");
        RenderPass odd = pass;
        odd.load = LoadAction(7);
        refused(r, p, odd, d, "load action 7: MTLLoadAction is DontCare (0), Load (1) or Clear (2)");
        RenderPass dontStore = pass;   // DontCare is served (stored anyway, in sysmem); another number is not
        dontStore.store = StoreAction::kDontCare;
        Encoded dc(r, p, dontStore, d);
        CHECK(dc.ok);
        dontStore.store = StoreAction(5);
        refused(r, p, dontStore, d, "store action 5: Store (1) and DontCare (0) are served");
    }
    // the draw
    {
        Program p = handProgram();
        Draw x = d;
        x.primitive = Primitive(7);
        refused(r, p, pass, x, "primitive 7: MTLPrimitiveType is Point (0), Line (1), LineStrip (2), Triangle (3) or TriangleStrip (4)");
        x = d;
        x.vertexCount = 0;
        refused(r, p, pass, x, "a draw of 0 vertices");
        x = d;
        x.instanceCount = 0;
        refused(r, p, pass, x, "a draw of 0 instances");
        x = d;
        x.viewport.width = NAN;
        refused(r, p, pass, x, "a viewport with a non-finite value");
        x = d;
        x.viewport.width = 0;
        refused(r, p, pass, x, "a viewport of 0 x 64 pixels: both sides must be positive");
        x = d;
        x.viewport.x = 64;   // past the window altogether (one that only reaches past it is drawn, clipped: below)
        refused(r, p, pass, x, "viewport (64.000000, 0.000000, 64.000000, 64.000000) covers no pixel of the 64 x 64 target");
        x = d;
        x.viewport.y = -64;
        refused(r, p, pass, x, "viewport (0.000000, -64.000000, 64.000000, 64.000000) covers no pixel of the 64 x 64 target");
        {
            using namespace a6xx;
            x = d;
            x.viewport.x = -8;
            x.viewport.y = 4;
            x.viewport.width = 100;   // (the window 64 x 64: the viewport scissor clamped to (0, 4)-(63, 63))
            Encoded e(r, p, pass, x);
            CHECK(e.ok);
            CHECK_EQ(e.reg(regGrasScViewportScissor(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL, 0)), scissorXY(0, 4));
            CHECK_EQ(e.reg(regGrasScViewportScissor(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL, 0) + 1), scissorXY(63, 63));
        }
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
    for (uint32_t i = 0; i < render::kTableZeroedBytes / 4; i++) CHECK_EQ(tw[render::kTableTimestampsAt / 4 + i], 0u);   // the tail's, the depth ones, the Clears'
    for (uint32_t i = 0; i < 32; i++) CHECK_EQ(tw[render::kTableBorderColorAt / 4 + i], 0u);
    CHECK_EQ(tw[render::kTableZeroedBytes / 4], 0xeeeeeeeeu);
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
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_CNTL), 0x2014u);   // half-pixel lines, and FRONT_CW: Metal's default Clockwise front (probe 9cu)
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
    render::Options sysmem;
    sysmem.gmem = false;   // (the window checked is a sysmem pass's: a tile's is its bin)
    auto sm = drawOnce(r, flatProgram(), sp, wholeDraw(3, 48, 20), sysmem);
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
        // the 7-word form pointing at no index buffer: outside every buffer
        Twin w;
        size_t dr = w.e.find(CP_DRAW_INDX_OFFSET);
        std::vector<uint32_t> seven = {drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_DMA, IGNORE_VISIBILITY) | CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(INDEX4_SIZE_16_BIT),
                                       1, 3, 0, 0, 0, 3};
        w.words.erase(w.words.begin() + dr, w.words.begin() + dr + 4);
        w.words.insert(w.words.begin() + dr, pm4::type7(CP_DRAW_INDX_OFFSET, 7));
        w.words.insert(w.words.begin() + dr + 1, seven.begin(), seven.end());
        CHECK(has(w.fault(), "an indexed draw of 3 indices from 0, MAX_INDICES 3, FIRST_INDX 0: outside every buffer, misaligned, or more than the buffer holds"));
    }
    {
        Twin w;
        size_t dr = w.e.find(CP_DRAW_INDX_OFFSET);
        w.words.erase(w.words.begin() + dr, w.words.begin() + dr + 4);
        std::vector<uint32_t> five = {pm4::type7(CP_DRAW_INDX_OFFSET, 5), drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_DMA, IGNORE_VISIBILITY), 1, 3, 0, 0};
        w.words.insert(w.words.begin() + dr, five.begin(), five.end());
        CHECK(has(w.fault(), "CP_DRAW_INDX_OFFSET of 5 words"));
    }
    {
        Twin w;
        w.words[w.e.find(CP_DRAW_INDX_OFFSET) + 1] = drawIndxOffset0(DI_PT_POINTLIST, DI_SRC_SEL_AUTO_INDEX, IGNORE_VISIBILITY);
        CHECK(has(w.fault(), "a draw of primitive type 9: triangle lists (4) and strips (6) and line lists (2) and strips (3) are what this GPU model runs"));
    }
    {
        Twin w;
        w.words[w.e.find(CP_DRAW_INDX_OFFSET) + 1] = drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_DMA, IGNORE_VISIBILITY);
        CHECK(has(w.fault(), "a draw whose indices come from source 0 in a 3-word packet: AUTO_INDEX (2) takes 3 words, DMA (0) 7"));
    }
    {
        Twin w;
        w.words[w.e.find(CP_DRAW_INDX_OFFSET) + 2] = 0;
        CHECK(has(w.fault(), "a draw of 0 instances"));
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
        CHECK(has(w.fault(), "RB_MRT_PITCH[0] 128 bytes for rows of 64 pixels of 4 bytes and 1 sample(s): shorter than a row"));
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
        CHECK(has(w.fault(), "SP_FS_MRT_REG[0] names format 74, RB_MRT_BUF_INFO 48"));
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
        CHECK(has(w.fault(), "SP_FS_OUTPUT_CNTL1.MRT 2 and RB_FS_OUTPUT_CNTL1.MRT 1: 1 to 8 targets, the same in both"));
    }
    {
        Twin w;
        // no register and still four components: the RB would write what an unset register holds
        w.at(regSpFsOutputReg(0)) = spFsOutputReg(kRegidNone, false);
        CHECK(has(w.fault(), "RENDER_COMPONENTS 0xf (SP) and 0xf (RB): RT0 is written by no register and must name no component"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_RENDER_COMPONENTS) = 0x7;
        CHECK(has(w.fault(), "RENDER_COMPONENTS 0xf (SP) and 0x7 (RB): RT0 needs all four components in both"));
    }
    {
        // a write mask of none is Metal's to ask for (MTLColorWriteMaskNone): the draw writes nothing, and is no fault
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)) = 0;
        Raw raw(w.r);
        CHECK_EQ(raw.submit(w.words), 0);
        CHECK_EQ(w.r.fake->stats().lastDraw.mrtControl, 0u);
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)) |= A6XX_RB_MRT_CONTROL_BLEND;   // BLEND without BLEND2, and the two ENABLE_BLENDs off
        CHECK(has(w.fault(), "the blend of target 0 is on in some of RB_MRT_CONTROL (0x781: BLEND and BLEND2)"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)) |= A6XX_RB_MRT_CONTROL_BLEND | A6XX_RB_MRT_CONTROL_BLEND2;
        w.at(REG_A6XX_SP_BLEND_CNTL) |= 1;
        w.at(REG_A6XX_RB_BLEND_CNTL) |= 1;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, 0)) = A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(adreno_rb_blend_factor(2));
        CHECK(has(w.fault(), "names blend factor 2, which adreno_rb_blend_factor does not have"));
    }
    {
        Twin w;
        w.at(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)) |= A6XX_RB_MRT_CONTROL_BLEND | A6XX_RB_MRT_CONTROL_BLEND2;
        w.at(REG_A6XX_SP_BLEND_CNTL) |= 1;
        w.at(REG_A6XX_RB_BLEND_CNTL) |= 1;
        w.at(regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, 0)) = A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(a3xx_rb_blend_opcode(5)) |
                                                          A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(FACTOR_ONE) | A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(FACTOR_ONE);
        CHECK(has(w.fault(), "names a blend opcode past BLEND_MAX_DST_SRC (4)"));
    }
    {
        render::Options o;
        o.fsOutputs = false;
        Twin w(o);
        CHECK(has(w.fault(), "SP_FS_OUTPUT_CNTL1.MRT 0 and RB_FS_OUTPUT_CNTL1.MRT 0: 1 to 8 targets, the same in both"));
    }
    {
        render::Options o;
        o.blendZsa = false;
        Twin w(o);
        CHECK(has(w.fault(), "blend register 0x8820 not written in this submission: the draw's write mask and blend would be the last owner's"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_DEPTH_CNTL) = A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE;
        CHECK(has(w.fault(), "a depth test or write with no depth buffer (RB_DEPTH_BUFFER_INFO DEPTH6_NONE)"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_STENCIL_CONTROL) = A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE;   // the enable in one copy only
        CHECK(has(w.fault(), "the stencil enable must agree"));
    }
    {
        Twin w;
        w.at(REG_A6XX_RB_STENCIL_CONTROL) = A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE;
        w.at(REG_A6XX_GRAS_SU_STENCIL_CNTL) = A6XX_GRAS_SU_STENCIL_CNTL_STENCIL_ENABLE;   // both, and no stencil buffer
        CHECK(has(w.fault(), "stencil state with no separate stencil buffer beside a depth buffer"));
    }
    {
        // a depth format with nothing behind it: GRAS_SU's copy still says none
        Twin w;
        w.at(REG_A6XX_RB_DEPTH_BUFFER_INFO) = A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_32);
        CHECK(has(w.fault(), "depth format 4 in RB_DEPTH_BUFFER_INFO and 0 in GRAS_SU_DEPTH_BUFFER_INFO"));
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
        Twin w;   // one block's DEST without MSAA_DISABLE at one sample: the blocks disagree
        w.at(REG_A6XX_RB_DEST_MSAA_CNTL) = 0;
        CHECK(has(w.fault(), "the MSAA registers disagree (0 against GRAS_DEST_MSAA_CNTL's count 0)"));
    }
    {
        Twin w;   // GRAS alone at four samples: the others still say one
        w.at(REG_A6XX_GRAS_DEST_MSAA_CNTL) = A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE | A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES(MSAA_FOUR);
        CHECK(has(w.fault(), "the MSAA registers disagree"));
    }
    {
        render::Options o;
        o.msaaOff = false;   // none of them written: zeros, MSAA_DISABLE missing at one sample
        Twin w(o);
        CHECK(has(w.fault(), "the MSAA registers disagree (0 against GRAS_DEST_MSAA_CNTL's count 0)"));
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
        // ST6_IBO into SB6_IBO: the 3D table takes ST6_SHADER, as both witnesses load it
        CHECK(has(w.fault(), "CP_LOAD_STATE6 into SB6_IBO of type 3, source 2: the plain CP_LOAD_STATE6 of ST6_SHADER from memory is what this GPU model runs"));
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

// A pushed range whose source is not a multiple of 16 is loaded direct, the words as the buffer holds them at encoding:
// the CP rounds an indirect source down to 16 (probe 9o4), and the fake faults on one, so the draw completing is the
// direct load having been taken. The same range at 16 stays an indirect load.
TEST(a_pushed_range_off_16_bytes_is_loaded_direct) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    const Program p{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
    std::string why;
    auto b = newBuffer(*r.gpu, 4096, &why);
    float* f = static_cast<float*>(b->contents());
    for (int i = 0; i < 64; i++) f[i] = float(i);
    for (uint32_t off : {8u, 16u}) {
        Bindings fsb;
        fsb.buffers = {Binding{b.get(), off}};
        Encoded e(r, p, pass, wholeDraw(3), render::Options{}, Bindings{}, fsb);
        CHECK(e.ok);
        size_t at = e.findLoad(CP_LOAD_STATE6_FRAG, ST6_CONSTANTS, SB6_FS_SHADER);
        CHECK(at != size_t(-1));
        if (at != size_t(-1)) {
            const auto& w = e.stream.words();
            const bool direct = ((w[at + 1] & CP_LOAD_STATE6_0_STATE_SRC__MASK) >> CP_LOAD_STATE6_0_STATE_SRC__SHIFT) == SS6_DIRECT;
            CHECK_EQ(direct, off == 8);
            if (direct) {
                float first;
                memcpy(&first, &w[at + 4], 4);
                CHECK_EQ(first, 2.0f);   // the float at byte 8
            }
        }
        auto cb = drawOnce(r, p, pass, wholeDraw(3), render::Options{}, Bindings{}, fsb);
        CHECK(cb->status() == CommandBuffer::kCompleted);
        if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
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

// A VS that reads driver params ([[base_vertex]], or [[instance_id]], which the front end makes the hardware's id plus
// base_instance) gets turnip's one vec4 every draw: draw id 0, the vertex offset VFD_INDEX_OFFSET also adds, the first
// instance VFD_INSTANCE_START_OFFSET also holds, 0. The values are made distinct so a swapped pair or a vertex offset
// taken from the wrong kind of draw shows.
// An FS that writes no colour (CoreAnimation's path_edges_fragment, drawn for its stencil) and one that counts a slot it
// never writes are both drawn as one slot with nothing to write: RB_MRT_CONTROL[0] 0, so attachment 0 keeps its bytes.
TEST(a_fragment_stage_that_writes_no_colour_is_drawn_with_nothing_written) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    for (uint32_t outputs : {0u, 1u}) {
        Program p = handProgram();
        p.fs.numColorOutputs = outputs;
        p.fs.colorRegid[0] = a6xx::kRegidNone;
        Encoded e(r, p, pass, wholeDraw(3));
        CHECK(e.ok);
        if (!e.ok) std::printf("       %s\n", e.error.c_str());
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0u);
        CHECK_EQ(e.reg(REG_A6XX_RB_FS_OUTPUT_CNTL1), A6XX_RB_FS_OUTPUT_CNTL1_MRT(1));
        auto cb = drawOnce(r, p, pass, wholeDraw(3));
        CHECK(cb->status() == CommandBuffer::kCompleted);
        if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    }
}

// Private memory: encoded alone, a stage that wants some and is given none is refused by name, and one given too little
// or the other layout too; through a command buffer the queue's buffer is bound, its address, MEMSIZEPERITEM (512 >> 9),
// TOTALPVTMEMSIZE (512 x 4096 fibers = 2 MiB >> 12) and the HW stack past it in the stage's registers, the other stage
// all zeros, and the same buffer again for the next program that fits in it.
TEST(a_stage_that_wants_private_memory_gets_the_queues) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Program p = handProgram();
    p.fs.k.pvtmemSize = 64;
    {
        Encoded e(r, p, pass, wholeDraw(3));
        CHECK(!e.ok);
        expectRefusal(e.error, "hand_fs wants 64 bytes of private memory a fiber and none is bound");
        Bindings small;
        small.privateMemory = PrivateMemory{0x100000, 32, 4096, false};
        Encoded f(r, p, pass, wholeDraw(3), render::Options{}, Bindings{}, small);
        CHECK(!f.ok);
        expectRefusal(f.error, "hand_fs wants 64 bytes a fiber: the private memory bound has 32 at 1048576");
    }
    PrivateMemory pm;
    std::string why;
    auto scratch = r.queue->privateMemory(64, false, &pm, &why);
    CHECK(scratch != nullptr);
    CHECK_EQ(pm.perFiber, 512u);
    CHECK_EQ(pm.perSp, 512u * 4096u);
    CHECK_EQ(scratch->length(), 2ull * 512u * 4096u);
    Bindings fsb;
    fsb.privateMemory = pm;
    Encoded e(r, p, pass, wholeDraw(3), render::Options{}, Bindings{}, fsb);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_OBJ_FIRST_EXEC_OFFSET + 3), A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(512));
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_OBJ_FIRST_EXEC_OFFSET + 4), uint32_t(scratch->gpuAddress()));
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_OBJ_FIRST_EXEC_OFFSET + 6), (512u * 4096u) >> 12);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET), (512u * 4096u) >> 11);
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_OBJ_FIRST_EXEC_OFFSET + 4), 0u);
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET), 0u);
    auto cb = drawOnce(r, p, pass, wholeDraw(3));
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    PrivateMemory again;
    CHECK(r.queue->privateMemory(300, false, &again, &why) == scratch);   // 300 fits in the 512 already there
    CHECK(r.queue->privateMemory(600, false, &again, &why) != scratch);   // 600 grows it to 1024
    CHECK_EQ(again.perFiber, 1024u);
}

// Lines: Metal's Line and LineStrip are the hardware's line list and strip, one pixel wide (LINEHALFWIDTH 2 quarters)
TEST(lines_are_drawn_as_the_hardwares_line_list_and_strip) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    for (auto [prim, want] : {std::pair<Primitive, uint32_t>{Primitive::kLine, DI_PT_LINELIST}, {Primitive::kLineStrip, DI_PT_LINESTRIP}}) {
        Draw d = wholeDraw(2);
        d.primitive = prim;
        Encoded e(r, flatProgram(), pass, d);
        CHECK(e.ok);
        size_t at = e.find(CP_DRAW_INDX_OFFSET);
        CHECK(at != size_t(-1));
        if (at != size_t(-1)) {
            CHECK_EQ((e.stream.words()[at + 1] & CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__MASK) >> CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__SHIFT, want);
        }
        CHECK_EQ((e.reg(REG_A6XX_GRAS_SU_CNTL) & A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__MASK) >> A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__SHIFT, 2u);
        auto cb = drawOnce(r, flatProgram(), pass, d);
        CHECK(cb->status() == CommandBuffer::kCompleted);
    }
}

TEST(a_vertex_stage_gets_its_driver_params_every_draw) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Program p = handProgram();
    p.vs.k.driverParamsOffsetVec4 = 5;
    Draw d = wholeDraw(3);
    d.vertexStart = 3;
    d.baseInstance = 7;
    Encoded e(r, p, pass, d);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    using namespace a6xx;
    const auto& w = e.stream.words();
    size_t at = e.findLoad(CP_LOAD_STATE6_GEOM, ST6_CONSTANTS, SB6_VS_SHADER);
    CHECK(at != size_t(-1));
    if (at != size_t(-1)) {
        CHECK_EQ(w[at + 1], loadState6_0(5, ST6_CONSTANTS, SS6_DIRECT, SB6_VS_SHADER, 1));
        CHECK_EQ(w[at + 4], 0u);
        CHECK_EQ(w[at + 5], 3u);
        CHECK_EQ(w[at + 6], 7u);
        CHECK_EQ(w[at + 7], 0u);
    }
    CHECK_EQ(e.reg(REG_A6XX_VFD_INDEX_OFFSET), 3u);
    CHECK_EQ(e.reg(REG_A6XX_VFD_INSTANCE_START_OFFSET), 7u);
    // a VS without driver params gets no constant load of its own
    Program q = handProgram();
    Encoded f(r, q, pass, d);
    CHECK(f.ok);
    CHECK_EQ(f.findLoad(CP_LOAD_STATE6_GEOM, ST6_CONSTANTS, SB6_VS_SHADER), size_t(-1));
}

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
    fmt[e.findLastReg(regVfdDecodeInstr(0))] = vfdDecodeInstr(0, 0, false, FMT6_5_6_5_UNORM, WZYX, true);   // a texture format only
    CHECK(has(faultOf(r, fmt), "vertex attribute 0: VFD_DECODE_INSTR.FORMAT is not a vertex format this model knows"));
}

// The colour attribute in each family of formats: the decode word's FORMAT, SWAP and FLOAT bit as MTLVertexDescriptor.h
// and fd6_format_table.c name them, written out here rather than read from render.cpp's table. A format whose element
// does not fit is refused by the driver's own size: an 8-byte element at offset 8 of a 12-byte vertex runs the sixth
// vertex past the 72 bytes bound, a 4-byte one does not.
TEST(each_vertex_format_family_decodes_as_metal_names_it) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    std::string why;
    auto vb = newBuffer(*r.gpu, 6 * 12, &why);
    Bindings vsb;
    vsb.buffers = {Binding{vb.get(), 0}};
    struct Case {
        uint32_t mtl;
        a6xx_format fmt;
        a3xx_color_swap swap;
        bool isFloat;
        bool fits;
    };
    const Case cases[] = {
        {7, FMT6_8_8_UNORM, WZYX, true, true},             // UChar2Normalized
        {12, FMT6_8_8_8_8_SNORM, WZYX, true, true},        // Char4Normalized
        {3, FMT6_8_8_8_8_UINT, WZYX, false, true},         // UChar4
        {42, FMT6_8_8_8_8_UNORM, WXYZ, true, true},        // UChar4Normalized_BGRA: R and B swapped on the way in
        {19, FMT6_16_16_UNORM, WZYX, true, true},          // UShort2Normalized (sokol-debugtext's texture coordinate)
        {22, FMT6_16_16_SNORM, WZYX, true, true},          // Short2Normalized
        {16, FMT6_16_16_SINT, WZYX, false, true},          // Short2
        {53, FMT6_16_FLOAT, WZYX, true, true},             // Half
        {36, FMT6_32_UINT, WZYX, false, true},             // UInt
        {32, FMT6_32_SINT, WZYX, false, true},             // Int
        {21, FMT6_16_16_16_16_UNORM, WZYX, true, false},   // UShort4Normalized: 8 bytes
        {33, FMT6_32_32_SINT, WZYX, false, false},         // Int2: 8 bytes
    };
    for (const Case& c : cases) {
        Program p = attrProgram();
        p.vertexDescriptor.attributes[1].format = VertexFormat(c.mtl);
        Encoded e(r, p, pass, wholeDraw(6), render::Options{}, vsb, Bindings{});
        CHECK_EQ(e.ok, c.fits);
        if (e.ok != c.fits) std::printf("       MTLVertexFormat %u: %s\n", c.mtl, e.ok ? "accepted" : e.error.c_str());
        if (!c.fits) {
            CHECK(has(e.error, "[[attribute(1)]]"));
            continue;
        }
        CHECK_EQ(e.reg(regVfdDecodeInstr(1)), vfdDecodeInstr(0, 8, false, c.fmt, c.swap, c.isFloat));
        auto cb = drawOnce(r, p, pass, wholeDraw(6), render::Options{}, vsb, Bindings{});
        CHECK(cb->status() == CommandBuffer::kCompleted);
        if (cb->status() != CommandBuffer::kCompleted) std::printf("       MTLVertexFormat %u: %s\n", c.mtl, r.fake->stats().lastFault.c_str());
    }
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
    refusedWith(p, vsb, 6, "tri_attr_vs_air: [[attribute(1)]] is MTLVertexFormat 41: the 1010102, RG11B10 and RGB9E5 formats are not served");
    p = attrProgram();
    p.vertexDescriptor.layouts[0].step = VertexStep::kConstant;
    refusedWith(p, vsb, 6, "tri_attr_vs_air: buffer 0 steps constantly: per-vertex and per-instance fetch are served");
    p = attrProgram();
    p.vertexDescriptor.layouts[0].step = VertexStep::kPerInstance;
    p.vertexDescriptor.layouts[0].stepRate = 0;
    refusedWith(p, vsb, 6, "tri_attr_vs_air: buffer 0 steps per instance at a rate of 0: Metal's step rate is 1 or more");
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


// --- a Clear: the 2D engine's solid fill, as turnip clears a single-sampled sysmem attachment ----------------------------

namespace {
// every pixel of T is the BPP bytes of WANT
bool allPixels(const Texture& t, const uint8_t* want, uint32_t bpp) {
    auto* b = static_cast<const uint8_t*>(t.contents());
    for (uint32_t y = 0; y < t.height(); y++) {
        for (uint32_t x = 0; x < t.width(); x++) {
            if (memcmp(b + y * t.bytesPerRow() + x * bpp, want, bpp) != 0) return false;
        }
    }
    return true;
}

void seed(const Texture& t) { memset(const_cast<void*>(static_cast<const void*>(t.contents())), 0xa5, t.buffer()->length()); }

// a pass with no draw through CommandBuffer::clear, committed and waited for
std::shared_ptr<CommandBuffer> clearOnce(Rig& r, const RenderPass& pass) {
    auto cb = r.queue->commandBuffer();
    if (cb->clear(pass)) {
        cb->commit();
        cb->waitUntilCompleted();
    }
    return cb;
}

RenderPass clearPass(const Texture* t, double r, double g, double b, double a) {
    RenderPass p;
    p.color0 = t;
    p.load = LoadAction::kClear;
    p.clearColor[0] = r;
    p.clearColor[1] = g;
    p.clearColor[2] = b;
    p.clearColor[3] = a;
    return p;
}
}  // namespace

// a copy turned by a quarter: RB_2D_BLIT_CNTL and GRAS_2D_BLIT_CNTL carry the a6xx_rotation in bits 0..2, the
// destination is the source's sides swapped, and a destination of the source's own size (a scale, turned) is refused
TEST(a_turned_copy_sets_the_2d_rotation_and_takes_the_sides_swapped) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc a, b;
    a.format = b.format = PixelFormat::kBGRA8Unorm;
    a.width = 64;
    a.height = 32;
    b.width = 32;
    b.height = 64;
    a.usage = b.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    std::shared_ptr<Texture> src = Texture::make(*r.gpu, a, &why);
    std::shared_ptr<Texture> dst = Texture::make(*r.gpu, b, &why);
    std::shared_ptr<Texture> same = Texture::make(*r.gpu, a, &why);
    CHECK(src != nullptr && dst != nullptr && same != nullptr);
    if (src == nullptr || dst == nullptr || same == nullptr) return;
    auto table = newBuffer(*r.gpu, render::kTableBytes, &why);
    auto lastReg = [](const pm4::Stream& st, uint32_t reg) {
        uint32_t v = 0xdeadbeef;
        const auto& w = st.words();
        for (size_t i = 0; i < w.size();) {
            pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kRegisters && reg >= h.reg && reg < h.reg + h.count) v = w[i + 1 + (reg - h.reg)];
            i += 1 + h.count;
        }
        return v;
    };
    for (uint32_t rot : {1u, 3u}) {
        render::Options o;
        o.rotate2d = rot;
        pm4::Stream st;
        CHECK(render::encodeBlit(st, *src, 0, render::BlitRect{0, 0, 64, 32}, *dst, 0, render::BlitRect{0, 0, 32, 64}, false, table->gpuAddress(),
                                 static_cast<uint8_t*>(table->contents()), &why, o));
        CHECK_EQ(lastReg(st, REG_A6XX_RB_2D_BLIT_CNTL) & 7u, rot);
        CHECK_EQ(lastReg(st, REG_A6XX_GRAS_2D_BLIT_CNTL) & 7u, rot);
        CHECK_EQ(lastReg(st, REG_A6XX_GRAS_2D_DST_BR), (63u << 16) | 31u);
        pm4::Stream bad;
        CHECK(!render::encodeBlit(bad, *src, 0, render::BlitRect{0, 0, 64, 32}, *same, 0, render::BlitRect{0, 0, 64, 32}, false, table->gpuAddress(),
                                  static_cast<uint8_t*>(table->contents()), &why, o));
    }
    pm4::Stream plain;
    CHECK(render::encodeBlit(plain, *src, 0, render::BlitRect{0, 0, 64, 32}, *same, 0, render::BlitRect{0, 0, 64, 32}, false, table->gpuAddress(),
                             static_cast<uint8_t*>(table->contents()), &why, render::Options{}));
    CHECK_EQ(lastReg(plain, REG_A6XX_RB_2D_BLIT_CNTL) & 7u, 0u);
}

TEST(a_clear_fills_the_target_through_the_2d_engine_before_the_draw) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    seed(*t);
    RenderPass pass = clearPass(t.get(), 0.25, 0.5, 0.75, 1.0);
    Encoded e(r, flatProgram(), pass, wholeDraw());
    CHECK(e.ok);
    // in the pass, before the first draw state: after the RM6_DIRECT_RENDER marker, before the draw and before RB_MRT
    size_t marker = e.find(CP_SET_MARKER), blit = e.find(CP_BLIT), draw = e.find(CP_DRAW_INDX_OFFSET);
    CHECK(marker != size_t(-1) && blit != size_t(-1) && draw != size_t(-1));
    CHECK(marker < blit && blit < draw);
    CHECK(blit < e.findReg(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 0)));
    CHECK_EQ(e.find(CP_BLIT, blit + 1), size_t(-1));   // one fill
    // the words, as literals: SOLID_COLOR 0x80, FMT6_8_8_8_8_UNORM (48) << 8, MASK 0xf << 20, R2D_UNORM8 (16) << 24
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_BLIT_CNTL), 0x10f03080u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_2D_BLIT_CNTL), 0x10f03080u);
    CHECK_EQ(e.reg(REG_A6XX_SP_2D_DST_FORMAT), 0xf000u | (48u << 3));
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_DST_INFO), 48u);                    // linear, WZYX, no sRGB
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_DST_PITCH), 4u);                    // 256 bytes in units of 64
    CHECK_EQ(e.reg(REG_A6XX_GRAS_2D_DST_TL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_2D_DST_BR), (63u << 16) | 63u);
    // 0.25 * 255 = 63.75 -> 64, 0.5 * 255 = 127.5 -> 128 (even), 0.75 * 255 = 191.25 -> 191, 1 -> 255
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_SRC_SOLID_C0), 64u);
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_SRC_SOLID_C0 + 1), 128u);
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_SRC_SOLID_C0 + 2), 191u);
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_SRC_SOLID_C0 + 3), 255u);
    CHECK_EQ(uint64_t(e.reg(REG_A6XX_RB_2D_DST)) | uint64_t(e.reg(REG_A6XX_RB_2D_DST + 1)) << 32, t->gpuAddress());
    // through the command buffer in sysmem: the fake carries the fill out (and rasterizes nothing), so every pixel is
    // the colour (a GMEM pass clears in its tiles instead: a_clear_in_gmem_comes_out_as_the_fill_does)
    render::Options sysmem;
    sysmem.gmem = false;
    auto cb = drawOnce(r, flatProgram(), pass, wholeDraw(), sysmem);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const uint8_t want[4] = {64, 128, 191, 255};
    CHECK(allPixels(*t, want, 4));
    CHECK_EQ(r.fake->stats().fills, 1u);
    CHECK_EQ(r.fake->stats().draws, 1u);
    CHECK_EQ(r.fake->stats().faults, 0u);
    // the twin: a Load pass draws without a fill, so the canary stays
    seed(*t);
    RenderPass load = pass;
    load.load = LoadAction::kLoad;
    Encoded l(r, flatProgram(), load, wholeDraw());
    CHECK(l.ok);
    CHECK_EQ(l.find(CP_BLIT), size_t(-1));
    auto lc = drawOnce(r, flatProgram(), load, wholeDraw(), sysmem);
    CHECK(lc->status() == CommandBuffer::kCompleted);
    const uint8_t canary[4] = {0xa5, 0xa5, 0xa5, 0xa5};
    CHECK(allPixels(*t, canary, 4));
    CHECK_EQ(r.fake->stats().fills, 1u);
}

TEST(a_clear_packs_the_colour_as_each_format_holds_it) {
    struct Case {
        PixelFormat format;
        double c[4];
        uint32_t bpp;
        uint8_t want[16];
    };
    // the halves: 0.25 = 0x3400, -2 = 0xc000, 65504 = 0x7bff (the largest), 1e-7 = 2 units of 2^-24 (a subnormal, rounded)
    // the words: 1.5f = 0x3fc00000, -0.0f = 0x80000000
    const Case cases[] = {
        {PixelFormat::kRGBA8Unorm, {0.25, 0.5, 0.75, 1.0}, 4, {64, 128, 191, 255}},
        {PixelFormat::kBGRA8Unorm, {0.25, 0.5, 0.75, 1.0}, 4, {191, 128, 64, 255}},
        {PixelFormat::kRGBA8Unorm, {-1.0, 2.0, 0.5 / 255.0, 1.5 / 255.0}, 4, {0, 255, 0, 2}},   // clamped; 0.5 and 1.5 to even
        // linear 0.5 is sRGB 0.7354 -> 187.5 -> 188; alpha stays linear: 0.5 -> 128
        {PixelFormat::kRGBA8UnormSrgb, {0.5, 0.5, 0.5, 0.5}, 4, {188, 188, 188, 128}},
        {PixelFormat::kBGRA8UnormSrgb, {1.0, 0.0, 0.002, 1.0}, 4, {7, 0, 255, 255}},   // 0.002 * 12.92 * 255 = 6.6 -> 7, then swapped
        {PixelFormat::kR8Unorm, {1.0, 0.5, 0.5, 0.5}, 1, {255}},
        {PixelFormat::kRG8Unorm, {0.0, 1.0, 0.5, 0.5}, 2, {0, 255}},
        {PixelFormat::kRGBA16Float, {0.25, -2.0, 65504.0, 1e-7}, 8, {0x00, 0x34, 0x00, 0xc0, 0xff, 0x7b, 0x02, 0x00}},
        {PixelFormat::kR32Float, {1.5, 9.0, 9.0, 9.0}, 4, {0x00, 0x00, 0xc0, 0x3f}},
        {PixelFormat::kRGBA32Float, {1.5, -0.0, 0.0, 1.0}, 16, {0, 0, 0xc0, 0x3f, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x80, 0x3f}},
        {PixelFormat::kR32Uint, {7.0, 0.0, 0.0, 0.0}, 4, {7, 0, 0, 0}},
        {PixelFormat::kRGBA32Uint, {1.0, 2.0, 4294967295.0, 0.0}, 16, {1, 0, 0, 0, 2, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0}},
    };
    for (const Case& c : cases) {
        Rig r;
        auto t = target(r, 64, 64, kTextureUsageRenderTarget | kTextureUsageRead, c.format);
        seed(*t);
        auto cb = clearOnce(r, clearPass(t.get(), c.c[0], c.c[1], c.c[2], c.c[3]));
        CHECK(cb->status() == CommandBuffer::kCompleted);
        bool ok = allPixels(*t, c.want, c.bpp);
        CHECK(ok);
        if (!ok) {
            const uint8_t* b = static_cast<const uint8_t*>(t->contents());
            std::fprintf(stderr, "  %s: pixel 0 is", t->info().name);
            for (uint32_t i = 0; i < c.bpp; i++) std::fprintf(stderr, " %02x", b[i]);
            std::fprintf(stderr, "\n");
        }
        CHECK_EQ(r.fake->stats().fills, 1u);
        CHECK_EQ(r.fake->stats().faults, 0u);
    }
}

TEST(a_clear_without_a_draw_is_a_pass_of_its_own) {
    using namespace a6xx;
    Rig r;
    auto t = target(r, 48, 20);
    seed(*t);
    RenderPass pass = clearPass(t.get(), 0.0, 0.0, 1.0, 1.0);
    pm4::Stream s;
    std::string why;
    auto table = newBuffer(*r.gpu, render::kTableBytes, &why);
    CHECK(render::encodeClear(s, pass, table->gpuAddress(), static_cast<uint8_t*>(table->contents()), &why));
    bool sawBlit = false, sawDraw = false, sawMarker = false;
    for (size_t i = 0; i < s.words().size();) {
        pm4::Header h = pm4::decode(s.words()[i]);
        if (h.kind == pm4::Header::kCommand) {
            sawBlit |= h.opcode == CP_BLIT;
            sawDraw |= h.opcode == CP_DRAW_INDX_OFFSET;
            sawMarker |= h.opcode == CP_SET_MARKER;
        }
        i += 1 + h.count;
    }
    CHECK(sawBlit && sawMarker && !sawDraw);
    auto cb = clearOnce(r, pass);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const uint8_t blue[4] = {0, 0, 255, 255};
    CHECK(allPixels(*t, blue, 4));   // 48 x 20: the rectangle is the target's, not a fixed 64 x 64
    // the rows past the target's height (the layout pads to 4 rows) and the bytes past its width keep the canary
    const uint8_t* b = static_cast<const uint8_t*>(t->contents());
    CHECK_EQ(b[48 * 4], 0xa5u);
    CHECK_EQ(b[20 * t->bytesPerRow()], 0xa5u);
    CHECK_EQ(r.fake->stats().fills, 1u);
    CHECK_EQ(r.fake->stats().draws, 0u);
    // refused: nothing to write without a Clear, and the target rules a draw has
    for (LoadAction a : {LoadAction::kLoad, LoadAction::kDontCare}) {
        RenderPass p = pass;
        p.load = a;
        auto c = r.queue->commandBuffer();
        CHECK(!c->clear(p));
        expectRefusal(c->error(), a == LoadAction::kLoad ? "a pass without a draw and with load action 1: only a Clear writes anything then"
                                                          : "a pass without a draw and with load action 0: only a Clear writes anything then");
    }
    auto plain = target(r, 64, 64, kTextureUsageRead);
    auto c = r.queue->commandBuffer();
    CHECK(!c->clear(clearPass(plain.get(), 0, 0, 0, 0)));
    expectRefusal(c->error(), "the 64 x 64 RGBA8Unorm texture was made without render-target usage (MTLTextureUsageRenderTarget, 4); a draw needs it");
    CHECK_EQ(r.fake->stats().submits, 1u);
}

TEST(a_clear_refuses_a_colour_it_cannot_pack) {
    Rig r;
    auto t = target(r);
    auto u = target(r, 64, 64, kTextureUsageRenderTarget, PixelFormat::kR32Uint);
    struct Case {
        const Texture* t;
        double c[4];
        const char* want;
    };
    const Case cases[] = {
        {t.get(), {0, 0, 0, INFINITY}, "a clear colour with a non-finite channel (channel 3) into RGBA8Unorm"},
        {u.get(), {2.5, 0, 0, 0}, "a clear colour of 2.500000 in channel 0 of R32Uint: an integer target takes whole numbers 0 to 4294967295"},
        {u.get(), {-1, 0, 0, 0}, "a clear colour of -1.000000 in channel 0 of R32Uint: an integer target takes whole numbers 0 to 4294967295"},
        {u.get(), {4294967296.0, 0, 0, 0}, "a clear colour of 4294967296.000000 in channel 0 of R32Uint: an integer target takes whole numbers 0 to 4294967295"},
    };
    for (const Case& c : cases) {
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->clear(clearPass(c.t, c.c[0], c.c[1], c.c[2], c.c[3])));
        expectRefusal(cb->error(), c.want);
    }
    CHECK_EQ(r.fake->stats().submits, 0u);
}

namespace {
// a clear-only pass over a fresh rig, its words to mutate
struct ClearTwin {
    ClearTwin() : t(target(r)) {
        seed(*t);
        pass = clearPass(t.get(), 0.25, 0.5, 0.75, 1.0);
        std::string why;
        table = newBuffer(*r.gpu, render::kTableBytes, &why);
        CHECK(render::encodeClear(s, pass, table->gpuAddress(), static_cast<uint8_t*>(table->contents()), &why));
        words = s.words();
    }
    uint32_t& at(uint32_t reg) {
        for (size_t i = 0; i < words.size();) {
            pm4::Header h = pm4::decode(words[i]);
            if (h.kind == pm4::Header::kRegisters && reg >= h.reg && reg < h.reg + h.count) return words[i + 1 + (reg - h.reg)];
            i += 1 + h.count;
        }
        CHECK(false);
        return words[0];
    }
    std::string fault() { return faultOf(r, words); }
    Rig r;
    std::shared_ptr<Texture> t;
    RenderPass pass;
    std::shared_ptr<Buffer> table;
    pm4::Stream s;
    std::vector<uint32_t> words;
};
}  // namespace

TEST(the_fake_refuses_a_fill_whose_registers_do_not_hold_together) {
    using namespace a6xx;
    {
        // unmutated it passes and fills: the twins below fail for their own reason
        ClearTwin w;
        Raw raw(w.r);
        CHECK_EQ(raw.submit(w.words), 0);
        const uint8_t want[4] = {64, 128, 191, 255};
        CHECK(allPixels(*w.t, want, 4));
        CHECK_EQ(w.r.fake->stats().fills, 1u);
        const uint32_t* tw = static_cast<const uint32_t*>(w.table->contents());
        CHECK_EQ(tw[render::kTableClearFlushAt / 4], render::kClearFlushValue);   // the clear's own CCU flush landed
        CHECK_EQ(tw[0], 1u);
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_GRAS_2D_BLIT_CNTL) ^= 0x10u << 24 ^ 0x3u << 24;
        CHECK(has(w.fault(), "RB_2D_BLIT_CNTL 0x10f03080 and GRAS_2D_BLIT_CNTL 0x3f03080 differ"));
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_GRAS_2D_BLIT_CNTL) &= ~A6XX_RB_2D_BLIT_CNTL_SOLID_COLOR;
        w.at(REG_A6XX_RB_2D_BLIT_CNTL) &= ~A6XX_RB_2D_BLIT_CNTL_SOLID_COLOR;
        // (a copy now, with no source set up: refused for that)
        CHECK(has(w.fault(), "a copy from format 0 into 48"));
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_SP_2D_DST_FORMAT) &= ~A6XX_SP_2D_DST_FORMAT_MASK__MASK;
        CHECK(has(w.fault(), "component mask is not 0xf"));
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_SP_2D_DST_FORMAT) = sp2dDstFormat(FMT6_32_FLOAT, false, false, false);
        CHECK(has(w.fault(), "the fill's format differs between RB_2D_BLIT_CNTL (48), RB_2D_DST_INFO (48) and SP_2D_DST_FORMAT (74)"));
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_RB_2D_BLIT_CNTL) = w.at(REG_A6XX_GRAS_2D_BLIT_CNTL) = rb2dBlitCntlClear(FMT6_8_8_8_8_UNORM, R2D_FLOAT16);
        CHECK(has(w.fault(), "a fill of format 48 through intermediate format 3: format_to_ifmt picks 16"));
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_RB_2D_DST_INFO) |= A6XX_RB_2D_DST_INFO_SRGB;
        CHECK(has(w.fault(), "the sRGB bits"));
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_RB_2D_DST_PITCH) = A6XX_RB_2D_DST_PITCH(192);
        CHECK(has(w.fault(), "rows wider than the pitch"));
    }
    {
        ClearTwin w;
        w.at(REG_A6XX_RB_2D_DST + 1) = 0x7;
        CHECK(has(w.fault(), "outside every buffer"));
    }
    {
        // the rectangle's last row exactly at the end of the GPU's buffer passes; one row more is outside it
        ClearTwin w;
        const uint32_t rows = uint32_t(w.t->buffer()->bo().size() / w.t->bytesPerRow());
        w.at(REG_A6XX_GRAS_2D_DST_BR) = regXY(63, rows - 1);
        Raw raw(w.r);
        CHECK_EQ(raw.submit(w.words), 0);
        w.at(REG_A6XX_GRAS_2D_DST_BR) = regXY(63, rows);
        CHECK(has(w.fault(), "rows of 256 bytes: outside every buffer"));
    }
}

// --- blending: colour attachment 0's blend state into RB_MRT_CONTROL / RB_MRT_BLEND_CONTROL, the constants as floats ---

TEST(a_draw_blends_as_the_pipeline_says) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    // off (Metal's default): the write mask alone, all four components, no blend anywhere
    {
        Encoded e(r, flatProgram(), pass, wholeDraw());
        CHECK(e.ok);
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0x780u);
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, 0)), 0u);
        CHECK_EQ(e.reg(REG_A6XX_SP_BLEND_CNTL), 0x100u);
        CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_CNTL), 0xffff0100u);
    }
    // source-over, straight alpha: rgb SourceAlpha / OneMinusSourceAlpha, alpha One / OneMinusSourceAlpha, both Add.
    // As literals: FACTOR_SRC_ALPHA 6, FACTOR_ONE_MINUS_SRC_ALPHA 7 << 8, FACTOR_ONE 1 << 16, 7 << 24
    Program p = flatProgram();
    p.blend = ColorBlend{true, 0, 0, 4, 5, 1, 5, 0xf};
    Draw d = wholeDraw();
    d.blendColor[0] = 0.5f;
    d.blendColor[3] = -1.0f;
    {
        Encoded e(r, p, pass, d);
        CHECK(e.ok);
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0x783u);   // BLEND, BLEND2, the four components
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, 0)), 0x07010706u);
        CHECK_EQ(e.reg(REG_A6XX_SP_BLEND_CNTL), 0x101u);
        CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_CNTL), 0xffff0101u);
        CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_RED_F32), 0x3f000000u);
        CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_GREEN_F32), 0u);
        CHECK_EQ(e.reg(REG_A6XX_RB_BLEND_ALPHA_F32), 0xbf800000u);
        auto cb = drawOnce(r, p, pass, d);
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(r.fake->stats().lastDraw.mrtBlendControl, 0x07010706u);
    }
    // every Metal factor onto its adreno number (MTLBlendFactor 0..14 -> adreno_rb_blend_factor), as source RGB, and the
    // five operations as the RGB opcode
    const uint32_t adreno[15] = {0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 16, 12, 13, 14, 15};
    for (uint32_t f = 0; f < 15; f++) {
        Program q = flatProgram();
        q.blend = ColorBlend{true, f % 5, 0, f, 0, 1, 0, 0xf};
        Encoded e(r, q, pass, wholeDraw());
        CHECK(e.ok);
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_BLEND_CONTROL, 0)), adreno[f] | (f % 5) << 5 | 1u << 16);
    }
    // the write mask: MTLColorWriteMask Red 8 | Alpha 1 is the component enable's R 1 | A 8; Green 4 alone is G 2
    {
        Program q = flatProgram();
        q.blend.writeMask = 8 | 1;
        Encoded e(r, q, pass, wholeDraw());
        CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0x9u << 7);
        q.blend.writeMask = 4;
        Encoded g(r, q, pass, wholeDraw());
        CHECK_EQ(g.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0x2u << 7);
        q.blend.writeMask = 0;   // None: legal, writes nothing
        Encoded z(r, q, pass, wholeDraw());
        CHECK(z.ok);
        CHECK_EQ(z.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0u);
        auto cb = drawOnce(r, q, pass, wholeDraw());
        CHECK(cb->status() == CommandBuffer::kCompleted);
    }
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// What a buffer holds, counted as it is recorded (metal_fps reads these beside the GPU time): two draws into one pass
// share it, a draw into another target opens a second, a copy is a pass of its own and is counted as a copy
TEST(a_command_buffer_counts_its_passes_and_draws) {
    Rig r;
    auto t = target(r), u = target(r);
    RenderPass a, b;
    a.color0 = t.get();
    b.color0 = u.get();
    auto cb = r.queue->commandBuffer();
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    CHECK(cb->draw(flatProgram(), b, wholeDraw(), Bindings{}, Bindings{}));
    CHECK(cb->blit(*t, 0, render::BlitRect{0, 0, 16, 16}, *u, 0, render::BlitRect{0, 0, 16, 16}, false));
    const CommandBuffer::Counts& c = cb->counts();
    CHECK_EQ(c.passes, 2u);
    CHECK_EQ(c.draws, 3u);
    CHECK_EQ(c.copies, 1u);
    CHECK_EQ(c.dispatches, 0u);
}

// Profiling: a stamp at each pass's start and after its tail. The buffer still completes on the fake, and the passes
// are described as they were recorded -- a draw pass of 64 x 64 with two draws, then a 16 x 16 copy
TEST(a_profiled_command_buffer_lists_its_passes) {
    Rig r;
    auto t = target(r), u = target(r);
    RenderPass a;
    a.color0 = t.get();
    auto cb = r.queue->commandBuffer();
    cb->setProfile(true);
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    CHECK(cb->blit(*t, 0, render::BlitRect{0, 0, 16, 16}, *u, 0, render::BlitRect{0, 0, 16, 16}, false));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const auto p = cb->passTimes();
    CHECK_EQ(p.size(), size_t(2));
    if (p.size() == 2) {
        CHECK(p[0].kind == CommandBuffer::kPassDraw && p[0].width == 64 && p[0].height == 64 && p[0].draws == 2);
        CHECK(p[1].kind == CommandBuffer::kPassCopy && p[1].width == 16 && p[1].draws == 0);
        // (the fake's counter steps once a stamp: each pass's end one after its start, the copy's after the draw's)
        CHECK(p[0].us > 0 && p[1].us > 0);
    }
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// (a measurement, not a rule: how many words a draw adds to its pass, and of which packets -- the per-draw cost on the
// phone is the command processor reading these)
// A second draw of the same state into the same pass writes only what changed: far fewer words than the first, which
// carries the pass's whole state; with dedupRegs off it writes as many. Both buffers complete on the fake, whose checks
// hold each draw's state together, so the registers left out were the ones already holding their values.
TEST(a_draw_in_an_open_pass_leaves_out_registers_already_holding_their_values) {
    Rig r;
    auto t = target(r);
    RenderPass a;
    a.color0 = t.get();
    size_t grew[2][2] = {};
    for (int dedup = 0; dedup < 2; dedup++) {
        render::Options o;
        o.dedupRegs = dedup != 0;
        o.gmem = false;   // (a GMEM pass's draws are gathered off the stream until its end)
        auto cb = r.queue->commandBuffer();
        const size_t at0 = cb->streamWords();
        CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}, o));
        const size_t at1 = cb->streamWords();
        CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}, o));
        const size_t at2 = cb->streamWords();
        grew[dedup][0] = at1 - at0;
        grew[dedup][1] = at2 - at1;
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
    }
    std::printf("       a draw then the same draw: %zu and %zu words written in full, %zu and %zu deduplicated\n", grew[0][0], grew[0][1],
                grew[1][0], grew[1][1]);
    CHECK(grew[1][1] * 3 < grew[0][1]);   // the second draw, deduplicated: under a third of its full size
    CHECK(grew[0][1] + 64 > grew[0][0] - 64 || grew[0][1] > 300);   // (in full, the second draw is a draw's full size)
    CHECK_EQ(r.fake->stats().faults, 0u);
}

TEST(the_words_of_a_draw_counted) {
    Rig r;
    auto t = target(r);
    RenderPass a;
    a.color0 = t.get();
    Encoded e(r, flatProgram(), a, wholeDraw());
    CHECK(e.ok);
    std::map<std::string, uint32_t> words;
    const auto& w = e.stream.words();
    uint32_t regs = 0, total = 0;
    for (size_t i = 0; i < w.size();) {
        pm4::Header h = pm4::decode(w[i]);
        if (h.kind == pm4::Header::kRegisters) regs += 1 + h.count;
        else words["opcode " + std::to_string(h.opcode)] += 1 + h.count;
        total += 1 + h.count;
        i += 1 + h.count;
    }
    std::printf("       a draw with its pass start and end: %u words, %u of them register writes", total, regs);
    for (const auto& [k, v] : words) std::printf("; %s %u", k.c_str(), v);
    std::printf("\n");
}

TEST(a_draw_refuses_a_blend_it_cannot_serve) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    auto blendOf = [](uint32_t rgbOp, uint32_t srcRgb, uint32_t dstRgb, uint32_t writeMask = 0xf) {
        Program p = flatProgram();
        p.blend = ColorBlend{true, rgbOp, 0, srcRgb, dstRgb, 1, 0, writeMask};
        return p;
    };
    refused(r, blendOf(0, 15, 0), pass, wholeDraw(), "the source RGB blend factor 15 reads a second source colour, and tri_flat_fs_air writes none ([[color(0), index(1)]])");
    refused(r, blendOf(0, 1, 18), pass, wholeDraw(), "the destination RGB blend factor 18 reads a second source colour, and tri_flat_fs_air writes none ([[color(0), index(1)]])");
    refused(r, blendOf(0, 19, 0), pass, wholeDraw(), "the source RGB blend factor 19: MTLBlendFactor runs 0 to 18");
    refused(r, blendOf(5, 1, 0), pass, wholeDraw(), "blend operation 5: MTLBlendOperation runs Add 0 to Max 4");
    Program mask = flatProgram();
    mask.blend.writeMask = 0x10;
    refused(r, mask, pass, wholeDraw(), "a colour write mask of 16: MTLColorWriteMask has the four bits Red 8, Green 4, Blue 2, Alpha 1");
    Draw d = wholeDraw();
    d.blendColor[2] = std::nanf("");
    refused(r, blendOf(0, 11, 0), pass, d, "a blend colour with a non-finite channel (channel 2)");
    // an integer target does not blend (the FS writes integers: the hand program's integer twin)
    auto u = target(r, 64, 64, kTextureUsageRenderTarget, PixelFormat::kR32Uint);
    RenderPass ints;
    ints.color0 = u.get();
    Program ip = handProgram();
    ip.fs.outputs[0].pad = kElemUint;
    ip.blend = ColorBlend{true, 0, 0, 1, 1, 1, 1, 0xf};
    refused(r, ip, ints, wholeDraw(), "blending into R32Uint: an integer format has no blend (Metal refuses the pipeline too)");
    // ...while the same program with the blend off draws there
    ip.blend.enabled = false;
    Encoded e(r, ip, ints, wholeDraw());
    CHECK(e.ok);
}

// --- textures in the fragment stage: the FS_TEX tables and the prefetch ---------------------------------------------------

namespace {
Program texProgram() { return Program{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"}; }

Sampler nearestClamp() {
    Sampler s;
    std::string why;
    CHECK(makeSampler(SamplerDesc{}, &s, &why));
    return s;
}
}  // namespace

TEST(a_draw_samples_a_texture_through_the_fs_prefetch) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    auto tex = target(r, 4, 4, kTextureUsageRead);
    Sampler smp = nearestClamp();
    RenderPass pass;
    pass.color0 = t.get();
    Bindings fsb;
    fsb.textures = {tex.get()};
    fsb.samplers = {&smp};
    Encoded e(r, texProgram(), pass, wholeDraw(6), render::Options{}, Bindings{}, fsb);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    // the prefetch, as literals: COUNT 1 (the FS reads its perspective barycentric, so no IJ_WRITE_DISABLE) and ENDOFQUAD
    // (0x10: an implicit-level sample needs its quads whole, compiler.cpp: needsQuadHelpers), then the command: src 0,
    // sampler 0, texture 0, dst r0.z (2) << 16, wrmask 0xf << 22, TEX_PREFETCH_SAM (1) << 29
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_PREFETCH_CNTL), 0x11u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_PREFETCH_CMD), 0x23c20000u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_PREFETCH_CNTL + 1), 0x23c20000u);   // ...in the same pkt4 as the CNTL
    // the tables in the pass's FS part, the counts in the registers
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_TEX_COUNT), 1u);
    const uint64_t texAt = uint64_t(e.reg(REG_A6XX_SP_FS_TEX_CONST)) | uint64_t(e.reg(REG_A6XX_SP_FS_TEX_CONST + 1)) << 32;
    const uint64_t sampAt = uint64_t(e.reg(REG_A6XX_SP_FS_TEX_SAMP)) | uint64_t(e.reg(REG_A6XX_SP_FS_TEX_SAMP + 1)) << 32;
    CHECK_EQ(texAt, e.table->gpuAddress() + render::kTableFsAt);
    CHECK_EQ(sampAt, e.table->gpuAddress() + render::kTableFsAt + 16 * 64);
    TexConst want = tex->sampledDescriptor();
    CHECK(memcmp(e.tableWords() + render::kTableFsAt / 4, want.dword, sizeof want.dword) == 0);
    CHECK(memcmp(e.tableWords() + (render::kTableFsAt + 16 * 64) / 4, smp.words, 16) == 0);
    CHECK_EQ((e.reg(REG_A6XX_SP_FS_CONFIG) & A6XX_SP_FS_CONFIG_NTEX__MASK) >> A6XX_SP_FS_CONFIG_NTEX__SHIFT, 1u);
    CHECK_EQ((e.reg(REG_A6XX_SP_FS_CONFIG) & A6XX_SP_FS_CONFIG_NSAMP__MASK) >> A6XX_SP_FS_CONFIG_NSAMP__SHIFT, 1u);
    // the two SB6_FS_TEX loads by CP_LOAD_STATE6_FRAG: samplers (ST6_SHADER, 1 unit) and textures (ST6_CONSTANTS, 1 unit)
    bool sampLoad = false, texLoad = false;
    for (size_t i = e.find(CP_LOAD_STATE6_FRAG); i != size_t(-1); i = e.find(CP_LOAD_STATE6_FRAG, i + 1)) {
        uint32_t w0 = e.stream.words()[i + 1];
        if (((w0 & CP_LOAD_STATE6_0_STATE_BLOCK__MASK) >> CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT) != SB6_FS_TEX) continue;
        uint32_t type = (w0 & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT;
        uint32_t units = (w0 & CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> CP_LOAD_STATE6_0_NUM_UNIT__SHIFT;
        sampLoad |= type == ST6_SHADER && units == 1 && e.stream.words()[i + 2] == uint32_t(sampAt);
        texLoad |= type == ST6_CONSTANTS && units == 1 && e.stream.words()[i + 2] == uint32_t(texAt);
    }
    CHECK(sampLoad && texLoad);
    // through the command buffer, the fake judging the tables and the prefetch
    auto cb = drawOnce(r, texProgram(), pass, wholeDraw(6), render::Options{}, Bindings{}, fsb);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.fsTextureCount, 1u);
    CHECK_EQ(r.fake->stats().lastDraw.fsSamplerCount, 1u);
    CHECK_EQ(r.fake->stats().lastDraw.prefetchCount, 1u);
    CHECK_EQ(r.fake->stats().lastDraw.prefetchCmd[0], 0x23c20000u);
    // a draw without textures writes the count 0 (and no prefetch)
    Encoded f(r, flatProgram(), pass, wholeDraw());
    CHECK_EQ(f.reg(REG_A6XX_SP_FS_TEX_COUNT), 0u);
    CHECK_EQ(f.reg(REG_A6XX_SP_FS_PREFETCH_CNTL) & A6XX_SP_FS_PREFETCH_CNTL_COUNT__MASK, 0u);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// --- textures in the vertex stage: the VS_TEX tables ----------------------------------------------------------------------

TEST(a_draw_samples_a_texture_in_the_vertex_stage) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    auto tex = target(r, 4, 1, kTextureUsageRead);
    auto other = target(r, 4, 1, kTextureUsageRead);
    Sampler smp = nearestClamp();
    RenderPass pass;
    pass.color0 = t.get();
    const Program p{kernels::tri_vtex_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_vtex"};
    const Draw d = wholeDraw(6);   // as triangles: the fake runs no points (the phone's probe 9vt draws them)
    Bindings vsb;
    vsb.textures = {tex.get()};
    vsb.samplers = {&smp};
    Encoded e(r, p, pass, d, render::Options{}, vsb, Bindings{});
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    // the VS's tables past the FS's in the pass's FS part, the count in its register, the FS's count 0
    CHECK_EQ(e.reg(REG_A6XX_SP_VS_TEX_COUNT), 1u);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_TEX_COUNT), 0u);
    const uint64_t texAt = uint64_t(e.reg(REG_A6XX_SP_VS_TEX_CONST)) | uint64_t(e.reg(REG_A6XX_SP_VS_TEX_CONST + 1)) << 32;
    const uint64_t sampAt = uint64_t(e.reg(REG_A6XX_SP_VS_TEX_SAMP)) | uint64_t(e.reg(REG_A6XX_SP_VS_TEX_SAMP + 1)) << 32;
    CHECK_EQ(texAt, e.table->gpuAddress() + render::kTableFsAt + 16 * 64 + 16 * 16);
    CHECK_EQ(sampAt, texAt + 16 * 64);
    // the descriptor is the bound texture's and not its twin's (same size and format, another address)
    TexConst want = tex->sampledDescriptor(), twin = other->sampledDescriptor();
    CHECK(memcmp(want.dword, twin.dword, sizeof want.dword) != 0);
    CHECK(memcmp(e.tableWords() + (texAt - e.table->gpuAddress()) / 4, want.dword, sizeof want.dword) == 0);
    CHECK(memcmp(e.tableWords() + (sampAt - e.table->gpuAddress()) / 4, smp.words, 16) == 0);
    CHECK_EQ((e.reg(REG_A6XX_SP_VS_CONFIG) & A6XX_SP_VS_CONFIG_NTEX__MASK) >> A6XX_SP_VS_CONFIG_NTEX__SHIFT, 1u);
    // the two SB6_VS_TEX loads go by CP_LOAD_STATE6_GEOM, none by _FRAG
    bool sampLoad = false, texLoad = false;
    for (size_t i = e.find(CP_LOAD_STATE6_GEOM); i != size_t(-1); i = e.find(CP_LOAD_STATE6_GEOM, i + 1)) {
        uint32_t w0 = e.stream.words()[i + 1];
        if (((w0 & CP_LOAD_STATE6_0_STATE_BLOCK__MASK) >> CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT) != SB6_VS_TEX) continue;
        uint32_t type = (w0 & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT;
        uint32_t units = (w0 & CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> CP_LOAD_STATE6_0_NUM_UNIT__SHIFT;
        sampLoad |= type == ST6_SHADER && units == 1 && e.stream.words()[i + 2] == uint32_t(sampAt);
        texLoad |= type == ST6_CONSTANTS && units == 1 && e.stream.words()[i + 2] == uint32_t(texAt);
    }
    CHECK(sampLoad && texLoad);
    // with the VS group switched off the fake must refuse the draw (no SP_VS_TEX_COUNT): its rule is not vacuous
    render::Options off;
    off.vsTextures = false;
    auto bad = drawOnce(r, p, pass, d, off, vsb, Bindings{});
    CHECK(bad->status() != CommandBuffer::kCompleted || r.fake->stats().faults != 0);
    expectRefusal(r.fake->stats().lastFault, "SP_VS_TEX_COUNT not written in this submission: the draw would sample the last owner's texture table");
    // through a command buffer on a rig of its own, the fake judging the VS tables
    Rig r2;
    auto t2 = target(r2);
    auto tex2 = target(r2, 4, 1, kTextureUsageRead);
    RenderPass pass2;
    pass2.color0 = t2.get();
    Bindings vsb2;
    vsb2.textures = {tex2.get()};
    vsb2.samplers = {&smp};
    auto cb = drawOnce(r2, p, pass2, d, render::Options{}, vsb2, Bindings{});
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r2.fake->stats().lastFault.c_str());
    CHECK_EQ(r2.fake->stats().faults, 0u);
    CHECK_EQ(r2.fake->stats().lastDraw.vsTextureCount, 1u);
    CHECK_EQ(r2.fake->stats().lastDraw.vsSamplerCount, 1u);
    CHECK_EQ(r2.fake->stats().lastDraw.fsTextureCount, 0u);
}

TEST(a_draw_refuses_a_texture_the_fs_cannot_sample) {
    Rig r;
    auto t = target(r);
    auto tex = target(r, 4, 4, kTextureUsageRead);
    auto ints = target(r, 4, 4, kTextureUsageRead, PixelFormat::kR32Uint);
    auto writeOnly = target(r, 4, 4, kTextureUsageWrite);
    Sampler smp = nearestClamp();
    RenderPass pass;
    pass.color0 = t.get();
    auto refusedWith = [&](const Program& p, const Bindings& fsb, const char* want) {
        Encoded e(r, p, pass, wholeDraw(6), render::Options{}, Bindings{}, fsb);
        CHECK(!e.ok);
        expectRefusal(e.error, want);
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->draw(p, pass, wholeDraw(6), Bindings{}, fsb));
        expectRefusal(cb->error(), want);
    };
    // nothing bound at all: the encoder alone refuses the texture slot; a command buffer puts the queue's null texture
    // there (uncommitted, so nothing is submitted); an unbound sampler is the all-zero sampler, in the encoder too
    Bindings none;
    {
        Encoded e(r, texProgram(), pass, wholeDraw(6), render::Options{}, Bindings{}, none);
        CHECK(!e.ok);
        expectRefusal(e.error, "tri_tex_fs_air: no texture bound at index 0");
        auto cb = r.queue->commandBuffer();
        CHECK(cb->draw(texProgram(), pass, wholeDraw(6), Bindings{}, none));
    }
    Bindings noSampler;
    noSampler.textures = {tex.get()};
    {
        Encoded e(r, texProgram(), pass, wholeDraw(6), render::Options{}, Bindings{}, noSampler);
        CHECK(e.ok);
        if (!e.ok) std::printf("       %s\n", e.error.c_str());
    }
    Bindings wrongClass;
    wrongClass.textures = {ints.get()};
    wrongClass.samplers = {&smp};
    refusedWith(texProgram(), wrongClass, "texture 0 is R32Uint (uint); tri_tex_fs_air reads it as texture2d<float>");
    Bindings unreadable;
    unreadable.textures = {writeOnly.get()};
    unreadable.samplers = {&smp};
    refusedWith(texProgram(), unreadable, "texture 0 was made without shader-read usage; tri_tex_fs_air samples it");
    // the prefetch's own slots: a texture index the FS does not declare, a bindless or unknown command
    Bindings ok;
    ok.textures = {tex.get()};
    ok.samplers = {&smp};
    Program p = texProgram();
    p.fs.prefetch[0].texId = 3;
    refusedWith(p, ok, "tri_tex_fs_air's prefetch 0 samples texture 3 with sampler 0: not a texture and a sampler the FS declares");
    p = texProgram();
    p.fs.prefetch[0].bindless = 1;
    refusedWith(p, ok, "tri_tex_fs_air's prefetch 0 is bindless: the textures here are SB6_FS_TEX tables");
    p = texProgram();
    p.fs.prefetch[0].cmd = 0;
    refusedWith(p, ok, "tri_tex_fs_air's prefetch 0 is not a plain sample (TEX_PREFETCH_SAM): no other prefetch is written here");
    CHECK_EQ(r.fake->stats().submits, 0u);
}

namespace {
// the textured draw encoded over a fresh rig, its words to mutate
struct TexTwin {
    explicit TexTwin(const render::Options& o = render::Options{}) : t(target(r)), tex(target(r, 4, 4, kTextureUsageRead)) {
        smp = nearestClamp();
        pass.color0 = t.get();
        fsb.textures = {tex.get()};
        fsb.samplers = {&smp};
        e = std::make_unique<Encoded>(r, texProgram(), pass, wholeDraw(6), o, Bindings{}, fsb);
        CHECK(e->ok);
        words = e->stream.words();
    }
    uint32_t& at(uint32_t reg) {
        size_t i = e->findReg(reg);
        CHECK(i != size_t(-1));
        return words[i == size_t(-1) ? 0 : i];
    }
    std::string fault() { return faultOf(r, words); }
    Rig r;
    std::shared_ptr<Texture> t, tex;
    Sampler smp;
    RenderPass pass;
    Bindings fsb;
    std::unique_ptr<Encoded> e;
    std::vector<uint32_t> words;
};
}  // namespace

TEST(the_fake_refuses_fs_textures_or_a_prefetch_that_do_not_hold_together) {
    using namespace a6xx;
    {
        TexTwin w;
        Raw raw(w.r);
        CHECK_EQ(raw.submit(w.words), 0);
        CHECK_EQ(w.r.fake->stats().lastDraw.prefetchCount, 1u);
    }
    {
        render::Options o;
        o.fsTextures = false;
        TexTwin w(o);
        CHECK(has(w.fault(), "SP_FS_TEX_COUNT not written in this submission"));
    }
    {
        TexTwin w;
        w.at(REG_A6XX_SP_FS_TEX_COUNT) = 2;
        CHECK(has(w.fault(), "the FS's texture descriptors were not loaded from where SP_FS_TEX_CONST points, or not as many as SP_FS_TEX_COUNT says"));
    }
    {
        TexTwin w;
        w.at(REG_A6XX_SP_FS_TEX_SAMP) += 16;
        CHECK(has(w.fault(), "the FS's sampler descriptors were not loaded from where SP_FS_TEX_SAMP points"));
    }
    {
        TexTwin w;
        w.at(REG_A6XX_SP_FS_CONFIG) &= ~A6XX_SP_FS_CONFIG_NSAMP__MASK;
        CHECK(has(w.fault(), "SP_FS_CONFIG names 1 textures and 0 samplers; SP_FS_TEX_COUNT says 1"));
    }
    {
        TexTwin w;
        w.at(REG_A6XX_SP_FS_PREFETCH_CNTL + 1) = spFsPrefetchCmd(0, 0, 1, 2, 0xf, false, TEX_PREFETCH_SAM);
        CHECK(has(w.fault(), "SP_FS_PREFETCH_CMD[0] samples texture 1 with sampler 0: past the loaded tables (1 textures, 1 samplers)"));
    }
    {
        TexTwin w;
        w.at(REG_A6XX_SP_FS_PREFETCH_CNTL + 1) |= A6XX_SP_FS_PREFETCH_CMD_BINDLESS;
        CHECK(has(w.fault(), "is not a plain, non-bindless sample (TEX_PREFETCH_SAM)"));
    }
    {
        TexTwin w;
        w.at(REG_A6XX_SP_FS_PREFETCH_CNTL + 1) &= ~A6XX_SP_FS_PREFETCH_CMD_WRMASK__MASK;
        CHECK(has(w.fault(), "SP_FS_PREFETCH_CMD[0] writes components 0 into register 0x2: nothing, or no register"));
    }
    {
        // the texture's descriptor zeroed in the table after encoding: the prefetch samples a slot nothing is bound at
        TexTwin w;
        memset(static_cast<uint8_t*>(w.e->table->contents()) + render::kTableFsAt, 0, 64);
        CHECK(has(w.fault(), "SP_FS_PREFETCH_CMD[0] samples texture 0, whose descriptor is zero"));
    }
}

// --- depth: a Depth32Float attachment, the depth test, its Clear and its flush ----------------------------------------------

namespace {
std::shared_ptr<Texture> depthTarget(Rig& r, uint32_t w = 64, uint32_t h = 64, uint32_t usage = kTextureUsageRenderTarget) {
    return target(r, w, h, usage, PixelFormat::kDepth32Float);
}
}  // namespace

TEST(a_depth32float_texture_is_tiled_and_an_attachment_only) {
    Rig r;
    auto d = depthTarget(r, 48, 20);
    CHECK(d != nullptr);
    CHECK_EQ(d->bytesPerRow(), 256u);               // 48 * 4 rounded to 64 pixels
    CHECK_EQ(d->buffer()->length(), 8192u);         // 20 rows counted to 32 (the tiled height alignment of 16), 4096-aligned
    uint32_t px = 0;
    std::string why;
    CHECK(!d->replace(0, 0, 1, 1, &px, 4, &why));
    CHECK(has(why, "a Depth32Float texture is tiled (TILE6_3): its bytes are not rows the CPU can address"));
    CHECK(!d->read(0, 0, 1, 1, &px, 4, &why));
    TextureDesc td;
    td.format = PixelFormat::kDepth32Float;
    td.width = td.height = 16;
    td.usage = kTextureUsageRenderTarget | kTextureUsageWrite;
    CHECK(Texture::make(*r.gpu, td, &why) == nullptr);
    CHECK(has(why, "a Depth32Float texture with usage 6: a depth texture is tiled (TILE6_3) and is served as a render target (4), sampled too (5), not stored"));
    // sampled too: its view is the tiled layout, TILE6_3 with TILE_ALL, FMT6_32_FLOAT, at the texture's own address
    {
        using namespace a6xx;
        td.usage = kTextureUsageRenderTarget | kTextureUsageRead;
        auto sd = Texture::make(*r.gpu, td, &why);
        CHECK(sd != nullptr);
        if (sd != nullptr) {
            const TexConst c = sd->sampledDescriptor();
            CHECK_EQ((c.dword[0] & A6XX_TEX_CONST_0_TILE_MODE__MASK) >> A6XX_TEX_CONST_0_TILE_MODE__SHIFT, uint32_t(TILE6_3));
            CHECK((c.dword[3] & A6XX_TEX_CONST_3_TILE_ALL) != 0);
            CHECK_EQ((c.dword[0] & A6XX_TEX_CONST_0_FMT__MASK) >> A6XX_TEX_CONST_0_FMT__SHIFT, uint32_t(FMT6_32_FLOAT));
            CHECK_EQ(uint64_t(c.dword[4]), uint64_t(uint32_t(sd->gpuAddress())));
        }
    }
}

TEST(a_draw_tests_and_writes_depth_through_its_attachment) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    auto d = depthTarget(r);
    RenderPass pass;
    pass.color0 = t.get();
    pass.depth = d.get();
    pass.depthLoad = LoadAction::kClear;
    pass.clearDepth = 1.0;
    Draw dr = wholeDraw();
    dr.depth = DepthState{1, true};   // Less, write
    Encoded e(r, flatProgram(), pass, dr);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    // the attachment, as literals: DEPTH6_32 (4), pitch 256 >> 6, layer 16384 >> 6, its base, BASE_GMEM 0
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_BUFFER_INFO), 4u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_BUFFER_PITCH), 4u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH), 256u);
    CHECK_EQ(uint64_t(e.reg(REG_A6XX_RB_DEPTH_BUFFER_BASE)) | uint64_t(e.reg(REG_A6XX_RB_DEPTH_BUFFER_BASE + 1)) << 32, d->gpuAddress());
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO), 4u);
    // the test: Z_TEST 1 | Z_WRITE 2 | ZFUNC(Less 1) << 2 | Z_READ 0x40; the GRAS copy; EARLY_Z (no kill)
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_CNTL), 0x47u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_DEPTH_CNTL), 1u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_PLANE_CNTL), 0u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL), 0u);
    // the depth Clear: one CP_BLIT into FMT6_32_FLOAT (74) TILE6_3, R2D_FLOAT32 (4), SOLID_C0 the bits of 1.0f
    size_t blit = e.find(CP_BLIT);
    CHECK(blit != size_t(-1) && e.find(CP_BLIT, blit + 1) == size_t(-1));
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_DST_INFO), 74u | 3u << 8);
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_BLIT_CNTL), 0x04f04a80u);
    CHECK_EQ(e.reg(REG_A6XX_RB_2D_SRC_SOLID_C0), 0x3f800000u);
    CHECK_EQ(uint64_t(e.reg(REG_A6XX_RB_2D_DST)) | uint64_t(e.reg(REG_A6XX_RB_2D_DST + 1)) << 32, d->gpuAddress());
    // through the command buffer: the fake judges the attachment and wants the depth flushed at the end
    auto cb = drawOnce(r, flatProgram(), pass, dr);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().tiledFills, 1u);
    CHECK_EQ(r.fake->stats().lastDraw.depthCntl, 0x47u);
    CHECK_EQ(r.fake->stats().lastDraw.depthBase, d->gpuAddress());
    // Metal's default depth state with the attachment bound: the test off -- ZFUNC Always (7) alone, LATE_Z
    Encoded off(r, flatProgram(), pass, wholeDraw());
    CHECK(off.ok);
    CHECK_EQ(off.reg(REG_A6XX_RB_DEPTH_CNTL), 7u << 2);
    CHECK_EQ(off.reg(REG_A6XX_GRAS_SU_DEPTH_CNTL), 0u);
    CHECK_EQ(off.reg(REG_A6XX_RB_DEPTH_PLANE_CNTL), uint32_t(A6XX_LATE_Z));
    // a pass that only clears the depth (the colour loads): a pass of its own
    RenderPass depthOnly = pass;
    depthOnly.load = LoadAction::kLoad;
    auto c = r.queue->commandBuffer();
    CHECK(c->clear(depthOnly));
    c->commit();
    c->waitUntilCompleted();
    CHECK(c->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().tiledFills, 2u);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// A pass of depth alone (a shadow map's; SpringBoard's Siri orb has one): no colour attachment, so the window is the
// depth texture's, the FS's [[color(0)]] is written nowhere (RB_MRT_CONTROL 0, no output register), and the MRT
// registers name the depth texture's memory, which nothing lets them write. The control first: the same draw with a colour attachment writes colour.
TEST(a_pass_of_depth_alone_draws_depth) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    auto d = depthTarget(r);
    RenderPass withColour;
    withColour.color0 = t.get();
    withColour.depth = d.get();
    withColour.depthLoad = LoadAction::kClear;
    withColour.clearDepth = 1.0;
    Draw dr = wholeDraw();
    dr.depth = DepthState{1, true};
    Encoded c(r, flatProgram(), withColour, dr);
    CHECK(c.ok);
    CHECK(c.reg(REG_A6XX_RB_MRT_CONTROL) != 0u);
    CHECK_EQ(uint64_t(c.reg(REG_A6XX_RB_MRT_BASE)) | uint64_t(c.reg(REG_A6XX_RB_MRT_BASE + 1)) << 32, t->gpuAddress());
    RenderPass pass = withColour;
    pass.color0 = nullptr;
    Encoded e(r, flatProgram(), pass, dr);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    CHECK_EQ(e.reg(REG_A6XX_RB_MRT_CONTROL), 0u);
    CHECK_EQ(uint64_t(e.reg(REG_A6XX_RB_MRT_BASE)) | uint64_t(e.reg(REG_A6XX_RB_MRT_BASE + 1)) << 32, d->gpuAddress());   // never written
    CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_CNTL), 0x47u);
    CHECK_EQ(uint64_t(e.reg(REG_A6XX_RB_DEPTH_BUFFER_BASE)) | uint64_t(e.reg(REG_A6XX_RB_DEPTH_BUFFER_BASE + 1)) << 32, d->gpuAddress());
    // the window is the depth texture's 64 x 64: the scissor's bottom right (63, 63)
    CHECK_EQ(e.reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR), A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X(63) | A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y(63));
    auto cb = drawOnce(r, flatProgram(), pass, dr);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.depthBase, d->gpuAddress());
    CHECK_EQ(r.fake->stats().faults, 0u);
    // with no attachment at all there is nothing to draw into
    RenderPass none;
    Encoded n(r, flatProgram(), none, wholeDraw());
    CHECK(!n.ok);
    expectRefusal(n.error, "a render pass with no attachment at all: nothing to draw into");
}

TEST(a_draw_refuses_a_depth_setup_it_cannot_serve) {
    Rig r;
    auto t = target(r);
    auto d = depthTarget(r);
    auto small = depthTarget(r, 32, 32);
    auto colour = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Draw dr = wholeDraw();
    dr.depth = DepthState{1, true};
    refused(r, flatProgram(), pass, dr, "a depth test (compare 1, write) with no depth attachment");
    pass.depth = colour.get();
    refused(r, flatProgram(), pass, dr, "a RGBA8Unorm texture as the depth attachment: Depth32Float (252) is served");
    pass.depth = small.get();
    refused(r, flatProgram(), pass, dr, "a 32 x 32 depth attachment (level 0) beside a 64 x 64 colour attachment (level 0): a depth attachment smaller than the colour leaves pixels without one");
    pass.depth = d.get();
    Draw bad = dr;
    bad.depth.compare = 8;
    refused(r, flatProgram(), pass, bad, "depth compare function 8: MTLCompareFunction runs Never 0 to Always 7");
    RenderPass far = pass;
    far.depthLoad = LoadAction::kClear;
    far.clearDepth = 2.0;
    refused(r, flatProgram(), far, dr, "a clear depth of 2.000000: Metal's clearDepth is 0 to 1");
    RenderPass swapped;
    swapped.color0 = d.get();
    refused(r, flatProgram(), swapped, wholeDraw(), "a Depth32Float texture as colour attachment 0: a depth format is the depth attachment's");
    // a pass without a draw that clears nothing
    auto cb = r.queue->commandBuffer();
    CHECK(!cb->clear(pass));
    expectRefusal(cb->error(), "a pass without a draw and with load action 1: only a Clear writes anything then");
}

TEST(the_fake_refuses_a_depth_draw_that_does_not_hold_together) {
    using namespace a6xx;
    auto twin = [](const render::Options& o, auto mutate) {
        Rig r;
        auto t = target(r);
        auto d = depthTarget(r);
        RenderPass pass;
        pass.color0 = t.get();
        pass.depth = d.get();
        Draw dr = wholeDraw();
        dr.depth = DepthState{1, true};
        Encoded e(r, flatProgram(), pass, dr, o);
        CHECK(e.ok);
        std::vector<uint32_t> w = e.stream.words();
        mutate(e, w);
        return faultOf(r, w);
    };
    auto none = [](Encoded&, std::vector<uint32_t>&) {};
    {
        render::Options o;
        o.tailDepthFlush = false;
        CHECK(has(twin(o, none), "the depth stays in the CCU: no PC_CCU_FLUSH_DEPTH_TS (4-word) after a draw that writes depth"));
    }
    CHECK(has(twin(render::Options{}, [](Encoded& e, std::vector<uint32_t>& w) { w[e.findReg(REG_A6XX_GRAS_SU_DEPTH_CNTL)] = 0; }),
              "the test enable must agree, and a write needs the test"));
    CHECK(has(twin(render::Options{}, [](Encoded& e, std::vector<uint32_t>& w) { w[e.findReg(REG_A6XX_RB_DEPTH_BUFFER_BASE)] += 64; }),
              "not 4096-aligned, not a multiple of 256 a sample, narrower than the window, or outside every buffer"));
    CHECK(has(twin(render::Options{}, [](Encoded& e, std::vector<uint32_t>& w) { w[e.findReg(REG_A6XX_RB_DEPTH_BUFFER_PITCH)] = 2; }),
              "not 4096-aligned, not a multiple of 256 a sample, narrower than the window, or outside every buffer"));
    CHECK(has(twin(render::Options{}, [](Encoded& e, std::vector<uint32_t>& w) { w[e.findReg(REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO)] = 0; }),
              "depth format 4 in RB_DEPTH_BUFFER_INFO and 0 in GRAS_SU_DEPTH_BUFFER_INFO"));
    // unmutated, the same stream passes (each refusal above is its mutation's)
    {
        Rig r;
        auto t = target(r);
        auto d = depthTarget(r);
        RenderPass pass;
        pass.color0 = t.get();
        pass.depth = d.get();
        Draw dr = wholeDraw();
        dr.depth = DepthState{1, true};
        Encoded e(r, flatProgram(), pass, dr);
        Raw raw(r);
        CHECK_EQ(raw.submit(e.stream.words()), 0);
        CHECK_EQ(e.tableWords()[render::kTableDepthFlushAt / 4], render::kDepthFlushValue);
    }
}

// --- a texture over an IOSurface: the path to the screen's memory -----------------------------------------------------------

TEST(a_texture_over_an_iosurface_draws_into_the_surfaces_memory) {
    Rig r;
    const uint32_t kBGRA = 0x42475241u, kRGBA = 0x52474241u;
    r.fake->addSurface(7, 64, 64, 256, kBGRA);
    TextureDesc d;
    d.format = PixelFormat::kBGRA8Unorm;
    d.width = d.height = 64;
    d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    std::string why;
    uint32_t before = r.fake->liveBuffers();
    auto t = Texture::wrapSurface(*r.gpu, 7, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK_EQ(t->surface(), 7u);
    CHECK_EQ(t->bytesPerRow(), 256u);
    CHECK_EQ(r.fake->liveBuffers(), before + 1);
    // the same surface again: the same buffer, as the kernel hands out the same handle
    auto t2 = Texture::wrapSurface(*r.gpu, 7, d, &why);
    CHECK(t2 != nullptr && t2->buffer() == t->buffer());
    CHECK_EQ(r.fake->liveBuffers(), before + 1);
    // a Clear of it lands in the surface: BGRA in memory, B first
    RenderPass pass;
    pass.color0 = t.get();
    pass.load = LoadAction::kClear;
    pass.clearColor[0] = 0.25;
    pass.clearColor[1] = 0.5;
    pass.clearColor[2] = 0.75;
    pass.clearColor[3] = 1.0;
    auto cb = r.queue->commandBuffer();
    CHECK(cb->clear(pass));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    std::vector<uint8_t> px = r.fake->surfaceBytes(7);
    const uint8_t want[4] = {0xbf, 0x80, 0x40, 0xff};
    bool all = px.size() >= 64 * 256;
    for (uint32_t y = 0; all && y < 64; y++)
        for (uint32_t x = 0; all && x < 64; x++) all = memcmp(px.data() + y * 256 + x * 4, want, 4) == 0;
    CHECK(all);
    // the surface outlives its buffer: both textures and the command buffer that kept it gone, the handle closed once,
    // the pixels kept
    cb.reset();
    const uint32_t withSurface = r.fake->liveBuffers();   // (the queue keeps buffers of its own past a command buffer)
    t.reset();
    CHECK_EQ(r.fake->liveBuffers(), withSurface);         // the other texture holds the surface's buffer still
    t2.reset();
    CHECK_EQ(r.fake->liveBuffers(), withSurface - 1);
    px = r.fake->surfaceBytes(7);
    CHECK(px.size() >= 4 && memcmp(px.data(), want, 4) == 0);
    // ...and a surface whose rows start at an offset and are 320 bytes apart (64-aligned, not 256-aligned): the Clear
    // lands from the offset on, the bytes before it untouched, the descriptor's PITCHALIGN 0
    r.fake->addSurface(8, 48, 20, 320, kRGBA, 128);
    TextureDesc e;
    e.format = PixelFormat::kRGBA8Unorm;
    e.width = 48;
    e.height = 20;
    e.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    auto o = Texture::wrapSurface(*r.gpu, 8, e, &why);
    CHECK(o != nullptr);
    if (o != nullptr) {
        CHECK_EQ(o->gpuAddress(), o->buffer()->gpuAddress() + 128);
        CHECK_EQ((o->sampledDescriptor().dword[2] & a6xx::A6XX_TEX_CONST_2_PITCHALIGN__MASK) >> a6xx::A6XX_TEX_CONST_2_PITCHALIGN__SHIFT, 0u);
        RenderPass p2;
        p2.color0 = o.get();
        p2.load = LoadAction::kClear;
        p2.clearColor[1] = 1.0;
        p2.clearColor[3] = 1.0;
        auto c2 = r.queue->commandBuffer();
        CHECK(c2->clear(p2));
        c2->commit();
        c2->waitUntilCompleted();
        std::vector<uint8_t> q = r.fake->surfaceBytes(8);
        const uint8_t green[4] = {0, 0xff, 0, 0xff};
        bool rows = true;
        for (uint32_t y = 0; rows && y < 20; y++)
            for (uint32_t x = 0; rows && x < 48; x++) rows = memcmp(q.data() + 128 + y * 320 + x * 4, green, 4) == 0;
        CHECK(rows);
        bool head = true;
        for (uint32_t i = 0; i < 128; i++) head = head && q[i] == 0;
        CHECK(head);
        CHECK_EQ(q[128 + 48 * 4], 0u);   // past the width in the first row: not the texture's
    }
}

// CoreAnimation's YCbCr8_420_2P (500) over a '420f' surface: one texture of both planes, the luma at the surface's
// offset and the chroma where the caller says, in the descriptor as fdl6_view_init writes NV12 -- FMT
// R8_G8B8_2PLANE_420_UNORM, the chroma pitch in dword 6, its address in 7 and 8 -- and refused when it is not that.
TEST(a_two_plane_surface_is_one_ycbcr_texture) {
    using namespace a6xx;
    Rig r;
    const uint32_t k420f = 0x34323066u, kBGRA = 0x42475241u;
    r.fake->addSurface(11, 64, 64, 128, k420f, 0, 2);
    r.fake->addSurface(12, 64, 64, 128, kBGRA);
    TextureDesc d;
    d.format = PixelFormat::kYCbCr8_420_2P;
    d.width = d.height = 64;
    d.usage = kTextureUsageRead;
    const Texture::SurfacePlane chroma{128 * 64, 128};
    std::string why;
    auto t = Texture::wrapSurface(*r.gpu, 11, d, &why, &chroma);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::fprintf(stderr, "  %s\n", why.c_str());
        return;
    }
    const TexConst c = t->sampledDescriptor();
    CHECK_EQ((c.dword[0] & A6XX_TEX_CONST_0_FMT__MASK) >> A6XX_TEX_CONST_0_FMT__SHIFT, uint32_t(FMT6_R8_G8B8_2PLANE_420_UNORM));
    CHECK_EQ(c.dword[0] & A6XX_TEX_CONST_0_MIPLVLS__MASK, 0u);
    CHECK_EQ(c.dword[4], uint32_t(t->gpuAddress()));
    CHECK_EQ(c.dword[6], 128u << 8);
    CHECK_EQ(c.dword[7], uint32_t(t->buffer()->gpuAddress() + 128 * 64));
    CHECK_EQ(c.dword[8], uint32_t((t->buffer()->gpuAddress() + 128 * 64) >> 32));
    // refused: no second plane, a one-plane surface, a usage it does not serve, a chroma plane off the end, a texture
    // of its own memory, and a CPU write
    auto refusedWith = [&](uint32_t id, const TextureDesc& desc, const Texture::SurfacePlane* p, const char* want) {
        std::string w;
        CHECK(Texture::wrapSurface(*r.gpu, id, desc, &w, p) == nullptr);
        CHECK(has(w, want));
        if (!has(w, want)) std::fprintf(stderr, "  got \"%s\"\n", w.c_str());
    };
    refusedWith(11, d, nullptr, "with no second plane given");
    refusedWith(12, d, &chroma, "a two-plane '420f' or '420v' surface and its second plane are what it takes");
    TextureDesc rt = d;
    rt.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    refusedWith(11, rt, &chroma, "one level, read by shaders, is what it serves");
    const Texture::SurfacePlane far{128 * 64 * 2, 128};
    refusedWith(11, d, &far, "chroma rows of 128 from 16384");
    CHECK(Texture::make(*r.gpu, d, &why) == nullptr);
    CHECK(has(why, "served over a two-plane IOSurface alone"));
    uint8_t px[4] = {};
    CHECK(!t->replace(0, 0, 0, 1, 1, px, 4, &why));
    CHECK(has(why, "two planes of different sizes"));
}

TEST(a_texture_over_an_iosurface_refuses_what_the_gpu_cannot_take) {
    Rig r;
    const uint32_t kBGRA = 0x42475241u, kRGBA = 0x52474241u;
    r.fake->addSurface(1, 64, 64, 100 * 4 + 12, kBGRA);   // 412-byte rows: not a multiple of 64
    r.fake->addSurface(2, 64, 64, 256, kRGBA);
    r.fake->addSurface(3, 64, 64, 256, kBGRA, 0, 2);
    r.fake->addSurface(4, 64, 64, 256, kBGRA, 0, 1, true);
    r.fake->addSurface(5, 32, 32, 128, kBGRA);
    TextureDesc d;
    d.format = PixelFormat::kBGRA8Unorm;
    d.width = d.height = 64;
    d.usage = kTextureUsageRenderTarget;
    auto refusedWith = [&](uint32_t id, const TextureDesc& desc, const char* want) {
        std::string why;
        CHECK(Texture::wrapSurface(*r.gpu, id, desc, &why) == nullptr);
        CHECK(has(why, want));
        if (!has(why, want)) std::fprintf(stderr, "  got \"%s\"\n", why.c_str());
    };
    refusedWith(99, d, "IOSurface 99: the driver gave no buffer for it: no such surface");
    refusedWith(4, d, "a surface around a program's own memory is not one the GPU takes");
    refusedWith(1, d, "IOSurface 1's rows of 412 bytes from offset 0");
    // a four-byte surface reads in any four-byte colour format, its bytes as they are (an 'RGBA' one as BGRA8Unorm has
    // red and blue the other way round, as on an Apple GPU); a format of another size is refused
    {
        std::string w;
        CHECK(Texture::wrapSurface(*r.gpu, 2, d, &w) != nullptr);
        TextureDesc two = d;
        two.format = PixelFormat::kRG8Unorm;
        refusedWith(2, two, "IOSurface 2's pixel format 1380401729 is not RG8Unorm's");
    }
    refusedWith(3, d, "IOSurface 3 has 2 planes: a single-plane surface is what is served");
    refusedWith(5, d, "a 64 x 64 texture over the 32 x 32 IOSurface 5");
    // a luminance-and-alpha mask surface ('LA08') is RG8Unorm's, and no other format's
    r.fake->addSurface(6, 64, 64, 128, 0x4c413038u);
    TextureDesc la = d;
    la.format = PixelFormat::kRG8Unorm;
    {
        std::string w;
        CHECK(Texture::wrapSurface(*r.gpu, 6, la, &w) != nullptr);
        if (!w.empty()) std::fprintf(stderr, "  %s\n", w.c_str());
    }
    refusedWith(6, d, "IOSurface 6's pixel format 1279340600 is not BGRA8Unorm's");
    TextureDesc z = d;
    z.format = PixelFormat::kDepth32Float;
    refusedWith(5, z, "a depth texture is tiled, a surface's rows are linear");
    // a refused wrap leaves no buffer open (the surface's own was closed again)
    CHECK_EQ(r.fake->liveBuffers(), 0u);
}

// --- indexed and instanced draws ---------------------------------------------------------------------------------------------

namespace {
std::shared_ptr<Buffer> bufferWith(Rig& r, const void* bytes, uint64_t length) {
    std::string why;
    auto b = newBuffer(*r.gpu, length, &why);
    CHECK(b != nullptr);
    if (b != nullptr) memcpy(b->contents(), bytes, length);
    return b;
}
}  // namespace

TEST(an_indexed_draw_is_the_seven_word_form_with_primitive_restart) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    // four 12-byte vertices (tri_attr's layout) and the quad as two triangles through six 16-bit indices
    uint8_t verts[4 * 12] = {};
    auto vb = bufferWith(r, verts, sizeof verts);
    const uint16_t idx16[6] = {0, 1, 2, 2, 1, 3};
    auto ib = bufferWith(r, idx16, sizeof idx16);
    Bindings vsb;
    vsb.buffers = {Binding{vb.get(), 0}};
    Draw d = wholeDraw(6);
    d.indexBuffer = ib.get();
    d.indexType = 0;
    Encoded e(r, attrProgram(), pass, d, render::Options{}, vsb);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    size_t at = e.find(CP_DRAW_INDX_OFFSET);
    CHECK(at != size_t(-1));
    if (at != size_t(-1)) {
        const auto& w = e.stream.words();
        CHECK_EQ(pm4::decode(w[at]).count, 7u);
        CHECK_EQ(w[at + 1], 0x404u);   // TRILIST 4, DMA 0 << 6, IGNORE_VISIBILITY 0 << 8, INDEX4_SIZE_16_BIT 1 << 10
        CHECK_EQ(w[at + 2], 1u);       // instances
        CHECK_EQ(w[at + 3], 6u);       // indices
        CHECK_EQ(w[at + 4], 0u);       // FIRST_INDX
        CHECK_EQ(uint64_t(w[at + 5]) | uint64_t(w[at + 6]) << 32, ib->gpuAddress());
        CHECK_EQ(w[at + 7], uint32_t(ib->length() / 2));   // MAX_INDICES: what the buffer holds
    }
    CHECK_EQ(e.reg(REG_A6XX_PC_PRIMITIVE_CNTL_0), A6XX_PC_PRIMITIVE_CNTL_0_PRIMITIVE_RESTART);
    CHECK_EQ(e.reg(REG_A6XX_PC_RESTART_INDEX), 0xffffu);
    CHECK_EQ(e.reg(REG_A6XX_VFD_INDEX_OFFSET), 0u);
    auto cb = drawOnce(r, attrProgram(), pass, d, render::Options{}, vsb);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK(r.fake->stats().lastDraw.indexed);
    // 32-bit indices from an offset, a base vertex: INDEX4_SIZE_32_BIT, the base past the offset, the restart all-ones
    const uint32_t idx32[4] = {9, 0, 1, 2};
    auto ib32 = bufferWith(r, idx32, sizeof idx32);
    Draw d32 = wholeDraw(3);
    d32.indexBuffer = ib32.get();
    d32.indexType = 1;
    d32.indexOffset = 4;
    d32.baseVertex = 1;
    uint8_t five[5 * 12] = {};
    auto vb5 = bufferWith(r, five, sizeof five);
    Bindings vsb5;
    vsb5.buffers = {Binding{vb5.get(), 0}};
    Encoded e32(r, attrProgram(), pass, d32, render::Options{}, vsb5);
    CHECK(e32.ok);
    size_t at32 = e32.find(CP_DRAW_INDX_OFFSET);
    if (at32 != size_t(-1)) {
        const auto& w = e32.stream.words();
        CHECK_EQ(w[at32 + 1], 0x804u);
        CHECK_EQ(uint64_t(w[at32 + 5]) | uint64_t(w[at32 + 6]) << 32, ib32->gpuAddress() + 4);
        CHECK_EQ(w[at32 + 7], 3u);
    }
    CHECK_EQ(e32.reg(REG_A6XX_PC_RESTART_INDEX), 0xffffffffu);
    CHECK_EQ(e32.reg(REG_A6XX_VFD_INDEX_OFFSET), 1u);
    auto cb32 = drawOnce(r, attrProgram(), pass, d32, render::Options{}, vsb5);
    CHECK(cb32->status() == CommandBuffer::kCompleted);
    // an index past what the vertex buffer holds (index 9 + base 1 = vertex 10 of 5) is the fake's to catch: it reads
    // the indices; drawn from offset 0 the 9 is in the draw
    Draw bad = d32;
    bad.indexOffset = 0;
    auto cbBad = drawOnce(r, attrProgram(), pass, bad, render::Options{}, vsb5);
    CHECK(cbBad->status() == CommandBuffer::kError);
    CHECK(has(r.fake->stats().lastFault, "vertex attribute 0: vertex 10's element ends at byte"));
    // a draw without indices leaves restart off
    Encoded plain(r, attrProgram(), pass, wholeDraw(3), render::Options{}, vsb);
    CHECK_EQ(plain.reg(REG_A6XX_PC_PRIMITIVE_CNTL_0), 0u);
}

TEST(an_indexed_or_instanced_draw_refuses_what_it_cannot_serve) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    uint8_t verts[6 * 12] = {};
    auto vb = bufferWith(r, verts, sizeof verts);
    const uint16_t idx[3] = {0, 1, 2};
    auto ib = bufferWith(r, idx, sizeof idx);
    Bindings vsb;
    vsb.buffers = {Binding{vb.get(), 0}};
    auto refusedWith = [&](const Program& p, const Draw& d, const char* want) {
        Encoded e(r, p, pass, d, render::Options{}, vsb);
        CHECK(!e.ok);
        expectRefusal(e.error, want);
    };
    Draw d = wholeDraw(3);
    d.indexBuffer = ib.get();
    Draw x = d;
    x.indexType = 2;
    refusedWith(attrProgram(), x, "index type 2: MTLIndexType is UInt16 (0) or UInt32 (1)");
    x = d;
    x.indexOffset = 1;
    refusedWith(attrProgram(), x, "an index buffer offset of 1: a multiple of the index size (2) inside the buffer");
    x = d;
    x.vertexCount = ib->length() / 2 + 1;
    refusedWith(attrProgram(), x, ("a draw of " + std::to_string(ib->length() / 2 + 1) + " indices from an index buffer holding " + std::to_string(ib->length() / 2) +
                                   " past its offset").c_str());
    x = d;
    x.baseVertex = -1;
    refusedWith(attrProgram(), x, "a base vertex of -1: a negative one is not served (VFD_INDEX_OFFSET is added unsigned)");
    // per instance: the last instance step past the buffer
    Program inst = attrProgram();
    inst.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerInstance, 1};
    Draw many = wholeDraw(3);
    many.instanceCount = 7;
    refusedWith(inst, many, "tri_attr_vs_air: [[attribute(0)]] of instance step 6 ends at byte 80 of buffer 0; 72 are bound");
    // ...while a step rate of 2 over the same 7 instances reads steps 0 to 3: inside
    inst.vertexDescriptor.layouts[0].stepRate = 2;
    Encoded ok(r, inst, pass, many, render::Options{}, vsb);
    CHECK(ok.ok);
    CHECK_EQ(ok.reg(a6xx::REG_A6XX_VFD_DECODE_INSTR) & a6xx::A6XX_VFD_DECODE_INSTR_INSTANCED, a6xx::A6XX_VFD_DECODE_INSTR_INSTANCED);
    CHECK_EQ(ok.reg(a6xx::regVfdDecodeStepRate(0)), 2u);
    size_t at = ok.find(a6xx::CP_DRAW_INDX_OFFSET);
    CHECK(at != size_t(-1) && ok.stream.words()[at + 2] == 7u);
    auto cb = drawOnce(r, inst, pass, many, render::Options{}, vsb);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().lastDraw.numInstances, 7u);
}

// --- an indexed constant buffer: an ldc through a UBO descriptor --------------------------------------------------------------

TEST(a_draw_loads_the_ubo_descriptors_an_ldc_reads_through) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
    auto pos = bufferWith(r, xy, sizeof xy);
    Program p{kernels::tri_upos_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_upos"};
    CHECK_EQ(p.vs.k.numUbos, 1u);
    Bindings vsb;
    vsb.buffers = {Binding{pos.get(), 0}};
    Encoded e(r, p, pass, wholeDraw(6), render::Options{}, vsb);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    // the load: CP_LOAD_STATE6_GEOM, ST6_UBO, SS6_DIRECT, SB6_VS_SHADER, one unit, then the descriptor: the buffer's
    // address and, above its high bits, its length in vec4 -- the buffer object's whole page (4096 / 16 = 256) is what
    // bufferWith's length is not: the length asked for is 48 bytes, 3 vec4
    bool found = false;
    for (size_t i = e.find(CP_LOAD_STATE6_GEOM); i != size_t(-1); i = e.find(CP_LOAD_STATE6_GEOM, i + 1)) {
        const auto& w = e.stream.words();
        const uint32_t w0 = w[i + 1];
        if (((w0 & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) != ST6_UBO) continue;
        found = true;
        CHECK_EQ((w0 & CP_LOAD_STATE6_0_STATE_BLOCK__MASK) >> CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT, uint32_t(SB6_VS_SHADER));
        CHECK_EQ((w0 & CP_LOAD_STATE6_0_STATE_SRC__MASK) >> CP_LOAD_STATE6_0_STATE_SRC__SHIFT, uint32_t(SS6_DIRECT));
        CHECK_EQ((w0 & CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> CP_LOAD_STATE6_0_NUM_UNIT__SHIFT, 1u);
        CHECK_EQ(w[i + 4], uint32_t(pos->gpuAddress()));
        CHECK_EQ(w[i + 5], uint32_t(pos->gpuAddress() >> 32) | 3u << 17);
    }
    CHECK(found);
    auto cb = drawOnce(r, p, pass, wholeDraw(6), render::Options{}, vsb);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.vsUbos, 1u);
    // refused: nothing bound, an offset off a vec4
    Encoded none(r, p, pass, wholeDraw(6));
    CHECK(!none.ok);
    expectRefusal(none.error, "tri_upos_vs_air: no buffer bound at index 0 (a constant buffer it indexes)");
    Bindings off;
    off.buffers = {Binding{pos.get(), 6}};
    Encoded misaligned(r, p, pass, wholeDraw(6), render::Options{}, off);
    CHECK(!misaligned.ok);
    expectRefusal(misaligned.error, "tri_upos_vs_air: buffer 0: offset 6 is not a multiple of 4 inside it");
    // ...and 8, off a vec4 but a multiple of 4, is drawn (probe 9o2)
    Bindings eight;
    eight.buffers = {Binding{pos.get(), 8}};
    Encoded at8(r, p, pass, wholeDraw(6), render::Options{}, eight);
    CHECK(at8.ok);
    CHECK(drawOnce(r, p, pass, wholeDraw(6), render::Options{}, eight)->status() == CommandBuffer::kCompleted);
    // a gap below the highest UBO, which no ldc reads (path_blit_vert_lph's 0 beside its 1 and 2): a descriptor of size 0
    // there, the bound one after it; and the highest itself unbound is still refused by name
    {
        Program gap = p;
        gap.vs.k.numUbos = 2;
        Bindings g;
        g.buffers = {Binding{nullptr, 0}, Binding{pos.get(), 0}};
        Encoded eg(r, gap, pass, wholeDraw(6), render::Options{}, g);
        CHECK(eg.ok);
        if (!eg.ok) std::printf("       %s\n", eg.error.c_str());
        bool seen = false;
        for (size_t i = eg.find(CP_LOAD_STATE6_GEOM); i != size_t(-1); i = eg.find(CP_LOAD_STATE6_GEOM, i + 1)) {
            const auto& w = eg.stream.words();
            if (((w[i + 1] & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) != ST6_UBO) continue;
            seen = true;
            CHECK_EQ((w[i + 1] & CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> CP_LOAD_STATE6_0_NUM_UNIT__SHIFT, 2u);
            CHECK_EQ(w[i + 4], 0u);
            CHECK_EQ(w[i + 5], 0u);
            CHECK_EQ(w[i + 6], uint32_t(pos->gpuAddress()));
        }
        CHECK(seen);
        Bindings top;
        top.buffers = {Binding{pos.get(), 0}};
        Encoded et(r, gap, pass, wholeDraw(6), render::Options{}, top);
        CHECK(!et.ok);
        expectRefusal(et.error, "tri_upos_vs_air: no buffer bound at index 1 (a constant buffer it indexes)");
    }
    // the fake's twin: a descriptor whose size runs past every buffer
    std::vector<uint32_t> words = e.stream.words();
    for (size_t i = e.find(CP_LOAD_STATE6_GEOM); i != size_t(-1); i = e.find(CP_LOAD_STATE6_GEOM, i + 1)) {
        if (((words[i + 1] & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) == ST6_UBO) words[i + 5] |= A6XX_UBO_1_SIZE__MASK;
    }
    CHECK(has(faultOf(r, words), "UBO descriptor 0 of the vertex stage: 32767 vec4 at"));
}

// --- a device buffer in a draw: the 3D pipeline's IBO table ------------------------------------------------------------------

TEST(a_draw_loads_the_3d_ibo_table_an_ldib_reads_through) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
    auto pos = bufferWith(r, xy, sizeof xy);
    Program p{kernels::tri_dpos_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_dpos"};
    CHECK_EQ(p.vs.k.numSsbos, 1u);
    Bindings vsb;
    vsb.buffers = {Binding{pos.get(), 0}};
    Encoded e(r, p, pass, wholeDraw(6), render::Options{}, vsb);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    CHECK_EQ(e.reg(REG_A6XX_SP_IBO_COUNT), 1u);
    CHECK_EQ((e.reg(REG_A6XX_SP_VS_CONFIG) & A6XX_SP_CS_CONFIG_NIBO__MASK) >> A6XX_SP_CS_CONFIG_NIBO__SHIFT, 1u);
    CHECK_EQ((e.reg(REG_A6XX_SP_FS_CONFIG) & A6XX_SP_CS_CONFIG_NIBO__MASK) >> A6XX_SP_CS_CONFIG_NIBO__SHIFT, 0u);
    const uint64_t iboAt = uint64_t(e.reg(REG_A6XX_SP_IBO)) | uint64_t(e.reg(REG_A6XX_SP_IBO + 1)) << 32;
    CHECK_EQ(iboAt, e.table->gpuAddress() + render::kTableVsAt);
    // the load: the plain CP_LOAD_STATE6, ST6_SHADER into SB6_IBO, from the table, one unit
    size_t load = e.find(CP_LOAD_STATE6);
    CHECK(load != size_t(-1));
    if (load != size_t(-1)) CHECK_EQ(e.stream.words()[load + 1], loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_IBO, 1));
    TexConst want = bufferIbo(pos->gpuAddress(), uint32_t(pos->length() / 4));
    CHECK(memcmp(e.tableWords() + render::kTableVsAt / 4, want.dword, sizeof want.dword) == 0);
    auto cb = drawOnce(r, p, pass, wholeDraw(6), render::Options{}, vsb);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.gfxIbos, 1u);
    // a draw without device buffers: the count 0 and no load
    Encoded flat(r, flatProgram(), pass, wholeDraw());
    CHECK_EQ(flat.reg(REG_A6XX_SP_IBO_COUNT), 0u);
    CHECK_EQ(flat.find(CP_LOAD_STATE6), size_t(-1));
    // refused: nothing bound; the two stages at one index with different buffers (one table for both)
    Encoded none(r, p, pass, wholeDraw(6));
    CHECK(!none.ok);
    expectRefusal(none.error, "tri_dpos_vs_air: no buffer bound at index 0 (a device buffer it addresses)");
    // a slot below the highest left unbound (mask_color_fragment's 1): turnip's null descriptor there, the bound one
    // after it; the highest unbound is still refused
    {
        Program gap = p;
        gap.vs.k.numSsbos = 2;
        gap.vs.k.numIbos = 2;
        Bindings g;
        g.buffers = {Binding{nullptr, 0}, Binding{pos.get(), 0}};
        Encoded eg(r, gap, pass, wholeDraw(6), render::Options{}, g);
        CHECK(eg.ok);
        if (!eg.ok) std::printf("       %s\n", eg.error.c_str());
        CHECK_EQ(eg.reg(REG_A6XX_SP_IBO_COUNT), 2u);
        const uint32_t zeros[16] = {};
        CHECK(memcmp(eg.tableWords() + render::kTableVsAt / 4, zeros, sizeof zeros) == 0);
        CHECK(memcmp(eg.tableWords() + render::kTableVsAt / 4 + 16, want.dword, sizeof want.dword) == 0);
        Bindings first;
        first.buffers = {Binding{pos.get(), 0}};
        Encoded et(r, gap, pass, wholeDraw(6), render::Options{}, first);
        CHECK(!et.ok);
        expectRefusal(et.error, "tri_dpos_vs_air: no buffer bound at index 1 (a device buffer it addresses)");
    }
    auto other = bufferWith(r, xy, sizeof xy);
    Program both = p;
    both.fs.k.numSsbos = 1;
    both.fs.k.numIbos = 1;
    Bindings fsb;
    fsb.buffers = {Binding{other.get(), 0}};
    Encoded clash(r, both, pass, wholeDraw(6), render::Options{}, vsb, fsb);
    CHECK(!clash.ok);
    expectRefusal(clash.error, "the vertex and fragment stages bind different device buffers at table slot 0: the 3D pipeline has one IBO table for both");
    // compiled as compileStage does for a program (Program::fsIboBase: the FS's buffers past the VS's), the same two
    // bindings share the table: slot 0 the VS's buffer, slot 1 the FS's [[buffer(0)]]
    {
        Program based = p;
        based.fs.k.numSsbos = 2;
        based.fs.k.numIbos = 2;
        based.fsIboBase = 1;
        Encoded e(r, based, pass, wholeDraw(6), render::Options{}, vsb, fsb);
        CHECK(e.ok);
        if (!e.ok) std::printf("       %s\n", e.error.c_str());
        CHECK_EQ(e.reg(REG_A6XX_SP_IBO_COUNT), 2u);
        const TexConst vsWant = bufferIbo(pos->gpuAddress(), uint32_t(pos->length() / 4));
        const TexConst fsWant = bufferIbo(other->gpuAddress(), uint32_t(other->length() / 4));
        CHECK(memcmp(e.tableWords() + render::kTableVsAt / 4, vsWant.dword, sizeof vsWant.dword) == 0);
        CHECK(memcmp(e.tableWords() + render::kTableVsAt / 4 + 16, fsWant.dword, sizeof fsWant.dword) == 0);
    }
    fsb.buffers = {Binding{pos.get(), 0}};   // the same binding in both: one table entry serves them
    Encoded shared(r, both, pass, wholeDraw(6), render::Options{}, vsb, fsb);
    CHECK(shared.ok);
    CHECK_EQ(shared.reg(REG_A6XX_SP_IBO_COUNT), 1u);
    // the fake's twins: a count past the load, and the descriptor zeroed
    std::vector<uint32_t> words = e.stream.words();
    words[e.findLastReg(REG_A6XX_SP_IBO_COUNT)] = 2;   // (the batch start's static table writes it 0 first)
    CHECK(has(faultOf(r, words), "the 3D IBO descriptors were not loaded into SB6_IBO from where SP_IBO points, or not as many as SP_IBO_COUNT says"));
    memset(static_cast<uint8_t*>(e.table->contents()) + render::kTableVsAt, 0, 64);
    CHECK(has(faultOf(r, e.stream.words()), "IBO 0 is all zeros"));
}

// --- draws into one pass: the command buffer joins a draw to the open pass into the same attachments -------------------------

TEST(draws_into_the_same_attachments_share_one_pass) {
    Rig r;
    auto t = target(r);
    auto other = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    auto run = [&](const std::function<void(CommandBuffer&)>& record) {
        const uint32_t before = r.fake->stats().passes;
        auto cb = r.queue->commandBuffer();
        record(*cb);
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
        return r.fake->stats().passes - before;
    };
    // three draws, one pass (and the tail at submit: the fake faults a submission whose colour stays in the CCU)
    CHECK_EQ(run([&](CommandBuffer& cb) {
                 for (int i = 0; i < 3; i++) CHECK(cb.draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{}));
             }),
             1u);
    CHECK_EQ(r.fake->stats().draws, 3u);
    // another target between them: a pass each time the attachments change
    RenderPass pass2;
    pass2.color0 = other.get();
    CHECK_EQ(run([&](CommandBuffer& cb) {
                 cb.draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{});
                 cb.draw(flatProgram(), pass2, wholeDraw(), Bindings{}, Bindings{});
                 cb.draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{});
             }),
             3u);
    // a Clear asked for by a later draw begins a pass of its own (its fill must not land over the first draw unseen)
    RenderPass cleared = pass;
    cleared.load = LoadAction::kClear;
    CHECK_EQ(run([&](CommandBuffer& cb) {
                 cb.draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{});
                 cb.draw(flatProgram(), cleared, wholeDraw(), Bindings{}, Bindings{});
             }),
             2u);
    // a blit between two draws closes the pass: the blit sees the first draw's colour in memory
    std::string why;
    auto b = newBuffer(*r.gpu, 64, &why);
    CHECK_EQ(run([&](CommandBuffer& cb) {
                 cb.draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{});
                 cb.fill(*b, 0, 64, 7);
                 cb.draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{});
             }),
             2u);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

TEST(draws_take_their_tables_and_programs_from_a_few_buffers_and_upload_changed_words_again) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    // forty draws recorded: a buffer from the kernel for each table and program was 120 of them (and one more for the
    // passes' counter stamps, which CommandQueue::preferGmem reads)
    auto cb = r.queue->commandBuffer();
    const uint32_t before = r.fake->liveBuffers();
    for (int i = 0; i < 40; i++) CHECK(cb->draw(flatProgram(), pass, wholeDraw(), Bindings{}, Bindings{}));
    const uint32_t asked = r.fake->liveBuffers() - before;
    CHECK(asked >= 1 && asked <= 3);
    if (asked > 3) std::printf("       %u buffers for 40 draws\n", asked);
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    // the same pointer with other words in it: the second draw's VS must be the new words, not the first upload. Both
    // word sets are the same length, so only the contents tell them apart.
    const Program base = flatProgram();
    std::vector<uint32_t> words(base.vs.k.words, base.vs.k.words + base.vs.k.numWords);
    Program p = base;
    p.vs.k.words = words.data();
    auto cb2 = r.queue->commandBuffer();
    CHECK(cb2->draw(p, pass, wholeDraw(), Bindings{}, Bindings{}));
    words.back() ^= 0x5a5a5a5au;   // (past the instructions the fake checks: the constants or the padding)
    CHECK(cb2->draw(p, pass, wholeDraw(), Bindings{}, Bindings{}));
    cb2->commit();
    cb2->waitUntilCompleted();
    CHECK(cb2->status() == CommandBuffer::kCompleted);
    const auto at = r.fake->memory(r.fake->stats().lastDraw.vsProgram, 4ull * words.size());
    CHECK_EQ(at.size(), 4ull * words.size());
    CHECK(at.size() == 4ull * words.size() && std::memcmp(at.data(), words.data(), at.size()) == 0);
    // and the unchanged FS was uploaded once: the same address in both draws is all one submission shows, so this
    // compares the last draw's FS words with the program's
    const auto fs = r.fake->memory(r.fake->stats().lastDraw.fsProgram, 4ull * base.fs.k.numWords);
    CHECK(fs.size() == 4ull * base.fs.k.numWords && std::memcmp(fs.data(), base.fs.k.words, fs.size()) == 0);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

TEST(a_draw_binds_the_queues_null_texture_at_the_reserved_slot_over_anything_bound_there) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    auto other = target(r, 4, 4, kTextureUsageRead);
    Sampler smp = nearestClamp();
    RenderPass pass;
    pass.color0 = t.get();
    const Program nullProg{kernels::tri_color_vs_air, kernels::tri_null_fs_air, Linkage{}, "tri_null"};
    CHECK_EQ(uint32_t(kernels::tri_null_fs_air.k.textures[0].isNull), 1u);
    std::string why;
    auto null = r.queue->nullTexture(&why);
    CHECK(null != nullptr);
    if (null == nullptr) return;
    CHECK(null == r.queue->nullTexture(&why));   // one per queue
    CHECK(null->width() == 1 && null->height() == 1 && null->format() == PixelFormat::kRGBA8Unorm);
    const auto texel = r.fake->memory(null->gpuAddress(), 4);
    CHECK(texel.size() == 4 && texel[0] == 0 && texel[1] == 0 && texel[2] == 0 && texel[3] == 0);
    const TexConst want = null->sampledDescriptor();
    auto slot0 = [&](const Bindings& fsb, std::string* error) -> std::vector<uint8_t> {
        auto cb = drawOnce(r, nullProg, pass, wholeDraw(6), render::Options{}, Bindings{}, fsb);
        *error = cb->status() == CommandBuffer::kCompleted ? "" : cb->error() + " | " + r.fake->stats().lastFault;
        if (!error->empty()) return {};
        CHECK_EQ(r.fake->stats().lastDraw.fsTextureCount, 1u);
        return r.fake->memory(r.fake->stats().lastDraw.fsTexConst, 64);
    };
    // nothing bound at 0: the queue's texture is
    Bindings samplerOnly;
    samplerOnly.samplers = {&smp};
    std::string error;
    auto d = slot0(samplerOnly, &error);
    CHECK(error.empty());
    if (!error.empty()) std::printf("       %s\n", error.c_str());
    CHECK(d.size() == 64 && memcmp(d.data(), want.dword, 64) == 0);
    // the application's texture at 0 is not the shader's null texture: the queue's goes over it
    Bindings bound = samplerOnly;
    bound.textures = {other.get()};
    d = slot0(bound, &error);
    CHECK(error.empty());
    CHECK(d.size() == 64 && memcmp(d.data(), want.dword, 64) == 0);
    const TexConst theirs = other->sampledDescriptor();
    CHECK(memcmp(want.dword, theirs.dword, 64) != 0);   // (else the line above could not tell the two apart)
    // without the mark the slot is an ordinary one with nothing bound, which gets the queue's texture too (an unbound
    // slot is read as the null texture); with something bound there it keeps the application's (bound, above, is the
    // marked slot's case)
    Program unmarked = nullProg;
    unmarked.fs.k.textures[0].isNull = 0;
    auto cb = r.queue->commandBuffer();
    CHECK(cb->draw(unmarked, pass, wholeDraw(6), Bindings{}, samplerOnly));
    if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
    // and the encoder alone binds nothing of its own: the substitution is the command buffer's
    Encoded e(r, nullProg, pass, wholeDraw(6), render::Options{}, Bindings{}, samplerOnly);
    CHECK(!e.ok);
    expectRefusal(e.error, "tri_null_fs_air: no texture bound at index 0");
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// Two colour targets: tri_mrt_fs writes [[color(0)]] and [[color(1)]], each into an attachment of its own -- RB_MRT[i]
// at its texture, SP_FS_OUTPUT_REG[i] its register, MRT 2 and both components' nibbles, each target's blend registers --
// and what the fake judges of each. The refusals by name: an output without an attachment, a second attachment of
// another size; a Clear on each is two fills; and a pass with the second attachment joins only the same second one.
TEST(a_draw_writes_two_colour_targets_each_into_its_own_attachment) {
    using namespace a6xx;
    Rig r;
    auto t0 = target(r), t1 = target(r), other = target(r), small = target(r, 32, 32);
    const Program mrt{kernels::tri_flat_vs_air, kernels::tri_mrt_fs_air, Linkage{}, "tri_mrt"};
    RenderPass pass;
    pass.color0 = t0.get();
    pass.more[0].texture = t1.get();
    Encoded e(r, mrt, pass, wholeDraw(6));
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    CHECK_EQ(e.reg(REG_A6XX_RB_FS_OUTPUT_CNTL1), A6XX_RB_FS_OUTPUT_CNTL1_MRT(2));
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_OUTPUT_CNTL1), A6XX_SP_FS_OUTPUT_CNTL1_MRT(2));
    CHECK_EQ(e.reg(regSpFsOutputReg(0)) & A6XX_SP_FS_OUTPUT_REG_REGID__MASK, 0x00u);   // r0.x
    CHECK_EQ(e.reg(regSpFsOutputReg(1)) & A6XX_SP_FS_OUTPUT_REG_REGID__MASK, 0x04u);   // r1.x
    CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_COMPONENTS), 0xffu);
    CHECK_EQ(uint64_t(e.reg(regRbMrt(REG_A6XX_RB_MRT_BASE, 1))) | uint64_t(e.reg(regRbMrt(REG_A6XX_RB_MRT_BASE, 1) + 1)) << 32, t1->gpuAddress());
    CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 1)), e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)));   // the full write mask in both
    auto cb = drawOnce(r, mrt, pass, wholeDraw(6));
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.mrtCount, 2u);
    CHECK_EQ(r.fake->stats().lastDraw.mrtBases[0], t0->gpuAddress());
    CHECK_EQ(r.fake->stats().lastDraw.mrtBases[1], t1->gpuAddress());
    // one attachment and a [[color(1)]] written: the output with no attachment is written nowhere -- no register for
    // slot 1, RB_MRT_CONTROL[1] 0, its components out of SP_FS_RENDER_COMPONENTS -- and attachment 0 is drawn as before
    RenderPass one;
    one.color0 = t0.get();
    Encoded noSecond(r, mrt, one, wholeDraw(6));
    CHECK(noSecond.ok);
    if (!noSecond.ok) std::printf("       %s\n", noSecond.error.c_str());
    CHECK_EQ(noSecond.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 1)), 0u);
    CHECK_EQ(noSecond.reg(REG_A6XX_SP_FS_RENDER_COMPONENTS) & 0xf0u, 0u);
    CHECK_EQ(noSecond.reg(REG_A6XX_SP_FS_RENDER_COMPONENTS) & 0xfu, 0xfu);
    {
        auto cb1 = drawOnce(r, mrt, one, wholeDraw(6));
        CHECK(cb1->status() == CommandBuffer::kCompleted);
        if (cb1->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb1->error().c_str(), r.fake->stats().lastFault.c_str());
    }
    RenderPass mixed = pass;
    mixed.more[0].texture = small.get();
    // attachments of different sizes: the window is the smaller (Metal's render target size), a viewport past it clipped
    // to it by the viewport scissor
    Encoded sizes(r, mrt, mixed, wholeDraw(6));
    CHECK(sizes.ok);
    CHECK_EQ(sizes.reg(a6xx::regGrasScViewportScissor(a6xx::REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL, 0) + 1), a6xx::scissorXY(31, 31));
    Encoded fits(r, mrt, mixed, wholeDraw(6, 32, 32));
    CHECK(fits.ok);
    if (!fits.ok) std::printf("       %s\n", fits.error.c_str());
    CHECK_EQ(fits.reg(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR), A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X(31) | A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y(31));
    // a Clear on each: two fills, each waited for on a word of its own (attachment 1's at kTableMoreClearFlushAt)
    RenderPass cleared = pass;
    cleared.load = LoadAction::kClear;
    cleared.more[0].load = LoadAction::kClear;
    const uint32_t fillsBefore = r.fake->stats().fills;
    Encoded c(r, mrt, cleared, wholeDraw(6));
    CHECK(c.ok);
    bool ownWord = false;
    for (size_t i = c.find(CP_WAIT_REG_MEM); i != size_t(-1); i = c.find(CP_WAIT_REG_MEM, i + 1)) {
        ownWord |= c.stream.words()[i + 2] == uint32_t(c.table->gpuAddress() + render::kTableMoreClearFlushAt);
    }
    CHECK(ownWord);
    render::Options sysmem;
    sysmem.gmem = false;
    auto cb2 = drawOnce(r, mrt, cleared, wholeDraw(6), sysmem);
    CHECK(cb2->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().fills - fillsBefore, 2u);
    // joined passes: the same second attachment joins, another one begins a pass
    auto passesOf = [&](const RenderPass& second) {
        const uint32_t before = r.fake->stats().passes;
        auto b = r.queue->commandBuffer();
        CHECK(b->draw(mrt, pass, wholeDraw(6), Bindings{}, Bindings{}));
        CHECK(b->draw(mrt, second, wholeDraw(6), Bindings{}, Bindings{}));
        b->commit();
        b->waitUntilCompleted();
        CHECK(b->status() == CommandBuffer::kCompleted);
        return r.fake->stats().passes - before;
    };
    CHECK_EQ(passesOf(pass), 1u);
    RenderPass elsewhere = pass;
    elsewhere.more[0].texture = other.get();
    CHECK_EQ(passesOf(elsewhere), 2u);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// A framebuffer fetch: tri_fb_fs reads [[color(0)]] through ir3's isamm at the slot after its textures (it has none:
// slot 0), so colour attachment 0's sampled descriptor goes there. With fbReadOrdered the rasterizer keeps the fetches in
// raster order (GRAS_SC_CNTL.SINGLE_PRIM_MODE FLUSH_PER_OVERLAP_AND_OVERWRITE in sysmem) and there is no barrier; by
// default, before the draw the colour CCU is flushed on the draw table's own word and the texture caches invalidated,
// so that it reads what the draws before it wrote. Two such draws in one pass: each its own flush, on its
// own table.
TEST(a_draw_that_reads_its_target_gets_the_target_as_a_texture_and_a_barrier_before_it) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    const Program fb{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
    CHECK_EQ(uint32_t(kernels::tri_fb_fs_air.fbRead), 1u);
    CHECK_EQ(kernels::tri_fb_fs_air.k.numSamp, 1u);
    RenderPass pass;
    pass.color0 = t.get();
    {
        // In sysmem a fetch is never left to the rasterizer's ordering (FLUSH_PER_OVERLAP_AND_OVERWRITE flushes at every
        // overlap): the barrier, NO_FLUSH. In a tile it is: FLUSH_PER_OVERLAP, and the texture caches dropped before the
        // first fetching draw of a run alone (a CACHE_INVALIDATE before the draw; none for the one after it).
        render::Options ord;
        ord.fbReadOrdered = true;
        Encoded o(r, fb, pass, wholeDraw(6), ord);
        CHECK(o.ok);
        CHECK_EQ(o.reg(REG_A6XX_GRAS_SC_CNTL) & A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__MASK, 0u);
        auto invalidates = [&](const pm4::Stream& st) {
            uint32_t n = 0;
            for (size_t i = 0; i + 1 < st.words().size();) {
                const pm4::Header h = pm4::decode(st.words()[i]);
                n += h.kind == pm4::Header::kCommand && h.opcode == CP_EVENT_WRITE && (st.words()[i + 1] & CP_EVENT_WRITE_0_EVENT__MASK) == CACHE_INVALIDATE;
                i += 1 + h.count;
            }
            return n;
        };
        render::GmemPlan plan;
        CHECK(render::gmemPlan(pass, &plan, nullptr));
        std::string owhy;
        auto ot = newBuffer(*r.gpu, render::kTableBytes, &owhy);
        pm4::Stream first, next;
        CHECK(ot != nullptr && render::encodeInPass(first, fb, pass, wholeDraw(6), Bindings{}, Bindings{}, o.vsProgram->gpuAddress(), o.fsProgram->gpuAddress(),
                                                    ot->gpuAddress(), static_cast<uint8_t*>(ot->contents()), &owhy, ord, &plan, false));
        CHECK(ot != nullptr && render::encodeInPass(next, fb, pass, wholeDraw(6), Bindings{}, Bindings{}, o.vsProgram->gpuAddress(), o.fsProgram->gpuAddress(),
                                                    ot->gpuAddress(), static_cast<uint8_t*>(ot->contents()), &owhy, ord, &plan, true));
        CHECK(invalidates(first) == 1 && invalidates(next) == 0);
        uint32_t sc = 0;
        for (size_t i = 0; i < next.words().size();) {
            const pm4::Header h = pm4::decode(next.words()[i]);
            if (h.kind == pm4::Header::kRegisters)
                for (uint32_t k = 0; k < h.count; k++) if (h.reg + k == REG_A6XX_GRAS_SC_CNTL) sc = next.words()[i + 1 + k];
            i += 1 + h.count;
        }
        CHECK_EQ((sc & A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__MASK) >> A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__SHIFT, uint32_t(FLUSH_PER_OVERLAP));
        Encoded plain(r, flatProgram(), pass, wholeDraw(6));
        CHECK_EQ(plain.reg(REG_A6XX_GRAS_SC_CNTL) & A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__MASK, 0u);
    }
    render::Options barrier;
    barrier.fbReadOrdered = false;
    Encoded e(r, fb, pass, wholeDraw(6), barrier);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    const TexConst want = t->sampledDescriptor();
    CHECK(memcmp(e.tableWords() + render::kTableFsAt / 4, want.dword, 64) == 0);
    CHECK_EQ(e.reg(REG_A6XX_SP_FS_TEX_COUNT), 1u);
    // the flush on the draw's word, waited for, and the invalidate, all before the draw packet
    size_t flush = size_t(-1);
    for (size_t i = e.find(CP_EVENT_WRITE); i != size_t(-1); i = e.find(CP_EVENT_WRITE, i + 1)) {
        if ((e.stream.words()[i + 1] & CP_EVENT_WRITE_0_EVENT__MASK) == PC_CCU_FLUSH_COLOR_TS &&
            e.stream.words()[i + 2] == uint32_t(e.table->gpuAddress() + render::kTableFbReadFlushAt)) {
            flush = i;
            break;
        }
    }
    const size_t draw = e.find(CP_DRAW_INDX_OFFSET);
    CHECK(flush != size_t(-1) && flush < draw);
    size_t inval = size_t(-1);
    for (size_t i = e.find(CP_EVENT_WRITE, flush == size_t(-1) ? 0 : flush); i != size_t(-1); i = e.find(CP_EVENT_WRITE, i + 1)) {
        if ((e.stream.words()[i + 1] & CP_EVENT_WRITE_0_EVENT__MASK) == CACHE_INVALIDATE) {
            inval = i;
            break;
        }
    }
    CHECK(inval != size_t(-1) && inval < draw);
    // ...and a draw that does not read its target has neither: the barrier is the fetch's
    Encoded plain(r, flatProgram(), pass, wholeDraw(6));
    bool plainFlush = false;
    for (size_t i = plain.find(CP_EVENT_WRITE); i != size_t(-1); i = plain.find(CP_EVENT_WRITE, i + 1)) {
        plainFlush |= plain.stream.words()[i + 2] == uint32_t(plain.table->gpuAddress() + render::kTableFbReadFlushAt);
    }
    CHECK(!plainFlush);
    // through the command buffer: two in one sysmem pass, the fake judging the descriptor at slot 0
    render::Options sysmem;
    sysmem.gmem = false;
    sysmem.fbReadOrdered = false;
    auto cb = r.queue->commandBuffer();
    CHECK(cb->draw(fb, pass, wholeDraw(6), Bindings{}, Bindings{}, sysmem));
    CHECK(cb->draw(fb, pass, wholeDraw(6), Bindings{}, Bindings{}, sysmem));
    const uint32_t before = r.fake->stats().passes;
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().passes - before, 1u);
    CHECK_EQ(r.fake->stats().lastDraw.fsTextureCount, 1u);
    const auto d = r.fake->memory(r.fake->stats().lastDraw.fsTexConst, 64);
    CHECK(d.size() == 64 && memcmp(d.data(), want.dword, 64) == 0);
    CHECK_EQ(r.fake->stats().faults, 0u);
    // In GMEM the fetch reads the attachment's bin where the draws before it in the tile left it: GMEM's layout
    // (TILE6_2), the bin's rows (64 pixels of 4 bytes), at the bin's place (0), no swap; and with no barrier, which would
    // flush a tile's colour to memory the fetch no longer reads
    auto g = r.queue->commandBuffer();
    CHECK(g->draw(fb, pass, wholeDraw(6), Bindings{}, Bindings{}));
    CHECK(g->draw(fb, pass, wholeDraw(6), Bindings{}, Bindings{}));
    g->commit();
    g->waitUntilCompleted();
    CHECK(g->status() == CommandBuffer::kCompleted);
    if (g->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", g->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(g->counts().gmemPasses, 1u);
    const auto gd = r.fake->memory(r.fake->stats().lastDraw.fsTexConst, 64);
    uint32_t gw[16] = {};
    if (gd.size() == 64) memcpy(gw, gd.data(), 64);
    CHECK_EQ((gw[0] & A6XX_TEX_CONST_0_TILE_MODE__MASK) >> A6XX_TEX_CONST_0_TILE_MODE__SHIFT, uint32_t(TILE6_2));
    CHECK_EQ(gw[0] & A6XX_TEX_CONST_0_SWAP__MASK, 0u);
    CHECK_EQ((gw[2] & A6XX_TEX_CONST_2_PITCH__MASK) >> A6XX_TEX_CONST_2_PITCH__SHIFT, 64u * 4);
    CHECK(gw[4] == 0 && (gw[5] & A6XX_TEX_CONST_5_BASE_HI__MASK) == 0);
    pm4::Stream gb;
    std::string gwhy;
    auto gt = newBuffer(*r.gpu, render::kTableBytes, &gwhy);
    render::GmemPlan plan;
    CHECK(render::gmemPlan(pass, &plan, nullptr));
    CHECK(gt != nullptr && render::encodeInPass(gb, fb, pass, wholeDraw(6), Bindings{}, Bindings{}, e.vsProgram->gpuAddress(), e.fsProgram->gpuAddress(),
                                                gt->gpuAddress(), static_cast<uint8_t*>(gt->contents()), &gwhy, render::Options{}, &plan));
    bool gmemFlush = false;
    for (size_t i = 0; gt != nullptr && i + 2 < gb.words().size(); i++) gmemFlush |= gb.words()[i + 2] == uint32_t(gt->gpuAddress() + render::kTableFbReadFlushAt);
    CHECK(!gmemFlush);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// Mip levels drawn into and made: tri_flat into level 2 of a 64 x 64 texture of seven (RB_MRT[0] at the level's address,
// the window 16 x 16); a copy of a 16 x 8 rectangle into another texture (the fake carries copies out texel for
// texel); generateMipmaps over a level 0 of 0x20 and 0x60 in a checkerboard: every 2 x 2 block averages to 0x40, so
// every level below is 0x40 in every byte, through six filtered blits.
TEST(mip_levels_are_drawn_into_copied_and_generated) {
    using namespace a6xx;
    Rig r;
    TextureDesc d;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = d.height = 64;
    d.mipLevels = 7;
    d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    std::string why;
    auto t = Texture::make(*r.gpu, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    RenderPass pass;
    pass.color0 = t.get();
    pass.level = 2;
    render::Options sysmem;
    sysmem.gmem = false;   // (the window checked is a sysmem pass's: a tile's is its bin)
    auto cb = drawOnce(r, flatProgram(), pass, wholeDraw(6, 16, 16), sysmem);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.mrtBases[0], t->levelAddress(2));
    CHECK_EQ(r.fake->stats().lastDraw.windowScissor[2], 15u);
    RenderPass past = pass;
    past.level = 7;
    Encoded e(r, flatProgram(), past, wholeDraw(6, 16, 16));
    CHECK(!e.ok);
    expectRefusal(e.error, "colour attachment 0 names mip level 7 of a texture of 7");
    // a copy: 16 x 8 of level 0 at (8, 4) into another 64 x 64 texture at (0, 0)
    std::vector<uint32_t> rows(64 * 64);
    for (uint32_t i = 0; i < rows.size(); i++) rows[i] = 0x01000000u * (i & 0xff) + i;
    CHECK(t->replace(0, 0, 0, 64, 64, rows.data(), 256, &why));
    TextureDesc od = d;
    od.mipLevels = 1;
    auto other = Texture::make(*r.gpu, od, &why);
    auto cb2 = r.queue->commandBuffer();
    CHECK(cb2->blit(*t, 0, render::BlitRect{8, 4, 16, 8}, *other, 0, render::BlitRect{0, 0, 16, 8}, false));
    cb2->commit();
    cb2->waitUntilCompleted();
    CHECK(cb2->status() == CommandBuffer::kCompleted);
    std::vector<uint32_t> back(16 * 8);
    CHECK(other->read(0, 0, 16, 8, back.data(), 64, &why));
    bool same = true;
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 16; x++) same = same && back[y * 16 + x] == rows[(y + 4) * 64 + x + 8];
    CHECK(same);
    // generateMipmaps over a checkerboard
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) rows[y * 64 + x] = (x + y) % 2 ? 0x60606060u : 0x20202020u;
    CHECK(t->replace(0, 0, 0, 64, 64, rows.data(), 256, &why));
    const uint32_t copiesBefore = r.fake->stats().copies;
    auto cb3 = r.queue->commandBuffer();
    CHECK(cb3->generateMipmaps(*t));
    cb3->commit();
    cb3->waitUntilCompleted();
    CHECK(cb3->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().copies - copiesBefore, 6u);
    bool all40 = true;
    for (uint32_t l = 1; l < 7; l++) {
        const TextureLevel L = t->level(l);
        std::vector<uint32_t> lv(L.width * L.height);
        CHECK(t->read(l, 0, 0, L.width, L.height, lv.data(), L.width * 4, &why));
        for (uint32_t v : lv) all40 = all40 && v == 0x40404040u;
    }
    CHECK(all40);
    // refusals: another format, an integer format filtered
    TextureDesc fd = od;
    fd.format = PixelFormat::kRGBA16Float;
    auto f16 = Texture::make(*r.gpu, fd, &why);
    auto cb4 = r.queue->commandBuffer();
    CHECK(!cb4->blit(*t, 0, render::BlitRect{0, 0, 8, 8}, *f16, 0, render::BlitRect{0, 0, 8, 8}, false));
    expectRefusal(cb4->error(), "a copy from RGBA8Unorm to RGBA16Float: the two must be one pixel format (an sRGB pair is one)");
    // a level onto itself (VectorKit does both): in place it is nothing, so no copy reaches the engine; shifted by
    // less than its size it reads as a memmove -- through a texture of its own, two copies
    for (uint32_t i = 0; i < rows.size(); i++) rows[i] = 0x01000000u * (i & 0xff) + i;
    CHECK(t->replace(0, 0, 0, 64, 64, rows.data(), 256, &why));
    const uint32_t copiesBeforeSelf = r.fake->stats().copies;
    auto cb5 = r.queue->commandBuffer();
    CHECK(cb5->blit(*t, 0, render::BlitRect{0, 0, 8, 8}, *t, 0, render::BlitRect{0, 0, 8, 8}, false));
    CHECK(cb5->blit(*t, 0, render::BlitRect{0, 0, 8, 8}, *t, 0, render::BlitRect{4, 4, 8, 8}, false));
    cb5->commit();
    cb5->waitUntilCompleted();
    CHECK(cb5->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().copies - copiesBeforeSelf, 2u);
    std::vector<uint32_t> moved(12 * 12);
    CHECK(t->read(0, 0, 0, 12, 12, moved.data(), 48, &why));
    bool memmove = true;
    for (uint32_t y = 0; y < 12; y++)
        for (uint32_t x = 0; x < 12; x++) {
            // inside the destination: the source as it was before the copy; elsewhere: untouched
            const bool inside = x >= 4 && y >= 4;
            const uint32_t want = inside ? rows[(y - 4) * 64 + x - 4] : rows[y * 64 + x];
            memmove = memmove && moved[y * 12 + x] == want;
        }
    CHECK(memmove);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// [[color(1)]] written alone: slot 0 an RB_MRT entry nothing writes (no register, no components), slot 1 the attachment
TEST(a_draw_that_writes_only_color_1_leaves_attachment_0_unwritten) {
    using namespace a6xx;
    Rig r;
    auto t0 = target(r), t1 = target(r);
    const Program p{kernels::tri_flat_vs_air, kernels::tri_mrt1_fs_air, Linkage{}, "tri_mrt1"};
    RenderPass pass;
    pass.color0 = t0.get();
    pass.more[0].texture = t1.get();
    Encoded e(r, p, pass, wholeDraw(6));
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    CHECK_EQ(e.reg(REG_A6XX_RB_FS_OUTPUT_CNTL1), A6XX_RB_FS_OUTPUT_CNTL1_MRT(2));
    CHECK_EQ(e.reg(regSpFsOutputReg(0)) & A6XX_SP_FS_OUTPUT_REG_REGID__MASK, uint32_t(kRegidNone));
    CHECK_EQ(e.reg(REG_A6XX_RB_RENDER_COMPONENTS), 0xf0u);
    CHECK_EQ(e.reg(regRbMrt(REG_A6XX_RB_MRT_CONTROL, 0)), 0u);
    auto cb = drawOnce(r, p, pass, wholeDraw(6));
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.mrtBases[1], t1->gpuAddress());
    // no attachment 1: the one output it writes is written nowhere, and the pass draws nothing into attachment 0
    RenderPass one;
    one.color0 = t0.get();
    Encoded none(r, p, one, wholeDraw(6));
    CHECK(none.ok);
    CHECK_EQ(none.reg(REG_A6XX_SP_FS_RENDER_COMPONENTS), 0u);
}

// A stencil attachment: the S8 plane of a Depth32Float_Stencil8 texture after its depth plane, in RB_STENCIL_INFO's
// six registers (SEPARATE_STENCIL, the plane's pitch and layer, its base); a stencil Clear is a TILE6_3 fill of the
// plane (FMT6_8_UINT through R2D_INT8); a draw's stencil state in RB_STENCIL_CONTROL and the three mask registers.
TEST(a_stencil_attachment_is_the_s8_plane_and_its_test_the_draws_state) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    TextureDesc dd;
    dd.format = PixelFormat::kDepth32FloatStencil8;
    dd.width = dd.height = 64;
    dd.usage = kTextureUsageRenderTarget;
    std::string why;
    auto ds = Texture::make(*r.gpu, dd, &why);
    CHECK(ds != nullptr);
    if (ds == nullptr) return;
    CHECK_EQ(ds->depthPlaneBytes(), 16384u);   // 64 rows (to 16) of 256 bytes, to 4096
    CHECK_EQ(ds->stencilOffset(), 16384u);
    CHECK_EQ(ds->stencilPitch(), 128u);        // 64 bytes to 128
    CHECK_EQ(ds->stencilPlaneBytes(), 8192u);  // 64 rows (to 32) of 128, to 4096
    RenderPass pass;
    pass.color0 = t.get();
    pass.depth = ds.get();
    pass.stencil = ds.get();
    pass.stencilLoad = LoadAction::kClear;
    pass.clearStencil = 0x5a;
    Draw d = wholeDraw(6);
    d.stencil.enabled = true;
    d.stencil.front = StencilFace{3, 0, 0, 2, 0xff, 0x0f};   // LessEqual, Keep / Keep / Replace, write mask 0x0f
    d.stencil.back = d.stencil.front;
    d.stencil.frontReference = 7;
    d.stencil.backReference = 7;
    Encoded e(r, flatProgram(), pass, d);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    const uint64_t plane = ds->gpuAddress() + 16384;
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCIL_INFO), A6XX_RB_STENCIL_INFO_SEPARATE_STENCIL);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCIL_INFO + 1), 128u >> 6);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCIL_INFO + 2), 8192u >> 6);
    CHECK_EQ(uint64_t(e.reg(REG_A6XX_RB_STENCIL_INFO + 3)) | uint64_t(e.reg(REG_A6XX_RB_STENCIL_INFO + 4)) << 32, plane);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCILREF), 0x0707u);
    CHECK_EQ(e.reg(REG_A6XX_RB_STENCILREF + 2), 0x0f0fu);   // RB_STENCILWRMASK
    const uint32_t control = e.reg(REG_A6XX_RB_STENCIL_CONTROL);
    CHECK((control & A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE) != 0);
    CHECK_EQ((control & A6XX_RB_STENCIL_CONTROL_FUNC__MASK) >> A6XX_RB_STENCIL_CONTROL_FUNC__SHIFT, 3u);
    CHECK_EQ((control & A6XX_RB_STENCIL_CONTROL_ZPASS__MASK) >> A6XX_RB_STENCIL_CONTROL_ZPASS__SHIFT, 2u);
    // the Clear: a tiled fill into the plane (the fake judges it and counts it; the tiled order is not modelled)
    const uint32_t tiledBefore = r.fake->stats().tiledFills;
    auto cb = drawOnce(r, flatProgram(), pass, d);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().tiledFills - tiledBefore, 1u);
    // refusals: a stencil test with no attachment, a stencil attachment of another texture
    RenderPass noStencil = pass;
    noStencil.stencil = nullptr;
    Encoded e2(r, flatProgram(), noStencil, d);
    CHECK(!e2.ok);
    expectRefusal(e2.error, "a stencil test with no stencil attachment");
    TextureDesc d32 = dd;
    d32.format = PixelFormat::kDepth32Float;
    auto plain = Texture::make(*r.gpu, d32, &why);
    RenderPass other = pass;
    other.depth = plain.get();
    Encoded e3(r, flatProgram(), other, d);
    CHECK(!e3.ok);
    expectRefusal(e3.error, "a stencil attachment that is not the depth attachment's own Depth32Float_Stencil8 texture: the S8 plane of that one texture is what is served");
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// A program longer than the instruction cache, through the fake: the preload it checks (at most 127 units, never more
// than INSTRLEN) is what a 128-unit FS gets, and the draw completes. The twin preloads all 128 and must fault.
TEST(a_program_longer_than_the_instruction_cache_is_preloaded_in_part) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Program p = handProgram();
    static const std::vector<uint32_t> longWords(128 * 32, 0);
    p.fs.k.words = longWords.data();
    p.fs.k.numWords = uint32_t(longWords.size());
    p.fs.k.instrlen = 128;
    auto cb = drawOnce(r, p, pass, wholeDraw(3));
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    // the twin: the FS program's preload raised to the whole 128 units, which the fake refuses as this GPU hangs on it
    Encoded e(r, p, pass, wholeDraw(3));
    CHECK(e.ok);
    std::vector<uint32_t> w = e.stream.words();
    bool found = false;
    for (size_t i = e.find(a6xx::CP_LOAD_STATE6_FRAG); i != size_t(-1); i = e.find(a6xx::CP_LOAD_STATE6_FRAG, i + 1)) {
        const uint32_t d0 = w[i + 1];
        const uint32_t type = (d0 & a6xx::CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> a6xx::CP_LOAD_STATE6_0_STATE_TYPE__SHIFT;
        const uint32_t block = (d0 & a6xx::CP_LOAD_STATE6_0_STATE_BLOCK__MASK) >> a6xx::CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT;
        if (type == a6xx::ST6_SHADER && block == a6xx::SB6_FS_SHADER) {
            CHECK_EQ((d0 & a6xx::CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> a6xx::CP_LOAD_STATE6_0_NUM_UNIT__SHIFT, 127u);
            w[i + 1] = (d0 & ~a6xx::CP_LOAD_STATE6_0_NUM_UNIT__MASK) | a6xx::CP_LOAD_STATE6_0_NUM_UNIT(128);
            found = true;
        }
    }
    CHECK(found);
    CHECK(has(faultOf(r, w), "a program preload of 128 units: 128 hangs this GPU"));
}

// Occlusion queries (-setVisibilityResultMode:offset:): around the draw the running sample count is copied out twice
// (RB_SAMPLE_COUNT_ADDR, ZPASS_DONE) and the result word gets end - begin added (CP_MEM_TO_MEM, DOUBLE | NEG_C), after
// a zeroing CP_MEM_WRITE on the encoder's first draw into it. The fake counts one sample per index: 6 from a seeded word
// reset first is 6, 3 more without the reset 9, a reset again 3. A mode past Counting, a mode with no buffer and an
// offset off 8 or past the end are refused by name.
TEST(an_occlusion_query_adds_the_draws_samples_to_its_word) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    std::string why;
    auto vis = newBuffer(*r.gpu, 64, &why);
    CHECK(vis != nullptr);
    if (vis == nullptr) return;
    auto* words = static_cast<uint64_t*>(vis->contents());
    for (int i = 0; i < 8; i++) words[i] = 0xdeadbeefcafef00dull;
    auto counted = [&](uint32_t vertices, bool reset) {
        Draw d = wholeDraw(vertices);
        d.visibility = vis.get();
        d.visibilityMode = 2;
        d.visibilityOffset = 16;
        d.visibilityReset = reset;
        auto cb = drawOnce(r, flatProgram(), pass, d);
        CHECK(cb->status() == CommandBuffer::kCompleted);
        if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
        return words[2];
    };
    CHECK_EQ(counted(6, true), uint64_t(6));
    CHECK_EQ(counted(3, false), uint64_t(9));
    CHECK_EQ(counted(3, true), uint64_t(3));
    CHECK_EQ(words[1], 0xdeadbeefcafef00dull);   // the words beside it are not written
    CHECK_EQ(words[3], 0xdeadbeefcafef00dull);
    // a draw with the mode off writes nothing
    {
        Draw d = wholeDraw(3);
        d.visibility = vis.get();
        auto cb = drawOnce(r, flatProgram(), pass, d);
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(words[2], uint64_t(3));
    }
    auto refusedWith = [&](Draw d, const char* want) {
        auto cb = r.queue->commandBuffer();
        CHECK(!cb->draw(flatProgram(), pass, d, Bindings{}, Bindings{}));
        expectRefusal(cb->error(), want);
    };
    Draw d = wholeDraw(3);
    d.visibilityMode = 3;
    d.visibility = vis.get();
    refusedWith(d, "visibility result mode 3: MTLVisibilityResultMode is Disabled (0), Boolean (1) or Counting (2)");
    d.visibilityMode = 1;
    d.visibility = nullptr;
    refusedWith(d, "a visibility result mode and no visibility result buffer in the pass");
    d.visibility = vis.get();
    d.visibilityOffset = 4;
    refusedWith(d, "a visibility result at offset 4: a multiple of 8 with its 8 bytes inside the 64-byte buffer");
    d.visibilityOffset = 64;
    refusedWith(d, "a visibility result at offset 64: a multiple of 8 with its 8 bytes inside the 64-byte buffer");
}

// --- multisampling: a 4x target drawn into under the pass's count, cleared as its memory, resolved by the 2D engine -------

namespace {
std::shared_ptr<Texture> msTarget(Rig& r, uint32_t samples = 4) {
    TextureDesc d;
    d.type = TextureType::k2DMultisample;
    d.samples = samples;
    d.width = d.height = 64;
    d.usage = kTextureUsageRenderTarget;
    std::string why;
    auto t = Texture::make(*r.gpu, d, &why);
    CHECK(t != nullptr);
    return t;
}
}  // namespace

TEST(a_multisample_pass_draws_under_its_count) {
    using namespace a6xx;
    Rig r;
    auto ms = msTarget(r);
    RenderPass pass;
    pass.color0 = ms.get();
    // a pipeline of one sample into it draws at the attachments' count, as an Apple GPU does without API validation
    // (SiriUICore's orb): the same MSAA state as a pipeline of four
    Encoded oneSample(r, flatProgram(), pass, wholeDraw());
    CHECK(oneSample.ok);
    Program p = flatProgram();
    p.samples = 4;
    Encoded e(r, p, pass, wholeDraw());
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    // MSAA_FOUR (2) in all three blocks, MSAA_DISABLE nowhere; the target's rows hold four samples a pixel
    CHECK_EQ(e.reg(REG_A6XX_GRAS_RAS_MSAA_CNTL), 2u);
    CHECK_EQ(e.reg(REG_A6XX_GRAS_DEST_MSAA_CNTL), 2u);
    CHECK_EQ(e.reg(REG_A6XX_RB_DEST_MSAA_CNTL), 2u);
    CHECK_EQ(e.reg(REG_A6XX_SP_TP_DEST_MSAA_CNTL), 2u);
    for (uint32_t reg : {REG_A6XX_GRAS_RAS_MSAA_CNTL, REG_A6XX_GRAS_DEST_MSAA_CNTL, REG_A6XX_RB_DEST_MSAA_CNTL, REG_A6XX_SP_TP_DEST_MSAA_CNTL}) {
        CHECK_EQ(oneSample.reg(reg), e.reg(reg));   // the one-sample pipeline under the attachments' four
    }
    CHECK_EQ(e.reg(REG_A6XX_RB_MRT_PITCH) << 6, uint32_t(ms->bytesPerRow()));
    CHECK_EQ(ms->bytesPerRow(), 64u * 4 * 4);
    auto cb = drawOnce(r, p, pass, wholeDraw());
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.samples, 4u);
    // a single-sample pass beside it is still one sample
    auto one = target(r);
    RenderPass flat;
    flat.color0 = one.get();
    Encoded f(r, flatProgram(), flat, wholeDraw());
    CHECK_EQ(f.reg(REG_A6XX_GRAS_DEST_MSAA_CNTL), uint32_t(A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE));
    // attachments of two counts in one pass are refused
    RenderPass mixed;
    mixed.color0 = ms.get();
    mixed.more[0].texture = one.get();
    Encoded m(r, p, mixed, wholeDraw());
    CHECK(!m.ok);
    expectRefusal(m.error, "colour attachment 1 has 1 sample(s) beside another attachment's 4: a pass's attachments share one sample count");
}

TEST(a_multisample_texture_is_cleared_whole_and_resolved_to_the_mean) {
    Rig r;
    auto ms = msTarget(r);
    auto out = target(r);
    // a Clear fills every sample: resolved, every pixel is the clear colour (0.25, 0.5, 0.75, 1 -> 40 80 bf ff)
    RenderPass pass;
    pass.color0 = ms.get();
    pass.load = LoadAction::kClear;
    const double c[4] = {0.25, 0.5, 0.75, 1.0};
    for (int i = 0; i < 4; i++) pass.clearColor[i] = c[i];
    auto cb = r.queue->commandBuffer();
    CHECK(cb->clear(pass));
    CHECK(cb->blit(*ms, 0, render::BlitRect{0, 0, 64, 64}, *out, 0, render::BlitRect{0, 0, 64, 64}, false));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    auto* px = static_cast<const uint8_t*>(out->buffer()->contents());
    uint32_t cleared = 0;
    for (uint32_t j = 0; j < 64; j++)
        for (uint32_t i = 0; i < 64; i++) cleared += memcmp(px + j * out->bytesPerRow() + i * 4, "\x40\x80\xbf\xff", 4) == 0;
    CHECK_EQ(cleared, 4096u);
    // four different samples in pixel (3, 5): its resolve is their mean, the pixels beside it the clear colour
    auto* raw = static_cast<uint8_t*>(ms->buffer()->contents()) + 5 * ms->bytesPerRow() + 3 * 16;
    const uint8_t samples[4][4] = {{0, 0, 0, 0}, {100, 0, 0, 255}, {200, 4, 0, 255}, {101, 0, 3, 255}};
    memcpy(raw, samples, 16);
    cb = r.queue->commandBuffer();
    CHECK(cb->blit(*ms, 0, render::BlitRect{0, 0, 64, 64}, *out, 0, render::BlitRect{0, 0, 64, 64}, false));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const uint8_t* got = px + 5 * out->bytesPerRow() + 3 * 4;
    const uint8_t want[4] = {100, 1, 1, 191};   // (0+100+200+101)/4 = 100.25, (0+0+4+0)/4 = 1, (0+0+0+3)/4 = 0.75, (0+255*3)/4 = 191.25
    CHECK(memcmp(got, want, 4) == 0);
    if (memcmp(got, want, 4) != 0) std::printf("       resolved to %02x %02x %02x %02x\n", got[0], got[1], got[2], got[3]);
    CHECK(memcmp(got - 4, "\x40\x80\xbf\xff", 4) == 0);
    // a copy into a multisample texture, and a scaled resolve, are refused
    cb = r.queue->commandBuffer();
    CHECK(!cb->blit(*out, 0, render::BlitRect{0, 0, 64, 64}, *ms, 0, render::BlitRect{0, 0, 64, 64}, false));
    expectRefusal(cb->error(), "a copy into a multisample texture: its samples are drawn, cleared or resolved from, not copied into");
    cb = r.queue->commandBuffer();
    CHECK(!cb->blit(*ms, 0, render::BlitRect{0, 0, 64, 64}, *out, 0, render::BlitRect{0, 0, 32, 32}, true));
    expectRefusal(cb->error(), "a resolve that scales or filters: a multisample source is resolved pixel for pixel");
}

// A depth texture sampled (a shadow map's lookup): tri_shadow_fs compares each pixel's texel of a Depth32Float texture
// with REF through a Less sampler. The FS's table holds the tiled view (TILE6_3, TILE_ALL), which the fake reads as the
// depth layout; the same draw with TILE_ALL taken off that descriptor is refused by the fake (the twin: its rule bites).
TEST(a_draw_samples_a_depth_texture_through_a_compare_sampler) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    auto depth = depthTarget(r, 64, 64, kTextureUsageRenderTarget | kTextureUsageRead);
    std::string why;
    auto refBuf = newBuffer(*r.gpu, 16, &why);
    CHECK(depth != nullptr && refBuf != nullptr);
    if (depth == nullptr || refBuf == nullptr) return;
    *static_cast<float*>(refBuf->contents()) = 0.5f;
    SamplerDesc sdesc;
    sdesc.compareFunction = 1;   // Less
    Sampler smp;
    CHECK(makeSampler(sdesc, &smp, &why));
    const Program p{kernels::tri_flat_vs_air, kernels::tri_shadow_fs_air, Linkage{}, "tri_shadow"};
    RenderPass pass;
    pass.color0 = t.get();
    Bindings fsb;
    fsb.buffers = {Binding{refBuf.get(), 0}};
    fsb.textures = {depth.get()};
    fsb.samplers = {&smp};
    Encoded e(r, p, pass, wholeDraw(6), render::Options{}, Bindings{}, fsb);
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    const uint32_t* texWords = e.tableWords() + render::kTableFsAt / 4;
    CHECK_EQ((texWords[0] & A6XX_TEX_CONST_0_TILE_MODE__MASK) >> A6XX_TEX_CONST_0_TILE_MODE__SHIFT, uint32_t(TILE6_3));
    CHECK((texWords[3] & A6XX_TEX_CONST_3_TILE_ALL) != 0);
    CHECK_EQ((e.tableWords()[(render::kTableFsAt + 16 * 64) / 4 + 1] & A6XX_TEX_SAMP_1_COMPARE_FUNC__MASK) >> A6XX_TEX_SAMP_1_COMPARE_FUNC__SHIFT, 1u);
    auto cb = drawOnce(r, p, pass, wholeDraw(6), render::Options{}, Bindings{}, fsb);
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().faults, 0u);
    // the twin: the encoded words run raw, as they are (the control) and then with TILE_ALL taken off the descriptor in
    // their table
    Raw raw(r);
    const uint32_t faultsBefore = r.fake->stats().faults;
    CHECK_EQ(raw.submit(e.stream.words()), 0);
    CHECK_EQ(r.fake->stats().faults, faultsBefore);
    e.tableWords()[render::kTableFsAt / 4 + 3] &= ~A6XX_TEX_CONST_3_TILE_ALL;
    CHECK_EQ(raw.submit(e.stream.words()), ETIMEDOUT);
    CHECK(has(r.fake->stats().lastFault, "tile mode 3 without TILE_ALL"));
}

// Culling: MTLCullMode as CULL_FRONT / CULL_BACK, and FRONT_CW for Metal's Clockwise front -- the phone's answer (probe
// 9cu), the opposite of what the negative y scale suggested; no cull with CounterClockwise is all three bits clear
TEST(a_draw_culls_as_its_cull_mode_and_winding_say) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    auto su = [&](uint32_t cull, uint32_t winding) {
        Draw d = wholeDraw();
        d.cullMode = cull;
        d.winding = winding;
        Encoded e(r, flatProgram(), pass, d);
        CHECK(e.ok);
        return e.reg(REG_A6XX_GRAS_SU_CNTL) & (A6XX_GRAS_SU_CNTL_CULL_FRONT | A6XX_GRAS_SU_CNTL_CULL_BACK | A6XX_GRAS_SU_CNTL_FRONT_CW);
    };
    CHECK_EQ(su(0, 1), 0u);
    CHECK_EQ(su(0, 0), A6XX_GRAS_SU_CNTL_FRONT_CW);
    CHECK_EQ(su(2, 0), A6XX_GRAS_SU_CNTL_CULL_BACK | A6XX_GRAS_SU_CNTL_FRONT_CW);
    CHECK_EQ(su(1, 1), A6XX_GRAS_SU_CNTL_CULL_FRONT);
    CHECK_EQ(su(2, 1), A6XX_GRAS_SU_CNTL_CULL_BACK);
    Draw bad = wholeDraw();
    bad.cullMode = 3;
    Encoded e(r, flatProgram(), pass, bad);
    CHECK(!e.ok);
    expectRefusal(e.error, "cull mode 3, winding 0: MTLCullMode is 0 to 2, MTLWinding 0 or 1");
}

// A slice as a render target (sokol's cubemaprt draws each face of a cube): RB_MRT_BASE the face's memory, slices
// sliceBytes apart; face 0 is the control (the texture's own address), and a face past the six is refused.
TEST(a_draw_into_a_cube_face_aims_at_that_face) {
    using namespace a6xx;
    Rig r;
    TextureDesc d;
    d.type = TextureType::kCube;
    d.width = d.height = 64;
    d.depth = 6;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    std::string why;
    auto cube = Texture::make(*r.gpu, d, &why);
    CHECK(cube != nullptr);
    if (cube == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    auto base = [](const Encoded& e) { return uint64_t(e.reg(REG_A6XX_RB_MRT_BASE)) | uint64_t(e.reg(REG_A6XX_RB_MRT_BASE + 1)) << 32; };
    RenderPass pass;
    pass.color0 = cube.get();
    Encoded e0(r, flatProgram(), pass, wholeDraw());
    CHECK(e0.ok);
    CHECK_EQ(base(e0), cube->gpuAddress());
    pass.slice = 3;
    Encoded e3(r, flatProgram(), pass, wholeDraw());
    CHECK(e3.ok);
    CHECK_EQ(base(e3), cube->gpuAddress() + 3 * cube->sliceBytes());
    auto cb = drawOnce(r, flatProgram(), pass, wholeDraw());
    CHECK(cb->status() == CommandBuffer::kCompleted);
    pass.slice = 6;
    Encoded e6(r, flatProgram(), pass, wholeDraw());
    CHECK(!e6.ok);
    expectRefusal(e6.error, "slice 6 of a texture of 6");
}

// A depth texture of mip levels (sokol's miprender draws every level with depth): each level its own tiled layout, the
// RB told the level's base and pitch. Level 0 is the control: the texture's own address and pitch.
TEST(a_draw_into_a_depth_mip_level_aims_at_that_level) {
    using namespace a6xx;
    Rig r;
    TextureDesc dd;
    dd.format = PixelFormat::kDepth32Float;
    dd.width = dd.height = 256;
    dd.mipLevels = 9;
    dd.usage = kTextureUsageRenderTarget;
    TextureDesc cd;
    cd.width = cd.height = 256;
    cd.mipLevels = 9;
    cd.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    std::string why;
    auto depth = Texture::make(*r.gpu, dd, &why);
    auto colour = depth ? Texture::make(*r.gpu, cd, &why) : nullptr;
    CHECK(colour != nullptr);
    if (colour == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    const TextureLevel L2 = depth->level(2);
    CHECK_EQ(L2.width, 64u);
    CHECK_EQ(L2.pitch, 256u);
    CHECK_EQ(L2.offset % 4096, 0u);
    CHECK_EQ(depth->level(1).offset, 256u * 1024);   // level 0: 256 rows of 1024 bytes
    auto base = [](const Encoded& e) { return uint64_t(e.reg(REG_A6XX_RB_DEPTH_BUFFER_BASE)) | uint64_t(e.reg(REG_A6XX_RB_DEPTH_BUFFER_BASE + 1)) << 32; };
    for (uint32_t level : {0u, 2u}) {
        RenderPass pass;
        pass.color0 = colour.get();
        pass.level = level;
        pass.depth = depth.get();
        pass.depthLevel = level;
        pass.depthLoad = LoadAction::kClear;
        pass.clearDepth = 1.0;
        Draw d = wholeDraw(3, 256 >> level, 256 >> level);
        d.depth = DepthState{1, true};
        Encoded e(r, flatProgram(), pass, d);
        CHECK(e.ok);
        if (!e.ok) std::printf("       %s\n", e.error.c_str());
        CHECK_EQ(base(e), depth->levelAddress(level));
        CHECK_EQ(e.reg(REG_A6XX_RB_DEPTH_BUFFER_PITCH) << 6, uint32_t(depth->level(level).pitch));
        auto cb = drawOnce(r, flatProgram(), pass, d);
        CHECK(cb->status() == CommandBuffer::kCompleted);
        if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    }
}

// A vertex stage whose corners are a module constant table (tri_arr_vs, sokol's bufferless triangle): ir3 pushes the
// table from its constant-data UBO, and the draw loads those words direct from the program's own. The pushed words
// must be the table: (-1, -1, 0, 1) first, (1, -1, 0, 1) after it, which an unwritten range (zeros) is not.
TEST(a_vertex_stage_reads_its_constant_table) {
    using namespace a6xx;
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    const Program p{kernels::tri_arr_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_arr"};
    CHECK(p.vs.k.constantDataUbo != 0);
    CHECK(p.vs.k.constantDataSize >= 96u);   // the table, padded by ir3 to its vec4 rows
    Encoded e(r, p, pass, wholeDraw(6));
    CHECK(e.ok);
    if (!e.ok) std::printf("       %s\n", e.error.c_str());
    // the direct CP_LOAD_STATE6_GEOM of the vertex constants carries the table's words
    bool found = false;
    const float first[8] = {-1, -1, 0, 1, 1, -1, 0, 1};
    for (size_t i = e.find(CP_LOAD_STATE6_GEOM); i != size_t(-1) && !found; i = e.find(CP_LOAD_STATE6_GEOM, i + 1)) {
        const uint32_t w0 = e.stream.words()[i + 1];
        if (((w0 & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT) != ST6_CONSTANTS ||
            ((w0 & CP_LOAD_STATE6_0_STATE_SRC__MASK) >> CP_LOAD_STATE6_0_STATE_SRC__SHIFT) != SS6_DIRECT) continue;
        found = memcmp(&e.stream.words()[i + 4], first, sizeof first) == 0;
    }
    CHECK(found);
    auto cb = drawOnce(r, p, pass, wholeDraw(6));
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
}

// An indirect draw (-drawPrimitives:indirectBuffer:...) reads its counts and starts as the GPU runs it. The twin that
// shows it: the words change after the draw is recorded and before the commit, and the draw runs the new ones -- the
// count, the instances, and the first vertex the CP puts in VFD_INDEX_OFFSET. An offset off 4 is refused.
TEST(an_indirect_draw_reads_its_arguments_when_it_runs) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Program p = handProgram();
    std::string why;
    auto args = newBuffer(*r.gpu, 64, &why);
    CHECK(args != nullptr);
    if (args == nullptr) return;
    uint32_t* w = static_cast<uint32_t*>(args->contents());
    w[1] = 3;   // vertexCount, instanceCount, vertexStart, baseInstance at offset 4
    w[2] = 1;
    w[3] = 0;
    w[4] = 0;
    Draw d = wholeDraw(3);
    d.indirect = args.get();
    d.indirectOffset = 4;
    auto cb = r.queue->commandBuffer();
    CHECK(cb->draw(p, pass, d, Bindings{}, Bindings{}));
    if (!cb->error().empty()) std::printf("       %s\n", cb->error().c_str());
    w[1] = 6;
    w[2] = 2;
    w[3] = 9;
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    FakeGpu::Stats s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    if (s.faults != 0) std::printf("       %s\n", s.lastFault.c_str());
    CHECK_EQ(s.lastDraw.numIndices, 6u);
    CHECK_EQ(s.lastDraw.numInstances, 2u);
    CHECK_EQ(s.lastDraw.vertexIndexOffset, 9u);
    d.indirectOffset = 2;
    auto bad = drawOnce(r, p, pass, d);
    CHECK(bad->status() != CommandBuffer::kCompleted);
    CHECK(bad->error().find("at a multiple of 4") != std::string::npos);
}

// A parallel render encoder's sub-encoders record into command buffers of their own and are appended in the order they
// were made: append(other) takes OTHER's packets, uses and kept objects as if recorded here. The twin is the order: A
// (3 vertices) then B (6) leaves B's draw last, B then A leaves A's. An error OTHER ended in is the parent's.
TEST(append_runs_another_command_buffers_work_after_this_ones) {
    Rig r;
    auto t = target(r);
    RenderPass pass;
    pass.color0 = t.get();
    Program p = handProgram();
    for (const bool aFirst : {true, false}) {
        auto parent = r.queue->commandBuffer();
        auto a = r.queue->commandBuffer();
        auto b = r.queue->commandBuffer();
        CHECK(a->draw(p, pass, wholeDraw(3), Bindings{}, Bindings{}));
        CHECK(b->draw(p, pass, wholeDraw(6), Bindings{}, Bindings{}));
        CHECK(parent->append(aFirst ? *a : *b));
        CHECK(parent->append(aFirst ? *b : *a));
        const uint64_t before = r.fake->stats().draws;
        parent->commit();
        parent->waitUntilCompleted();
        CHECK(parent->status() == CommandBuffer::kCompleted);
        FakeGpu::Stats s = r.fake->stats();
        CHECK_EQ(s.faults, 0u);
        CHECK_EQ(s.draws - before, 2u);
        CHECK_EQ(s.lastDraw.numIndices, aFirst ? 6u : 3u);
    }
    auto parent = r.queue->commandBuffer();
    auto bad = r.queue->commandBuffer();
    Draw zero = wholeDraw(3);
    zero.vertexCount = 0;
    CHECK(!bad->draw(p, pass, zero, Bindings{}, Bindings{}));
    CHECK(!parent->append(*bad));
    CHECK(parent->error().find("a draw of 0 vertices") != std::string::npos);
}

// generateMipmaps of a cube: every face's levels from its own level 0 (a blit addresses slice 0 alone, so each
// (level, slice) goes through its one-level 2D view). Face 3 holds a checkerboard of 0x60 and 0x20, which averages to
// 0x40 at level 1; face 0 holds zeros and keeps them -- the twin that says the faces were not mixed.
TEST(generate_mipmaps_of_a_cube_makes_each_faces_own_levels) {
    Rig r;
    std::string why;
    TextureDesc d;
    d.type = TextureType::kCube;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = d.height = 16;
    d.depth = 6;
    d.mipLevels = 3;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto cube = Texture::make(*r.gpu, d, &why);
    CHECK(cube != nullptr);
    if (cube == nullptr) return;
    std::vector<uint32_t> face(16 * 16);
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++) face[y * 16 + x] = (x + y) % 2 ? 0x60606060u : 0x20202020u;
    CHECK(cube->replaceSlices(0, 0, 0, 3, 16, 16, 1, face.data(), 64, 1024, &why));
    auto cb = r.queue->commandBuffer();
    CHECK(cb->generateMipmaps(*cube));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    std::vector<uint32_t> l1(8 * 8, 0), z1(8 * 8, 1);
    CHECK(cube->readSlices(1, 0, 0, 3, 8, 8, 1, l1.data(), 32, 256, &why));
    CHECK(cube->readSlices(1, 0, 0, 0, 8, 8, 1, z1.data(), 32, 256, &why));
    bool all40 = true, zeros = true;
    for (uint32_t v : l1) all40 = all40 && v == 0x40404040u;
    for (uint32_t v : z1) zeros = zeros && v == 0;
    CHECK(all40);
    CHECK(zeros);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// RGB10A2Unorm, Resident Evil 4's main colour target: FMT 10_10_10_2_UNORM to the TP and 10_10_10_2_UNORM_DEST to the
// RB (fd6_format_table.c's colour column) -- a draw into it names the second in RB_MRT_BUF_INFO, and a 2D copy reads
// the first and writes the second, giving the words back as they were. The twin: RGBA8, whose two columns are one.
TEST(rgb10a2_is_drawn_through_its_own_colour_format) {
    using namespace a6xx;
    Rig r;
    std::string why;
    TextureDesc d;
    d.format = PixelFormat::kRGB10A2Unorm;
    d.width = d.height = 16;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto t = Texture::make(*r.gpu, d, &why);
    auto u = Texture::make(*r.gpu, d, &why);
    CHECK(t != nullptr && u != nullptr);
    if (t == nullptr || u == nullptr) return;
    RenderPass pass;
    pass.color0 = t.get();
    auto cb = drawOnce(r, flatProgram(), pass, wholeDraw(6, 16, 16));
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK_EQ(r.fake->stats().lastDraw.mrtFormats[0], uint32_t(FMT6_10_10_10_2_UNORM_DEST));
    std::vector<uint32_t> words(16 * 16), back(16 * 16, 0);
    for (uint32_t i = 0; i < words.size(); i++) words[i] = 0x9e3779b1u * (i + 1);
    CHECK(t->replace(0, 0, 0, 16, 16, words.data(), 64, &why));
    auto cb2 = r.queue->commandBuffer();
    CHECK(cb2->blit(*t, 0, render::BlitRect{0, 0, 16, 16}, *u, 0, render::BlitRect{0, 0, 16, 16}, false));
    cb2->commit();
    cb2->waitUntilCompleted();
    CHECK(cb2->status() == CommandBuffer::kCompleted);
    if (cb2->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb2->error().c_str(), r.fake->stats().lastFault.c_str());
    CHECK(u->read(0, 0, 0, 16, 16, back.data(), 64, &why));
    CHECK(back == words);
    TextureDesc rgba = d;
    rgba.format = PixelFormat::kRGBA8Unorm;
    auto v = Texture::make(*r.gpu, rgba, &why);
    RenderPass p2;
    p2.color0 = v.get();
    auto cb3 = drawOnce(r, flatProgram(), p2, wholeDraw(6, 16, 16));
    CHECK_EQ(r.fake->stats().lastDraw.mrtFormats[0], uint32_t(FMT6_8_8_8_8_UNORM));
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// A depth 2D array, a shadow map's cascades as Resident Evil 4 makes them (Depth32Float, drawn into and sampled): each
// layer a tiled plane (the rows to 16, the plane 4096-aligned), a pass drawing into layer 2 points RB_DEPTH_BUFFER_BASE
// two planes on, and the array's descriptor reads every layer TILE6_3, ARRAY_PITCH one plane, DEPTH the layers. The
// twin: layer 0 is the base itself. A stencil format in an array is still refused.
TEST(a_depth_array_is_drawn_layer_by_layer_and_sampled_whole) {
    using namespace a6xx;
    Rig r;
    std::string why;
    auto t = target(r);
    TextureDesc d;
    d.type = TextureType::k2DArray;
    d.format = PixelFormat::kDepth32Float;
    d.width = d.height = 64;
    d.depth = 4;
    d.usage = kTextureUsageRead | kTextureUsageRenderTarget;
    auto z = Texture::make(*r.gpu, d, &why);
    CHECK(z != nullptr);
    if (z == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    const uint64_t plane = z->depthPlaneBytes();
    CHECK_EQ(z->sliceBytes(), plane);
    CHECK(z->buffer()->length() >= plane * 4);
    for (uint32_t layer : {0u, 2u}) {
        RenderPass pass;
        pass.color0 = t.get();
        pass.depth = z.get();
        pass.depthSlice = layer;
        pass.depthLoad = LoadAction::kClear;
        Draw dr = wholeDraw();
        dr.depth = DepthState{1, true};
        auto cb = drawOnce(r, flatProgram(), pass, dr);
        CHECK(cb->status() == CommandBuffer::kCompleted);
        if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
        CHECK_EQ(r.fake->stats().lastDraw.depthBase, z->gpuAddress() + layer * plane);
    }
    const TexConst s = z->sampledDescriptor();
    CHECK_EQ((s.dword[0] & A6XX_TEX_CONST_0_TILE_MODE__MASK) >> A6XX_TEX_CONST_0_TILE_MODE__SHIFT, uint32_t(TILE6_3));
    CHECK_EQ((s.dword[3] & A6XX_TEX_CONST_3_ARRAY_PITCH__MASK) >> A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT << 12, plane);
    CHECK_EQ((s.dword[5] & A6XX_TEX_CONST_5_DEPTH__MASK) >> A6XX_TEX_CONST_5_DEPTH__SHIFT, 4u);
    TextureDesc ds = d;
    ds.format = PixelFormat::kDepth32FloatStencil8;
    CHECK(Texture::make(*r.gpu, ds, &why) == nullptr);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// ...and a layer past the array's, or a layer of a depth texture that is not an array, is refused by name.
TEST(a_depth_slice_past_the_layers_is_refused) {
    Rig r;
    std::string why;
    auto t = target(r);
    auto flat = depthTarget(r);
    RenderPass pass;
    pass.color0 = t.get();
    pass.depth = flat.get();
    pass.depthSlice = 1;
    Encoded e(r, flatProgram(), pass, wholeDraw());
    CHECK(!e.ok);
    expectRefusal(e.error, "depth slice 1 of a depth texture of 1 layer(s)");
}

// A surface's texture viewed whole in another colour format of as many bytes a pixel (mediaserverd's MPS reads an
// RGBA8 one as R32Uint): the same rows, address and pitch; a format of another size, or a single level of it, refused.
TEST(a_surface_texture_views_in_a_format_of_the_same_size) {
    Rig r;
    r.fake->addSurface(7, 64, 64, 256, 0x52474241u);
    TextureDesc d;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = d.height = 64;
    d.usage = kTextureUsageRead;
    std::string why;
    auto t = Texture::wrapSurface(*r.gpu, 7, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) return;
    auto v = Texture::view(t, PixelFormat::kR32Uint, TextureType::k2D, 0, 1, 0, 1, &why);
    CHECK(v != nullptr);
    if (v != nullptr) {
        CHECK_EQ(v->gpuAddress(), t->gpuAddress());
        CHECK_EQ(v->bytesPerRow(), t->bytesPerRow());
        CHECK(v->format() == PixelFormat::kR32Uint);
    }
    CHECK(Texture::view(t, PixelFormat::kRG8Unorm, TextureType::k2D, 0, 1, 0, 1, &why) == nullptr);
}

// A surface reads in the colour formats of the size its FourCC names: 'L00h' (one half) as R16Float, not as RGBA8Unorm;
// a FourCC this list does not know is refused whatever the format.
TEST(a_surface_reads_in_the_formats_of_its_fourccs_size) {
    Rig r;
    r.fake->addSurface(8, 64, 64, 128, 0x4c303068u);   // 'L00h'
    r.fake->addSurface(9, 64, 64, 256, 0x41424344u);   // 'ABCD', not a format
    TextureDesc d;
    d.format = PixelFormat::kR16Float;
    d.width = d.height = 64;
    d.usage = kTextureUsageRead;
    std::string why;
    CHECK(Texture::wrapSurface(*r.gpu, 8, d, &why) != nullptr);
    TextureDesc four = d;
    four.format = PixelFormat::kRGBA8Unorm;
    CHECK(Texture::wrapSurface(*r.gpu, 8, four, &why) == nullptr);
    CHECK(Texture::wrapSurface(*r.gpu, 9, four, &why) == nullptr);
    CHECK(has(why, "IOSurface 9's pixel format 1094861636 is not RGBA8Unorm's"));
}

// A wider format over a surface names its bytes as fewer pixels (the live wallpaper writes a 'BGRA' surface through
// RGBA32Uint, a texel for four pixels): accepted while the texture's row fits the surface's -- 16 RGBA32Uint texels over
// 64 BGRA pixels -- and refused when it does not (17). A narrower format than the surface's is still refused.
TEST(a_wider_format_reads_a_surface_as_fewer_pixels) {
    Rig r;
    r.fake->addSurface(10, 64, 64, 256, 0x42475241u);
    TextureDesc d;
    d.format = PixelFormat::kRGBA32Uint;
    d.width = 16;
    d.height = 64;
    d.usage = kTextureUsageWrite;
    std::string why;
    auto t = Texture::wrapSurface(*r.gpu, 10, d, &why);
    CHECK(t != nullptr);
    if (t == nullptr) std::printf("       %s\n", why.c_str());
    TextureDesc wide = d;
    wide.width = 17;
    CHECK(Texture::wrapSurface(*r.gpu, 10, wide, &why) == nullptr);
    TextureDesc narrow = d;
    narrow.format = PixelFormat::kRG8Unorm;
    narrow.width = 64;
    CHECK(Texture::wrapSurface(*r.gpu, 10, narrow, &why) == nullptr);
}

// GMEM. The fake rasterizes nothing, so a tile holds what its load put there and GMEM starts each tile as 0xcd: a
// target whose every pixel is its own coordinates comes back unchanged only if every tile was loaded from and stored to
// the right rows, at the right place in GMEM, and the tiles cover it -- a store without its load, a tile in the wrong
// place or a gap all show as pixels that differ.
namespace {
void fillCoords(Texture& t, uint32_t salt = 0) {
    auto* p = static_cast<uint8_t*>(t.contents());
    const uint32_t bpp = t.info().bytesPerPixel;
    for (uint32_t y = 0; y < t.height(); y++) {
        for (uint32_t x = 0; x < t.width(); x++) {
            const uint32_t v = (y << 16 | x) ^ salt;
            for (uint32_t b = 0; b < bpp; b += 4) std::memcpy(p + y * t.bytesPerRow() + x * bpp + b, &v, 4);
        }
    }
}
uint32_t coordsWrong(const Texture& t, uint32_t salt = 0) {
    const auto* p = static_cast<const uint8_t*>(t.contents());
    const uint32_t bpp = t.info().bytesPerPixel;
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < t.height(); y++) {
        for (uint32_t x = 0; x < t.width(); x++) {
            const uint32_t v = (y << 16 | x) ^ salt;
            for (uint32_t b = 0; b < bpp; b += 4) wrong += std::memcmp(p + y * t.bytesPerRow() + x * bpp + b, &v, 4) != 0;
        }
    }
    return wrong;
}
}  // namespace

TEST(the_gmem_plan_fits_the_bins_in_gmem_and_covers_the_target) {
    Rig r;
    for (const auto& [w, h, f] : {std::tuple<uint32_t, uint32_t, PixelFormat>{1125, 2436, PixelFormat::kBGRA8Unorm}, {1125, 2436, PixelFormat::kRGBA16Float},
                                  {64, 64, PixelFormat::kRGBA8Unorm}, {2048, 100, PixelFormat::kRGBA8Unorm}, {17, 3, PixelFormat::kRGBA8Unorm}}) {
        auto t = target(r, w, h, kTextureUsageRenderTarget | kTextureUsageRead, f);
        RenderPass a;
        a.color0 = t.get();
        render::GmemPlan p;
        std::string why;
        CHECK(render::gmemPlan(a, &p, &why));
        const uint32_t bpp = t->info().bytesPerPixel;
        CHECK(p.binW % 32 == 0 && p.binH % 16 == 0 && p.binW <= 1024 && p.binH <= 1008);
        CHECK(uint64_t(p.binW) * p.binH * bpp <= a6xx::kGmemCcuColorOffset);
        CHECK(p.cols * p.binW >= w && (p.cols - 1) * p.binW < w && p.rows * p.binH >= h && (p.rows - 1) * p.binH < h);
        std::printf("       %ux%u %s: %u x %u tiles of %u x %u\n", w, h, t->info().name, p.cols, p.rows, p.binW, p.binH);
    }
    // the screen at 4 bytes a pixel: two columns of 576 and twelve rows of 208, 24 tiles
    {
        auto t = target(r, 1125, 2436, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
        RenderPass a;
        a.color0 = t.get();
        render::GmemPlan p;
        CHECK(render::gmemPlan(a, &p, nullptr));
        CHECK(p.cols == 2 && p.rows == 12 && p.binW == 576 && p.binH == 208);
    }
    // what stays in sysmem: a second colour attachment of another size, an integer target
    {
        auto t = target(r), u = target(r, 32, 32);
        RenderPass a;
        a.color0 = t.get();
        a.more[0].texture = u.get();
        render::GmemPlan p;
        std::string why;
        CHECK(!render::gmemPlan(a, &p, &why) && !why.empty());
        auto i = target(r, 64, 64, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kR32Uint);
        RenderPass b;
        b.color0 = i.get();
        CHECK(!render::gmemPlan(b, &p, &why));
    }
}

TEST(a_pass_in_gmem_loads_and_stores_every_tile_in_place) {
    for (const auto& [w, h] : {std::pair<uint32_t, uint32_t>{64, 64}, {1125, 2436}, {100, 37}}) {
        for (int gmem = 0; gmem < 2; gmem++) {
            Rig r;
            auto t = target(r, w, h, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
            fillCoords(*t);
            RenderPass a;
            a.color0 = t.get();
            render::Options o;
            o.gmem = gmem != 0;
            render::GmemPlan p;
            CHECK(render::gmemPlan(a, &p, nullptr));
            auto cb = r.queue->commandBuffer();
            CHECK(cb->draw(flatProgram(), a, wholeDraw(3, w, h), Bindings{}, Bindings{}, o));
            CHECK(cb->draw(flatProgram(), a, wholeDraw(3, w, h), Bindings{}, Bindings{}, o));
            cb->commit();
            cb->waitUntilCompleted();
            CHECK(cb->status() == CommandBuffer::kCompleted);
            const auto s = r.fake->stats();
            CHECK_EQ(s.faults, 0u);
            CHECK_EQ(coordsWrong(*t), 0u);
            if (gmem) {
                CHECK_EQ(cb->counts().gmemPasses, 1u);
                CHECK_EQ(cb->counts().tiles, p.tiles());
                CHECK_EQ(s.tiles, p.tiles());
                CHECK(s.tileLoads == p.tiles() && s.tileStores == p.tiles());
                CHECK_EQ(s.draws, 2 * p.tiles());   // every draw in every tile
                // the stores cover the target once in 16 x 4 granules
                CHECK_EQ(s.storedPixels, uint64_t((w + 15) / 16 * 16) * ((h + 3) / 4 * 4));
            } else {
                CHECK(cb->counts().gmemPasses == 0 && s.tiles == 0 && s.tileStores == 0 && s.draws == 2);
            }
        }
    }
}

// the twin of the test above: the same pass without its loads (gmemLoad off) stores the 0xcd GMEM held, every pixel
// wrong -- so the coordinates surviving above is the loads' doing, not the model leaving memory alone
TEST(a_gmem_store_without_its_load_shows) {
    Rig r;
    auto t = target(r, 100, 37, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
    fillCoords(*t);
    RenderPass a;
    a.color0 = t.get();
    render::Options o;
    o.gmemLoad = false;
    auto cb = r.queue->commandBuffer();
    CHECK(cb->draw(flatProgram(), a, wholeDraw(3, 100, 37), Bindings{}, Bindings{}, o));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().tileLoads, 0u);
    CHECK_EQ(coordsWrong(*t), 100u * 37u);
}

// a draw a tile cannot replay ends the GMEM pass: it and the draws after it are a sysmem pass of their own
TEST(a_draw_gmem_cannot_replay_sends_the_pass_to_sysmem) {
    Rig r;
    auto t = target(r);
    fillCoords(*t);
    std::string why;
    auto vis = newBuffer(*r.gpu, 64, &why);
    RenderPass a;
    a.color0 = t.get();
    auto cb = r.queue->commandBuffer();
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    Draw d = wholeDraw();
    d.visibilityMode = 1;
    d.visibility = vis.get();
    CHECK(cb->draw(flatProgram(), a, d, Bindings{}, Bindings{}));
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const auto s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    // (the first draw in a tile, then the other two in a sysmem pass that loads what the tile stored)
    CHECK(cb->counts().gmemPasses == 1 && cb->counts().passes == 2 && s.tiles == 1 && s.draws == 3);
    CHECK_EQ(coordsWrong(*t), 0u);
}

// CoreAnimation's main pass: two screen-sized attachments, the second with DontCare as its load. GMEM below the colour
// cache is shared out between them by bytes a pixel, so the tiles are half as tall as one attachment's. tri_mrt writes
// both: each tile loads and stores both, and both come back as they were (the fake draws nothing; a bin overlapping
// the other's would scramble one of them). tri_flat writes the first alone: the second is neither loaded nor stored.
TEST(a_pass_of_two_attachments_in_gmem_loads_and_stores_those_drawn) {
    const Program mrt{kernels::tri_flat_vs_air, kernels::tri_mrt_fs_air, Linkage{}, "tri_mrt"};
    for (int both = 0; both < 2; both++) {
        Rig r;
        auto t = target(r, 1125, 2436, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
        auto u = target(r, 1125, 2436, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
        fillCoords(*t);
        fillCoords(*u, 0x80000000u);
        RenderPass a;
        a.color0 = t.get();
        a.more[0].texture = u.get();
        a.more[0].load = LoadAction::kLoad;
        render::GmemPlan p;
        CHECK(render::gmemPlan(a, &p, nullptr));
        CHECK(p.binW == 576 && p.binH == 96 && p.base[0] == 0 && p.base[1] == 0x3c000 && p.room[0] == 0x3c000 && p.room[1] == 0x3c000);
        auto cb = r.queue->commandBuffer();
        CHECK(cb->draw(both ? mrt : flatProgram(), a, wholeDraw(3, 1125, 2436), Bindings{}, Bindings{}));
        CHECK(cb->draw(both ? mrt : flatProgram(), a, wholeDraw(3, 1125, 2436), Bindings{}, Bindings{}));
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        const auto s = r.fake->stats();
        CHECK_EQ(s.faults, 0u);
        if (s.faults) std::printf("       %s\n", s.lastFault.c_str());
        CHECK_EQ(cb->counts().gmemPasses, 1u);
        const uint32_t per = both ? 2 : 1;
        CHECK(s.tiles == p.tiles() && s.tileLoads == per * p.tiles() && s.tileStores == per * p.tiles());
        CHECK_EQ(coordsWrong(*t), 0u);
        CHECK_EQ(coordsWrong(*u, 0x80000000u), 0u);
    }
}

// A pass whose draws touch a corner is tiled over that corner alone: tri_flat at a 100 x 50 viewport at (300, 1000)
// and another at (350, 1010) -- one tile, its loads and stores over the area's granules, the rest of the target left as
// it was (here the coordinates throughout: the fake draws nothing, so a store anywhere puts back what was loaded).
TEST(a_gmem_pass_is_tiled_over_its_render_area) {
    Rig r;
    auto t = target(r, 1125, 2436, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
    fillCoords(*t);
    RenderPass a;
    a.color0 = t.get();
    auto cb = r.queue->commandBuffer();
    Draw d1 = wholeDraw(3), d2 = wholeDraw(3);
    d1.viewport = Viewport{300, 1000, 100, 50, 0, 1};
    d2.viewport = Viewport{350, 1010, 100, 50, 0, 1};
    CHECK(cb->draw(flatProgram(), a, d1, Bindings{}, Bindings{}));
    CHECK(cb->draw(flatProgram(), a, d2, Bindings{}, Bindings{}));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const auto s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    CHECK(s.tiles == 1 && s.tileLoads == 1 && s.tileStores == 1);
    // the area (300, 1000)-(449, 1059) in 16 x 4 granules: x 288 to 463, y 1000 to 1059
    CHECK_EQ(s.storedPixels, uint64_t(464 - 288) * (1060 - 1000));
    CHECK_EQ(coordsWrong(*t), 0u);
}

// A framebuffer fetch in a pass whose draws touch a corner: the fetch's descriptor was written with the plan's bin
// width before the area was known, so the tiles over the area keep that width (the fake judges the descriptor's pitch
// against the bin the tile uses). With the bins narrowed to the area the fetch read sheared rows: SpringBoard in
// stripes over two tile columns.
TEST(a_fetch_in_a_corner_reads_bins_of_the_width_its_descriptor_has) {
    Rig r;
    auto t = target(r, 1125, 2436, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
    const Program fb{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
    RenderPass a;
    a.color0 = t.get();
    render::GmemPlan whole;
    CHECK(render::gmemPlan(a, &whole, nullptr));
    auto cb = r.queue->commandBuffer();
    Draw d = wholeDraw(6);
    d.viewport = Viewport{300, 1000, 100, 50, 0, 1};
    CHECK(cb->draw(fb, a, d, Bindings{}, Bindings{}));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const auto s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    if (s.faults) std::printf("       %s\n", s.lastFault.c_str());
    CHECK(cb->counts().gmemPasses == 1 && s.tiles == 1);
    const auto gd = r.fake->memory(s.lastDraw.fsTexConst, 64);
    uint32_t gw[16] = {};
    if (gd.size() == 64) memcpy(gw, gd.data(), 64);
    CHECK_EQ((gw[2] & a6xx::A6XX_TEX_CONST_2_PITCH__MASK) >> a6xx::A6XX_TEX_CONST_2_PITCH__SHIFT, whole.binW * 4);
}

// GMEM or sysmem by what such passes cost: untried sysmem first, then GMEM, then the cheaper, the other again every
// sixteenth pass -- every sixty-fourth when it is dearer by half. And a command buffer's completion feeds its passes'
// times in: after one sysmem pass into a target, the same pass of the next buffer is tried in GMEM.
TEST(the_queue_picks_gmem_or_sysmem_by_what_each_cost_a_target) {
    Rig r;
    CHECK(!r.queue->preferGmem(1));   // nothing known: sysmem tried
    r.queue->tuned(1, false, 100);
    CHECK(r.queue->preferGmem(1));    // sysmem known, GMEM not: GMEM tried
    r.queue->tuned(1, true, 110);
    uint32_t gmem = 0;
    for (int i = 0; i < 64; i++) gmem += r.queue->preferGmem(1);
    CHECK_EQ(gmem, 4u);               // sysmem the cheaper, by a little: GMEM on the sixteenth passes
    r.queue->tuned(2, true, 900);
    r.queue->tuned(2, false, 100);
    gmem = 0;
    for (int i = 0; i < 128; i++) gmem += r.queue->preferGmem(2);
    CHECK_EQ(gmem, 2u);               // by far: on the sixty-fourth
    r.queue->tuned(1, false, 5000);   // (sysmem grew dear: its average follows, GMEM taken)
    r.queue->tuned(1, false, 5000);
    gmem = 0;
    for (int i = 0; i < 64; i++) gmem += r.queue->preferGmem(1);
    CHECK_EQ(gmem, 63u);
    auto t = target(r);
    RenderPass a;
    a.color0 = t.get();
    for (int round = 0; round < 2; round++) {
        auto cb = r.queue->commandBuffer();
        cb->setGmemTuned(true);
        CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(cb->counts().gmemPasses, round == 0 ? 0u : 1u);
    }
    CHECK_EQ(r.queue->lastDraws(render::tuneKeyOf(a), 0), 1u);
    // (untuned, the default: GMEM whatever the queue knows -- what a test asking for GMEM gets)
    auto u = r.queue->commandBuffer();
    CHECK(u->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    u->commit();
    u->waitUntilCompleted();
    CHECK_EQ(u->counts().gmemPasses, 1u);
}

// GMEM for a pass of enough draws, decided at its end: the pass is recorded both ways and the words of one go to the
// stream -- 1 draw and 3 draws under a minimum of 2, and 2 fetching draws (whose sysmem side has a table of its own: the
// fetch reads memory there); and a pass in which a draw a tile cannot replay comes (a visibility count) goes on in
// sysmem whole, its draws not split off into a pass of their own
TEST(gmem_for_a_pass_of_enough_draws_decided_at_its_end) {
    Rig r;
    auto t = target(r);
    RenderPass a;
    a.color0 = t.get();
    const Program fb{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
    for (const auto& [draws, fetch, want] : {std::tuple<int, bool, uint32_t>{1, false, 0}, {3, false, 1}, {1, false, 0}, {2, true, 1}, {1, true, 0}}) {
        auto cb = r.queue->commandBuffer();
        cb->setGmemMinDraws(2);
        for (int i = 0; i < draws; i++) CHECK(cb->draw(fetch ? fb : flatProgram(), a, wholeDraw(6), Bindings{}, Bindings{}));
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(r.fake->stats().faults, 0u);
        CHECK_EQ(cb->counts().gmemPasses, want);
        CHECK_EQ(cb->counts().passes, 1u);
    }
    std::string why;
    auto vis = newBuffer(*r.gpu, 64, &why);
    auto cb = r.queue->commandBuffer();
    cb->setGmemMinDraws(2);
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    Draw d = wholeDraw();
    d.visibilityMode = 1;
    d.visibility = vis.get();
    CHECK(cb->draw(flatProgram(), a, d, Bindings{}, Bindings{}));
    CHECK(cb->draw(flatProgram(), a, wholeDraw(), Bindings{}, Bindings{}));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().faults, 0u);
    CHECK(cb->counts().gmemPasses == 0 && cb->counts().passes == 1);
}

// A DontCare attachment is not loaded when the granules past its edge stay in its rows: 100 x 36 (the right edge in the
// rows' padding, 36 rows a multiple of 4) -- stored, not loaded; 100 x 37 (a granule row past the last) -- loaded too
TEST(a_dontcare_attachment_is_loaded_only_when_the_granules_pass_its_last_row) {
    for (const auto& [h, loads] : {std::pair<uint32_t, uint32_t>{36, 0}, {37, 1}}) {
        Rig r;
        auto t = target(r, 100, h, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
        RenderPass a;
        a.color0 = t.get();
        a.load = LoadAction::kDontCare;
        auto cb = r.queue->commandBuffer();
        CHECK(cb->draw(flatProgram(), a, wholeDraw(3, 100, h), Bindings{}, Bindings{}));
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        const auto s = r.fake->stats();
        CHECK(s.tiles == 1 && s.tileLoads == loads && s.tileStores == 1);
    }
}

// A Clear in a GMEM pass is done in each tile by the event blit (RB_BLIT_CLEAR_COLOR, packed RGBA as the base format)
// rather than by the 2D engine into memory and loaded back. What the tiles store must be what the fill writes: the
// same bytes for BGRA8 (whose swap the store puts back), sRGB RGBA8 and RGBA16Float. A format gmemClearWords does not
// pack (R32Float) is filled as before and loaded.
TEST(a_clear_in_gmem_comes_out_as_the_fill_does) {
    for (const PixelFormat f : {PixelFormat::kBGRA8Unorm, PixelFormat::kRGBA8UnormSrgb, PixelFormat::kRGBA16Float, PixelFormat::kR32Float}) {
        std::vector<uint8_t> got[2];
        uint32_t clears = 0, fills = 0;
        for (int gm = 0; gm < 2; gm++) {
            Rig r;
            auto t = target(r, 100, 37, kTextureUsageRenderTarget | kTextureUsageRead, f);
            fillCoords(*t);
            RenderPass a;
            a.color0 = t.get();
            a.load = LoadAction::kClear;
            const double c[4] = {0.25, 0.5, 0.75, 0.6};
            for (int k = 0; k < 4; k++) a.clearColor[k] = c[k];
            render::Options o;
            o.gmem = gm != 0;
            // (a draw over a corner: the Clear covers the whole target all the same)
            Draw d = wholeDraw(3, 100, 37);
            d.viewport = Viewport{40, 10, 30, 12, 0, 1};
            auto cb = drawOnce(r, flatProgram(), a, d, o);
            CHECK(cb->status() == CommandBuffer::kCompleted);
            CHECK_EQ(r.fake->stats().faults, 0u);
            const uint8_t* p = static_cast<const uint8_t*>(t->contents());
            for (uint32_t y = 0; y < 37; y++) got[gm].insert(got[gm].end(), p + y * t->bytesPerRow(), p + y * t->bytesPerRow() + 100 * t->info().bytesPerPixel);
            if (gm) clears = r.fake->stats().tileClears, fills = r.fake->stats().fills;
        }
        CHECK(got[0] == got[1]);
        const bool packed = f != PixelFormat::kR32Float;
        CHECK_EQ(clears, packed ? 1u : 0u);
        CHECK_EQ(fills, packed ? 0u : 1u);
    }
}

// A binned GMEM pass: the binning pass runs every draw once (in BINNING_PASS), then each tile is given its streams
// (CP_SET_BIN_DATA5) and draws USE_VISIBILITY; the target comes back as it was through the tiles' loads and stores
TEST(a_binned_gmem_pass_bins_its_draws_then_tells_each_tile_its_streams) {
    Rig r;
    auto t = target(r, 1125, 300, kTextureUsageRenderTarget | kTextureUsageRead, PixelFormat::kBGRA8Unorm);
    fillCoords(*t);
    RenderPass a;
    a.color0 = t.get();
    render::GmemPlan p;
    CHECK(render::gmemPlan(a, &p, nullptr));
    render::Options o;
    o.gmemBinning = true;
    auto cb = r.queue->commandBuffer();
    for (int i = 0; i < 3; i++) CHECK(cb->draw(flatProgram(), a, wholeDraw(3, 1125, 300), Bindings{}, Bindings{}, o));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    const auto s = r.fake->stats();
    CHECK_EQ(s.faults, 0u);
    if (s.faults) std::printf("       %s\n", s.lastFault.c_str());
    CHECK(s.binningPasses == 1 && s.binnedDraws == 3 && s.binData == p.tiles() && s.tiles == p.tiles());
    CHECK_EQ(s.lastDraw.visCull, 1u);   // USE_VISIBILITY
    CHECK_EQ(coordsWrong(*t), 0u);
}
