// render::link over the stage fixtures and over stages built by hand: the numbers ir3_link_shaders would give (worked
// by hand in air-vs-fs.md from the same tables, and with MESA=1 held to Mesa's own function by compiler_test through
// linkProgram), and the sentence each refusal comes with. Every positive check has beside it a mutation the link
// refuses, so that no check passes for want of looking.
//
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/render.hpp"
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_flat_vs_air.hpp"
#include "test.hpp"

using namespace hakios::metal;
namespace kernels = hakios::metal::kernels;

namespace {

// gl_varying_slot / gl_frag_result numbers as the fixtures carry them (Mesa shader_enums.h:320, 359, 987)
constexpr uint8_t kSlotPos = 0;
constexpr uint8_t kSlotVar0 = 32;
constexpr uint8_t kSlotVar1 = 33;
constexpr uint8_t kFragData0 = 4;
constexpr uint32_t regid(uint32_t n, uint32_t c) { return n * 4 + c; }

void expectRefusal(const std::string& got, const char* want) {
    CHECK(got == want);
    if (got != want) std::fprintf(stderr, "  got      \"%s\"\n  expected \"%s\"\n", got.c_str(), want);
}

bool sameVar(const Linkage::Var& v, uint8_t slot, uint8_t regid_, uint8_t compmask, uint8_t loc) {
    bool same = v.slot == slot && v.regid == regid_ && v.compmask == compmask && v.loc == loc;
    if (!same) std::fprintf(stderr, "  var {%u, 0x%02x, 0x%x, %u}, expected {%u, 0x%02x, 0x%x, %u}\n", v.slot, v.regid, v.compmask, v.loc, slot, regid_, compmask, loc);
    return same;
}

bool allZero(const uint32_t* a, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != 0) return false;
    }
    return true;
}

}  // namespace

