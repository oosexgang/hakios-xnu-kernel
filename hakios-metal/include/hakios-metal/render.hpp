// hakios-metal: the render path. ONE draw of a Program (a vertex and a fragment stage) into ONE colour target in
// SYSMEM (direct render, no GMEM tiling, no binning) mode, encoded the way compute.cpp encodes a dispatch: one
// stream, a table the pass owns, every refusal by name before a word is written.
//
// SPDX-License-Identifier: MIT
//
// The structural reference is turnip, Mesa's Vulkan driver for Adreno (src/freedreno/vulkan: tu6_sysmem_render_begin/
// end in tu_cmd_buffer.cc, tu6_emit_program/vpc/fs_inputs/fs_outputs in tu_pipeline.cc and tu_shader.cc, and the r3d
// clear/blit path in tu_clear_blit.cc, which draws a triangle into one colour attachment with the smallest state set
// there is), with fd6 (src/gallium/drivers/freedreno/a6xx) as the second witness. Nothing of either is linked: they
// are read and cited, register by register, and the fake GPU checks that the stream carries what they write.
//
// What one draw is made of, in the order the stream has it (render::Options names each group, so that
// hakios-metal-probe --bisect can switch any one off when the phone hangs):
//   batch start   fd6_emit_restore / tu6_init_hw: CP_SET_MODE 0, the three invalidates, HLSQ_INVALIDATE_CMD, WFI, the
//                 static register table a6xx.hpp documents (kStaticState...), CP_SET_DRAW_STATE disable-all, the 32
//                 VFD_FETCH sizes, the border colour bases, WFI, RB_CCU_CNTL for sysmem
//   pass begin    window scissor and offsets, GRAS/RB_BIN_CONTROL with BUFFERS_IN_SYSMEM, CP_SET_MARKER
//                 RM6_DIRECT_RENDER, CP_SKIP_IB2_ENABLE_GLOBAL 0, CP_SET_VISIBILITY_OVERRIDE 1, CP_SET_MODE 0
//   the target    RB_DEPTH_BUFFER_INFO DEPTH6_NONE and the stencil zeros; RB_MRT[0] (BUF_INFO, PITCH, ARRAY_PITCH, BASE,
//                 BASE_GMEM 0), SP_FS_MRT_REG[0], the flag buffer zeros, GRAS_LRZ_MRT_BUF_INFO_0, dither/sRGB off,
//                 MSAA off, RB_RENDER_CNTL, LRZ off
//   the program   SP_xS_CONFIG / HLSQ_xS_CNTL for the five stages (HS/DS/GS zero), each stage's CTRL_REG0, INSTRLEN,
//                 OBJ_START block, the ST6_SHADER preload (CP_LOAD_STATE6_GEOM for the VS, _FRAG for the FS),
//                 immediates; the VFD registers (no attributes: the VS reads vertex_id); the VPC link (SP_VS_OUT_REG,
//                 SP_VS_VPC_DST_REG, VPC_VS_PACK, PC_VS_OUT_CNTL...); the FS inputs (HLSQ_CONTROL_1..5, GRAS_CNTL,
//                 RB_RENDER_CONTROL0/1, VPC_VAR_DISABLE, VPC_CNTL_0) and outputs (SP_FS_OUTPUT_REG, RENDER_COMPONENTS)
//   raster etc.   GRAS_CL_CNTL, GRAS_SU_CNTL (cull none), polygon modes, the viewport (Metal's rule: NDC y up and
//                 row 0 at the top, so the y scale the GPU gets is NEGATIVE), scissors, guardband, blend off, depth
//                 and stencil off
//   the draw      CP_DRAW_INDX_OFFSET: a triangle list of N vertices, auto index, one instance
//   the tail      CP_WAIT_FOR_IDLE; PC_CCU_FLUSH_COLOR_TS writing a value (no TIMESTAMP bit: the 28 October lesson)
//                 and a wait for it, which is what puts the colour in memory; LRZ_FLUSH; CACHE_FLUSH_TS and its wait.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hakios-metal/compute.hpp"
#include "hakios-metal/kernel.hpp"
#include "hakios-metal/pm4.hpp"

namespace hakios::metal {

class Texture;
struct FormatInfo;

// --- one shader stage as plain data ------------------------------------------------------------------------------------
// Everything a VS or an FS shares with a compute Kernel is Kernel's own field set (words, instrlen, constlen, the
// footprints, branchstack, immediates, uboRanges, numUbos/numSsbos/numIbos/numSamp, textures[]...). A Stage is a Kernel
// plus what the 3D registers read of a vertex or fragment variant; the compute-only fields of its Kernel (localSize,
// localIdRegid, workGroupIdRegid, numBufs, bufs, numwgConstRegid) stay at their zeros / kNoRegid / kNoConstRegid.
enum StageKind : uint8_t { kStageVertex = 0, kStageFragment = 1 };

constexpr uint32_t kMaxStageInputs = 34;      // ir3_shader_variant::inputs[32 + 2]
constexpr uint32_t kMaxStageOutputs = 32;     // ir3's outputs[], and the linkage's var[]
constexpr uint32_t kMaxColorOutputs = 8;      // SP_FS_OUTPUT_REG[8], RB_MRT[8]
constexpr uint32_t kIjCount = 7;              // ir3's IJ_COUNT: persp/linear x pixel/sample/centroid, persp centre rhw

// one entry of ir3_shader_variant::inputs: a VS attribute or system value, an FS varying or system value
struct StageInput {
    uint8_t slot;       // gl_varying_slot / VERT_ATTRIB_GENERIC0 + n / a gl_system_value when sysval
    uint8_t regid;      // kRegidNone (0xfc) when unused
    uint8_t compmask;
    uint8_t inloc;      // FS: the VPC location bary.f reads; ir3_link_shaders keys on it
    uint8_t sysval;     // a system value, not an attribute or varying (the VFD and the link skip these)
    uint8_t bary;       // FS: a varying read by bary.f -- what ir3_next_varying walks (compmask && bary); a sysval has 0
    uint8_t flat;       // FS: INTERP_FLAT in VPC_VARYING_INTERP_MODE
    uint8_t half;
};
// one entry of ir3_shader_variant::outputs
struct StageOutput {
    uint8_t slot;       // VARYING_SLOT_POS / VARYING_SLOT_VAR0 + k for a VS; FRAG_RESULT_DATA0 + n for an FS
    uint8_t regid;
    uint8_t half;       // SP_FS_OUTPUT_REG.HALF_PRECISION for an FS colour
    uint8_t pad;
};

// one of ir3_shader_variant::sampler_prefetch (ir3_shader.h:354-365): a texture sample the FS issues before its first
// instruction, at the varying in register SRC (a coordinate ir3 found interpolated and otherwise unused), into DST
constexpr uint32_t kMaxSamplerPrefetch = 4;   // IR3_MAX_SAMPLER_PREFETCH (ir3_shader.h:342); SP_FS_PREFETCH_CNTL.COUNT holds 3 bits
struct SamplerPrefetch {
    uint8_t src, sampId, texId, dst, wrmask, half;
    uint8_t cmd;              // a6xx_tex_prefetch_cmd: TEX_PREFETCH_SAM (1) for ir3's OPC_SAM, 0 for an opcode the writer did not know
    uint8_t bindless;         // refused when set (the descriptors here are SB6_FS_TEX tables, not bindless sets)
};

struct Stage {
    Kernel k;                 // the shared fields; SP_xS_CTRL_REG0, INSTRLEN, HLSQ_xS_CNTL.CONSTLEN... read them
    uint8_t kind;             // StageKind, from nir->info.stage
    uint8_t pad0[3];

