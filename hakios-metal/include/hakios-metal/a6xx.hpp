// The Adreno A6xx registers, packet words and descriptor fields a compute dispatch and one draw write, for the Adreno
// 7c+ Gen 3 (the A635 family, chip id 0x06030500: Mesa's "a6xx gen4", the same generation as the A660).
//
// Every number here is Mesa 25.0.7's (MIT), from the register XML in src/freedreno/registers/adreno/ (a6xx.xml,
// adreno_pm4.xml, adreno_common.xml) through the headers Mesa's build generates from it: a6xx.xml.h,
// adreno_pm4.xml.h, adreno_common.xml.h. The names are Mesa's, on purpose: tools/check-a6xx.py reads this file and
// those headers and checks every constant here by name, so each item below carries the header and line it comes
// from. The few values that are not in the headers (facts about this chip from Mesa's freedreno_devices.py, the
// alignment attributes that only the XML carries, one ir3 convention) are marked `not-in-headers:` with where they
// come from, and the script lists them instead of checking them. A `__SCALE` constant is the right shift Mesa's
// encoder applies before placing a field (the field holds the value in bigger units); the script reads it out of the
// encoder's body.
//
// The packets are built with pm4.hpp (type4 for register writes, type7 for the commands named here). The order in
// which a dispatch writes all this is Mesa's computerator (src/freedreno/computerator/a6xx.cc, its A6XX path) and
// Gallium's a6xx/fd6_compute.cc; the order of a draw is turnip's sysmem render pass (src/freedreno/vulkan) with
// Gallium's a6xx/fd6_gmem.cc as the second witness; those orders live with the code that emits them (compute.cpp,
// render.cpp), not here. The file runs: the chip facts, the enums, the compute registers, the texture and sampler
// descriptors, the compute values, the batch-start table, then the render path's registers and values.
#pragma once

#include <cstdint>