// The fixtures themselves say what the listings said (tools/kernels/tri_*.ll, the footers in the fixtures): the shape
// the link reads. Each number here is from the ir3 listing of 29 October, not from the link.
TEST(stage_fixtures_carry_what_the_listings_say) {
    const Stage& fvs = kernels::tri_flat_vs_air;
    CHECK_EQ(fvs.kind, uint8_t(kStageVertex));
    CHECK_EQ(fvs.numInputs, 1u);
    CHECK_EQ(fvs.inputs[0].slot, 9u);        // SYSTEM_VALUE_VERTEX_ID
    CHECK(fvs.inputs[0].sysval && !fvs.inputs[0].bary);
    CHECK_EQ(fvs.inputs[0].regid, regid(0, 0));
    CHECK_EQ(fvs.vertexIdRegid, regid(0, 0));
    CHECK_EQ(fvs.instanceIdRegid, a6xx::kRegidNone);
    CHECK_EQ(fvs.numOutputs, 1u);
    CHECK_EQ(fvs.outputs[0].slot, kSlotPos);
    CHECK_EQ(fvs.positionRegid, uint32_t(fvs.outputs[0].regid));   // r1.z in the listing
    CHECK_EQ(fvs.positionRegid, regid(1, 2));
    CHECK_EQ(fvs.numAttributes, 0u);
    CHECK_EQ(fvs.numColorOutputs, 0u);
    CHECK_EQ(fvs.k.constlen, 8u);            // ir3's gen-6 floor for a VS (ir3_compiler_nir.c:6035-6037)
    // instrlen is ir3's DIV_ROUND_UP(instr_count, 16) over the ENCODED instructions (ir3.c:264; a (rptN) counts once),
    // 16 here with `end`; the binary is padded to at least four nops past them and then to 128 bytes (ir3.c:270,
    // ir3_shader.c:112), which is why the words are two groups long while INSTRLEN says one -- as turnip programs it
    CHECK_EQ(fvs.k.instrlen, 1u);
    CHECK_EQ(fvs.k.numWords, 64u);
    CHECK_EQ(fvs.k.numUboRanges, 0u);
    CHECK_EQ(fvs.k.numSsbos, 0u);
    CHECK_EQ(fvs.k.numUbos, 0u);
    CHECK(fvs.writesPos);

    const Stage& ffs = kernels::tri_flat_fs_air;
    CHECK_EQ(ffs.kind, uint8_t(kStageFragment));
    CHECK_EQ(ffs.totalIn, 0u);
    CHECK_EQ(ffs.numInputs, 1u);             // the unused BARYCENTRIC_PERSP_PIXEL entry at r63.x ir3 always adds
    CHECK_EQ(ffs.inputs[0].slot, 56u);
    CHECK(ffs.inputs[0].sysval && !ffs.inputs[0].bary);
    CHECK_EQ(ffs.inputs[0].regid, a6xx::kRegidNone);
    CHECK_EQ(ffs.ijRegid[0], a6xx::kRegidNone);
    CHECK_EQ(ffs.numColorOutputs, 1u);
    CHECK_EQ(ffs.colorRegid[0], regid(0, 0));
    CHECK_EQ(ffs.outputs[0].slot, kFragData0);
    CHECK_EQ(ffs.outputs[0].pad, 0u);        // the output's class: float
    CHECK_EQ(ffs.k.constlen, 0u);
    CHECK_EQ(ffs.k.instrlen, 1u);
    CHECK_EQ(ffs.k.numImmediates, 0u);       // the colour is four mov immediates, no const
    CHECK(!ffs.writesPos && !ffs.writesSmask && !ffs.writesStencilref && !ffs.hasKill && !ffs.fbRead);
    CHECK_EQ(ffs.numSamplerPrefetch, 0u);

    const Stage& cvs = kernels::tri_color_vs_air;
    CHECK_EQ(cvs.numOutputs, 2u);
    CHECK_EQ(cvs.outputs[0].slot, kSlotPos);
    CHECK_EQ(cvs.outputs[1].slot, kSlotVar0);
    CHECK_EQ(cvs.positionRegid, regid(3, 1));        // r3.y
    CHECK_EQ(cvs.outputs[1].regid, regid(4, 1));     // r4.y
    CHECK_EQ(cvs.k.constlen, 8u);
    CHECK_EQ(cvs.k.instrlen, 2u);            // 31 encoded instructions with `end`: two groups; 96 words with the padding
    CHECK_EQ(cvs.k.numWords, 96u);

    const Stage& cfs = kernels::tri_color_fs_air;
    CHECK_EQ(cfs.totalIn, 4u);
    CHECK_EQ(cfs.numInputs, 2u);
    CHECK_EQ(cfs.inputs[0].slot, kSlotVar0);
    CHECK(cfs.inputs[0].bary && !cfs.inputs[0].sysval && !cfs.inputs[0].flat);
    CHECK_EQ(cfs.inputs[0].compmask, 0xfu);
    CHECK_EQ(cfs.inputs[0].inloc, 0u);
    CHECK_EQ(cfs.inputs[0].regid, a6xx::kRegidNone);        // a bary input has no register of its own: bary.f fetches it
    CHECK_EQ(cfs.inputs[1].slot, 56u);               // SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL at r0.x
    CHECK_EQ(cfs.inputs[1].regid, regid(0, 0));
    CHECK_EQ(cfs.ijRegid[0], regid(0, 0));
    for (uint32_t i = 1; i < kIjCount; i++) CHECK_EQ(cfs.ijRegid[i], a6xx::kRegidNone);
    CHECK_EQ(cfs.colorRegid[0], regid(0, 2));        // r0.z
    CHECK_EQ(cfs.k.constlen, 0u);
}