    StageInput inputs[kMaxStageInputs];
    uint32_t numInputs;
    StageOutput outputs[kMaxStageOutputs];
    uint32_t numOutputs;

    // --- a vertex stage ---
    uint32_t numAttributes;   // inputs that are not sysvals: VFD_CONTROL_0.FETCH_CNT/DECODE_CNT, one VFD_DEST_CNTL_INSTR each; refused when != 0 (no vertex descriptor yet)
    uint32_t vertexIdRegid;   // SYSTEM_VALUE_VERTEX_ID -> VFD_CONTROL_1.REGID4VTX; kRegidNone when unread
    uint32_t instanceIdRegid; // SYSTEM_VALUE_INSTANCE_ID -> VFD_CONTROL_1.REGID4INST
    uint32_t positionRegid;   // VARYING_SLOT_POS -> the linkage's position entry, VPC_VS_PACK.POSITIONLOC; refused when kRegidNone
    uint32_t pointSizeRegid;  // VARYING_SLOT_PSIZ; refused when valid
    uint8_t clipMask, cullMask, writesLayer, writesViewport;   // refused when non-zero

    // --- a fragment stage ---
    uint32_t totalIn;         // fs->total_in: VPC_CNTL_0.NUMNONPOSVAR and .VARYING, SP_FS_CTRL_REG0.VARYING, HLSQ_FS_CNTL_0.VARYINGS, RB_RENDER_CONTROL0.UNK10
    uint32_t fragCoordRegid;  // SYSTEM_VALUE_FRAG_COORD -> HLSQ_CONTROL_4_REG.XYCOORDREGID (ZWCOORDREGID = +2); kRegidNone when unread
    uint8_t fragcoordCompmask;   // GRAS_CNTL.COORD_MASK, RB_RENDER_CONTROL0.COORD_MASK
    uint8_t pad1[3];
    uint32_t ijRegid[kIjCount];  // SYSTEM_VALUE_BARYCENTRIC_* -> HLSQ_CONTROL_3/4_REG, GRAS_CNTL, RB_RENDER_CONTROL0; sample/centroid ones refused
    uint32_t faceRegid;       // SYSTEM_VALUE_FRONT_FACE -> HLSQ_CONTROL_2_REG.FACEREGID, RB_RENDER_CONTROL1.FACENESS
    uint32_t sampleIdRegid, sampleMaskInRegid;   // refused when valid (no MSAA)
    uint32_t colorRegid[kMaxColorOutputs];       // FRAG_RESULT_DATA0 + i -> SP_FS_OUTPUT_REG[i].REGID (| HALF_PRECISION when colorHalf)
    uint8_t colorHalf[kMaxColorOutputs];
    uint32_t numColorOutputs; // highest written + 1; step one: exactly 1
    uint32_t depthRegid, sampleMaskRegid, stencilRefRegid;   // SP_FS_OUTPUT_CNTL0; refused when valid
    uint8_t writesPos, writesSmask, writesStencilref, dualSrcBlend, hasKill, perSamp, readsPrimid, fbRead;   // the last six refused when set (hasKill allowed: early-z is off)
    uint8_t needPixlod, needFullQuad;   // SP_FS_CTRL_REG0.PIXLODENABLE / LODPIXMASK (textures in the FS: step two)
    uint8_t pad2[2];
    uint32_t numSamplerPrefetch;        // SP_FS_PREFETCH_CNTL.COUNT, one SP_FS_PREFETCH_CMD each
    SamplerPrefetch prefetch[kMaxSamplerPrefetch];
    uint8_t prefetchEndOfQuad;          // SP_FS_PREFETCH_CNTL.ENDOFQUAD: the prefetch is the last use of helper invocations
    uint8_t pad3[3];
};

// --- the link of a VS with an FS, as ir3_link_shaders computes it (ir3_shader.h) -----------------------------------------
// Plain data so a fixture can carry it and a build without Mesa can compute it: render::link is a port of
// ir3_link_add / ir3_link_shaders over Stage::inputs/outputs; with MESA=1 compiler_test holds it to Mesa's own.
struct Linkage {
    struct Var { uint8_t slot, regid, compmask, loc; } var[kMaxStageOutputs];   // SP_VS_OUT_REG[i/2] halves, SP_VS_VPC_DST_REG[i/4] bytes
    uint32_t cnt;             // SP_VS_PRIMITIVE_CNTL.OUT; at least 1 (position)
    uint32_t maxLoc;          // VPC_VS_PACK.STRIDE_IN_VPC, PC_VS_OUT_CNTL.STRIDE_IN_VPC
    uint32_t varmask[4];      // VPC_VAR_DISABLE[i] = ~varmask[i]
    uint32_t positionLoc;     // VPC_VS_PACK.POSITIONLOC
    uint32_t interpMode[8];   // VPC_VARYING_INTERP_MODE[i]: 2 bits a component at inloc*2, INTERP_FLAT where the FS input is flat; else 0 (smooth)
    uint32_t psReplMode[8];   // VPC_VARYING_PS_REPL_MODE[i]: zeros (no point sprites); written as fd6 writes them, all eight
    uint32_t mrtComponents;   // SP_FS_RENDER_COMPONENTS / RB_RENDER_COMPONENTS: 0xf per colour output written
    uint32_t psizeLoc = 0xff; // VPC_VS_PACK.PSIZELOC: the VS's [[point_size]], linked after the position (0xff: none written)
};

// --- the vertex descriptor (MTLVertexDescriptor), in Metal's own numbers -------------------------------------------------
// A VS [[stage_in]] attribute is a VS input at VERT_ATTRIB_GENERIC0 + n for [[attribute(n)]]; what it is in memory is not
// the shader's but the pipeline's: format, byte offset and the [[buffer(b)]] it is fetched from, and that buffer's stride
// and step. The hardware's two tables follow turnip's (tu6_emit_vertex_input, tu_pipeline.cc:2330-2360; tu6_emit_vfd_dest,
// tu_shader.cc:1573-1612): decode entry n = attribute n (VFD_DECODE_INSTR / VFD_DEST_CNTL_INSTR, IDX naming the fetch),
// fetch entry b = Metal buffer index b (VFD_FETCH BASE / SIZE / STRIDE), as turnip uses the Vulkan binding number.
enum class VertexFormat : uint32_t {   // MTLVertexFormat (MTLVertexDescriptor.h); the served ones
    kInvalid = 0,
    kUChar2 = 1,
    kUChar3 = 2,
    kUChar4 = 3,
    kChar2 = 4,
    kChar3 = 5,
    kChar4 = 6,
    kUChar2Normalized = 7,
    kUChar3Normalized = 8,
    kUChar4Normalized = 9,
    kChar2Normalized = 10,
    kChar3Normalized = 11,
    kChar4Normalized = 12,
    kUShort2 = 13,
    kUShort3 = 14,
    kUShort4 = 15,
    kShort2 = 16,
    kShort3 = 17,
    kShort4 = 18,
    kUShort2Normalized = 19,
    kUShort3Normalized = 20,
    kUShort4Normalized = 21,
    kShort2Normalized = 22,
    kShort3Normalized = 23,
    kShort4Normalized = 24,
    kHalf2 = 25,
    kHalf3 = 26,
    kHalf4 = 27,
    kFloat = 28,
    kFloat2 = 29,
    kFloat3 = 30,
    kFloat4 = 31,
    kInt = 32,
    kInt2 = 33,
    kInt3 = 34,
    kInt4 = 35,
    kUInt = 36,
    kUInt2 = 37,
    kUInt3 = 38,
    kUInt4 = 39,
    kUChar4Normalized_BGRA = 42,
    kUChar = 45,
    kChar = 46,
    kUCharNormalized = 47,
    kCharNormalized = 48,
    kUShort = 49,
    kShort = 50,
    kUShortNormalized = 51,
    kShortNormalized = 52,
    kHalf = 53,
};
enum class VertexStep : uint32_t { kConstant = 0, kPerVertex = 1, kPerInstance = 2 };   // MTLVertexStepFunction; per vertex is served

constexpr uint32_t kMaxVertexAttributes = 31;   // MTLVertexDescriptor's attributes[0..30]
constexpr uint32_t kMaxVertexBuffers = 31;      // its layouts[0..30], one per [[buffer(n)]] index

struct VertexAttribute {
    VertexFormat format = VertexFormat::kInvalid;   // kInvalid: not described
    uint32_t offset = 0;                            // bytes into each vertex of the buffer: VFD_DECODE_INSTR.OFFSET (12 bits)
    uint32_t bufferIndex = 0;                       // the [[buffer(n)]] it is fetched from: VFD_DECODE_INSTR.IDX
};
struct VertexLayout {
    uint32_t stride = 0;                            // VFD_FETCH_STRIDE (12 bits); 0 refused (Metal's constant step only)
    VertexStep step = VertexStep::kPerVertex;
    uint32_t stepRate = 1;
};
struct VertexDescriptor {
    VertexAttribute attributes[kMaxVertexAttributes];
    VertexLayout layouts[kMaxVertexBuffers];
};

// MTLRenderPipelineColorAttachmentDescriptor's blend state for attachment 0, Metal's numbers: MTLBlendOperation (Add 0,
// Subtract 1, ReverseSubtract 2, Min 3, Max 4) and MTLBlendFactor (Zero 0, One 1, SourceColor 2, OneMinusSourceColor 3,
// SourceAlpha 4, OneMinusSourceAlpha 5, DestinationColor 6, OneMinusDestinationColor 7, DestinationAlpha 8,
// OneMinusDestinationAlpha 9, SourceAlphaSaturated 10, BlendColor 11, OneMinusBlendColor 12, BlendAlpha 13,
// OneMinusBlendAlpha 14, Source1Color 15 .. OneMinusSource1Alpha 18), the write mask MTLColorWriteMask's bits (Red 8,
// Green 4, Blue 2, Alpha 1; Metal's default All 0xf). Off by default, as Metal's is.
struct ColorBlend {
    bool enabled = false;
    uint32_t rgbOp = 0, alphaOp = 0;
    uint32_t srcRgb = 1, dstRgb = 0, srcAlpha = 1, dstAlpha = 0;   // One, Zero: Metal's defaults
    uint32_t writeMask = 0xf;
};

struct Program {
    Stage vs;
    Stage fs;
    Linkage link;
    const char* name;
    VertexDescriptor vertexDescriptor{};   // what the VS's [[stage_in]] attributes are in memory (none: a VS without attributes)
    ColorBlend blend{};                    // colour attachment 0's blending and write mask
    ColorBlend moreBlend[kMaxColorOutputs - 1]{};   // ...and attachments 1 to 7's, for an FS that writes [[color(n)]] there
    // The 3D pipeline has one IBO table for both stages, so the FS's device buffers sit past the VS's: its [[buffer(n)]]
    // is table slot FS_IBO_BASE + n, which is how it was compiled (compileStage's iboBase: the VS's count)
    uint32_t fsIboBase = 0;
    // MTLRenderPipelineDescriptor.rasterSampleCount: 1, or the 2 or 4 samples of the multisample attachments it draws into
    uint32_t samples = 1;
    // whether each stage stores to or does atomics on its device buffers (bit 0 the VS, bit 1 the FS), as the compiler
    // found in the NIR: a stage that only reads them may be replayed tile by tile (gmemDrawOk). A program not made by
    // linkProgram says all: unknown is taken as writing.
    uint8_t ssboWrites = 3;
};

namespace render {

// VS to FS as ir3_link_shaders does it: every FS input with compmask && bary and inloc < totalIn takes the VS output of
// the same slot at loc = inloc (ir3_link_add), then the position is appended at maxLoc with compmask 0xf. False with
// *why: an FS varying no VS output serves (ir3 would read zeros through regid 63.0; refused here by slot), a VS without
// a position, more than kMaxStageOutputs entries. Implemented in src/link.cpp.
bool link(const Stage& vs, const Stage& fs, Linkage* out, std::string* why);

}  // namespace render

// --- the pass and the draw --------------------------------------------------------------------------------------------
enum class LoadAction : uint32_t { kDontCare = 0, kLoad = 1, kClear = 2 };     // MTLLoadAction's numbers
enum class StoreAction : uint32_t { kDontCare = 0, kStore = 1 };               // MTLStoreAction's numbers
enum class Primitive : uint32_t { kPoint = 0, kLine = 1, kLineStrip = 2, kTriangle = 3, kTriangleStrip = 4 };   // MTLPrimitiveType

struct Viewport { double x, y, width, height, znear, zfar; };   // MTLViewport, Metal's top-left origin
struct Scissor { uint32_t x, y, width, height; };               // MTLScissorRect

// a colour attachment past 0 (MTLRenderPassColorAttachmentDescriptor at index 1 to 7), with attachment 0's rules
struct ColorAttachment {
    const Texture* texture = nullptr;            // none: the attachment is not set
    uint32_t level = 0;                          // the mip level drawn into (MTLRenderPassAttachmentDescriptor.level)
    LoadAction load = LoadAction::kLoad;
    StoreAction store = StoreAction::kStore;
    double clearColor[4] = {0, 0, 0, 0};
    uint32_t slice = 0;                          // a 2D array's layer, a cube's face or a 3D texture's depth plane drawn into
};

struct RenderPass {
    const Texture* color0 = nullptr;             // made with kTextureUsageRenderTarget
    uint32_t level = 0;                          // its mip level drawn into: the window is that level's size
    uint32_t slice = 0;                          // its layer, face or depth plane drawn into (as ColorAttachment::slice)
    LoadAction load = LoadAction::kLoad;         // kClear: the 2D engine fills the whole target with clearColor before the draw (turnip's sysmem clear)
    StoreAction store = StoreAction::kStore;
    double clearColor[4] = {0, 0, 0, 0};         // MTLClearColor, RGBA whatever the format's memory order; whole numbers for an integer target
    // Colour attachments 1 to 7, each the size of attachment 0. One the FS writes ([[color(n)]]) is RB_MRT[n]; one it
    // does not write is only cleared when it asks for a Clear, and otherwise left as it is -- what Metal leaves for an
    // output the function does not write.
    ColorAttachment more[kMaxColorOutputs - 1];
    // the depth attachment (MTLRenderPassDepthAttachmentDescriptor): a kDepth32Float texture or none; Metal's defaults
    // are DontCare / DontCare / 1.0. In sysmem Load and DontCare are the same, and the depth is always flushed to memory
    // at the pass's end (each draw is a pass of its own here, so the next one must find it there)
    const Texture* depth = nullptr;
    uint32_t depthLevel = 0;                     // its mip level drawn into (each level its own tiled layout, texture.cpp: depthLevel)
    uint32_t depthSlice = 0;                     // a depth 2D array's layer drawn into: a shadow map's cascade
    LoadAction depthLoad = LoadAction::kDontCare;
    StoreAction depthStore = StoreAction::kDontCare;
    double clearDepth = 1.0;
    // the stencil attachment (MTLRenderPassStencilAttachmentDescriptor): the depth texture itself when it is a
    // Depth32Float_Stencil8 one (its S8 plane), or none; Metal's defaults DontCare / DontCare / 0
    const Texture* stencil = nullptr;
    LoadAction stencilLoad = LoadAction::kDontCare;
    StoreAction stencilStore = StoreAction::kDontCare;
    uint32_t clearStencil = 0;
};

// MTLDepthStencilDescriptor's depth half, the encoder's state each draw reads: COMPARE an MTLCompareFunction (Never 0,
// Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6, Always 7 -- adreno_compare_func's numbering),
// WRITE depthWriteEnabled. Metal's defaults (Always, NO) are a depth test that does nothing.
struct DepthState {
    uint32_t compare = 7;
    bool write = false;
};

// MTLDepthStencilDescriptor's stencil half, the encoder's state each draw reads, and -setStencilReferenceValue:. A
// face's COMPARE is an MTLCompareFunction and its three operations MTLStencilOperations (Keep 0 .. DecrementWrap 7),
// which are adreno_compare_func's and adreno_stencil_op's numbers; ENABLED false is Metal's default (no
// stencilDescriptor: no test, nothing written).
struct StencilFace {
    uint32_t compare = 7, stencilFail = 0, depthFail = 0, pass = 0;
    uint32_t readMask = 0xff, writeMask = 0xff;
};
struct StencilState {
    bool enabled = false;
    StencilFace front, back;
    uint32_t frontReference = 0, backReference = 0;
};

struct Draw {
    Primitive primitive = Primitive::kTriangle;  // step one: kTriangle and kTriangleStrip (DI_PT_TRILIST / DI_PT_TRISTRIP)
    uint32_t cullMode = 0;                       // MTLCullMode: None 0, Front 1, Back 2
    uint32_t winding = 0;                        // MTLWinding, the front face's: Clockwise 0, CounterClockwise 1 (in Metal's y-up NDC)
    uint32_t vertexStart = 0;                    // VFD_INDEX_OFFSET: added to vertex_id by VFD_ADD_OFFSET.VERTEX (the static table)
    uint32_t vertexCount = 0;                    // CP_DRAW_INDX_OFFSET's NUM_INDICES (the index count of an indexed draw); 0 refused
    uint32_t instanceCount = 1;                  // NUM_INSTANCES, 1 or more
    // an indexed draw (-drawIndexedPrimitives:...): INDEX_BUFFER's indices from INDEX_OFFSET, INDEX_TYPE an MTLIndexType
    // (UInt16 0, UInt32 1), BASE_VERTEX added to each (VFD_INDEX_OFFSET; a negative one is refused). Primitive restart is
    // on, as Metal has it, at the type's all-ones index. VERTEX_START is not read then.
    const Buffer* indexBuffer = nullptr;
    uint64_t indexOffset = 0;
    uint32_t indexType = 0;
    int32_t baseVertex = 0;
    uint32_t baseInstance = 0;                   // VFD_INSTANCE_START_OFFSET, and the base [[instance_id]] counts from
    // an indirect draw (-drawPrimitives:indirectBuffer:..., -drawIndexedPrimitives:...indirectBuffer:...): the counts,
    // starts and bases are INDIRECT's words at INDIRECT_OFFSET when the GPU runs the draw -- MTLDrawPrimitivesIndirect
    // Arguments (4 words) or the indexed form (5 words), Vulkan's two draw commands word for word -- and the fields above
    // that they replace are not read; the index buffer, its type and offset still are
    const Buffer* indirect = nullptr;
    uint64_t indirectOffset = 0;
    Viewport viewport{};
    Scissor scissor{};                           // width 0: the whole target
    float blendColor[4] = {0, 0, 0, 0};          // -setBlendColorRed:green:blue:alpha: (RB_BLEND_*_F32), read by the BlendColor/BlendAlpha factors
    DepthState depth{};                          // -setDepthStencilState: (refused with a depth test and no depth attachment)
    StencilState stencil{};                      // ...its stencil half (refused enabled with no stencil attachment)
    // -setVisibilityResultMode:offset: over the pass's visibilityResultBuffer: VISIBILITY_MODE an MTLVisibilityResultMode
    // (Disabled 0, Boolean 1, Counting 2). Either kept mode adds the samples the draw lets through (depth and stencil
    // tests passed) to the 64-bit word at VISIBILITY_OFFSET, which a Boolean reader takes as "any"; VISIBILITY_RESET
    // zeroes the word first (the encoder's first draw into it, so that a pass counts from 0 as Metal's does).
    const Buffer* visibility = nullptr;
    uint32_t visibilityMode = 0;
    uint64_t visibilityOffset = 0;
    bool visibilityReset = false;
};

namespace render {

// IEEE half bits of F, round to nearest even (the packing a Clear of a half format and the probe's expectations use)
uint32_t halfBitsOf(float f);

// What a pass owns in GPU memory (one 16 KiB page on the phone): the tail's timestamp words, a zero border colour
// entry (the TP's base registers point here, as in compute), a hole, room for step two's per-stage descriptor tables.
constexpr uint32_t kTableBytes = 8192;
constexpr uint32_t kTableTimestampsAt = 0;        // the CCU flush's and the cache flush's values: 4 words
constexpr uint32_t kTableClearFlushAt = 12;       // ...the fourth: the CCU flush after a Clear's fill, which writes kClearFlushValue
constexpr uint32_t kClearFlushValue = 4;
constexpr uint32_t kTableDepthFlushAt = 16;       // the depth CCU flush the tail writes when a depth attachment is bound, kDepthFlushValue
constexpr uint32_t kDepthFlushValue = 5;
constexpr uint32_t kTableDepthClearFlushAt = 20;  // the depth CCU flush after a depth Clear's fill, kDepthClearFlushValue
constexpr uint32_t kDepthClearFlushValue = 6;
constexpr uint32_t kTableDepthClearDepthFlushAt = 24;   // ...and the depth CCU's flush after it, kDepthClearDepthFlushValue
constexpr uint32_t kDepthClearDepthFlushValue = 7;
// Colour attachments 1 to 7's Clears, a word each (28 to 52), each waited for at kClearFlushValue: a word of its own
// per attachment, because a second Clear waiting on the first one's word would find the value already there and run on
// before its own fill had left the CCU.
constexpr uint32_t kTableMoreClearFlushAt = 28;
// A draw whose FS reads its target (framebuffer fetch): the CCU flush before it, which puts what the draws before it
// wrote where the texture unit reads; a word of the DRAW's own table (its zeroed part), so that every such draw in a
// pass waits for its own flush and not for an earlier one's word
constexpr uint32_t kTableFbReadFlushAt = 56;
constexpr uint32_t kFbReadFlushValue = 8;
// A stencil Clear's two flushes (the colour CCU, through which the 2D engine wrote, then the depth CCU, which holds
// stencil as it holds depth), each waited for on its own word
constexpr uint32_t kTableStencilClearFlushAt = 64;
constexpr uint32_t kStencilClearFlushValue = 9;
constexpr uint32_t kTableStencilClearDepthFlushAt = 68;
constexpr uint32_t kStencilClearDepthFlushValue = 10;
// a GMEM pass's stores: the blit cache cleaned (PC_CCU_RESOLVE_TS, turnip's FD_CCU_CLEAN_BLIT_CACHE after the last
// tile) and waited for before the tail, so what the tiles stored is in memory before anything reads it
constexpr uint32_t kTableBlitFlushAt = 72;
constexpr uint32_t kBlitFlushValue = 11;
// the binning pass's cache flush, before the tiles read the visibility streams (fd6's FD6_FLUSH_CACHE after it)
constexpr uint32_t kTableBinFlushAt = 76;
constexpr uint32_t kBinFlushValue = 12;
constexpr uint32_t kTableZeroedBytes = 96;        // what tableStart zeroes from kTableTimestampsAt: every word above
static_assert(kTableMoreClearFlushAt + 4 * (kMaxColorOutputs - 1) <= kTableZeroedBytes, "the Clears' words are zeroed");
// an occlusion query's two snapshots of the running sample count (RB_SAMPLE_COUNT_ADDR), begin and end, 64 bits each
// and 16 bytes apart as turnip's occlusion_query_slot has them (a padding word after each)
constexpr uint32_t kTableVisibilityAt = 96;
constexpr uint32_t kVisibilityEndAt = 16;
static_assert(kTableVisibilityAt >= kTableZeroedBytes && kTableVisibilityAt + 32 <= 128, "the snapshots lie between the zeroed words and the border colour");
constexpr uint32_t kTableBorderColorAt = 128;     // Metal's three border colours (texture.hpp: writeBorderColors), 128-aligned
constexpr uint32_t kTableHoleAt = 512;            // 64 bytes nobody addresses
constexpr uint32_t kTableVsAt = 1024;             // step two: the VS's descriptor tables (UBO, IBO, TEX, SAMP)
constexpr uint32_t kTableFsAt = 4096;             // step two: the FS's
static_assert(kTableBorderColorAt % 128 == 0 && kTableBorderColorAt + 3 * 128 <= kTableHoleAt && kTableHoleAt + 64 <= kTableVsAt &&
              kTableFsAt + 4096 <= kTableBytes, "the table's parts fit");

// Each group switchable, as compute::Options are: the instrument that found the TIMESTAMP bit. The defaults are the
// whole stream as turnip's sysmem pass and r3d path have it; the *Alt fields pick the other witness's value where the
// two disagree, so one boot of --bisect can tell which the hardware wants.
struct Options {
    bool batchStart = true;      // CP_SET_MODE 0, PC_CCU_INVALIDATE_COLOR/DEPTH + CACHE_INVALIDATE (fd6's three), HLSQ_INVALIDATE_CMD all,
                                 // WFI, kStaticState, CP_SET_DRAW_STATE disable-all, kStaticStateAfterDrawState, the 32 VFD_FETCH sizes,
                                 // the border colour bases, kStaticStateTail, WFI (fd6_emit_restore / tu6_init_hw)
    bool ccuCntl = true;         // WFI then RB_CCU_CNTL = sysmem (both witnesses: 0x10000000)
    bool ccuConcurrentResolve = false;   // ...with bit 2 set (0x10000004): what hakios's kernel wrote at boot; unverified either way
    bool passBegin = true;       // window scissor + GRAS_2D_RESOLVE_CNTL, the four window offsets, GRAS/RB_BIN_CONTROL sysmem, RB_BIN_CONTROL2,
                                 // CP_SKIP_IB2_ENABLE_GLOBAL 0, CP_SET_VISIBILITY_OVERRIDE 1, CP_SET_MODE 0
    bool binControlLrzFeedback = false;  // GRAS/RB_BIN_CONTROL with turnip's LRZ_FEEDBACK_ZMODE_MASK 3 (0x03c00000) instead of fd6's 0x00c00000
    bool marker = true;          // CP_SET_MARKER RM6_DIRECT_RENDER, inside passBegin but its own switch
    bool zsNone = true;          // RB_DEPTH_BUFFER_INFO DEPTH6_NONE..., GRAS_SU_DEPTH_BUFFER_INFO, RB_STENCIL_INFO
    bool mrt = true;             // RB_MRT[0] six registers, SP_FS_MRT_REG[0], RB_MRT_FLAG_BUFFER[0] zeros, GRAS_LRZ_MRT_BUF_INFO_0, dither, sRGB, GRAS_MAX_LAYER_INDEX
    bool msaaOff = true;         // SP_TP/GRAS/RB RAS_MSAA_CNTL 0 and DEST_MSAA_CNTL MSAA_DISABLE, RB_BLIT_GMEM_MSAA_CNTL 0
    bool renderCntl = true;      // RB_RENDER_CNTL = CCUSINGLECACHELINESIZE(2) (r3d writes 0: renderCntlZero)
    bool renderCntlZero = false;
    bool lrzOff = true;          // GRAS_LRZ_CNTL 0, RB_LRZ_CNTL 0, GRAS_LRZ_BUFFER_BASE/PITCH/FAST_CLEAR_BUFFER_BASE 0
    bool programConfig = true;   // HLSQ_INVALIDATE_CMD (vs..fs, gfx_ibo), SP_xS_CONFIG and HLSQ_xS_CNTL for the five stages, SP_IBO_COUNT 0
    bool programVs = true;       // SP_VS_CTRL_REG0, INSTRLEN, the OBJ_START block, HW_STACK_OFFSET, the preload, the immediates
    bool programFs = true;       // the same for the FS
    bool fsTextures = true;      // the FS's SB6_FS_TEX loads, SP_FS_TEX_SAMP/CONST, SP_FS_TEX_COUNT (0 without textures)
    bool vsTextures = true;      // the VS's SB6_VS_TEX loads, SP_VS_TEX_SAMP/CONST, SP_VS_TEX_COUNT (0 without textures)
    bool vfd = true;             // PC/VFD_MULTIVIEW_CNTL 0, VFD_CONTROL_0..6, VFD_INDEX_OFFSET, VFD_INSTANCE_START_OFFSET, PC_RESTART_INDEX
    bool vpc = true;             // the Linkage registers, VPC_VS_CLIP_CNTL(_V2), GRAS_VS_CL_CNTL, VPC_VS_LAYER_CNTL(_V2), GRAS_VS_LAYER_CNTL, PC_PS_CNTL
    bool fsInputs = true;        // SP_FS_PREFETCH_CNTL, HLSQ_CONTROL_1..5_REG, HLSQ_FS_CNTL_0, GRAS_CNTL, RB_RENDER_CONTROL0/1, RB_SAMPLE_CNTL,
                                 // GRAS_LRZ_PS_INPUT_CNTL, GRAS_SAMPLE_CNTL, VPC_VAR_DISABLE[0..3], VPC_CNTL_0
    bool fsOutputs = true;       // SP_FS_OUTPUT_CNTL0/1, SP_FS_OUTPUT_REG[0], SP/RB_RENDER_COMPONENTS, RB_FS_OUTPUT_CNTL0/1
    bool rast = true;            // GRAS_CL_CNTL, GRAS_SU_CNTL, GRAS_SU_POINT_MINMAX/SIZE, poly offsets, PC_PRIMITIVE_CNTL_0, polygon modes,
                                 // PC_RASTER_CNTL, VPC_UNKNOWN_9107, RB_UNKNOWN_8A00.., GRAS_SC_CNTL
    bool viewportScissor = true; // GRAS_CL_VPORT[0] (y scale negative), GRAS_SC_VIEWPORT_SCISSOR[0], GRAS_CL_Z_CLAMP[0], guardband, RB_Z_CLAMP, GRAS_SC_SCREEN_SCISSOR[0]
    bool blendZsa = true;        // SP/RB_BLEND_CNTL, RB_MRT_CONTROL[0] 0x780, RB_MRT_BLEND_CONTROL[0] 0, blend colour, depth/stencil/alpha zeros, RB_Z_BOUNDS
    bool visCullUse = false;     // CP_DRAW_INDX_OFFSET with USE_VISIBILITY (turnip's and fd6's pipelines) instead of r3d's IGNORE_VISIBILITY
    bool tailCcuFlush = true;    // CP_WAIT_FOR_IDLE, PC_CCU_FLUSH_COLOR_TS writing a value + CP_WAIT_REG_MEM for it
    bool fbReadBarrier = true;   // before a draw whose FS reads its target: the CCU flush waited for, CACHE_INVALIDATE (off: the probe's control)
    bool fbReadOrdered = true;   // ...in GMEM the rasterizer's own ordering (GRAS_SC_CNTL.SINGLE_PRIM_MODE) instead, the barrier
                                 // kept for the first fetching draw after one that is not (fd6_draw.cc's rule); off: the
                                 // barrier before every fetching draw there too
    bool tailDepthFlush = true;  // with a depth attachment: PC_CCU_FLUSH_DEPTH_TS writing a value + the wait for it
    bool tailLrzFlush = true;    // LRZ_FLUSH, 1 word
    bool tailCacheFlush = true;  // CACHE_FLUSH_TS + CP_WAIT_MEM_GTE, as compute's tail
    bool tailRbDone = false;     // RB_DONE_TS + wait: in neither witness's render end; here for the bisect
    bool tailTimestampBit = false;   // must stay clear (28 October); here for the fake's test only
    bool dedupRegs = true;       // in an open pass, a register write that would put the value the register already holds is
                                 // left out (CommandBuffer::draw); off: every draw writes all of its state, as before
    bool gmem = true;            // a pass that qualifies (gmemPlan) is drawn tile by tile in GMEM: each tile loaded, every draw of the
                                 // pass replayed into it, stored once; off: every pass in sysmem, each draw's blending through memory
    bool gmemBinning = false;    // a GMEM pass's draws binned first by the hardware (the VSC), each tile drawing only those that reach
                                 // it, rather than every draw in every tile. NOT WORKING: the binning pass runs and the VSC
                                 // writes its streams, but handing a tile its streams (CP_SET_BIN_DATA5 by address or by
                                 // offset) hangs the command processor; worth at most the 0.31 us a draw a tile replays cost
    uint8_t gmemBinningStage = 0;   // (the bisect of that on the phone: 1 the binning pass without its draws, 2 with them but the tiles
                                    // ignoring the streams -- both run; 3 the streams given but the draws ignoring them, 4 all
                                    // but CP_SKIP_IB2_ENABLE_GLOBAL, 5 the streams given under override 1, 0 all of it -- these hang)
    bool gmemLoad = true;        // each tile's load from memory (off: the fake's twin, whose stores then show GMEM's own contents)
    uint8_t rotate2d = 0;        // encodeBlit's RB_2D_BLIT_CNTL.ROTATE (a6xx_rotation: 0, 90, 180, 270, hflip, vflip as 0..5);
                                 // a quarter turn takes a destination rectangle of the source's sides swapped
};
// A draw joins the open pass only with options equal to the pass's, compared as bytes (CommandBuffer::draw): with a
// member wider than a byte the struct had padding, which a stack-made Options leaves as garbage, and draws that should
// have joined each opened a pass of their own -- some runs, not others. Bytes only, so there is no padding.
static_assert(alignof(Options) == 1, "render::Options holds bools and bytes only: no padding for the join's memcmp to read");

// Tiled rendering (GMEM). Sysmem rendering sends every blended pixel of every draw through the colour CCU to memory and
// back: CoreAnimation's full-screen layers then cost a read and a write of the whole target each (8 blended layers at
// 1125 x 2436 took the GPU 7.6 ms so, 3.1 ms in GMEM: probe "GMEM cost"). In GMEM the target is loaded tile by tile into the on-chip memory, all of the pass's draws run
// against the tile there, and the tile is stored once. The tiles: as few columns as the bin width allows
// (kTileMaxW), as tall as the GMEM below the CCU's colour cache holds (kGmemCcuColorOffset), balanced so the last
// row and column are not slivers.
struct GmemPlan {
    // Where in GMEM each colour attachment's bin lies (RB_MRT's BASE_GMEM) and how much room it has there: GMEM below
    // the CCU's colour cache shared out by bytes a pixel. Fixed when the pass opens -- the draws are encoded with it --
    // while the bins' size waits for the render area at its end (gmemTiles).
    uint32_t base[kMaxColorOutputs] = {}, room[kMaxColorOutputs] = {}, bpp[kMaxColorOutputs] = {};
    // the render area, X0 Y0 X1 Y1 inclusive (what the draws can touch), the grid's corner (aligned to the tile
    // alignment) and the bins over it
    uint32_t area[4] = {}, x0 = 0, y0 = 0;
    uint32_t binW = 0, binH = 0, cols = 0, rows = 0;
    uint32_t tiles() const { return cols * rows; }
};
// whether PASS can be drawn in GMEM and how: its colour attachments the window's size, linear, single-sampled, 4 or 8
// bytes a pixel, their rows with room for the event blit's 16 x 4 granules past the edges; no depth or stencil. A bin
// holds every attachment's pixels (CoreAnimation's main pass draws into two screens at once). False with *why
// otherwise (the pass then runs in sysmem, as before).
bool gmemPlan(const RenderPass& pass, GmemPlan* plan, std::string* why);
// PLAN's bins over the render area AREA (X0 Y0 X1 Y1 inclusive, inside the window): turnip's tiling of a render area
// (tu_tiling_config_update_tile_layout) -- the grid's corner at the area's aligned down, the bins as wide as the plan's
// (gmemPlan picks the width; a fetch's descriptor holds it), as tall as every attachment's room holds. A pass whose draws touch a corner of the screen is drawn in the
// tiles over that corner, and loads and stores no more. False when no bin fits (the plan unchanged).
bool gmemTiles(GmemPlan* plan, const uint32_t area[4]);
// the pixels a draw can touch, X0 Y0 X1 Y1 inclusive: its viewport clamped to the window, within its scissor (both as
// encodeInPass writes them); false for a draw it would refuse for them
bool drawArea(const RenderPass& pass, const Draw& draw, uint32_t area[4]);
// what CommandQueue::preferGmem knows a pass by: its window and its attachments' formats, hashed, never 0
uint64_t tuneKeyOf(const RenderPass& pass);
// the colour attachments a draw of PROGRAM writes into, as a mask: a slot the stage writes, with an attachment, and a
// write mask that lets something through
uint32_t drawnAttachments(const Program& program, const RenderPass& pass);
// whether a draw may be replayed per tile: the replay runs its vertex stage once a tile and reads the targets' memory
// as it was before the pass, so no stage that writes buffers (Program::ssboWrites), no visibility result, no texture over an attachment's
// memory (a framebuffer fetch reads the attachment's bin in GMEM instead: encodeInPass)
bool gmemDrawOk(const Program& program, const RenderPass& pass, const Draw& draw, const Bindings& vsBindings, const Bindings& fsBindings, std::string* why);
// after encodeBegin's words, in place of the draws: the CCU moved for tiles, then per tile the bin selected, the
// attachments in LOAD (a mask) loaded, the BODY_WORDS at GPU address BODIES called (the pass's draws, the first complete
// from an empty register state: CommandBuffer::draw), those in STORE stored; then the blit cache cleaned and the CCU
// back to sysmem. encodeEnd's tail follows as for any pass. What to load and store is CommandBuffer::closePass's;
// FETCH: a draw of the pass reads an attachment (framebuffer fetch), so the texture caches are dropped at each tile.
// CLEAR: the attachments cleared in GMEM instead of loaded (gmemClearWords' packing in CLEAR_WORDS), which
// encodeBegin was told to leave unfilled
// VSC: with Options::gmemBinning, the visibility streams' memory (vscBytes'), the draws having been encoded USE_VISIBILITY
void encodeGmem(pm4::Stream& stream, const RenderPass& pass, const GmemPlan& plan, uint64_t bodies, uint32_t bodyWords, uint64_t table, const Options& o,
                uint32_t load, uint32_t store, bool fetch, uint32_t clear = 0, const uint32_t (*clearWords)[4] = nullptr, uint64_t vsc = 0);
// the visibility streams' memory a binned GMEM pass needs: each pipe's draw stream and its size word, each pipe's
// primitive stream
constexpr uint32_t kVscDrawPitch = 0x4000, kVscPrimPitch = 0x20000;
// (32: a6xx::kNumVscPipes; the size words in a page of their own, so that the primitive streams start page-aligned as
// fd6's, which are a buffer of their own)
constexpr uint64_t kVscSizesBytes = 0x1000;
constexpr uint64_t vscBytes() { return 32ull * (kVscDrawPitch + kVscPrimPitch) + kVscSizesBytes; }
// A Clear done in a tile by the event blit rather than by the 2D engine into memory and loaded back: the colour packed
// as turnip packs it for RB_BLIT_CLEAR_COLOR (pack_blit_event_clear_value: the format's base, RGBA order, sRGB encoded)
// -- 8-bit four-channel unorm and sRGB, and RGBA16Float; false for a format left to the fill
bool gmemClearWords(const FormatInfo& f, const double rgba[4], uint32_t out[4]);

// Everything into STREAM, nothing on refusal. VS_PROGRAM / FS_PROGRAM are the GPU addresses of the two stages' words,
// 128-byte aligned (turnip asserts it; the ST6_SHADER preload unit is 128 bytes); TABLE / TABLE_CPU the kTableBytes
// the pass owns. VS_BINDINGS / FS_BINDINGS: Metal's per-stage [[buffer(n)]] (step one accepts none: a stage with
// numUboRanges, numSsbos, numUbos or numTextureSlots > 0 is refused by name).
// Refused by name, among others: a target without kTextureUsageRenderTarget, a target format whose element class the
// FS colour output does not match, numColorOutputs != 1, depth/sample-mask/stencil outputs, writesPos, perSamp,
// fbRead, dualSrcBlend, readsPrimid, numSamplerPrefetch != 0, a VS attribute the vertex descriptor does not describe or
// describes in a format, step or layout not served or whose fetch runs past its bound buffer, instanceCount != 1, a primitive
// other than triangles and strips, vertexCount 0, a viewport or scissor outside the target, a Clear colour with a
// non-finite channel (or a fractional or out-of-range one into an integer target), store other than kStore, instrlen > 127, pvtmem, the two stages' constlen over 512 vec4 together, and whatever link() refuses.
bool encode(pm4::Stream& stream, const Program& program, const RenderPass& pass, const Draw& draw,
            const Bindings& vsBindings, const Bindings& fsBindings, uint64_t vsProgram, uint64_t fsProgram,
            uint64_t table, uint8_t* tableCpu, std::string* why, const Options& options = Options{});

// The same draw in three parts, for a pass of several (CommandBuffer::draw joins a draw to the open pass into the same
// attachments): encodeBegin the batch start, the pass begin and the clears; encodeInPass one draw's state and the draw,
// TABLE then the draw's own (its descriptors), the border colours and the timestamps being the pass's; encodeEnd the
// tail into the pass's TABLE. encode is the three over one table.
// (SKIP_CLEARS: colour attachments whose Clear a GMEM pass does in its tiles instead)
bool encodeBegin(pm4::Stream& stream, const RenderPass& pass, uint64_t table, uint8_t* tableCpu, std::string* why,
                 const Options& options = Options{}, uint32_t skipClears = 0);
// (GMEM: the pass's plan when it is drawn in tiles -- the attachments' bins in RB_MRT's BASE_GMEM, a framebuffer fetch
// reading its bin in GMEM; nullptr in sysmem. AFTER_FETCH: the draw before it in the pass read its attachments too, so
// in raster order a fetch needs no barrier)
bool encodeInPass(pm4::Stream& stream, const Program& program, const RenderPass& pass, const Draw& draw,
                  const Bindings& vsBindings, const Bindings& fsBindings, uint64_t vsProgram, uint64_t fsProgram,
                  uint64_t table, uint8_t* tableCpu, std::string* why, const Options& options = Options{}, const GmemPlan* gmem = nullptr,
                  bool afterFetch = false);
void encodeEnd(pm4::Stream& stream, const RenderPass& pass, uint64_t table, const Options& options = Options{});

// A pass with no draw: the batch start, the pass begin, the Clear's fill, the tail. What -[MTLRenderCommandEncoder
// endEncoding] needs for an encoder that recorded nothing; any load action but kClear is refused (nothing to write).
bool encodeClear(pm4::Stream& stream, const RenderPass& pass, uint64_t table, uint8_t* tableCpu, std::string* why,
                 const Options& options = Options{});

// A rectangle of a texture's level: X, Y its top left, W x H its size in pixels
struct BlitRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

// The 2D engine's copy, a pass of its own: rectangle S of SRC's mip level SL into rectangle D of DST's level DL, the two
// of one pixel format (the sRGB pair counts as one), through turnip's r2d path (r2d_setup_common, r2d_src, r2d_dst,
// r2d_coords, r2d_run). The same size is a copy texel for texel; another size a scale, LINEAR filtering it (FILTER in
// SP_PS_2D_SRC_INFO, tu6_blit_image) or not; an sRGB format so filtered is filtered in linear space, as Metal's
// generateMipmaps does it, and an unfiltered copy moves the stored bytes. False with *why: a level or a rectangle outside the texture, a depth
// texture, another format, a linear scale of an integer format, a texture copied into the very rectangle it is read from.
bool encodeBlit(pm4::Stream& stream, const Texture& src, uint32_t srcLevel, BlitRect s, const Texture& dst, uint32_t dstLevel, BlitRect d, bool linear,
                uint64_t table, uint8_t* tableCpu, std::string* why, const Options& o = Options{});

// LENGTH bytes from GPU address FROM to TO, at any alignment, by the 2D engine: what the command processor, which moves
// words, cannot do (turnip's copy_buffer with a block of one byte). Each piece is one row of R8_UNORM texels -- the
// unorm round trip gives every byte back as it was -- whose source and destination are addressed from 64 bytes below
// (what SP_PS_2D_SRC and RB_2D_DST take), the rest of the way being the rectangle's x, at most 0x4000 texels from there.
// The two ranges must not overlap, as Metal asks of a copy. A pass of its own, with what it wrote in memory at its end.
void encodeBytes(pm4::Stream& stream, uint64_t from, uint64_t to, uint64_t length, uint64_t table, uint8_t* tableCpu, const Options& o = Options{});

}  // namespace render

}  // namespace hakios::metal
