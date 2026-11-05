// render::link -- the VS to FS link as ir3_link_shaders computes it (Mesa 25.0.7, src/freedreno/ir3/ir3_shader.h:
// ir3_link_add at 1289-1308, ir3_link_shaders at 1311-1361), ported over the plain data of render.hpp's Stage so that a
// build without Mesa can compute it and a fixture can carry it; with MESA=1, compiler_test holds this port to Mesa's own
// function over the same two tables (src/compiler.cpp: linkProgram).
//
// SPDX-License-Identifier: MIT
//
// What the link decides, and which register reads it (turnip tu6_emit_vpc, tu_pipeline.cc:890-1070; fd6_program.cc
// setup_stateobj's linkage block): the FRAGMENT stage's bary inputs set the VPC locations ("what the hw calls outloc
// is taken from the inloc of the frag shader", ir3_shader.h:729-735), each paired with the VERTEX output of the same
// slot for SP_VS_OUT_REG (regid, compmask) and SP_VS_VPC_DST_REG (loc); the position is appended after them at
// max_loc (VPC_VS_PACK.POSITIONLOC) with compmask 0xf; max_loc is the stride (VPC_VS_PACK / PC_VS_OUT_CNTL
// .STRIDE_IN_VPC); varmask, the FS's varying locations without the position's, is the complement of VPC_VAR_DISABLE;
// cnt is SP_VS_PRIMITIVE_CNTL.OUT.
//
// Where this port is stricter than Mesa: ir3_link_shaders gives an FS varying that no VS output serves the register
// regid(63, 0) (default_regid with pack_vs_out) and the FS then reads zeros through it -- a quiet black. Here that is
// refused by slot, so that a vertex function and a fragment function whose varyings do not match are told apart from
// a draw that happened. The COL/BFC fallback of ir3_find_output (ir3_shader.h:1197-1215, GL's two-sided colour) is
// not ported: hakios's front end assigns VARYING_SLOT_VAR0 + n to every Metal varying and never COL0/BFC0
// (air_to_nir.c translate_output / translate_input), so the fallback could never fire on a Stage from it.
#include "hakios-metal/render.hpp"

#include "hakios-metal/a6xx.hpp"