// pair 1: no varying, the position alone at location 0 (air-vs-fs.md, "Pair 1" worked by hand)
TEST(link_tri_flat_is_the_position_alone) {
    Linkage l{};
    std::string why;
    CHECK(render::link(kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, &l, &why));
    if (!why.empty()) std::printf("       %s\n", why.c_str());
    CHECK_EQ(l.cnt, 1u);
    CHECK_EQ(l.maxLoc, 4u);
    CHECK_EQ(l.positionLoc, 0u);
    CHECK(sameVar(l.var[0], kSlotPos, uint8_t(regid(1, 2)), 0xf, 0));
    CHECK(allZero(l.varmask, 4));           // VPC_VAR_DISABLE = ~0 four times: the position's locations are not varyings
    CHECK(allZero(l.interpMode, 8));
    CHECK(allZero(l.psReplMode, 8));
    CHECK_EQ(l.mrtComponents, 0xfu);
    // the mutations: the same link refused, with the sentence
    Stage vs = kernels::tri_flat_vs_air;
    vs.positionRegid = a6xx::kRegidNone;
    CHECK(!render::link(vs, kernels::tri_flat_fs_air, &l, &why));
    expectRefusal(why, "tri_flat_vs_air writes no position: nothing to rasterize");
    vs = kernels::tri_flat_vs_air;
    vs.positionRegid |= 0x100;              // ir3_get_output_regid's HALF_REG_ID
    CHECK(!render::link(vs, kernels::tri_flat_fs_air, &l, &why));
    expectRefusal(why, "tri_flat_vs_air writes its position in a half register: not served");
    CHECK(!render::link(kernels::tri_flat_fs_air, kernels::tri_flat_fs_air, &l, &why));
    expectRefusal(why, "tri_flat_fs_air is not a vertex stage");
    CHECK(!render::link(kernels::tri_flat_vs_air, kernels::tri_flat_vs_air, &l, &why));
    expectRefusal(why, "tri_flat_vs_air is not a fragment stage");
    // a colour output the FS does not write leaves its nibble clear
    Stage fs = kernels::tri_flat_fs_air;
    fs.colorRegid[0] = a6xx::kRegidNone;
    CHECK(render::link(kernels::tri_flat_vs_air, fs, &l, &why));
    CHECK_EQ(l.mrtComponents, 0u);
}

// pair 2: the colour varying at location 0 (the FS's inloc), the position after it at 4 ("Pair 2" worked by hand)
TEST(link_tri_color_puts_the_varying_first_and_the_position_at_4) {
    Linkage l{};
    std::string why;
    CHECK(render::link(kernels::tri_color_vs_air, kernels::tri_color_fs_air, &l, &why));
    if (!why.empty()) std::printf("       %s\n", why.c_str());
    CHECK_EQ(l.cnt, 2u);
    CHECK_EQ(l.maxLoc, 8u);
    CHECK_EQ(l.positionLoc, 4u);
    CHECK(sameVar(l.var[0], kSlotVar0, uint8_t(regid(4, 1)), 0xf, 0));
    CHECK(sameVar(l.var[1], kSlotPos, uint8_t(regid(3, 1)), 0xf, 4));
    CHECK_EQ(l.varmask[0], 0xfu);           // VPC_VAR_DISABLE[0] = 0xfffffff0
    CHECK_EQ(l.varmask[1], 0u);
    CHECK_EQ(l.varmask[2], 0u);
    CHECK_EQ(l.varmask[3], 0u);
    CHECK(allZero(l.interpMode, 8));        // smooth: INTERP_SMOOTH is 0
    CHECK(allZero(l.psReplMode, 8));
    CHECK_EQ(l.mrtComponents, 0xfu);
    // a flat varying: INTERP_FLAT (1) in each component's two bits at inloc * 2
    Stage fs = kernels::tri_color_fs_air;
    fs.inputs[0].flat = 1;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.interpMode[0], 0x55u);
    CHECK(allZero(l.interpMode + 1, 7));
    // a varying at inloc 8 with two components: the locations move with it, the varmask follows
    fs = kernels::tri_color_fs_air;
    fs.inputs[0].inloc = 8;
    fs.inputs[0].compmask = 0x3;
    fs.inputs[0].flat = 1;
    fs.totalIn = 10;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.cnt, 2u);
    CHECK(sameVar(l.var[0], kSlotVar0, uint8_t(regid(4, 1)), 0x3, 8));
    CHECK_EQ(l.maxLoc, 14u);
    CHECK_EQ(l.positionLoc, 10u);
    CHECK_EQ(l.varmask[0], 0x3u << 8);
    CHECK_EQ(l.interpMode[0], 0x5u << 16);
    // ir3_link_shaders' own rule: an input at or past total_in is a varying ir3 allocated and found unread -- skipped,
    // so the link is the position alone (the mutation that shows totalIn is looked at)
    fs = kernels::tri_color_fs_air;
    fs.totalIn = 0;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.cnt, 1u);
    CHECK_EQ(l.maxLoc, 4u);
    CHECK_EQ(l.positionLoc, 0u);
    // and a bary input with an empty compmask is not a varying either (ir3_next_varying: compmask && bary)
    fs = kernels::tri_color_fs_air;
    fs.inputs[0].compmask = 0;
    CHECK(render::link(kernels::tri_color_vs_air, fs, &l, &why));
    CHECK_EQ(l.cnt, 1u);
}