namespace hakios::metal::a6xx {

// --- this chip -----------------------------------------------------------------------------------------------------
// From Mesa's src/freedreno/common/freedreno_devices.py: the entry for chip_id ...06030500 ("Adreno 7c+ Gen 3", every
// speedbin of the A635) is a6xx_base + a6xx_gen4 with num_ccu = 2.

inline constexpr uint32_t kChipId = 0x06030500;             // not-in-headers: freedreno_devices.py GPUId(chip_id=0x..06030500)
inline constexpr bool kHasLpac = true;                       // not-in-headers: freedreno_devices.py a6xx_gen4.has_lpac
inline constexpr bool kSupportsDoubleThreadsize = true;      // not-in-headers: freedreno_devices.py a6xx_base.supports_double_threadsize
// CP_LOAD_STATE6_0_NUM_UNIT for a shader (ST6_SHADER) is capped at this many instruction groups: Mesa's comment says
// the blob allows 128 but 128 hangs this generation, so 127 it is (a longer shader is fetched past the preload).
inline constexpr uint32_t kInstrCacheSize = 127;             // not-in-headers: freedreno_devices.py a6xx_gen4.instr_cache_size
inline constexpr uint32_t kFibersPerSp = 128 * 2 * 16;       // not-in-headers: freedreno_devices.py, the A635 entry (fibers_per_sp)
inline constexpr uint32_t kNumSpCores = 2;                   // not-in-headers: freedreno_devices.py num_sp_cores = num_ccu for the A635
inline constexpr uint32_t kCsSharedMemSize = 32 * 1024;      // not-in-headers: freedreno_devices.py, the A635 entry (cs_shared_mem_size)
inline constexpr uint32_t kThreadsizeBase = 64;              // not-in-headers: freedreno_devices.py A6xxGPUInfo default threadsize_base
inline constexpr uint32_t kMaxWaves = 16;                    // not-in-headers: freedreno_devices.py A6xxGPUInfo default max_waves
inline constexpr uint32_t kWaveGranularity = 2;              // not-in-headers: freedreno_devices.py, the A635 entry (wave_granularity)
// ir3's "no register": regid(63, 0) = 63 << 2 | 0. The *CONSTID / *REGID fields take it for a value the shader does
// not read (the work group size and offset constants, the linear local id), and it is what Mesa writes there.
inline constexpr uint32_t kRegidNone = 0xfc;                 // not-in-headers: ir3/instr-a3xx.h INVALID_REG = regid(63, 0)
// The alignments the XML gives the 64-bit address registers (reg64 align="..."); the generated header drops them.
inline constexpr uint32_t kSpCsObjStartAlign = 32;           // not-in-headers: a6xx.xml SP_CS_OBJ_START align="32"
inline constexpr uint32_t kSpCsPvtMemAddrAlign = 32;         // not-in-headers: a6xx.xml SP_CS_PVT_MEM_ADDR align="32"
inline constexpr uint32_t kSpCsIboAlign = 16;                // not-in-headers: a6xx.xml SP_CS_IBO align="16"
inline constexpr uint32_t kSpCsTexConstAlign = 64;           // not-in-headers: a6xx.xml SP_CS_TEX_CONST align="64"
inline constexpr uint32_t kSpCsTexSampAlign = 16;            // not-in-headers: a6xx.xml SP_CS_TEX_SAMP align="16"
// What both Mesa emitters give CP_WAIT_REG_MEM_5_DELAY_LOOP_CYCLES when waiting for a timestamp (a choice, not a fact)
inline constexpr uint32_t kDelayLoopCycles = 16;             // not-in-headers: a6xx.cc cache_flush and fd6_emit.h fd6_cache_flush both pass 16

// --- enums ----------------------------------------------------------------------------------------------------------

enum a3xx_threadmode : uint32_t {
    MULTI = 0,                     // adreno_common.xml.h:174
    SINGLE = 1,                    // adreno_common.xml.h:175  one wave per SP at a time
};

enum a6xx_threadsize : uint32_t {
    THREAD64 = 0,                  // a6xx.xml.h:2299
    THREAD128 = 1,                 // a6xx.xml.h:2300  ir3's double_threadsize; this chip supports it
};

// The split of the local buffer between constants and shared memory; must be at least the shader's CONSTLEN.
enum a6xx_const_ram_mode : uint32_t {
    CONSTLEN_128 = 0,              // a6xx.xml.h:2304
    CONSTLEN_192 = 1,              // a6xx.xml.h:2305
    CONSTLEN_256 = 2,              // a6xx.xml.h:2306
    CONSTLEN_512 = 3,              // a6xx.xml.h:2307  the XML says a7xx only; a6xx.cc's ladder still picks it above 256
};

enum a6xx_isam_mode : uint32_t {
    ISAMMODE_CL = 1,               // a6xx.xml.h:2321
    ISAMMODE_GL = 2,               // a6xx.xml.h:2322  what computerator and Gallium set in SP_MODE_CONTROL
};

enum a6xx_tile_mode : uint32_t {
    TILE6_LINEAR = 0,              // a6xx.xml.h:57
};

// The texture formats served (texture.hpp's table), as fd6_format_table.c's .tex column names them for the Metal
// pixel formats: the sRGB ones are the UNORM format plus TEX_CONST_0_SRGB, BGRA8 is 8_8_8_8_UNORM with SWAP WXYZ.
enum a6xx_format : uint32_t {
    FMT6_8_UNORM = 3,              // a6xx.xml.h:64   R8Unorm
    FMT6_8_8_UNORM = 15,           // a6xx.xml.h:72   RG8Unorm
    FMT6_8_8_8_8_UNORM = 48,       // a6xx.xml.h:86   RGBA8Unorm, RGBA8Unorm_sRGB, and with SWAP = WXYZ the BGRA8 pair
    FMT6_32_FLOAT = 74,            // a6xx.xml.h:105  R32Float
    FMT6_32_UINT = 75,             // a6xx.xml.h:106  a raw buffer: one dword per element; R32Uint
    FMT6_16_16_16_16_FLOAT = 98,   // a6xx.xml.h:116  RGBA16Float
    FMT6_32_32_FLOAT = 103,        // a6xx.xml.h:121  a float2 vertex attribute (fd6_format_table.c:258 VTC(R32G32_FLOAT, 32_32_FLOAT, WZYX))
    FMT6_32_32_UINT = 104,         // a6xx.xml.h:122
    FMT6_32_32_32_FLOAT = 116,     // a6xx.xml.h:129  a float3 attribute
    FMT6_32_32_32_32_FLOAT = 130,  // a6xx.xml.h:133  RGBA32Float
    FMT6_32_32_32_32_UINT = 131,   // a6xx.xml.h:134  RGBA32Uint
    FMT6_NONE = 255,               // a6xx.xml.h:189  what the fake GPU's bytes-per-pixel table answers for a format it does not know
};

// TEX_CONST_0.SWAP: the component order in memory, applied before the border colour (the XML); the swizzle after
enum a3xx_color_swap : uint32_t {
    WZYX = 0,                      // adreno_common.xml.h:189  RGBA order in memory: every served format but BGRA
    WXYZ = 1,                      // adreno_common.xml.h:190  BGRA order (fd6_format_table.c:174)
    ZYXW = 2,                      // adreno_common.xml.h:191
    XYZW = 3,                      // adreno_common.xml.h:192
};

enum a3xx_msaa_samples : uint32_t {
    MSAA_ONE = 0,                  // adreno_common.xml.h:167  TEX_CONST_0.SAMPLES of every texture served (no MSAA)
    MSAA_TWO = 1,                  // adreno_common.xml.h:168
    MSAA_FOUR = 2,                 // adreno_common.xml.h:169
    MSAA_EIGHT = 3,                // adreno_common.xml.h:170
};

// TEX_SAMP_1.COMPARE_FUNC. MTLCompareFunction (MTLDepthStencil.h: Never 0, Less, Equal, LessEqual, Greater, NotEqual,
// GreaterEqual, Always 7) has the same order, as PIPE_FUNC and VkCompareOp do (Gallium and turnip cast 1:1).
enum adreno_compare_func : uint32_t {
    FUNC_NEVER = 0,                // adreno_common.xml.h:70
    FUNC_LESS = 1,                 // adreno_common.xml.h:71
    FUNC_EQUAL = 2,                // adreno_common.xml.h:72
    FUNC_LEQUAL = 3,               // adreno_common.xml.h:73
    FUNC_GREATER = 4,              // adreno_common.xml.h:74
    FUNC_NOTEQUAL = 5,             // adreno_common.xml.h:75
    FUNC_GEQUAL = 6,               // adreno_common.xml.h:76
    FUNC_ALWAYS = 7,               // adreno_common.xml.h:77
};

enum a6xx_tex_type : uint32_t {
    A6XX_TEX_1D = 0,               // a6xx.xml.h:1130
    A6XX_TEX_2D = 1,               // a6xx.xml.h:1131
    A6XX_TEX_CUBE = 2,             // a6xx.xml.h:1132
    A6XX_TEX_3D = 3,               // a6xx.xml.h:1133
    A6XX_TEX_BUFFER = 4,           // a6xx.xml.h:1134  a buffer view: WIDTH/HEIGHT hold the element count
    A6XX_TEX_IMG_BUFFER = 5,       // a6xx.xml.h:1135
};

enum a6xx_tex_swiz : uint32_t {
    A6XX_TEX_X = 0,                // a6xx.xml.h:2362
    A6XX_TEX_Y = 1,                // a6xx.xml.h:2363
    A6XX_TEX_Z = 2,                // a6xx.xml.h:2364
    A6XX_TEX_W = 3,                // a6xx.xml.h:2365
    A6XX_TEX_ZERO = 4,             // a6xx.xml.h:2366
    A6XX_TEX_ONE = 5,              // a6xx.xml.h:2367  what SWIZ_W reads for a format without alpha (fdl6_format_swiz, fd6_view.c:117-120)
};

// TEX_SAMP_0.XY_MAG / XY_MIN. Gallium (fd6_texture.cc tex_filter): nearest -> NEAREST, linear -> LINEAR, or ANISO when
// the anisotropy field is not ANISO_1.
enum a6xx_tex_filter : uint32_t {
    A6XX_TEX_NEAREST = 0,          // a6xx.xml.h:2333
    A6XX_TEX_LINEAR = 1,           // a6xx.xml.h:2334
    A6XX_TEX_ANISO = 2,            // a6xx.xml.h:2335
    A6XX_TEX_CUBIC = 3,            // a6xx.xml.h:2336  "a650 only" (the XML)
};

// TEX_SAMP_0.WRAP_S/T/R. Not MTLSamplerAddressMode's order (ClampToEdge 0, MirrorClampToEdge 1, Repeat 2, MirrorRepeat 3,
// ClampToZero 4, ClampToBorderColor 5): texture.cpp maps. MIRROR_CLAMP "only works for PoT" (fd6_texture.cc:47).
enum a6xx_tex_clamp : uint32_t {
    A6XX_TEX_REPEAT = 0,           // a6xx.xml.h:2340
    A6XX_TEX_CLAMP_TO_EDGE = 1,    // a6xx.xml.h:2341
    A6XX_TEX_MIRROR_REPEAT = 2,    // a6xx.xml.h:2342
    A6XX_TEX_CLAMP_TO_BORDER = 3,  // a6xx.xml.h:2343  Metal's ClampToZero: border entry 0 of the zero table (TEX_SAMP_2.BCOLOR 0)
    A6XX_TEX_MIRROR_CLAMP = 4,     // a6xx.xml.h:2344
};

// TEX_SAMP_0.ANISO: Gallium's util_last_bit(MIN2(max_anisotropy >> 1, 8)) maps 1, 2, 4, 8, 16 to these in order
enum a6xx_tex_aniso : uint32_t {
    A6XX_TEX_ANISO_1 = 0,          // a6xx.xml.h:2348
    A6XX_TEX_ANISO_2 = 1,          // a6xx.xml.h:2349
    A6XX_TEX_ANISO_4 = 2,          // a6xx.xml.h:2350
    A6XX_TEX_ANISO_8 = 3,          // a6xx.xml.h:2351
    A6XX_TEX_ANISO_16 = 4,         // a6xx.xml.h:2352
};

enum a6xx_reduction_mode : uint32_t {
    A6XX_REDUCTION_MODE_AVERAGE = 0,   // a6xx.xml.h:2356  the filter's weighted mean: every Metal sampler
    A6XX_REDUCTION_MODE_MIN = 1,       // a6xx.xml.h:2357
    A6XX_REDUCTION_MODE_MAX = 2,       // a6xx.xml.h:2358
};

// CP_LOAD_STATE6: what is loaded, from where, and for which stage
enum a6xx_state_block : uint32_t {
    SB6_VS_TEX = 0,                // adreno_pm4.xml.h:405
    SB6_FS_TEX = 4,                // adreno_pm4.xml.h:409
    SB6_CS_TEX = 5,                // adreno_pm4.xml.h:410  compute textures and samplers
    SB6_VS_SHADER = 8,             // adreno_pm4.xml.h:411
    SB6_FS_SHADER = 12,            // adreno_pm4.xml.h:415
    SB6_CS_SHADER = 13,            // adreno_pm4.xml.h:416  the compute program, its constants, and (with ST6_IBO) its IBOs
    SB6_IBO = 14,                  // adreno_pm4.xml.h:417  the 3D pipeline's IBOs (CP_LOAD_STATE6, not _FRAG)
    SB6_CS_IBO = 15,               // adreno_pm4.xml.h:418
};

enum a6xx_state_type : uint32_t {
    ST6_SHADER = 0,                // adreno_pm4.xml.h:422  NUM_UNIT in instruction groups (ir3's instrlen)
    ST6_CONSTANTS = 1,             // adreno_pm4.xml.h:423  NUM_UNIT and DST_OFF in vec4
    ST6_UBO = 2,                   // adreno_pm4.xml.h:424
    ST6_IBO = 3,                   // adreno_pm4.xml.h:425  NUM_UNIT in 16-dword descriptors
};

enum a6xx_state_src : uint32_t {
    SS6_DIRECT = 0,                // adreno_pm4.xml.h:429  the data follows in the packet
    SS6_BINDLESS = 1,              // adreno_pm4.xml.h:430
    SS6_INDIRECT = 2,              // adreno_pm4.xml.h:431  the data is at EXT_SRC_ADDR
    SS6_UBO = 3,                   // adreno_pm4.xml.h:432
};

// CP_EVENT_WRITE's event. Several names share a value because the XML gives each generation its own name for the
// same number; only the A6XX ones a compute dispatch, a draw and their barriers use are here. turnip gives the
// 4-word form (an address and a value, no TIMESTAMP bit) to the events that complete asynchronously: RB_DONE_TS,
// CACHE_FLUSH_TS, PC_CCU_RESOLVE_TS, PC_CCU_FLUSH_DEPTH_TS, PC_CCU_FLUSH_COLOR_TS (freedreno_gpu_event.h:60-85
// needs_seqno); the invalidates, LRZ_FLUSH, LRZ_CLEAR, BLIT, LABEL and ZPASS_DONE are 1-word events.
enum vgt_event_type : uint32_t {
    CACHE_FLUSH_TS = 4,            // adreno_pm4.xml.h:60   flush the caches, then write the timestamp (Gallium's FD_CACHE_CLEAN)
    CACHE_FLUSH = 6,               // adreno_pm4.xml.h:62
    START_COMPUTE_CTRS = 15,       // adreno_pm4.xml.h:85
    STOP_COMPUTE_CTRS = 16,        // adreno_pm4.xml.h:86
    ZPASS_DONE = 21,               // adreno_pm4.xml.h:75
    CACHE_FLUSH_AND_INV_EVENT = 22, // adreno_pm4.xml.h:76  the A2xx name of the same value
    RB_DONE_TS = 22,               // adreno_pm4.xml.h:77   the GPU is done with everything before it; timestamp (FD_RB_DONE)
    PC_CCU_INVALIDATE_DEPTH = 24,  // adreno_pm4.xml.h:91
    PC_CCU_INVALIDATE_COLOR = 25,  // adreno_pm4.xml.h:92
    PC_CCU_RESOLVE_TS = 26,        // adreno_pm4.xml.h:93
    PC_CCU_FLUSH_DEPTH_TS = 28,    // adreno_pm4.xml.h:94
    PC_CCU_FLUSH_COLOR_TS = 29,    // adreno_pm4.xml.h:95   the colour CCU cache to memory: what puts a draw's pixels where the CPU reads them
    BLIT = 30,                     // adreno_pm4.xml.h:96
    LRZ_CLEAR = 37,                // adreno_pm4.xml.h:98
    LRZ_FLUSH = 38,                // adreno_pm4.xml.h:99   both witnesses end a sysmem pass with it (tu_lrz.cc:465; fd6_gmem.cc:2114)
    CACHE_INVALIDATE = 49,         // adreno_pm4.xml.h:108  invalidate UCHE: before reading what the CPU or a blit wrote
    LABEL = 63,                    // adreno_pm4.xml.h:109  a no-op event; Gallium's dummy to roll the register context
};

// CP_SET_MARKER's MODE: RM6_DIRECT_RENDER before a sysmem pass (tu_cmd_buffer.cc:2130-2131; fd6_gmem.cc:2041-2042);
// there is no RM6_BYPASS in this Mesa, value 1 is this name (adreno_pm4.xml:1797-1798)
enum a6xx_marker : uint32_t {
    RM6_DIRECT_RENDER = 1,         // adreno_pm4.xml.h:519
    RM6_COMPUTE = 8,               // adreno_pm4.xml.h:526
};

enum cp_cond_function : uint32_t {
    WRITE_ALWAYS = 0,              // adreno_pm4.xml.h:464
    WRITE_LT = 1,                  // adreno_pm4.xml.h:465
    WRITE_LE = 2,                  // adreno_pm4.xml.h:466
    WRITE_EQ = 3,                  // adreno_pm4.xml.h:467
    WRITE_NE = 4,                  // adreno_pm4.xml.h:468
    WRITE_GE = 5,                  // adreno_pm4.xml.h:469
    WRITE_GT = 6,                  // adreno_pm4.xml.h:470
};

enum poll_memory_type : uint32_t {
    POLL_REGISTER = 0,             // adreno_pm4.xml.h:474
    POLL_MEMORY = 1,               // adreno_pm4.xml.h:475
    POLL_SCRATCH = 2,              // adreno_pm4.xml.h:476
    POLL_ON_CHIP = 3,              // adreno_pm4.xml.h:477
};

// The type-7 opcodes a dispatch and a draw use (pm4.hpp has the ones every stream uses, under its own names).
enum Opcode : uint32_t {
    CP_WAIT_MEM_WRITES = 0x12,     // adreno_pm4.xml.h:283  in turnip's flush order, after CACHE_INVALIDATE (tu_cmd_buffer.cc:194-250)
    CP_WAIT_FOR_ME = 0x13,         // adreno_pm4.xml.h:270
    CP_WAIT_MEM_GTE = 0x14,        // adreno_pm4.xml.h:326  wait until a memory word >= REF (signed)
    CP_SKIP_IB2_ENABLE_GLOBAL = 0x1d,  // adreno_pm4.xml.h:294  1 word, 0 at pass begin and end (tu_cmd_buffer.cc:2134-2135, 2161-2162); undocumented
    CP_SKIP_IB2_ENABLE_LOCAL = 0x23,   // adreno_pm4.xml.h:295  1 word, 1: fd6 only ("I think that is not required", fd6_gmem.cc:2048)
    CP_WAIT_FOR_IDLE = 0x26,       // adreno_pm4.xml.h:219
    CP_LOAD_STATE6_GEOM = 0x32,    // adreno_pm4.xml.h:308  the VS's loads (tu6_stage2opcode: VS -> GEOM, FS -> FRAG, tu_shader.cc:1349)
    CP_EXEC_CS = 0x33,             // adreno_pm4.xml.h:287
    CP_LOAD_STATE6_FRAG = 0x34,    // adreno_pm4.xml.h:309  what compute uses, for its shader, constants, IBOs and textures
    CP_LOAD_STATE6 = 0x36,         // adreno_pm4.xml.h:310
    CP_DRAW_INDX_OFFSET = 0x38,    // adreno_pm4.xml.h:272  the draw: 3 words without an index buffer (adreno_pm4.xml:906-916)
    CP_WAIT_REG_MEM = 0x3c,        // adreno_pm4.xml.h:220
    CP_REG_TO_MEM = 0x3e,          // adreno_pm4.xml.h:228
    CP_INDIRECT_BUFFER = 0x3f,     // adreno_pm4.xml.h:216  (pm4.hpp's kIndirectBuffer; here for the batch-start sequence)
    CP_EXEC_CS_INDIRECT = 0x41,    // adreno_pm4.xml.h:286
    CP_MEM_TO_REG = 0x42,          // adreno_pm4.xml.h:285
    CP_SET_DRAW_STATE = 0x43,      // adreno_pm4.xml.h:271
    CP_EVENT_WRITE = 0x46,         // adreno_pm4.xml.h:234
    CP_SET_AMBLE = 0x55,           // adreno_pm4.xml.h:332  the IBs the CP runs around a context switch
    CP_CONTEXT_REG_BUNCH = 0x5c,   // adreno_pm4.xml.h:292  (register, value) pairs: turnip's stream-out disable and VFD_FETCH strides use it; not needed with type4
    CP_SET_MODE = 0x63,            // adreno_pm4.xml.h:307  Gallium writes 1 so that CP_SET_DRAW_STATE runs at once, 0 at batch start and pass begin
    CP_SET_VISIBILITY_OVERRIDE = 0x64,  // adreno_pm4.xml.h:298  1 word, 1 at a sysmem pass begin (tu_cmd_buffer.cc:2138-2139); undocumented
    CP_SET_MARKER = 0x65,          // adreno_pm4.xml.h:290
    CP_MEM_TO_MEM = 0x73,          // adreno_pm4.xml.h:304
};

// --- the draw's enums ----------------------------------------------------------------------------------------------------

// CP_DRAW_INDX_OFFSET's PRIM_TYPE (MTLPrimitiveType maps through fd6_screen.cc:132's table: triangles -> TRILIST,
// strips -> TRISTRIP, points -> POINTLIST, lines -> LINELIST/LINESTRIP; RECTLIST is r3d's)
enum pc_di_primtype : uint32_t {
    DI_PT_NONE = 0,            // adreno_pm4.xml.h:127
    DI_PT_POINTLIST_PSIZE = 1, // adreno_pm4.xml.h:128
    DI_PT_LINELIST = 2,        // adreno_pm4.xml.h:129
    DI_PT_LINESTRIP = 3,       // adreno_pm4.xml.h:130
    DI_PT_TRILIST = 4,         // adreno_pm4.xml.h:131
    DI_PT_TRIFAN = 5,          // adreno_pm4.xml.h:132
    DI_PT_TRISTRIP = 6,        // adreno_pm4.xml.h:133
    DI_PT_RECTLIST = 8,        // adreno_pm4.xml.h:135
    DI_PT_POINTLIST = 9,       // adreno_pm4.xml.h:136
};

// SOURCE_SELECT: AUTO_INDEX for a non-indexed draw (the VFD counts), DMA for an index buffer
enum pc_di_src_sel : uint32_t {
    DI_SRC_SEL_DMA = 0,        // adreno_pm4.xml.h:176
    DI_SRC_SEL_IMMEDIATE = 1,  // adreno_pm4.xml.h:177
    DI_SRC_SEL_AUTO_INDEX = 2, // adreno_pm4.xml.h:178
    DI_SRC_SEL_AUTO_XFB = 3,   // adreno_pm4.xml.h:179
};

// VIS_CULL: whether the draw consults a visibility stream; none exists in a sysmem pass (CP_SET_VISIBILITY_OVERRIDE 1)
enum pc_di_vis_cull_mode : uint32_t {
    IGNORE_VISIBILITY = 0,     // adreno_pm4.xml.h:198
    USE_VISIBILITY = 1,        // adreno_pm4.xml.h:199
};

enum a4xx_index_size : uint32_t {
    INDEX4_SIZE_8_BIT = 0,     // adreno_pm4.xml.h:436
    INDEX4_SIZE_16_BIT = 1,    // adreno_pm4.xml.h:437
    INDEX4_SIZE_32_BIT = 2,    // adreno_pm4.xml.h:438
};

enum a6xx_patch_type : uint32_t {
    TESS_QUADS = 0,            // adreno_pm4.xml.h:442
    TESS_TRIANGLES = 1,        // adreno_pm4.xml.h:443
    TESS_ISOLINES = 2,         // adreno_pm4.xml.h:444
};

// GRAS/RB_BIN_CONTROL.BUFFERS_LOCATION: the colour and depth buffers of a sysmem pass are in memory, not in GMEM
enum a6xx_buffers_location : uint32_t {
    BUFFERS_IN_GMEM = 0,       // a6xx.xml.h:2213
    BUFFERS_IN_SYSMEM = 3,     // a6xx.xml.h:2214
};

// GRAS/RB_BIN_CONTROL.LRZ_FEEDBACK_ZMODE_MASK: turnip writes EARLY_Z_OR_EARLY_LRZ_LATE_Z on a chip with lrz feedback
// (this one), fd6 and r3d NONE
enum a6xx_lrz_feedback_mask : uint32_t {
    LRZ_FEEDBACK_NONE = 0,     // a6xx.xml.h:2218
    LRZ_FEEDBACK_EARLY_Z = 1,  // a6xx.xml.h:2219
    LRZ_FEEDBACK_EARLY_LRZ_LATE_Z = 2, // a6xx.xml.h:2220
    LRZ_FEEDBACK_EARLY_Z_OR_EARLY_LRZ_LATE_Z = 3, // a6xx.xml.h:2221
    LRZ_FEEDBACK_LATE_Z = 4,   // a6xx.xml.h:2222
};

// RB_DEPTH_BUFFER_INFO / GRAS_SU_DEPTH_BUFFER_INFO.DEPTH_FORMAT: NONE for a pass without a depth attachment
enum a6xx_depth_format : uint32_t {
    DEPTH6_NONE = 0,           // a6xx.xml.h:199
    DEPTH6_16 = 1,             // a6xx.xml.h:200
    DEPTH6_24_8 = 2,           // a6xx.xml.h:201
    DEPTH6_32 = 4,             // a6xx.xml.h:202
};

// VPC_POLYGON_MODE / PC_POLYGON_MODE: filled triangles
enum a6xx_polygon_mode : uint32_t {
    POLYMODE6_POINTS = 1,      // a6xx.xml.h:193
    POLYMODE6_LINES = 2,       // a6xx.xml.h:194
    POLYMODE6_TRIANGLES = 3,   // a6xx.xml.h:195
};

// VPC_VARYING_INTERP_MODE[i]: two bits per varying component
enum a6xx_varying_interp_mode : uint32_t {
    INTERP_SMOOTH = 0,         // a6xx.xml.h:2285
    INTERP_FLAT = 1,           // a6xx.xml.h:2286
    INTERP_ZERO = 2,           // a6xx.xml.h:2287
    INTERP_ONE = 3,            // a6xx.xml.h:2288
};

// VPC_VARYING_PS_REPL_MODE[i]: point sprite coordinate replacement, none for a triangle
enum a6xx_varying_ps_repl_mode : uint32_t {
    PS_REPL_NONE = 0,          // a6xx.xml.h:2292
    PS_REPL_S = 1,             // a6xx.xml.h:2293
    PS_REPL_T = 2,             // a6xx.xml.h:2294
    PS_REPL_ONE_MINUS_T = 3,   // a6xx.xml.h:2295
};

// RB/GRAS_SU_DEPTH_PLANE_CNTL.Z_MODE: when the depth test runs relative to the FS; pipelines with the test off write
// LATE_Z (tu_cmd_buffer.cc:5569-5571), r3d EARLY_Z
enum a6xx_ztest_mode : uint32_t {
    A6XX_EARLY_Z = 0,          // a6xx.xml.h:1139
    A6XX_LATE_Z = 1,           // a6xx.xml.h:1140
    A6XX_EARLY_LRZ_LATE_Z = 2, // a6xx.xml.h:1141
    A6XX_INVALID_ZTEST = 3,    // a6xx.xml.h:1142
};

// GRAS_SC_CNTL.SINGLE_PRIM_MODE: NO_FLUSH for a sysmem pass without feedback loops (tu_pipeline.cc:3351-3380)
enum a6xx_single_prim_mode : uint32_t {
    NO_FLUSH = 0,              // a6xx.xml.h:2190
    FLUSH_PER_OVERLAP_AND_OVERWRITE = 1, // a6xx.xml.h:2191
    FLUSH_PER_OVERLAP = 3,     // a6xx.xml.h:2192
};

// RB_RENDER_CONTROL1 / GRAS_LRZ_PS_INPUT_CNTL.FRAGCOORDSAMPLEMODE: CENTER without sample shading
enum a6xx_fragcoord_sample_mode : uint32_t {
    FRAGCOORD_CENTER = 0,      // a6xx.xml.h:2240
    FRAGCOORD_SAMPLE = 3,      // a6xx.xml.h:2241
};

// RB_MRT_CONTROL.ROP_CODE: turnip leaves ROP_CLEAR (VK_LOGIC_OP_CLEAR's) when the ROP is off, fd6 ROP_COPY; inert either way
enum a3xx_rop_code : uint32_t {
    ROP_CLEAR = 0,             // adreno_common.xml.h:141
    ROP_NOR = 1,               // adreno_common.xml.h:142
    ROP_AND_INVERTED = 2,      // adreno_common.xml.h:143
    ROP_COPY_INVERTED = 3,     // adreno_common.xml.h:144
    ROP_AND_REVERSE = 4,       // adreno_common.xml.h:145
    ROP_INVERT = 5,            // adreno_common.xml.h:146
    ROP_XOR = 6,               // adreno_common.xml.h:147
    ROP_NAND = 7,              // adreno_common.xml.h:148
    ROP_AND = 8,               // adreno_common.xml.h:149
    ROP_EQUIV = 9,             // adreno_common.xml.h:150
    ROP_NOOP = 10,             // adreno_common.xml.h:151
    ROP_OR_INVERTED = 11,      // adreno_common.xml.h:152
    ROP_COPY = 12,             // adreno_common.xml.h:153
    ROP_OR_REVERSE = 13,       // adreno_common.xml.h:154
    ROP_OR = 14,               // adreno_common.xml.h:155
    ROP_SET = 15,              // adreno_common.xml.h:156
};

// GRAS_SU_CNTL.LINE_MODE: turnip RECTANGULAR, fd6 BRESENHAM unless multisampled; lines only
enum a5xx_line_mode : uint32_t {
    BRESENHAM = 0,             // adreno_common.xml.h:215
    RECTANGULAR = 1,           // adreno_common.xml.h:216
};

// RB_MRT_BLEND_CONTROL's opcodes and factors (MTLBlendOperation / MTLBlendFactor map onto them: a later step), the
// dither modes and stencil ops
enum a3xx_rb_blend_opcode : uint32_t {
    BLEND_DST_PLUS_SRC = 0,    // adreno_common.xml.h:196
    BLEND_SRC_MINUS_DST = 1,   // adreno_common.xml.h:197
    BLEND_DST_MINUS_SRC = 2,   // adreno_common.xml.h:198
    BLEND_MIN_DST_SRC = 3,     // adreno_common.xml.h:199
    BLEND_MAX_DST_SRC = 4,     // adreno_common.xml.h:200
};

enum adreno_rb_blend_factor : uint32_t {
    FACTOR_ZERO = 0,           // adreno_common.xml.h:92
    FACTOR_ONE = 1,            // adreno_common.xml.h:93
    FACTOR_SRC_COLOR = 4,      // adreno_common.xml.h:94
    FACTOR_ONE_MINUS_SRC_COLOR = 5, // adreno_common.xml.h:95
    FACTOR_SRC_ALPHA = 6,      // adreno_common.xml.h:96
    FACTOR_ONE_MINUS_SRC_ALPHA = 7, // adreno_common.xml.h:97
    FACTOR_DST_COLOR = 8,      // adreno_common.xml.h:98
    FACTOR_ONE_MINUS_DST_COLOR = 9, // adreno_common.xml.h:99
    FACTOR_DST_ALPHA = 10,     // adreno_common.xml.h:100
    FACTOR_ONE_MINUS_DST_ALPHA = 11, // adreno_common.xml.h:101
    FACTOR_CONSTANT_COLOR = 12, // adreno_common.xml.h:102
    FACTOR_ONE_MINUS_CONSTANT_COLOR = 13, // adreno_common.xml.h:103
    FACTOR_CONSTANT_ALPHA = 14, // adreno_common.xml.h:104
    FACTOR_ONE_MINUS_CONSTANT_ALPHA = 15, // adreno_common.xml.h:105
    FACTOR_SRC_ALPHA_SATURATE = 16, // adreno_common.xml.h:106
    FACTOR_SRC1_COLOR = 20,    // adreno_common.xml.h:107
    FACTOR_ONE_MINUS_SRC1_COLOR = 21, // adreno_common.xml.h:108
    FACTOR_SRC1_ALPHA = 22,    // adreno_common.xml.h:109
    FACTOR_ONE_MINUS_SRC1_ALPHA = 23, // adreno_common.xml.h:110
};

enum adreno_rb_dither_mode : uint32_t {
    DITHER_DISABLE = 0,        // adreno_common.xml.h:123
    DITHER_ALWAYS = 1,         // adreno_common.xml.h:124
    DITHER_IF_ALPHA_OFF = 2,   // adreno_common.xml.h:125
};

enum adreno_stencil_op : uint32_t {
    STENCIL_KEEP = 0,          // adreno_common.xml.h:81
    STENCIL_ZERO = 1,          // adreno_common.xml.h:82
    STENCIL_REPLACE = 2,       // adreno_common.xml.h:83
    STENCIL_INCR_CLAMP = 3,    // adreno_common.xml.h:84
    STENCIL_DECR_CLAMP = 4,    // adreno_common.xml.h:85
    STENCIL_INVERT = 5,        // adreno_common.xml.h:86
    STENCIL_INCR_WRAP = 6,     // adreno_common.xml.h:87
    STENCIL_DECR_WRAP = 7,     // adreno_common.xml.h:88
};

// --- a field encoder, as Mesa's generated ones work ---------------------------------------------------------------------

constexpr uint32_t field(uint32_t value, uint32_t shift, uint32_t mask) { return (value << shift) & mask; }

// --- SP registers --------------------------------------------------------------------------------------------------------

inline constexpr uint32_t REG_A6XX_SP_MODE_CONTROL = 0xab00;                           // a6xx.xml.h:9430
// half-register loads from the constant file read a 32-bit value and convert, instead of 16 packed bits
inline constexpr uint32_t A6XX_SP_MODE_CONTROL_CONSTANT_DEMOTION_ENABLE = 0x00000001;  // a6xx.xml.h:9431
inline constexpr uint32_t A6XX_SP_MODE_CONTROL_ISAMMODE__MASK = 0x00000006;            // a6xx.xml.h:9432
inline constexpr uint32_t A6XX_SP_MODE_CONTROL_ISAMMODE__SHIFT = 1;                    // a6xx.xml.h:9433
inline constexpr uint32_t A6XX_SP_MODE_CONTROL_SHARED_CONSTS_ENABLE = 0x00000008;      // a6xx.xml.h:9438
constexpr uint32_t A6XX_SP_MODE_CONTROL_ISAMMODE(a6xx_isam_mode v) {
    return field(v, A6XX_SP_MODE_CONTROL_ISAMMODE__SHIFT, A6XX_SP_MODE_CONTROL_ISAMMODE__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_PERFCTR_ENABLE = 0xae0f;                          // a6xx.xml.h:9587
inline constexpr uint32_t A6XX_SP_PERFCTR_ENABLE_VS = 0x00000001;                       // a6xx.xml.h:9588
inline constexpr uint32_t A6XX_SP_PERFCTR_ENABLE_HS = 0x00000002;                       // a6xx.xml.h:9589
inline constexpr uint32_t A6XX_SP_PERFCTR_ENABLE_DS = 0x00000004;                       // a6xx.xml.h:9590
inline constexpr uint32_t A6XX_SP_PERFCTR_ENABLE_GS = 0x00000008;                       // a6xx.xml.h:9591
inline constexpr uint32_t A6XX_SP_PERFCTR_ENABLE_FS = 0x00000010;                       // a6xx.xml.h:9592
inline constexpr uint32_t A6XX_SP_PERFCTR_ENABLE_CS = 0x00000020;                       // a6xx.xml.h:9593

inline constexpr uint32_t REG_A6XX_SP_FLOAT_CNTL = 0xae04;                              // a6xx.xml.h:9571  computerator writes 0
inline constexpr uint32_t A6XX_SP_FLOAT_CNTL_F16_NO_INF = 0x00000008;                   // a6xx.xml.h:9572

// Gallium's workaround for a hardware bug: when the program is longer than the instruction cache, the SP may bounds
// check a fetch against the *fragment* shader's length from the other register context, so it is set to the compute
// length too, followed by a LABEL event to roll the context (fd6_compute.cc, Mesa merge request 19023).
inline constexpr uint32_t REG_A6XX_SP_FS_INSTRLEN = 0xab05;                             // a6xx.xml.h:9473

inline constexpr uint32_t REG_A6XX_SP_CS_CTRL_REG0 = 0xa9b0;                            // a6xx.xml.h:9109
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_THREADMODE__MASK = 0x00000001;           // a6xx.xml.h:9110
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_THREADMODE__SHIFT = 0;                   // a6xx.xml.h:9111
// footprints are register counts: ir3's max register index plus one, half and full separately
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT__MASK = 0x0000007e;     // a6xx.xml.h:9116
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT__SHIFT = 1;             // a6xx.xml.h:9117
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT__MASK = 0x00001f80;     // a6xx.xml.h:9122
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT__SHIFT = 7;             // a6xx.xml.h:9123
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_UNK13 = 0x00002000;                      // a6xx.xml.h:9128  meaning unknown to Mesa
// the flow control nesting depth; ir3 writes ceil(min(branchstack, the compiler's branchstack_size) / 2)
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_BRANCHSTACK__MASK = 0x000fc000;          // a6xx.xml.h:9129
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_BRANCHSTACK__SHIFT = 14;                 // a6xx.xml.h:9130
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_THREADSIZE__MASK = 0x00100000;           // a6xx.xml.h:9135
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_THREADSIZE__SHIFT = 20;                  // a6xx.xml.h:9136
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_UNK21 = 0x00200000;                      // a6xx.xml.h:9141  meaning unknown to Mesa
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_UNK22 = 0x00400000;                      // a6xx.xml.h:9142  meaning unknown to Mesa
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_EARLYPREAMBLE = 0x00800000;              // a6xx.xml.h:9143
// the half registers overlap the full ones (hrN is half of rN/2); ir3 sets it for every a6xx shader
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG0_MERGEDREGS = 0x80000000;                 // a6xx.xml.h:9144
constexpr uint32_t A6XX_SP_CS_CTRL_REG0_THREADMODE(a3xx_threadmode v) {
    return field(v, A6XX_SP_CS_CTRL_REG0_THREADMODE__SHIFT, A6XX_SP_CS_CTRL_REG0_THREADMODE__MASK);
}
constexpr uint32_t A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(uint32_t regs) {
    return field(regs, A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT__SHIFT, A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT__MASK);
}
constexpr uint32_t A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(uint32_t regs) {
    return field(regs, A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT__SHIFT, A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT__MASK);
}
constexpr uint32_t A6XX_SP_CS_CTRL_REG0_BRANCHSTACK(uint32_t depth) {
    return field(depth, A6XX_SP_CS_CTRL_REG0_BRANCHSTACK__SHIFT, A6XX_SP_CS_CTRL_REG0_BRANCHSTACK__MASK);
}
constexpr uint32_t A6XX_SP_CS_CTRL_REG0_THREADSIZE(a6xx_threadsize v) {
    return field(v, A6XX_SP_CS_CTRL_REG0_THREADSIZE__SHIFT, A6XX_SP_CS_CTRL_REG0_THREADSIZE__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_CS_CTRL_REG1 = 0xa9b1;                            // a6xx.xml.h:9147
// 0: all 32 KiB of shared memory; otherwise (n + 1) KiB. ldl/stl beyond the limit read as address 0.
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG1_SHARED_SIZE__MASK = 0x0000001f;          // a6xx.xml.h:9148
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG1_SHARED_SIZE__SHIFT = 0;                  // a6xx.xml.h:9149
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG1_CONSTANTRAMMODE__MASK = 0x00000060;      // a6xx.xml.h:9154
inline constexpr uint32_t A6XX_SP_CS_CTRL_REG1_CONSTANTRAMMODE__SHIFT = 5;              // a6xx.xml.h:9155
constexpr uint32_t A6XX_SP_CS_CTRL_REG1_SHARED_SIZE(uint32_t n) {
    return field(n, A6XX_SP_CS_CTRL_REG1_SHARED_SIZE__SHIFT, A6XX_SP_CS_CTRL_REG1_SHARED_SIZE__MASK);
}
constexpr uint32_t A6XX_SP_CS_CTRL_REG1_CONSTANTRAMMODE(a6xx_const_ram_mode v) {
    return field(v, A6XX_SP_CS_CTRL_REG1_CONSTANTRAMMODE__SHIFT, A6XX_SP_CS_CTRL_REG1_CONSTANTRAMMODE__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_CS_OBJ_FIRST_EXEC_OFFSET = 0xa9b3;                // a6xx.xml.h:9165  Gallium writes 0
inline constexpr uint32_t REG_A6XX_SP_CS_OBJ_START = 0xa9b4;                            // a6xx.xml.h:9168  64-bit: the program's address, lo then hi

inline constexpr uint32_t REG_A6XX_SP_CS_PVT_MEM_PARAM = 0xa9b6;                        // a6xx.xml.h:9171
// per-fiber private memory (what ldp/stp address), in 512-byte units
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM__MASK = 0x000000ff;   // a6xx.xml.h:9172
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM__SHIFT = 0;           // a6xx.xml.h:9173
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM__SCALE = 9;           // a6xx.xml.h:9174  the encoder's val >> 9
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__MASK = 0xff000000;  // a6xx.xml.h:9179
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__SHIFT = 24;    // a6xx.xml.h:9180
constexpr uint32_t A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(uint32_t bytes) {
    return field(bytes >> A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM__SCALE, A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM__SHIFT,
                 A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM__MASK);
}
constexpr uint32_t A6XX_SP_CS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD(uint32_t calls4) {
    return field(calls4, A6XX_SP_CS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__SHIFT, A6XX_SP_CS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_CS_PVT_MEM_ADDR = 0xa9b7;                         // a6xx.xml.h:9187  64-bit

inline constexpr uint32_t REG_A6XX_SP_CS_PVT_MEM_SIZE = 0xa9b9;                         // a6xx.xml.h:9190
// per-SP private memory in 4 KiB units: per-fiber size times fibers_per_sp, rounded up to 4 KiB; the buffer holds
// num_sp_cores of these
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__MASK = 0x0003ffff;   // a6xx.xml.h:9191
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SHIFT = 0;           // a6xx.xml.h:9192
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SCALE = 12;          // a6xx.xml.h:9193  the encoder's val >> 12
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_SIZE_PERWAVEMEMLAYOUT = 0x80000000;        // a6xx.xml.h:9198  ir3's pvtmem_per_wave
constexpr uint32_t A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(uint32_t bytes) {
    return field(bytes >> A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SCALE, A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SHIFT,
                 A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_CS_TEX_COUNT = 0xa9ba;                            // a6xx.xml.h:9201
inline constexpr uint32_t REG_A6XX_SP_CS_CONFIG = 0xa9bb;                               // a6xx.xml.h:9204
inline constexpr uint32_t A6XX_SP_CS_CONFIG_BINDLESS_TEX = 0x00000001;                  // a6xx.xml.h:9205
inline constexpr uint32_t A6XX_SP_CS_CONFIG_BINDLESS_SAMP = 0x00000002;                 // a6xx.xml.h:9206
inline constexpr uint32_t A6XX_SP_CS_CONFIG_BINDLESS_IBO = 0x00000004;                  // a6xx.xml.h:9207
inline constexpr uint32_t A6XX_SP_CS_CONFIG_BINDLESS_UBO = 0x00000008;                  // a6xx.xml.h:9208
inline constexpr uint32_t A6XX_SP_CS_CONFIG_ENABLED = 0x00000100;                       // a6xx.xml.h:9209
inline constexpr uint32_t A6XX_SP_CS_CONFIG_NTEX__MASK = 0x0001fe00;                    // a6xx.xml.h:9210
inline constexpr uint32_t A6XX_SP_CS_CONFIG_NTEX__SHIFT = 9;                            // a6xx.xml.h:9211
inline constexpr uint32_t A6XX_SP_CS_CONFIG_NSAMP__MASK = 0x003e0000;                   // a6xx.xml.h:9216
inline constexpr uint32_t A6XX_SP_CS_CONFIG_NSAMP__SHIFT = 17;                          // a6xx.xml.h:9217
inline constexpr uint32_t A6XX_SP_CS_CONFIG_NIBO__MASK = 0x1fc00000;                    // a6xx.xml.h:9222
inline constexpr uint32_t A6XX_SP_CS_CONFIG_NIBO__SHIFT = 22;                           // a6xx.xml.h:9223
constexpr uint32_t A6XX_SP_CS_CONFIG_NTEX(uint32_t n) { return field(n, A6XX_SP_CS_CONFIG_NTEX__SHIFT, A6XX_SP_CS_CONFIG_NTEX__MASK); }
constexpr uint32_t A6XX_SP_CS_CONFIG_NSAMP(uint32_t n) { return field(n, A6XX_SP_CS_CONFIG_NSAMP__SHIFT, A6XX_SP_CS_CONFIG_NSAMP__MASK); }
constexpr uint32_t A6XX_SP_CS_CONFIG_NIBO(uint32_t n) { return field(n, A6XX_SP_CS_CONFIG_NIBO__SHIFT, A6XX_SP_CS_CONFIG_NIBO__MASK); }

inline constexpr uint32_t REG_A6XX_SP_CS_INSTRLEN = 0xa9bc;                             // a6xx.xml.h:9230  ir3's instrlen, in instruction groups

inline constexpr uint32_t REG_A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET = 0xa9bd;              // a6xx.xml.h:9233
// the per-SP private memory size again, in 2 KiB units: the hardware call stack lives after the private memory
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET__MASK = 0x0007ffff; // a6xx.xml.h:9234
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SHIFT = 0;         // a6xx.xml.h:9235
inline constexpr uint32_t A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SCALE = 11;        // a6xx.xml.h:9236  the encoder's val >> 11
constexpr uint32_t A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET(uint32_t bytes) {
    return field(bytes >> A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SCALE, A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SHIFT,
                 A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET__MASK);
}

// New in gen4 (has_lpac): the SP's own copy of HLSQ_CS_CNTL_0/1, written with the same values
inline constexpr uint32_t REG_A6XX_SP_CS_CNTL_0 = 0xa9c2;                               // a6xx.xml.h:9249
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_WGIDCONSTID__MASK = 0x000000ff;             // a6xx.xml.h:9250
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_WGIDCONSTID__SHIFT = 0;                     // a6xx.xml.h:9251
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_WGSIZECONSTID__MASK = 0x0000ff00;           // a6xx.xml.h:9256
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_WGSIZECONSTID__SHIFT = 8;                   // a6xx.xml.h:9257
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_WGOFFSETCONSTID__MASK = 0x00ff0000;         // a6xx.xml.h:9262
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_WGOFFSETCONSTID__SHIFT = 16;                // a6xx.xml.h:9263
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_LOCALIDREGID__MASK = 0xff000000;            // a6xx.xml.h:9268
inline constexpr uint32_t A6XX_SP_CS_CNTL_0_LOCALIDREGID__SHIFT = 24;                   // a6xx.xml.h:9269
constexpr uint32_t A6XX_SP_CS_CNTL_0_WGIDCONSTID(uint32_t regid) {
    return field(regid, A6XX_SP_CS_CNTL_0_WGIDCONSTID__SHIFT, A6XX_SP_CS_CNTL_0_WGIDCONSTID__MASK);
}
constexpr uint32_t A6XX_SP_CS_CNTL_0_WGSIZECONSTID(uint32_t regid) {
    return field(regid, A6XX_SP_CS_CNTL_0_WGSIZECONSTID__SHIFT, A6XX_SP_CS_CNTL_0_WGSIZECONSTID__MASK);
}
constexpr uint32_t A6XX_SP_CS_CNTL_0_WGOFFSETCONSTID(uint32_t regid) {
    return field(regid, A6XX_SP_CS_CNTL_0_WGOFFSETCONSTID__SHIFT, A6XX_SP_CS_CNTL_0_WGOFFSETCONSTID__MASK);
}
constexpr uint32_t A6XX_SP_CS_CNTL_0_LOCALIDREGID(uint32_t regid) {
    return field(regid, A6XX_SP_CS_CNTL_0_LOCALIDREGID__SHIFT, A6XX_SP_CS_CNTL_0_LOCALIDREGID__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_CS_CNTL_1 = 0xa9c3;                               // a6xx.xml.h:9276
inline constexpr uint32_t A6XX_SP_CS_CNTL_1_LINEARLOCALIDREGID__MASK = 0x000000ff;      // a6xx.xml.h:9277
inline constexpr uint32_t A6XX_SP_CS_CNTL_1_LINEARLOCALIDREGID__SHIFT = 0;              // a6xx.xml.h:9278
inline constexpr uint32_t A6XX_SP_CS_CNTL_1_SINGLE_SP_CORE = 0x00000100;                // a6xx.xml.h:9283
inline constexpr uint32_t A6XX_SP_CS_CNTL_1_THREADSIZE__MASK = 0x00000200;              // a6xx.xml.h:9284  must match SP_CS_CTRL_REG0
inline constexpr uint32_t A6XX_SP_CS_CNTL_1_THREADSIZE__SHIFT = 9;                      // a6xx.xml.h:9285
inline constexpr uint32_t A6XX_SP_CS_CNTL_1_THREADSIZE_SCALAR = 0x00000400;             // a6xx.xml.h:9290
constexpr uint32_t A6XX_SP_CS_CNTL_1_LINEARLOCALIDREGID(uint32_t regid) {
    return field(regid, A6XX_SP_CS_CNTL_1_LINEARLOCALIDREGID__SHIFT, A6XX_SP_CS_CNTL_1_LINEARLOCALIDREGID__MASK);
}
constexpr uint32_t A6XX_SP_CS_CNTL_1_THREADSIZE(a6xx_threadsize v) {
    return field(v, A6XX_SP_CS_CNTL_1_THREADSIZE__SHIFT, A6XX_SP_CS_CNTL_1_THREADSIZE__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_CS_TEX_SAMP = 0xa9e2;                             // a6xx.xml.h:9318  64-bit
inline constexpr uint32_t REG_A6XX_SP_CS_TEX_CONST = 0xa9e6;                            // a6xx.xml.h:9324  64-bit
inline constexpr uint32_t REG_A6XX_SP_CS_IBO = 0xa9f2;                                  // a6xx.xml.h:9363  64-bit: the IBO descriptors' address
inline constexpr uint32_t REG_A6XX_SP_CS_IBO_COUNT = 0xaa00;                            // a6xx.xml.h:9366  how many (7 bits)

// --- HLSQ registers ------------------------------------------------------------------------------------------------------

// Gallium writes this on chips without double threadsize, where the compute threadsize is read from here; this chip
// has double threadsize, so it is not needed on it.
inline constexpr uint32_t REG_A6XX_HLSQ_FS_CNTL_0 = 0xb980;                             // a6xx.xml.h:10375
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_0_THREADSIZE__MASK = 0x00000001;            // a6xx.xml.h:10376
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_0_THREADSIZE__SHIFT = 0;                    // a6xx.xml.h:10377
constexpr uint32_t A6XX_HLSQ_FS_CNTL_0_THREADSIZE(a6xx_threadsize v) {
    return field(v, A6XX_HLSQ_FS_CNTL_0_THREADSIZE__SHIFT, A6XX_HLSQ_FS_CNTL_0_THREADSIZE__MASK);
}

inline constexpr uint32_t REG_A6XX_HLSQ_CS_CNTL = 0xb987;                               // a6xx.xml.h:10499
// the constant length in vec4, held in units of 4 vec4: the encoder divides by 4 (Mesa asserts it is a multiple)
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_CONSTLEN__MASK = 0x000000ff;                // a6xx.xml.h:10500
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_CONSTLEN__SHIFT = 0;                        // a6xx.xml.h:10501
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_CONSTLEN__SCALE = 2;                        // a6xx.xml.h:10502  the encoder's val >> 2
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_ENABLED = 0x00000100;                       // a6xx.xml.h:10507
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_READ_IMM_SHARED_CONSTS = 0x00000200;        // a6xx.xml.h:10508
constexpr uint32_t A6XX_HLSQ_CS_CNTL_CONSTLEN(uint32_t vec4s) {
    return field(vec4s >> A6XX_HLSQ_CS_CNTL_CONSTLEN__SCALE, A6XX_HLSQ_CS_CNTL_CONSTLEN__SHIFT, A6XX_HLSQ_CS_CNTL_CONSTLEN__MASK);
}

inline constexpr uint32_t REG_A6XX_HLSQ_CS_NDRANGE_0 = 0xb990;                          // a6xx.xml.h:10644
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__MASK = 0x00000003;          // a6xx.xml.h:10645  1 to 3; Mesa's XML has what it does as a TODO
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__SHIFT = 0;                  // a6xx.xml.h:10646
// the local sizes are written minus one
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__MASK = 0x00000ffc;         // a6xx.xml.h:10651
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__SHIFT = 2;                 // a6xx.xml.h:10652
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__MASK = 0x003ff000;         // a6xx.xml.h:10657
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__SHIFT = 12;                // a6xx.xml.h:10658
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__MASK = 0xffc00000;         // a6xx.xml.h:10663
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__SHIFT = 22;                // a6xx.xml.h:10664
constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM(uint32_t dim) {
    return field(dim, A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__SHIFT, A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX(uint32_t minusOne) {
    return field(minusOne, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__SHIFT, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY(uint32_t minusOne) {
    return field(minusOne, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__SHIFT, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(uint32_t minusOne) {
    return field(minusOne, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__SHIFT, A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ__MASK);
}
// global size = local size times the number of groups, per axis; the offsets are 0
inline constexpr uint32_t REG_A6XX_HLSQ_CS_NDRANGE_1 = 0xb991;                          // a6xx.xml.h:10671  GLOBALSIZE_X, the whole word
inline constexpr uint32_t REG_A6XX_HLSQ_CS_NDRANGE_2 = 0xb992;                          // a6xx.xml.h:10680  GLOBALOFF_X
inline constexpr uint32_t REG_A6XX_HLSQ_CS_NDRANGE_3 = 0xb993;                          // a6xx.xml.h:10689  GLOBALSIZE_Y
inline constexpr uint32_t REG_A6XX_HLSQ_CS_NDRANGE_4 = 0xb994;                          // a6xx.xml.h:10698  GLOBALOFF_Y
inline constexpr uint32_t REG_A6XX_HLSQ_CS_NDRANGE_5 = 0xb995;                          // a6xx.xml.h:10707  GLOBALSIZE_Z
inline constexpr uint32_t REG_A6XX_HLSQ_CS_NDRANGE_6 = 0xb996;                          // a6xx.xml.h:10716  GLOBALOFF_Z
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_1_GLOBALSIZE_X__MASK = 0xffffffff;       // a6xx.xml.h:10672
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_2_GLOBALOFF_X__MASK = 0xffffffff;        // a6xx.xml.h:10681
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_3_GLOBALSIZE_Y__MASK = 0xffffffff;       // a6xx.xml.h:10690
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_4_GLOBALOFF_Y__MASK = 0xffffffff;        // a6xx.xml.h:10699
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_5_GLOBALSIZE_Z__MASK = 0xffffffff;       // a6xx.xml.h:10708
inline constexpr uint32_t A6XX_HLSQ_CS_NDRANGE_6_GLOBALOFF_Z__MASK = 0xffffffff;        // a6xx.xml.h:10717

inline constexpr uint32_t REG_A6XX_HLSQ_CS_CNTL_0 = 0xb997;                             // a6xx.xml.h:10725
// where the shader gets its ids: the work group id and size and offset are constants (vec3, high registers), the local
// id is a register; kRegidNone for the ones it does not read (ir3 reports which through its sysvals)
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID__MASK = 0x000000ff;           // a6xx.xml.h:10726
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID__SHIFT = 0;                   // a6xx.xml.h:10727
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGSIZECONSTID__MASK = 0x0000ff00;         // a6xx.xml.h:10732
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGSIZECONSTID__SHIFT = 8;                 // a6xx.xml.h:10733
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGOFFSETCONSTID__MASK = 0x00ff0000;       // a6xx.xml.h:10738
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGOFFSETCONSTID__SHIFT = 16;              // a6xx.xml.h:10739
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID__MASK = 0xff000000;          // a6xx.xml.h:10744
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID__SHIFT = 24;                 // a6xx.xml.h:10745
constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID(uint32_t regid) {
    return field(regid, A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID__SHIFT, A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGSIZECONSTID(uint32_t regid) {
    return field(regid, A6XX_HLSQ_CS_CNTL_0_WGSIZECONSTID__SHIFT, A6XX_HLSQ_CS_CNTL_0_WGSIZECONSTID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_WGOFFSETCONSTID(uint32_t regid) {
    return field(regid, A6XX_HLSQ_CS_CNTL_0_WGOFFSETCONSTID__SHIFT, A6XX_HLSQ_CS_CNTL_0_WGOFFSETCONSTID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID(uint32_t regid) {
    return field(regid, A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID__SHIFT, A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID__MASK);
}

inline constexpr uint32_t REG_A6XX_HLSQ_CS_CNTL_1 = 0xb998;                             // a6xx.xml.h:10752
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_LINEARLOCALIDREGID__MASK = 0x000000ff;    // a6xx.xml.h:10753  gl_LocalInvocationIndex
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_LINEARLOCALIDREGID__SHIFT = 0;            // a6xx.xml.h:10754
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_SINGLE_SP_CORE = 0x00000100;              // a6xx.xml.h:10759
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_THREADSIZE__MASK = 0x00000200;            // a6xx.xml.h:10760  must match SP_CS_CTRL_REG0
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_THREADSIZE__SHIFT = 9;                    // a6xx.xml.h:10761
inline constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_THREADSIZE_SCALAR = 0x00000400;           // a6xx.xml.h:10766  one thread per wave
constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_LINEARLOCALIDREGID(uint32_t regid) {
    return field(regid, A6XX_HLSQ_CS_CNTL_1_LINEARLOCALIDREGID__SHIFT, A6XX_HLSQ_CS_CNTL_1_LINEARLOCALIDREGID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_CNTL_1_THREADSIZE(a6xx_threadsize v) {
    return field(v, A6XX_HLSQ_CS_CNTL_1_THREADSIZE__SHIFT, A6XX_HLSQ_CS_CNTL_1_THREADSIZE__MASK);
}

// both Mesa emitters write 1 to each; the XML says nothing about them (the Vulkan blob does not use them)
inline constexpr uint32_t REG_A6XX_HLSQ_CS_KERNEL_GROUP_X = 0xb999;                     // a6xx.xml.h:10769
inline constexpr uint32_t REG_A6XX_HLSQ_CS_KERNEL_GROUP_Y = 0xb99a;                     // a6xx.xml.h:10772
inline constexpr uint32_t REG_A6XX_HLSQ_CS_KERNEL_GROUP_Z = 0xb99b;                     // a6xx.xml.h:10775

// Gen4 (has_lpac) only: the HLSQ's copy of SP_CS_CTRL_REG1, same layout. Gallium gives it the same shared size;
// computerator writes SHARED_SIZE(1) and only the mode.
inline constexpr uint32_t REG_A6XX_HLSQ_CS_CTRL_REG1 = 0xb9d0;                          // a6xx.xml.h:10944
inline constexpr uint32_t A6XX_HLSQ_CS_CTRL_REG1_SHARED_SIZE__MASK = 0x0000001f;        // a6xx.xml.h:10945
inline constexpr uint32_t A6XX_HLSQ_CS_CTRL_REG1_SHARED_SIZE__SHIFT = 0;                // a6xx.xml.h:10946
inline constexpr uint32_t A6XX_HLSQ_CS_CTRL_REG1_CONSTANTRAMMODE__MASK = 0x00000060;    // a6xx.xml.h:10951
inline constexpr uint32_t A6XX_HLSQ_CS_CTRL_REG1_CONSTANTRAMMODE__SHIFT = 5;            // a6xx.xml.h:10952
constexpr uint32_t A6XX_HLSQ_CS_CTRL_REG1_SHARED_SIZE(uint32_t n) {
    return field(n, A6XX_HLSQ_CS_CTRL_REG1_SHARED_SIZE__SHIFT, A6XX_HLSQ_CS_CTRL_REG1_SHARED_SIZE__MASK);
}
constexpr uint32_t A6XX_HLSQ_CS_CTRL_REG1_CONSTANTRAMMODE(a6xx_const_ram_mode v) {
    return field(v, A6XX_HLSQ_CS_CTRL_REG1_CONSTANTRAMMODE__SHIFT, A6XX_HLSQ_CS_CTRL_REG1_CONSTANTRAMMODE__MASK);
}

// Drops the CP_LOAD_STATE6 loads still queued for the stages whose bits are set: written before a new program so that
// nothing of the previous one is loaded late. Both emitters set every stage bit and GFX_IBO; Gallium adds CS_IBO.
inline constexpr uint32_t REG_A6XX_HLSQ_INVALIDATE_CMD = 0xbb08;                        // a6xx.xml.h:10992
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_VS_STATE = 0x00000001;               // a6xx.xml.h:10993
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_HS_STATE = 0x00000002;               // a6xx.xml.h:10994
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_DS_STATE = 0x00000004;               // a6xx.xml.h:10995
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_GS_STATE = 0x00000008;               // a6xx.xml.h:10996
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_FS_STATE = 0x00000010;               // a6xx.xml.h:10997
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_CS_STATE = 0x00000020;               // a6xx.xml.h:10998
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_CS_IBO = 0x00000040;                 // a6xx.xml.h:10999
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_GFX_IBO = 0x00000080;                // a6xx.xml.h:11000
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_CS_SHARED_CONST = 0x00080000;        // a6xx.xml.h:11001
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_GFX_SHARED_CONST = 0x00000100;       // a6xx.xml.h:11002
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_CS_BINDLESS__MASK = 0x00003e00;      // a6xx.xml.h:11003
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_CS_BINDLESS__SHIFT = 9;              // a6xx.xml.h:11004
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_GFX_BINDLESS__MASK = 0x0007c000;     // a6xx.xml.h:11009
inline constexpr uint32_t A6XX_HLSQ_INVALIDATE_CMD_GFX_BINDLESS__SHIFT = 14;            // a6xx.xml.h:11010

// --- packets --------------------------------------------------------------------------------------------------------------

// CP_LOAD_STATE6_FRAG, 3 words (indirect) or 3 + the data (direct). What NUM_UNIT counts depends on STATE_TYPE.
inline constexpr uint32_t CP_LOAD_STATE6_0_DST_OFF__MASK = 0x00003fff;                  // adreno_pm4.xml.h:705  in the same units as NUM_UNIT
inline constexpr uint32_t CP_LOAD_STATE6_0_DST_OFF__SHIFT = 0;                          // adreno_pm4.xml.h:706
inline constexpr uint32_t CP_LOAD_STATE6_0_STATE_TYPE__MASK = 0x0000c000;               // adreno_pm4.xml.h:711
inline constexpr uint32_t CP_LOAD_STATE6_0_STATE_TYPE__SHIFT = 14;                      // adreno_pm4.xml.h:712
inline constexpr uint32_t CP_LOAD_STATE6_0_STATE_SRC__MASK = 0x00030000;                // adreno_pm4.xml.h:717
inline constexpr uint32_t CP_LOAD_STATE6_0_STATE_SRC__SHIFT = 16;                       // adreno_pm4.xml.h:718
inline constexpr uint32_t CP_LOAD_STATE6_0_STATE_BLOCK__MASK = 0x003c0000;              // adreno_pm4.xml.h:723
inline constexpr uint32_t CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT = 18;                     // adreno_pm4.xml.h:724
inline constexpr uint32_t CP_LOAD_STATE6_0_NUM_UNIT__MASK = 0xffc00000;                 // adreno_pm4.xml.h:729
inline constexpr uint32_t CP_LOAD_STATE6_0_NUM_UNIT__SHIFT = 22;                        // adreno_pm4.xml.h:730
constexpr uint32_t CP_LOAD_STATE6_0_DST_OFF(uint32_t v) { return field(v, CP_LOAD_STATE6_0_DST_OFF__SHIFT, CP_LOAD_STATE6_0_DST_OFF__MASK); }
constexpr uint32_t CP_LOAD_STATE6_0_STATE_TYPE(a6xx_state_type v) {
    return field(v, CP_LOAD_STATE6_0_STATE_TYPE__SHIFT, CP_LOAD_STATE6_0_STATE_TYPE__MASK);
}
constexpr uint32_t CP_LOAD_STATE6_0_STATE_SRC(a6xx_state_src v) {
    return field(v, CP_LOAD_STATE6_0_STATE_SRC__SHIFT, CP_LOAD_STATE6_0_STATE_SRC__MASK);
}
constexpr uint32_t CP_LOAD_STATE6_0_STATE_BLOCK(a6xx_state_block v) {
    return field(v, CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT, CP_LOAD_STATE6_0_STATE_BLOCK__MASK);
}
constexpr uint32_t CP_LOAD_STATE6_0_NUM_UNIT(uint32_t v) { return field(v, CP_LOAD_STATE6_0_NUM_UNIT__SHIFT, CP_LOAD_STATE6_0_NUM_UNIT__MASK); }
// words 1 and 2: the source address (SS6_INDIRECT), low word with its two low bits dropped (dword aligned), high word
inline constexpr uint32_t CP_LOAD_STATE6_1_EXT_SRC_ADDR__MASK = 0xfffffffc;             // adreno_pm4.xml.h:738
inline constexpr uint32_t CP_LOAD_STATE6_1_EXT_SRC_ADDR__SHIFT = 2;                     // adreno_pm4.xml.h:739
inline constexpr uint32_t CP_LOAD_STATE6_1_EXT_SRC_ADDR__SCALE = 2;                     // adreno_pm4.xml.h:740  the encoder's val >> 2
inline constexpr uint32_t CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI__MASK = 0xffffffff;          // adreno_pm4.xml.h:748
constexpr uint32_t CP_LOAD_STATE6_1_EXT_SRC_ADDR(uint32_t lo) {
    return field(lo >> CP_LOAD_STATE6_1_EXT_SRC_ADDR__SCALE, CP_LOAD_STATE6_1_EXT_SRC_ADDR__SHIFT, CP_LOAD_STATE6_1_EXT_SRC_ADDR__MASK);
}
constexpr uint32_t CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(uint32_t hi) { return hi; }

// CP_EXEC_CS, 4 words: word 0 has no fields (Mesa writes 0), then the number of work groups per axis
inline constexpr uint32_t CP_EXEC_CS_1_NGROUPS_X__MASK = 0xffffffff;                    // adreno_pm4.xml.h:2511
inline constexpr uint32_t CP_EXEC_CS_2_NGROUPS_Y__MASK = 0xffffffff;                    // adreno_pm4.xml.h:2520
inline constexpr uint32_t CP_EXEC_CS_3_NGROUPS_Z__MASK = 0xffffffff;                    // adreno_pm4.xml.h:2529

// CP_EVENT_WRITE, 1 word (the event) or 4 (the event, then an address and the value written there when it completes)
inline constexpr uint32_t CP_EVENT_WRITE_0_EVENT__MASK = 0x000000ff;                    // adreno_pm4.xml.h:2329
inline constexpr uint32_t CP_EVENT_WRITE_0_EVENT__SHIFT = 0;                            // adreno_pm4.xml.h:2330
// With TIMESTAMP set the GPU writes back its timestamp instead of the packet's value. Measured on the phone on
// 28 October (hakios-metal-probe --bisect): with the bit set on the two end-of-dispatch events (RB_DONE_TS,
// CACHE_FLUSH_TS) the CP_WAIT_REG_MEM and CP_WAIT_MEM_GTE that poll for the seqno never saw it and the CP spun for
// ever, every dispatch; with it clear the whole tail completes in 41 us. Gallium sets the bit on every 4-word event,
// computerator never does: computerator's form is the one that works here. eventWrite0 leaves it clear.
inline constexpr uint32_t CP_EVENT_WRITE_0_TIMESTAMP = 0x40000000;                      // adreno_pm4.xml.h:2335
inline constexpr uint32_t CP_EVENT_WRITE_0_IRQ = 0x80000000;                            // adreno_pm4.xml.h:2336
inline constexpr uint32_t CP_EVENT_WRITE_1_ADDR_0_LO__MASK = 0xffffffff;                // adreno_pm4.xml.h:2340
inline constexpr uint32_t CP_EVENT_WRITE_2_ADDR_0_HI__MASK = 0xffffffff;                // adreno_pm4.xml.h:2349
constexpr uint32_t CP_EVENT_WRITE_0_EVENT(vgt_event_type v) { return field(v, CP_EVENT_WRITE_0_EVENT__SHIFT, CP_EVENT_WRITE_0_EVENT__MASK); }

// CP_SET_MARKER, 1 word: tells the command processor the mode, for its save and restore (RM6_COMPUTE before a dispatch)
inline constexpr uint32_t A6XX_CP_SET_MARKER_0_MODE__MASK = 0x0000000f;                 // adreno_pm4.xml.h:2616
inline constexpr uint32_t A6XX_CP_SET_MARKER_0_MODE__SHIFT = 0;                         // adreno_pm4.xml.h:2617
inline constexpr uint32_t A6XX_CP_SET_MARKER_0_USES_GMEM = 0x00000010;                  // adreno_pm4.xml.h:2622
inline constexpr uint32_t A6XX_CP_SET_MARKER_0_MARKER_MODE__MASK = 0x00000100;          // adreno_pm4.xml.h:2610  set: the low bits mean IFPC instead
inline constexpr uint32_t A6XX_CP_SET_MARKER_0_MARKER_MODE__SHIFT = 8;                  // adreno_pm4.xml.h:2611
constexpr uint32_t A6XX_CP_SET_MARKER_0_MODE(a6xx_marker v) { return field(v, A6XX_CP_SET_MARKER_0_MODE__SHIFT, A6XX_CP_SET_MARKER_0_MODE__MASK); }

// CP_WAIT_REG_MEM, 6 words: the command processor loops until (memory or register) & MASK compares FUNCTION with REF,
// pausing DELAY_LOOP_CYCLES between polls
inline constexpr uint32_t CP_WAIT_REG_MEM_0_FUNCTION__MASK = 0x00000007;                // adreno_pm4.xml.h:2073
inline constexpr uint32_t CP_WAIT_REG_MEM_0_FUNCTION__SHIFT = 0;                        // adreno_pm4.xml.h:2074
inline constexpr uint32_t CP_WAIT_REG_MEM_0_SIGNED_COMPARE = 0x00000008;                // adreno_pm4.xml.h:2079
inline constexpr uint32_t CP_WAIT_REG_MEM_0_POLL__MASK = 0x00000030;                    // adreno_pm4.xml.h:2080
inline constexpr uint32_t CP_WAIT_REG_MEM_0_POLL__SHIFT = 4;                            // adreno_pm4.xml.h:2081
inline constexpr uint32_t CP_WAIT_REG_MEM_0_WRITE_MEMORY = 0x00000100;                  // adreno_pm4.xml.h:2086
inline constexpr uint32_t CP_WAIT_REG_MEM_1_POLL_ADDR_LO__MASK = 0xffffffff;            // adreno_pm4.xml.h:2090
inline constexpr uint32_t CP_WAIT_REG_MEM_2_POLL_ADDR_HI__MASK = 0xffffffff;            // adreno_pm4.xml.h:2099
inline constexpr uint32_t CP_WAIT_REG_MEM_3_REF__MASK = 0xffffffff;                     // adreno_pm4.xml.h:2108
inline constexpr uint32_t CP_WAIT_REG_MEM_4_MASK__MASK = 0xffffffff;                    // adreno_pm4.xml.h:2117
inline constexpr uint32_t CP_WAIT_REG_MEM_5_DELAY_LOOP_CYCLES__MASK = 0xffffffff;       // adreno_pm4.xml.h:2126
constexpr uint32_t CP_WAIT_REG_MEM_0_FUNCTION(cp_cond_function v) {
    return field(v, CP_WAIT_REG_MEM_0_FUNCTION__SHIFT, CP_WAIT_REG_MEM_0_FUNCTION__MASK);
}
constexpr uint32_t CP_WAIT_REG_MEM_0_POLL(poll_memory_type v) { return field(v, CP_WAIT_REG_MEM_0_POLL__SHIFT, CP_WAIT_REG_MEM_0_POLL__MASK); }

// CP_WAIT_MEM_GTE, 4 words: wait until the memory word is >= REF, signed. Word 0 is reserved (Mesa writes 0).
inline constexpr uint32_t CP_WAIT_MEM_GTE_0_RESERVED__MASK = 0xffffffff;                // adreno_pm4.xml.h:2037
inline constexpr uint32_t CP_WAIT_MEM_GTE_1_POLL_ADDR_LO__MASK = 0xffffffff;            // adreno_pm4.xml.h:2046
inline constexpr uint32_t CP_WAIT_MEM_GTE_2_POLL_ADDR_HI__MASK = 0xffffffff;            // adreno_pm4.xml.h:2055
inline constexpr uint32_t CP_WAIT_MEM_GTE_3_REF__MASK = 0xffffffff;                     // adreno_pm4.xml.h:2064

// CP_REG_TO_MEM, 3 words: copy max(CNT, 1) registers from REG to the address (64B: as 64-bit values)
inline constexpr uint32_t CP_REG_TO_MEM_0_REG__MASK = 0x0003ffff;                       // adreno_pm4.xml.h:1608
inline constexpr uint32_t CP_REG_TO_MEM_0_REG__SHIFT = 0;                               // adreno_pm4.xml.h:1609
inline constexpr uint32_t CP_REG_TO_MEM_0_CNT__MASK = 0x3ffc0000;                       // adreno_pm4.xml.h:1614
inline constexpr uint32_t CP_REG_TO_MEM_0_CNT__SHIFT = 18;                              // adreno_pm4.xml.h:1615
inline constexpr uint32_t CP_REG_TO_MEM_0_64B = 0x40000000;                             // adreno_pm4.xml.h:1620
inline constexpr uint32_t CP_REG_TO_MEM_0_ACCUMULATE = 0x80000000;                      // adreno_pm4.xml.h:1621
inline constexpr uint32_t CP_REG_TO_MEM_1_DEST__MASK = 0xffffffff;                      // adreno_pm4.xml.h:1625
inline constexpr uint32_t CP_REG_TO_MEM_2_DEST_HI__MASK = 0xffffffff;                   // adreno_pm4.xml.h:1634
constexpr uint32_t CP_REG_TO_MEM_0_REG(uint32_t reg) { return field(reg, CP_REG_TO_MEM_0_REG__SHIFT, CP_REG_TO_MEM_0_REG__MASK); }
constexpr uint32_t CP_REG_TO_MEM_0_CNT(uint32_t n) { return field(n, CP_REG_TO_MEM_0_CNT__SHIFT, CP_REG_TO_MEM_0_CNT__MASK); }

// --- the texture descriptor: a buffer view (an IBO), a 2D texture (the texture table), a 2D image (an IBO) -----------------
// 16 dwords, REG_A6XX_TEX_CONST_0 to _15; a buffer sets 0, 1, 2, 4, 5 and leaves the rest 0; a 2D view sets 0 to 6
// (fdl6_view_init, fd6_view.c:232-250; dwords 7 to 10 are UBWC, which a linear texture has none of).

inline constexpr uint32_t REG_A6XX_TEX_CONST_0 = 0x0;                                   // a6xx.xml.h:11294
inline constexpr uint32_t REG_A6XX_TEX_CONST_1 = 0x1;                                   // a6xx.xml.h:11354
inline constexpr uint32_t REG_A6XX_TEX_CONST_2 = 0x2;                                   // a6xx.xml.h:11370
inline constexpr uint32_t REG_A6XX_TEX_CONST_3 = 0x3;                                   // a6xx.xml.h:11403
inline constexpr uint32_t REG_A6XX_TEX_CONST_4 = 0x4;                                   // a6xx.xml.h:11422
inline constexpr uint32_t REG_A6XX_TEX_CONST_5 = 0x5;                                   // a6xx.xml.h:11432
inline constexpr uint32_t REG_A6XX_TEX_CONST_6 = 0x6;                                   // a6xx.xml.h:11447
inline constexpr uint32_t REG_A6XX_TEX_CONST_15 = 0xf;                                  // a6xx.xml.h:11525
inline constexpr uint32_t kTexConstDwords = 16;                                         // not-in-headers: REG_A6XX_TEX_CONST_15 + 1; Mesa's A6XX_TEX_CONST_DWORDS

inline constexpr uint32_t A6XX_TEX_CONST_0_TILE_MODE__MASK = 0x00000003;                // a6xx.xml.h:11295
inline constexpr uint32_t A6XX_TEX_CONST_0_TILE_MODE__SHIFT = 0;                        // a6xx.xml.h:11296
inline constexpr uint32_t A6XX_TEX_CONST_0_SRGB = 0x00000004;                           // a6xx.xml.h:11301
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_X__MASK = 0x00000070;                   // a6xx.xml.h:11302
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_X__SHIFT = 4;                           // a6xx.xml.h:11303
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_Y__MASK = 0x00000380;                   // a6xx.xml.h:11308
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_Y__SHIFT = 7;                           // a6xx.xml.h:11309
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_Z__MASK = 0x00001c00;                   // a6xx.xml.h:11314
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_Z__SHIFT = 10;                          // a6xx.xml.h:11315
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_W__MASK = 0x0000e000;                   // a6xx.xml.h:11320
inline constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_W__SHIFT = 13;                          // a6xx.xml.h:11321
inline constexpr uint32_t A6XX_TEX_CONST_0_MIPLVLS__MASK = 0x000f0000;                  // a6xx.xml.h:11326  level count minus one
inline constexpr uint32_t A6XX_TEX_CONST_0_MIPLVLS__SHIFT = 16;                         // a6xx.xml.h:11327
inline constexpr uint32_t A6XX_TEX_CONST_0_CHROMA_MIDPOINT_X = 0x00010000;              // a6xx.xml.h:11332  YUV only; overlaps MIPLVLS
inline constexpr uint32_t A6XX_TEX_CONST_0_CHROMA_MIDPOINT_Y = 0x00040000;              // a6xx.xml.h:11333
inline constexpr uint32_t A6XX_TEX_CONST_0_SAMPLES__MASK = 0x00300000;                  // a6xx.xml.h:11334  log2 of the sample count (a3xx_msaa_samples)
inline constexpr uint32_t A6XX_TEX_CONST_0_SAMPLES__SHIFT = 20;                         // a6xx.xml.h:11335
inline constexpr uint32_t A6XX_TEX_CONST_0_FMT__MASK = 0x3fc00000;                      // a6xx.xml.h:11340
inline constexpr uint32_t A6XX_TEX_CONST_0_FMT__SHIFT = 22;                             // a6xx.xml.h:11341
inline constexpr uint32_t A6XX_TEX_CONST_0_SWAP__MASK = 0xc0000000;                     // a6xx.xml.h:11346  component swap, before border color; 0 = WZYX
inline constexpr uint32_t A6XX_TEX_CONST_0_SWAP__SHIFT = 30;                            // a6xx.xml.h:11347
constexpr uint32_t A6XX_TEX_CONST_0_TILE_MODE(a6xx_tile_mode v) { return field(v, A6XX_TEX_CONST_0_TILE_MODE__SHIFT, A6XX_TEX_CONST_0_TILE_MODE__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_X(a6xx_tex_swiz v) { return field(v, A6XX_TEX_CONST_0_SWIZ_X__SHIFT, A6XX_TEX_CONST_0_SWIZ_X__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_Y(a6xx_tex_swiz v) { return field(v, A6XX_TEX_CONST_0_SWIZ_Y__SHIFT, A6XX_TEX_CONST_0_SWIZ_Y__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_Z(a6xx_tex_swiz v) { return field(v, A6XX_TEX_CONST_0_SWIZ_Z__SHIFT, A6XX_TEX_CONST_0_SWIZ_Z__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_SWIZ_W(a6xx_tex_swiz v) { return field(v, A6XX_TEX_CONST_0_SWIZ_W__SHIFT, A6XX_TEX_CONST_0_SWIZ_W__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_MIPLVLS(uint32_t v) { return field(v, A6XX_TEX_CONST_0_MIPLVLS__SHIFT, A6XX_TEX_CONST_0_MIPLVLS__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_SAMPLES(a3xx_msaa_samples v) { return field(v, A6XX_TEX_CONST_0_SAMPLES__SHIFT, A6XX_TEX_CONST_0_SAMPLES__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_FMT(a6xx_format v) { return field(v, A6XX_TEX_CONST_0_FMT__SHIFT, A6XX_TEX_CONST_0_FMT__MASK); }
constexpr uint32_t A6XX_TEX_CONST_0_SWAP(a3xx_color_swap v) { return field(v, A6XX_TEX_CONST_0_SWAP__SHIFT, A6XX_TEX_CONST_0_SWAP__MASK); }

// a buffer's element count: its low 15 bits in WIDTH and the rest in HEIGHT (so 2^30 elements at most)
inline constexpr uint32_t A6XX_TEX_CONST_1_WIDTH__MASK = 0x00007fff;                    // a6xx.xml.h:11355
inline constexpr uint32_t A6XX_TEX_CONST_1_WIDTH__SHIFT = 0;                            // a6xx.xml.h:11356
inline constexpr uint32_t A6XX_TEX_CONST_1_HEIGHT__MASK = 0x3fff8000;                   // a6xx.xml.h:11361
inline constexpr uint32_t A6XX_TEX_CONST_1_HEIGHT__SHIFT = 15;                          // a6xx.xml.h:11362
constexpr uint32_t A6XX_TEX_CONST_1_WIDTH(uint32_t v) { return field(v, A6XX_TEX_CONST_1_WIDTH__SHIFT, A6XX_TEX_CONST_1_WIDTH__MASK); }
constexpr uint32_t A6XX_TEX_CONST_1_HEIGHT(uint32_t v) { return field(v, A6XX_TEX_CONST_1_HEIGHT__SHIFT, A6XX_TEX_CONST_1_HEIGHT__MASK); }

// For TYPE = A6XX_TEX_BUFFER the struct fields take the place of PITCH/PITCHALIGN (they overlap): STRUCTSIZETEXELS
// is "probably for D3D structured UAVs, normally 1" (the XML); STARTOFFSETTEXELS is how far into the first 64 bytes
// the buffer starts, in elements, which is how an address that is not 64-byte aligned is expressed.
// For a 2D texture PITCHALIGN is "minimum pitch (for mipmap levels): log2(pitchalign / 64)" (the XML): Mesa writes
// layout->pitchalign - 6, which for a texture it laid out itself is log2(bytes per pixel) (fd6_layout.c:194-201,
// fd6_view.c:244); the storage (IBO) view of the same image leaves it out (fd6_view.c:390-392).
inline constexpr uint32_t A6XX_TEX_CONST_2_PITCHALIGN__MASK = 0x0000000f;               // a6xx.xml.h:11383
inline constexpr uint32_t A6XX_TEX_CONST_2_PITCHALIGN__SHIFT = 0;                       // a6xx.xml.h:11384
inline constexpr uint32_t A6XX_TEX_CONST_2_STRUCTSIZETEXELS__MASK = 0x0000fff0;         // a6xx.xml.h:11371
inline constexpr uint32_t A6XX_TEX_CONST_2_STRUCTSIZETEXELS__SHIFT = 4;                 // a6xx.xml.h:11372
inline constexpr uint32_t A6XX_TEX_CONST_2_STARTOFFSETTEXELS__MASK = 0x003f0000;        // a6xx.xml.h:11377
inline constexpr uint32_t A6XX_TEX_CONST_2_STARTOFFSETTEXELS__SHIFT = 16;               // a6xx.xml.h:11378
inline constexpr uint32_t A6XX_TEX_CONST_2_PITCH__MASK = 0x1fffff80;                    // a6xx.xml.h:11389  in bytes
inline constexpr uint32_t A6XX_TEX_CONST_2_PITCH__SHIFT = 7;                            // a6xx.xml.h:11390
inline constexpr uint32_t A6XX_TEX_CONST_2_TYPE__MASK = 0xe0000000;                     // a6xx.xml.h:11395
inline constexpr uint32_t A6XX_TEX_CONST_2_TYPE__SHIFT = 29;                            // a6xx.xml.h:11396
constexpr uint32_t A6XX_TEX_CONST_2_PITCHALIGN(uint32_t log2Of64s) {
    return field(log2Of64s, A6XX_TEX_CONST_2_PITCHALIGN__SHIFT, A6XX_TEX_CONST_2_PITCHALIGN__MASK);
}
constexpr uint32_t A6XX_TEX_CONST_2_STRUCTSIZETEXELS(uint32_t v) {
    return field(v, A6XX_TEX_CONST_2_STRUCTSIZETEXELS__SHIFT, A6XX_TEX_CONST_2_STRUCTSIZETEXELS__MASK);
}
constexpr uint32_t A6XX_TEX_CONST_2_STARTOFFSETTEXELS(uint32_t v) {
    return field(v, A6XX_TEX_CONST_2_STARTOFFSETTEXELS__SHIFT, A6XX_TEX_CONST_2_STARTOFFSETTEXELS__MASK);
}
constexpr uint32_t A6XX_TEX_CONST_2_PITCH(uint32_t bytes) { return field(bytes, A6XX_TEX_CONST_2_PITCH__SHIFT, A6XX_TEX_CONST_2_PITCH__MASK); }
constexpr uint32_t A6XX_TEX_CONST_2_TYPE(a6xx_tex_type v) { return field(v, A6XX_TEX_CONST_2_TYPE__SHIFT, A6XX_TEX_CONST_2_TYPE__MASK); }

inline constexpr uint32_t A6XX_TEX_CONST_3_ARRAY_PITCH__MASK = 0x007fffff;              // a6xx.xml.h:11404  the layer size, in 4 KiB
inline constexpr uint32_t A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT = 0;                      // a6xx.xml.h:11405
inline constexpr uint32_t A6XX_TEX_CONST_3_ARRAY_PITCH__SCALE = 12;                     // a6xx.xml.h:11406  the encoder's val >> 12
inline constexpr uint32_t A6XX_TEX_CONST_3_MIN_LAYERSZ__MASK = 0x07800000;              // a6xx.xml.h:11411
inline constexpr uint32_t A6XX_TEX_CONST_3_MIN_LAYERSZ__SHIFT = 23;                     // a6xx.xml.h:11412
inline constexpr uint32_t A6XX_TEX_CONST_3_TILE_ALL = 0x08000000;                       // a6xx.xml.h:11418
inline constexpr uint32_t A6XX_TEX_CONST_3_FLAG = 0x10000000;                           // a6xx.xml.h:11419
constexpr uint32_t A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t bytes) {
    return field(bytes >> A6XX_TEX_CONST_3_ARRAY_PITCH__SCALE, A6XX_TEX_CONST_3_ARRAY_PITCH__SHIFT, A6XX_TEX_CONST_3_ARRAY_PITCH__MASK);
}

// the base address: bits 31:5 of the low word (so 32-byte aligned; the low 5 bits are dropped), 17 bits of the high
// word (a 49-bit address), and DEPTH above them
inline constexpr uint32_t A6XX_TEX_CONST_4_BASE_LO__MASK = 0xffffffe0;                  // a6xx.xml.h:11423
inline constexpr uint32_t A6XX_TEX_CONST_4_BASE_LO__SHIFT = 5;                          // a6xx.xml.h:11424
inline constexpr uint32_t A6XX_TEX_CONST_4_BASE_LO__SCALE = 5;                          // a6xx.xml.h:11425  the encoder's val >> 5
inline constexpr uint32_t A6XX_TEX_CONST_5_BASE_HI__MASK = 0x0001ffff;                  // a6xx.xml.h:11433
inline constexpr uint32_t A6XX_TEX_CONST_5_BASE_HI__SHIFT = 0;                          // a6xx.xml.h:11434
inline constexpr uint32_t A6XX_TEX_CONST_5_DEPTH__MASK = 0x3ffe0000;                    // a6xx.xml.h:11439
inline constexpr uint32_t A6XX_TEX_CONST_5_DEPTH__SHIFT = 17;                           // a6xx.xml.h:11440
constexpr uint32_t A6XX_TEX_CONST_4_BASE_LO(uint32_t lo) {
    return field(lo >> A6XX_TEX_CONST_4_BASE_LO__SCALE, A6XX_TEX_CONST_4_BASE_LO__SHIFT, A6XX_TEX_CONST_4_BASE_LO__MASK);
}
constexpr uint32_t A6XX_TEX_CONST_5_BASE_HI(uint32_t hi) { return field(hi, A6XX_TEX_CONST_5_BASE_HI__SHIFT, A6XX_TEX_CONST_5_BASE_HI__MASK); }
constexpr uint32_t A6XX_TEX_CONST_5_DEPTH(uint32_t v) { return field(v, A6XX_TEX_CONST_5_DEPTH__SHIFT, A6XX_TEX_CONST_5_DEPTH__MASK); }

// the lowest level of detail a sample may read, as unsigned 4.8 fixed point (Mesa's encoder is uint32_t(val * 256.0),
// not a shift, so there is no __SCALE for it); Gallium passes 0 (fd6_view.c:250), and so does every texture here
inline constexpr uint32_t A6XX_TEX_CONST_6_MIN_LOD_CLAMP__MASK = 0x00000fff;            // a6xx.xml.h:11448
inline constexpr uint32_t A6XX_TEX_CONST_6_MIN_LOD_CLAMP__SHIFT = 0;                    // a6xx.xml.h:11449
constexpr uint32_t A6XX_TEX_CONST_6_MIN_LOD_CLAMP(uint32_t fixed88) {
    return field(fixed88, A6XX_TEX_CONST_6_MIN_LOD_CLAMP__SHIFT, A6XX_TEX_CONST_6_MIN_LOD_CLAMP__MASK);
}

struct TexConst {
    uint32_t dword[kTexConstDwords] = {};
};

// A raw buffer as an IBO: `elements` of `elementBytes` each (4 for FMT6_32_UINT) at `iova`. Laid out as turnip's
// storage buffer descriptor (tu_descriptor_set.cc) and fdl6_buffer_view_init do it: the base is the address rounded
// down to 64 bytes and the remainder goes in STARTOFFSETTEXELS, so any element-aligned address works. For a 64-byte
// aligned address this is exactly computerator's cs_ibo_emit descriptor (which writes the address straight in).
// No swizzle (every component reads X), as both of those write it.
constexpr TexConst bufferIbo(uint64_t iova, uint32_t elements, a6xx_format format = FMT6_32_UINT, uint32_t elementBytes = 4) {
    TexConst d{};
    uint64_t base = iova & ~uint64_t(0x3f);
    uint32_t startTexels = uint32_t(iova & 0x3f) / elementBytes;
    d.dword[0] = A6XX_TEX_CONST_0_TILE_MODE(TILE6_LINEAR) | A6XX_TEX_CONST_0_FMT(format);
    d.dword[1] = A6XX_TEX_CONST_1_WIDTH(elements & 0x7fff) | A6XX_TEX_CONST_1_HEIGHT(elements >> 15);
    d.dword[2] = A6XX_TEX_CONST_2_STRUCTSIZETEXELS(1) | A6XX_TEX_CONST_2_STARTOFFSETTEXELS(startTexels) |
                 A6XX_TEX_CONST_2_TYPE(A6XX_TEX_BUFFER);
    d.dword[4] = A6XX_TEX_CONST_4_BASE_LO(uint32_t(base));
    d.dword[5] = A6XX_TEX_CONST_5_BASE_HI(uint32_t(base >> 32));
    return d;
}

// A linear 2D image's layer size as fdl6_layout computes it (fd6_layout.c:249-250, 284, 304-305): the rows of the last
// (here the only) level are counted to a multiple of 4 ("over-fetch on the last level"), and the layer is padded to
// 4 KiB, which is also ARRAY_PITCH's unit.
constexpr uint64_t layerBytes2d(uint32_t height, uint64_t pitchBytes) {
    uint64_t rows = (uint64_t(height) + 3) & ~uint64_t(3);
    return (rows * pitchBytes + 4095) & ~uint64_t(4095);
}

// the format's swizzle for a linear colour format: identity, with W reading ONE when the format has no alpha
// (fdl6_format_swiz, fd6_view.c:117-120: "for rgbx, force A to 1. Harmless for R/RG, where we already get 1")
constexpr uint32_t texSwizzle(bool hasAlpha) {
    return A6XX_TEX_CONST_0_SWIZ_X(A6XX_TEX_X) | A6XX_TEX_CONST_0_SWIZ_Y(A6XX_TEX_Y) | A6XX_TEX_CONST_0_SWIZ_Z(A6XX_TEX_Z) |
           A6XX_TEX_CONST_0_SWIZ_W(hasAlpha ? A6XX_TEX_W : A6XX_TEX_ONE);
}

// A linear 2D texture of one level and one layer, as a sam/saml/isaml reads it: fdl6_view_init's `descriptor`
// (fd6_view.c:232-250) for TILE6_LINEAR, no UBWC, level_count 1, identity view swizzle, min_lod_clamp 0. PITCHALIGN
// is log2(pitch alignment / 64): for a pitch Mesa chose, log2(bytes per pixel) (fd6_layout.c:194-201). The base goes
// in raw (fd6_view.c:248), so it must be 32-byte aligned for the field (fdl6_layout gives 64).
constexpr TexConst texture2d(uint64_t iova, uint32_t width, uint32_t height, uint32_t pitchBytes, uint32_t pitchalignLog2Cpp,
                             a6xx_format fmt, a3xx_color_swap swap, bool hasAlpha, bool srgb) {
    TexConst d{};
    d.dword[0] = A6XX_TEX_CONST_0_TILE_MODE(TILE6_LINEAR) | (srgb ? A6XX_TEX_CONST_0_SRGB : 0u) | A6XX_TEX_CONST_0_FMT(fmt) |
                 A6XX_TEX_CONST_0_SAMPLES(MSAA_ONE) | A6XX_TEX_CONST_0_SWAP(swap) | texSwizzle(hasAlpha) | A6XX_TEX_CONST_0_MIPLVLS(0);
    d.dword[1] = A6XX_TEX_CONST_1_WIDTH(width) | A6XX_TEX_CONST_1_HEIGHT(height);
    d.dword[2] = A6XX_TEX_CONST_2_PITCHALIGN(pitchalignLog2Cpp) | A6XX_TEX_CONST_2_PITCH(pitchBytes) | A6XX_TEX_CONST_2_TYPE(A6XX_TEX_2D);
    d.dword[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t(layerBytes2d(height, pitchBytes)));
    d.dword[4] = uint32_t(iova);
    d.dword[5] = A6XX_TEX_CONST_5_BASE_HI(uint32_t(iova >> 32)) | A6XX_TEX_CONST_5_DEPTH(1);
    d.dword[6] = A6XX_TEX_CONST_6_MIN_LOD_CLAMP(0);
    return d;
}

// The same memory as a stib/ldib writes and reads it, at an IBO slot: fdl6_view_init's `storage_descriptor`
// (fd6_view.c:383-397), which is the texture descriptor without SAMPLES and MIPLVLS in dword 0 and without PITCHALIGN
// in dword 2 (DEPTH is the layer count there too; 1 here). For the served formats the storage format and swap are the
// texture ones (fd6_format_table.c: one .tex/.rb pair, fd6_pipe2swap for both).
constexpr TexConst image2d(uint64_t iova, uint32_t width, uint32_t height, uint32_t pitchBytes, a6xx_format fmt, a3xx_color_swap swap,
                           bool hasAlpha, bool srgb) {
    TexConst d{};
    d.dword[0] = A6XX_TEX_CONST_0_FMT(fmt) | (srgb ? A6XX_TEX_CONST_0_SRGB : 0u) | texSwizzle(hasAlpha) |
                 A6XX_TEX_CONST_0_TILE_MODE(TILE6_LINEAR) | A6XX_TEX_CONST_0_SWAP(swap);
    d.dword[1] = A6XX_TEX_CONST_1_WIDTH(width) | A6XX_TEX_CONST_1_HEIGHT(height);
    d.dword[2] = A6XX_TEX_CONST_2_PITCH(pitchBytes) | A6XX_TEX_CONST_2_TYPE(A6XX_TEX_2D);
    d.dword[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t(layerBytes2d(height, pitchBytes)));
    d.dword[4] = uint32_t(iova);
    d.dword[5] = A6XX_TEX_CONST_5_BASE_HI(uint32_t(iova >> 32)) | A6XX_TEX_CONST_5_DEPTH(1);
    return d;
}

// --- the sampler descriptor: 4 dwords (REG_A6XX_TEX_SAMP_0 to _3), 16 bytes a unit in the SB6_CS_TEX ST6_SHADER load ------
// Both Mesa drivers write it the same way (fd6_sampler_state_create, fd6_texture.cc:250-306; tu_CreateSampler,
// tu_sampler.cc:69-98). The two LOD clamps are unsigned 4.8 fixed point in 12 bits and the bias signed 5.8 in 13: Mesa's
// encoders multiply a float by 256 and cast, which is not a shift, so there is no __SCALE for them; kLodFractionBits
// says how texture.cpp converts, and the fields below take the fixed-point value ready made.

inline constexpr uint32_t REG_A6XX_TEX_SAMP_0 = 0x0;                                    // a6xx.xml.h:11204
inline constexpr uint32_t A6XX_TEX_SAMP_0_MIPFILTER_LINEAR_NEAR = 0x00000001;           // a6xx.xml.h:11205  the mip filter is linear (Gallium: PIPE_TEX_MIPFILTER_LINEAR)
inline constexpr uint32_t A6XX_TEX_SAMP_0_XY_MAG__MASK = 0x00000006;                    // a6xx.xml.h:11206
inline constexpr uint32_t A6XX_TEX_SAMP_0_XY_MAG__SHIFT = 1;                            // a6xx.xml.h:11207
inline constexpr uint32_t A6XX_TEX_SAMP_0_XY_MIN__MASK = 0x00000018;                    // a6xx.xml.h:11212
inline constexpr uint32_t A6XX_TEX_SAMP_0_XY_MIN__SHIFT = 3;                            // a6xx.xml.h:11213
inline constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_S__MASK = 0x000000e0;                    // a6xx.xml.h:11218
inline constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_S__SHIFT = 5;                            // a6xx.xml.h:11219
inline constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_T__MASK = 0x00000700;                    // a6xx.xml.h:11224
inline constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_T__SHIFT = 8;                            // a6xx.xml.h:11225
inline constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_R__MASK = 0x00003800;                    // a6xx.xml.h:11230
inline constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_R__SHIFT = 11;                           // a6xx.xml.h:11231
inline constexpr uint32_t A6XX_TEX_SAMP_0_ANISO__MASK = 0x0001c000;                     // a6xx.xml.h:11236
inline constexpr uint32_t A6XX_TEX_SAMP_0_ANISO__SHIFT = 14;                            // a6xx.xml.h:11237
inline constexpr uint32_t A6XX_TEX_SAMP_0_LOD_BIAS__MASK = 0xfff80000;                  // a6xx.xml.h:11242  signed 5.8 fixed point ("no idea how many bits for real", the XML)
inline constexpr uint32_t A6XX_TEX_SAMP_0_LOD_BIAS__SHIFT = 19;                         // a6xx.xml.h:11243
inline constexpr uint32_t REG_A6XX_TEX_SAMP_1 = 0x1;                                    // a6xx.xml.h:11250
inline constexpr uint32_t A6XX_TEX_SAMP_1_CLAMPENABLE = 0x00000001;                     // a6xx.xml.h:11251  clamp unorm/snorm results after filtering (the XML); neither Mesa driver sets it
inline constexpr uint32_t A6XX_TEX_SAMP_1_COMPARE_FUNC__MASK = 0x0000000e;              // a6xx.xml.h:11252
inline constexpr uint32_t A6XX_TEX_SAMP_1_COMPARE_FUNC__SHIFT = 1;                      // a6xx.xml.h:11253
inline constexpr uint32_t A6XX_TEX_SAMP_1_CUBEMAPSEAMLESSFILTOFF = 0x00000010;          // a6xx.xml.h:11258
inline constexpr uint32_t A6XX_TEX_SAMP_1_UNNORM_COORDS = 0x00000020;                   // a6xx.xml.h:11259  Metal's coord::pixel (normalizedCoordinates NO)
inline constexpr uint32_t A6XX_TEX_SAMP_1_MIPFILTER_LINEAR_FAR = 0x00000040;            // a6xx.xml.h:11260  Gallium sets it for PIPE_TEX_MIPFILTER_NONE (fd6_texture.cc:279); turnip: "disables mipmapping"
inline constexpr uint32_t A6XX_TEX_SAMP_1_MAX_LOD__MASK = 0x000fff00;                   // a6xx.xml.h:11261  unsigned 4.8 fixed point: 0 to 4095/256
inline constexpr uint32_t A6XX_TEX_SAMP_1_MAX_LOD__SHIFT = 8;                           // a6xx.xml.h:11262
inline constexpr uint32_t A6XX_TEX_SAMP_1_MIN_LOD__MASK = 0xfff00000;                   // a6xx.xml.h:11267
inline constexpr uint32_t A6XX_TEX_SAMP_1_MIN_LOD__SHIFT = 20;                          // a6xx.xml.h:11268
inline constexpr uint32_t REG_A6XX_TEX_SAMP_2 = 0x2;                                    // a6xx.xml.h:11275
inline constexpr uint32_t A6XX_TEX_SAMP_2_REDUCTION_MODE__MASK = 0x00000003;            // a6xx.xml.h:11276
inline constexpr uint32_t A6XX_TEX_SAMP_2_REDUCTION_MODE__SHIFT = 0;                    // a6xx.xml.h:11277
inline constexpr uint32_t A6XX_TEX_SAMP_2_CHROMA_LINEAR = 0x00000020;                   // a6xx.xml.h:11282  Gallium sets it whenever mag and min are both linear (fd6_texture.cc:295-300)
inline constexpr uint32_t A6XX_TEX_SAMP_2_BCOLOR__MASK = 0xffffff80;                    // a6xx.xml.h:11283  the entry in the table at SP_TP_BORDER_COLOR_BASE_ADDR (128 bytes each)
inline constexpr uint32_t A6XX_TEX_SAMP_2_BCOLOR__SHIFT = 7;                            // a6xx.xml.h:11284
inline constexpr uint32_t REG_A6XX_TEX_SAMP_3 = 0x3;                                    // a6xx.xml.h:11291  no fields; both drivers write 0
inline constexpr uint32_t kTexSampDwords = 4;                                           // not-in-headers: REG_A6XX_TEX_SAMP_3 + 1; Mesa's A6XX_TEX_SAMP_DWORDS
inline constexpr uint32_t kLodFractionBits = 8;                                         // not-in-headers: a6xx.xml.h's A6XX_TEX_SAMP_1_MIN_LOD/MAX_LOD encoders, uint32_t(val * 256.0)
inline constexpr uint32_t kLodMaxFixed = 0xfff;                                         // not-in-headers: the 12-bit field's ceiling, 4095/256 (tu_sampler.cc:66-67 clamps to it)
constexpr uint32_t A6XX_TEX_SAMP_0_XY_MAG(a6xx_tex_filter v) { return field(v, A6XX_TEX_SAMP_0_XY_MAG__SHIFT, A6XX_TEX_SAMP_0_XY_MAG__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_0_XY_MIN(a6xx_tex_filter v) { return field(v, A6XX_TEX_SAMP_0_XY_MIN__SHIFT, A6XX_TEX_SAMP_0_XY_MIN__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_S(a6xx_tex_clamp v) { return field(v, A6XX_TEX_SAMP_0_WRAP_S__SHIFT, A6XX_TEX_SAMP_0_WRAP_S__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_T(a6xx_tex_clamp v) { return field(v, A6XX_TEX_SAMP_0_WRAP_T__SHIFT, A6XX_TEX_SAMP_0_WRAP_T__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_0_WRAP_R(a6xx_tex_clamp v) { return field(v, A6XX_TEX_SAMP_0_WRAP_R__SHIFT, A6XX_TEX_SAMP_0_WRAP_R__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_0_ANISO(a6xx_tex_aniso v) { return field(v, A6XX_TEX_SAMP_0_ANISO__SHIFT, A6XX_TEX_SAMP_0_ANISO__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_0_LOD_BIAS(int32_t fixed88) { return field(uint32_t(fixed88), A6XX_TEX_SAMP_0_LOD_BIAS__SHIFT, A6XX_TEX_SAMP_0_LOD_BIAS__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_1_COMPARE_FUNC(adreno_compare_func v) {
    return field(v, A6XX_TEX_SAMP_1_COMPARE_FUNC__SHIFT, A6XX_TEX_SAMP_1_COMPARE_FUNC__MASK);
}
constexpr uint32_t A6XX_TEX_SAMP_1_MAX_LOD(uint32_t fixed88) { return field(fixed88, A6XX_TEX_SAMP_1_MAX_LOD__SHIFT, A6XX_TEX_SAMP_1_MAX_LOD__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_1_MIN_LOD(uint32_t fixed88) { return field(fixed88, A6XX_TEX_SAMP_1_MIN_LOD__SHIFT, A6XX_TEX_SAMP_1_MIN_LOD__MASK); }
constexpr uint32_t A6XX_TEX_SAMP_2_REDUCTION_MODE(a6xx_reduction_mode v) {
    return field(v, A6XX_TEX_SAMP_2_REDUCTION_MODE__SHIFT, A6XX_TEX_SAMP_2_REDUCTION_MODE__MASK);
}
constexpr uint32_t A6XX_TEX_SAMP_2_BCOLOR(uint32_t entry) { return field(entry, A6XX_TEX_SAMP_2_BCOLOR__SHIFT, A6XX_TEX_SAMP_2_BCOLOR__MASK); }

struct TexSamp {
    uint32_t dword[kTexSampDwords] = {};
};

// a sampler in the hardware's own terms; the defaults are the fields' zeros but for maxLod, which is the field's ceiling
// (an unclamped level of detail), so that SamplerFields{} is "nearest, repeat, every level"
struct SamplerFields {
    a6xx_tex_filter mag = A6XX_TEX_NEAREST;
    a6xx_tex_filter min = A6XX_TEX_NEAREST;
    bool mipLinearNear = false;      // MIPFILTER_LINEAR_NEAR: the mip filter is linear
    bool mipLinearFar = false;       // MIPFILTER_LINEAR_FAR: no mipmapping (Gallium's reading of PIPE_TEX_MIPFILTER_NONE)
    a6xx_tex_clamp wrapS = A6XX_TEX_REPEAT;
    a6xx_tex_clamp wrapT = A6XX_TEX_REPEAT;
    a6xx_tex_clamp wrapR = A6XX_TEX_REPEAT;
    a6xx_tex_aniso aniso = A6XX_TEX_ANISO_1;
    int32_t lodBias88 = 0;
    bool unnormCoords = false;
    bool compare = false;            // COMPARE_FUNC is written only with this set (Gallium: compare_mode)
    adreno_compare_func compareFunc = FUNC_NEVER;
    uint32_t minLod88 = 0;
    uint32_t maxLod88 = kLodMaxFixed;
    bool chromaLinear = false;
    a6xx_reduction_mode reduction = A6XX_REDUCTION_MODE_AVERAGE;
    uint32_t bcolor = 0;
};

// the four words, composed as fd6_sampler_state_create composes them (fd6_texture.cc:270-303); dword 3 is 0
constexpr TexSamp texSamp(const SamplerFields& f) {
    TexSamp s{};
    s.dword[0] = (f.mipLinearNear ? A6XX_TEX_SAMP_0_MIPFILTER_LINEAR_NEAR : 0u) | A6XX_TEX_SAMP_0_XY_MAG(f.mag) | A6XX_TEX_SAMP_0_XY_MIN(f.min) |
                 A6XX_TEX_SAMP_0_ANISO(f.aniso) | A6XX_TEX_SAMP_0_WRAP_S(f.wrapS) | A6XX_TEX_SAMP_0_WRAP_T(f.wrapT) |
                 A6XX_TEX_SAMP_0_WRAP_R(f.wrapR) | A6XX_TEX_SAMP_0_LOD_BIAS(f.lodBias88);
    s.dword[1] = (f.mipLinearFar ? A6XX_TEX_SAMP_1_MIPFILTER_LINEAR_FAR : 0u) | (f.unnormCoords ? A6XX_TEX_SAMP_1_UNNORM_COORDS : 0u) |
                 A6XX_TEX_SAMP_1_MIN_LOD(f.minLod88) | A6XX_TEX_SAMP_1_MAX_LOD(f.maxLod88) |
                 (f.compare ? A6XX_TEX_SAMP_1_COMPARE_FUNC(f.compareFunc) : 0u);
    s.dword[2] = A6XX_TEX_SAMP_2_BCOLOR(f.bcolor) | (f.chromaLinear ? A6XX_TEX_SAMP_2_CHROMA_LINEAR : 0u) |
                 A6XX_TEX_SAMP_2_REDUCTION_MODE(f.reduction);
    s.dword[3] = 0;
    return s;
}

// --- whole register values, as the dispatch sequence computes them ----------------------------------------------------------
// These follow what computerator's a6xx.cc and Gallium's fd6_compute.cc compute from ir3's shader info; the inputs are
// in the natural units (bytes, vec4s, register counts).

// SP_CS_CTRL_REG1's and HLSQ_CS_CTRL_REG1's mode from the constant length in vec4: the smallest split that holds it
constexpr a6xx_const_ram_mode constRamMode(uint32_t constlenVec4) {
    return constlenVec4 > 256 ? CONSTLEN_512 : constlenVec4 > 192 ? CONSTLEN_256 : constlenVec4 > 128 ? CONSTLEN_192 : CONSTLEN_128;
}

// SHARED_SIZE from the shared memory in bytes: (n + 1) KiB enabled, never less than 2 KiB (Mesa's floor of 1)
constexpr uint32_t sharedSizeField(uint32_t bytes) {
    uint32_t n = bytes == 0 ? 0 : (bytes - 1) / 1024;
    return n < 1 ? 1 : n;
}

constexpr uint32_t spCsCtrlReg0(uint32_t fullRegs, uint32_t halfRegs, uint32_t branchstack, a6xx_threadsize threadsize,
                                bool mergedregs = true, bool earlyPreamble = false) {
    return A6XX_SP_CS_CTRL_REG0_THREADSIZE(threadsize) | A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(fullRegs) |
           A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(halfRegs) | (mergedregs ? A6XX_SP_CS_CTRL_REG0_MERGEDREGS : 0u) |
           (earlyPreamble ? A6XX_SP_CS_CTRL_REG0_EARLYPREAMBLE : 0u) | A6XX_SP_CS_CTRL_REG0_BRANCHSTACK(branchstack);
}

constexpr uint32_t spCsCtrlReg1(uint32_t sharedBytes, uint32_t constlenVec4) {
    return A6XX_SP_CS_CTRL_REG1_SHARED_SIZE(sharedSizeField(sharedBytes)) |
           A6XX_SP_CS_CTRL_REG1_CONSTANTRAMMODE(constRamMode(constlenVec4));
}

// HLSQ_CS_CNTL from the constant length in vec4, rounded up to the 4 the field counts in, enabled
constexpr uint32_t hlsqCsCntl(uint32_t constlenVec4) {
    return A6XX_HLSQ_CS_CNTL_CONSTLEN((constlenVec4 + 3) & ~3u) | A6XX_HLSQ_CS_CNTL_ENABLED;
}

constexpr uint32_t spCsConfig(uint32_t ibos, uint32_t textures, uint32_t samplers) {
    return A6XX_SP_CS_CONFIG_ENABLED | A6XX_SP_CS_CONFIG_NIBO(ibos) | A6XX_SP_CS_CONFIG_NTEX(textures) | A6XX_SP_CS_CONFIG_NSAMP(samplers);
}

// HLSQ_CS_CNTL_0 and SP_CS_CNTL_0 (same layout): the work group id constant and local id register the shader reads
constexpr uint32_t csCntl0(uint32_t workGroupIdConst, uint32_t localIdReg) {
    return A6XX_HLSQ_CS_CNTL_0_WGIDCONSTID(workGroupIdConst) | A6XX_HLSQ_CS_CNTL_0_WGSIZECONSTID(kRegidNone) |
           A6XX_HLSQ_CS_CNTL_0_WGOFFSETCONSTID(kRegidNone) | A6XX_HLSQ_CS_CNTL_0_LOCALIDREGID(localIdReg);
}

// HLSQ_CS_CNTL_1 and SP_CS_CNTL_1 (same layout): no linear local id, the thread size
constexpr uint32_t csCntl1(a6xx_threadsize threadsize) {
    return A6XX_HLSQ_CS_CNTL_1_LINEARLOCALIDREGID(kRegidNone) | A6XX_HLSQ_CS_CNTL_1_THREADSIZE(threadsize);
}

constexpr uint32_t loadState6_0(uint32_t dstOff, a6xx_state_type type, a6xx_state_src src, a6xx_state_block block, uint32_t numUnit) {
    return CP_LOAD_STATE6_0_DST_OFF(dstOff) | CP_LOAD_STATE6_0_STATE_TYPE(type) | CP_LOAD_STATE6_0_STATE_SRC(src) |
           CP_LOAD_STATE6_0_STATE_BLOCK(block) | CP_LOAD_STATE6_0_NUM_UNIT(numUnit);
}

// HLSQ_CS_NDRANGE_0 from the local size itself (the fields hold it minus one)
constexpr uint32_t ndrange0(uint32_t dimensions, uint32_t localX, uint32_t localY, uint32_t localZ) {
    return A6XX_HLSQ_CS_NDRANGE_0_KERNELDIM(dimensions) | A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX(localX - 1) |
           A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY(localY - 1) | A6XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(localZ - 1);
}

// `timestamp` is the TIMESTAMP bit: it must stay clear (see it above); the 4-word form of the packet writes the
// packet's value with the bit clear, which is what the waits of the dispatch tail poll for
constexpr uint32_t eventWrite0(vgt_event_type event, bool timestamp = false) {
    return CP_EVENT_WRITE_0_EVENT(event) | (timestamp ? CP_EVENT_WRITE_0_TIMESTAMP : 0u);
}

constexpr uint32_t setMarker0(a6xx_marker mode) { return A6XX_CP_SET_MARKER_0_MODE(mode); }

constexpr uint32_t waitRegMem0(cp_cond_function function, poll_memory_type poll) {
    return CP_WAIT_REG_MEM_0_FUNCTION(function) | CP_WAIT_REG_MEM_0_POLL(poll);
}

// --- the state every batch starts with ---------------------------------------------------------------------------------------
// Gallium never trusts the registers it finds: at the start of every batch (fd6_emit_restore in a6xx/fd6_emit.cc) it
// runs, through CP_INDIRECT_BUFFER, a ring it built once with fd6_emit_static_regs, which writes every register below.
// A dispatch touches almost none of them, but the hardware reads them all the same, and a GPU no window server has
// set up has them at whatever power-up or the last user left. Everything here is the A6XX path for this chip
// (has_early_preamble off, so SP_FS_CTRL_REG0 is not reset; magic_raw empty).
//
// The order, as compute.cpp should write it (a type4 of one register each, unless said):
//   CP_SET_MODE 0 (1 word); the three cache invalidates (CP_EVENT_WRITE, 1 word each: PC_CCU_INVALIDATE_COLOR,
//   PC_CCU_INVALIDATE_DEPTH, CACHE_INVALIDATE; no wait between them); HLSQ_INVALIDATE_CMD = hlsqInvalidateCmdAll();
//   CP_WAIT_FOR_IDLE; then the static registers: every entry of kStaticState in order; CP_SET_DRAW_STATE (3 words:
//   setDrawState0(0, CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS, 0), 0, 0); every entry of kStaticStateAfterDrawState;
//   REG_A6XX_VFD_FETCH_SIZE(i) = 0 for i in 0..31 (vfdFetchSize(i), 32 type4 of one); SP_TP_BORDER_COLOR_BASE_ADDR and
//   SP_PS_TP_BORDER_COLOR_BASE_ADDR (type4 of two each) = a 128-byte aligned buffer of kBorderColorBytes;
//   every entry of kStaticStateTail; then CP_WAIT_FOR_IDLE and REG_A6XX_RB_CCU_CNTL = rbCcuCntlSysmem() (fd6_emit_ccu_cntl's
//   A6XX path: the wait first, "early a6xx needed this"). Gallium ends with three CP_SET_AMBLE (3 words each) telling
//   the CP what to run around a preemption; they are optional for a batch that is not preempted (see below).
// emit_marker6 (a CP_WAIT_FOR_IDLE and a write of a counter to CP_SCRATCH_REG(7), or (6) around the IB) is compiled
// out of Gallium's release builds (freedreno_util.h __EMIT_MARKER is 0 without DEBUG), so there is nothing to copy.

struct RegWrite {
    uint32_t reg;
    uint32_t value;
};

// Mesa's per-chip "magic" values, as its generated freedreno_devices.h has them for this chip (__info14, the entry every
// A635 chip id maps to). Mesa does not know what most of them mean either; they are what the blob writes.
inline constexpr uint32_t kMagicPcPowerCntl = 1;                  // not-in-headers: freedreno_devices.h __info14 .magic.PC_POWER_CNTL
inline constexpr uint32_t kMagicTpl1DbgEcoCntl = 0x05008000;      // not-in-headers: freedreno_devices.h __info14 .magic.TPL1_DBG_ECO_CNTL (83918848)
inline constexpr uint32_t kMagicGrasDbgEcoCntl = 0;               // not-in-headers: freedreno_devices.h __info14 .magic.GRAS_DBG_ECO_CNTL
inline constexpr uint32_t kMagicSpChickenBits = 0x00001400;       // not-in-headers: freedreno_devices.h __info14 .magic.SP_CHICKEN_BITS (5120)
inline constexpr uint32_t kMagicUcheClientPf = 0x00000084;        // not-in-headers: freedreno_devices.h __info14 .magic.UCHE_CLIENT_PF (132)
inline constexpr uint32_t kMagicPcModeCntl = 0x1f;                // not-in-headers: freedreno_devices.h __info14 .magic.PC_MODE_CNTL (31)
inline constexpr uint32_t kMagicSpDbgEcoCntl = 0x00000006;        // not-in-headers: freedreno_devices.h __info14 .magic.SP_DBG_ECO_CNTL
inline constexpr uint32_t kMagicRbDbgEcoCntl = 0x04100000;        // not-in-headers: freedreno_devices.h __info14 .magic.RB_DBG_ECO_CNTL (68157440)
inline constexpr uint32_t kMagicHlsqDbgEcoCntl = 0;               // not-in-headers: freedreno_devices.h __info14 .magic.HLSQ_DBG_ECO_CNTL
inline constexpr uint32_t kMagicRbUnknown8E01 = 0;                // not-in-headers: freedreno_devices.h __info14 .magic.RB_UNKNOWN_8E01
inline constexpr uint32_t kMagicVpcDbgEcoCntl = 0x02000000;       // not-in-headers: freedreno_devices.h __info14 .magic.VPC_DBG_ECO_CNTL (33554432)
inline constexpr uint32_t kMagicUcheUnknown0E12 = 1;              // not-in-headers: freedreno_devices.h __info14 .magic.UCHE_UNKNOWN_0E12
// and the chip facts fd6_emit_static_regs and fd6_emit_ccu_cntl branch on
inline constexpr bool kHasGmemFastClear = true;                   // not-in-headers: freedreno_devices.h __info14 .has_gmem_fast_clear
inline constexpr bool kConcurrentResolve = false;                 // not-in-headers: freedreno_devices.h __info14 has no .concurrent_resolve (false)
inline constexpr bool kHasEarlyPreamble = false;                  // not-in-headers: freedreno_devices.h __info14 has no .has_early_preamble (a6xx_gen4 leaves it off)
inline constexpr uint32_t kNumCcu = 2;                            // not-in-headers: freedreno_devices.h __info14 .num_ccu
inline constexpr uint32_t kSysmemPerCcuDepthCacheSize = 0x10000;  // not-in-headers: freedreno_devices.h __info14 .sysmem_per_ccu_depth_cache_size (65536)
inline constexpr uint32_t kSysmemPerCcuColorCacheSize = 0x10000;  // not-in-headers: freedreno_devices.h __info14 .sysmem_per_ccu_color_cache_size (65536)
// the border color table both TP base registers point at: Gallium's is FD6_MAX_BORDER_COLORS (256) entries of
// sizeof(struct fd6_bcolor_entry) (128 bytes: 32 fp32/fp16/int words); zeros are a valid table (black, transparent)
inline constexpr uint32_t kBorderColorBytes = 256 * 128;          // not-in-headers: fd6_texture.h FD6_MAX_BORDER_COLORS * FD6_BORDER_COLOR_SIZE
inline constexpr uint32_t kBorderColorAlign = 128;                // not-in-headers: a6xx.xml SP_TP_BORDER_COLOR_BASE_ADDR align="128"

// the registers, in the order of their first write
inline constexpr uint32_t REG_A6XX_RB_DBG_ECO_CNTL = 0x8e04;                           // a6xx.xml.h:6711
inline constexpr uint32_t REG_A6XX_SP_DBG_ECO_CNTL = 0xae00;                           // a6xx.xml.h:9559
inline constexpr uint32_t REG_A6XX_TPL1_UNKNOWN_B605 = 0xb605;                         // a6xx.xml.h:10161  "always 0x0 or 0x44?" (the XML); Gallium: 0x44
inline constexpr uint32_t REG_A6XX_TPL1_DBG_ECO_CNTL = 0xb600;                         // a6xx.xml.h:10128
inline constexpr uint32_t REG_A6XX_HLSQ_UNKNOWN_BE00 = 0xbe00;                         // a6xx.xml.h:11136  Gallium: 0x80
inline constexpr uint32_t REG_A6XX_HLSQ_UNKNOWN_BE01 = 0xbe01;                         // a6xx.xml.h:11139
inline constexpr uint32_t REG_A6XX_VPC_DBG_ECO_CNTL = 0x9600;                          // a6xx.xml.h:7417
inline constexpr uint32_t REG_A6XX_GRAS_DBG_ECO_CNTL = 0x8600;                         // a6xx.xml.h:5242
inline constexpr uint32_t REG_A6XX_HLSQ_DBG_ECO_CNTL = 0xbe04;                         // a6xx.xml.h:11142
inline constexpr uint32_t REG_A6XX_SP_CHICKEN_BITS = 0xae03;                           // a6xx.xml.h:9568
inline constexpr uint32_t REG_A6XX_SP_IBO_COUNT = 0xab20;                              // a6xx.xml.h:9515  the 3D pipeline's IBO count (SP_CS_IBO_COUNT is compute's)
inline constexpr uint32_t REG_A6XX_SP_UNKNOWN_B182 = 0xb182;                           // a6xx.xml.h:9656
inline constexpr uint32_t REG_A6XX_HLSQ_SHARED_CONSTS = 0xbb11;                        // a6xx.xml.h:11099  Vulkan push constants; off
inline constexpr uint32_t A6XX_HLSQ_SHARED_CONSTS_ENABLE = 0x00000001;                 // a6xx.xml.h:11100
inline constexpr uint32_t REG_A6XX_UCHE_UNKNOWN_0E12 = 0x0e12;                         // a6xx.xml.h:4253
inline constexpr uint32_t REG_A6XX_UCHE_CLIENT_PF = 0x0e19;                            // a6xx.xml.h:3940
inline constexpr uint32_t A6XX_UCHE_CLIENT_PF_PERFSEL__MASK = 0x000000ff;              // a6xx.xml.h:3941
inline constexpr uint32_t A6XX_UCHE_CLIENT_PF_PERFSEL__SHIFT = 0;                      // a6xx.xml.h:3942
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8E01 = 0x8e01;                           // a6xx.xml.h:6708
inline constexpr uint32_t REG_A6XX_SP_UNKNOWN_A9A8 = 0xa9a8;                           // a6xx.xml.h:9093  "always 0x0?" (the XML)
inline constexpr uint32_t REG_A6XX_VFD_ADD_OFFSET = 0xa009;                            // a6xx.xml.h:7960
inline constexpr uint32_t A6XX_VFD_ADD_OFFSET_VERTEX = 0x00000001;                     // a6xx.xml.h:7961  add VFD_INDEX_OFFSET to the vertex id register
inline constexpr uint32_t A6XX_VFD_ADD_OFFSET_INSTANCE = 0x00000002;                   // a6xx.xml.h:7962
inline constexpr uint32_t REG_A6XX_VPC_UNKNOWN_9107 = 0x9107;                          // a6xx.xml.h:7094
inline constexpr uint32_t A6XX_VPC_UNKNOWN_9107_RASTER_DISCARD = 0x00000001;           // a6xx.xml.h:7095
inline constexpr uint32_t A6XX_VPC_UNKNOWN_9107_UNK2 = 0x00000004;                     // a6xx.xml.h:7096
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8811 = 0x8811;                           // a6xx.xml.h:5679  bits 4..6; Gallium: 0x10
inline constexpr uint32_t REG_A6XX_PC_MODE_CNTL = 0x9804;                              // a6xx.xml.h:7464
inline constexpr uint32_t REG_A6XX_GRAS_LRZ_PS_INPUT_CNTL = 0x8101;                    // a6xx.xml.h:4987
inline constexpr uint32_t A6XX_GRAS_LRZ_PS_INPUT_CNTL_SAMPLEID = 0x00000001;           // a6xx.xml.h:4988
inline constexpr uint32_t REG_A6XX_GRAS_SAMPLE_CNTL = 0x8109;                          // a6xx.xml.h:5029
inline constexpr uint32_t A6XX_GRAS_SAMPLE_CNTL_PER_SAMP_MODE = 0x00000001;            // a6xx.xml.h:5030
inline constexpr uint32_t REG_A6XX_GRAS_UNKNOWN_8110 = 0x8110;                         // a6xx.xml.h:5059  bits 0..1; Gallium: 2
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8818 = 0x8818;                           // a6xx.xml.h:5685  "always 0x0?" (the XML)
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8819 = 0x8819;                           // a6xx.xml.h:5688
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_881A = 0x881a;                           // a6xx.xml.h:5691
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_881B = 0x881b;                           // a6xx.xml.h:5694
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_881C = 0x881c;                           // a6xx.xml.h:5697
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_881D = 0x881d;                           // a6xx.xml.h:5700
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_881E = 0x881e;                           // a6xx.xml.h:5703
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_88F0 = 0x88f0;                           // a6xx.xml.h:6459  "always 0x0?" (the XML)
inline constexpr uint32_t REG_A6XX_VPC_POINT_COORD_INVERT = 0x9236;                    // a6xx.xml.h:7234
inline constexpr uint32_t A6XX_VPC_POINT_COORD_INVERT_INVERT = 0x00000001;             // a6xx.xml.h:7235
inline constexpr uint32_t REG_A6XX_VPC_UNKNOWN_9300 = 0x9300;                          // a6xx.xml.h:7238  "always 0x0?" (the XML)
inline constexpr uint32_t REG_A6XX_VPC_SO_DISABLE = 0x9306;                            // a6xx.xml.h:7377
inline constexpr uint32_t A6XX_VPC_SO_DISABLE_DISABLE = 0x00000001;                    // a6xx.xml.h:7378  no stream-out
inline constexpr uint32_t REG_A6XX_PC_RASTER_CNTL = 0x9980;                            // a6xx.xml.h:7541
inline constexpr uint32_t A6XX_PC_RASTER_CNTL_STREAM__MASK = 0x00000003;               // a6xx.xml.h:7542
inline constexpr uint32_t A6XX_PC_RASTER_CNTL_STREAM__SHIFT = 0;                       // a6xx.xml.h:7543
inline constexpr uint32_t A6XX_PC_RASTER_CNTL_DISCARD = 0x00000004;                    // a6xx.xml.h:7548
inline constexpr uint32_t REG_A6XX_PC_MULTIVIEW_CNTL = 0x9b07;                         // a6xx.xml.h:7696
inline constexpr uint32_t A6XX_PC_MULTIVIEW_CNTL_ENABLE = 0x00000001;                  // a6xx.xml.h:7697
inline constexpr uint32_t A6XX_PC_MULTIVIEW_CNTL_DISABLEMULTIPOS = 0x00000002;         // a6xx.xml.h:7698
inline constexpr uint32_t A6XX_PC_MULTIVIEW_CNTL_VIEWS__MASK = 0x0000007c;             // a6xx.xml.h:7699
inline constexpr uint32_t A6XX_PC_MULTIVIEW_CNTL_VIEWS__SHIFT = 2;                     // a6xx.xml.h:7700
inline constexpr uint32_t REG_A6XX_SP_UNKNOWN_B183 = 0xb183;                           // a6xx.xml.h:9659
inline constexpr uint32_t REG_A6XX_GRAS_SU_CONSERVATIVE_RAS_CNTL = 0x8099;             // a6xx.xml.h:4546
inline constexpr uint32_t A6XX_GRAS_SU_CONSERVATIVE_RAS_CNTL_CONSERVATIVERASEN = 0x00000001;  // a6xx.xml.h:4547
inline constexpr uint32_t REG_A6XX_GRAS_VS_LAYER_CNTL = 0x809b;                        // a6xx.xml.h:4568
inline constexpr uint32_t A6XX_GRAS_VS_LAYER_CNTL_WRITES_LAYER = 0x00000001;           // a6xx.xml.h:4569
inline constexpr uint32_t A6XX_GRAS_VS_LAYER_CNTL_WRITES_VIEW = 0x00000002;            // a6xx.xml.h:4570
inline constexpr uint32_t REG_A6XX_GRAS_SC_CNTL = 0x80a0;                              // a6xx.xml.h:4583
// the XML notes CCUSINGLECACHELINESIZE is ignored unless UNK9 is set; Gallium writes 2 and leaves UNK9 clear all the same
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_CCUSINGLECACHELINESIZE__MASK = 0x00000007; // a6xx.xml.h:4584
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_CCUSINGLECACHELINESIZE__SHIFT = 0;         // a6xx.xml.h:4585
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_UNK9 = 0x00000200;                         // a6xx.xml.h:4614
inline constexpr uint32_t REG_A6XX_GRAS_UNKNOWN_80AF = 0x80af;                         // a6xx.xml.h:4792
inline constexpr uint32_t REG_A6XX_VPC_UNKNOWN_9210 = 0x9210;                          // a6xx.xml.h:7162  "always 0x0" (the XML)
inline constexpr uint32_t REG_A6XX_VPC_UNKNOWN_9211 = 0x9211;                          // a6xx.xml.h:7165
inline constexpr uint32_t REG_A6XX_VPC_UNKNOWN_9602 = 0x9602;                          // a6xx.xml.h:7423  "always 0x0?" (the XML)
inline constexpr uint32_t REG_A6XX_PC_UNKNOWN_9E72 = 0x9e72;                           // a6xx.xml.h:7825  "always 0x0" (the XML)
inline constexpr uint32_t REG_A6XX_SP_TP_MODE_CNTL = 0xb309;                           // a6xx.xml.h:9818
inline constexpr uint32_t A6XX_SP_TP_MODE_CNTL_ISAMMODE__MASK = 0x00000003;            // a6xx.xml.h:9819
inline constexpr uint32_t A6XX_SP_TP_MODE_CNTL_ISAMMODE__SHIFT = 0;                    // a6xx.xml.h:9820
// Gallium: 0xa0 in these bits (the blob mostly writes 0xb2, which "seems to kill texture gather offsets", fd6_emit.cc)
inline constexpr uint32_t A6XX_SP_TP_MODE_CNTL_UNK3__MASK = 0x000000fc;                // a6xx.xml.h:9825
inline constexpr uint32_t A6XX_SP_TP_MODE_CNTL_UNK3__SHIFT = 2;                        // a6xx.xml.h:9826
inline constexpr uint32_t REG_A6XX_HLSQ_CONTROL_5_REG = 0xb986;                        // a6xx.xml.h:10484  both register ids "none"
inline constexpr uint32_t A6XX_HLSQ_CONTROL_5_REG_LINELENGTHREGID__MASK = 0x000000ff;  // a6xx.xml.h:10485
inline constexpr uint32_t A6XX_HLSQ_CONTROL_5_REG_LINELENGTHREGID__SHIFT = 0;          // a6xx.xml.h:10486
inline constexpr uint32_t A6XX_HLSQ_CONTROL_5_REG_FOVEATIONQUALITYREGID__MASK = 0x0000ff00;  // a6xx.xml.h:10491
inline constexpr uint32_t A6XX_HLSQ_CONTROL_5_REG_FOVEATIONQUALITYREGID__SHIFT = 8;    // a6xx.xml.h:10492
inline constexpr uint32_t REG_A6XX_VFD_MODE_CNTL = 0xa007;                             // a6xx.xml.h:7940
inline constexpr uint32_t A6XX_VFD_MODE_CNTL_RENDER_MODE__MASK = 0x00000007;           // a6xx.xml.h:7941
inline constexpr uint32_t A6XX_VFD_MODE_CNTL_RENDER_MODE__SHIFT = 0;                   // a6xx.xml.h:7942
inline constexpr uint32_t REG_A6XX_VFD_MULTIVIEW_CNTL = 0xa008;                        // a6xx.xml.h:7949
inline constexpr uint32_t A6XX_VFD_MULTIVIEW_CNTL_ENABLE = 0x00000001;                 // a6xx.xml.h:7950
inline constexpr uint32_t REG_A6XX_VPC_SO_STREAM_CNTL = 0x9305;                        // a6xx.xml.h:7344
inline constexpr uint32_t REG_A6XX_GRAS_LRZ_CNTL = 0x8100;                             // a6xx.xml.h:4964
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_ENABLE = 0x00000001;                      // a6xx.xml.h:4965
inline constexpr uint32_t REG_A6XX_RB_LRZ_CNTL = 0x8898;                               // a6xx.xml.h:6181
inline constexpr uint32_t A6XX_RB_LRZ_CNTL_ENABLE = 0x00000001;                        // a6xx.xml.h:6182
inline constexpr uint32_t REG_A6XX_RB_DEPTH_PLANE_CNTL = 0x8870;                       // a6xx.xml.h:5908
inline constexpr uint32_t REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL = 0x8094;                  // a6xx.xml.h:4500
// VFD_FETCH[i], 32 of them, 4 registers each: BASE (64-bit), SIZE, STRIDE. Gallium zeroes every SIZE so that a BASE
// left by another process is never fetched from (an IOVA fault otherwise).
inline constexpr uint32_t REG_A6XX_VFD_FETCH = 0xa010;                                 // a6xx.xml.h:7971  the array's first register
inline constexpr uint32_t REG_A6XX_VFD_FETCH__STRIDE = 4;                              // a6xx.xml.h:7971  registers per entry
inline constexpr uint32_t REG_A6XX_VFD_FETCH_SIZE = 0xa012;                            // a6xx.xml.h:7976  entry 0's SIZE
inline constexpr uint32_t kVfdFetchCount = 32;                                         // not-in-headers: a6xx.xml VFD_FETCH length="32"; fd6_emit.cc's loop
inline constexpr uint32_t REG_A6XX_SP_TP_BORDER_COLOR_BASE_ADDR = 0xb302;              // a6xx.xml.h:9693  64-bit
inline constexpr uint32_t REG_A6XX_SP_PS_TP_BORDER_COLOR_BASE_ADDR = 0xb180;           // a6xx.xml.h:9653  64-bit
inline constexpr uint32_t REG_A6XX_PC_DGEN_SU_CONSERVATIVE_RAS_CNTL = 0x980a;          // a6xx.xml.h:7483
inline constexpr uint32_t A6XX_PC_DGEN_SU_CONSERVATIVE_RAS_CNTL_CONSERVATIVERASEN = 0x00000001;  // a6xx.xml.h:7484
inline constexpr uint32_t REG_A6XX_SP_FS_CTRL_REG0 = 0xa980;                           // a6xx.xml.h:8749  reset to 0 only on chips with early preamble
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_EARLYPREAMBLE = 0x10000000;             // a6xx.xml.h:8788
// the preemption preamble (fd6_gmem.cc fd6_build_preemption_preamble) adds these two, both = kMagicPcPowerCntl
inline constexpr uint32_t REG_A6XX_PC_POWER_CNTL = 0x9805;                             // a6xx.xml.h:7467
inline constexpr uint32_t REG_A6XX_VFD_POWER_CNTL = 0xa0f8;                            // a6xx.xml.h:8034
// CP_SCRATCH_REG[i], 8 of them: emit_marker6 writes (7) and, around an IB, (6)
inline constexpr uint32_t REG_A6XX_CP_SCRATCH_REG = 0x0883;                            // a6xx.xml.h:2538  the array's first register
inline constexpr uint32_t REG_A6XX_CP_SCRATCH_REG__STRIDE = 1;                         // a6xx.xml.h:2538

enum a6xx_render_mode : uint32_t {
    RENDERING_PASS = 0,            // a6xx.xml.h:2208
    BINNING_PASS = 1,              // a6xx.xml.h:2209
};

// RB_CCU_CNTL: where in GMEM the color and depth CCU caches live and how big they are. For rendering straight to
// memory ("sysmem", every compute batch) Gallium puts the depth cache at GMEM offset 0 and the color cache right after
// it, each at full size (fd6_screen.cc fd6_screen_init: depth cache = num_ccu * sysmem_per_ccu_depth_cache_size). The
// offsets are GMEM-relative, in 4 KiB units: no GMEM base address is involved, and the GMEM size matters only for the
// tiled ("gmem") configuration, which puts the color cache at the end of GMEM.
inline constexpr uint32_t REG_A6XX_RB_CCU_CNTL = 0x8e07;                               // a6xx.xml.h:6720
inline constexpr uint32_t A6XX_RB_CCU_CNTL_GMEM_FAST_CLEAR_DISABLE = 0x00000001;       // a6xx.xml.h:6721  set when the chip lacks it; this one has it
inline constexpr uint32_t A6XX_RB_CCU_CNTL_CONCURRENT_RESOLVE = 0x00000004;            // a6xx.xml.h:6722
inline constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_OFFSET_HI__MASK = 0x00000080;         // a6xx.xml.h:6723  bit 21 of the offset
inline constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_OFFSET_HI__SHIFT = 7;                 // a6xx.xml.h:6724
inline constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__MASK = 0x00000200;         // a6xx.xml.h:6729
inline constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__SHIFT = 9;                 // a6xx.xml.h:6730
inline constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_CACHE_SIZE__MASK = 0x00000c00;        // a6xx.xml.h:6735
inline constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_CACHE_SIZE__SHIFT = 10;               // a6xx.xml.h:6736
inline constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_OFFSET__MASK = 0x001ff000;            // a6xx.xml.h:6741
inline constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_OFFSET__SHIFT = 12;                   // a6xx.xml.h:6742
inline constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_OFFSET__SCALE = 12;                   // a6xx.xml.h:6743  the encoder's val >> 12
inline constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_CACHE_SIZE__MASK = 0x00600000;        // a6xx.xml.h:6748
inline constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_CACHE_SIZE__SHIFT = 21;               // a6xx.xml.h:6749
inline constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_OFFSET__MASK = 0xff800000;            // a6xx.xml.h:6754
inline constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_OFFSET__SHIFT = 23;                   // a6xx.xml.h:6755
inline constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_OFFSET__SCALE = 12;                   // a6xx.xml.h:6756  the encoder's val >> 12

enum a6xx_ccu_cache_size : uint32_t {
    CCU_CACHE_SIZE_FULL = 0,       // a6xx.xml.h:2266
    CCU_CACHE_SIZE_HALF = 1,       // a6xx.xml.h:2267
    CCU_CACHE_SIZE_QUARTER = 2,    // a6xx.xml.h:2268  this chip's gmem_ccu_color_cache_fraction (tiled rendering only)
    CCU_CACHE_SIZE_EIGHTH = 3,     // a6xx.xml.h:2269
};

constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_OFFSET(uint32_t bytes) {
    return field(bytes >> A6XX_RB_CCU_CNTL_DEPTH_OFFSET__SCALE, A6XX_RB_CCU_CNTL_DEPTH_OFFSET__SHIFT, A6XX_RB_CCU_CNTL_DEPTH_OFFSET__MASK);
}
constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_OFFSET(uint32_t bytes) {
    return field(bytes >> A6XX_RB_CCU_CNTL_COLOR_OFFSET__SCALE, A6XX_RB_CCU_CNTL_COLOR_OFFSET__SHIFT, A6XX_RB_CCU_CNTL_COLOR_OFFSET__MASK);
}
constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_OFFSET_HI(uint32_t v) {
    return field(v, A6XX_RB_CCU_CNTL_DEPTH_OFFSET_HI__SHIFT, A6XX_RB_CCU_CNTL_DEPTH_OFFSET_HI__MASK);
}
constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI(uint32_t v) {
    return field(v, A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__SHIFT, A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI__MASK);
}
constexpr uint32_t A6XX_RB_CCU_CNTL_DEPTH_CACHE_SIZE(a6xx_ccu_cache_size v) {
    return field(v, A6XX_RB_CCU_CNTL_DEPTH_CACHE_SIZE__SHIFT, A6XX_RB_CCU_CNTL_DEPTH_CACHE_SIZE__MASK);
}
constexpr uint32_t A6XX_RB_CCU_CNTL_COLOR_CACHE_SIZE(a6xx_ccu_cache_size v) {
    return field(v, A6XX_RB_CCU_CNTL_COLOR_CACHE_SIZE__SHIFT, A6XX_RB_CCU_CNTL_COLOR_CACHE_SIZE__MASK);
}
constexpr uint32_t A6XX_SP_TP_MODE_CNTL_ISAMMODE(a6xx_isam_mode v) { return field(v, A6XX_SP_TP_MODE_CNTL_ISAMMODE__SHIFT, A6XX_SP_TP_MODE_CNTL_ISAMMODE__MASK); }
constexpr uint32_t A6XX_SP_TP_MODE_CNTL_UNK3(uint32_t v) { return field(v, A6XX_SP_TP_MODE_CNTL_UNK3__SHIFT, A6XX_SP_TP_MODE_CNTL_UNK3__MASK); }
constexpr uint32_t A6XX_GRAS_SC_CNTL_CCUSINGLECACHELINESIZE(uint32_t v) {
    return field(v, A6XX_GRAS_SC_CNTL_CCUSINGLECACHELINESIZE__SHIFT, A6XX_GRAS_SC_CNTL_CCUSINGLECACHELINESIZE__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_5_REG_LINELENGTHREGID(uint32_t regid) {
    return field(regid, A6XX_HLSQ_CONTROL_5_REG_LINELENGTHREGID__SHIFT, A6XX_HLSQ_CONTROL_5_REG_LINELENGTHREGID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_5_REG_FOVEATIONQUALITYREGID(uint32_t regid) {
    return field(regid, A6XX_HLSQ_CONTROL_5_REG_FOVEATIONQUALITYREGID__SHIFT, A6XX_HLSQ_CONTROL_5_REG_FOVEATIONQUALITYREGID__MASK);
}
constexpr uint32_t A6XX_VFD_MODE_CNTL_RENDER_MODE(a6xx_render_mode v) {
    return field(v, A6XX_VFD_MODE_CNTL_RENDER_MODE__SHIFT, A6XX_VFD_MODE_CNTL_RENDER_MODE__MASK);
}
constexpr uint32_t A6XX_PC_RASTER_CNTL_STREAM(uint32_t v) { return field(v, A6XX_PC_RASTER_CNTL_STREAM__SHIFT, A6XX_PC_RASTER_CNTL_STREAM__MASK); }
constexpr uint32_t A6XX_UCHE_CLIENT_PF_PERFSEL(uint32_t v) { return field(v, A6XX_UCHE_CLIENT_PF_PERFSEL__SHIFT, A6XX_UCHE_CLIENT_PF_PERFSEL__MASK); }

// the registers of array entries
constexpr uint32_t vfdFetchSize(uint32_t i) { return REG_A6XX_VFD_FETCH_SIZE + REG_A6XX_VFD_FETCH__STRIDE * i; }
constexpr uint32_t cpScratchReg(uint32_t i) { return REG_A6XX_CP_SCRATCH_REG + REG_A6XX_CP_SCRATCH_REG__STRIDE * i; }

// the whole values Gallium writes
constexpr uint32_t spTpModeCntl() { return A6XX_SP_TP_MODE_CNTL_UNK3(0x28) | A6XX_SP_TP_MODE_CNTL_ISAMMODE(ISAMMODE_GL); }  // 0xa0 | GL
constexpr uint32_t grasScCntl() { return A6XX_GRAS_SC_CNTL_CCUSINGLECACHELINESIZE(2); }
constexpr uint32_t hlsqControl5Reg() {
    return A6XX_HLSQ_CONTROL_5_REG_LINELENGTHREGID(kRegidNone) | A6XX_HLSQ_CONTROL_5_REG_FOVEATIONQUALITYREGID(kRegidNone);
}
constexpr uint32_t spPerfctrEnableAll() {
    return A6XX_SP_PERFCTR_ENABLE_VS | A6XX_SP_PERFCTR_ENABLE_HS | A6XX_SP_PERFCTR_ENABLE_DS | A6XX_SP_PERFCTR_ENABLE_GS |
           A6XX_SP_PERFCTR_ENABLE_FS | A6XX_SP_PERFCTR_ENABLE_CS;
}
// HLSQ_INVALIDATE_CMD as the batch start writes it: every kind of pending load dropped, both shared-const kinds, all
// five bindless bases of each pipeline (the A6XX masks are 5 bits wide)
constexpr uint32_t hlsqInvalidateCmdAll() {
    return A6XX_HLSQ_INVALIDATE_CMD_VS_STATE | A6XX_HLSQ_INVALIDATE_CMD_HS_STATE | A6XX_HLSQ_INVALIDATE_CMD_DS_STATE |
           A6XX_HLSQ_INVALIDATE_CMD_GS_STATE | A6XX_HLSQ_INVALIDATE_CMD_FS_STATE | A6XX_HLSQ_INVALIDATE_CMD_CS_STATE |
           A6XX_HLSQ_INVALIDATE_CMD_CS_IBO | A6XX_HLSQ_INVALIDATE_CMD_GFX_IBO | A6XX_HLSQ_INVALIDATE_CMD_CS_SHARED_CONST |
           A6XX_HLSQ_INVALIDATE_CMD_GFX_SHARED_CONST |
           field(0x1f, A6XX_HLSQ_INVALIDATE_CMD_CS_BINDLESS__SHIFT, A6XX_HLSQ_INVALIDATE_CMD_CS_BINDLESS__MASK) |
           field(0x1f, A6XX_HLSQ_INVALIDATE_CMD_GFX_BINDLESS__SHIFT, A6XX_HLSQ_INVALIDATE_CMD_GFX_BINDLESS__MASK);
}
// RB_CCU_CNTL for rendering to memory on this chip: depth cache at 0, color cache after it, both full size
// (the color offset is the depth cache's size: num_ccu * sysmem_per_ccu_depth_cache_size; the offsets' bit 21 goes
// in the _HI fields, which 0x20000 does not reach)
constexpr uint32_t rbCcuCntlSysmem(uint32_t colorOffset = kNumCcu * kSysmemPerCcuDepthCacheSize) {
    return (kHasGmemFastClear ? 0u : A6XX_RB_CCU_CNTL_GMEM_FAST_CLEAR_DISABLE) |
           (kConcurrentResolve ? A6XX_RB_CCU_CNTL_CONCURRENT_RESOLVE : 0u) | A6XX_RB_CCU_CNTL_DEPTH_OFFSET_HI(0) |
           A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI(colorOffset >> 21) | A6XX_RB_CCU_CNTL_DEPTH_CACHE_SIZE(CCU_CACHE_SIZE_FULL) |
           A6XX_RB_CCU_CNTL_DEPTH_OFFSET(0) | A6XX_RB_CCU_CNTL_COLOR_CACHE_SIZE(CCU_CACHE_SIZE_FULL) |
           A6XX_RB_CCU_CNTL_COLOR_OFFSET(colorOffset & 0x1fffff);
}

// fd6_emit_static_regs, A6XX, this chip: up to CP_SET_DRAW_STATE
inline constexpr RegWrite kStaticState[] = {
    {REG_A6XX_RB_DBG_ECO_CNTL, kMagicRbDbgEcoCntl},
    {REG_A6XX_SP_FLOAT_CNTL, A6XX_SP_FLOAT_CNTL_F16_NO_INF},   // computerator's dispatch prelude then writes 0 here
    {REG_A6XX_SP_DBG_ECO_CNTL, kMagicSpDbgEcoCntl},
    {REG_A6XX_SP_PERFCTR_ENABLE, spPerfctrEnableAll()},
    {REG_A6XX_TPL1_UNKNOWN_B605, 0x44},
    {REG_A6XX_TPL1_DBG_ECO_CNTL, kMagicTpl1DbgEcoCntl},
    {REG_A6XX_HLSQ_UNKNOWN_BE00, 0x80},
    {REG_A6XX_HLSQ_UNKNOWN_BE01, 0},
    {REG_A6XX_VPC_DBG_ECO_CNTL, kMagicVpcDbgEcoCntl},
    {REG_A6XX_GRAS_DBG_ECO_CNTL, kMagicGrasDbgEcoCntl},
    {REG_A6XX_HLSQ_DBG_ECO_CNTL, kMagicHlsqDbgEcoCntl},
    {REG_A6XX_SP_CHICKEN_BITS, kMagicSpChickenBits},
    {REG_A6XX_SP_IBO_COUNT, 0},
    {REG_A6XX_SP_UNKNOWN_B182, 0},
    {REG_A6XX_HLSQ_SHARED_CONSTS, 0},
    {REG_A6XX_UCHE_UNKNOWN_0E12, kMagicUcheUnknown0E12},
    {REG_A6XX_UCHE_CLIENT_PF, kMagicUcheClientPf},
    {REG_A6XX_RB_UNKNOWN_8E01, kMagicRbUnknown8E01},
    {REG_A6XX_SP_UNKNOWN_A9A8, 0},
    {REG_A6XX_SP_MODE_CONTROL, A6XX_SP_MODE_CONTROL_CONSTANT_DEMOTION_ENABLE | A6XX_SP_MODE_CONTROL_ISAMMODE(ISAMMODE_GL)},
    {REG_A6XX_VFD_ADD_OFFSET, A6XX_VFD_ADD_OFFSET_VERTEX},
    {REG_A6XX_VPC_UNKNOWN_9107, 0},
    {REG_A6XX_RB_UNKNOWN_8811, 0x10},
    {REG_A6XX_PC_MODE_CNTL, kMagicPcModeCntl},
    {REG_A6XX_GRAS_LRZ_PS_INPUT_CNTL, 0},
    {REG_A6XX_GRAS_SAMPLE_CNTL, 0},
    {REG_A6XX_GRAS_UNKNOWN_8110, 0x2},
    {REG_A6XX_RB_UNKNOWN_8818, 0},
    {REG_A6XX_RB_UNKNOWN_8819, 0},
    {REG_A6XX_RB_UNKNOWN_881A, 0},
    {REG_A6XX_RB_UNKNOWN_881B, 0},
    {REG_A6XX_RB_UNKNOWN_881C, 0},
    {REG_A6XX_RB_UNKNOWN_881D, 0},
    {REG_A6XX_RB_UNKNOWN_881E, 0},
    {REG_A6XX_RB_UNKNOWN_88F0, 0},
    {REG_A6XX_VPC_POINT_COORD_INVERT, 0},
    {REG_A6XX_VPC_UNKNOWN_9300, 0},
    {REG_A6XX_VPC_SO_DISABLE, A6XX_VPC_SO_DISABLE_DISABLE},
    {REG_A6XX_PC_RASTER_CNTL, 0},
    {REG_A6XX_PC_MULTIVIEW_CNTL, 0},
    {REG_A6XX_SP_UNKNOWN_B183, 0},
    {REG_A6XX_GRAS_SU_CONSERVATIVE_RAS_CNTL, 0},
    {REG_A6XX_GRAS_VS_LAYER_CNTL, 0},
    {REG_A6XX_GRAS_SC_CNTL, grasScCntl()},
    {REG_A6XX_GRAS_UNKNOWN_80AF, 0},
    {REG_A6XX_VPC_UNKNOWN_9210, 0},
    {REG_A6XX_VPC_UNKNOWN_9211, 0},
    {REG_A6XX_VPC_UNKNOWN_9602, 0},
    {REG_A6XX_PC_UNKNOWN_9E72, 0},
    {REG_A6XX_SP_TP_MODE_CNTL, spTpModeCntl()},
    {REG_A6XX_HLSQ_CONTROL_5_REG, hlsqControl5Reg()},
    {REG_A6XX_VFD_MODE_CNTL, A6XX_VFD_MODE_CNTL_RENDER_MODE(RENDERING_PASS)},
    {REG_A6XX_VFD_MULTIVIEW_CNTL, 0},
};
// after CP_SET_DRAW_STATE (GRAS_LRZ_CNTL and RB_LRZ_CNTL are written twice, as Gallium does)
inline constexpr RegWrite kStaticStateAfterDrawState[] = {
    {REG_A6XX_VPC_SO_STREAM_CNTL, 0},
    {REG_A6XX_GRAS_LRZ_CNTL, 0},
    {REG_A6XX_RB_LRZ_CNTL, 0},
    {REG_A6XX_RB_DEPTH_PLANE_CNTL, 0},
    {REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL, 0},
    {REG_A6XX_GRAS_LRZ_CNTL, 0},
    {REG_A6XX_RB_LRZ_CNTL, 0},
};
// after the 32 VFD_FETCH sizes and the two border color addresses
inline constexpr RegWrite kStaticStateTail[] = {
    {REG_A6XX_PC_DGEN_SU_CONSERVATIVE_RAS_CNTL, 0},
};

// CP_SET_DRAW_STATE, 3 words per group: Gallium's batch start sends one group with DISABLE_ALL_GROUPS and no address,
// to drop any state group a previous owner of the GPU left pending
inline constexpr uint32_t CP_SET_DRAW_STATE__0_COUNT__MASK = 0x0000ffff;               // adreno_pm4.xml.h:1362
inline constexpr uint32_t CP_SET_DRAW_STATE__0_COUNT__SHIFT = 0;                       // adreno_pm4.xml.h:1363
inline constexpr uint32_t CP_SET_DRAW_STATE__0_DIRTY = 0x00010000;                     // adreno_pm4.xml.h:1368
inline constexpr uint32_t CP_SET_DRAW_STATE__0_DISABLE = 0x00020000;                   // adreno_pm4.xml.h:1369
inline constexpr uint32_t CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS = 0x00040000;        // adreno_pm4.xml.h:1370
inline constexpr uint32_t CP_SET_DRAW_STATE__0_LOAD_IMMED = 0x00080000;                // adreno_pm4.xml.h:1371
inline constexpr uint32_t CP_SET_DRAW_STATE__0_BINNING = 0x00100000;                   // adreno_pm4.xml.h:1372
inline constexpr uint32_t CP_SET_DRAW_STATE__0_GMEM = 0x00200000;                      // adreno_pm4.xml.h:1373
inline constexpr uint32_t CP_SET_DRAW_STATE__0_SYSMEM = 0x00400000;                    // adreno_pm4.xml.h:1374
inline constexpr uint32_t CP_SET_DRAW_STATE__0_GROUP_ID__MASK = 0x1f000000;            // adreno_pm4.xml.h:1375
inline constexpr uint32_t CP_SET_DRAW_STATE__0_GROUP_ID__SHIFT = 24;                   // adreno_pm4.xml.h:1376
inline constexpr uint32_t CP_SET_DRAW_STATE__1_ADDR_LO__MASK = 0xffffffff;             // adreno_pm4.xml.h:1384
inline constexpr uint32_t CP_SET_DRAW_STATE__2_ADDR_HI__MASK = 0xffffffff;             // adreno_pm4.xml.h:1393
constexpr uint32_t CP_SET_DRAW_STATE__0_COUNT(uint32_t v) { return field(v, CP_SET_DRAW_STATE__0_COUNT__SHIFT, CP_SET_DRAW_STATE__0_COUNT__MASK); }
constexpr uint32_t CP_SET_DRAW_STATE__0_GROUP_ID(uint32_t v) {
    return field(v, CP_SET_DRAW_STATE__0_GROUP_ID__SHIFT, CP_SET_DRAW_STATE__0_GROUP_ID__MASK);
}
constexpr uint32_t setDrawState0(uint32_t count, uint32_t flags, uint32_t groupId) {
    return CP_SET_DRAW_STATE__0_COUNT(count) | flags | CP_SET_DRAW_STATE__0_GROUP_ID(groupId);
}

// CP_SET_AMBLE, 3 words: an IB (address, then its length and kind) the CP runs by itself around a context switch.
// Gallium sets the BIN_PREAMBLE to its preemption preamble (the static registers again, RB_CCU_CNTL, PC_POWER_CNTL
// and VFD_POWER_CNTL = kMagicPcPowerCntl, and a CP_MEM_TO_REG restoring the VSC state) and clears PREAMBLE and
// POSTAMBLE (address 0, 0 dwords). A batch that is never preempted does not need any of the three.
inline constexpr uint32_t CP_SET_AMBLE_0_ADDR_LO__MASK = 0xffffffff;                   // adreno_pm4.xml.h:2843
inline constexpr uint32_t CP_SET_AMBLE_1_ADDR_HI__MASK = 0xffffffff;                   // adreno_pm4.xml.h:2852
inline constexpr uint32_t CP_SET_AMBLE_2_DWORDS__MASK = 0x000fffff;                    // adreno_pm4.xml.h:2861
inline constexpr uint32_t CP_SET_AMBLE_2_DWORDS__SHIFT = 0;                            // adreno_pm4.xml.h:2862
inline constexpr uint32_t CP_SET_AMBLE_2_TYPE__MASK = 0x00300000;                      // adreno_pm4.xml.h:2867
inline constexpr uint32_t CP_SET_AMBLE_2_TYPE__SHIFT = 20;                             // adreno_pm4.xml.h:2868

enum amble_type : uint32_t {
    PREAMBLE_AMBLE_TYPE = 0,       // adreno_pm4.xml.h:566  run whenever the CP switches back to this context
    BIN_PREAMBLE_AMBLE_TYPE = 1,   // adreno_pm4.xml.h:567  run when switching back during a tiled render with skipsaverestore
    POSTAMBLE_AMBLE_TYPE = 2,      // adreno_pm4.xml.h:568
    KMD_AMBLE_TYPE = 3,            // adreno_pm4.xml.h:569  the kernel's
};

constexpr uint32_t CP_SET_AMBLE_2_DWORDS(uint32_t v) { return field(v, CP_SET_AMBLE_2_DWORDS__SHIFT, CP_SET_AMBLE_2_DWORDS__MASK); }
constexpr uint32_t CP_SET_AMBLE_2_TYPE(amble_type v) { return field(v, CP_SET_AMBLE_2_TYPE__SHIFT, CP_SET_AMBLE_2_TYPE__MASK); }

// CP_MEM_TO_REG, 3 words (the preemption preamble's VSC restore): CNT registers from REG are loaded from the address
inline constexpr uint32_t CP_MEM_TO_REG_0_REG__MASK = 0x0003ffff;                      // adreno_pm4.xml.h:1741
inline constexpr uint32_t CP_MEM_TO_REG_0_REG__SHIFT = 0;                              // adreno_pm4.xml.h:1742
inline constexpr uint32_t CP_MEM_TO_REG_0_CNT__MASK = 0x3ff80000;                      // adreno_pm4.xml.h:1747
inline constexpr uint32_t CP_MEM_TO_REG_0_CNT__SHIFT = 19;                             // adreno_pm4.xml.h:1748
inline constexpr uint32_t CP_MEM_TO_REG_0_SHIFT_BY_2 = 0x40000000;                     // adreno_pm4.xml.h:1753

}  // namespace hakios::metal::a6xx