namespace hakios::metal::render {

namespace {

// gl_varying_slot numbers the Stage tables use (Mesa src/compiler/shader_enums.h:320, 359): POS is 0 and the generic
// varyings start at 32, VARn = 32 + n; the slots between are GL's fixed-function ones, which the front end never emits
constexpr uint8_t kVaryingSlotPos = 0;
constexpr uint8_t kVaryingSlotVar0 = 32;
constexpr uint8_t kVaryingSlotPsiz = 12;   // gl_varying_slot's VARYING_SLOT_PSIZ: a VS's [[point_size]]
constexpr uint8_t kVaryingSlotPntc = 25;   // [[point_coord]] (compiler.cpp lowers it to this input), filled by the VPC

// VPC_VARYING_INTERP_MODE's two-bit field per component: INTERP_SMOOTH 0, INTERP_FLAT 1, INTERP_ZERO 2, INTERP_ONE 3
// (a6xx.xml.h enum a6xx_varying_interp_mode); 16 components per register (tu_pipeline.cc tu6_emit_vpc_varying_modes)
constexpr uint32_t kInterpFlat = 1;
constexpr uint32_t kInterpZero = 2;
constexpr uint32_t kInterpOne = 3;
// VPC_VARYING_PS_REPL_MODE's: the point sprite's S and T (a6xx.xml.h enum a6xx_varying_ps_repl_mode)
constexpr uint32_t kPsReplS = 1;
constexpr uint32_t kPsReplT = 2;

std::string slotName(uint8_t slot) {
    if (slot == kVaryingSlotPos) return "POS";
    if (slot >= kVaryingSlotVar0) return "VAR" + std::to_string(slot - kVaryingSlotVar0);
    return "slot " + std::to_string(slot);
}

// util_last_bit over a compmask: how many components the mask spans, counting from .x
uint32_t lastBit(uint32_t mask) {
    uint32_t n = 0;
    while (mask != 0) {
        n++;
        mask >>= 1;
    }
    return n;
}

// ir3_find_output (ir3_shader.h:1190-1196), without the COL/BFC fallback (see the file comment)
int findOutput(const Stage& vs, uint8_t slot) {
    for (uint32_t j = 0; j < vs.numOutputs; j++) {
        if (vs.outputs[j].slot == slot) return int(j);
    }
    return -1;
}

// ir3_link_add (ir3_shader.h:1289-1308): the location's components into varmask, max_loc raised past them, and an
// entry in var[] when the VS has a register for it. False when var[] is full (ir3 asserts; here a refusal upstream).
bool linkAdd(Linkage& l, uint8_t slot, uint32_t regid, uint8_t compmask, uint8_t loc) {
    for (uint32_t j = 0; j < lastBit(compmask); j++) {
        uint32_t comploc = loc + j;
        l.varmask[comploc / 32] |= 1u << (comploc % 32);
    }
    uint32_t end = loc + lastBit(compmask);
    if (end > l.maxLoc) l.maxLoc = end;
    if (regid != a6xx::kRegidNone) {
        if (l.cnt >= kMaxStageOutputs) return false;
        l.var[l.cnt] = Linkage::Var{slot, uint8_t(regid), compmask, loc};
        l.cnt++;
    }
    return true;
}

bool refuse(std::string* why, const std::string& text) {
    if (why != nullptr) *why = text;
    return false;
}

}  // namespace

bool link(const Stage& vs, const Stage& fs, Linkage* out, std::string* why) {
    if (vs.kind != kStageVertex) return refuse(why, std::string(vs.k.name ? vs.k.name : "the first stage") + " is not a vertex stage");
    if (fs.kind != kStageFragment) return refuse(why, std::string(fs.k.name ? fs.k.name : "the second stage") + " is not a fragment stage");
    Linkage l{};
    // --- ir3_link_shaders' walk: the FS's bary inputs (ir3_next_varying: compmask && bary, ir3_shader.h:1222-1229),
    // each within total_in (:1338-1346; an input past it is a varying ir3 allocated and then found unread), each
    // paired with the VS output of its slot
    for (uint32_t j = 0; j < fs.numInputs; j++) {
        const StageInput& in = fs.inputs[j];
        if (in.compmask == 0 || !in.bary) continue;
        if (in.inloc >= fs.totalIn) continue;
        if (in.slot == kVaryingSlotPntc) {
            // no VS register: the VPC writes S and T for a point sprite, and the z and w a vec4 read would see as 0 and
            // 1, at the input's packed components (tu6_vpc_varying_mode). Metal's point_coord has its origin at the
            // top left, as Vulkan's has, so T and not ONE_MINUS_T. On a primitive that is not a point Metal leaves
            // the value undefined.
            if (!linkAdd(l, in.slot, a6xx::kRegidNone, in.compmask, in.inloc)) {
                return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and the fragment stage reads more varyings than that");
            }
            uint32_t loc = in.inloc;
            const uint32_t repl[2] = {kPsReplS, kPsReplT}, interp[2] = {kInterpZero, kInterpOne};
            for (uint32_t c = 0; c < 4; c++) {
                if (((in.compmask >> c) & 1) == 0) continue;
                if (c < 2) l.psReplMode[loc / 16] |= repl[c] << ((loc % 16) * 2);
                else l.interpMode[loc / 16] |= interp[c - 2] << ((loc % 16) * 2);
                loc++;
            }
            continue;
        }
        int k = findOutput(vs, in.slot);
        if (k < 0) {
            return refuse(why, "the fragment stage reads varying slot " + std::to_string(in.slot) + " (" + slotName(in.slot) +
                                   "), which the vertex stage does not write");
        }
        if (!linkAdd(l, in.slot, vs.outputs[k].regid, in.compmask, in.inloc)) {
            return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and the fragment stage reads more varyings than that");
        }
        // VPC_VARYING_INTERP_MODE: INTERP_FLAT for each component of a flat input, at two bits per location
        // (tu_pipeline.cc tu6_emit_vpc_varying_modes; fd6_program.cc emit_interp_state); smooth is the zero
        if (in.flat) {
            for (uint32_t c = 0; c < lastBit(in.compmask); c++) {
                if (((in.compmask >> c) & 1) == 0) continue;
                uint32_t loc = in.inloc + c;
                l.interpMode[loc / 16] |= kInterpFlat << ((loc % 16) * 2);
            }
        }
    }
    // --- the position, appended at max_loc with every component (tu_pipeline.cc:938-950, fd6_program.cc:707-710)
    if (vs.positionRegid == a6xx::kRegidNone) {
        return refuse(why, std::string(vs.k.name ? vs.k.name : "the vertex stage") + " writes no position: nothing to rasterize");
    }
    if (vs.positionRegid > 0xff) {
        // ir3_get_output_regid's HALF_REG_ID (0x100): a half-precision position has no place in SP_VS_OUT_REG's 8-bit regid
        return refuse(why, std::string(vs.k.name ? vs.k.name : "the vertex stage") + " writes its position in a half register: not served");
    }
    // varmask is the FS's varyings only: VPC_VAR_DISABLE is ~varmask and neither driver disables the position's
    // locations through it -- turnip builds the mask from the FS's bary inputs alone (tu_shader.cc:1790-1803), fd6
    // takes linkage.varmask BEFORE the position is appended (fd6_program.cc:654-658); ir3_link_add would set the
    // position's bits too, so they are taken back after it
    uint32_t varyingsOnly[4] = {l.varmask[0], l.varmask[1], l.varmask[2], l.varmask[3]};
    l.positionLoc = l.maxLoc;
    if (!linkAdd(l, kVaryingSlotPos, vs.positionRegid, 0xf, uint8_t(l.maxLoc))) {
        return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and has no room left for the position");
    }
    for (uint32_t i = 0; i < 4; i++) l.varmask[i] = varyingsOnly[i];
    // --- the point size after it, one component, when the VS writes one (tu6_emit_vpc, tu_pipeline.cc:944-947): what a
    // point is rasterized at; a line or a triangle ignores it
    if (vs.pointSizeRegid != a6xx::kRegidNone) {
        l.psizeLoc = l.maxLoc;
        if (!linkAdd(l, kVaryingSlotPsiz, uint8_t(vs.pointSizeRegid), 0x1, uint8_t(l.maxLoc))) {
            return refuse(why, "the link holds " + std::to_string(kMaxStageOutputs) + " entries and has no room left for the point size");
        }
        for (uint32_t i = 0; i < 4; i++) l.varmask[i] = varyingsOnly[i];
    }
    // --- the colour outputs: 0xf per target written (SP_FS_RENDER_COMPONENTS / RB_RENDER_COMPONENTS, tu_shader.cc:1877-1891)
    for (uint32_t i = 0; i < fs.numColorOutputs && i < kMaxColorOutputs; i++) {
        if (fs.colorRegid[i] != a6xx::kRegidNone) l.mrtComponents |= 0xfu << (i * 4);
    }
    // psReplMode is zero but for a [[point_coord]] input's (above); fd6 writes all eight registers so (render.hpp)
    *out = l;
    return true;
}

}  // namespace hakios::metal::render