// an unused varying is fine (the VS writes VAR0, the FS reads nothing); a varying the VS does not write is not
TEST(link_refuses_a_varying_the_vertex_stage_does_not_write) {
    Linkage l{};
    std::string why;
    CHECK(render::link(kernels::tri_color_vs_air, kernels::tri_flat_fs_air, &l, &why));
    CHECK_EQ(l.cnt, 1u);
    CHECK_EQ(l.maxLoc, 4u);
    CHECK(sameVar(l.var[0], kSlotPos, uint8_t(regid(3, 1)), 0xf, 0));
    // the fixtures crossed the other way
    CHECK(!render::link(kernels::tri_flat_vs_air, kernels::tri_color_fs_air, &l, &why));
    expectRefusal(why, "the fragment stage reads varying slot 32 (VAR0), which the vertex stage does not write");
    // built by hand: the FS wants VAR1, the VS writes VAR0
    Stage fs = kernels::tri_color_fs_air;
    fs.inputs[0].slot = kSlotVar1;
    CHECK(!render::link(kernels::tri_color_vs_air, fs, &l, &why));
    expectRefusal(why, "the fragment stage reads varying slot 33 (VAR1), which the vertex stage does not write");
    // and the VS renamed to serve it links again: the refusal was about the slot and nothing else
    Stage vs = kernels::tri_color_vs_air;
    vs.outputs[1].slot = kSlotVar1;
    CHECK(render::link(vs, fs, &l, &why));
    CHECK_EQ(l.cnt, 2u);
    CHECK(sameVar(l.var[0], kSlotVar1, uint8_t(regid(4, 1)), 0xf, 0));
    // a VS with no position at all (its one output renamed away) is refused by the position rule, after the varyings
    vs = kernels::tri_color_vs_air;
    vs.positionRegid = a6xx::kRegidNone;
    CHECK(!render::link(vs, kernels::tri_color_fs_air, &l, &why));
    expectRefusal(why, "tri_color_vs_air writes no position: nothing to rasterize");
}

// the link's own capacity: var[] holds kMaxStageOutputs entries, the position needs one of them
TEST(link_refuses_more_varyings_than_the_table_holds) {
    Stage vs = kernels::tri_color_vs_air;
    Stage fs = kernels::tri_color_fs_air;
    // 32 varyings VAR0..VAR31, one component each at inloc i, every one written by the VS
    vs.numOutputs = kMaxStageOutputs;
    vs.outputs[0] = StageOutput{kSlotPos, uint8_t(regid(3, 1)), 0, 0};
    for (uint32_t i = 1; i < kMaxStageOutputs; i++) vs.outputs[i] = StageOutput{uint8_t(kSlotVar0 + i - 1), uint8_t(i), 0, 0};
    fs.numInputs = kMaxStageOutputs - 1;
    for (uint32_t i = 0; i < fs.numInputs; i++) fs.inputs[i] = StageInput{uint8_t(kSlotVar0 + i), a6xx::kRegidNone, 0x1, uint8_t(i), 0, 1, 0, 0};
    fs.totalIn = fs.numInputs;
    Linkage l{};
    std::string why;
    // 31 varyings + the position = 32: fits exactly
    CHECK(render::link(vs, fs, &l, &why));
    CHECK_EQ(l.cnt, kMaxStageOutputs);
    CHECK_EQ(l.positionLoc, 31u);
    CHECK_EQ(l.maxLoc, 35u);
    // the VS writing VAR31 too and the FS reading it: 32 varyings, no room for the position
    vs.numOutputs = kMaxStageOutputs;
    vs.outputs[0] = StageOutput{uint8_t(kSlotVar0 + 31), 40, 0, 0};   // the position entry replaced; positionRegid still says r3.y
    fs.numInputs = kMaxStageOutputs;
    fs.inputs[31] = StageInput{uint8_t(kSlotVar0 + 31), a6xx::kRegidNone, 0x1, 31, 0, 1, 0, 0};
    fs.totalIn = 32;
    CHECK(!render::link(vs, fs, &l, &why));
    expectRefusal(why, "the link holds 32 entries and has no room left for the position");
}
