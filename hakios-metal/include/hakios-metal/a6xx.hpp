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
    TILE6_2 = 2,                   // a6xx.xml.h:58  GMEM's own layout: a fetch from a GMEM attachment reads through it (tu_emit_input_attachments)
    TILE6_3 = 3,                   // a6xx.xml.h:59  the depth buffer's layout: the hardware has no linear one (fd6_layout.c:157-164)
};

// The texture formats served (texture.hpp's table), as fd6_format_table.c's .tex column names them for the Metal
// pixel formats: the sRGB ones are the UNORM format plus TEX_CONST_0_SRGB, BGRA8 is 8_8_8_8_UNORM with SWAP WXYZ.
enum a6xx_format : uint32_t {
    FMT6_A8_UNORM = 2,             // a6xx.xml.h:63   A8Unorm: the TP returns (0, 0, 0, a) and the RB writes alpha (fd6_format_table.c:53 _TC)
    FMT6_8_UNORM = 3,              // a6xx.xml.h:64   R8Unorm
    FMT6_5_5_5_1_UNORM = 10,       // a6xx.xml.h:69   BGR5A1Unorm with SWAP = WXYZ (fd6_format_table.c:111 _TC(B5G5R5A1_UNORM))
    FMT6_5_6_5_UNORM = 14,         // a6xx.xml.h:71   B5G6R5Unorm with SWAP = WXYZ (fd6_format_table.c:108 _TC(B5G6R5_UNORM))
    FMT6_8_UINT = 5,               // a6xx.xml.h:66   a stencil plane (S8_UINT: fd6_format_table.c, the 2D engine's clear of it)
    FMT6_16_UNORM = 21,            // a6xx.xml.h:77   R16Unorm (fd6_format_table.c:70 VTC)
    FMT6_16_UINT = 24,             // a6xx.xml.h:80   R16Uint (:72 VTC)
    FMT6_16_SINT = 25,             // a6xx.xml.h:81   R16Sint (:73 VTC)
    FMT6_16_FLOAT = 23,            // a6xx.xml.h:79   R16Float (fd6_format_table.c:76 VTC)
    FMT6_8_8_UNORM = 15,           // a6xx.xml.h:72   RG8Unorm
    FMT6_8_8_8_8_UNORM = 48,       // a6xx.xml.h:86   RGBA8Unorm, RGBA8Unorm_sRGB, and with SWAP = WXYZ the BGRA8 pair
    FMT6_32_FLOAT = 74,            // a6xx.xml.h:105  R32Float
    FMT6_32_UINT = 75,             // a6xx.xml.h:106  a raw buffer: one dword per element; R32Uint
    FMT6_10_10_10_2_UNORM = 54,    // a6xx.xml.h:92  RGB10A2 to the TP (fd6_format_table.c:202 FMT(R10G10B10A2_UNORM, ..., 10_10_10_2_UNORM_DEST))
    FMT6_10_10_10_2_UNORM_DEST = 55,   // a6xx.xml.h:93  ...and to the RB and the 2D engine's destination: the colour column
    FMT6_11_11_10_FLOAT = 66,      // a6xx.xml.h:97  R11G11B10 float, red in the low bits (fd6_format_table.c:216 VTC(R11G11B10_FLOAT, 11_11_10_FLOAT, WZYX))
    FMT6_16_16_FLOAT = 69,         // a6xx.xml.h:100  a half2 vertex attribute (fd6_format_table.c:155 VTC(R16G16_FLOAT, 16_16_FLOAT, WZYX))
    FMT6_16_16_16_16_FLOAT = 98,   // a6xx.xml.h:116  RGBA16Float
    FMT6_32_32_FLOAT = 103,        // a6xx.xml.h:121  a float2 vertex attribute (fd6_format_table.c:258 VTC(R32G32_FLOAT, 32_32_FLOAT, WZYX))
    FMT6_32_32_UINT = 104,         // a6xx.xml.h:122
    FMT6_32_32_32_FLOAT = 116,     // a6xx.xml.h:129  a float3 attribute
    FMT6_32_32_32_32_FLOAT = 130,  // a6xx.xml.h:133  RGBA32Float
    FMT6_R8_G8B8_2PLANE_420_UNORM = 142,   // a6xx.xml.h:139  NV12: Y and an interleaved CbCr plane at half size (fd6_format_table.c:357-358)
    FMT6_32_32_32_32_UINT = 131,   // a6xx.xml.h:134  RGBA32Uint
    // vertex attribute formats (render.cpp's kVertexFormats; fd6_format_table.c's V column)
    FMT6_8_SNORM = 4,              // a6xx.xml.h:65
    FMT6_8_SINT = 6,               // a6xx.xml.h:67
    FMT6_8_8_SNORM = 16,           // a6xx.xml.h:73
    FMT6_8_8_UINT = 17,            // a6xx.xml.h:74
    FMT6_8_8_SINT = 18,            // a6xx.xml.h:75
    FMT6_8_8_8_UNORM = 33,         // a6xx.xml.h:82
    FMT6_8_8_8_SNORM = 34,         // a6xx.xml.h:83
    FMT6_8_8_8_UINT = 35,          // a6xx.xml.h:84
    FMT6_8_8_8_SINT = 36,          // a6xx.xml.h:85
    FMT6_8_8_8_8_SNORM = 50,       // a6xx.xml.h:88
    FMT6_8_8_8_8_UINT = 51,        // a6xx.xml.h:89
    FMT6_8_8_8_8_SINT = 52,        // a6xx.xml.h:90
    FMT6_16_SNORM = 22,            // a6xx.xml.h:78
    FMT6_16_16_UNORM = 67,         // a6xx.xml.h:98
    FMT6_16_16_SNORM = 68,         // a6xx.xml.h:99
    FMT6_16_16_UINT = 70,          // a6xx.xml.h:101
    FMT6_16_16_SINT = 71,          // a6xx.xml.h:102
    FMT6_16_16_16_UNORM = 88,      // a6xx.xml.h:109
    FMT6_16_16_16_SNORM = 89,      // a6xx.xml.h:110
    FMT6_16_16_16_FLOAT = 90,      // a6xx.xml.h:111
    FMT6_16_16_16_UINT = 91,       // a6xx.xml.h:112
    FMT6_16_16_16_SINT = 92,       // a6xx.xml.h:113
    FMT6_16_16_16_16_UNORM = 96,   // a6xx.xml.h:114
    FMT6_16_16_16_16_SNORM = 97,   // a6xx.xml.h:115
    FMT6_16_16_16_16_UINT = 99,    // a6xx.xml.h:117
    FMT6_16_16_16_16_SINT = 100,   // a6xx.xml.h:118
    FMT6_32_SINT = 76,             // a6xx.xml.h:107
    FMT6_32_32_SINT = 105,         // a6xx.xml.h:123
    FMT6_32_32_32_UINT = 114,      // a6xx.xml.h:127
    FMT6_32_32_32_SINT = 115,      // a6xx.xml.h:128
    FMT6_32_32_32_32_SINT = 132,   // a6xx.xml.h:135
    // ASTC (LDR and sRGB alike: TEX_CONST_0.SRGB tells them apart), sampled only (fd6_format_table.c's _T_ rows)
    // ETC2 and EAC, 4 x 4 blocks of 8 or 16 bytes: sampled alone (fd6_format_table.c:290-299, _T_ rows)
    FMT6_ETC2_RG11_UNORM = 171,    // a6xx.xml.h:156
    FMT6_ETC2_RG11_SNORM = 172,
    FMT6_ETC2_R11_UNORM = 173,
    FMT6_ETC2_R11_SNORM = 174,
    FMT6_ETC2_RGB8 = 176,          // a6xx.xml.h:161
    FMT6_ETC2_RGBA8 = 177,
    FMT6_ETC2_RGB8A1 = 178,
    FMT6_ASTC_4x4 = 193,           // a6xx.xml.h:174
    FMT6_ASTC_5x4 = 194,           // a6xx.xml.h:175
    FMT6_ASTC_5x5 = 195,           // a6xx.xml.h:176
    FMT6_ASTC_6x5 = 196,           // a6xx.xml.h:177
    FMT6_ASTC_6x6 = 197,           // a6xx.xml.h:178
    FMT6_ASTC_8x5 = 198,           // a6xx.xml.h:179
    FMT6_ASTC_8x6 = 199,           // a6xx.xml.h:180
    FMT6_ASTC_8x8 = 200,           // a6xx.xml.h:181
    FMT6_ASTC_10x5 = 201,          // a6xx.xml.h:182
    FMT6_ASTC_10x6 = 202,          // a6xx.xml.h:183
    FMT6_ASTC_10x8 = 203,          // a6xx.xml.h:184
    FMT6_ASTC_10x10 = 204,         // a6xx.xml.h:185
    FMT6_ASTC_12x10 = 205,         // a6xx.xml.h:186
    FMT6_ASTC_12x12 = 206,         // a6xx.xml.h:187
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
    UNK_2C = 44,                   // adreno_pm4.xml.h:106  before the binning pass's draws (emit_binning_pass)
    UNK_2D = 45,                   // adreno_pm4.xml.h:107  ...and after them
    LRZ_FLUSH = 38,                // adreno_pm4.xml.h:99   both witnesses end a sysmem pass with it (tu_lrz.cc:465; fd6_gmem.cc:2114)
    CACHE_INVALIDATE = 49,         // adreno_pm4.xml.h:108  invalidate UCHE: before reading what the CPU or a blit wrote
    LABEL = 63,                    // adreno_pm4.xml.h:109  a no-op event; Gallium's dummy to roll the register context
};

// CP_SET_MARKER's MODE: RM6_DIRECT_RENDER before a sysmem pass (tu_cmd_buffer.cc:2130-2131; fd6_gmem.cc:2041-2042);
// there is no RM6_BYPASS in this Mesa, value 1 is this name (adreno_pm4.xml:1797-1798)
enum a6xx_marker : uint32_t {
    RM6_DIRECT_RENDER = 1,         // adreno_pm4.xml.h:519
    RM6_BIN_VISIBILITY = 2,        // adreno_pm4.xml.h:520  the binning pass (emit_binning_pass)
    RM6_BIN_RENDER_START = 4,      // adreno_pm4.xml.h:522  a GMEM tile's draws begin (tu6_emit_tile_select)
    RM6_BIN_RESOLVE = 6,           // adreno_pm4.xml.h:524  the tile's store to memory (tu6_emit_tile_store)
    RM6_BIN_RENDER_END = 7,        // adreno_pm4.xml.h:525
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
    CP_SET_BIN_DATA5 = 0x2f,       // adreno_pm4.xml.h:227  a tile's visibility streams by address, 7 words (fd6_emit_tile_prep): hangs this CP
    CP_SET_BIN_DATA5_OFFSET = 0x2e,   // adreno_pm4.xml.h:329  ...by offsets from the VSC_*_ADDRESS registers, 4 words (tu6_emit_tile_select)
    CP_WAIT_MEM_GTE = 0x14,        // adreno_pm4.xml.h:326  wait until a memory word >= REF (signed)
    CP_SKIP_IB2_ENABLE_GLOBAL = 0x1d,  // adreno_pm4.xml.h:294  1 word, 0 at pass begin and end (tu_cmd_buffer.cc:2134-2135, 2161-2162); undocumented
    CP_SKIP_IB2_ENABLE_LOCAL = 0x23,   // adreno_pm4.xml.h:295  1 word, 1: fd6 only ("I think that is not required", fd6_gmem.cc:2048)
    CP_WAIT_FOR_IDLE = 0x26,       // adreno_pm4.xml.h:219
    CP_BLIT = 0x2c,                // adreno_pm4.xml.h:305  the 2D engine's run: a loadAction Clear's solid fill (r2d_run, tu_clear_blit.cc:551-570)
    CP_LOAD_STATE6_GEOM = 0x32,    // adreno_pm4.xml.h:308  the VS's loads (tu6_stage2opcode: VS -> GEOM, FS -> FRAG, tu_shader.cc:1349)
    CP_EXEC_CS = 0x33,             // adreno_pm4.xml.h:287
    CP_LOAD_STATE6_FRAG = 0x34,    // adreno_pm4.xml.h:309  what compute uses, for its shader, constants, IBOs and textures
    CP_LOAD_STATE6 = 0x36,         // adreno_pm4.xml.h:310
    CP_DRAW_INDX_OFFSET = 0x38,    // adreno_pm4.xml.h:272  the draw: 3 words without an index buffer (adreno_pm4.xml:906-916)
    CP_DRAW_INDIRECT_MULTI = 0x2a, // adreno_pm4.xml.h:275  an indirect draw, its words read from memory as it runs
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
inline constexpr uint32_t A6XX_TEX_CONST_3_MIN_LAYERSZ__SCALE = 12;                     // a6xx.xml.h:11413  the encoder's val >> 12
inline constexpr uint32_t A6XX_TEX_CONST_3_TILE_ALL = 0x08000000;                       // a6xx.xml.h:11418
inline constexpr uint32_t A6XX_TEX_CONST_3_FLAG = 0x10000000;                           // a6xx.xml.h:11419
// a 3D texture's last level's slice size, in 4 KiB (fd6_view.c:308-310)
constexpr uint32_t A6XX_TEX_CONST_3_MIN_LAYERSZ(uint32_t bytes) {
    return field(bytes >> A6XX_TEX_CONST_3_MIN_LAYERSZ__SCALE, A6XX_TEX_CONST_3_MIN_LAYERSZ__SHIFT, A6XX_TEX_CONST_3_MIN_LAYERSZ__MASK);
}
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

// A two-plane YUV texture's second plane: its pitch in bytes, the field the MIPLVLS-free layout reuses (fd6_view.c:280-281)
inline constexpr uint32_t A6XX_TEX_CONST_6_PLANE_PITCH__MASK = 0xffffff00;              // a6xx.xml.h:11454
inline constexpr uint32_t A6XX_TEX_CONST_6_PLANE_PITCH__SHIFT = 8;                      // a6xx.xml.h:11455
constexpr uint32_t A6XX_TEX_CONST_6_PLANE_PITCH(uint32_t bytes) {
    return field(bytes, A6XX_TEX_CONST_6_PLANE_PITCH__SHIFT, A6XX_TEX_CONST_6_PLANE_PITCH__MASK);
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
// (LEVELS mip levels from the one at IOVA, whose size and pitch the others follow by the layout's rule -- the TP finds
// them itself, fdl6_view_init gives it only the first; LAYER_BYTES the whole layer, 0: one level's, layerBytes2d)
constexpr TexConst texture2d(uint64_t iova, uint32_t width, uint32_t height, uint32_t pitchBytes, uint32_t pitchalignLog2Cpp,
                             a6xx_format fmt, a3xx_color_swap swap, bool hasAlpha, bool srgb, uint32_t levels = 1, uint64_t layerBytes = 0) {
    TexConst d{};
    d.dword[0] = A6XX_TEX_CONST_0_TILE_MODE(TILE6_LINEAR) | (srgb ? A6XX_TEX_CONST_0_SRGB : 0u) | A6XX_TEX_CONST_0_FMT(fmt) |
                 A6XX_TEX_CONST_0_SAMPLES(MSAA_ONE) | A6XX_TEX_CONST_0_SWAP(swap) | texSwizzle(hasAlpha) | A6XX_TEX_CONST_0_MIPLVLS(levels - 1);
    d.dword[1] = A6XX_TEX_CONST_1_WIDTH(width) | A6XX_TEX_CONST_1_HEIGHT(height);
    d.dword[2] = A6XX_TEX_CONST_2_PITCHALIGN(pitchalignLog2Cpp) | A6XX_TEX_CONST_2_PITCH(pitchBytes) | A6XX_TEX_CONST_2_TYPE(A6XX_TEX_2D);
    d.dword[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(uint32_t(layerBytes != 0 ? layerBytes : layerBytes2d(height, pitchBytes)));
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

// CP_EXEC_CS_INDIRECT word 3 (A5XX_CP_EXEC_CS_INDIRECT_3, adreno_pm4.xml.h:2574-2584, the form turnip emits on a6xx,
// tu_cmd_buffer.cc:7002-7008): the local size, each axis minus one, beside the group counts' address in words 1-2
inline constexpr uint32_t A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX__MASK = 0x00000ffc;
inline constexpr uint32_t A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX__SHIFT = 2;
inline constexpr uint32_t A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY__MASK = 0x003ff000;
inline constexpr uint32_t A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY__SHIFT = 12;
inline constexpr uint32_t A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ__MASK = 0xffc00000;
inline constexpr uint32_t A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ__SHIFT = 22;
constexpr uint32_t execCsIndirect3(uint32_t localX, uint32_t localY, uint32_t localZ) {
    return field(localX - 1, A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX__SHIFT, A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX__MASK) |
           field(localY - 1, A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY__SHIFT, A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY__MASK) |
           field(localZ - 1, A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ__SHIFT, A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ__MASK);
}

// CP_DRAW_INDIRECT_MULTI (adreno_pm4.xml.h:275, 448-449, 1209-1216): word 1's opcode and DST_OFF, the vec4 of the VS's
// constants the CP writes the draw's params to (draw id, vertex offset, first instance; 0 writes none)
enum a6xx_indirect_op : uint32_t { INDIRECT_OP_NORMAL = 2, INDIRECT_OP_INDEXED = 4 };
constexpr uint32_t drawIndirectMulti1(a6xx_indirect_op op, uint32_t dstOffVec4) {
    return field(op, 0, 0x0000000fu) | field(dstOffVec4, 8, 0x003fff00u);
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
//   every entry of kStaticStateTail (fd6's PC_DGEN_SU_CONSERVATIVE_RAS_CNTL, then RB_ALPHA_CONTROL 0, which only
//   turnip's table writes and a draw depends on: the render contract's batch start); then CP_WAIT_FOR_IDLE and
//   REG_A6XX_RB_CCU_CNTL = rbCcuCntlSysmem() (fd6_emit_ccu_cntl's A6XX path: the wait first, "early a6xx needed
//   this"). Gallium ends with three CP_SET_AMBLE (3 words each) telling the CP what to run around a preemption; they
//   are optional for a batch that is not preempted (see below). Where the two witnesses' tables differ the table
//   keeps fd6's value (SP_FLOAT_CNTL F16_NO_INF where turnip writes 0; GRAS_UNKNOWN_8110 2 where turnip writes 0);
//   neither is measured, both are --bisect items.
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
// tiled rendering: the on-chip memory and the tile grid (a6xx_gen4 / adreno_7c3 in freedreno_devices.py)
inline constexpr uint32_t kGmemBytes = 512 * 1024;                // not-in-headers: freedreno_devices.py adreno_7c3 gmem_size (Linux a6xx_gpu adreno_7c3 SZ_512K)
inline constexpr uint32_t kGmemCcuColorCacheFraction = 4;          // not-in-headers: freedreno_devices.py a6xx_gen4 gmem_ccu_color_cache_fraction QUARTER
inline constexpr uint32_t kTileAlignW = 32, kTileAlignH = 16;      // not-in-headers: freedreno_devices.py a6xx_gen4 tile_align_w/h
inline constexpr uint32_t kGmemAlignW = 16, kGmemAlignH = 4;       // not-in-headers: freedreno_devices.py a6xx_gen4 gmem_align_w/h (the event blit's granule)
inline constexpr uint32_t kTileMaxW = 1024, kTileMaxH = 1008;      // not-in-headers: freedreno_devices.py a6xx_gen4 tile_max_w/h
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
// turnip's static table adds the alpha test, off (tu_cmd_buffer.cc:1457); fd6 writes it in its zsa state (fd6_zsa.cc:223-227)
inline constexpr uint32_t REG_A6XX_RB_ALPHA_CONTROL = 0x8864;                               // a6xx.xml.h:5873
inline constexpr uint32_t A6XX_RB_ALPHA_CONTROL_ALPHA_REF__MASK = 0x000000ff;               // a6xx.xml.h:5874
inline constexpr uint32_t A6XX_RB_ALPHA_CONTROL_ALPHA_REF__SHIFT = 0;                       // a6xx.xml.h:5875
inline constexpr uint32_t A6XX_RB_ALPHA_CONTROL_ALPHA_TEST = 0x00000100;                    // a6xx.xml.h:5880
inline constexpr uint32_t A6XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC__MASK = 0x00000e00;         // a6xx.xml.h:5881
inline constexpr uint32_t A6XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC__SHIFT = 9;                 // a6xx.xml.h:5882
constexpr uint32_t A6XX_RB_ALPHA_CONTROL_ALPHA_REF(uint32_t v) {
    return field(v, A6XX_RB_ALPHA_CONTROL_ALPHA_REF__SHIFT, A6XX_RB_ALPHA_CONTROL_ALPHA_REF__MASK);
}
constexpr uint32_t A6XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC(adreno_compare_func v) {
    return field(v, A6XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC__SHIFT, A6XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC__MASK);
}
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
// after the 32 VFD_FETCH sizes and the two border color addresses; RB_ALPHA_CONTROL is turnip's (tu_cmd_buffer.cc:1457)
inline constexpr RegWrite kStaticStateTail[] = {
    {REG_A6XX_PC_DGEN_SU_CONSERVATIVE_RAS_CNTL, 0},
    {REG_A6XX_RB_ALPHA_CONTROL, 0},
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

// --- the render path: the registers of one draw into one colour target in SYSMEM (direct render) mode ---------------------
// Everything below is what turnip's sysmem render pass and r3d clear path (src/freedreno/vulkan: tu_cmd_buffer.cc
// tu6_sysmem_render_begin/end, tu_pipeline.cc tu6_emit_vpc/rast/viewport/blend, tu_shader.cc tu6_emit_xs/fs_inputs/
// fs_outputs, tu_clear_blit.cc r3d_*) and fd6's (src/gallium/drivers/freedreno/a6xx: fd6_gmem.cc fd6_emit_sysmem_prep,
// fd6_program.cc, fd6_rasterizer.cc, fd6_blend.cc, fd6_zsa.cc) write for ONE triangle with no depth, no blending, no
// MSAA, no vertex attributes. The order the stream writes them in is render.cpp's; the value each takes for the 64x64
// RGBA8 case is worked out in tests/a6xx_test.cpp. Registers an array holds (GRAS_CL_VPORT[i], RB_MRT[i]...) are
// named by their entry-0 register and the array's __STRIDE; reg*(i) below adds the strides.

// GRAS: the clipper (CL), the setup unit (SU) and the scan converter (SC). GRAS_CL_CNTL for Metal: ZERO_GB_SCALE_Z (depth
// in [0, 1], what turnip sets for Vulkan: tu_pipeline.cc:3142-3150) and VP_CLIP_CODE_IGNORE; the viewport transform
// on (r3d's window-space VS sets VP_XFORM_DISABLE | PERSP_DIVISION_DISABLE | CLIP_DISABLE instead, tu_clear_blit.cc:946-951).
inline constexpr uint32_t REG_A6XX_GRAS_CL_CNTL = 0x8000;                                   // a6xx.xml.h:4256
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_CLIP_DISABLE = 0x00000001;                      // a6xx.xml.h:4257
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_ZNEAR_CLIP_DISABLE = 0x00000002;                // a6xx.xml.h:4258
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_ZFAR_CLIP_DISABLE = 0x00000004;                 // a6xx.xml.h:4259
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_Z_CLAMP_ENABLE = 0x00000020;                    // a6xx.xml.h:4260
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_ZERO_GB_SCALE_Z = 0x00000040;                   // a6xx.xml.h:4261
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_VP_CLIP_CODE_IGNORE = 0x00000080;               // a6xx.xml.h:4262
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_VP_XFORM_DISABLE = 0x00000100;                  // a6xx.xml.h:4263
inline constexpr uint32_t A6XX_GRAS_CL_CNTL_PERSP_DIVISION_DISABLE = 0x00000200;            // a6xx.xml.h:4264

// clip and cull distance masks of the VS: both 0 without clip distances (tu_pipeline.cc:1009-1011)
inline constexpr uint32_t REG_A6XX_GRAS_VS_CL_CNTL = 0x8001;                                // a6xx.xml.h:4267
inline constexpr uint32_t A6XX_GRAS_VS_CL_CNTL_CLIP_MASK__MASK = 0x000000ff;                // a6xx.xml.h:4268
inline constexpr uint32_t A6XX_GRAS_VS_CL_CNTL_CLIP_MASK__SHIFT = 0;                        // a6xx.xml.h:4269
inline constexpr uint32_t A6XX_GRAS_VS_CL_CNTL_CULL_MASK__MASK = 0x0000ff00;                // a6xx.xml.h:4274
inline constexpr uint32_t A6XX_GRAS_VS_CL_CNTL_CULL_MASK__SHIFT = 8;                        // a6xx.xml.h:4275
constexpr uint32_t A6XX_GRAS_VS_CL_CNTL_CLIP_MASK(uint32_t v) {
    return field(v, A6XX_GRAS_VS_CL_CNTL_CLIP_MASK__SHIFT, A6XX_GRAS_VS_CL_CNTL_CLIP_MASK__MASK);
}
constexpr uint32_t A6XX_GRAS_VS_CL_CNTL_CULL_MASK(uint32_t v) {
    return field(v, A6XX_GRAS_VS_CL_CNTL_CULL_MASK__SHIFT, A6XX_GRAS_VS_CL_CNTL_CULL_MASK__MASK);
}

// 0 for a target with one layer (tu_cmd_buffer.cc:574-576)
inline constexpr uint32_t REG_A6XX_GRAS_MAX_LAYER_INDEX = 0x8004;                           // a6xx.xml.h:4312

// which barycentrics the FS reads (one bit per valid ij regid) and the fragcoord components; 0 for a constant-colour FS
// (tu_shader.cc:1747-1756). RB_RENDER_CONTROL0 repeats the IJ bits and COORD_MASK at the same positions.
inline constexpr uint32_t REG_A6XX_GRAS_CNTL = 0x8005;                                      // a6xx.xml.h:4315
inline constexpr uint32_t A6XX_GRAS_CNTL_IJ_PERSP_PIXEL = 0x00000001;                       // a6xx.xml.h:4316
inline constexpr uint32_t A6XX_GRAS_CNTL_IJ_PERSP_CENTROID = 0x00000002;                    // a6xx.xml.h:4317
inline constexpr uint32_t A6XX_GRAS_CNTL_IJ_PERSP_SAMPLE = 0x00000004;                      // a6xx.xml.h:4318
inline constexpr uint32_t A6XX_GRAS_CNTL_IJ_LINEAR_PIXEL = 0x00000008;                      // a6xx.xml.h:4319
inline constexpr uint32_t A6XX_GRAS_CNTL_IJ_LINEAR_CENTROID = 0x00000010;                   // a6xx.xml.h:4320
inline constexpr uint32_t A6XX_GRAS_CNTL_IJ_LINEAR_SAMPLE = 0x00000020;                     // a6xx.xml.h:4321
inline constexpr uint32_t A6XX_GRAS_CNTL_COORD_MASK__MASK = 0x000003c0;                     // a6xx.xml.h:4322
inline constexpr uint32_t A6XX_GRAS_CNTL_COORD_MASK__SHIFT = 6;                             // a6xx.xml.h:4323
inline constexpr uint32_t A6XX_GRAS_CNTL_UNK10 = 0x00000400;                                // a6xx.xml.h:4328
inline constexpr uint32_t A6XX_GRAS_CNTL_UNK11 = 0x00000800;                                // a6xx.xml.h:4329
constexpr uint32_t A6XX_GRAS_CNTL_COORD_MASK(uint32_t v) {
    return field(v, A6XX_GRAS_CNTL_COORD_MASK__SHIFT, A6XX_GRAS_CNTL_COORD_MASK__MASK);
}

// the guardband, 3.6 floating point in each of HORZ and VERT: fd_calc_guardband(offset, scale) over each axis
// (freedreno_guardband.h:14-75, MAX_GB 0x1ff), capped at 511 (tu_pipeline.cc:2458-2461, 2515-2517)
inline constexpr uint32_t REG_A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ = 0x8006;                     // a6xx.xml.h:4332
inline constexpr uint32_t A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ__MASK = 0x000001ff;          // a6xx.xml.h:4333
inline constexpr uint32_t A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ__SHIFT = 0;                  // a6xx.xml.h:4334
inline constexpr uint32_t A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT__MASK = 0x0007fc00;          // a6xx.xml.h:4339
inline constexpr uint32_t A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT__SHIFT = 10;                 // a6xx.xml.h:4340
constexpr uint32_t A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ(uint32_t v) {
    return field(v, A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ__SHIFT, A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ__MASK);
}
constexpr uint32_t A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT(uint32_t v) {
    return field(v, A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT__SHIFT, A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT__MASK);
}

// the viewport transform: six floats (fui) per viewport, one pkt4 of 6 at XOFFSET(0). turnip: offset = x + w/2,
// scale = w/2 (tu_pipeline.cc:2442-2461). Metal's NDC y points up and its row 0 is the top, so YSCALE is written
// NEGATIVE here (-h/2 with YOFFSET y + h/2), the way air_run.c sets softpipe's viewport for Metal (reasoning from the
// convention; the probe's orientation line measures it).
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT = 0x8010;                                  // a6xx.xml.h:4365  the array's first register
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT__STRIDE = 6;                               // a6xx.xml.h:4365  registers per entry
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT_XOFFSET = 0x8010;                          // a6xx.xml.h:4367  entry 0
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT_XSCALE = 0x8011;                           // a6xx.xml.h:4376  entry 0
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT_YOFFSET = 0x8012;                          // a6xx.xml.h:4385  entry 0
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT_YSCALE = 0x8013;                           // a6xx.xml.h:4394  entry 0
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT_ZOFFSET = 0x8014;                          // a6xx.xml.h:4403  entry 0
inline constexpr uint32_t REG_A6XX_GRAS_CL_VPORT_ZSCALE = 0x8015;                           // a6xx.xml.h:4412  entry 0

// the depth clamp range, two floats per viewport: turnip always writes min/max depth (tu_pipeline.cc:2505-2514)
inline constexpr uint32_t REG_A6XX_GRAS_CL_Z_CLAMP = 0x8070;                                // a6xx.xml.h:4421  the array's first register
inline constexpr uint32_t REG_A6XX_GRAS_CL_Z_CLAMP__STRIDE = 2;                             // a6xx.xml.h:4421  registers per entry
inline constexpr uint32_t REG_A6XX_GRAS_CL_Z_CLAMP_MIN = 0x8070;                            // a6xx.xml.h:4423  entry 0
inline constexpr uint32_t REG_A6XX_GRAS_CL_Z_CLAMP_MAX = 0x8071;                            // a6xx.xml.h:4432  entry 0

// culling, front face, line width and mode. LINEHALFWIDTH is quarter pixels (the encoder's val * 4.0): turnip writes
// the Vulkan line width 1.0 as half width 0.5 = 2 and LINE_MODE RECTANGULAR, so a triangle draw's word is 0x2010
// (tu_pipeline.cc:3128-3140). FRONT_CW is judged in window space after the viewport transform, which the negative
// YSCALE flips, so it stays clear until measured; cull none is Metal's default.
inline constexpr uint32_t REG_A6XX_GRAS_SU_CNTL = 0x8090;                                   // a6xx.xml.h:4441
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_CULL_FRONT = 0x00000001;                        // a6xx.xml.h:4442
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_CULL_BACK = 0x00000002;                         // a6xx.xml.h:4443
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_FRONT_CW = 0x00000004;                          // a6xx.xml.h:4444
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__MASK = 0x000007f8;               // a6xx.xml.h:4445
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__SHIFT = 3;                       // a6xx.xml.h:4446
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__MUL = 4;                         // a6xx.xml.h:4447  the encoder's val * 4.0: fixed point
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_POLY_OFFSET = 0x00000800;                       // a6xx.xml.h:4451
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_UNK12 = 0x00001000;                             // a6xx.xml.h:4452
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_LINE_MODE__MASK = 0x00002000;                   // a6xx.xml.h:4453
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_LINE_MODE__SHIFT = 13;                          // a6xx.xml.h:4454
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_UNK15__MASK = 0x00018000;                       // a6xx.xml.h:4459
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_UNK15__SHIFT = 15;                              // a6xx.xml.h:4460
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_MULTIVIEW_ENABLE = 0x00020000;                  // a6xx.xml.h:4465
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_RENDERTARGETINDEXINCR = 0x00040000;             // a6xx.xml.h:4466
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_VIEWPORTINDEXINCR = 0x00080000;                 // a6xx.xml.h:4467
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_UNK20__MASK = 0x00700000;                       // a6xx.xml.h:4468
inline constexpr uint32_t A6XX_GRAS_SU_CNTL_UNK20__SHIFT = 20;                              // a6xx.xml.h:4469
constexpr uint32_t A6XX_GRAS_SU_CNTL_LINEHALFWIDTH(uint32_t v) {
    return field(v, A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__SHIFT, A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__MASK);
}
constexpr uint32_t A6XX_GRAS_SU_CNTL_LINE_MODE(a5xx_line_mode v) {
    return field(v, A6XX_GRAS_SU_CNTL_LINE_MODE__SHIFT, A6XX_GRAS_SU_CNTL_LINE_MODE__MASK);
}
constexpr uint32_t A6XX_GRAS_SU_CNTL_UNK15(uint32_t v) {
    return field(v, A6XX_GRAS_SU_CNTL_UNK15__SHIFT, A6XX_GRAS_SU_CNTL_UNK15__MASK);
}
constexpr uint32_t A6XX_GRAS_SU_CNTL_UNK20(uint32_t v) {
    return field(v, A6XX_GRAS_SU_CNTL_UNK20__SHIFT, A6XX_GRAS_SU_CNTL_UNK20__MASK);
}

// point size clamp and size, in sixteenths (the encoders' val * 16.0): turnip MIN 1/16, MAX 4092, SIZE 1.0
// (tu_pipeline.cc:3207-3209) = 0xffc00001 and 0x10
inline constexpr uint32_t REG_A6XX_GRAS_SU_POINT_MINMAX = 0x8091;                           // a6xx.xml.h:4476
inline constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MIN__MASK = 0x0000ffff;                 // a6xx.xml.h:4477
inline constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MIN__SHIFT = 0;                         // a6xx.xml.h:4478
inline constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MIN__MUL = 16;                          // a6xx.xml.h:4479  the encoder's val * 16.0: fixed point
inline constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MAX__MASK = 0xffff0000;                 // a6xx.xml.h:4483
inline constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MAX__SHIFT = 16;                        // a6xx.xml.h:4484
inline constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MAX__MUL = 16;                          // a6xx.xml.h:4485  the encoder's val * 16.0: fixed point
constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MIN(uint32_t v) {
    return field(v, A6XX_GRAS_SU_POINT_MINMAX_MIN__SHIFT, A6XX_GRAS_SU_POINT_MINMAX_MIN__MASK);
}
constexpr uint32_t A6XX_GRAS_SU_POINT_MINMAX_MAX(uint32_t v) {
    return field(v, A6XX_GRAS_SU_POINT_MINMAX_MAX__SHIFT, A6XX_GRAS_SU_POINT_MINMAX_MAX__MASK);
}

inline constexpr uint32_t REG_A6XX_GRAS_SU_POINT_SIZE = 0x8092;                             // a6xx.xml.h:4491
inline constexpr uint32_t A6XX_GRAS_SU_POINT_SIZE__MASK = 0x0000ffff;                       // a6xx.xml.h:4492
inline constexpr uint32_t A6XX_GRAS_SU_POINT_SIZE__SHIFT = 0;                               // a6xx.xml.h:4493
inline constexpr uint32_t A6XX_GRAS_SU_POINT_SIZE__MUL = 16;                                // a6xx.xml.h:4494  the encoder's val * 16.0: fixed point
constexpr uint32_t A6XX_GRAS_SU_POINT_SIZE(uint32_t v) {
    return field(v, A6XX_GRAS_SU_POINT_SIZE__SHIFT, A6XX_GRAS_SU_POINT_SIZE__MASK);
}

// Z_MODE of the depth plane (GRAS_SU_DEPTH_PLANE_CNTL is in the static table above): pipelines with the depth test
// off write A6XX_LATE_Z (tu_cmd_buffer.cc:5569-5571), r3d writes 0 = EARLY_Z (tu_clear_blit.cc:1612, 1615)
inline constexpr uint32_t A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE__MASK = 0x00000003;          // a6xx.xml.h:4501
inline constexpr uint32_t A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE__SHIFT = 0;                  // a6xx.xml.h:4502
constexpr uint32_t A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE(a6xx_ztest_mode v) {
    return field(v, A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE__SHIFT, A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE__MASK);
}

// polygon offset, three floats (fui): 0 (fd6_rasterizer.cc:67-69)
inline constexpr uint32_t REG_A6XX_GRAS_SU_POLY_OFFSET_SCALE = 0x8095;                      // a6xx.xml.h:4509
inline constexpr uint32_t REG_A6XX_GRAS_SU_POLY_OFFSET_OFFSET = 0x8096;                     // a6xx.xml.h:4518
inline constexpr uint32_t REG_A6XX_GRAS_SU_POLY_OFFSET_OFFSET_CLAMP = 0x8097;               // a6xx.xml.h:4527

// the depth attachment's format, DEPTH6_NONE without one (tu_cmd_buffer.cc:415-433)
inline constexpr uint32_t REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO = 0x8098;                      // a6xx.xml.h:4536
inline constexpr uint32_t A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK = 0x00000007;   // a6xx.xml.h:4537
inline constexpr uint32_t A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT__SHIFT = 0;           // a6xx.xml.h:4538
inline constexpr uint32_t A6XX_GRAS_SU_DEPTH_BUFFER_INFO_UNK3 = 0x00000008;                 // a6xx.xml.h:4543
constexpr uint32_t A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(a6xx_depth_format v) {
    return field(v, A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT__SHIFT, A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK);
}

// depth and stencil tests off (tu_pipeline.cc:3330, 3245-3270)
inline constexpr uint32_t REG_A6XX_GRAS_SU_DEPTH_CNTL = 0x8114;                             // a6xx.xml.h:5931
inline constexpr uint32_t A6XX_GRAS_SU_DEPTH_CNTL_Z_TEST_ENABLE = 0x00000001;               // a6xx.xml.h:5932

inline constexpr uint32_t REG_A6XX_GRAS_SU_STENCIL_CNTL = 0x8115;                           // a6xx.xml.h:6070
inline constexpr uint32_t A6XX_GRAS_SU_STENCIL_CNTL_STENCIL_ENABLE = 0x00000001;            // a6xx.xml.h:6071

// GRAS_SC_CNTL is in the static table above with CCUSINGLECACHELINESIZE; the sysmem pass writes it again with
// SINGLE_PRIM_MODE(NO_FLUSH) (tu_pipeline.cc:3351-3380 tu6_emit_prim_mode_sysmem; fd6_emit.cc:562-580)
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__MASK = 0x00000018;            // a6xx.xml.h:4590
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__SHIFT = 3;                    // a6xx.xml.h:4591
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_RASTER_MODE__MASK = 0x00000020;                 // a6xx.xml.h:4596
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_RASTER_MODE__SHIFT = 5;                         // a6xx.xml.h:4597
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_RASTER_DIRECTION__MASK = 0x000000c0;            // a6xx.xml.h:4602
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_RASTER_DIRECTION__SHIFT = 6;                    // a6xx.xml.h:4603
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_SEQUENCED_THREAD_DISTRIBUTION__MASK = 0x00000100;  // a6xx.xml.h:4608
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_SEQUENCED_THREAD_DISTRIBUTION__SHIFT = 8;       // a6xx.xml.h:4609
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_ROTATION__MASK = 0x00000c00;                    // a6xx.xml.h:4615
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_ROTATION__SHIFT = 10;                           // a6xx.xml.h:4616
inline constexpr uint32_t A6XX_GRAS_SC_CNTL_EARLYVIZOUTEN = 0x00001000;                     // a6xx.xml.h:4621
constexpr uint32_t A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE(a6xx_single_prim_mode v) {
    return field(v, A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__SHIFT, A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_CNTL_RASTER_MODE(uint32_t v) {
    return field(v, A6XX_GRAS_SC_CNTL_RASTER_MODE__SHIFT, A6XX_GRAS_SC_CNTL_RASTER_MODE__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_CNTL_RASTER_DIRECTION(uint32_t v) {
    return field(v, A6XX_GRAS_SC_CNTL_RASTER_DIRECTION__SHIFT, A6XX_GRAS_SC_CNTL_RASTER_DIRECTION__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_CNTL_SEQUENCED_THREAD_DISTRIBUTION(uint32_t v) {
    return field(v, A6XX_GRAS_SC_CNTL_SEQUENCED_THREAD_DISTRIBUTION__SHIFT, A6XX_GRAS_SC_CNTL_SEQUENCED_THREAD_DISTRIBUTION__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_CNTL_ROTATION(uint32_t v) {
    return field(v, A6XX_GRAS_SC_CNTL_ROTATION__SHIFT, A6XX_GRAS_SC_CNTL_ROTATION__MASK);
}

// the bin size and where the buffers are: for a sysmem pass BINW = BINH = 0 (the encoders shift by 5 and 4: the fields
// hold the size in 32- and 16-pixel units), RENDER_MODE RENDERING_PASS, BUFFERS_LOCATION BUFFERS_IN_SYSMEM. turnip
// adds LRZ_FEEDBACK_ZMODE_MASK(EARLY_Z_OR_EARLY_LRZ_LATE_Z) because this chip has lrz feedback (tu_cmd_buffer.cc:
// 2104-2113, 587-624) = 0x03c00000; fd6 and r3d write 0x00c00000 (fd6_gmem.cc:2027-2030; tu_clear_blit.cc:1573-1577).
// RB_BIN_CONTROL has the same layout and takes the same value (static_assert below).
inline constexpr uint32_t REG_A6XX_GRAS_BIN_CONTROL = 0x80a1;                               // a6xx.xml.h:4624
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINW__MASK = 0x0000003f;                    // a6xx.xml.h:4625
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINW__SHIFT = 0;                            // a6xx.xml.h:4626
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINW__SCALE = 5;                            // a6xx.xml.h:4627  the encoder's val >> 5
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINH__MASK = 0x00007f00;                    // a6xx.xml.h:4632
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINH__SHIFT = 8;                            // a6xx.xml.h:4633
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINH__SCALE = 4;                            // a6xx.xml.h:4634  the encoder's val >> 4
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_RENDER_MODE__MASK = 0x001c0000;             // a6xx.xml.h:4639
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_RENDER_MODE__SHIFT = 18;                    // a6xx.xml.h:4640
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_FORCE_LRZ_WRITE_DIS = 0x00200000;           // a6xx.xml.h:4645
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__MASK = 0x00c00000;        // a6xx.xml.h:4646
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__SHIFT = 22;               // a6xx.xml.h:4647
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__MASK = 0x07000000; // a6xx.xml.h:4652
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__SHIFT = 24;        // a6xx.xml.h:4653
inline constexpr uint32_t A6XX_GRAS_BIN_CONTROL_UNK27 = 0x08000000;                         // a6xx.xml.h:4658
constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINW(uint32_t v) {
    return field(v >> A6XX_GRAS_BIN_CONTROL_BINW__SCALE, A6XX_GRAS_BIN_CONTROL_BINW__SHIFT, A6XX_GRAS_BIN_CONTROL_BINW__MASK);
}
constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BINH(uint32_t v) {
    return field(v >> A6XX_GRAS_BIN_CONTROL_BINH__SCALE, A6XX_GRAS_BIN_CONTROL_BINH__SHIFT, A6XX_GRAS_BIN_CONTROL_BINH__MASK);
}
constexpr uint32_t A6XX_GRAS_BIN_CONTROL_RENDER_MODE(a6xx_render_mode v) {
    return field(v, A6XX_GRAS_BIN_CONTROL_RENDER_MODE__SHIFT, A6XX_GRAS_BIN_CONTROL_RENDER_MODE__MASK);
}
constexpr uint32_t A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION(a6xx_buffers_location v) {
    return field(v, A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__SHIFT, A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__MASK);
}
constexpr uint32_t A6XX_GRAS_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK(a6xx_lrz_feedback_mask v) {
    return field(v, A6XX_GRAS_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__SHIFT, A6XX_GRAS_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__MASK);
}

// MSAA off: SAMPLES MSAA_ONE in the RAS registers and MSAA_DISABLE in the DEST ones, GRAS, RB and SP_TP alike
// (tu_cmd_buffer.cc:892-910 tu6_emit_msaa; fd6_gmem.cc:1037-1060)
inline constexpr uint32_t REG_A6XX_GRAS_RAS_MSAA_CNTL = 0x80a2;                             // a6xx.xml.h:4661
inline constexpr uint32_t A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES__MASK = 0x00000003;               // a6xx.xml.h:4662
inline constexpr uint32_t A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES__SHIFT = 0;                       // a6xx.xml.h:4663
inline constexpr uint32_t A6XX_GRAS_RAS_MSAA_CNTL_UNK2 = 0x00000004;                        // a6xx.xml.h:4668
inline constexpr uint32_t A6XX_GRAS_RAS_MSAA_CNTL_UNK3 = 0x00000008;                        // a6xx.xml.h:4669
constexpr uint32_t A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES(a3xx_msaa_samples v) {
    return field(v, A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES__SHIFT, A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES__MASK);
}

inline constexpr uint32_t REG_A6XX_GRAS_DEST_MSAA_CNTL = 0x80a3;                            // a6xx.xml.h:4672
inline constexpr uint32_t A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__MASK = 0x00000003;              // a6xx.xml.h:4673
inline constexpr uint32_t A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__SHIFT = 0;                      // a6xx.xml.h:4674
inline constexpr uint32_t A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE = 0x00000004;               // a6xx.xml.h:4679
constexpr uint32_t A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES(a3xx_msaa_samples v) {
    return field(v, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__SHIFT, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES__MASK);
}

// programmable sample locations off: 0 in GRAS, RB and SP_TP (fd6_context.cc:300-306)
inline constexpr uint32_t REG_A6XX_GRAS_SAMPLE_CONFIG = 0x80a4;                             // a6xx.xml.h:4682
inline constexpr uint32_t A6XX_GRAS_SAMPLE_CONFIG_UNK0 = 0x00000001;                        // a6xx.xml.h:4683
inline constexpr uint32_t A6XX_GRAS_SAMPLE_CONFIG_LOCATION_ENABLE = 0x00000002;             // a6xx.xml.h:4684

// the screen and viewport scissors, one TL/BR pair per viewport, 16-bit X and Y (a6xx_scissor_xy): BR is INCLUSIVE,
// w-1 and h-1 (tu_pipeline.cc:2463-2496, 2657-2688; fd6 stores maxx - 1 too, freedreno_state.c:371-372)
inline constexpr uint32_t REG_A6XX_GRAS_SC_SCREEN_SCISSOR = 0x80b0;                         // a6xx.xml.h:4795  the array's first register
inline constexpr uint32_t REG_A6XX_GRAS_SC_SCREEN_SCISSOR__STRIDE = 2;                      // a6xx.xml.h:4795  registers per entry
inline constexpr uint32_t REG_A6XX_GRAS_SC_SCREEN_SCISSOR_TL = 0x80b0;                      // a6xx.xml.h:4797  entry 0
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X__MASK = 0x0000ffff;              // a6xx.xml.h:4798
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X__SHIFT = 0;                      // a6xx.xml.h:4799
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__MASK = 0xffff0000;              // a6xx.xml.h:4804
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__SHIFT = 16;                     // a6xx.xml.h:4805
constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X(uint32_t v) {
    return field(v, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X__SHIFT, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y(uint32_t v) {
    return field(v, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__SHIFT, A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__MASK);
}
inline constexpr uint32_t REG_A6XX_GRAS_SC_SCREEN_SCISSOR_BR = 0x80b1;                      // a6xx.xml.h:4812  entry 0
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_BR_X__MASK = 0x0000ffff;              // a6xx.xml.h:4813
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_BR_X__SHIFT = 0;                      // a6xx.xml.h:4814
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y__MASK = 0xffff0000;              // a6xx.xml.h:4819
inline constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y__SHIFT = 16;                     // a6xx.xml.h:4820
constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_BR_X(uint32_t v) {
    return field(v, A6XX_GRAS_SC_SCREEN_SCISSOR_BR_X__SHIFT, A6XX_GRAS_SC_SCREEN_SCISSOR_BR_X__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y(uint32_t v) {
    return field(v, A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y__SHIFT, A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y__MASK);
}

inline constexpr uint32_t REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR = 0x80d0;                       // a6xx.xml.h:4827  the array's first register
inline constexpr uint32_t REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR__STRIDE = 2;                    // a6xx.xml.h:4827  registers per entry
inline constexpr uint32_t REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL = 0x80d0;                    // a6xx.xml.h:4829  entry 0
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_X__MASK = 0x0000ffff;            // a6xx.xml.h:4830
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_X__SHIFT = 0;                    // a6xx.xml.h:4831
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_Y__MASK = 0xffff0000;            // a6xx.xml.h:4836
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_Y__SHIFT = 16;                   // a6xx.xml.h:4837
constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_X(uint32_t v) {
    return field(v, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_X__SHIFT, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_X__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_Y(uint32_t v) {
    return field(v, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_Y__SHIFT, A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_Y__MASK);
}
inline constexpr uint32_t REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR = 0x80d1;                    // a6xx.xml.h:4844  entry 0
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_X__MASK = 0x0000ffff;            // a6xx.xml.h:4845
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_X__SHIFT = 0;                    // a6xx.xml.h:4846
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_Y__MASK = 0xffff0000;            // a6xx.xml.h:4851
inline constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_Y__SHIFT = 16;                   // a6xx.xml.h:4852
constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_X(uint32_t v) {
    return field(v, A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_X__SHIFT, A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_X__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_Y(uint32_t v) {
    return field(v, A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_Y__SHIFT, A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_Y__MASK);
}

// the window scissor, 14-bit X and Y (a6xx_reg_xy): the pass begin writes TL 0 and BR (w-1, h-1)
// (tu_cmd_buffer.cc:736-747 tu6_emit_window_scissor)
inline constexpr uint32_t REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL = 0x80f0;                      // a6xx.xml.h:4859
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__MASK = 0x00003fff;              // a6xx.xml.h:4860
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__SHIFT = 0;                      // a6xx.xml.h:4861
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK = 0x3fff0000;              // a6xx.xml.h:4866
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__SHIFT = 16;                     // a6xx.xml.h:4867
constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X(uint32_t v) {
    return field(v, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__SHIFT, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y(uint32_t v) {
    return field(v, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__SHIFT, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK);
}
inline constexpr uint32_t REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR = 0x80f1;                      // a6xx.xml.h:4874
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__MASK = 0x00003fff;              // a6xx.xml.h:4875
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__SHIFT = 0;                      // a6xx.xml.h:4876
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__MASK = 0x3fff0000;              // a6xx.xml.h:4881
inline constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__SHIFT = 16;                     // a6xx.xml.h:4882
constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X(uint32_t v) {
    return field(v, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__SHIFT, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X__MASK);
}
constexpr uint32_t A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y(uint32_t v) {
    return field(v, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__SHIFT, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__MASK);
}

// GRAS_LRZ_CNTL (static table above, ENABLE): the other bits, all clear for a pass without LRZ (tu_lrz.cc:936-944)
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_LRZ_WRITE = 0x00000002;                        // a6xx.xml.h:4966
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_GREATER = 0x00000004;                          // a6xx.xml.h:4967
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_FC_ENABLE = 0x00000008;                        // a6xx.xml.h:4968
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_Z_TEST_ENABLE = 0x00000010;                    // a6xx.xml.h:4969
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_Z_BOUNDS_ENABLE = 0x00000020;                  // a6xx.xml.h:4970
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_DIR__MASK = 0x000000c0;                        // a6xx.xml.h:4971
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_DIR__SHIFT = 6;                                // a6xx.xml.h:4972
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_DIR_WRITE = 0x00000100;                        // a6xx.xml.h:4977
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_DISABLE_ON_WRONG_DIR = 0x00000200;             // a6xx.xml.h:4978
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_Z_FUNC__MASK = 0x00003800;                     // a6xx.xml.h:4979
inline constexpr uint32_t A6XX_GRAS_LRZ_CNTL_Z_FUNC__SHIFT = 11;                            // a6xx.xml.h:4980
constexpr uint32_t A6XX_GRAS_LRZ_CNTL_DIR(uint32_t v) {
    return field(v, A6XX_GRAS_LRZ_CNTL_DIR__SHIFT, A6XX_GRAS_LRZ_CNTL_DIR__MASK);
}
constexpr uint32_t A6XX_GRAS_LRZ_CNTL_Z_FUNC(adreno_compare_func v) {
    return field(v, A6XX_GRAS_LRZ_CNTL_Z_FUNC__SHIFT, A6XX_GRAS_LRZ_CNTL_Z_FUNC__MASK);
}

// GRAS_LRZ_PS_INPUT_CNTL (static table above, SAMPLEID): FRAGCOORDSAMPLEMODE, CENTER without sample shading
inline constexpr uint32_t A6XX_GRAS_LRZ_PS_INPUT_CNTL_FRAGCOORDSAMPLEMODE__MASK = 0x00000006;  // a6xx.xml.h:4989
inline constexpr uint32_t A6XX_GRAS_LRZ_PS_INPUT_CNTL_FRAGCOORDSAMPLEMODE__SHIFT = 1;       // a6xx.xml.h:4990
constexpr uint32_t A6XX_GRAS_LRZ_PS_INPUT_CNTL_FRAGCOORDSAMPLEMODE(a6xx_fragcoord_sample_mode v) {
    return field(v, A6XX_GRAS_LRZ_PS_INPUT_CNTL_FRAGCOORDSAMPLEMODE__SHIFT, A6XX_GRAS_LRZ_PS_INPUT_CNTL_FRAGCOORDSAMPLEMODE__MASK);
}

// the colour format again, for LRZ's sake: written with the MRT (tu_cmd_buffer.cc:532; fd6_gmem.cc:140)
inline constexpr uint32_t REG_A6XX_GRAS_LRZ_MRT_BUF_INFO_0 = 0x8102;                        // a6xx.xml.h:4997
inline constexpr uint32_t A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__MASK = 0x000000ff;     // a6xx.xml.h:4998
inline constexpr uint32_t A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__SHIFT = 0;             // a6xx.xml.h:4999
constexpr uint32_t A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT(a6xx_format v) {
    return field(v, A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__SHIFT, A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT__MASK);
}

// the LRZ buffer: 64-bit base, pitch, 64-bit fast-clear base, all 0 without one (fd6_gmem.cc:250-260 emit_lrz;
// tu_lrz.cc:65-78)
inline constexpr uint32_t REG_A6XX_GRAS_LRZ_BUFFER_BASE = 0x8103;                           // a6xx.xml.h:5006
inline constexpr uint32_t REG_A6XX_GRAS_LRZ_BUFFER_PITCH = 0x8105;                          // a6xx.xml.h:5009
inline constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH__MASK = 0x000000ff;              // a6xx.xml.h:5010
inline constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH__SHIFT = 0;                      // a6xx.xml.h:5011
inline constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH__SCALE = 5;                      // a6xx.xml.h:5012  the encoder's val >> 5
inline constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_ARRAY_PITCH__MASK = 0x1ffffc00;        // a6xx.xml.h:5017
inline constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_ARRAY_PITCH__SHIFT = 10;               // a6xx.xml.h:5018
inline constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_ARRAY_PITCH__SCALE = 8;                // a6xx.xml.h:5019  the encoder's val >> 8
constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH(uint32_t v) {
    return field(v >> A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH__SCALE, A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH__SHIFT, A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH__MASK);
}
constexpr uint32_t A6XX_GRAS_LRZ_BUFFER_PITCH_ARRAY_PITCH(uint32_t v) {
    return field(v >> A6XX_GRAS_LRZ_BUFFER_PITCH_ARRAY_PITCH__SCALE, A6XX_GRAS_LRZ_BUFFER_PITCH_ARRAY_PITCH__SHIFT, A6XX_GRAS_LRZ_BUFFER_PITCH_ARRAY_PITCH__MASK);
}
inline constexpr uint32_t REG_A6XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE = 0x8106;                // a6xx.xml.h:5026

// written with the window scissor, the same TL and BR (tu_cmd_buffer.cc:745-746; fd6_gmem.cc:876-884)
inline constexpr uint32_t REG_A6XX_GRAS_2D_RESOLVE_CNTL_1 = 0x840a;                         // a6xx.xml.h:5212
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_1_X__MASK = 0x00003fff;                 // a6xx.xml.h:5213
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_1_X__SHIFT = 0;                         // a6xx.xml.h:5214
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_1_Y__MASK = 0x3fff0000;                 // a6xx.xml.h:5219
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_1_Y__SHIFT = 16;                        // a6xx.xml.h:5220
constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_1_X(uint32_t v) {
    return field(v, A6XX_GRAS_2D_RESOLVE_CNTL_1_X__SHIFT, A6XX_GRAS_2D_RESOLVE_CNTL_1_X__MASK);
}
constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_1_Y(uint32_t v) {
    return field(v, A6XX_GRAS_2D_RESOLVE_CNTL_1_Y__SHIFT, A6XX_GRAS_2D_RESOLVE_CNTL_1_Y__MASK);
}
inline constexpr uint32_t REG_A6XX_GRAS_2D_RESOLVE_CNTL_2 = 0x840b;                         // a6xx.xml.h:5227
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_2_X__MASK = 0x00003fff;                 // a6xx.xml.h:5228
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_2_X__SHIFT = 0;                         // a6xx.xml.h:5229
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_2_Y__MASK = 0x3fff0000;                 // a6xx.xml.h:5234
inline constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_2_Y__SHIFT = 16;                        // a6xx.xml.h:5235
constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_2_X(uint32_t v) {
    return field(v, A6XX_GRAS_2D_RESOLVE_CNTL_2_X__SHIFT, A6XX_GRAS_2D_RESOLVE_CNTL_2_X__MASK);
}
constexpr uint32_t A6XX_GRAS_2D_RESOLVE_CNTL_2_Y(uint32_t v) {
    return field(v, A6XX_GRAS_2D_RESOLVE_CNTL_2_Y__SHIFT, A6XX_GRAS_2D_RESOLVE_CNTL_2_Y__MASK);
}

// RB: the render backend. RB_BIN_CONTROL: GRAS_BIN_CONTROL's twin (tu_cmd_buffer.cc:606-612); RB_BIN_CONTROL2 = 0
inline constexpr uint32_t REG_A6XX_RB_BIN_CONTROL = 0x8800;                                 // a6xx.xml.h:5259
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BINW__MASK = 0x0000003f;                      // a6xx.xml.h:5260
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BINW__SHIFT = 0;                              // a6xx.xml.h:5261
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BINW__SCALE = 5;                              // a6xx.xml.h:5262  the encoder's val >> 5
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BINH__MASK = 0x00007f00;                      // a6xx.xml.h:5267
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BINH__SHIFT = 8;                              // a6xx.xml.h:5268
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BINH__SCALE = 4;                              // a6xx.xml.h:5269  the encoder's val >> 4
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_RENDER_MODE__MASK = 0x001c0000;               // a6xx.xml.h:5274
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_RENDER_MODE__SHIFT = 18;                      // a6xx.xml.h:5275
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_FORCE_LRZ_WRITE_DIS = 0x00200000;             // a6xx.xml.h:5280
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BUFFERS_LOCATION__MASK = 0x00c00000;          // a6xx.xml.h:5281
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_BUFFERS_LOCATION__SHIFT = 22;                 // a6xx.xml.h:5282
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__MASK = 0x07000000;   // a6xx.xml.h:5287
inline constexpr uint32_t A6XX_RB_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__SHIFT = 24;          // a6xx.xml.h:5288
constexpr uint32_t A6XX_RB_BIN_CONTROL_BINW(uint32_t v) {
    return field(v >> A6XX_RB_BIN_CONTROL_BINW__SCALE, A6XX_RB_BIN_CONTROL_BINW__SHIFT, A6XX_RB_BIN_CONTROL_BINW__MASK);
}
constexpr uint32_t A6XX_RB_BIN_CONTROL_BINH(uint32_t v) {
    return field(v >> A6XX_RB_BIN_CONTROL_BINH__SCALE, A6XX_RB_BIN_CONTROL_BINH__SHIFT, A6XX_RB_BIN_CONTROL_BINH__MASK);
}
constexpr uint32_t A6XX_RB_BIN_CONTROL_RENDER_MODE(a6xx_render_mode v) {
    return field(v, A6XX_RB_BIN_CONTROL_RENDER_MODE__SHIFT, A6XX_RB_BIN_CONTROL_RENDER_MODE__MASK);
}
constexpr uint32_t A6XX_RB_BIN_CONTROL_BUFFERS_LOCATION(a6xx_buffers_location v) {
    return field(v, A6XX_RB_BIN_CONTROL_BUFFERS_LOCATION__SHIFT, A6XX_RB_BIN_CONTROL_BUFFERS_LOCATION__MASK);
}
constexpr uint32_t A6XX_RB_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK(a6xx_lrz_feedback_mask v) {
    return field(v, A6XX_RB_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__SHIFT, A6XX_RB_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__MASK);
}

inline constexpr uint32_t REG_A6XX_RB_BIN_CONTROL2 = 0x88d3;                                // a6xx.xml.h:6251
inline constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINW__MASK = 0x0000003f;                     // a6xx.xml.h:6252
inline constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINW__SHIFT = 0;                             // a6xx.xml.h:6253
inline constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINW__SCALE = 5;                             // a6xx.xml.h:6254  the encoder's val >> 5
inline constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINH__MASK = 0x00007f00;                     // a6xx.xml.h:6259
inline constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINH__SHIFT = 8;                             // a6xx.xml.h:6260
inline constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINH__SCALE = 4;                             // a6xx.xml.h:6261  the encoder's val >> 4
constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINW(uint32_t v) {
    return field(v >> A6XX_RB_BIN_CONTROL2_BINW__SCALE, A6XX_RB_BIN_CONTROL2_BINW__SHIFT, A6XX_RB_BIN_CONTROL2_BINW__MASK);
}
constexpr uint32_t A6XX_RB_BIN_CONTROL2_BINH(uint32_t v) {
    return field(v >> A6XX_RB_BIN_CONTROL2_BINH__SCALE, A6XX_RB_BIN_CONTROL2_BINH__SHIFT, A6XX_RB_BIN_CONTROL2_BINH__MASK);
}

// the event blit: CP_EVENT_WRITE BLIT moves the current bin between GMEM and memory (LOAD, STORE) or clears it in GMEM,
// as RB_BLIT_INFO says, over the bin's part of the blit scissor (tu_clear_blit.cc:1909-2009 event_blit_setup/_run)
enum a6xx_blit_event_type : uint32_t {
    BLIT_EVENT_STORE = 0,              // a6xx.xml.h:2254
    BLIT_EVENT_STORE_AND_CLEAR = 1,    // a6xx.xml.h:2255
    BLIT_EVENT_CLEAR = 2,              // a6xx.xml.h:2256
    BLIT_EVENT_LOAD = 3,               // a6xx.xml.h:2257
};
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_88D0 = 0x88d0;                                 // a6xx.xml.h:6206  turnip writes 0 before each event blit
inline constexpr uint32_t REG_A6XX_RB_BLIT_SCISSOR_TL = 0x88d1;                              // a6xx.xml.h:6221  a6xx_reg_xy, BR inclusive
inline constexpr uint32_t REG_A6XX_RB_BLIT_SCISSOR_BR = 0x88d2;                              // a6xx.xml.h:6236
inline constexpr uint32_t REG_A6XX_RB_BLIT_BASE_GMEM = 0x88d6;                               // a6xx.xml.h:6292  bytes into GMEM
inline constexpr uint32_t REG_A6XX_RB_BLIT_DST_INFO = 0x88d7;                                // a6xx.xml.h:6295
inline constexpr uint32_t A6XX_RB_BLIT_DST_INFO_TILE_MODE__MASK = 0x00000003;                // a6xx.xml.h:6296
inline constexpr uint32_t A6XX_RB_BLIT_DST_INFO_TILE_MODE__SHIFT = 0;                        // a6xx.xml.h:6297
inline constexpr uint32_t A6XX_RB_BLIT_DST_INFO_COLOR_SWAP__MASK = 0x00000060;               // a6xx.xml.h:6309
inline constexpr uint32_t A6XX_RB_BLIT_DST_INFO_COLOR_SWAP__SHIFT = 5;                       // a6xx.xml.h:6310
inline constexpr uint32_t A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__MASK = 0x00007f80;             // a6xx.xml.h:6315
inline constexpr uint32_t A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__SHIFT = 7;                     // a6xx.xml.h:6316
inline constexpr uint32_t REG_A6XX_RB_BLIT_DST = 0x88d8;                                     // a6xx.xml.h:6325  64-bit
inline constexpr uint32_t REG_A6XX_RB_BLIT_DST_PITCH = 0x88da;                               // a6xx.xml.h:6328  bytes >> 6
inline constexpr uint32_t REG_A6XX_RB_BLIT_FLAG_DST = 0x88dc;                                // a6xx.xml.h:6348  64-bit, then FLAG_DST_PITCH: 0 without UBWC
inline constexpr uint32_t REG_A6XX_RB_BLIT_INFO = 0x88e3;                                    // a6xx.xml.h:6380
inline constexpr uint32_t A6XX_RB_BLIT_INFO_TYPE__MASK = 0x00000003;                         // a6xx.xml.h:6381
inline constexpr uint32_t A6XX_RB_BLIT_INFO_TYPE__SHIFT = 0;                                 // a6xx.xml.h:6382
inline constexpr uint32_t A6XX_RB_BLIT_INFO_SAMPLE_0 = 0x00000004;                           // a6xx.xml.h:6387  integer formats: turnip sets it (no averaging)
constexpr uint32_t rbBlitDstInfo(a6xx_format fmt, a6xx_tile_mode tile, a3xx_color_swap swap) {
    return field(tile, A6XX_RB_BLIT_DST_INFO_TILE_MODE__SHIFT, A6XX_RB_BLIT_DST_INFO_TILE_MODE__MASK) |
           field(swap, A6XX_RB_BLIT_DST_INFO_COLOR_SWAP__SHIFT, A6XX_RB_BLIT_DST_INFO_COLOR_SWAP__MASK) |
           field(fmt, A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__SHIFT, A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT__MASK);
}
inline constexpr uint32_t A6XX_RB_BLIT_INFO_CLEAR_MASK__MASK = 0x000000f0;                   // a6xx.xml.h:6389  the components a CLEAR writes
inline constexpr uint32_t A6XX_RB_BLIT_INFO_CLEAR_MASK__SHIFT = 4;                           // a6xx.xml.h:6390
inline constexpr uint32_t REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0 = 0x88df;                         // a6xx.xml.h:6368  four words: the colour packed as the format's base
constexpr uint32_t rbBlitInfo(a6xx_blit_event_type type, bool sample0, uint32_t clearMask = 0) {
    return field(type, A6XX_RB_BLIT_INFO_TYPE__SHIFT, A6XX_RB_BLIT_INFO_TYPE__MASK) | (sample0 ? A6XX_RB_BLIT_INFO_SAMPLE_0 : 0u) |
           field(clearMask, A6XX_RB_BLIT_INFO_CLEAR_MASK__SHIFT, A6XX_RB_BLIT_INFO_CLEAR_MASK__MASK);
}

// CCUSINGLECACHELINESIZE(2) = 0x10 for a sysmem pass (tu_cmd_buffer.cc:633-693 tu6_emit_render_cntl; fd6_gmem.cc:
// 531-552); r3d_dst writes 0 (tu_clear_blit.cc:1408). FLAG_MRTS/FLAG_DEPTH name the UBWC flag buffers: clear, so
// the RB_MRT_FLAG_BUFFER words are inert.
inline constexpr uint32_t REG_A6XX_RB_RENDER_CNTL = 0x8801;                                 // a6xx.xml.h:5325
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_CCUSINGLECACHELINESIZE__MASK = 0x00000038;    // a6xx.xml.h:5326
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_CCUSINGLECACHELINESIZE__SHIFT = 3;            // a6xx.xml.h:5327
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_EARLYVIZOUTEN = 0x00000040;                   // a6xx.xml.h:5332
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_BINNING = 0x00000080;                         // a6xx.xml.h:5333
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_UNK8__MASK = 0x00000700;                      // a6xx.xml.h:5334
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_UNK8__SHIFT = 8;                              // a6xx.xml.h:5335
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_RASTER_MODE__MASK = 0x00000100;               // a6xx.xml.h:5340
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_RASTER_MODE__SHIFT = 8;                       // a6xx.xml.h:5341
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_RASTER_DIRECTION__MASK = 0x00000600;          // a6xx.xml.h:5346
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_RASTER_DIRECTION__SHIFT = 9;                  // a6xx.xml.h:5347
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_CONSERVATIVERASEN = 0x00000800;               // a6xx.xml.h:5352
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_INNERCONSERVATIVERASEN = 0x00001000;          // a6xx.xml.h:5353
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_FLAG_DEPTH = 0x00004000;                      // a6xx.xml.h:5354
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_FLAG_MRTS__MASK = 0x00ff0000;                 // a6xx.xml.h:5355
inline constexpr uint32_t A6XX_RB_RENDER_CNTL_FLAG_MRTS__SHIFT = 16;                        // a6xx.xml.h:5356
constexpr uint32_t A6XX_RB_RENDER_CNTL_CCUSINGLECACHELINESIZE(uint32_t v) {
    return field(v, A6XX_RB_RENDER_CNTL_CCUSINGLECACHELINESIZE__SHIFT, A6XX_RB_RENDER_CNTL_CCUSINGLECACHELINESIZE__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_CNTL_UNK8(uint32_t v) {
    return field(v, A6XX_RB_RENDER_CNTL_UNK8__SHIFT, A6XX_RB_RENDER_CNTL_UNK8__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_CNTL_RASTER_MODE(uint32_t v) {
    return field(v, A6XX_RB_RENDER_CNTL_RASTER_MODE__SHIFT, A6XX_RB_RENDER_CNTL_RASTER_MODE__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_CNTL_RASTER_DIRECTION(uint32_t v) {
    return field(v, A6XX_RB_RENDER_CNTL_RASTER_DIRECTION__SHIFT, A6XX_RB_RENDER_CNTL_RASTER_DIRECTION__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_CNTL_FLAG_MRTS(uint32_t v) {
    return field(v, A6XX_RB_RENDER_CNTL_FLAG_MRTS__SHIFT, A6XX_RB_RENDER_CNTL_FLAG_MRTS__MASK);
}

inline constexpr uint32_t REG_A6XX_RB_RAS_MSAA_CNTL = 0x8802;                               // a6xx.xml.h:5386
inline constexpr uint32_t A6XX_RB_RAS_MSAA_CNTL_SAMPLES__MASK = 0x00000003;                 // a6xx.xml.h:5387
inline constexpr uint32_t A6XX_RB_RAS_MSAA_CNTL_SAMPLES__SHIFT = 0;                         // a6xx.xml.h:5388
inline constexpr uint32_t A6XX_RB_RAS_MSAA_CNTL_UNK2 = 0x00000004;                          // a6xx.xml.h:5393
inline constexpr uint32_t A6XX_RB_RAS_MSAA_CNTL_UNK3 = 0x00000008;                          // a6xx.xml.h:5394
constexpr uint32_t A6XX_RB_RAS_MSAA_CNTL_SAMPLES(a3xx_msaa_samples v) {
    return field(v, A6XX_RB_RAS_MSAA_CNTL_SAMPLES__SHIFT, A6XX_RB_RAS_MSAA_CNTL_SAMPLES__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_DEST_MSAA_CNTL = 0x8803;                              // a6xx.xml.h:5397
inline constexpr uint32_t A6XX_RB_DEST_MSAA_CNTL_SAMPLES__MASK = 0x00000003;                // a6xx.xml.h:5398
inline constexpr uint32_t A6XX_RB_DEST_MSAA_CNTL_SAMPLES__SHIFT = 0;                        // a6xx.xml.h:5399
inline constexpr uint32_t A6XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE = 0x00000004;                 // a6xx.xml.h:5404
constexpr uint32_t A6XX_RB_DEST_MSAA_CNTL_SAMPLES(a3xx_msaa_samples v) {
    return field(v, A6XX_RB_DEST_MSAA_CNTL_SAMPLES__SHIFT, A6XX_RB_DEST_MSAA_CNTL_SAMPLES__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_SAMPLE_CONFIG = 0x8804;                               // a6xx.xml.h:5407
inline constexpr uint32_t A6XX_RB_SAMPLE_CONFIG_UNK0 = 0x00000001;                          // a6xx.xml.h:5408
inline constexpr uint32_t A6XX_RB_SAMPLE_CONFIG_LOCATION_ENABLE = 0x00000002;               // a6xx.xml.h:5409
inline constexpr uint32_t REG_A6XX_RB_BLIT_GMEM_MSAA_CNTL = 0x88d5;                         // a6xx.xml.h:6283
inline constexpr uint32_t A6XX_RB_BLIT_GMEM_MSAA_CNTL_SAMPLES__MASK = 0x00000018;           // a6xx.xml.h:6284
inline constexpr uint32_t A6XX_RB_BLIT_GMEM_MSAA_CNTL_SAMPLES__SHIFT = 3;                   // a6xx.xml.h:6285
constexpr uint32_t A6XX_RB_BLIT_GMEM_MSAA_CNTL_SAMPLES(a3xx_msaa_samples v) {
    return field(v, A6XX_RB_BLIT_GMEM_MSAA_CNTL_SAMPLES__SHIFT, A6XX_RB_BLIT_GMEM_MSAA_CNTL_SAMPLES__MASK);
}

// what the FS reads: the IJ bits and COORD_MASK as GRAS_CNTL has them (same positions), UNK10 set when the FS has
// varyings (tu_shader.cc:1758-1778); 0 for a constant colour, 0x401 with one smooth varying
inline constexpr uint32_t REG_A6XX_RB_RENDER_CONTROL0 = 0x8809;                             // a6xx.xml.h:5514
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_IJ_PERSP_PIXEL = 0x00000001;              // a6xx.xml.h:5515
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_IJ_PERSP_CENTROID = 0x00000002;           // a6xx.xml.h:5516
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_IJ_PERSP_SAMPLE = 0x00000004;             // a6xx.xml.h:5517
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL = 0x00000008;             // a6xx.xml.h:5518
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_CENTROID = 0x00000010;          // a6xx.xml.h:5519
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_SAMPLE = 0x00000020;            // a6xx.xml.h:5520
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_COORD_MASK__MASK = 0x000003c0;            // a6xx.xml.h:5521
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_COORD_MASK__SHIFT = 6;                    // a6xx.xml.h:5522
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL0_UNK10 = 0x00000400;                       // a6xx.xml.h:5527
constexpr uint32_t A6XX_RB_RENDER_CONTROL0_COORD_MASK(uint32_t v) {
    return field(v, A6XX_RB_RENDER_CONTROL0_COORD_MASK__SHIFT, A6XX_RB_RENDER_CONTROL0_COORD_MASK__MASK);
}

// FRAGCOORDSAMPLEMODE CENTER, the face/sample/mask bits for the system values the FS reads: 0 here
inline constexpr uint32_t REG_A6XX_RB_RENDER_CONTROL1 = 0x880a;                             // a6xx.xml.h:5530
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_SAMPLEMASK = 0x00000001;                  // a6xx.xml.h:5531
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_POSTDEPTHCOVERAGE = 0x00000002;           // a6xx.xml.h:5532
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_FACENESS = 0x00000004;                    // a6xx.xml.h:5533
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_SAMPLEID = 0x00000008;                    // a6xx.xml.h:5534
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_FRAGCOORDSAMPLEMODE__MASK = 0x00000030;   // a6xx.xml.h:5535
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_FRAGCOORDSAMPLEMODE__SHIFT = 4;           // a6xx.xml.h:5536
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_CENTERRHW = 0x00000040;                   // a6xx.xml.h:5541
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_LINELENGTHEN = 0x00000080;                // a6xx.xml.h:5542
inline constexpr uint32_t A6XX_RB_RENDER_CONTROL1_FOVEATION = 0x00000100;                   // a6xx.xml.h:5543
constexpr uint32_t A6XX_RB_RENDER_CONTROL1_FRAGCOORDSAMPLEMODE(a6xx_fragcoord_sample_mode v) {
    return field(v, A6XX_RB_RENDER_CONTROL1_FRAGCOORDSAMPLEMODE__SHIFT, A6XX_RB_RENDER_CONTROL1_FRAGCOORDSAMPLEMODE__MASK);
}

// what the FS writes besides colour: nothing (tu_shader.cc:1886-1890)
inline constexpr uint32_t REG_A6XX_RB_FS_OUTPUT_CNTL0 = 0x880b;                             // a6xx.xml.h:5546
inline constexpr uint32_t A6XX_RB_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE = 0x00000001;        // a6xx.xml.h:5547
inline constexpr uint32_t A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_Z = 0x00000002;               // a6xx.xml.h:5548
inline constexpr uint32_t A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_SAMPMASK = 0x00000004;        // a6xx.xml.h:5549
inline constexpr uint32_t A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_STENCILREF = 0x00000008;      // a6xx.xml.h:5550

// MRT = the number of colour targets, 1 (tu_pipeline.cc:3006-3008); SP_FS_OUTPUT_CNTL1 is its twin
inline constexpr uint32_t REG_A6XX_RB_FS_OUTPUT_CNTL1 = 0x880c;                             // a6xx.xml.h:5553
inline constexpr uint32_t A6XX_RB_FS_OUTPUT_CNTL1_MRT__MASK = 0x0000000f;                   // a6xx.xml.h:5554
inline constexpr uint32_t A6XX_RB_FS_OUTPUT_CNTL1_MRT__SHIFT = 0;                           // a6xx.xml.h:5555
constexpr uint32_t A6XX_RB_FS_OUTPUT_CNTL1_MRT(uint32_t v) {
    return field(v, A6XX_RB_FS_OUTPUT_CNTL1_MRT__SHIFT, A6XX_RB_FS_OUTPUT_CNTL1_MRT__MASK);
}

// the components written per target, 0xf for RT0 (tu_shader.cc:1892-1893); SP_FS_RENDER_COMPONENTS its twin
inline constexpr uint32_t REG_A6XX_RB_RENDER_COMPONENTS = 0x880d;                           // a6xx.xml.h:5562
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT0__MASK = 0x0000000f;                 // a6xx.xml.h:5563
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT0__SHIFT = 0;                         // a6xx.xml.h:5564
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT1__MASK = 0x000000f0;                 // a6xx.xml.h:5569
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT1__SHIFT = 4;                         // a6xx.xml.h:5570
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT2__MASK = 0x00000f00;                 // a6xx.xml.h:5575
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT2__SHIFT = 8;                         // a6xx.xml.h:5576
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT3__MASK = 0x0000f000;                 // a6xx.xml.h:5581
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT3__SHIFT = 12;                        // a6xx.xml.h:5582
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT4__MASK = 0x000f0000;                 // a6xx.xml.h:5587
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT4__SHIFT = 16;                        // a6xx.xml.h:5588
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT5__MASK = 0x00f00000;                 // a6xx.xml.h:5593
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT5__SHIFT = 20;                        // a6xx.xml.h:5594
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT6__MASK = 0x0f000000;                 // a6xx.xml.h:5599
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT6__SHIFT = 24;                        // a6xx.xml.h:5600
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT7__MASK = 0xf0000000;                 // a6xx.xml.h:5605
inline constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT7__SHIFT = 28;                        // a6xx.xml.h:5606
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT0(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT0__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT0__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT1(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT1__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT1__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT2(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT2__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT2__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT3(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT3__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT3__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT4(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT4__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT4__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT5(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT5__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT5__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT6(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT6__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT6__MASK);
}
constexpr uint32_t A6XX_RB_RENDER_COMPONENTS_RT7(uint32_t v) {
    return field(v, A6XX_RB_RENDER_COMPONENTS_RT7__SHIFT, A6XX_RB_RENDER_COMPONENTS_RT7__MASK);
}

// dithering off (tu_cmd_buffer.cc:552-564), sRGB conversion off for a UNORM target (:569-572), one sample
inline constexpr uint32_t REG_A6XX_RB_DITHER_CNTL = 0x880e;                                 // a6xx.xml.h:5613
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT0__MASK = 0x00000003;          // a6xx.xml.h:5614
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT0__SHIFT = 0;                  // a6xx.xml.h:5615
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT1__MASK = 0x0000000c;          // a6xx.xml.h:5620
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT1__SHIFT = 2;                  // a6xx.xml.h:5621
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT2__MASK = 0x00000030;          // a6xx.xml.h:5626
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT2__SHIFT = 4;                  // a6xx.xml.h:5627
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT3__MASK = 0x000000c0;          // a6xx.xml.h:5632
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT3__SHIFT = 6;                  // a6xx.xml.h:5633
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT4__MASK = 0x00000300;          // a6xx.xml.h:5638
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT4__SHIFT = 8;                  // a6xx.xml.h:5639
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT5__MASK = 0x00000c00;          // a6xx.xml.h:5644
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT5__SHIFT = 10;                 // a6xx.xml.h:5645
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT6__MASK = 0x00003000;          // a6xx.xml.h:5650
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT6__SHIFT = 12;                 // a6xx.xml.h:5651
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT7__MASK = 0x0000c000;          // a6xx.xml.h:5656
inline constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT7__SHIFT = 14;                 // a6xx.xml.h:5657
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT0(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT0__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT0__MASK);
}
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT1(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT1__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT1__MASK);
}
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT2(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT2__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT2__MASK);
}
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT3(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT3__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT3__MASK);
}
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT4(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT4__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT4__MASK);
}
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT5(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT5__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT5__MASK);
}
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT6(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT6__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT6__MASK);
}
constexpr uint32_t A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT7(adreno_rb_dither_mode v) {
    return field(v, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT7__SHIFT, A6XX_RB_DITHER_CNTL_DITHER_MODE_MRT7__MASK);
}

inline constexpr uint32_t REG_A6XX_RB_SRGB_CNTL = 0x880f;                                   // a6xx.xml.h:5664
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT0 = 0x00000001;                         // a6xx.xml.h:5665
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT1 = 0x00000002;                         // a6xx.xml.h:5666
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT2 = 0x00000004;                         // a6xx.xml.h:5667
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT3 = 0x00000008;                         // a6xx.xml.h:5668
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT4 = 0x00000010;                         // a6xx.xml.h:5669
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT5 = 0x00000020;                         // a6xx.xml.h:5670
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT6 = 0x00000040;                         // a6xx.xml.h:5671
inline constexpr uint32_t A6XX_RB_SRGB_CNTL_SRGB_MRT7 = 0x00000080;                         // a6xx.xml.h:5672

inline constexpr uint32_t REG_A6XX_RB_SAMPLE_CNTL = 0x8810;                                 // a6xx.xml.h:5675
inline constexpr uint32_t A6XX_RB_SAMPLE_CNTL_PER_SAMP_MODE = 0x00000001;                   // a6xx.xml.h:5676

// the sample counter: r3d disables it around its draw (tu_clear_blit.cc:1649, 1690-1691); optional
inline constexpr uint32_t REG_A6XX_RB_SAMPLE_COUNT_CONTROL = 0x8891;                        // a6xx.xml.h:6176
inline constexpr uint32_t A6XX_RB_SAMPLE_COUNT_CONTROL_DISABLE = 0x00000001;                // a6xx.xml.h:6177
inline constexpr uint32_t A6XX_RB_SAMPLE_COUNT_CONTROL_COPY = 0x00000002;                   // a6xx.xml.h:6178
// where ZPASS_DONE copies the running sample count (64 bits) when RB_SAMPLE_COUNT_CONTROL.COPY is set: an occlusion
// query's begin and end snapshots (turnip's emit_begin_occlusion_query / emit_end_occlusion_query, A6XX path)
inline constexpr uint32_t REG_A6XX_RB_SAMPLE_COUNT_ADDR = 0x8927;                           // a6xx.xml.h:6540
// CP_MEM_TO_MEM word 0: DST = A + B - C on 64-bit values, the query's result += end - begin
inline constexpr uint32_t CP_MEM_TO_MEM_0_NEG_C = 0x00000004;                               // adreno_pm4.xml.h:1778
inline constexpr uint32_t CP_MEM_TO_MEM_0_DOUBLE = 0x20000000;                              // adreno_pm4.xml.h:1779

// RB_DEPTH_PLANE_CNTL (static table above): Z_MODE, see GRAS_SU_DEPTH_PLANE_CNTL
inline constexpr uint32_t A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE__MASK = 0x00000003;               // a6xx.xml.h:5909
inline constexpr uint32_t A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE__SHIFT = 0;                       // a6xx.xml.h:5910
constexpr uint32_t A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE(a6xx_ztest_mode v) {
    return field(v, A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE__SHIFT, A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE__MASK);
}

// the depth test: 0 (tu_pipeline.cc:3329); fd6 keeps ZFUNC when the test is off (fd6_zsa.cc:103), inert
inline constexpr uint32_t REG_A6XX_RB_DEPTH_CNTL = 0x8871;                                  // a6xx.xml.h:5917
inline constexpr uint32_t A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE = 0x00000001;                    // a6xx.xml.h:5918
inline constexpr uint32_t A6XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE = 0x00000002;                   // a6xx.xml.h:5919
inline constexpr uint32_t A6XX_RB_DEPTH_CNTL_ZFUNC__MASK = 0x0000001c;                      // a6xx.xml.h:5920
inline constexpr uint32_t A6XX_RB_DEPTH_CNTL_ZFUNC__SHIFT = 2;                              // a6xx.xml.h:5921
inline constexpr uint32_t A6XX_RB_DEPTH_CNTL_Z_CLAMP_ENABLE = 0x00000020;                   // a6xx.xml.h:5926
inline constexpr uint32_t A6XX_RB_DEPTH_CNTL_Z_READ_ENABLE = 0x00000040;                    // a6xx.xml.h:5927
inline constexpr uint32_t A6XX_RB_DEPTH_CNTL_Z_BOUNDS_ENABLE = 0x00000080;                  // a6xx.xml.h:5928
constexpr uint32_t A6XX_RB_DEPTH_CNTL_ZFUNC(adreno_compare_func v) {
    return field(v, A6XX_RB_DEPTH_CNTL_ZFUNC__SHIFT, A6XX_RB_DEPTH_CNTL_ZFUNC__MASK);
}

// the depth attachment, one pkt4 of 6 at RB_DEPTH_BUFFER_INFO: DEPTH6_NONE, pitch 0, array pitch 0, base 0 (64-bit),
// base in GMEM 0 (tu_cmd_buffer.cc:415-433; fd6_gmem.cc:231-247). The pitches are stored >> 6 like the MRT ones.
inline constexpr uint32_t REG_A6XX_RB_DEPTH_BUFFER_INFO = 0x8872;                           // a6xx.xml.h:5935
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK = 0x00000007;        // a6xx.xml.h:5936
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__SHIFT = 0;                // a6xx.xml.h:5937
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_INFO_UNK3__MASK = 0x00000018;                // a6xx.xml.h:5942
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_INFO_UNK3__SHIFT = 3;                        // a6xx.xml.h:5943
constexpr uint32_t A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(a6xx_depth_format v) {
    return field(v, A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__SHIFT, A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT__MASK);
}
constexpr uint32_t A6XX_RB_DEPTH_BUFFER_INFO_UNK3(uint32_t v) {
    return field(v, A6XX_RB_DEPTH_BUFFER_INFO_UNK3__SHIFT, A6XX_RB_DEPTH_BUFFER_INFO_UNK3__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_DEPTH_BUFFER_PITCH = 0x8873;                          // a6xx.xml.h:5972
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_PITCH__MASK = 0x00003fff;                    // a6xx.xml.h:5973
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_PITCH__SHIFT = 0;                            // a6xx.xml.h:5974
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_PITCH__SCALE = 6;                            // a6xx.xml.h:5975  the encoder's val >> 6
constexpr uint32_t A6XX_RB_DEPTH_BUFFER_PITCH(uint32_t v) {
    return field(v >> A6XX_RB_DEPTH_BUFFER_PITCH__SCALE, A6XX_RB_DEPTH_BUFFER_PITCH__SHIFT, A6XX_RB_DEPTH_BUFFER_PITCH__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH = 0x8874;                    // a6xx.xml.h:5982
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH__MASK = 0x0fffffff;              // a6xx.xml.h:5983
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH__SHIFT = 0;                      // a6xx.xml.h:5984
inline constexpr uint32_t A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH__SCALE = 6;                      // a6xx.xml.h:5985  the encoder's val >> 6
constexpr uint32_t A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(uint32_t v) {
    return field(v >> A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH__SCALE, A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH__SHIFT, A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH__MASK);
}
// the depth flag buffer (UBWC metadata), three words at RB_DEPTH_FLAG_BUFFER_BASE: 0, no UBWC (tu6_emit_zs :450-451)
inline constexpr uint32_t REG_A6XX_RB_DEPTH_FLAG_BUFFER_BASE = 0x8900;                     // a6xx.xml.h:6492
inline constexpr uint32_t REG_A6XX_RB_DEPTH_BUFFER_BASE = 0x8875;                           // a6xx.xml.h:5992
inline constexpr uint32_t REG_A6XX_RB_DEPTH_BUFFER_BASE_GMEM = 0x8877;                      // a6xx.xml.h:5995

// depth bounds, two floats (fui): 0.0 and 1.0 (tu_pipeline.cc:3271-3273); inert without Z_BOUNDS_ENABLE
inline constexpr uint32_t REG_A6XX_RB_Z_BOUNDS_MIN = 0x8878;                                // a6xx.xml.h:5998
inline constexpr uint32_t REG_A6XX_RB_Z_BOUNDS_MAX = 0x8879;                                // a6xx.xml.h:6007

// the stencil test and its masks: all 0 (tu_pipeline.cc:3245-3270; r3d tu_clear_blit.cc:1616-1620)
inline constexpr uint32_t REG_A6XX_RB_STENCIL_CONTROL = 0x8880;                             // a6xx.xml.h:6016
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE = 0x00000001;              // a6xx.xml.h:6017
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF = 0x00000002;           // a6xx.xml.h:6018
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_STENCIL_READ = 0x00000004;                // a6xx.xml.h:6019
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FUNC__MASK = 0x00000700;                  // a6xx.xml.h:6020
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FUNC__SHIFT = 8;                          // a6xx.xml.h:6021
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FAIL__MASK = 0x00003800;                  // a6xx.xml.h:6026
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FAIL__SHIFT = 11;                         // a6xx.xml.h:6027
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZPASS__MASK = 0x0001c000;                 // a6xx.xml.h:6032
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZPASS__SHIFT = 14;                        // a6xx.xml.h:6033
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZFAIL__MASK = 0x000e0000;                 // a6xx.xml.h:6038
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZFAIL__SHIFT = 17;                        // a6xx.xml.h:6039
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FUNC_BF__MASK = 0x00700000;               // a6xx.xml.h:6044
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FUNC_BF__SHIFT = 20;                      // a6xx.xml.h:6045
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FAIL_BF__MASK = 0x03800000;               // a6xx.xml.h:6050
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FAIL_BF__SHIFT = 23;                      // a6xx.xml.h:6051
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZPASS_BF__MASK = 0x1c000000;              // a6xx.xml.h:6056
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZPASS_BF__SHIFT = 26;                     // a6xx.xml.h:6057
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZFAIL_BF__MASK = 0xe0000000;              // a6xx.xml.h:6062
inline constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZFAIL_BF__SHIFT = 29;                     // a6xx.xml.h:6063
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FUNC(adreno_compare_func v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_FUNC__SHIFT, A6XX_RB_STENCIL_CONTROL_FUNC__MASK);
}
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FAIL(adreno_stencil_op v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_FAIL__SHIFT, A6XX_RB_STENCIL_CONTROL_FAIL__MASK);
}
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZPASS(adreno_stencil_op v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_ZPASS__SHIFT, A6XX_RB_STENCIL_CONTROL_ZPASS__MASK);
}
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZFAIL(adreno_stencil_op v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_ZFAIL__SHIFT, A6XX_RB_STENCIL_CONTROL_ZFAIL__MASK);
}
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FUNC_BF(adreno_compare_func v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_FUNC_BF__SHIFT, A6XX_RB_STENCIL_CONTROL_FUNC_BF__MASK);
}
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_FAIL_BF(adreno_stencil_op v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_FAIL_BF__SHIFT, A6XX_RB_STENCIL_CONTROL_FAIL_BF__MASK);
}
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZPASS_BF(adreno_stencil_op v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_ZPASS_BF__SHIFT, A6XX_RB_STENCIL_CONTROL_ZPASS_BF__MASK);
}
constexpr uint32_t A6XX_RB_STENCIL_CONTROL_ZFAIL_BF(adreno_stencil_op v) {
    return field(v, A6XX_RB_STENCIL_CONTROL_ZFAIL_BF__SHIFT, A6XX_RB_STENCIL_CONTROL_ZFAIL_BF__MASK);
}

inline constexpr uint32_t REG_A6XX_RB_STENCIL_INFO = 0x8881;                                // a6xx.xml.h:6074
inline constexpr uint32_t A6XX_RB_STENCIL_INFO_SEPARATE_STENCIL = 0x00000001;               // a6xx.xml.h:6075
inline constexpr uint32_t A6XX_RB_STENCIL_INFO_UNK1 = 0x00000002;                           // a6xx.xml.h:6076

inline constexpr uint32_t REG_A6XX_RB_STENCILREF = 0x8887;                                  // a6xx.xml.h:6116
inline constexpr uint32_t A6XX_RB_STENCILREF_REF__MASK = 0x000000ff;                        // a6xx.xml.h:6117
inline constexpr uint32_t A6XX_RB_STENCILREF_REF__SHIFT = 0;                                // a6xx.xml.h:6118
inline constexpr uint32_t A6XX_RB_STENCILREF_BFREF__MASK = 0x0000ff00;                      // a6xx.xml.h:6123
inline constexpr uint32_t A6XX_RB_STENCILREF_BFREF__SHIFT = 8;                              // a6xx.xml.h:6124
constexpr uint32_t A6XX_RB_STENCILREF_REF(uint32_t v) {
    return field(v, A6XX_RB_STENCILREF_REF__SHIFT, A6XX_RB_STENCILREF_REF__MASK);
}
constexpr uint32_t A6XX_RB_STENCILREF_BFREF(uint32_t v) {
    return field(v, A6XX_RB_STENCILREF_BFREF__SHIFT, A6XX_RB_STENCILREF_BFREF__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_STENCILMASK = 0x8888;                                 // a6xx.xml.h:6131
inline constexpr uint32_t A6XX_RB_STENCILMASK_MASK__MASK = 0x000000ff;                      // a6xx.xml.h:6132
inline constexpr uint32_t A6XX_RB_STENCILMASK_MASK__SHIFT = 0;                              // a6xx.xml.h:6133
inline constexpr uint32_t A6XX_RB_STENCILMASK_BFMASK__MASK = 0x0000ff00;                    // a6xx.xml.h:6138
inline constexpr uint32_t A6XX_RB_STENCILMASK_BFMASK__SHIFT = 8;                            // a6xx.xml.h:6139
constexpr uint32_t A6XX_RB_STENCILMASK_MASK(uint32_t v) {
    return field(v, A6XX_RB_STENCILMASK_MASK__SHIFT, A6XX_RB_STENCILMASK_MASK__MASK);
}
constexpr uint32_t A6XX_RB_STENCILMASK_BFMASK(uint32_t v) {
    return field(v, A6XX_RB_STENCILMASK_BFMASK__SHIFT, A6XX_RB_STENCILMASK_BFMASK__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_STENCILWRMASK = 0x8889;                               // a6xx.xml.h:6146
inline constexpr uint32_t A6XX_RB_STENCILWRMASK_WRMASK__MASK = 0x000000ff;                  // a6xx.xml.h:6147
inline constexpr uint32_t A6XX_RB_STENCILWRMASK_WRMASK__SHIFT = 0;                          // a6xx.xml.h:6148
inline constexpr uint32_t A6XX_RB_STENCILWRMASK_BFWRMASK__MASK = 0x0000ff00;                // a6xx.xml.h:6153
inline constexpr uint32_t A6XX_RB_STENCILWRMASK_BFWRMASK__SHIFT = 8;                        // a6xx.xml.h:6154
constexpr uint32_t A6XX_RB_STENCILWRMASK_WRMASK(uint32_t v) {
    return field(v, A6XX_RB_STENCILWRMASK_WRMASK__SHIFT, A6XX_RB_STENCILWRMASK_WRMASK__MASK);
}
constexpr uint32_t A6XX_RB_STENCILWRMASK_BFWRMASK(uint32_t v) {
    return field(v, A6XX_RB_STENCILWRMASK_BFWRMASK__SHIFT, A6XX_RB_STENCILWRMASK_BFWRMASK__MASK);
}

// the window offset (a6xx_reg_xy), 0 for a sysmem pass, written to RB, RB2, SP and SP_TP (tu_cmd_buffer.cc:750-768;
// fd6_gmem.cc:1223-1235 writes these four and no A7XX one)
inline constexpr uint32_t REG_A6XX_RB_WINDOW_OFFSET = 0x8890;                               // a6xx.xml.h:6161
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET_X__MASK = 0x00003fff;                       // a6xx.xml.h:6162
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET_X__SHIFT = 0;                               // a6xx.xml.h:6163
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET_Y__MASK = 0x3fff0000;                       // a6xx.xml.h:6168
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET_Y__SHIFT = 16;                              // a6xx.xml.h:6169
constexpr uint32_t A6XX_RB_WINDOW_OFFSET_X(uint32_t v) {
    return field(v, A6XX_RB_WINDOW_OFFSET_X__SHIFT, A6XX_RB_WINDOW_OFFSET_X__MASK);
}
constexpr uint32_t A6XX_RB_WINDOW_OFFSET_Y(uint32_t v) {
    return field(v, A6XX_RB_WINDOW_OFFSET_Y__SHIFT, A6XX_RB_WINDOW_OFFSET_Y__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_WINDOW_OFFSET2 = 0x88d4;                              // a6xx.xml.h:6268
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET2_X__MASK = 0x00003fff;                      // a6xx.xml.h:6269
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET2_X__SHIFT = 0;                              // a6xx.xml.h:6270
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET2_Y__MASK = 0x3fff0000;                      // a6xx.xml.h:6275
inline constexpr uint32_t A6XX_RB_WINDOW_OFFSET2_Y__SHIFT = 16;                             // a6xx.xml.h:6276
constexpr uint32_t A6XX_RB_WINDOW_OFFSET2_X(uint32_t v) {
    return field(v, A6XX_RB_WINDOW_OFFSET2_X__SHIFT, A6XX_RB_WINDOW_OFFSET2_X__MASK);
}
constexpr uint32_t A6XX_RB_WINDOW_OFFSET2_Y(uint32_t v) {
    return field(v, A6XX_RB_WINDOW_OFFSET2_Y__SHIFT, A6XX_RB_WINDOW_OFFSET2_Y__MASK);
}

// the depth clamp again for the RB, two floats (fui): 0.0 and 1.0 (tu_pipeline.cc:2531-2533)
inline constexpr uint32_t REG_A6XX_RB_Z_CLAMP_MIN = 0x88c0;                                 // a6xx.xml.h:6188
inline constexpr uint32_t REG_A6XX_RB_Z_CLAMP_MAX = 0x88c1;                                 // a6xx.xml.h:6197

// the blend constant, four floats (fui): 0 (tu_pipeline.cc:3081-3082)
inline constexpr uint32_t REG_A6XX_RB_BLEND_RED_F32 = 0x8860;                               // a6xx.xml.h:5837
inline constexpr uint32_t REG_A6XX_RB_BLEND_GREEN_F32 = 0x8861;                             // a6xx.xml.h:5846
inline constexpr uint32_t REG_A6XX_RB_BLEND_BLUE_F32 = 0x8862;                              // a6xx.xml.h:5855
inline constexpr uint32_t REG_A6XX_RB_BLEND_ALPHA_F32 = 0x8863;                             // a6xx.xml.h:5864

// blending off: no ENABLE_BLEND bit, INDEPENDENT_BLEND (turnip sets it always, :3016-3022; fd6 and r3d not), the full
// SAMPLE_MASK = 0xffff0100 (r3d 0xffff0000, tu_clear_blit.cc:1608)
inline constexpr uint32_t REG_A6XX_RB_BLEND_CNTL = 0x8865;                                  // a6xx.xml.h:5889
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_ENABLE_BLEND__MASK = 0x000000ff;               // a6xx.xml.h:5890
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_ENABLE_BLEND__SHIFT = 0;                       // a6xx.xml.h:5891
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_INDEPENDENT_BLEND = 0x00000100;                // a6xx.xml.h:5896
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_DUAL_COLOR_IN_ENABLE = 0x00000200;             // a6xx.xml.h:5897
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_ALPHA_TO_COVERAGE = 0x00000400;                // a6xx.xml.h:5898
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_ALPHA_TO_ONE = 0x00000800;                     // a6xx.xml.h:5899
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_SAMPLE_MASK__MASK = 0xffff0000;                // a6xx.xml.h:5900
inline constexpr uint32_t A6XX_RB_BLEND_CNTL_SAMPLE_MASK__SHIFT = 16;                       // a6xx.xml.h:5901
constexpr uint32_t A6XX_RB_BLEND_CNTL_ENABLE_BLEND(uint32_t v) {
    return field(v, A6XX_RB_BLEND_CNTL_ENABLE_BLEND__SHIFT, A6XX_RB_BLEND_CNTL_ENABLE_BLEND__MASK);
}
constexpr uint32_t A6XX_RB_BLEND_CNTL_SAMPLE_MASK(uint32_t v) {
    return field(v, A6XX_RB_BLEND_CNTL_SAMPLE_MASK__SHIFT, A6XX_RB_BLEND_CNTL_SAMPLE_MASK__MASK);
}

// RB_MRT[i], 8 registers an entry: CONTROL, BLEND_CONTROL, then the six the target's pkt4 writes at BUF_INFO
// (tu_cmd_buffer.cc:500-507; r3d_dst tu_clear_blit.cc:1392-1398): BUF_INFO, PITCH, ARRAY_PITCH, BASE lo, BASE hi,
// BASE_GMEM. CONTROL = COMPONENT_ENABLE(0xf) = 0x780 with ROP off (turnip :3044-3049; ROP_CODE is inert while
// ROP_ENABLE is clear: fd6 leaves ROP_COPY there, 0x7e0). BUF_INFO = COLOR_FORMAT | COLOR_TILE_MODE | COLOR_SWAP
// (fd6_view.c:408-413): 0x30 for linear RGBA8. PITCH and ARRAY_PITCH are stored >> 6: Mesa's encoders assert the
// value is a multiple of 64 (a6xx.xml.h:5816, 5826), which fdl6_layout's pitch (align(width * cpp, 64 * cpp),
// fd6_layout.c:194-201) and the 4 KiB layer always are. BASE is 64-bit, align 1 in the XML (a6xx.xml:3714-3715);
// the layout gives linear images 64 (fd6_layout.c:181). BASE_GMEM is 0 in sysmem (tu_clear_blit.cc:1400).
inline constexpr uint32_t REG_A6XX_RB_MRT = 0x8820;                                         // a6xx.xml.h:5706  the array's first register
inline constexpr uint32_t REG_A6XX_RB_MRT__STRIDE = 8;                                      // a6xx.xml.h:5706  registers per entry
inline constexpr uint32_t REG_A6XX_RB_MRT_CONTROL = 0x8820;                                 // a6xx.xml.h:5708  entry 0
inline constexpr uint32_t A6XX_RB_MRT_CONTROL_BLEND = 0x00000001;                           // a6xx.xml.h:5709
inline constexpr uint32_t A6XX_RB_MRT_CONTROL_BLEND2 = 0x00000002;                          // a6xx.xml.h:5710
inline constexpr uint32_t A6XX_RB_MRT_CONTROL_ROP_ENABLE = 0x00000004;                      // a6xx.xml.h:5711
inline constexpr uint32_t A6XX_RB_MRT_CONTROL_ROP_CODE__MASK = 0x00000078;                  // a6xx.xml.h:5712
inline constexpr uint32_t A6XX_RB_MRT_CONTROL_ROP_CODE__SHIFT = 3;                          // a6xx.xml.h:5713
inline constexpr uint32_t A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__MASK = 0x00000780;          // a6xx.xml.h:5718
inline constexpr uint32_t A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__SHIFT = 7;                  // a6xx.xml.h:5719
constexpr uint32_t A6XX_RB_MRT_CONTROL_ROP_CODE(a3xx_rop_code v) {
    return field(v, A6XX_RB_MRT_CONTROL_ROP_CODE__SHIFT, A6XX_RB_MRT_CONTROL_ROP_CODE__MASK);
}
constexpr uint32_t A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE(uint32_t v) {
    return field(v, A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__SHIFT, A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_MRT_BLEND_CONTROL = 0x8821;                           // a6xx.xml.h:5726  entry 0
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR__MASK = 0x0000001f;      // a6xx.xml.h:5727
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR__SHIFT = 0;              // a6xx.xml.h:5728
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE__MASK = 0x000000e0;    // a6xx.xml.h:5733
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE__SHIFT = 5;            // a6xx.xml.h:5734
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR__MASK = 0x00001f00;     // a6xx.xml.h:5739
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR__SHIFT = 8;             // a6xx.xml.h:5740
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR__MASK = 0x001f0000;    // a6xx.xml.h:5745
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR__SHIFT = 16;           // a6xx.xml.h:5746
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE__MASK = 0x00e00000;  // a6xx.xml.h:5751
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE__SHIFT = 21;         // a6xx.xml.h:5752
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR__MASK = 0x1f000000;   // a6xx.xml.h:5757
inline constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR__SHIFT = 24;          // a6xx.xml.h:5758
constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(adreno_rb_blend_factor v) {
    return field(v, A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR__SHIFT, A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR__MASK);
}
constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(a3xx_rb_blend_opcode v) {
    return field(v, A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE__SHIFT, A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE__MASK);
}
constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(adreno_rb_blend_factor v) {
    return field(v, A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR__SHIFT, A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR__MASK);
}
constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(adreno_rb_blend_factor v) {
    return field(v, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR__SHIFT, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR__MASK);
}
constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(a3xx_rb_blend_opcode v) {
    return field(v, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE__SHIFT, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE__MASK);
}
constexpr uint32_t A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR(adreno_rb_blend_factor v) {
    return field(v, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR__SHIFT, A6XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_MRT_BUF_INFO = 0x8822;                                // a6xx.xml.h:5765  entry 0; the header marks the accessor deprecated
inline constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK = 0x000000ff;             // a6xx.xml.h:5766
inline constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__SHIFT = 0;                     // a6xx.xml.h:5767
inline constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__MASK = 0x00000300;          // a6xx.xml.h:5772
inline constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__SHIFT = 8;                  // a6xx.xml.h:5773
inline constexpr uint32_t A6XX_RB_MRT_BUF_INFO_UNK10 = 0x00000400;                          // a6xx.xml.h:5778
inline constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__MASK = 0x00006000;               // a6xx.xml.h:5779
inline constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__SHIFT = 13;                      // a6xx.xml.h:5780
constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT(a6xx_format v) {
    return field(v, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__SHIFT, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK);
}
constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(a6xx_tile_mode v) {
    return field(v, A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__SHIFT, A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE__MASK);
}
constexpr uint32_t A6XX_RB_MRT_BUF_INFO_COLOR_SWAP(a3xx_color_swap v) {
    return field(v, A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__SHIFT, A6XX_RB_MRT_BUF_INFO_COLOR_SWAP__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_MRT_PITCH = 0x8823;                                   // a6xx.xml.h:5811  entry 0
inline constexpr uint32_t A6XX_RB_MRT_PITCH__MASK = 0xffffffff;                             // a6xx.xml.h:5812
inline constexpr uint32_t A6XX_RB_MRT_PITCH__SHIFT = 0;                                     // a6xx.xml.h:5813
inline constexpr uint32_t A6XX_RB_MRT_PITCH__SCALE = 6;                                     // a6xx.xml.h:5814  the encoder's val >> 6
constexpr uint32_t A6XX_RB_MRT_PITCH(uint32_t v) {
    return field(v >> A6XX_RB_MRT_PITCH__SCALE, A6XX_RB_MRT_PITCH__SHIFT, A6XX_RB_MRT_PITCH__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_MRT_ARRAY_PITCH = 0x8824;                             // a6xx.xml.h:5821  entry 0
inline constexpr uint32_t A6XX_RB_MRT_ARRAY_PITCH__MASK = 0xffffffff;                       // a6xx.xml.h:5822
inline constexpr uint32_t A6XX_RB_MRT_ARRAY_PITCH__SHIFT = 0;                               // a6xx.xml.h:5823
inline constexpr uint32_t A6XX_RB_MRT_ARRAY_PITCH__SCALE = 6;                               // a6xx.xml.h:5824  the encoder's val >> 6
constexpr uint32_t A6XX_RB_MRT_ARRAY_PITCH(uint32_t v) {
    return field(v >> A6XX_RB_MRT_ARRAY_PITCH__SCALE, A6XX_RB_MRT_ARRAY_PITCH__SHIFT, A6XX_RB_MRT_ARRAY_PITCH__MASK);
}
inline constexpr uint32_t REG_A6XX_RB_MRT_BASE = 0x8825;                                    // a6xx.xml.h:5831  entry 0
inline constexpr uint32_t REG_A6XX_RB_MRT_BASE_GMEM = 0x8827;                               // a6xx.xml.h:5834  entry 0

// the UBWC flag buffer of each target, 3 registers an entry (ADDR 64-bit, PITCH): zeros without UBWC, and inert while
// RB_RENDER_CNTL.FLAG_MRTS is clear; both witnesses write the block all the same (tu_cmd_buffer.cc:502-503, fd6_gmem.cc:117-119)
inline constexpr uint32_t REG_A6XX_RB_MRT_FLAG_BUFFER = 0x8903;                             // a6xx.xml.h:6518  the array's first register
inline constexpr uint32_t REG_A6XX_RB_MRT_FLAG_BUFFER__STRIDE = 3;                          // a6xx.xml.h:6518  registers per entry
inline constexpr uint32_t REG_A6XX_RB_MRT_FLAG_BUFFER_ADDR = 0x8903;                        // a6xx.xml.h:6520  entry 0
inline constexpr uint32_t REG_A6XX_RB_MRT_FLAG_BUFFER_PITCH = 0x8905;                       // a6xx.xml.h:6523  entry 0
inline constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH__MASK = 0x000007ff;           // a6xx.xml.h:6524
inline constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH__SHIFT = 0;                   // a6xx.xml.h:6525
inline constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH__SCALE = 6;                   // a6xx.xml.h:6526  the encoder's val >> 6
inline constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH__MASK = 0x1ffff800;     // a6xx.xml.h:6531
inline constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH__SHIFT = 11;            // a6xx.xml.h:6532
inline constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH__SCALE = 7;             // a6xx.xml.h:6533  the encoder's val >> 7
constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH(uint32_t v) {
    return field(v >> A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH__SCALE, A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH__SHIFT, A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH__MASK);
}
constexpr uint32_t A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH(uint32_t v) {
    return field(v >> A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH__SCALE, A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH__SHIFT, A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH__MASK);
}

// shading rate (has_legacy_pipeline_shading_rate): both write 0 (tu_pipeline.cc:3211-3216; fd6_rasterizer.cc:131-136)
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8A00 = 0x8a00;                                // a6xx.xml.h:6543
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8A10 = 0x8a10;                                // a6xx.xml.h:6546
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8A20 = 0x8a20;                                // a6xx.xml.h:6549
inline constexpr uint32_t REG_A6XX_RB_UNKNOWN_8A30 = 0x8a30;                                // a6xx.xml.h:6552

// PC: the primitive controller. PC_PRIMITIVE_CNTL_0: no restart, first vertex provoking (turnip :5848-5862; fd6 sets
// PROVOKING_VTX_LAST for GL, flat varyings only)
inline constexpr uint32_t REG_A6XX_PC_PRIMITIVE_CNTL_0 = 0x9b00;                            // a6xx.xml.h:7577
inline constexpr uint32_t A6XX_PC_PRIMITIVE_CNTL_0_PRIMITIVE_RESTART = 0x00000001;          // a6xx.xml.h:7578
inline constexpr uint32_t A6XX_PC_PRIMITIVE_CNTL_0_PROVOKING_VTX_LAST = 0x00000002;         // a6xx.xml.h:7579
inline constexpr uint32_t A6XX_PC_PRIMITIVE_CNTL_0_D3D_VERTEX_ORDERING = 0x00000004;        // a6xx.xml.h:7580
inline constexpr uint32_t A6XX_PC_PRIMITIVE_CNTL_0_UNK3 = 0x00000008;                       // a6xx.xml.h:7581

// how many VPC locations the VS writes (STRIDE_IN_VPC, the link's max_loc, the same as VPC_VS_PACK's) and which
// built-ins (tu_pipeline.cc:1015-1032): 4 for a position-only link
inline constexpr uint32_t REG_A6XX_PC_VS_OUT_CNTL = 0x9b01;                                 // a6xx.xml.h:7584
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC__MASK = 0x000000ff;             // a6xx.xml.h:7585
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC__SHIFT = 0;                     // a6xx.xml.h:7586
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_PSIZE = 0x00000100;                           // a6xx.xml.h:7591
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_LAYER = 0x00000200;                           // a6xx.xml.h:7592
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_VIEW = 0x00000400;                            // a6xx.xml.h:7593
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_PRIMITIVE_ID = 0x00000800;                    // a6xx.xml.h:7594
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_CLIP_MASK__MASK = 0x00ff0000;                 // a6xx.xml.h:7595
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_CLIP_MASK__SHIFT = 16;                        // a6xx.xml.h:7596
inline constexpr uint32_t A6XX_PC_VS_OUT_CNTL_SHADINGRATE = 0x01000000;                     // a6xx.xml.h:7601
constexpr uint32_t A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC(uint32_t v) {
    return field(v, A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC__SHIFT, A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC__MASK);
}
constexpr uint32_t A6XX_PC_VS_OUT_CNTL_CLIP_MASK(uint32_t v) {
    return field(v, A6XX_PC_VS_OUT_CNTL_CLIP_MASK__SHIFT, A6XX_PC_VS_OUT_CNTL_CLIP_MASK__MASK);
}

// the FS reads the primitive id: no (tu_shader.cc:2075)
inline constexpr uint32_t REG_A6XX_PC_PS_CNTL = 0x9806;                                     // a6xx.xml.h:7470
inline constexpr uint32_t A6XX_PC_PS_CNTL_PRIMITIVEIDEN = 0x00000001;                       // a6xx.xml.h:7471

// POLYMODE6_TRIANGLES, and VPC_POLYGON_MODE the same (tu_pipeline.cc:3152-3157)
inline constexpr uint32_t REG_A6XX_PC_POLYGON_MODE = 0x9981;                                // a6xx.xml.h:7523
inline constexpr uint32_t A6XX_PC_POLYGON_MODE_MODE__MASK = 0x00000003;                     // a6xx.xml.h:7524
inline constexpr uint32_t A6XX_PC_POLYGON_MODE_MODE__SHIFT = 0;                             // a6xx.xml.h:7525
constexpr uint32_t A6XX_PC_POLYGON_MODE_MODE(a6xx_polygon_mode v) {
    return field(v, A6XX_PC_POLYGON_MODE_MODE__SHIFT, A6XX_PC_POLYGON_MODE_MODE__MASK);
}

// 0xffffffff, written before every draw by fd6 (fd6_draw.cc:416-422); inert without PRIMITIVE_RESTART
inline constexpr uint32_t REG_A6XX_PC_RESTART_INDEX = 0x9803;                               // a6xx.xml.h:7461

// stream-out off, the third register of turnip's CP_CONTEXT_REG_BUNCH (tess_use_shared: tu_pipeline.cc:496-512)
inline constexpr uint32_t REG_A6XX_PC_SO_STREAM_CNTL = 0x9808;                              // a6xx.xml.h:7474
inline constexpr uint32_t A6XX_PC_SO_STREAM_CNTL_STREAM_ENABLE__MASK = 0x00078000;          // a6xx.xml.h:7475
inline constexpr uint32_t A6XX_PC_SO_STREAM_CNTL_STREAM_ENABLE__SHIFT = 15;                 // a6xx.xml.h:7476
constexpr uint32_t A6XX_PC_SO_STREAM_CNTL_STREAM_ENABLE(uint32_t v) {
    return field(v, A6XX_PC_SO_STREAM_CNTL_STREAM_ENABLE__SHIFT, A6XX_PC_SO_STREAM_CNTL_STREAM_ENABLE__MASK);
}

// VFD: the vertex fetcher. With no attributes FETCH_CNT = DECODE_CNT = 0 and no VFD_DECODE/DEST_CNTL entry is written
// (tu_shader.cc:1573-1612 tu6_emit_vfd_dest; fd6_program.cc:486-508)
inline constexpr uint32_t REG_A6XX_VFD_CONTROL_0 = 0xa000;                                  // a6xx.xml.h:7828
inline constexpr uint32_t A6XX_VFD_CONTROL_0_FETCH_CNT__MASK = 0x0000003f;                  // a6xx.xml.h:7829
inline constexpr uint32_t A6XX_VFD_CONTROL_0_FETCH_CNT__SHIFT = 0;                          // a6xx.xml.h:7830
inline constexpr uint32_t A6XX_VFD_CONTROL_0_DECODE_CNT__MASK = 0x00003f00;                 // a6xx.xml.h:7835
inline constexpr uint32_t A6XX_VFD_CONTROL_0_DECODE_CNT__SHIFT = 8;                         // a6xx.xml.h:7836
constexpr uint32_t A6XX_VFD_CONTROL_0_FETCH_CNT(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_0_FETCH_CNT__SHIFT, A6XX_VFD_CONTROL_0_FETCH_CNT__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_0_DECODE_CNT(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_0_DECODE_CNT__SHIFT, A6XX_VFD_CONTROL_0_DECODE_CNT__MASK);
}

// where the VS gets its ids: the vertex id register from ir3's SYSTEM_VALUE_VERTEX_ID sysval (which includes
// VFD_INDEX_OFFSET under the static VFD_ADD_OFFSET.VERTEX), kRegidNone for the rest (tu_shader.cc:1947-1966)
inline constexpr uint32_t REG_A6XX_VFD_CONTROL_1 = 0xa001;                                  // a6xx.xml.h:7843
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4VTX__MASK = 0x000000ff;                  // a6xx.xml.h:7844
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4VTX__SHIFT = 0;                          // a6xx.xml.h:7845
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4INST__MASK = 0x0000ff00;                 // a6xx.xml.h:7850
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4INST__SHIFT = 8;                         // a6xx.xml.h:7851
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4PRIMID__MASK = 0x00ff0000;               // a6xx.xml.h:7856
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4PRIMID__SHIFT = 16;                      // a6xx.xml.h:7857
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4VIEWID__MASK = 0xff000000;               // a6xx.xml.h:7862
inline constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4VIEWID__SHIFT = 24;                      // a6xx.xml.h:7863
constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4VTX(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_1_REGID4VTX__SHIFT, A6XX_VFD_CONTROL_1_REGID4VTX__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4INST(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_1_REGID4INST__SHIFT, A6XX_VFD_CONTROL_1_REGID4INST__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4PRIMID(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_1_REGID4PRIMID__SHIFT, A6XX_VFD_CONTROL_1_REGID4PRIMID__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_1_REGID4VIEWID(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_1_REGID4VIEWID__SHIFT, A6XX_VFD_CONTROL_1_REGID4VIEWID__MASK);
}

// the HS, DS and GS id registers, every one kRegidNone without those stages: 0xfcfc, 0xfcfcfcfc, 0xfc (the literal
// turnip writes, tu_shader.cc:2009), 0xfcfc (tu_shader.cc:1972-2036 tu6_emit_hs/ds/gs with a NULL shader)
inline constexpr uint32_t REG_A6XX_VFD_CONTROL_2 = 0xa002;                                  // a6xx.xml.h:7870
inline constexpr uint32_t A6XX_VFD_CONTROL_2_REGID_HSRELPATCHID__MASK = 0x000000ff;         // a6xx.xml.h:7871
inline constexpr uint32_t A6XX_VFD_CONTROL_2_REGID_HSRELPATCHID__SHIFT = 0;                 // a6xx.xml.h:7872
inline constexpr uint32_t A6XX_VFD_CONTROL_2_REGID_INVOCATIONID__MASK = 0x0000ff00;         // a6xx.xml.h:7877
inline constexpr uint32_t A6XX_VFD_CONTROL_2_REGID_INVOCATIONID__SHIFT = 8;                 // a6xx.xml.h:7878
constexpr uint32_t A6XX_VFD_CONTROL_2_REGID_HSRELPATCHID(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_2_REGID_HSRELPATCHID__SHIFT, A6XX_VFD_CONTROL_2_REGID_HSRELPATCHID__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_2_REGID_INVOCATIONID(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_2_REGID_INVOCATIONID__SHIFT, A6XX_VFD_CONTROL_2_REGID_INVOCATIONID__MASK);
}

inline constexpr uint32_t REG_A6XX_VFD_CONTROL_3 = 0xa003;                                  // a6xx.xml.h:7885
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_DSPRIMID__MASK = 0x000000ff;             // a6xx.xml.h:7886
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_DSPRIMID__SHIFT = 0;                     // a6xx.xml.h:7887
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_DSRELPATCHID__MASK = 0x0000ff00;         // a6xx.xml.h:7892
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_DSRELPATCHID__SHIFT = 8;                 // a6xx.xml.h:7893
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_TESSX__MASK = 0x00ff0000;                // a6xx.xml.h:7898
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_TESSX__SHIFT = 16;                       // a6xx.xml.h:7899
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_TESSY__MASK = 0xff000000;                // a6xx.xml.h:7904
inline constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_TESSY__SHIFT = 24;                       // a6xx.xml.h:7905
constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_DSPRIMID(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_3_REGID_DSPRIMID__SHIFT, A6XX_VFD_CONTROL_3_REGID_DSPRIMID__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_DSRELPATCHID(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_3_REGID_DSRELPATCHID__SHIFT, A6XX_VFD_CONTROL_3_REGID_DSRELPATCHID__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_TESSX(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_3_REGID_TESSX__SHIFT, A6XX_VFD_CONTROL_3_REGID_TESSX__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_3_REGID_TESSY(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_3_REGID_TESSY__SHIFT, A6XX_VFD_CONTROL_3_REGID_TESSY__MASK);
}

inline constexpr uint32_t REG_A6XX_VFD_CONTROL_4 = 0xa004;                                  // a6xx.xml.h:7912
inline constexpr uint32_t A6XX_VFD_CONTROL_4_UNK0__MASK = 0x000000ff;                       // a6xx.xml.h:7913
inline constexpr uint32_t A6XX_VFD_CONTROL_4_UNK0__SHIFT = 0;                               // a6xx.xml.h:7914
constexpr uint32_t A6XX_VFD_CONTROL_4_UNK0(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_4_UNK0__SHIFT, A6XX_VFD_CONTROL_4_UNK0__MASK);
}

inline constexpr uint32_t REG_A6XX_VFD_CONTROL_5 = 0xa005;                                  // a6xx.xml.h:7921
inline constexpr uint32_t A6XX_VFD_CONTROL_5_REGID_GSHEADER__MASK = 0x000000ff;             // a6xx.xml.h:7922
inline constexpr uint32_t A6XX_VFD_CONTROL_5_REGID_GSHEADER__SHIFT = 0;                     // a6xx.xml.h:7923
inline constexpr uint32_t A6XX_VFD_CONTROL_5_UNK8__MASK = 0x0000ff00;                       // a6xx.xml.h:7928
inline constexpr uint32_t A6XX_VFD_CONTROL_5_UNK8__SHIFT = 8;                               // a6xx.xml.h:7929
constexpr uint32_t A6XX_VFD_CONTROL_5_REGID_GSHEADER(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_5_REGID_GSHEADER__SHIFT, A6XX_VFD_CONTROL_5_REGID_GSHEADER__MASK);
}
constexpr uint32_t A6XX_VFD_CONTROL_5_UNK8(uint32_t v) {
    return field(v, A6XX_VFD_CONTROL_5_UNK8__SHIFT, A6XX_VFD_CONTROL_5_UNK8__MASK);
}

// PRIMID4PSEN only when the FS reads the primitive id (tu_shader.cc:2072-2073)
inline constexpr uint32_t REG_A6XX_VFD_CONTROL_6 = 0xa006;                                  // a6xx.xml.h:7936
inline constexpr uint32_t A6XX_VFD_CONTROL_6_PRIMID4PSEN = 0x00000001;                      // a6xx.xml.h:7937

// the draw's first vertex and first instance (tu_cmd_buffer.cc:6178-6180; fd6_draw.cc:404-414)
inline constexpr uint32_t REG_A6XX_VFD_INDEX_OFFSET = 0xa00e;                               // a6xx.xml.h:7965
inline constexpr uint32_t REG_A6XX_VFD_INSTANCE_START_OFFSET = 0xa00f;                      // a6xx.xml.h:7968

// the other registers of VFD_FETCH[i] (BASE 64-bit, align 1; STRIDE 12 bits, a6xx.xml:4618-4620); SIZE is above
inline constexpr uint32_t REG_A6XX_VFD_FETCH_BASE = 0xa010;                                 // a6xx.xml.h:7973  entry 0
inline constexpr uint32_t REG_A6XX_VFD_FETCH_STRIDE = 0xa013;                               // a6xx.xml.h:7979  entry 0

// VFD_DECODE[i], 2 registers an entry: how attribute i is decoded (tu_pipeline.cc:2330-2360; fd6_context.cc:81-99) and
// its instance step rate (turnip writes the divisor, 0 per-vertex; fd6 MAX2(1, divisor)). Step two's registers.
inline constexpr uint32_t REG_A6XX_VFD_DECODE = 0xa090;                                     // a6xx.xml.h:7982  the array's first register
inline constexpr uint32_t REG_A6XX_VFD_DECODE__STRIDE = 2;                                  // a6xx.xml.h:7982  registers per entry
inline constexpr uint32_t REG_A6XX_VFD_DECODE_INSTR = 0xa090;                               // a6xx.xml.h:7984  entry 0
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_IDX__MASK = 0x0000001f;                     // a6xx.xml.h:7985
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_IDX__SHIFT = 0;                             // a6xx.xml.h:7986
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_OFFSET__MASK = 0x0001ffe0;                  // a6xx.xml.h:7991
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_OFFSET__SHIFT = 5;                          // a6xx.xml.h:7992
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_INSTANCED = 0x00020000;                     // a6xx.xml.h:7997
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_FORMAT__MASK = 0x0ff00000;                  // a6xx.xml.h:7998
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_FORMAT__SHIFT = 20;                         // a6xx.xml.h:7999
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_SWAP__MASK = 0x30000000;                    // a6xx.xml.h:8004
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_SWAP__SHIFT = 28;                           // a6xx.xml.h:8005
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_UNK30 = 0x40000000;                         // a6xx.xml.h:8010
inline constexpr uint32_t A6XX_VFD_DECODE_INSTR_FLOAT = 0x80000000;                         // a6xx.xml.h:8011
constexpr uint32_t A6XX_VFD_DECODE_INSTR_IDX(uint32_t v) {
    return field(v, A6XX_VFD_DECODE_INSTR_IDX__SHIFT, A6XX_VFD_DECODE_INSTR_IDX__MASK);
}
constexpr uint32_t A6XX_VFD_DECODE_INSTR_OFFSET(uint32_t v) {
    return field(v, A6XX_VFD_DECODE_INSTR_OFFSET__SHIFT, A6XX_VFD_DECODE_INSTR_OFFSET__MASK);
}
constexpr uint32_t A6XX_VFD_DECODE_INSTR_FORMAT(a6xx_format v) {
    return field(v, A6XX_VFD_DECODE_INSTR_FORMAT__SHIFT, A6XX_VFD_DECODE_INSTR_FORMAT__MASK);
}
constexpr uint32_t A6XX_VFD_DECODE_INSTR_SWAP(a3xx_color_swap v) {
    return field(v, A6XX_VFD_DECODE_INSTR_SWAP__SHIFT, A6XX_VFD_DECODE_INSTR_SWAP__MASK);
}
inline constexpr uint32_t REG_A6XX_VFD_DECODE_STEP_RATE = 0xa091;                           // a6xx.xml.h:8014  entry 0

// VFD_DEST_CNTL[i]: the register and write mask attribute i lands in (tu_shader.cc:1600-1611)
inline constexpr uint32_t REG_A6XX_VFD_DEST_CNTL = 0xa0d0;                                  // a6xx.xml.h:8017  the array's first register
inline constexpr uint32_t REG_A6XX_VFD_DEST_CNTL__STRIDE = 1;                               // a6xx.xml.h:8017  registers per entry
inline constexpr uint32_t REG_A6XX_VFD_DEST_CNTL_INSTR = 0xa0d0;                            // a6xx.xml.h:8019  entry 0
inline constexpr uint32_t A6XX_VFD_DEST_CNTL_INSTR_WRITEMASK__MASK = 0x0000000f;            // a6xx.xml.h:8020
inline constexpr uint32_t A6XX_VFD_DEST_CNTL_INSTR_WRITEMASK__SHIFT = 0;                    // a6xx.xml.h:8021
inline constexpr uint32_t A6XX_VFD_DEST_CNTL_INSTR_REGID__MASK = 0x00000ff0;                // a6xx.xml.h:8026
inline constexpr uint32_t A6XX_VFD_DEST_CNTL_INSTR_REGID__SHIFT = 4;                        // a6xx.xml.h:8027
constexpr uint32_t A6XX_VFD_DEST_CNTL_INSTR_WRITEMASK(uint32_t v) {
    return field(v, A6XX_VFD_DEST_CNTL_INSTR_WRITEMASK__SHIFT, A6XX_VFD_DEST_CNTL_INSTR_WRITEMASK__MASK);
}
constexpr uint32_t A6XX_VFD_DEST_CNTL_INSTR_REGID(uint32_t v) {
    return field(v, A6XX_VFD_DEST_CNTL_INSTR_REGID__SHIFT, A6XX_VFD_DEST_CNTL_INSTR_REGID__MASK);
}

// VPC: the varying packer. VPC_VARYING_INTERP_MODE[0..7] and PS_REPL_MODE[0..7]: 2 bits per component at inloc * 2,
// INTERP_FLAT for flat inputs, PS_REPL for point sprites; fd6 writes all 8 + 8 (fd6_program.cc:1365-1370), turnip
// only the ones it needs and r3d PS_REPL[0] = 2 << 2 | 1 for its RECTLIST (tu_clear_blit.cc:936-937): all 16 are
// written as zeros here so that nothing of a blit's is left behind
inline constexpr uint32_t REG_A6XX_VPC_VARYING_INTERP = 0x9200;                             // a6xx.xml.h:7152  the array's first register
inline constexpr uint32_t REG_A6XX_VPC_VARYING_INTERP__STRIDE = 1;                          // a6xx.xml.h:7152  registers per entry
inline constexpr uint32_t REG_A6XX_VPC_VARYING_INTERP_MODE = 0x9200;                        // a6xx.xml.h:7154  entry 0

inline constexpr uint32_t REG_A6XX_VPC_VARYING_PS_REPL = 0x9208;                            // a6xx.xml.h:7157  the array's first register
inline constexpr uint32_t REG_A6XX_VPC_VARYING_PS_REPL__STRIDE = 1;                         // a6xx.xml.h:7157  registers per entry
inline constexpr uint32_t REG_A6XX_VPC_VARYING_PS_REPL_MODE = 0x9208;                       // a6xx.xml.h:7159  entry 0

// VPC_VAR_DISABLE[0..3]: ~varmask, one bit per component of the 128 VPC locations the FS does NOT read
// (tu_shader.cc:1791-1808; fd6_program.cc:659-663): 0xffffffff x 4 without varyings
inline constexpr uint32_t REG_A6XX_VPC_VAR = 0x9212;                                        // a6xx.xml.h:7168  the array's first register
inline constexpr uint32_t REG_A6XX_VPC_VAR__STRIDE = 1;                                     // a6xx.xml.h:7168  registers per entry
inline constexpr uint32_t REG_A6XX_VPC_VAR_DISABLE = 0x9212;                                // a6xx.xml.h:7170  entry 0

// stream-out off, the first register of turnip's bunch (tu_pipeline.cc:496-512)
inline constexpr uint32_t REG_A6XX_VPC_SO_CNTL = 0x9216;                                    // a6xx.xml.h:7173
inline constexpr uint32_t A6XX_VPC_SO_CNTL_ADDR__MASK = 0x000000ff;                         // a6xx.xml.h:7174
inline constexpr uint32_t A6XX_VPC_SO_CNTL_ADDR__SHIFT = 0;                                 // a6xx.xml.h:7175
inline constexpr uint32_t A6XX_VPC_SO_CNTL_RESET = 0x00010000;                              // a6xx.xml.h:7180
constexpr uint32_t A6XX_VPC_SO_CNTL_ADDR(uint32_t v) {
    return field(v, A6XX_VPC_SO_CNTL_ADDR__SHIFT, A6XX_VPC_SO_CNTL_ADDR__MASK);
}

// the VS's output layout: STRIDE_IN_VPC (max_loc), POSITIONLOC, PSIZELOC (0xff: none), EXTRAPOS 0 (tu_pipeline.cc:
// 994-998; fd6_program.cc:750-753): 0x00ff0004 for a position-only link
inline constexpr uint32_t REG_A6XX_VPC_VS_PACK = 0x9301;                                    // a6xx.xml.h:7241
inline constexpr uint32_t A6XX_VPC_VS_PACK_STRIDE_IN_VPC__MASK = 0x000000ff;                // a6xx.xml.h:7242
inline constexpr uint32_t A6XX_VPC_VS_PACK_STRIDE_IN_VPC__SHIFT = 0;                        // a6xx.xml.h:7243
inline constexpr uint32_t A6XX_VPC_VS_PACK_POSITIONLOC__MASK = 0x0000ff00;                  // a6xx.xml.h:7248
inline constexpr uint32_t A6XX_VPC_VS_PACK_POSITIONLOC__SHIFT = 8;                          // a6xx.xml.h:7249
inline constexpr uint32_t A6XX_VPC_VS_PACK_PSIZELOC__MASK = 0x00ff0000;                     // a6xx.xml.h:7254
inline constexpr uint32_t A6XX_VPC_VS_PACK_PSIZELOC__SHIFT = 16;                            // a6xx.xml.h:7255
inline constexpr uint32_t A6XX_VPC_VS_PACK_EXTRAPOS__MASK = 0x0f000000;                     // a6xx.xml.h:7260
inline constexpr uint32_t A6XX_VPC_VS_PACK_EXTRAPOS__SHIFT = 24;                            // a6xx.xml.h:7261
constexpr uint32_t A6XX_VPC_VS_PACK_STRIDE_IN_VPC(uint32_t v) {
    return field(v, A6XX_VPC_VS_PACK_STRIDE_IN_VPC__SHIFT, A6XX_VPC_VS_PACK_STRIDE_IN_VPC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_PACK_POSITIONLOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_PACK_POSITIONLOC__SHIFT, A6XX_VPC_VS_PACK_POSITIONLOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_PACK_PSIZELOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_PACK_PSIZELOC__SHIFT, A6XX_VPC_VS_PACK_PSIZELOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_PACK_EXTRAPOS(uint32_t v) {
    return field(v, A6XX_VPC_VS_PACK_EXTRAPOS__SHIFT, A6XX_VPC_VS_PACK_EXTRAPOS__MASK);
}

// clip distances: none, both locations 0xff, written twice (the _V2 copy too: tu_pipeline.cc:1000-1007) = 0x00ffff00
inline constexpr uint32_t REG_A6XX_VPC_VS_CLIP_CNTL = 0x9101;                               // a6xx.xml.h:6842
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK__MASK = 0x000000ff;               // a6xx.xml.h:6843
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK__SHIFT = 0;                       // a6xx.xml.h:6844
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC__MASK = 0x0000ff00;        // a6xx.xml.h:6849
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC__SHIFT = 8;                // a6xx.xml.h:6850
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC__MASK = 0x00ff0000;        // a6xx.xml.h:6855
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC__SHIFT = 16;               // a6xx.xml.h:6856
constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK(uint32_t v) {
    return field(v, A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK__SHIFT, A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK__MASK);
}
constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC__SHIFT, A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC__SHIFT, A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC__MASK);
}
inline constexpr uint32_t REG_A6XX_VPC_VS_CLIP_CNTL_V2 = 0x9311;                            // a6xx.xml.h:6905
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_MASK__MASK = 0x000000ff;            // a6xx.xml.h:6906
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_MASK__SHIFT = 0;                    // a6xx.xml.h:6907
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_03_LOC__MASK = 0x0000ff00;     // a6xx.xml.h:6912
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_03_LOC__SHIFT = 8;             // a6xx.xml.h:6913
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_47_LOC__MASK = 0x00ff0000;     // a6xx.xml.h:6918
inline constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_47_LOC__SHIFT = 16;            // a6xx.xml.h:6919
constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_MASK(uint32_t v) {
    return field(v, A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_MASK__SHIFT, A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_MASK__MASK);
}
constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_03_LOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_03_LOC__SHIFT, A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_03_LOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_47_LOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_47_LOC__SHIFT, A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_47_LOC__MASK);
}

// layer, view and shading rate locations: all 0xff, written twice = 0x00ffffff (tu_pipeline.cc:1042-1049)
inline constexpr uint32_t REG_A6XX_VPC_VS_LAYER_CNTL = 0x9104;                              // a6xx.xml.h:6968
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_LAYERLOC__MASK = 0x000000ff;               // a6xx.xml.h:6969
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_LAYERLOC__SHIFT = 0;                       // a6xx.xml.h:6970
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_VIEWLOC__MASK = 0x0000ff00;                // a6xx.xml.h:6975
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_VIEWLOC__SHIFT = 8;                        // a6xx.xml.h:6976
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC__MASK = 0x00ff0000;         // a6xx.xml.h:6981
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC__SHIFT = 16;                // a6xx.xml.h:6982
constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_LAYERLOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_LAYER_CNTL_LAYERLOC__SHIFT, A6XX_VPC_VS_LAYER_CNTL_LAYERLOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_VIEWLOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_LAYER_CNTL_VIEWLOC__SHIFT, A6XX_VPC_VS_LAYER_CNTL_VIEWLOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC__SHIFT, A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC__MASK);
}
inline constexpr uint32_t REG_A6XX_VPC_VS_LAYER_CNTL_V2 = 0x9314;                           // a6xx.xml.h:7031
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_LAYERLOC__MASK = 0x000000ff;            // a6xx.xml.h:7032
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_LAYERLOC__SHIFT = 0;                    // a6xx.xml.h:7033
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_VIEWLOC__MASK = 0x0000ff00;             // a6xx.xml.h:7038
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_VIEWLOC__SHIFT = 8;                     // a6xx.xml.h:7039
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_SHADINGRATELOC__MASK = 0x00ff0000;      // a6xx.xml.h:7044
inline constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_SHADINGRATELOC__SHIFT = 16;             // a6xx.xml.h:7045
constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_LAYERLOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_LAYER_CNTL_V2_LAYERLOC__SHIFT, A6XX_VPC_VS_LAYER_CNTL_V2_LAYERLOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_VIEWLOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_LAYER_CNTL_V2_VIEWLOC__SHIFT, A6XX_VPC_VS_LAYER_CNTL_V2_VIEWLOC__MASK);
}
constexpr uint32_t A6XX_VPC_VS_LAYER_CNTL_V2_SHADINGRATELOC(uint32_t v) {
    return field(v, A6XX_VPC_VS_LAYER_CNTL_V2_SHADINGRATELOC__SHIFT, A6XX_VPC_VS_LAYER_CNTL_V2_SHADINGRATELOC__MASK);
}

// what reaches the FS: NUMNONPOSVAR = fs->total_in, VARYING when it is not 0, PRIMIDLOC and VIEWIDLOC 0xff
// (tu_shader.cc:1810-1817): 0xff00ff00 without varyings, 0xff01ff04 with one float4
inline constexpr uint32_t REG_A6XX_VPC_CNTL_0 = 0x9304;                                     // a6xx.xml.h:7322
inline constexpr uint32_t A6XX_VPC_CNTL_0_NUMNONPOSVAR__MASK = 0x000000ff;                  // a6xx.xml.h:7323
inline constexpr uint32_t A6XX_VPC_CNTL_0_NUMNONPOSVAR__SHIFT = 0;                          // a6xx.xml.h:7324
inline constexpr uint32_t A6XX_VPC_CNTL_0_PRIMIDLOC__MASK = 0x0000ff00;                     // a6xx.xml.h:7329
inline constexpr uint32_t A6XX_VPC_CNTL_0_PRIMIDLOC__SHIFT = 8;                             // a6xx.xml.h:7330
inline constexpr uint32_t A6XX_VPC_CNTL_0_VARYING = 0x00010000;                             // a6xx.xml.h:7335
inline constexpr uint32_t A6XX_VPC_CNTL_0_VIEWIDLOC__MASK = 0xff000000;                     // a6xx.xml.h:7336
inline constexpr uint32_t A6XX_VPC_CNTL_0_VIEWIDLOC__SHIFT = 24;                            // a6xx.xml.h:7337
constexpr uint32_t A6XX_VPC_CNTL_0_NUMNONPOSVAR(uint32_t v) {
    return field(v, A6XX_VPC_CNTL_0_NUMNONPOSVAR__SHIFT, A6XX_VPC_CNTL_0_NUMNONPOSVAR__MASK);
}
constexpr uint32_t A6XX_VPC_CNTL_0_PRIMIDLOC(uint32_t v) {
    return field(v, A6XX_VPC_CNTL_0_PRIMIDLOC__SHIFT, A6XX_VPC_CNTL_0_PRIMIDLOC__MASK);
}
constexpr uint32_t A6XX_VPC_CNTL_0_VIEWIDLOC(uint32_t v) {
    return field(v, A6XX_VPC_CNTL_0_VIEWIDLOC__SHIFT, A6XX_VPC_CNTL_0_VIEWIDLOC__MASK);
}

inline constexpr uint32_t REG_A6XX_VPC_POLYGON_MODE = 0x9108;                               // a6xx.xml.h:7099
inline constexpr uint32_t A6XX_VPC_POLYGON_MODE_MODE__MASK = 0x00000003;                    // a6xx.xml.h:7100
inline constexpr uint32_t A6XX_VPC_POLYGON_MODE_MODE__SHIFT = 0;                            // a6xx.xml.h:7101
constexpr uint32_t A6XX_VPC_POLYGON_MODE_MODE(a6xx_polygon_mode v) {
    return field(v, A6XX_VPC_POLYGON_MODE_MODE__SHIFT, A6XX_VPC_POLYGON_MODE_MODE__MASK);
}

// SP_VS: the vertex shader's program registers (tu_shader.cc:1238-1356 tu6_emit_xs; fd6_program.cc:100-245).
// SP_VS_CTRL_REG0: the footprints are ir3's max_reg + 1 and max_half_reg + 1 (0 when no half register is used),
// BRANCHSTACK is ir3_shader_branchstack_hw = ceil(min(branchstack, 64) / 2) (ir3_shader.h:1419-1428), MERGEDREGS is
// bit 20 HERE (bit 31 in SP_FS_CTRL_REG0 and SP_CS_CTRL_REG0, a6xx.xml.h:8073) and there is no THREADSIZE field.
inline constexpr uint32_t REG_A6XX_SP_VS_CTRL_REG0 = 0xa800;                                // a6xx.xml.h:8047
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_THREADMODE__MASK = 0x00000001;               // a6xx.xml.h:8048
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_THREADMODE__SHIFT = 0;                       // a6xx.xml.h:8049
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT__MASK = 0x0000007e;         // a6xx.xml.h:8054
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT__SHIFT = 1;                 // a6xx.xml.h:8055
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT__MASK = 0x00001f80;         // a6xx.xml.h:8060
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT__SHIFT = 7;                 // a6xx.xml.h:8061
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_UNK13 = 0x00002000;                          // a6xx.xml.h:8066
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_BRANCHSTACK__MASK = 0x000fc000;              // a6xx.xml.h:8067
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_BRANCHSTACK__SHIFT = 14;                     // a6xx.xml.h:8068
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_MERGEDREGS = 0x00100000;                     // a6xx.xml.h:8073
inline constexpr uint32_t A6XX_SP_VS_CTRL_REG0_EARLYPREAMBLE = 0x00200000;                  // a6xx.xml.h:8074
constexpr uint32_t A6XX_SP_VS_CTRL_REG0_THREADMODE(a3xx_threadmode v) {
    return field(v, A6XX_SP_VS_CTRL_REG0_THREADMODE__SHIFT, A6XX_SP_VS_CTRL_REG0_THREADMODE__MASK);
}
constexpr uint32_t A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(uint32_t v) {
    return field(v, A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT__SHIFT, A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT__MASK);
}
constexpr uint32_t A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(uint32_t v) {
    return field(v, A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT__SHIFT, A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT__MASK);
}
constexpr uint32_t A6XX_SP_VS_CTRL_REG0_BRANCHSTACK(uint32_t v) {
    return field(v, A6XX_SP_VS_CTRL_REG0_BRANCHSTACK__SHIFT, A6XX_SP_VS_CTRL_REG0_BRANCHSTACK__MASK);
}

// OUT = the link's cnt (var entries), FLAGS_REGID 0 (tu_pipeline.cc:1038-1040)
inline constexpr uint32_t REG_A6XX_SP_VS_PRIMITIVE_CNTL = 0xa802;                           // a6xx.xml.h:8080
inline constexpr uint32_t A6XX_SP_VS_PRIMITIVE_CNTL_OUT__MASK = 0x0000003f;                 // a6xx.xml.h:8081
inline constexpr uint32_t A6XX_SP_VS_PRIMITIVE_CNTL_OUT__SHIFT = 0;                         // a6xx.xml.h:8082
inline constexpr uint32_t A6XX_SP_VS_PRIMITIVE_CNTL_FLAGS_REGID__MASK = 0x00003fc0;         // a6xx.xml.h:8087
inline constexpr uint32_t A6XX_SP_VS_PRIMITIVE_CNTL_FLAGS_REGID__SHIFT = 6;                 // a6xx.xml.h:8088
constexpr uint32_t A6XX_SP_VS_PRIMITIVE_CNTL_OUT(uint32_t v) {
    return field(v, A6XX_SP_VS_PRIMITIVE_CNTL_OUT__SHIFT, A6XX_SP_VS_PRIMITIVE_CNTL_OUT__MASK);
}
constexpr uint32_t A6XX_SP_VS_PRIMITIVE_CNTL_FLAGS_REGID(uint32_t v) {
    return field(v, A6XX_SP_VS_PRIMITIVE_CNTL_FLAGS_REGID__SHIFT, A6XX_SP_VS_PRIMITIVE_CNTL_FLAGS_REGID__MASK);
}

// SP_VS_OUT_REG[i]: two linked outputs per register (var[2i] in A, var[2i + 1] in B), DIV_ROUND_UP(cnt, 2) of them
// (tu_pipeline.cc:977-989); for a position-only link: pos_regid | 0xf << 8
inline constexpr uint32_t REG_A6XX_SP_VS_OUT = 0xa803;                                      // a6xx.xml.h:8095  the array's first register
inline constexpr uint32_t REG_A6XX_SP_VS_OUT__STRIDE = 1;                                   // a6xx.xml.h:8095  registers per entry
inline constexpr uint32_t REG_A6XX_SP_VS_OUT_REG = 0xa803;                                  // a6xx.xml.h:8097  entry 0
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_A_REGID__MASK = 0x000000ff;                    // a6xx.xml.h:8098
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_A_REGID__SHIFT = 0;                            // a6xx.xml.h:8099
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_A_COMPMASK__MASK = 0x00000f00;                 // a6xx.xml.h:8104
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_A_COMPMASK__SHIFT = 8;                         // a6xx.xml.h:8105
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_B_REGID__MASK = 0x00ff0000;                    // a6xx.xml.h:8110
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_B_REGID__SHIFT = 16;                           // a6xx.xml.h:8111
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_B_COMPMASK__MASK = 0x0f000000;                 // a6xx.xml.h:8116
inline constexpr uint32_t A6XX_SP_VS_OUT_REG_B_COMPMASK__SHIFT = 24;                        // a6xx.xml.h:8117
constexpr uint32_t A6XX_SP_VS_OUT_REG_A_REGID(uint32_t v) {
    return field(v, A6XX_SP_VS_OUT_REG_A_REGID__SHIFT, A6XX_SP_VS_OUT_REG_A_REGID__MASK);
}
constexpr uint32_t A6XX_SP_VS_OUT_REG_A_COMPMASK(uint32_t v) {
    return field(v, A6XX_SP_VS_OUT_REG_A_COMPMASK__SHIFT, A6XX_SP_VS_OUT_REG_A_COMPMASK__MASK);
}
constexpr uint32_t A6XX_SP_VS_OUT_REG_B_REGID(uint32_t v) {
    return field(v, A6XX_SP_VS_OUT_REG_B_REGID__SHIFT, A6XX_SP_VS_OUT_REG_B_REGID__MASK);
}
constexpr uint32_t A6XX_SP_VS_OUT_REG_B_COMPMASK(uint32_t v) {
    return field(v, A6XX_SP_VS_OUT_REG_B_COMPMASK__SHIFT, A6XX_SP_VS_OUT_REG_B_COMPMASK__MASK);
}

// SP_VS_VPC_DST_REG[i]: the VPC location of var[4i..4i + 3], DIV_ROUND_UP(cnt, 4) of them
inline constexpr uint32_t REG_A6XX_SP_VS_VPC_DST = 0xa813;                                  // a6xx.xml.h:8124  the array's first register
inline constexpr uint32_t REG_A6XX_SP_VS_VPC_DST__STRIDE = 1;                               // a6xx.xml.h:8124  registers per entry
inline constexpr uint32_t REG_A6XX_SP_VS_VPC_DST_REG = 0xa813;                              // a6xx.xml.h:8126  entry 0
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC0__MASK = 0x000000ff;                // a6xx.xml.h:8127
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC0__SHIFT = 0;                        // a6xx.xml.h:8128
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC1__MASK = 0x0000ff00;                // a6xx.xml.h:8133
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC1__SHIFT = 8;                        // a6xx.xml.h:8134
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC2__MASK = 0x00ff0000;                // a6xx.xml.h:8139
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC2__SHIFT = 16;                       // a6xx.xml.h:8140
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC3__MASK = 0xff000000;                // a6xx.xml.h:8145
inline constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC3__SHIFT = 24;                       // a6xx.xml.h:8146
constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC0(uint32_t v) {
    return field(v, A6XX_SP_VS_VPC_DST_REG_OUTLOC0__SHIFT, A6XX_SP_VS_VPC_DST_REG_OUTLOC0__MASK);
}
constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC1(uint32_t v) {
    return field(v, A6XX_SP_VS_VPC_DST_REG_OUTLOC1__SHIFT, A6XX_SP_VS_VPC_DST_REG_OUTLOC1__MASK);
}
constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC2(uint32_t v) {
    return field(v, A6XX_SP_VS_VPC_DST_REG_OUTLOC2__SHIFT, A6XX_SP_VS_VPC_DST_REG_OUTLOC2__MASK);
}
constexpr uint32_t A6XX_SP_VS_VPC_DST_REG_OUTLOC3(uint32_t v) {
    return field(v, A6XX_SP_VS_VPC_DST_REG_OUTLOC3__SHIFT, A6XX_SP_VS_VPC_DST_REG_OUTLOC3__MASK);
}

// the VS object, one pkt4 of 7 at OBJ_FIRST_EXEC_OFFSET: 0, OBJ_START lo and hi, PVT_MEM_PARAM 0, PVT_MEM_ADDR lo
// and hi 0, PVT_MEM_SIZE 0 (tu_shader.cc:1319-1326). The XML gives OBJ_START align 32 (a6xx.xml:4835) but turnip
// asserts 128: the ST6_SHADER preload unit is one instruction group of 128 bytes (tu_shader.cc:1313-1316).
inline constexpr uint32_t REG_A6XX_SP_VS_OBJ_FIRST_EXEC_OFFSET = 0xa81b;                    // a6xx.xml.h:8153
inline constexpr uint32_t REG_A6XX_SP_VS_OBJ_START = 0xa81c;                                // a6xx.xml.h:8156
inline constexpr uint32_t REG_A6XX_SP_VS_PVT_MEM_PARAM = 0xa81e;                            // a6xx.xml.h:8159
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM__MASK = 0x000000ff;       // a6xx.xml.h:8160
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM__SHIFT = 0;               // a6xx.xml.h:8161
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM__SCALE = 9;               // a6xx.xml.h:8162  the encoder's val >> 9
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__MASK = 0xff000000; // a6xx.xml.h:8167
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__SHIFT = 24;        // a6xx.xml.h:8168
constexpr uint32_t A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM(uint32_t v) {
    return field(v >> A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM__SCALE, A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM__SHIFT, A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM__MASK);
}
constexpr uint32_t A6XX_SP_VS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD(uint32_t v) {
    return field(v, A6XX_SP_VS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__SHIFT, A6XX_SP_VS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_VS_PVT_MEM_ADDR = 0xa81f;                             // a6xx.xml.h:8175
inline constexpr uint32_t REG_A6XX_SP_VS_PVT_MEM_SIZE = 0xa821;                             // a6xx.xml.h:8178
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__MASK = 0x0003ffff;       // a6xx.xml.h:8179
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SHIFT = 0;               // a6xx.xml.h:8180
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SCALE = 12;              // a6xx.xml.h:8181  the encoder's val >> 12
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_SIZE_PERWAVEMEMLAYOUT = 0x80000000;            // a6xx.xml.h:8186
constexpr uint32_t A6XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(uint32_t v) {
    return field(v >> A6XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SCALE, A6XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SHIFT, A6XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_VS_TEX_COUNT = 0xa822;                                // a6xx.xml.h:8189
// ENABLED | NTEX | NSAMP (turnip omits NIBO; fd6 writes num_ibos): 0x100 for a stage with no textures. SP_FS_CONFIG
// and the HS/DS/GS ones have the same layout (static_assert below).
inline constexpr uint32_t REG_A6XX_SP_VS_CONFIG = 0xa823;                                   // a6xx.xml.h:8192
inline constexpr uint32_t A6XX_SP_VS_CONFIG_BINDLESS_TEX = 0x00000001;                      // a6xx.xml.h:8193
inline constexpr uint32_t A6XX_SP_VS_CONFIG_BINDLESS_SAMP = 0x00000002;                     // a6xx.xml.h:8194
inline constexpr uint32_t A6XX_SP_VS_CONFIG_BINDLESS_IBO = 0x00000004;                      // a6xx.xml.h:8195
inline constexpr uint32_t A6XX_SP_VS_CONFIG_BINDLESS_UBO = 0x00000008;                      // a6xx.xml.h:8196
inline constexpr uint32_t A6XX_SP_VS_CONFIG_ENABLED = 0x00000100;                           // a6xx.xml.h:8197
inline constexpr uint32_t A6XX_SP_VS_CONFIG_NTEX__MASK = 0x0001fe00;                        // a6xx.xml.h:8198
inline constexpr uint32_t A6XX_SP_VS_CONFIG_NTEX__SHIFT = 9;                                // a6xx.xml.h:8199
inline constexpr uint32_t A6XX_SP_VS_CONFIG_NSAMP__MASK = 0x003e0000;                       // a6xx.xml.h:8204
inline constexpr uint32_t A6XX_SP_VS_CONFIG_NSAMP__SHIFT = 17;                              // a6xx.xml.h:8205
inline constexpr uint32_t A6XX_SP_VS_CONFIG_NIBO__MASK = 0x1fc00000;                        // a6xx.xml.h:8210
inline constexpr uint32_t A6XX_SP_VS_CONFIG_NIBO__SHIFT = 22;                               // a6xx.xml.h:8211
constexpr uint32_t A6XX_SP_VS_CONFIG_NTEX(uint32_t v) {
    return field(v, A6XX_SP_VS_CONFIG_NTEX__SHIFT, A6XX_SP_VS_CONFIG_NTEX__MASK);
}
constexpr uint32_t A6XX_SP_VS_CONFIG_NSAMP(uint32_t v) {
    return field(v, A6XX_SP_VS_CONFIG_NSAMP__SHIFT, A6XX_SP_VS_CONFIG_NSAMP__MASK);
}
constexpr uint32_t A6XX_SP_VS_CONFIG_NIBO(uint32_t v) {
    return field(v, A6XX_SP_VS_CONFIG_NIBO__SHIFT, A6XX_SP_VS_CONFIG_NIBO__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_VS_INSTRLEN = 0xa824;                                 // a6xx.xml.h:8218
inline constexpr uint32_t REG_A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET = 0xa825;                  // a6xx.xml.h:8221
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET_OFFSET__MASK = 0x0007ffff;     // a6xx.xml.h:8222
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SHIFT = 0;             // a6xx.xml.h:8223
inline constexpr uint32_t A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SCALE = 11;            // a6xx.xml.h:8224  the encoder's val >> 11
constexpr uint32_t A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET_OFFSET(uint32_t v) {
    return field(v >> A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SCALE, A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SHIFT, A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET_OFFSET__MASK);
}

// the three stages a Metal pipeline has no use for: SP_xS_CONFIG and HLSQ_xS_CNTL written 0 (tu_pipeline.cc:381-388)
inline constexpr uint32_t REG_A6XX_SP_HS_CONFIG = 0xa83b;                                   // a6xx.xml.h:8308
inline constexpr uint32_t A6XX_SP_HS_CONFIG_BINDLESS_TEX = 0x00000001;                      // a6xx.xml.h:8309
inline constexpr uint32_t A6XX_SP_HS_CONFIG_BINDLESS_SAMP = 0x00000002;                     // a6xx.xml.h:8310
inline constexpr uint32_t A6XX_SP_HS_CONFIG_BINDLESS_IBO = 0x00000004;                      // a6xx.xml.h:8311
inline constexpr uint32_t A6XX_SP_HS_CONFIG_BINDLESS_UBO = 0x00000008;                      // a6xx.xml.h:8312
inline constexpr uint32_t A6XX_SP_HS_CONFIG_ENABLED = 0x00000100;                           // a6xx.xml.h:8313
inline constexpr uint32_t A6XX_SP_HS_CONFIG_NTEX__MASK = 0x0001fe00;                        // a6xx.xml.h:8314
inline constexpr uint32_t A6XX_SP_HS_CONFIG_NTEX__SHIFT = 9;                                // a6xx.xml.h:8315
inline constexpr uint32_t A6XX_SP_HS_CONFIG_NSAMP__MASK = 0x003e0000;                       // a6xx.xml.h:8320
inline constexpr uint32_t A6XX_SP_HS_CONFIG_NSAMP__SHIFT = 17;                              // a6xx.xml.h:8321
inline constexpr uint32_t A6XX_SP_HS_CONFIG_NIBO__MASK = 0x1fc00000;                        // a6xx.xml.h:8326
inline constexpr uint32_t A6XX_SP_HS_CONFIG_NIBO__SHIFT = 22;                               // a6xx.xml.h:8327
constexpr uint32_t A6XX_SP_HS_CONFIG_NTEX(uint32_t v) {
    return field(v, A6XX_SP_HS_CONFIG_NTEX__SHIFT, A6XX_SP_HS_CONFIG_NTEX__MASK);
}
constexpr uint32_t A6XX_SP_HS_CONFIG_NSAMP(uint32_t v) {
    return field(v, A6XX_SP_HS_CONFIG_NSAMP__SHIFT, A6XX_SP_HS_CONFIG_NSAMP__MASK);
}
constexpr uint32_t A6XX_SP_HS_CONFIG_NIBO(uint32_t v) {
    return field(v, A6XX_SP_HS_CONFIG_NIBO__SHIFT, A6XX_SP_HS_CONFIG_NIBO__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_DS_CONFIG = 0xa863;                                   // a6xx.xml.h:8494
inline constexpr uint32_t A6XX_SP_DS_CONFIG_BINDLESS_TEX = 0x00000001;                      // a6xx.xml.h:8495
inline constexpr uint32_t A6XX_SP_DS_CONFIG_BINDLESS_SAMP = 0x00000002;                     // a6xx.xml.h:8496
inline constexpr uint32_t A6XX_SP_DS_CONFIG_BINDLESS_IBO = 0x00000004;                      // a6xx.xml.h:8497
inline constexpr uint32_t A6XX_SP_DS_CONFIG_BINDLESS_UBO = 0x00000008;                      // a6xx.xml.h:8498
inline constexpr uint32_t A6XX_SP_DS_CONFIG_ENABLED = 0x00000100;                           // a6xx.xml.h:8499
inline constexpr uint32_t A6XX_SP_DS_CONFIG_NTEX__MASK = 0x0001fe00;                        // a6xx.xml.h:8500
inline constexpr uint32_t A6XX_SP_DS_CONFIG_NTEX__SHIFT = 9;                                // a6xx.xml.h:8501
inline constexpr uint32_t A6XX_SP_DS_CONFIG_NSAMP__MASK = 0x003e0000;                       // a6xx.xml.h:8506
inline constexpr uint32_t A6XX_SP_DS_CONFIG_NSAMP__SHIFT = 17;                              // a6xx.xml.h:8507
inline constexpr uint32_t A6XX_SP_DS_CONFIG_NIBO__MASK = 0x1fc00000;                        // a6xx.xml.h:8512
inline constexpr uint32_t A6XX_SP_DS_CONFIG_NIBO__SHIFT = 22;                               // a6xx.xml.h:8513
constexpr uint32_t A6XX_SP_DS_CONFIG_NTEX(uint32_t v) {
    return field(v, A6XX_SP_DS_CONFIG_NTEX__SHIFT, A6XX_SP_DS_CONFIG_NTEX__MASK);
}
constexpr uint32_t A6XX_SP_DS_CONFIG_NSAMP(uint32_t v) {
    return field(v, A6XX_SP_DS_CONFIG_NSAMP__SHIFT, A6XX_SP_DS_CONFIG_NSAMP__MASK);
}
constexpr uint32_t A6XX_SP_DS_CONFIG_NIBO(uint32_t v) {
    return field(v, A6XX_SP_DS_CONFIG_NIBO__SHIFT, A6XX_SP_DS_CONFIG_NIBO__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_GS_CONFIG = 0xa894;                                   // a6xx.xml.h:8683
inline constexpr uint32_t A6XX_SP_GS_CONFIG_BINDLESS_TEX = 0x00000001;                      // a6xx.xml.h:8684
inline constexpr uint32_t A6XX_SP_GS_CONFIG_BINDLESS_SAMP = 0x00000002;                     // a6xx.xml.h:8685
inline constexpr uint32_t A6XX_SP_GS_CONFIG_BINDLESS_IBO = 0x00000004;                      // a6xx.xml.h:8686
inline constexpr uint32_t A6XX_SP_GS_CONFIG_BINDLESS_UBO = 0x00000008;                      // a6xx.xml.h:8687
inline constexpr uint32_t A6XX_SP_GS_CONFIG_ENABLED = 0x00000100;                           // a6xx.xml.h:8688
inline constexpr uint32_t A6XX_SP_GS_CONFIG_NTEX__MASK = 0x0001fe00;                        // a6xx.xml.h:8689
inline constexpr uint32_t A6XX_SP_GS_CONFIG_NTEX__SHIFT = 9;                                // a6xx.xml.h:8690
inline constexpr uint32_t A6XX_SP_GS_CONFIG_NSAMP__MASK = 0x003e0000;                       // a6xx.xml.h:8695
inline constexpr uint32_t A6XX_SP_GS_CONFIG_NSAMP__SHIFT = 17;                              // a6xx.xml.h:8696
inline constexpr uint32_t A6XX_SP_GS_CONFIG_NIBO__MASK = 0x1fc00000;                        // a6xx.xml.h:8701
inline constexpr uint32_t A6XX_SP_GS_CONFIG_NIBO__SHIFT = 22;                               // a6xx.xml.h:8702
constexpr uint32_t A6XX_SP_GS_CONFIG_NTEX(uint32_t v) {
    return field(v, A6XX_SP_GS_CONFIG_NTEX__SHIFT, A6XX_SP_GS_CONFIG_NTEX__MASK);
}
constexpr uint32_t A6XX_SP_GS_CONFIG_NSAMP(uint32_t v) {
    return field(v, A6XX_SP_GS_CONFIG_NSAMP__SHIFT, A6XX_SP_GS_CONFIG_NSAMP__MASK);
}
constexpr uint32_t A6XX_SP_GS_CONFIG_NIBO(uint32_t v) {
    return field(v, A6XX_SP_GS_CONFIG_NIBO__SHIFT, A6XX_SP_GS_CONFIG_NIBO__MASK);
}

// the VS's texture and sampler tables (64-bit each): step two
inline constexpr uint32_t REG_A6XX_SP_VS_TEX_SAMP = 0xa8a0;                                 // a6xx.xml.h:8725
inline constexpr uint32_t REG_A6XX_SP_VS_TEX_CONST = 0xa8a8;                                // a6xx.xml.h:8737

// SP_FS: the fragment shader's (REG_A6XX_SP_FS_CTRL_REG0 and SP_FS_INSTRLEN are above). SP_FS_CTRL_REG0 adds to the
// footprints THREADSIZE (THREAD128: this chip has double threadsize), VARYING when total_in != 0, LODPIXMASK when
// the shader needs full quads, PIXLODENABLE when it needs the pixel's lod, and UNK24 always ("unknown bit, seems
// unnecessary", tu_shader.cc:1284-1296; fd6_program.cc:159-171 sets it too); MERGEDREGS is bit 31 here.
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_THREADMODE__MASK = 0x00000001;               // a6xx.xml.h:8750
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_THREADMODE__SHIFT = 0;                       // a6xx.xml.h:8751
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT__MASK = 0x0000007e;         // a6xx.xml.h:8756
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT__SHIFT = 1;                 // a6xx.xml.h:8757
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT__MASK = 0x00001f80;         // a6xx.xml.h:8762
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT__SHIFT = 7;                 // a6xx.xml.h:8763
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_UNK13 = 0x00002000;                          // a6xx.xml.h:8768
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_BRANCHSTACK__MASK = 0x000fc000;              // a6xx.xml.h:8769
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_BRANCHSTACK__SHIFT = 14;                     // a6xx.xml.h:8770
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_THREADSIZE__MASK = 0x00100000;               // a6xx.xml.h:8775
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_THREADSIZE__SHIFT = 20;                      // a6xx.xml.h:8776
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_UNK21 = 0x00200000;                          // a6xx.xml.h:8781
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_VARYING = 0x00400000;                        // a6xx.xml.h:8782
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_LODPIXMASK = 0x00800000;                     // a6xx.xml.h:8783
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_UNK24 = 0x01000000;                          // a6xx.xml.h:8784
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_UNK25 = 0x02000000;                          // a6xx.xml.h:8785
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_PIXLODENABLE = 0x04000000;                   // a6xx.xml.h:8786
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_UNK27 = 0x08000000;                          // a6xx.xml.h:8787
inline constexpr uint32_t A6XX_SP_FS_CTRL_REG0_MERGEDREGS = 0x80000000;                     // a6xx.xml.h:8789
constexpr uint32_t A6XX_SP_FS_CTRL_REG0_THREADMODE(a3xx_threadmode v) {
    return field(v, A6XX_SP_FS_CTRL_REG0_THREADMODE__SHIFT, A6XX_SP_FS_CTRL_REG0_THREADMODE__MASK);
}
constexpr uint32_t A6XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(uint32_t v) {
    return field(v, A6XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT__SHIFT, A6XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT__MASK);
}
constexpr uint32_t A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(uint32_t v) {
    return field(v, A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT__SHIFT, A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT__MASK);
}
constexpr uint32_t A6XX_SP_FS_CTRL_REG0_BRANCHSTACK(uint32_t v) {
    return field(v, A6XX_SP_FS_CTRL_REG0_BRANCHSTACK__SHIFT, A6XX_SP_FS_CTRL_REG0_BRANCHSTACK__MASK);
}
constexpr uint32_t A6XX_SP_FS_CTRL_REG0_THREADSIZE(a6xx_threadsize v) {
    return field(v, A6XX_SP_FS_CTRL_REG0_THREADSIZE__SHIFT, A6XX_SP_FS_CTRL_REG0_THREADSIZE__MASK);
}

// the FS object block, as the VS's: a pkt4 of 7 at OBJ_FIRST_EXEC_OFFSET
inline constexpr uint32_t REG_A6XX_SP_FS_OBJ_FIRST_EXEC_OFFSET = 0xa982;                    // a6xx.xml.h:8795
inline constexpr uint32_t REG_A6XX_SP_FS_OBJ_START = 0xa983;                                // a6xx.xml.h:8798
inline constexpr uint32_t REG_A6XX_SP_FS_PVT_MEM_PARAM = 0xa985;                            // a6xx.xml.h:8801
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_PARAM_MEMSIZEPERITEM__MASK = 0x000000ff;       // a6xx.xml.h:8802
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_PARAM_MEMSIZEPERITEM__SHIFT = 0;               // a6xx.xml.h:8803
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_PARAM_MEMSIZEPERITEM__SCALE = 9;               // a6xx.xml.h:8804  the encoder's val >> 9
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__MASK = 0xff000000; // a6xx.xml.h:8809
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__SHIFT = 24;        // a6xx.xml.h:8810
constexpr uint32_t A6XX_SP_FS_PVT_MEM_PARAM_MEMSIZEPERITEM(uint32_t v) {
    return field(v >> A6XX_SP_FS_PVT_MEM_PARAM_MEMSIZEPERITEM__SCALE, A6XX_SP_FS_PVT_MEM_PARAM_MEMSIZEPERITEM__SHIFT, A6XX_SP_FS_PVT_MEM_PARAM_MEMSIZEPERITEM__MASK);
}
constexpr uint32_t A6XX_SP_FS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD(uint32_t v) {
    return field(v, A6XX_SP_FS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__SHIFT, A6XX_SP_FS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_FS_PVT_MEM_ADDR = 0xa986;                             // a6xx.xml.h:8817
inline constexpr uint32_t REG_A6XX_SP_FS_PVT_MEM_SIZE = 0xa988;                             // a6xx.xml.h:8820
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__MASK = 0x0003ffff;       // a6xx.xml.h:8821
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SHIFT = 0;               // a6xx.xml.h:8822
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SCALE = 12;              // a6xx.xml.h:8823  the encoder's val >> 12
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_SIZE_PERWAVEMEMLAYOUT = 0x80000000;            // a6xx.xml.h:8828
constexpr uint32_t A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(uint32_t v) {
    return field(v >> A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SCALE, A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__SHIFT, A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE__MASK);
}

// blending off in the SP: UNK8 alone (turnip .unk8 = true, tu_pipeline.cc:3009-3014; fd6_blend.cc:120-126); r3d 0
inline constexpr uint32_t REG_A6XX_SP_BLEND_CNTL = 0xa989;                                  // a6xx.xml.h:8831
inline constexpr uint32_t A6XX_SP_BLEND_CNTL_ENABLE_BLEND__MASK = 0x000000ff;               // a6xx.xml.h:8832
inline constexpr uint32_t A6XX_SP_BLEND_CNTL_ENABLE_BLEND__SHIFT = 0;                       // a6xx.xml.h:8833
inline constexpr uint32_t A6XX_SP_BLEND_CNTL_UNK8 = 0x00000100;                             // a6xx.xml.h:8838
inline constexpr uint32_t A6XX_SP_BLEND_CNTL_DUAL_COLOR_IN_ENABLE = 0x00000200;             // a6xx.xml.h:8839
inline constexpr uint32_t A6XX_SP_BLEND_CNTL_ALPHA_TO_COVERAGE = 0x00000400;                // a6xx.xml.h:8840
constexpr uint32_t A6XX_SP_BLEND_CNTL_ENABLE_BLEND(uint32_t v) {
    return field(v, A6XX_SP_BLEND_CNTL_ENABLE_BLEND__SHIFT, A6XX_SP_BLEND_CNTL_ENABLE_BLEND__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_SRGB_CNTL = 0xa98a;                                   // a6xx.xml.h:8843
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT0 = 0x00000001;                         // a6xx.xml.h:8844
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT1 = 0x00000002;                         // a6xx.xml.h:8845
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT2 = 0x00000004;                         // a6xx.xml.h:8846
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT3 = 0x00000008;                         // a6xx.xml.h:8847
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT4 = 0x00000010;                         // a6xx.xml.h:8848
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT5 = 0x00000020;                         // a6xx.xml.h:8849
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT6 = 0x00000040;                         // a6xx.xml.h:8850
inline constexpr uint32_t A6XX_SP_SRGB_CNTL_SRGB_MRT7 = 0x00000080;                         // a6xx.xml.h:8851

// 0xf per colour output written (tu_shader.cc:1873-1874): the RB twin must match
inline constexpr uint32_t REG_A6XX_SP_FS_RENDER_COMPONENTS = 0xa98b;                        // a6xx.xml.h:8854
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT0__MASK = 0x0000000f;              // a6xx.xml.h:8855
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT0__SHIFT = 0;                      // a6xx.xml.h:8856
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT1__MASK = 0x000000f0;              // a6xx.xml.h:8861
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT1__SHIFT = 4;                      // a6xx.xml.h:8862
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT2__MASK = 0x00000f00;              // a6xx.xml.h:8867
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT2__SHIFT = 8;                      // a6xx.xml.h:8868
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT3__MASK = 0x0000f000;              // a6xx.xml.h:8873
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT3__SHIFT = 12;                     // a6xx.xml.h:8874
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT4__MASK = 0x000f0000;              // a6xx.xml.h:8879
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT4__SHIFT = 16;                     // a6xx.xml.h:8880
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT5__MASK = 0x00f00000;              // a6xx.xml.h:8885
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT5__SHIFT = 20;                     // a6xx.xml.h:8886
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT6__MASK = 0x0f000000;              // a6xx.xml.h:8891
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT6__SHIFT = 24;                     // a6xx.xml.h:8892
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT7__MASK = 0xf0000000;              // a6xx.xml.h:8897
inline constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT7__SHIFT = 28;                     // a6xx.xml.h:8898
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT0(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT0__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT0__MASK);
}
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT1(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT1__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT1__MASK);
}
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT2(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT2__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT2__MASK);
}
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT3(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT3__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT3__MASK);
}
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT4(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT4__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT4__MASK);
}
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT5(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT5__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT5__MASK);
}
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT6(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT6__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT6__MASK);
}
constexpr uint32_t A6XX_SP_FS_RENDER_COMPONENTS_RT7(uint32_t v) {
    return field(v, A6XX_SP_FS_RENDER_COMPONENTS_RT7__SHIFT, A6XX_SP_FS_RENDER_COMPONENTS_RT7__MASK);
}

// the registers of the FS's depth, sample mask and stencil ref outputs, kRegidNone each = 0xfcfcfc00 (tu_shader.cc:1850-1854)
inline constexpr uint32_t REG_A6XX_SP_FS_OUTPUT_CNTL0 = 0xa98c;                             // a6xx.xml.h:8905
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE = 0x00000001;        // a6xx.xml.h:8906
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__MASK = 0x0000ff00;           // a6xx.xml.h:8907
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__SHIFT = 8;                   // a6xx.xml.h:8908
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID__MASK = 0x00ff0000;        // a6xx.xml.h:8913
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID__SHIFT = 16;               // a6xx.xml.h:8914
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_STENCILREF_REGID__MASK = 0xff000000;      // a6xx.xml.h:8919
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_STENCILREF_REGID__SHIFT = 24;             // a6xx.xml.h:8920
constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID(uint32_t v) {
    return field(v, A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__SHIFT, A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID__MASK);
}
constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID(uint32_t v) {
    return field(v, A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID__SHIFT, A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID__MASK);
}
constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL0_STENCILREF_REGID(uint32_t v) {
    return field(v, A6XX_SP_FS_OUTPUT_CNTL0_STENCILREF_REGID__SHIFT, A6XX_SP_FS_OUTPUT_CNTL0_STENCILREF_REGID__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_FS_OUTPUT_CNTL1 = 0xa98d;                             // a6xx.xml.h:8927
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL1_MRT__MASK = 0x0000000f;                   // a6xx.xml.h:8928
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL1_MRT__SHIFT = 0;                           // a6xx.xml.h:8929
constexpr uint32_t A6XX_SP_FS_OUTPUT_CNTL1_MRT(uint32_t v) {
    return field(v, A6XX_SP_FS_OUTPUT_CNTL1_MRT__SHIFT, A6XX_SP_FS_OUTPUT_CNTL1_MRT__MASK);
}

// SP_FS_OUTPUT_REG[i]: the ir3 output register of FRAG_RESULT_DATA0 + i, HALF_PRECISION when it is a half register
// (tu_shader.cc:1862-1871)
inline constexpr uint32_t REG_A6XX_SP_FS_OUTPUT = 0xa98e;                                   // a6xx.xml.h:8936  the array's first register
inline constexpr uint32_t REG_A6XX_SP_FS_OUTPUT__STRIDE = 1;                                // a6xx.xml.h:8936  registers per entry
inline constexpr uint32_t REG_A6XX_SP_FS_OUTPUT_REG = 0xa98e;                               // a6xx.xml.h:8938  entry 0
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_REG_REGID__MASK = 0x000000ff;                   // a6xx.xml.h:8939
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_REG_REGID__SHIFT = 0;                           // a6xx.xml.h:8940
inline constexpr uint32_t A6XX_SP_FS_OUTPUT_REG_HALF_PRECISION = 0x00000100;                // a6xx.xml.h:8945
constexpr uint32_t A6XX_SP_FS_OUTPUT_REG_REGID(uint32_t v) {
    return field(v, A6XX_SP_FS_OUTPUT_REG_REGID__SHIFT, A6XX_SP_FS_OUTPUT_REG_REGID__MASK);
}

// SP_FS_MRT_REG[i]: the target's format for the SP (fd6_view.c:415-418): COLOR_FORMAT, SINT/UINT for integer formats
inline constexpr uint32_t REG_A6XX_SP_FS_MRT = 0xa996;                                      // a6xx.xml.h:8948  the array's first register
inline constexpr uint32_t REG_A6XX_SP_FS_MRT__STRIDE = 1;                                   // a6xx.xml.h:8948  registers per entry
inline constexpr uint32_t REG_A6XX_SP_FS_MRT_REG = 0xa996;                                  // a6xx.xml.h:8950  entry 0
inline constexpr uint32_t A6XX_SP_FS_MRT_REG_COLOR_FORMAT__MASK = 0x000000ff;               // a6xx.xml.h:8951
inline constexpr uint32_t A6XX_SP_FS_MRT_REG_COLOR_FORMAT__SHIFT = 0;                       // a6xx.xml.h:8952
inline constexpr uint32_t A6XX_SP_FS_MRT_REG_COLOR_SINT = 0x00000100;                       // a6xx.xml.h:8957
inline constexpr uint32_t A6XX_SP_FS_MRT_REG_COLOR_UINT = 0x00000200;                       // a6xx.xml.h:8958
inline constexpr uint32_t A6XX_SP_FS_MRT_REG_UNK10 = 0x00000400;                            // a6xx.xml.h:8959
constexpr uint32_t A6XX_SP_FS_MRT_REG_COLOR_FORMAT(a6xx_format v) {
    return field(v, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__SHIFT, A6XX_SP_FS_MRT_REG_COLOR_FORMAT__MASK);
}

// texture prefetch: none in step one (COUNT 0); IJ_WRITE_DISABLE when the FS has no perspective barycentric input
// (tu_shader.cc:1652-1660): 0x8 for a constant colour, 0 with a smooth varying
inline constexpr uint32_t REG_A6XX_SP_FS_PREFETCH_CNTL = 0xa99e;                            // a6xx.xml.h:8962
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_COUNT__MASK = 0x00000007;                // a6xx.xml.h:8963
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_COUNT__SHIFT = 0;                        // a6xx.xml.h:8964
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_IJ_WRITE_DISABLE = 0x00000008;           // a6xx.xml.h:8969
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_ENDOFQUAD = 0x00000010;                  // a6xx.xml.h:8970
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_WRITE_COLOR_TO_OUTPUT = 0x00000020;      // a6xx.xml.h:8971
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID__MASK = 0x00007fc0;          // a6xx.xml.h:8972
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID__SHIFT = 6;                  // a6xx.xml.h:8973
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID4COORD__MASK = 0x01ff0000;    // a6xx.xml.h:8978
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID4COORD__SHIFT = 16;           // a6xx.xml.h:8979
constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_COUNT(uint32_t v) {
    return field(v, A6XX_SP_FS_PREFETCH_CNTL_COUNT__SHIFT, A6XX_SP_FS_PREFETCH_CNTL_COUNT__MASK);
}
constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID(uint32_t v) {
    return field(v, A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID__SHIFT, A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID__MASK);
}
constexpr uint32_t A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID4COORD(uint32_t v) {
    return field(v, A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID4COORD__SHIFT, A6XX_SP_FS_PREFETCH_CNTL_CONSTSLOTID4COORD__MASK);
}

// SP_FS_PREFETCH_CMD[i], one word a prefetch after SP_FS_PREFETCH_CNTL in the same pkt4 (fd6 emit_fs_inputs,
// fd6_program.cc:947-971): the coordinate's register (SRC), the sampler and texture slots (A6XX keeps them here; A7XX
// moves them to SP_FS_BINDLESS_PREFETCH_CMD), the destination register, the components written, HALF, and the
// operation -- TEX_PREFETCH_SAM, the only one ir3 makes (tex_opc_to_prefetch_cmd, fd6_program.cc:910-918)
inline constexpr uint32_t REG_A6XX_SP_FS_PREFETCH_CMD = 0xa99f;                              // a6xx.xml.h:8988  entry 0
inline constexpr uint32_t REG_A6XX_SP_FS_PREFETCH_CMD__STRIDE = 1;                           // a6xx.xml.h:8988
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_SRC__MASK = 127;                          // a6xx.xml.h:8989
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_SRC__SHIFT = 0;                           // a6xx.xml.h:8990
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_SAMP_ID__MASK = 0x00000780;               // a6xx.xml.h:8995
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_SAMP_ID__SHIFT = 7;                       // a6xx.xml.h:8996
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_TEX_ID__MASK = 0x0000f800;                // a6xx.xml.h:9001
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_TEX_ID__SHIFT = 11;                       // a6xx.xml.h:9002
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_DST__MASK = 0x003f0000;                   // a6xx.xml.h:9007
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_DST__SHIFT = 16;                          // a6xx.xml.h:9008
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_WRMASK__MASK = 0x03c00000;                // a6xx.xml.h:9013
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_WRMASK__SHIFT = 22;                       // a6xx.xml.h:9014
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_HALF = 0x04000000;                        // a6xx.xml.h:9019
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_BINDLESS = 0x10000000;                    // a6xx.xml.h:9021
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_CMD__MASK = 0xe0000000;                   // a6xx.xml.h:9022
inline constexpr uint32_t A6XX_SP_FS_PREFETCH_CMD_CMD__SHIFT = 29;                          // a6xx.xml.h:9023
enum a6xx_tex_prefetch_cmd : uint32_t {
    TEX_PREFETCH_SAM = 1,          // adreno_common.xml.h:221
};
constexpr uint32_t spFsPrefetchCmd(uint32_t src, uint32_t sampId, uint32_t texId, uint32_t dst, uint32_t wrmask, bool half, uint32_t cmd) {
    return field(src, A6XX_SP_FS_PREFETCH_CMD_SRC__SHIFT, A6XX_SP_FS_PREFETCH_CMD_SRC__MASK) |
           field(sampId, A6XX_SP_FS_PREFETCH_CMD_SAMP_ID__SHIFT, A6XX_SP_FS_PREFETCH_CMD_SAMP_ID__MASK) |
           field(texId, A6XX_SP_FS_PREFETCH_CMD_TEX_ID__SHIFT, A6XX_SP_FS_PREFETCH_CMD_TEX_ID__MASK) |
           field(dst, A6XX_SP_FS_PREFETCH_CMD_DST__SHIFT, A6XX_SP_FS_PREFETCH_CMD_DST__MASK) |
           field(wrmask, A6XX_SP_FS_PREFETCH_CMD_WRMASK__SHIFT, A6XX_SP_FS_PREFETCH_CMD_WRMASK__MASK) | (half ? A6XX_SP_FS_PREFETCH_CMD_HALF : 0u) |
           field(cmd, A6XX_SP_FS_PREFETCH_CMD_CMD__SHIFT, A6XX_SP_FS_PREFETCH_CMD_CMD__MASK);
}

inline constexpr uint32_t REG_A6XX_SP_FS_TEX_COUNT = 0xa9a7;                                // a6xx.xml.h:9090
inline constexpr uint32_t REG_A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET = 0xa9a9;                  // a6xx.xml.h:9096
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET__MASK = 0x0007ffff;     // a6xx.xml.h:9097
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SHIFT = 0;             // a6xx.xml.h:9098
inline constexpr uint32_t A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SCALE = 11;            // a6xx.xml.h:9099  the encoder's val >> 11
constexpr uint32_t A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET(uint32_t v) {
    return field(v >> A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SCALE, A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET__SHIFT, A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_FS_TEX_SAMP = 0xa9e0;                                 // a6xx.xml.h:9315
inline constexpr uint32_t REG_A6XX_SP_FS_TEX_CONST = 0xa9e4;                                // a6xx.xml.h:9321
inline constexpr uint32_t REG_A6XX_SP_FS_CONFIG = 0xab04;                                   // a6xx.xml.h:9447
inline constexpr uint32_t A6XX_SP_FS_CONFIG_BINDLESS_TEX = 0x00000001;                      // a6xx.xml.h:9448
inline constexpr uint32_t A6XX_SP_FS_CONFIG_BINDLESS_SAMP = 0x00000002;                     // a6xx.xml.h:9449
inline constexpr uint32_t A6XX_SP_FS_CONFIG_BINDLESS_IBO = 0x00000004;                      // a6xx.xml.h:9450
inline constexpr uint32_t A6XX_SP_FS_CONFIG_BINDLESS_UBO = 0x00000008;                      // a6xx.xml.h:9451
inline constexpr uint32_t A6XX_SP_FS_CONFIG_ENABLED = 0x00000100;                           // a6xx.xml.h:9452
inline constexpr uint32_t A6XX_SP_FS_CONFIG_NTEX__MASK = 0x0001fe00;                        // a6xx.xml.h:9453
inline constexpr uint32_t A6XX_SP_FS_CONFIG_NTEX__SHIFT = 9;                                // a6xx.xml.h:9454
inline constexpr uint32_t A6XX_SP_FS_CONFIG_NSAMP__MASK = 0x003e0000;                       // a6xx.xml.h:9459
inline constexpr uint32_t A6XX_SP_FS_CONFIG_NSAMP__SHIFT = 17;                              // a6xx.xml.h:9460
inline constexpr uint32_t A6XX_SP_FS_CONFIG_NIBO__MASK = 0x1fc00000;                        // a6xx.xml.h:9465
inline constexpr uint32_t A6XX_SP_FS_CONFIG_NIBO__SHIFT = 22;                               // a6xx.xml.h:9466
constexpr uint32_t A6XX_SP_FS_CONFIG_NTEX(uint32_t v) {
    return field(v, A6XX_SP_FS_CONFIG_NTEX__SHIFT, A6XX_SP_FS_CONFIG_NTEX__MASK);
}
constexpr uint32_t A6XX_SP_FS_CONFIG_NSAMP(uint32_t v) {
    return field(v, A6XX_SP_FS_CONFIG_NSAMP__SHIFT, A6XX_SP_FS_CONFIG_NSAMP__MASK);
}
constexpr uint32_t A6XX_SP_FS_CONFIG_NIBO(uint32_t v) {
    return field(v, A6XX_SP_FS_CONFIG_NIBO__SHIFT, A6XX_SP_FS_CONFIG_NIBO__MASK);
}

// the 3D pipeline's IBO descriptors (64-bit; SP_IBO_COUNT is above): step one binds none
inline constexpr uint32_t REG_A6XX_SP_IBO = 0xab1a;                                         // a6xx.xml.h:9512

// SP_TP: MSAA off, sample locations off, the window offset
inline constexpr uint32_t REG_A6XX_SP_TP_RAS_MSAA_CNTL = 0xb300;                            // a6xx.xml.h:9668
inline constexpr uint32_t A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES__MASK = 0x00000003;              // a6xx.xml.h:9669
inline constexpr uint32_t A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES__SHIFT = 0;                      // a6xx.xml.h:9670
inline constexpr uint32_t A6XX_SP_TP_RAS_MSAA_CNTL_UNK2__MASK = 0x0000000c;                 // a6xx.xml.h:9675
inline constexpr uint32_t A6XX_SP_TP_RAS_MSAA_CNTL_UNK2__SHIFT = 2;                         // a6xx.xml.h:9676
constexpr uint32_t A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES(a3xx_msaa_samples v) {
    return field(v, A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES__SHIFT, A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES__MASK);
}
constexpr uint32_t A6XX_SP_TP_RAS_MSAA_CNTL_UNK2(uint32_t v) {
    return field(v, A6XX_SP_TP_RAS_MSAA_CNTL_UNK2__SHIFT, A6XX_SP_TP_RAS_MSAA_CNTL_UNK2__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_TP_DEST_MSAA_CNTL = 0xb301;                           // a6xx.xml.h:9683
inline constexpr uint32_t A6XX_SP_TP_DEST_MSAA_CNTL_SAMPLES__MASK = 0x00000003;             // a6xx.xml.h:9684
inline constexpr uint32_t A6XX_SP_TP_DEST_MSAA_CNTL_SAMPLES__SHIFT = 0;                     // a6xx.xml.h:9685
inline constexpr uint32_t A6XX_SP_TP_DEST_MSAA_CNTL_MSAA_DISABLE = 0x00000004;              // a6xx.xml.h:9690
constexpr uint32_t A6XX_SP_TP_DEST_MSAA_CNTL_SAMPLES(a3xx_msaa_samples v) {
    return field(v, A6XX_SP_TP_DEST_MSAA_CNTL_SAMPLES__SHIFT, A6XX_SP_TP_DEST_MSAA_CNTL_SAMPLES__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_TP_SAMPLE_CONFIG = 0xb304;                            // a6xx.xml.h:9696
inline constexpr uint32_t A6XX_SP_TP_SAMPLE_CONFIG_UNK0 = 0x00000001;                       // a6xx.xml.h:9697
inline constexpr uint32_t A6XX_SP_TP_SAMPLE_CONFIG_LOCATION_ENABLE = 0x00000002;            // a6xx.xml.h:9698
inline constexpr uint32_t REG_A6XX_SP_TP_WINDOW_OFFSET = 0xb307;                            // a6xx.xml.h:9803
inline constexpr uint32_t A6XX_SP_TP_WINDOW_OFFSET_X__MASK = 0x00003fff;                    // a6xx.xml.h:9804
inline constexpr uint32_t A6XX_SP_TP_WINDOW_OFFSET_X__SHIFT = 0;                            // a6xx.xml.h:9805
inline constexpr uint32_t A6XX_SP_TP_WINDOW_OFFSET_Y__MASK = 0x3fff0000;                    // a6xx.xml.h:9810
inline constexpr uint32_t A6XX_SP_TP_WINDOW_OFFSET_Y__SHIFT = 16;                           // a6xx.xml.h:9811
constexpr uint32_t A6XX_SP_TP_WINDOW_OFFSET_X(uint32_t v) {
    return field(v, A6XX_SP_TP_WINDOW_OFFSET_X__SHIFT, A6XX_SP_TP_WINDOW_OFFSET_X__MASK);
}
constexpr uint32_t A6XX_SP_TP_WINDOW_OFFSET_Y(uint32_t v) {
    return field(v, A6XX_SP_TP_WINDOW_OFFSET_Y__SHIFT, A6XX_SP_TP_WINDOW_OFFSET_Y__MASK);
}
inline constexpr uint32_t REG_A6XX_SP_WINDOW_OFFSET = 0xb4d1;                               // a6xx.xml.h:10055
inline constexpr uint32_t A6XX_SP_WINDOW_OFFSET_X__MASK = 0x00003fff;                       // a6xx.xml.h:10056
inline constexpr uint32_t A6XX_SP_WINDOW_OFFSET_X__SHIFT = 0;                               // a6xx.xml.h:10057
inline constexpr uint32_t A6XX_SP_WINDOW_OFFSET_Y__MASK = 0x3fff0000;                       // a6xx.xml.h:10062
inline constexpr uint32_t A6XX_SP_WINDOW_OFFSET_Y__SHIFT = 16;                              // a6xx.xml.h:10063
constexpr uint32_t A6XX_SP_WINDOW_OFFSET_X(uint32_t v) {
    return field(v, A6XX_SP_WINDOW_OFFSET_X__SHIFT, A6XX_SP_WINDOW_OFFSET_X__MASK);
}
constexpr uint32_t A6XX_SP_WINDOW_OFFSET_Y(uint32_t v) {
    return field(v, A6XX_SP_WINDOW_OFFSET_Y__SHIFT, A6XX_SP_WINDOW_OFFSET_Y__MASK);
}

// HLSQ: per stage CONSTLEN | ENABLED, the length in vec4 held in units of 4 (the encoder's val >> 2, asserting a
// multiple: a6xx.xml.h:10203); hlsqVsCntl/hlsqFsCntl below round up as hlsqCsCntl does. HS, DS and GS are written 0.
inline constexpr uint32_t REG_A6XX_HLSQ_VS_CNTL = 0xb800;                                   // a6xx.xml.h:10198
inline constexpr uint32_t A6XX_HLSQ_VS_CNTL_CONSTLEN__MASK = 0x000000ff;                    // a6xx.xml.h:10199
inline constexpr uint32_t A6XX_HLSQ_VS_CNTL_CONSTLEN__SHIFT = 0;                            // a6xx.xml.h:10200
inline constexpr uint32_t A6XX_HLSQ_VS_CNTL_CONSTLEN__SCALE = 2;                            // a6xx.xml.h:10201  the encoder's val >> 2
inline constexpr uint32_t A6XX_HLSQ_VS_CNTL_ENABLED = 0x00000100;                           // a6xx.xml.h:10206
inline constexpr uint32_t A6XX_HLSQ_VS_CNTL_READ_IMM_SHARED_CONSTS = 0x00000200;            // a6xx.xml.h:10207
constexpr uint32_t A6XX_HLSQ_VS_CNTL_CONSTLEN(uint32_t v) {
    return field(v >> A6XX_HLSQ_VS_CNTL_CONSTLEN__SCALE, A6XX_HLSQ_VS_CNTL_CONSTLEN__SHIFT, A6XX_HLSQ_VS_CNTL_CONSTLEN__MASK);
}
inline constexpr uint32_t REG_A6XX_HLSQ_HS_CNTL = 0xb801;                                   // a6xx.xml.h:10210
inline constexpr uint32_t A6XX_HLSQ_HS_CNTL_CONSTLEN__MASK = 0x000000ff;                    // a6xx.xml.h:10211
inline constexpr uint32_t A6XX_HLSQ_HS_CNTL_CONSTLEN__SHIFT = 0;                            // a6xx.xml.h:10212
inline constexpr uint32_t A6XX_HLSQ_HS_CNTL_CONSTLEN__SCALE = 2;                            // a6xx.xml.h:10213  the encoder's val >> 2
inline constexpr uint32_t A6XX_HLSQ_HS_CNTL_ENABLED = 0x00000100;                           // a6xx.xml.h:10218
inline constexpr uint32_t A6XX_HLSQ_HS_CNTL_READ_IMM_SHARED_CONSTS = 0x00000200;            // a6xx.xml.h:10219
constexpr uint32_t A6XX_HLSQ_HS_CNTL_CONSTLEN(uint32_t v) {
    return field(v >> A6XX_HLSQ_HS_CNTL_CONSTLEN__SCALE, A6XX_HLSQ_HS_CNTL_CONSTLEN__SHIFT, A6XX_HLSQ_HS_CNTL_CONSTLEN__MASK);
}
inline constexpr uint32_t REG_A6XX_HLSQ_DS_CNTL = 0xb802;                                   // a6xx.xml.h:10222
inline constexpr uint32_t A6XX_HLSQ_DS_CNTL_CONSTLEN__MASK = 0x000000ff;                    // a6xx.xml.h:10223
inline constexpr uint32_t A6XX_HLSQ_DS_CNTL_CONSTLEN__SHIFT = 0;                            // a6xx.xml.h:10224
inline constexpr uint32_t A6XX_HLSQ_DS_CNTL_CONSTLEN__SCALE = 2;                            // a6xx.xml.h:10225  the encoder's val >> 2
inline constexpr uint32_t A6XX_HLSQ_DS_CNTL_ENABLED = 0x00000100;                           // a6xx.xml.h:10230
inline constexpr uint32_t A6XX_HLSQ_DS_CNTL_READ_IMM_SHARED_CONSTS = 0x00000200;            // a6xx.xml.h:10231
constexpr uint32_t A6XX_HLSQ_DS_CNTL_CONSTLEN(uint32_t v) {
    return field(v >> A6XX_HLSQ_DS_CNTL_CONSTLEN__SCALE, A6XX_HLSQ_DS_CNTL_CONSTLEN__SHIFT, A6XX_HLSQ_DS_CNTL_CONSTLEN__MASK);
}
inline constexpr uint32_t REG_A6XX_HLSQ_GS_CNTL = 0xb803;                                   // a6xx.xml.h:10234
inline constexpr uint32_t A6XX_HLSQ_GS_CNTL_CONSTLEN__MASK = 0x000000ff;                    // a6xx.xml.h:10235
inline constexpr uint32_t A6XX_HLSQ_GS_CNTL_CONSTLEN__SHIFT = 0;                            // a6xx.xml.h:10236
inline constexpr uint32_t A6XX_HLSQ_GS_CNTL_CONSTLEN__SCALE = 2;                            // a6xx.xml.h:10237  the encoder's val >> 2
inline constexpr uint32_t A6XX_HLSQ_GS_CNTL_ENABLED = 0x00000100;                           // a6xx.xml.h:10242
inline constexpr uint32_t A6XX_HLSQ_GS_CNTL_READ_IMM_SHARED_CONSTS = 0x00000200;            // a6xx.xml.h:10243
constexpr uint32_t A6XX_HLSQ_GS_CNTL_CONSTLEN(uint32_t v) {
    return field(v >> A6XX_HLSQ_GS_CNTL_CONSTLEN__SCALE, A6XX_HLSQ_GS_CNTL_CONSTLEN__SHIFT, A6XX_HLSQ_GS_CNTL_CONSTLEN__MASK);
}

// HLSQ_FS_CNTL_0 (THREADSIZE above): VARYINGS when total_in != 0 (tu_shader.cc:1737)
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_0_VARYINGS = 0x00000002;                        // a6xx.xml.h:10382
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_0_UNK2__MASK = 0x00000ffc;                      // a6xx.xml.h:10383
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_0_UNK2__SHIFT = 2;                              // a6xx.xml.h:10384
constexpr uint32_t A6XX_HLSQ_FS_CNTL_0_UNK2(uint32_t v) {
    return field(v, A6XX_HLSQ_FS_CNTL_0_UNK2__SHIFT, A6XX_HLSQ_FS_CNTL_0_UNK2__MASK);
}

// one pkt4 of 5 at HLSQ_CONTROL_1_REG (tu_shader.cc:1683-1700): PRIMALLOCTHRESHOLD = the chip's 7; then the FS's
// system value registers, kRegidNone (0xfc) for each it does not read: face/sample id/sample mask/centre rhw; the
// four pixel and centroid barycentrics; the two sample barycentrics and the fragcoord xy and zw registers;
// HLSQ_CONTROL_5_REG (above) = 0xfcfc
inline constexpr uint32_t REG_A6XX_HLSQ_CONTROL_1_REG = 0xb982;                             // a6xx.xml.h:10394
inline constexpr uint32_t A6XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD__MASK = 0x00000007;    // a6xx.xml.h:10395
inline constexpr uint32_t A6XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD__SHIFT = 0;            // a6xx.xml.h:10396
constexpr uint32_t A6XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD__SHIFT, A6XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD__MASK);
}
inline constexpr uint32_t REG_A6XX_HLSQ_CONTROL_2_REG = 0xb983;                             // a6xx.xml.h:10403
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_FACEREGID__MASK = 0x000000ff;             // a6xx.xml.h:10404
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_FACEREGID__SHIFT = 0;                     // a6xx.xml.h:10405
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_SAMPLEID__MASK = 0x0000ff00;              // a6xx.xml.h:10410
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_SAMPLEID__SHIFT = 8;                      // a6xx.xml.h:10411
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK__MASK = 0x00ff0000;            // a6xx.xml.h:10416
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK__SHIFT = 16;                   // a6xx.xml.h:10417
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_CENTERRHW__MASK = 0xff000000;             // a6xx.xml.h:10422
inline constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_CENTERRHW__SHIFT = 24;                    // a6xx.xml.h:10423
constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_FACEREGID(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_2_REG_FACEREGID__SHIFT, A6XX_HLSQ_CONTROL_2_REG_FACEREGID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_SAMPLEID(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_2_REG_SAMPLEID__SHIFT, A6XX_HLSQ_CONTROL_2_REG_SAMPLEID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK__SHIFT, A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_2_REG_CENTERRHW(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_2_REG_CENTERRHW__SHIFT, A6XX_HLSQ_CONTROL_2_REG_CENTERRHW__MASK);
}
inline constexpr uint32_t REG_A6XX_HLSQ_CONTROL_3_REG = 0xb984;                             // a6xx.xml.h:10430
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL__MASK = 0x000000ff;        // a6xx.xml.h:10431
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL__SHIFT = 0;                // a6xx.xml.h:10432
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL__MASK = 0x0000ff00;       // a6xx.xml.h:10437
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL__SHIFT = 8;               // a6xx.xml.h:10438
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID__MASK = 0x00ff0000;     // a6xx.xml.h:10443
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID__SHIFT = 16;            // a6xx.xml.h:10444
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID__MASK = 0xff000000;    // a6xx.xml.h:10449
inline constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID__SHIFT = 24;           // a6xx.xml.h:10450
constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL__SHIFT, A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL__SHIFT, A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID__SHIFT, A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID__SHIFT, A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID__MASK);
}
inline constexpr uint32_t REG_A6XX_HLSQ_CONTROL_4_REG = 0xb985;                             // a6xx.xml.h:10457
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE__MASK = 0x000000ff;       // a6xx.xml.h:10458
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE__SHIFT = 0;               // a6xx.xml.h:10459
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE__MASK = 0x0000ff00;      // a6xx.xml.h:10464
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE__SHIFT = 8;              // a6xx.xml.h:10465
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID__MASK = 0x00ff0000;          // a6xx.xml.h:10470
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID__SHIFT = 16;                 // a6xx.xml.h:10471
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID__MASK = 0xff000000;          // a6xx.xml.h:10476
inline constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID__SHIFT = 24;                 // a6xx.xml.h:10477
constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE__SHIFT, A6XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE__SHIFT, A6XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID__SHIFT, A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID__MASK);
}
constexpr uint32_t A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(uint32_t v) {
    return field(v, A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID__SHIFT, A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID__MASK);
}

inline constexpr uint32_t REG_A6XX_HLSQ_FS_CNTL = 0xbb10;                                   // a6xx.xml.h:11073
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_CONSTLEN__MASK = 0x000000ff;                    // a6xx.xml.h:11074
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_CONSTLEN__SHIFT = 0;                            // a6xx.xml.h:11075
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_CONSTLEN__SCALE = 2;                            // a6xx.xml.h:11076  the encoder's val >> 2
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_ENABLED = 0x00000100;                           // a6xx.xml.h:11081
inline constexpr uint32_t A6XX_HLSQ_FS_CNTL_READ_IMM_SHARED_CONSTS = 0x00000200;            // a6xx.xml.h:11082
constexpr uint32_t A6XX_HLSQ_FS_CNTL_CONSTLEN(uint32_t v) {
    return field(v >> A6XX_HLSQ_FS_CNTL_CONSTLEN__SCALE, A6XX_HLSQ_FS_CNTL_CONSTLEN__SHIFT, A6XX_HLSQ_FS_CNTL_CONSTLEN__MASK);
}

// CP_DRAW_INDX_OFFSET, 3 words for an auto-indexed draw (adreno_pm4.xml:906-916; the 7-word form is for DMA
// indices): word 0 = PRIM_TYPE | SOURCE_SELECT | VIS_CULL | INDEX_SIZE 0 | PATCH_TYPE 0, word 1 NUM_INSTANCES,
// word 2 NUM_INDICES. turnip's and fd6's pipelines write USE_VISIBILITY even in sysmem under
// CP_SET_VISIBILITY_OVERRIDE 1 (tu_cmd_buffer.cc:6061-6096; fd6_draw.cc:357-360); r3d writes IGNORE_VISIBILITY
// (tu_clear_blit.cc:1661-1670). The XML documents neither packet.
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__MASK = 0x0000003f;               // adreno_pm4.xml.h:886
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__SHIFT = 0;                       // adreno_pm4.xml.h:887
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__MASK = 0x000000c0;           // adreno_pm4.xml.h:892
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__SHIFT = 6;                   // adreno_pm4.xml.h:893
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_VIS_CULL__MASK = 0x00000300;                // adreno_pm4.xml.h:898
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_VIS_CULL__SHIFT = 8;                        // adreno_pm4.xml.h:899
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__MASK = 0x00000c00;              // adreno_pm4.xml.h:904
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__SHIFT = 10;                     // adreno_pm4.xml.h:905
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_PATCH_TYPE__MASK = 0x00003000;              // adreno_pm4.xml.h:910
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_PATCH_TYPE__SHIFT = 12;                     // adreno_pm4.xml.h:911
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_GS_ENABLE = 0x00010000;                     // adreno_pm4.xml.h:916
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_0_TESS_ENABLE = 0x00020000;                   // adreno_pm4.xml.h:917
constexpr uint32_t CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(pc_di_primtype v) {
    return field(v, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__SHIFT, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE__MASK);
}
constexpr uint32_t CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(pc_di_src_sel v) {
    return field(v, CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__SHIFT, CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT__MASK);
}
constexpr uint32_t CP_DRAW_INDX_OFFSET_0_VIS_CULL(pc_di_vis_cull_mode v) {
    return field(v, CP_DRAW_INDX_OFFSET_0_VIS_CULL__SHIFT, CP_DRAW_INDX_OFFSET_0_VIS_CULL__MASK);
}
constexpr uint32_t CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(a4xx_index_size v) {
    return field(v, CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__SHIFT, CP_DRAW_INDX_OFFSET_0_INDEX_SIZE__MASK);
}
constexpr uint32_t CP_DRAW_INDX_OFFSET_0_PATCH_TYPE(a6xx_patch_type v) {
    return field(v, CP_DRAW_INDX_OFFSET_0_PATCH_TYPE__SHIFT, CP_DRAW_INDX_OFFSET_0_PATCH_TYPE__MASK);
}
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_1_NUM_INSTANCES__MASK = 0xffffffff;           // adreno_pm4.xml.h:921
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_1_NUM_INSTANCES__SHIFT = 0;                   // adreno_pm4.xml.h:922
constexpr uint32_t CP_DRAW_INDX_OFFSET_1_NUM_INSTANCES(uint32_t v) {
    return field(v, CP_DRAW_INDX_OFFSET_1_NUM_INSTANCES__SHIFT, CP_DRAW_INDX_OFFSET_1_NUM_INSTANCES__MASK);
}
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_2_NUM_INDICES__MASK = 0xffffffff;             // adreno_pm4.xml.h:930
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_2_NUM_INDICES__SHIFT = 0;                     // adreno_pm4.xml.h:931
constexpr uint32_t CP_DRAW_INDX_OFFSET_2_NUM_INDICES(uint32_t v) {
    return field(v, CP_DRAW_INDX_OFFSET_2_NUM_INDICES__SHIFT, CP_DRAW_INDX_OFFSET_2_NUM_INDICES__MASK);
}
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_3_FIRST_INDX__MASK = 0xffffffff;              // adreno_pm4.xml.h:939
inline constexpr uint32_t CP_DRAW_INDX_OFFSET_3_FIRST_INDX__SHIFT = 0;                      // adreno_pm4.xml.h:940
constexpr uint32_t CP_DRAW_INDX_OFFSET_3_FIRST_INDX(uint32_t v) {
    return field(v, CP_DRAW_INDX_OFFSET_3_FIRST_INDX__SHIFT, CP_DRAW_INDX_OFFSET_3_FIRST_INDX__MASK);
}

// --- what the draw adds to the chip facts and ir3 conventions ------------------------------------------------------------

inline constexpr uint32_t kPrimAllocThreshold = 7;          // not-in-headers: freedreno_devices.py a6xx_base prim_alloc_threshold = 0x7 (HLSQ_CONTROL_1_REG)
inline constexpr uint32_t kBranchstackSize = 64;            // not-in-headers: ir3_compiler.c:150 branchstack_size for gen >= 6, the MIN2 in ir3_shader_branchstack_hw
inline constexpr uint32_t kMaxConstPipeline = 512;          // not-in-headers: ir3_compiler.c:185-188 max_const_pipeline for gen 6: the VS and FS constlens together, in vec4
inline constexpr uint32_t kVsConstlenFloor = 8;             // not-in-headers: ir3_compiler_nir.c:6035-6037, every gen >= 6 vertex shader has constlen >= 8
inline constexpr uint32_t kProgramAlign = 128;              // not-in-headers: tu_shader.cc:1313-1316 asserts binary_iova & 0x7f == 0 (one instruction group); the XML says 32 (a6xx.xml:4835, 4996)
inline constexpr uint32_t kSpXsPvtMemAddrAlign = 32;        // not-in-headers: a6xx.xml SP_VS/SP_FS_PVT_MEM_ADDR align="32"; tu_shader.cc:1317 asserts it
inline constexpr uint32_t kMrtBaseAlign = 64;               // not-in-headers: fd6_layout.c:181 base_align of a linear image; the XML gives RB_MRT_BASE align 1 (a6xx.xml:3714-3715)
inline constexpr uint32_t kMrtPitchAlign = 64;              // not-in-headers: a6xx.xml.h:5816 A6XX_RB_MRT_PITCH asserts !(val & 0x3f) before its >> 6
inline constexpr uint32_t kMrtPitchMax = 0xffff << 6;       // not-in-headers: a6xx.xml:3702 RB_MRT_PITCH shr=6 high=15: 65535 units of 64 bytes
inline constexpr uint32_t kLocNone = 0xff;                  // not-in-headers: the "no location" every VPC/PC *LOC field takes (tu_pipeline.cc:994-1007, 1042-1049)
inline constexpr uint32_t kMaxGuardband = 0x1ff;            // not-in-headers: freedreno_guardband.h:14 MAX_GB, all ones of the 9-bit HORZ/VERT fields
inline constexpr uint32_t kVpcVaryingRegs = 8;              // not-in-headers: a6xx.xml VPC_VARYING_INTERP and VPC_VARYING_PS_REPL length="8" (fd6_program.cc:1365-1370 writes all)
inline constexpr uint32_t kVpcVarDisableRegs = 4;           // not-in-headers: a6xx.xml VPC_VAR length="4": 128 components of varyings (tu_shader.cc:1791-1808)
inline constexpr uint32_t kMrtCount = 8;                    // not-in-headers: a6xx.xml RB_MRT length="8"; SP_FS_OUTPUT_REG and SP_FS_MRT_REG have 8 too
inline constexpr uint32_t kViewportWords = 6;               // not-in-headers: REG_A6XX_GRAS_CL_VPORT__STRIDE: XOFFSET, XSCALE, YOFFSET, YSCALE, ZOFFSET, ZSCALE

// ir3's barycentric kinds: the index into an FS variant's ij_regid[] (render.hpp's Stage::ijRegid), ir3_shader.h:122-130
// enum ir3_bary. The order is NOT the field order of HLSQ_CONTROL_3/4_REG or the bit order of GRAS_CNTL (ijBits below).
enum ir3_bary : uint32_t {
    IJ_PERSP_PIXEL = 0,            // not-in-headers: ir3_shader.h:123
    IJ_PERSP_SAMPLE = 1,           // not-in-headers: ir3_shader.h:124
    IJ_PERSP_CENTROID = 2,         // not-in-headers: ir3_shader.h:125
    IJ_PERSP_CENTER_RHW = 3,       // not-in-headers: ir3_shader.h:126
    IJ_LINEAR_PIXEL = 4,           // not-in-headers: ir3_shader.h:127
    IJ_LINEAR_CENTROID = 5,        // not-in-headers: ir3_shader.h:128
    IJ_LINEAR_SAMPLE = 6,          // not-in-headers: ir3_shader.h:129
    IJ_COUNT = 7,                  // not-in-headers: ir3_shader.h:130
};

// ir3's VALIDREG: a regid the shader reads (CONDREG(r, bit) is validReg(r) ? bit : 0)
constexpr bool validReg(uint32_t regid) { return regid != kRegidNone; }

// --- the layouts two registers share, so that one encoder serves both -------------------------------------------------------

static_assert(A6XX_RB_BIN_CONTROL_BINW__MASK == A6XX_GRAS_BIN_CONTROL_BINW__MASK && A6XX_RB_BIN_CONTROL_BINW__SCALE == A6XX_GRAS_BIN_CONTROL_BINW__SCALE &&
                  A6XX_RB_BIN_CONTROL_BINH__MASK == A6XX_GRAS_BIN_CONTROL_BINH__MASK && A6XX_RB_BIN_CONTROL_BINH__SCALE == A6XX_GRAS_BIN_CONTROL_BINH__SCALE &&
                  A6XX_RB_BIN_CONTROL_RENDER_MODE__MASK == A6XX_GRAS_BIN_CONTROL_RENDER_MODE__MASK &&
                  A6XX_RB_BIN_CONTROL_FORCE_LRZ_WRITE_DIS == A6XX_GRAS_BIN_CONTROL_FORCE_LRZ_WRITE_DIS &&
                  A6XX_RB_BIN_CONTROL_BUFFERS_LOCATION__MASK == A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION__MASK &&
                  A6XX_RB_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__MASK == A6XX_GRAS_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK__MASK,
              "GRAS_BIN_CONTROL and RB_BIN_CONTROL have the same layout: binControl() writes both");
static_assert(A6XX_SP_FS_CONFIG_ENABLED == A6XX_SP_VS_CONFIG_ENABLED && A6XX_SP_FS_CONFIG_NTEX__MASK == A6XX_SP_VS_CONFIG_NTEX__MASK &&
                  A6XX_SP_FS_CONFIG_NSAMP__MASK == A6XX_SP_VS_CONFIG_NSAMP__MASK && A6XX_SP_FS_CONFIG_NIBO__MASK == A6XX_SP_VS_CONFIG_NIBO__MASK &&
                  A6XX_SP_HS_CONFIG_NIBO__MASK == A6XX_SP_VS_CONFIG_NIBO__MASK && A6XX_SP_DS_CONFIG_NIBO__MASK == A6XX_SP_VS_CONFIG_NIBO__MASK &&
                  A6XX_SP_GS_CONFIG_NIBO__MASK == A6XX_SP_VS_CONFIG_NIBO__MASK && A6XX_SP_CS_CONFIG_NIBO__MASK == A6XX_SP_VS_CONFIG_NIBO__MASK,
              "the five SP_xS_CONFIG registers have the same layout: spXsConfig() writes any of them");
static_assert(A6XX_HLSQ_FS_CNTL_CONSTLEN__MASK == A6XX_HLSQ_VS_CNTL_CONSTLEN__MASK && A6XX_HLSQ_FS_CNTL_CONSTLEN__SCALE == A6XX_HLSQ_VS_CNTL_CONSTLEN__SCALE &&
                  A6XX_HLSQ_FS_CNTL_ENABLED == A6XX_HLSQ_VS_CNTL_ENABLED && A6XX_HLSQ_CS_CNTL_CONSTLEN__SCALE == A6XX_HLSQ_VS_CNTL_CONSTLEN__SCALE,
              "HLSQ_VS_CNTL, HLSQ_FS_CNTL and HLSQ_CS_CNTL have the same layout");
static_assert(A6XX_RB_RENDER_CONTROL0_IJ_PERSP_PIXEL == A6XX_GRAS_CNTL_IJ_PERSP_PIXEL && A6XX_RB_RENDER_CONTROL0_IJ_PERSP_CENTROID == A6XX_GRAS_CNTL_IJ_PERSP_CENTROID &&
                  A6XX_RB_RENDER_CONTROL0_IJ_PERSP_SAMPLE == A6XX_GRAS_CNTL_IJ_PERSP_SAMPLE && A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL == A6XX_GRAS_CNTL_IJ_LINEAR_PIXEL &&
                  A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_CENTROID == A6XX_GRAS_CNTL_IJ_LINEAR_CENTROID && A6XX_RB_RENDER_CONTROL0_IJ_LINEAR_SAMPLE == A6XX_GRAS_CNTL_IJ_LINEAR_SAMPLE &&
                  A6XX_RB_RENDER_CONTROL0_COORD_MASK__MASK == A6XX_GRAS_CNTL_COORD_MASK__MASK,
              "GRAS_CNTL's IJ bits and COORD_MASK sit where RB_RENDER_CONTROL0's do: one ijBits() value serves both");
static_assert(A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_MASK__MASK == A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK__MASK &&
                  A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_03_LOC__MASK == A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC__MASK &&
                  A6XX_VPC_VS_CLIP_CNTL_V2_CLIP_DIST_47_LOC__MASK == A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC__MASK &&
                  A6XX_VPC_VS_LAYER_CNTL_V2_LAYERLOC__MASK == A6XX_VPC_VS_LAYER_CNTL_LAYERLOC__MASK &&
                  A6XX_VPC_VS_LAYER_CNTL_V2_VIEWLOC__MASK == A6XX_VPC_VS_LAYER_CNTL_VIEWLOC__MASK &&
                  A6XX_VPC_VS_LAYER_CNTL_V2_SHADINGRATELOC__MASK == A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC__MASK,
              "the _V2 copies of VPC_VS_CLIP_CNTL and VPC_VS_LAYER_CNTL take the same word");
static_assert(A6XX_RB_FS_OUTPUT_CNTL1_MRT__MASK == A6XX_SP_FS_OUTPUT_CNTL1_MRT__MASK && A6XX_RB_RENDER_COMPONENTS_RT0__MASK == A6XX_SP_FS_RENDER_COMPONENTS_RT0__MASK &&
                  A6XX_RB_RENDER_COMPONENTS_RT7__MASK == A6XX_SP_FS_RENDER_COMPONENTS_RT7__MASK,
              "the SP and RB copies of FS_OUTPUT_CNTL1 and RENDER_COMPONENTS take the same word");
static_assert(A6XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT__MASK == A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT__MASK &&
                  A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT__MASK == A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT__MASK &&
                  A6XX_SP_FS_CTRL_REG0_BRANCHSTACK__MASK == A6XX_SP_VS_CTRL_REG0_BRANCHSTACK__MASK &&
                  A6XX_SP_FS_CTRL_REG0_MERGEDREGS != A6XX_SP_VS_CTRL_REG0_MERGEDREGS,
              "the footprints and branch stack sit alike in SP_VS_CTRL_REG0 and SP_FS_CTRL_REG0; MERGEDREGS does not (bit 20 vs 31)");
static_assert(A6XX_RB_WINDOW_OFFSET_X__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__MASK && A6XX_RB_WINDOW_OFFSET_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK &&
                  A6XX_RB_WINDOW_OFFSET2_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK && A6XX_SP_WINDOW_OFFSET_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK &&
                  A6XX_SP_TP_WINDOW_OFFSET_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK && A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK &&
                  A6XX_GRAS_2D_RESOLVE_CNTL_1_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK && A6XX_GRAS_2D_RESOLVE_CNTL_2_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK,
              "a6xx_reg_xy: the window scissor, the four window offsets and the 2D resolve registers share the 14-bit X/Y form (regXY)");
static_assert(A6XX_GRAS_SC_SCREEN_SCISSOR_BR_Y__MASK == A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__MASK && A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL_Y__MASK == A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__MASK &&
                  A6XX_GRAS_SC_VIEWPORT_SCISSOR_BR_Y__MASK == A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y__MASK && A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X__MASK == 0xffff,
              "a6xx_scissor_xy: the screen and viewport scissors share the 16-bit X/Y form (scissorXY)");
static_assert(A6XX_RB_RAS_MSAA_CNTL_SAMPLES__MASK == A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES__MASK && A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES__MASK == A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES__MASK &&
                  A6XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE == A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE && A6XX_SP_TP_DEST_MSAA_CNTL_MSAA_DISABLE == A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE,
              "the GRAS, RB and SP_TP MSAA registers take the same words");
static_assert(A6XX_RB_MRT_PITCH__SCALE == A6XX_RB_MRT_ARRAY_PITCH__SCALE && A6XX_RB_DEPTH_BUFFER_PITCH__SCALE == A6XX_RB_MRT_PITCH__SCALE &&
                  (1u << A6XX_RB_MRT_PITCH__SCALE) == kMrtPitchAlign,
              "the colour and depth pitches are held in 64-byte units");

// --- the registers of array entries, as vfdFetchSize(i) above --------------------------------------------------------------
// Each takes the entry-0 register of the field wanted (REG_A6XX_RB_MRT_PITCH, say) and adds the array's stride i times.

constexpr uint32_t regRbMrt(uint32_t reg0, uint32_t i) { return reg0 + REG_A6XX_RB_MRT__STRIDE * i; }
constexpr uint32_t regRbMrtFlagBuffer(uint32_t reg0, uint32_t i) { return reg0 + REG_A6XX_RB_MRT_FLAG_BUFFER__STRIDE * i; }
constexpr uint32_t regGrasClVport(uint32_t reg0, uint32_t i) { return reg0 + REG_A6XX_GRAS_CL_VPORT__STRIDE * i; }
constexpr uint32_t regGrasClZClamp(uint32_t reg0, uint32_t i) { return reg0 + REG_A6XX_GRAS_CL_Z_CLAMP__STRIDE * i; }
constexpr uint32_t regGrasScScreenScissor(uint32_t reg0, uint32_t i) { return reg0 + REG_A6XX_GRAS_SC_SCREEN_SCISSOR__STRIDE * i; }
constexpr uint32_t regGrasScViewportScissor(uint32_t reg0, uint32_t i) { return reg0 + REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR__STRIDE * i; }
constexpr uint32_t regSpVsOutReg(uint32_t i) { return REG_A6XX_SP_VS_OUT_REG + REG_A6XX_SP_VS_OUT__STRIDE * i; }
constexpr uint32_t regSpVsVpcDstReg(uint32_t i) { return REG_A6XX_SP_VS_VPC_DST_REG + REG_A6XX_SP_VS_VPC_DST__STRIDE * i; }
constexpr uint32_t regSpFsOutputReg(uint32_t i) { return REG_A6XX_SP_FS_OUTPUT_REG + REG_A6XX_SP_FS_OUTPUT__STRIDE * i; }
constexpr uint32_t regSpFsMrtReg(uint32_t i) { return REG_A6XX_SP_FS_MRT_REG + REG_A6XX_SP_FS_MRT__STRIDE * i; }
constexpr uint32_t regVpcVaryingInterpMode(uint32_t i) { return REG_A6XX_VPC_VARYING_INTERP_MODE + REG_A6XX_VPC_VARYING_INTERP__STRIDE * i; }
constexpr uint32_t regVpcVaryingPsReplMode(uint32_t i) { return REG_A6XX_VPC_VARYING_PS_REPL_MODE + REG_A6XX_VPC_VARYING_PS_REPL__STRIDE * i; }
constexpr uint32_t regVpcVarDisable(uint32_t i) { return REG_A6XX_VPC_VAR_DISABLE + REG_A6XX_VPC_VAR__STRIDE * i; }
constexpr uint32_t regVfdFetchBase(uint32_t i) { return REG_A6XX_VFD_FETCH_BASE + REG_A6XX_VFD_FETCH__STRIDE * i; }
constexpr uint32_t regVfdFetchStride(uint32_t i) { return REG_A6XX_VFD_FETCH_STRIDE + REG_A6XX_VFD_FETCH__STRIDE * i; }
constexpr uint32_t regVfdDecodeInstr(uint32_t i) { return REG_A6XX_VFD_DECODE_INSTR + REG_A6XX_VFD_DECODE__STRIDE * i; }
constexpr uint32_t regVfdDecodeStepRate(uint32_t i) { return REG_A6XX_VFD_DECODE_STEP_RATE + REG_A6XX_VFD_DECODE__STRIDE * i; }
constexpr uint32_t regVfdDestCntlInstr(uint32_t i) { return REG_A6XX_VFD_DEST_CNTL_INSTR + REG_A6XX_VFD_DEST_CNTL__STRIDE * i; }

// --- floats in registers -------------------------------------------------------------------------------------------------------

// The bits of a float, as Mesa's fui() (util/u_math.h) gives them to the encoders of the GRAS_CL_VPORT_*, Z_CLAMP,
// Z_BOUNDS, POLY_OFFSET and BLEND_*_F32 registers. Written out by hand because C++17 has no constexpr bit_cast: the
// exponent is found by halving or doubling (both exact), the mantissa is what is left below the leading one, scaled
// by 2^23 (exact: a float has 24 significant bits). A finite value round-trips exactly (tests/a6xx_test.cpp holds it
// to memcpy); NaN gives the quiet NaN, an infinity 0x7f800000 with its sign.
constexpr uint32_t fui(float f) {
    uint32_t sign = __builtin_signbit(f) ? 0x80000000u : 0u;
    if (f != f) {
        return 0x7fc00000u;
    }
    float a = f < 0 ? -f : f;
    if (a == 0.0f) {
        return sign;
    }
    if (a > 3.4028234663852886e38f) {
        return sign | 0x7f800000u;
    }
    int e = 127;
    while (a >= 2.0f) {
        a *= 0.5f;
        e++;
    }
    while (a < 1.0f && e > 1) {
        a *= 2.0f;
        e--;
    }
    if (a < 1.0f) {
        return sign | uint32_t(a * 8388608.0f);   // a denormal: exponent 0, the value is mantissa * 2^-149 = a * 2^-126
    }
    return sign | (uint32_t(e) << 23) | uint32_t((a - 1.0f) * 8388608.0f);
}

// GRAS_CL_VPORT[0]'s six words for a Metal viewport, as turnip's tu6_emit_viewport computes them (tu_pipeline.cc:
// 2442-2461: scale = extent / 2, offset = origin + scale, in float) with ONE change: the y scale is negated. Metal's
// MTLViewport has a top-left origin and its NDC y points up, so NDC y = +1 must land on row y (the top) and -1 on row
// y + height: y_window = (y + h/2) - (h/2) * y_ndc, where Vulkan's +h/2 puts NDC -1 at the top. air_run.c sets
// softpipe's viewport for Metal the same way (scale {w/2, -h/2, 1}, translate {w/2, h/2, 0}). This is reasoning from
// the convention, not a measurement: the probe's orientation line (which half of the target the first triangle fills)
// is what decides it. Depth is [0, 1] in Metal as in Vulkan: zscale = zfar - znear, zoffset = znear.
struct ViewportWords {
    uint32_t xoffset, xscale, yoffset, yscale, zoffset, zscale;
};
constexpr ViewportWords viewportWords(double x, double y, double width, double height, double znear, double zfar) {
    float xs = float(width) / 2.0f;
    float ys = float(height) / 2.0f;
    return ViewportWords{fui(float(x) + xs), fui(xs), fui(float(y) + ys), fui(-ys), fui(float(znear)), fui(float(zfar) - float(znear))};
}

// fd_calc_guardband (freedreno_guardband.h:17-75) for a4xx and later (the viewport may reach +-32768): the largest NDC
// extent [-gb, gb] whose window coordinates stay in range, as 3.6 floating point (the exponent minus one in the high
// three bits, the mantissa below its leading one in the low six), rounded down; MAX_GB when the viewport is not inside
// the guardband (gb_adj < 1), when the scale is 0, or when gb_adj needs more than 8 bits of exponent. For a 64x64
// viewport (offset 32, scale 32) gb_adj = min(1025, 1022.97) and frexp gives exponent 10: MAX_GB.
constexpr uint32_t calcGuardband(float offset, float scale) {
    float as = scale < 0 ? -scale : scale;
    if (as == 0.0f) {
        return kMaxGuardband;   // Mesa: the division gives an infinity, isinf(gb_adj) returns MAX_GB
    }
    float gbMinNdc = (-32768.0f - offset) / as;
    float gbMaxNdc = (32767.0f - offset) / as;
    float gbAdj = -gbMinNdc < gbMaxNdc ? -gbMinNdc : gbMaxNdc;
    if (!(gbAdj >= 1.0f)) {
        return kMaxGuardband;   // the viewport is not inside the guardband (or gbAdj is NaN)
    }
    // frexpf: gbAdj = m * 2^e with m in [0.5, 1); the halvings are exact
    int e = 0;
    float m = gbAdj;
    while (m >= 1.0f) {
        m *= 0.5f;
        e++;
    }
    if (e > 8) {
        return kMaxGuardband;
    }
    return (uint32_t(e - 1) << 6) | (uint32_t(m * 128.0f) - 64u);
}

// GRAS_CL_GUARDBAND_CLIP_ADJ for one viewport: each axis's guardband, capped at 511 as turnip caps it (tu_pipeline.cc:
// 2436, 2458-2461, 2515-2517)
constexpr uint32_t guardbandClipAdj(double xoffset, double xscale, double yoffset, double yscale) {
    uint32_t h = calcGuardband(float(xoffset), float(xscale));
    uint32_t v = calcGuardband(float(yoffset), float(yscale));
    return A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_HORZ(h < 511 ? h : 511) | A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ_VERT(v < 511 ? v : 511);
}

// --- whole register values, as one draw computes them -------------------------------------------------------------------------
// From the turnip emitters named at each register above, with ir3's variant info (render.hpp's Stage) as the input:
// register counts, regids (kRegidNone when unread), vec4 constlens, VPC locations, pixel units. Nothing here has run
// on the phone.

// a6xx_reg_xy, 14-bit X and Y: the window scissor TL/BR, the four window offsets, GRAS_2D_RESOLVE_CNTL_1/2
constexpr uint32_t regXY(uint32_t x, uint32_t y) { return A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X(x) | A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y(y); }
// a6xx_scissor_xy, 16-bit X and Y: the screen and viewport scissors' TL/BR (BR inclusive: w - 1, h - 1)
constexpr uint32_t scissorXY(uint32_t x, uint32_t y) { return A6XX_GRAS_SC_SCREEN_SCISSOR_TL_X(x) | A6XX_GRAS_SC_SCREEN_SCISSOR_TL_Y(y); }

// GRAS_BIN_CONTROL and RB_BIN_CONTROL (same layout): the bin size in pixels (the fields count 32 and 16 of them),
// the pass kind, where the buffers are, the LRZ feedback mask
constexpr uint32_t binControl(a6xx_render_mode mode, a6xx_buffers_location where, a6xx_lrz_feedback_mask lrz, uint32_t binw = 0,
                              uint32_t binh = 0) {
    return A6XX_GRAS_BIN_CONTROL_BINW(binw) | A6XX_GRAS_BIN_CONTROL_BINH(binh) | A6XX_GRAS_BIN_CONTROL_RENDER_MODE(mode) |
           A6XX_GRAS_BIN_CONTROL_BUFFERS_LOCATION(where) | A6XX_GRAS_BIN_CONTROL_LRZ_FEEDBACK_ZMODE_MASK(lrz);
}
// ...for a sysmem pass: no bins, RENDERING_PASS, BUFFERS_IN_SYSMEM; fd6 and r3d stop there (0x00c00000), turnip adds
// EARLY_Z_OR_EARLY_LRZ_LATE_Z on a chip with lrz feedback (0x03c00000): render::Options::binControlLrzFeedback
constexpr uint32_t binControlSysmem(bool lrzFeedback) {
    return binControl(RENDERING_PASS, BUFFERS_IN_SYSMEM, lrzFeedback ? LRZ_FEEDBACK_EARLY_Z_OR_EARLY_LRZ_LATE_Z : LRZ_FEEDBACK_NONE);
}

// ...for a GMEM tile: the bin's size, BUFFERS_IN_GMEM; with lrz feedback turnip's EARLY_LRZ_LATE_Z (no binning pass
// here), without it FORCE_LRZ_WRITE_DIS (tu_cmd_buffer.cc:1066-1077)
constexpr uint32_t binControlGmem(bool lrzFeedback, uint32_t binw, uint32_t binh) {
    return binControl(RENDERING_PASS, BUFFERS_IN_GMEM, lrzFeedback ? LRZ_FEEDBACK_EARLY_LRZ_LATE_Z : LRZ_FEEDBACK_NONE, binw, binh) |
           (lrzFeedback ? 0u : A6XX_GRAS_BIN_CONTROL_FORCE_LRZ_WRITE_DIS);
}
// RB_CCU_CNTL for tiled rendering: the colour cache shrunk to its GMEM fraction and moved to GMEM's end, so the tiles
// own everything below it (tu_cmd_buffer.cc tu6_emit_ccu_cntl, gmem: ccu_offset_gmem = gmem - num_ccu * color cache /
// fraction); depth cache unused (no depth attachment in a GMEM pass here). 0x3c400000 on this chip
constexpr uint32_t kGmemCcuColorOffset = kGmemBytes - kNumCcu * kSysmemPerCcuColorCacheSize / kGmemCcuColorCacheFraction;
constexpr uint32_t rbCcuCntlGmem() {
    return (kHasGmemFastClear ? 0u : A6XX_RB_CCU_CNTL_GMEM_FAST_CLEAR_DISABLE) | A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI(kGmemCcuColorOffset >> 21) |
           A6XX_RB_CCU_CNTL_COLOR_CACHE_SIZE(CCU_CACHE_SIZE_QUARTER) | A6XX_RB_CCU_CNTL_COLOR_OFFSET(kGmemCcuColorOffset & 0x1fffff);
}
static_assert(rbCcuCntlGmem() == 0x3c400000, "the GMEM CCU layout turnip computes for this chip");

// hardware binning (fd6_gmem.cc emit_binning_pass, update_vsc_pipe, fd6_emit_tile_prep): the binning pass runs every draw's
// vertex stage once over the whole window and the VSC writes, for each pipe of bins, which draws and primitives reach
// each bin; a tile then draws only those (CP_SET_BIN_DATA5, CP_SET_VISIBILITY_OVERRIDE 0, the draws USE_VISIBILITY)
inline constexpr uint32_t kNumVscPipes = 32;                      // not-in-headers: freedreno_devices.py A635 num_vsc_pipes
inline constexpr uint32_t REG_A6XX_VSC_BIN_SIZE = 0x0c02;                                    // a6xx.xml.h:4147  WIDTH >> 5 at 0, HEIGHT >> 4 at 8
inline constexpr uint32_t REG_A6XX_VSC_DRAW_STRM_SIZE_ADDRESS = 0x0c03;                      // a6xx.xml.h:4164  64-bit: each pipe's draw-stream size written back
inline constexpr uint32_t REG_A6XX_VSC_BIN_COUNT = 0x0c06;                                   // a6xx.xml.h:4167  NX at 1, NY at 11
inline constexpr uint32_t REG_A6XX_VSC_PIPE_CONFIG_REG = 0x0c10;                             // a6xx.xml.h:4184  32 of them: X 0-9, Y 10-19, W 20-25, H 26-31 (bins)
inline constexpr uint32_t REG_A6XX_VSC_PRIM_STRM_ADDRESS = 0x0c30;                           // a6xx.xml.h:4211  64-bit, then PITCH 0x0c32, LIMIT 0x0c33
inline constexpr uint32_t REG_A6XX_VSC_PRIM_STRM_PITCH = 0x0c32;
inline constexpr uint32_t REG_A6XX_VSC_DRAW_STRM_ADDRESS = 0x0c34;                           // a6xx.xml.h:4220  64-bit, then PITCH 0x0c36, LIMIT 0x0c37
inline constexpr uint32_t REG_A6XX_VSC_DRAW_STRM_PITCH = 0x0c36;
constexpr uint32_t vscBinSize(uint32_t w, uint32_t h) { return (w >> 5) | (h >> 4) << 8; }
constexpr uint32_t vscBinCount(uint32_t nx, uint32_t ny) { return nx << 1 | ny << 11; }
constexpr uint32_t vscPipeConfig(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { return x | y << 10 | w << 20 | h << 26; }
constexpr uint32_t cpSetBinData5_0(uint32_t vscSize, uint32_t n) { return vscSize << 16 | n << 22; }   // a6xx: VSC_SIZE 16-21, VSC_N 22-26

// RB_MRT_BUF_INFO[i]: fd6_view.c:408-413 (no UNK10; A6XX has no LOSSLESSCOMPEN)
constexpr uint32_t rbMrtBufInfo(a6xx_format fmt, a6xx_tile_mode tile, a3xx_color_swap swap) {
    return A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT(fmt) | A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(tile) | A6XX_RB_MRT_BUF_INFO_COLOR_SWAP(swap);
}
// RB_MRT_PITCH[i] and RB_MRT_ARRAY_PITCH[i] in bytes, held in 64-byte units. Precondition: a multiple of 64 (Mesa's
// encoders assert it, a6xx.xml.h:5816, 5826); rbMrtPitchValid says whether a pitch is one and fits the 16-bit field
// (a6xx.xml:3702 high=15), for the refusal render.cpp makes before encoding. The low 6 bits are dropped, as the >> 6 drops them.
constexpr bool rbMrtPitchValid(uint32_t bytes) { return bytes != 0 && bytes % kMrtPitchAlign == 0 && bytes <= kMrtPitchMax; }
constexpr uint32_t rbMrtPitch(uint32_t bytes) { return A6XX_RB_MRT_PITCH(bytes); }
constexpr uint32_t rbMrtArrayPitch(uint32_t bytes) { return A6XX_RB_MRT_ARRAY_PITCH(bytes); }
// SP_FS_MRT_REG[i]: fd6_view.c:415-418; SINT/UINT for the pure integer formats (R32Uint, RGBA32Uint)
constexpr uint32_t spFsMrtReg(a6xx_format fmt, bool sint, bool uint_) {
    return A6XX_SP_FS_MRT_REG_COLOR_FORMAT(fmt) | (sint ? A6XX_SP_FS_MRT_REG_COLOR_SINT : 0u) | (uint_ ? A6XX_SP_FS_MRT_REG_COLOR_UINT : 0u);
}
// RB_MRT_CONTROL[i] with blending and the ROP off: the write mask alone, ROP_CODE left at ROP_CLEAR (0) as turnip's
// default logic op leaves it (tu_pipeline.cc:2983, 3044-3049); 0x780 for all four components
constexpr uint32_t rbMrtControl(uint32_t componentMask) { return A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE(componentMask); }
// RB_BLEND_CNTL with no target blending: SAMPLE_MASK all ones; INDEPENDENT_BLEND as turnip (tu_pipeline.cc:3016-3022)
constexpr uint32_t rbBlendCntl(bool independentBlend, uint32_t sampleMask = 0xffff) {
    return (independentBlend ? A6XX_RB_BLEND_CNTL_INDEPENDENT_BLEND : 0u) | A6XX_RB_BLEND_CNTL_SAMPLE_MASK(sampleMask);
}
// SP_FS_RENDER_COMPONENTS and RB_RENDER_COMPONENTS: 0xf per colour output written (tu_shader.cc:1873-1874, 1892-1893)
constexpr uint32_t renderComponents(uint32_t numColorOutputs) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < numColorOutputs && i < kMrtCount; i++) {
        v |= A6XX_RB_RENDER_COMPONENTS_RT0(0xf) << (i * 4);
    }
    return v;
}
// GRAS_SC_CNTL for a sysmem pass (tu_pipeline.cc:3351-3380) and RB_RENDER_CNTL for one (tu_cmd_buffer.cc:633-693)
constexpr uint32_t grasScCntlSysmem() { return A6XX_GRAS_SC_CNTL_CCUSINGLECACHELINESIZE(2) | A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE(NO_FLUSH); }
constexpr uint32_t rbRenderCntlSysmem() { return A6XX_RB_RENDER_CNTL_CCUSINGLECACHELINESIZE(2); }

// CP_DRAW_INDX_OFFSET word 0 for a non-indexed draw: no index size, no patches, no GS or tessellation
constexpr uint32_t drawIndxOffset0(pc_di_primtype prim, pc_di_src_sel src, pc_di_vis_cull_mode vis) {
    return CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(prim) | CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(src) | CP_DRAW_INDX_OFFSET_0_VIS_CULL(vis);
}

// ir3_shader_branchstack_hw (ir3_shader.h:1419-1428): DIV_ROUND_UP(MIN2(branchstack, 64), 2)
constexpr uint32_t branchstackHw(uint32_t branchstack) { return ((branchstack < kBranchstackSize ? branchstack : kBranchstackSize) + 1) / 2; }
// SP_VS_CTRL_REG0 (tu_shader.cc:1255-1261): the footprints are max_reg + 1 and max_half_reg + 1 (0 when the shader
// uses no half register), BRANCHSTACK is branchstackHw's, MERGEDREGS is this register's bit 20, no THREADSIZE
constexpr uint32_t spVsCtrlReg0(uint32_t fullRegs, uint32_t halfRegs, uint32_t branchstack, bool mergedregs = true, bool earlyPreamble = false) {
    return A6XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(halfRegs) | A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(fullRegs) |
           A6XX_SP_VS_CTRL_REG0_BRANCHSTACK(branchstack) | (mergedregs ? A6XX_SP_VS_CTRL_REG0_MERGEDREGS : 0u) |
           (earlyPreamble ? A6XX_SP_VS_CTRL_REG0_EARLYPREAMBLE : 0u);
}
// SP_FS_CTRL_REG0 (tu_shader.cc:1284-1296): the footprints, THREADSIZE, VARYING when the FS has inputs, LODPIXMASK
// when it needs full quads, PIXLODENABLE when it needs the pixel lod, UNK24 ALWAYS (both witnesses), MERGEDREGS bit 31
constexpr uint32_t spFsCtrlReg0(uint32_t fullRegs, uint32_t halfRegs, uint32_t branchstack, a6xx_threadsize threadsize, bool varying,
                                bool lodpixmask, bool pixlodenable, bool earlyPreamble = false, bool mergedregs = true) {
    return A6XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(halfRegs) | A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(fullRegs) |
           A6XX_SP_FS_CTRL_REG0_BRANCHSTACK(branchstack) | A6XX_SP_FS_CTRL_REG0_THREADSIZE(threadsize) |
           (varying ? A6XX_SP_FS_CTRL_REG0_VARYING : 0u) | (lodpixmask ? A6XX_SP_FS_CTRL_REG0_LODPIXMASK : 0u) | A6XX_SP_FS_CTRL_REG0_UNK24 |
           (pixlodenable ? A6XX_SP_FS_CTRL_REG0_PIXLODENABLE : 0u) | (earlyPreamble ? A6XX_SP_FS_CTRL_REG0_EARLYPREAMBLE : 0u) |
           (mergedregs ? A6XX_SP_FS_CTRL_REG0_MERGEDREGS : 0u);
}
// HLSQ_VS_CNTL and HLSQ_FS_CNTL from the constant length in vec4, rounded up to the 4 the field counts in, enabled:
// as hlsqCsCntl does (ir3 gives a VS at least 8, kVsConstlenFloor; an FS may have 0 -> 0x100)
constexpr uint32_t hlsqVsCntl(uint32_t constlenVec4) { return A6XX_HLSQ_VS_CNTL_CONSTLEN((constlenVec4 + 3) & ~3u) | A6XX_HLSQ_VS_CNTL_ENABLED; }
constexpr uint32_t hlsqFsCntl(uint32_t constlenVec4) { return A6XX_HLSQ_FS_CNTL_CONSTLEN((constlenVec4 + 3) & ~3u) | A6XX_HLSQ_FS_CNTL_ENABLED; }
// SP_xS_CONFIG for any of the five stages: ENABLED | NTEX | NSAMP | NIBO (turnip writes NTEX = NSAMP = num_samp and
// no NIBO, tu_pipeline.cc:384-398; fd6 writes num_ibos; 0 IBOs in step one makes them agree)
constexpr uint32_t spXsConfig(uint32_t ntex, uint32_t nsamp, uint32_t nibo) {
    return A6XX_SP_VS_CONFIG_ENABLED | A6XX_SP_VS_CONFIG_NTEX(ntex) | A6XX_SP_VS_CONFIG_NSAMP(nsamp) | A6XX_SP_VS_CONFIG_NIBO(nibo);
}
// HLSQ_FS_CNTL_0: THREADSIZE | VARYINGS when total_in != 0 (tu_shader.cc:1737)
constexpr uint32_t hlsqFsCntl0(a6xx_threadsize threadsize, bool varyings) {
    return A6XX_HLSQ_FS_CNTL_0_THREADSIZE(threadsize) | (varyings ? A6XX_HLSQ_FS_CNTL_0_VARYINGS : 0u);
}

// VFD_CONTROL_1: the VS's id registers (tu_shader.cc:1947-1966), kRegidNone for each it does not read
constexpr uint32_t vfdControl1(uint32_t vtx, uint32_t inst, uint32_t primid, uint32_t viewid) {
    return A6XX_VFD_CONTROL_1_REGID4VTX(vtx) | A6XX_VFD_CONTROL_1_REGID4INST(inst) | A6XX_VFD_CONTROL_1_REGID4PRIMID(primid) |
           A6XX_VFD_CONTROL_1_REGID4VIEWID(viewid);
}
// VFD_CONTROL_2..5 with no HS, DS or GS: every id register kRegidNone (tu_shader.cc:1972-2036 with NULL shaders);
// VFD_CONTROL_4 is the literal 0xfc turnip writes (:2009) and VFD_CONTROL_5's UNK8 is 0xfc too
constexpr uint32_t vfdControl2None() { return A6XX_VFD_CONTROL_2_REGID_HSRELPATCHID(kRegidNone) | A6XX_VFD_CONTROL_2_REGID_INVOCATIONID(kRegidNone); }
constexpr uint32_t vfdControl3None() {
    return A6XX_VFD_CONTROL_3_REGID_DSPRIMID(kRegidNone) | A6XX_VFD_CONTROL_3_REGID_DSRELPATCHID(kRegidNone) |
           A6XX_VFD_CONTROL_3_REGID_TESSX(kRegidNone) | A6XX_VFD_CONTROL_3_REGID_TESSY(kRegidNone);
}
constexpr uint32_t vfdControl4None() { return A6XX_VFD_CONTROL_4_UNK0(kRegidNone); }
constexpr uint32_t vfdControl5None() { return A6XX_VFD_CONTROL_5_REGID_GSHEADER(kRegidNone) | A6XX_VFD_CONTROL_5_UNK8(kRegidNone); }
// VFD_DECODE_INSTR[i] for attribute i (tu_pipeline.cc:2330-2360): the binding, its byte offset, the fetch format from
// fd6_format_table's vertex column, UNK30 always, FLOAT unless the format is a pure integer one. Step two's.
constexpr uint32_t vfdDecodeInstr(uint32_t binding, uint32_t offset, bool instanced, a6xx_format fmt, a3xx_color_swap swap, bool isFloat) {
    return A6XX_VFD_DECODE_INSTR_IDX(binding) | A6XX_VFD_DECODE_INSTR_OFFSET(offset) | (instanced ? A6XX_VFD_DECODE_INSTR_INSTANCED : 0u) |
           A6XX_VFD_DECODE_INSTR_FORMAT(fmt) | A6XX_VFD_DECODE_INSTR_SWAP(swap) | A6XX_VFD_DECODE_INSTR_UNK30 |
           (isFloat ? A6XX_VFD_DECODE_INSTR_FLOAT : 0u);
}
constexpr uint32_t vfdDestCntlInstr(uint32_t writemask, uint32_t regid) {
    return A6XX_VFD_DEST_CNTL_INSTR_WRITEMASK(writemask) | A6XX_VFD_DEST_CNTL_INSTR_REGID(regid);
}

// the VPC link registers, from render::Linkage (ir3_link_shaders' result): tu_pipeline.cc:977-1053
constexpr uint32_t spVsOutReg(uint32_t aRegid, uint32_t aCompmask, uint32_t bRegid, uint32_t bCompmask) {
    return A6XX_SP_VS_OUT_REG_A_REGID(aRegid) | A6XX_SP_VS_OUT_REG_A_COMPMASK(aCompmask) | A6XX_SP_VS_OUT_REG_B_REGID(bRegid) |
           A6XX_SP_VS_OUT_REG_B_COMPMASK(bCompmask);
}
constexpr uint32_t spVsVpcDstReg(uint32_t loc0, uint32_t loc1, uint32_t loc2, uint32_t loc3) {
    return A6XX_SP_VS_VPC_DST_REG_OUTLOC0(loc0) | A6XX_SP_VS_VPC_DST_REG_OUTLOC1(loc1) | A6XX_SP_VS_VPC_DST_REG_OUTLOC2(loc2) |
           A6XX_SP_VS_VPC_DST_REG_OUTLOC3(loc3);
}
constexpr uint32_t spVsPrimitiveCntl(uint32_t out, uint32_t flagsRegid = 0) {
    return A6XX_SP_VS_PRIMITIVE_CNTL_OUT(out) | A6XX_SP_VS_PRIMITIVE_CNTL_FLAGS_REGID(flagsRegid);
}
constexpr uint32_t vpcVsPack(uint32_t strideInVpc, uint32_t positionLoc, uint32_t psizeLoc = kLocNone, uint32_t extraPos = 0) {
    return A6XX_VPC_VS_PACK_STRIDE_IN_VPC(strideInVpc) | A6XX_VPC_VS_PACK_POSITIONLOC(positionLoc) | A6XX_VPC_VS_PACK_PSIZELOC(psizeLoc) |
           A6XX_VPC_VS_PACK_EXTRAPOS(extraPos);
}
constexpr uint32_t vpcVsClipCntl(uint32_t clipMask, uint32_t dist03Loc = kLocNone, uint32_t dist47Loc = kLocNone) {
    return A6XX_VPC_VS_CLIP_CNTL_CLIP_MASK(clipMask) | A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_03_LOC(dist03Loc) | A6XX_VPC_VS_CLIP_CNTL_CLIP_DIST_47_LOC(dist47Loc);
}
constexpr uint32_t vpcVsLayerCntl(uint32_t layerLoc = kLocNone, uint32_t viewLoc = kLocNone, uint32_t shadingRateLoc = kLocNone) {
    return A6XX_VPC_VS_LAYER_CNTL_LAYERLOC(layerLoc) | A6XX_VPC_VS_LAYER_CNTL_VIEWLOC(viewLoc) | A6XX_VPC_VS_LAYER_CNTL_SHADINGRATELOC(shadingRateLoc);
}
constexpr uint32_t grasVsClCntl(uint32_t clipMask, uint32_t cullMask) {
    return A6XX_GRAS_VS_CL_CNTL_CLIP_MASK(clipMask) | A6XX_GRAS_VS_CL_CNTL_CULL_MASK(cullMask);
}
constexpr uint32_t pcVsOutCntl(uint32_t strideInVpc, bool psize = false, bool layer = false, bool view = false, bool primitiveId = false,
                               uint32_t clipMask = 0) {
    return A6XX_PC_VS_OUT_CNTL_STRIDE_IN_VPC(strideInVpc) | (psize ? A6XX_PC_VS_OUT_CNTL_PSIZE : 0u) | (layer ? A6XX_PC_VS_OUT_CNTL_LAYER : 0u) |
           (view ? A6XX_PC_VS_OUT_CNTL_VIEW : 0u) | (primitiveId ? A6XX_PC_VS_OUT_CNTL_PRIMITIVE_ID : 0u) | A6XX_PC_VS_OUT_CNTL_CLIP_MASK(clipMask);
}
constexpr uint32_t vpcCntl0(uint32_t numNonPosVar, uint32_t primidLoc, bool varying, uint32_t viewidLoc) {
    return A6XX_VPC_CNTL_0_NUMNONPOSVAR(numNonPosVar) | A6XX_VPC_CNTL_0_PRIMIDLOC(primidLoc) | (varying ? A6XX_VPC_CNTL_0_VARYING : 0u) |
           A6XX_VPC_CNTL_0_VIEWIDLOC(viewidLoc);
}
// VPC_VAR_DISABLE[i] from word i of the link's varmask: a set bit disables the component
constexpr uint32_t vpcVarDisable(uint32_t varmaskWord) { return ~varmaskWord; }
// VPC_VARYING_INTERP_MODE[i]'s two bits for one component at VPC location `loc`: which register (loc / 16) and
// where in it (loc % 16 * 2); render::link puts INTERP_FLAT there for a flat input (tu_pipeline.cc:752-796)
constexpr uint32_t vpcVaryingInterpModeReg(uint32_t loc) { return loc / 16; }
constexpr uint32_t vpcVaryingInterpModeBits(uint32_t loc, a6xx_varying_interp_mode mode) { return uint32_t(mode) << ((loc % 16) * 2); }

// the FS input registers (tu_shader.cc:1627-1812), kRegidNone for every system value the FS does not read
constexpr uint32_t hlsqControl1Reg() { return A6XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD(kPrimAllocThreshold); }
constexpr uint32_t hlsqControl2Reg(uint32_t faceRegid, uint32_t sampleIdRegid, uint32_t sampleMaskRegid, uint32_t centerRhwRegid) {
    return A6XX_HLSQ_CONTROL_2_REG_FACEREGID(faceRegid) | A6XX_HLSQ_CONTROL_2_REG_SAMPLEID(sampleIdRegid) |
           A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(sampleMaskRegid) | A6XX_HLSQ_CONTROL_2_REG_CENTERRHW(centerRhwRegid);
}
constexpr uint32_t hlsqControl3Reg(uint32_t ijPerspPixel, uint32_t ijLinearPixel, uint32_t ijPerspCentroid, uint32_t ijLinearCentroid) {
    return A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(ijPerspPixel) | A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(ijLinearPixel) |
           A6XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(ijPerspCentroid) | A6XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(ijLinearCentroid);
}
constexpr uint32_t hlsqControl4Reg(uint32_t ijPerspSample, uint32_t ijLinearSample, uint32_t xyCoordRegid, uint32_t zwCoordRegid) {
    return A6XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE(ijPerspSample) | A6XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE(ijLinearSample) |
           A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(xyCoordRegid) | A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwCoordRegid);
}
// the IJ bits of GRAS_CNTL and RB_RENDER_CONTROL0 from the FS's seven ij regids in ir3_bary order (tu_shader.cc:
// 1747-1756); `needSize` adds IJ_LINEAR_PIXEL (gl_FragCoord's size), `needSizePerSamp` IJ_LINEAR_SAMPLE
constexpr uint32_t ijBits(const uint32_t* ijRegid, bool needSize = false, bool needSizePerSamp = false) {
    return (validReg(ijRegid[IJ_PERSP_PIXEL]) ? A6XX_GRAS_CNTL_IJ_PERSP_PIXEL : 0u) |
           (validReg(ijRegid[IJ_PERSP_CENTROID]) ? A6XX_GRAS_CNTL_IJ_PERSP_CENTROID : 0u) |
           (validReg(ijRegid[IJ_PERSP_SAMPLE]) ? A6XX_GRAS_CNTL_IJ_PERSP_SAMPLE : 0u) |
           (validReg(ijRegid[IJ_LINEAR_PIXEL]) || needSize ? A6XX_GRAS_CNTL_IJ_LINEAR_PIXEL : 0u) |
           (validReg(ijRegid[IJ_LINEAR_CENTROID]) ? A6XX_GRAS_CNTL_IJ_LINEAR_CENTROID : 0u) |
           (validReg(ijRegid[IJ_LINEAR_SAMPLE]) || needSizePerSamp ? A6XX_GRAS_CNTL_IJ_LINEAR_SAMPLE : 0u);
}
constexpr uint32_t grasCntl(uint32_t ij, uint32_t fragcoordCompmask) { return ij | A6XX_GRAS_CNTL_COORD_MASK(fragcoordCompmask); }
constexpr uint32_t rbRenderControl0(uint32_t ij, uint32_t fragcoordCompmask, bool varyings) {
    return ij | A6XX_RB_RENDER_CONTROL0_COORD_MASK(fragcoordCompmask) | (varyings ? A6XX_RB_RENDER_CONTROL0_UNK10 : 0u);
}
constexpr uint32_t rbRenderControl1(bool faceness, bool sampleId, bool sampleMask, bool centerRhw, a6xx_fragcoord_sample_mode mode = FRAGCOORD_CENTER) {
    return A6XX_RB_RENDER_CONTROL1_FRAGCOORDSAMPLEMODE(mode) | (sampleMask ? A6XX_RB_RENDER_CONTROL1_SAMPLEMASK : 0u) |
           (sampleId ? A6XX_RB_RENDER_CONTROL1_SAMPLEID : 0u) | (centerRhw ? A6XX_RB_RENDER_CONTROL1_CENTERRHW : 0u) |
           (faceness ? A6XX_RB_RENDER_CONTROL1_FACENESS : 0u);
}
// SP_FS_PREFETCH_CNTL (tu_shader.cc:1652-1660): no prefetch commands in step one; IJ_WRITE_DISABLE when the FS has
// no perspective pixel barycentric
constexpr uint32_t spFsPrefetchCntl(uint32_t count, bool ijWriteDisable, bool endOfQuad = false) {
    return A6XX_SP_FS_PREFETCH_CNTL_COUNT(count) | (ijWriteDisable ? A6XX_SP_FS_PREFETCH_CNTL_IJ_WRITE_DISABLE : 0u) |
           (endOfQuad ? A6XX_SP_FS_PREFETCH_CNTL_ENDOFQUAD : 0u);
}

// the FS output registers (tu_shader.cc:1816-1898)
constexpr uint32_t spFsOutputCntl0(uint32_t depthRegid, uint32_t sampmaskRegid, uint32_t stencilrefRegid, bool dualColorIn = false) {
    return (dualColorIn ? A6XX_SP_FS_OUTPUT_CNTL0_DUAL_COLOR_IN_ENABLE : 0u) | A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID(depthRegid) |
           A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID(sampmaskRegid) | A6XX_SP_FS_OUTPUT_CNTL0_STENCILREF_REGID(stencilrefRegid);
}
constexpr uint32_t spFsOutputReg(uint32_t regid, bool half) { return A6XX_SP_FS_OUTPUT_REG_REGID(regid) | (half ? A6XX_SP_FS_OUTPUT_REG_HALF_PRECISION : 0u); }

// the rasterizer (tu_pipeline.cc:3120-3218). GRAS_SU_CNTL: the line half width in quarter pixels (turnip's width 1.0 ->
// 0.5 -> 2), RECTANGULAR lines; cull none and FRONT_CW clear are step one's. GRAS_CL_CNTL: Metal's [0, 1] depth
// (ZERO_GB_SCALE_Z) and VP_CLIP_CODE_IGNORE, as turnip for Vulkan; the point size words in sixteenths.
constexpr uint32_t grasSuCntl(bool cullFront, bool cullBack, bool frontCw, uint32_t lineHalfWidthQuarters = 2, a5xx_line_mode lineMode = RECTANGULAR,
                              bool polyOffset = false) {
    return (cullFront ? A6XX_GRAS_SU_CNTL_CULL_FRONT : 0u) | (cullBack ? A6XX_GRAS_SU_CNTL_CULL_BACK : 0u) | (frontCw ? A6XX_GRAS_SU_CNTL_FRONT_CW : 0u) |
           A6XX_GRAS_SU_CNTL_LINEHALFWIDTH(lineHalfWidthQuarters) | (polyOffset ? A6XX_GRAS_SU_CNTL_POLY_OFFSET : 0u) |
           A6XX_GRAS_SU_CNTL_LINE_MODE(lineMode);
}
constexpr uint32_t grasClCntl(bool zeroGbScaleZ = true, bool vpClipCodeIgnore = true, bool znearClipDisable = false, bool zfarClipDisable = false,
                              bool zClampEnable = false) {
    return (znearClipDisable ? A6XX_GRAS_CL_CNTL_ZNEAR_CLIP_DISABLE : 0u) | (zfarClipDisable ? A6XX_GRAS_CL_CNTL_ZFAR_CLIP_DISABLE : 0u) |
           (zClampEnable ? A6XX_GRAS_CL_CNTL_Z_CLAMP_ENABLE : 0u) | (zeroGbScaleZ ? A6XX_GRAS_CL_CNTL_ZERO_GB_SCALE_Z : 0u) |
           (vpClipCodeIgnore ? A6XX_GRAS_CL_CNTL_VP_CLIP_CODE_IGNORE : 0u);
}
constexpr uint32_t grasSuPointMinmax(uint32_t minSixteenths, uint32_t maxSixteenths) {
    return A6XX_GRAS_SU_POINT_MINMAX_MIN(minSixteenths) | A6XX_GRAS_SU_POINT_MINMAX_MAX(maxSixteenths);
}
constexpr uint32_t grasSuPointSize(uint32_t sixteenths) { return A6XX_GRAS_SU_POINT_SIZE(sixteenths); }


// --- the 2D engine: a solid-colour fill of the colour target, turnip's sysmem clear (r2d_*, tu_clear_blit.cc:145-570) ----
//
// A render pass with loadAction Clear begins with one CP_BLIT over the render area: turnip clears a single-sampled
// sysmem attachment with the 2D engine, not with a draw (clear_sysmem_attachment, tu_clear_blit.cc:4402-4444, which
// picks r3d only for MSAA). The 2D engine has its own copies of the format and the destination (RB_2D_DST_*), so the
// clear does not read RB_MRT or any shader state.

// the intermediate format the 2D engine converts the solid colour through (format_to_ifmt, tu_clear_blit.cc:41-71)
enum a6xx_2d_ifmt : uint32_t {
    R2D_UNORM8_SRGB = 1,           // a6xx.xml.h:1125  an sRGB target: the engine encodes, the clear value stays linear-packed (r2d_setup_common)
    R2D_FLOAT16 = 3,               // a6xx.xml.h:1124  16-bit float channels: the value as halfs
    R2D_FLOAT32 = 4,               // a6xx.xml.h:1123  32-bit float channels: the value's bits
    R2D_INT8 = 5,                  // a6xx.xml.h:1122
    R2D_INT16 = 6,                 // a6xx.xml.h:1121
    R2D_INT32 = 7,                 // a6xx.xml.h:1120  32-bit integer channels: the value as integers
    R2D_UNORM8 = 16,               // a6xx.xml.h:1119  8-bit unorm channels: each value rounded to 0..255 (tu_pack_float32_for_unorm)
};

// RB_2D_BLIT_CNTL and GRAS_2D_BLIT_CNTL take the same word (r2d_setup_common writes one value to both)
inline constexpr uint32_t REG_A6XX_RB_2D_BLIT_CNTL = 0x8c00;                                // a6xx.xml.h:6555
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_ROTATE__MASK = 0x00000007;                   // a6xx.xml: a6xx_rotation, bits 0..2
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_SOLID_COLOR = 128;                           // a6xx.xml.h:6569
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT__MASK = 0x0000ff00;             // a6xx.xml.h:6570
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT__SHIFT = 8;                     // a6xx.xml.h:6571
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_MASK__MASK = 0x00f00000;                     // a6xx.xml.h:6584
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_MASK__SHIFT = 20;                            // a6xx.xml.h:6585
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_IFMT__MASK = 0x1f000000;                     // a6xx.xml.h:6590
inline constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_IFMT__SHIFT = 24;                            // a6xx.xml.h:6591
inline constexpr uint32_t REG_A6XX_GRAS_2D_BLIT_CNTL = 0x8400;                              // a6xx.xml.h:5087
inline constexpr uint32_t A6XX_GRAS_2D_BLIT_CNTL_SOLID_COLOR = 128;                         // a6xx.xml.h:5101
inline constexpr uint32_t A6XX_GRAS_2D_BLIT_CNTL_COLOR_FORMAT__MASK = 0x0000ff00;           // a6xx.xml.h:5102
inline constexpr uint32_t A6XX_GRAS_2D_BLIT_CNTL_MASK__MASK = 0x00f00000;                   // a6xx.xml.h:5116
inline constexpr uint32_t A6XX_GRAS_2D_BLIT_CNTL_IFMT__MASK = 0x1f000000;                   // a6xx.xml.h:5122
static_assert(A6XX_GRAS_2D_BLIT_CNTL_SOLID_COLOR == A6XX_RB_2D_BLIT_CNTL_SOLID_COLOR &&
                  A6XX_GRAS_2D_BLIT_CNTL_COLOR_FORMAT__MASK == A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT__MASK &&
                  A6XX_GRAS_2D_BLIT_CNTL_MASK__MASK == A6XX_RB_2D_BLIT_CNTL_MASK__MASK && A6XX_GRAS_2D_BLIT_CNTL_IFMT__MASK == A6XX_RB_2D_BLIT_CNTL_IFMT__MASK,
              "one blit word serves both copies");
constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT(uint32_t v) {
    return field(v, A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT__SHIFT, A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT__MASK);
}
constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_MASK(uint32_t v) { return field(v, A6XX_RB_2D_BLIT_CNTL_MASK__SHIFT, A6XX_RB_2D_BLIT_CNTL_MASK__MASK); }
constexpr uint32_t A6XX_RB_2D_BLIT_CNTL_IFMT(uint32_t v) { return field(v, A6XX_RB_2D_BLIT_CNTL_IFMT__SHIFT, A6XX_RB_2D_BLIT_CNTL_IFMT__MASK); }

// SP_2D_DST_FORMAT: the format again for the SP side of the engine, UINT/SINT for the pure-integer formats
inline constexpr uint32_t REG_A6XX_SP_2D_DST_FORMAT = 0xacc0;                               // a6xx.xml.h:9521
inline constexpr uint32_t A6XX_SP_2D_DST_FORMAT_SINT = 2;                                   // a6xx.xml.h:9523
inline constexpr uint32_t A6XX_SP_2D_DST_FORMAT_UINT = 4;                                   // a6xx.xml.h:9524
inline constexpr uint32_t A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__MASK = 0x000007f8;            // a6xx.xml.h:9525
inline constexpr uint32_t A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__SHIFT = 3;                    // a6xx.xml.h:9526
inline constexpr uint32_t A6XX_SP_2D_DST_FORMAT_SRGB = 0x00000800;                          // a6xx.xml.h:9531
inline constexpr uint32_t A6XX_SP_2D_DST_FORMAT_MASK__MASK = 0x0000f000;                    // a6xx.xml.h:9532
inline constexpr uint32_t A6XX_SP_2D_DST_FORMAT_MASK__SHIFT = 12;                           // a6xx.xml.h:9533
inline constexpr uint32_t REG_A6XX_RB_2D_UNKNOWN_8C01 = 0x8c01;                             // a6xx.xml.h:6605
inline constexpr uint32_t REG_A6XX_GRAS_2D_DST_TL = 0x8405;                                 // a6xx.xml.h:5173
inline constexpr uint32_t REG_A6XX_GRAS_2D_DST_BR = 0x8406;                                 // a6xx.xml.h:5188
inline constexpr uint32_t A6XX_GRAS_2D_DST_TL_X__MASK = 0x00003fff;                         // a6xx.xml.h:5174
inline constexpr uint32_t A6XX_GRAS_2D_DST_TL_Y__MASK = 0x3fff0000;                         // a6xx.xml.h:5180
inline constexpr uint32_t A6XX_GRAS_2D_DST_BR_X__MASK = 0x00003fff;                         // a6xx.xml.h:5189
inline constexpr uint32_t A6XX_GRAS_2D_DST_BR_Y__MASK = 0x3fff0000;                         // a6xx.xml.h:5195
inline constexpr uint32_t REG_A6XX_RB_2D_SRC_SOLID_C0 = 0x8c2c;                             // a6xx.xml.h:6693
constexpr uint32_t A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT(uint32_t v) {
    return field(v, A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__SHIFT, A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT__MASK);
}
constexpr uint32_t A6XX_SP_2D_DST_FORMAT_MASK(uint32_t v) { return field(v, A6XX_SP_2D_DST_FORMAT_MASK__SHIFT, A6XX_SP_2D_DST_FORMAT_MASK__MASK); }

// the destination: INFO, BASE lo/hi, PITCH as one pkt4 from RB_2D_DST_INFO (r2d_dst, tu_clear_blit.cc:396-413), then
// the flag buffer's three words from RB_2D_DST_FLAGS (0: no UBWC)
inline constexpr uint32_t REG_A6XX_RB_2D_DST_INFO = 0x8c17;                                 // a6xx.xml.h:6608
inline constexpr uint32_t A6XX_RB_2D_DST_INFO_COLOR_FORMAT__MASK = 255;                     // a6xx.xml.h:6609
inline constexpr uint32_t A6XX_RB_2D_DST_INFO_COLOR_FORMAT__SHIFT = 0;                      // a6xx.xml.h:6610
inline constexpr uint32_t A6XX_RB_2D_DST_INFO_TILE_MODE__MASK = 0x00000300;                 // a6xx.xml.h:6615
inline constexpr uint32_t A6XX_RB_2D_DST_INFO_TILE_MODE__SHIFT = 8;                         // a6xx.xml.h:6616
inline constexpr uint32_t A6XX_RB_2D_DST_INFO_COLOR_SWAP__MASK = 0x00000c00;                // a6xx.xml.h:6621
inline constexpr uint32_t A6XX_RB_2D_DST_INFO_COLOR_SWAP__SHIFT = 10;                       // a6xx.xml.h:6622
inline constexpr uint32_t A6XX_RB_2D_DST_INFO_SRGB = 0x00002000;                            // a6xx.xml.h:6628
inline constexpr uint32_t REG_A6XX_RB_2D_DST = 0x8c18;                                      // a6xx.xml.h:6638
inline constexpr uint32_t REG_A6XX_RB_2D_DST_PITCH = 0x8c1a;                                // a6xx.xml.h:6641
inline constexpr uint32_t A6XX_RB_2D_DST_PITCH__MASK = 0x0000ffff;                          // a6xx.xml.h:6642
inline constexpr uint32_t A6XX_RB_2D_DST_PITCH__SHIFT = 0;                                  // a6xx.xml.h:6643
inline constexpr uint32_t REG_A6XX_RB_2D_DST_FLAGS = 0x8c20;                                // a6xx.xml.h:6667
inline constexpr uint32_t A6XX_RB_2D_DST_PITCH__SCALE = 6;                                  // a6xx.xml.h:6644  the encoder's val >> 6
constexpr uint32_t A6XX_RB_2D_DST_INFO_COLOR_FORMAT(uint32_t v) {
    return field(v, A6XX_RB_2D_DST_INFO_COLOR_FORMAT__SHIFT, A6XX_RB_2D_DST_INFO_COLOR_FORMAT__MASK);
}
constexpr uint32_t A6XX_RB_2D_DST_INFO_TILE_MODE(uint32_t v) { return field(v, A6XX_RB_2D_DST_INFO_TILE_MODE__SHIFT, A6XX_RB_2D_DST_INFO_TILE_MODE__MASK); }
constexpr uint32_t A6XX_RB_2D_DST_INFO_COLOR_SWAP(uint32_t v) { return field(v, A6XX_RB_2D_DST_INFO_COLOR_SWAP__SHIFT, A6XX_RB_2D_DST_INFO_COLOR_SWAP__MASK); }
constexpr uint32_t A6XX_RB_2D_DST_PITCH(uint32_t v) { return field(v >> A6XX_RB_2D_DST_PITCH__SCALE, A6XX_RB_2D_DST_PITCH__SHIFT, A6XX_RB_2D_DST_PITCH__MASK); }
static_assert(REG_A6XX_RB_2D_DST == REG_A6XX_RB_2D_DST_INFO + 1 && REG_A6XX_RB_2D_DST_PITCH == REG_A6XX_RB_2D_DST_INFO + 3,
              "INFO, BASE lo, BASE hi, PITCH are four consecutive registers: one pkt4");
static_assert(A6XX_GRAS_2D_DST_TL_X__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__MASK && A6XX_GRAS_2D_DST_TL_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK &&
                  A6XX_GRAS_2D_DST_BR_X__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X__MASK && A6XX_GRAS_2D_DST_BR_Y__MASK == A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y__MASK,
              "GRAS_2D_DST_TL/BR are a6xx_reg_xy: regXY encodes them");

// CP_BLIT, one word: the operation (r2d_run, tu_clear_blit.cc:551-570; RB_DBG_ECO_CNTL needs no switch around it on
// this chip: its _blit value is the plain one, freedreno_devices.py's A635 entry)
inline constexpr uint32_t BLIT_OP_SCALE = 3;                                                 // adreno_pm4.xml.h:505
inline constexpr uint32_t CP_BLIT_0_OP__MASK = 0x0000000f;                                   // adreno_pm4.xml.h:2439
inline constexpr uint32_t CP_BLIT_0_OP__SHIFT = 0;                                           // adreno_pm4.xml.h:2440

// the blit word of a solid fill of FMT through IFMT, all four channels written (r2d_setup_common: rotate 0, no scissor,
// no d24s8)
constexpr uint32_t rb2dBlitCntlClear(uint32_t fmt, uint32_t ifmt) {
    return A6XX_RB_2D_BLIT_CNTL_SOLID_COLOR | A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT(fmt) | A6XX_RB_2D_BLIT_CNTL_MASK(0xf) | A6XX_RB_2D_BLIT_CNTL_IFMT(ifmt);
}
constexpr uint32_t sp2dDstFormat(uint32_t fmt, bool sint, bool uint, bool srgb) {
    return (sint ? A6XX_SP_2D_DST_FORMAT_SINT : 0u) | (uint ? A6XX_SP_2D_DST_FORMAT_UINT : 0u) | A6XX_SP_2D_DST_FORMAT_COLOR_FORMAT(fmt) |
           (srgb ? A6XX_SP_2D_DST_FORMAT_SRGB : 0u) | A6XX_SP_2D_DST_FORMAT_MASK(0xf);
}
constexpr uint32_t rb2dDstInfo(uint32_t fmt, uint32_t tileMode, uint32_t swap, bool srgb) {
    return A6XX_RB_2D_DST_INFO_COLOR_FORMAT(fmt) | A6XX_RB_2D_DST_INFO_TILE_MODE(tileMode) | A6XX_RB_2D_DST_INFO_COLOR_SWAP(swap) |
           (srgb ? A6XX_RB_2D_DST_INFO_SRGB : 0u);
}
constexpr uint32_t cpBlit0(uint32_t op) { return field(op, CP_BLIT_0_OP__SHIFT, CP_BLIT_0_OP__MASK); }

// The 2D engine's source, for a copy or a scaled copy (r2d_src, tu_clear_blit.cc:268-301; fdl6_view_init's
// SP_PS_2D_SRC_INFO/SIZE, fd6_view.c:320-334): the image as INFO (format, tile, swap, sRGB, FILTER for a linear scale,
// and the two UNK bits every view sets), SIZE, the base and the pitch as one pkt4 of five, the flag buffer's three
// words 0; the rectangle read in GRAS_2D_SRC_TL/BR X and Y, integer pixels at bit 8, BR inclusive (r2d_coords,
// tu6_blit_image)
inline constexpr uint32_t REG_A6XX_GRAS_2D_SRC_TL_X = 0x8401;                               // a6xx.xml.h:5137
inline constexpr uint32_t REG_A6XX_GRAS_2D_SRC_BR_X = 0x8402;                               // a6xx.xml.h:5146
inline constexpr uint32_t REG_A6XX_GRAS_2D_SRC_TL_Y = 0x8403;                               // a6xx.xml.h:5155
inline constexpr uint32_t REG_A6XX_GRAS_2D_SRC_BR_Y = 0x8404;                               // a6xx.xml.h:5164
inline constexpr uint32_t A6XX_GRAS_2D_SRC_TL_X__MASK = 0x01ffff00;                         // a6xx.xml.h:5138
inline constexpr uint32_t A6XX_GRAS_2D_SRC_TL_X__SHIFT = 8;                                 // a6xx.xml.h:5139
inline constexpr uint32_t REG_A6XX_SP_PS_2D_SRC_INFO = 0xb4c0;                              // a6xx.xml.h:9836
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_COLOR_FORMAT__MASK = 0x000000ff;           // a6xx.xml.h:9837
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_TILE_MODE__MASK = 0x00000300;              // a6xx.xml.h:9843
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_TILE_MODE__SHIFT = 8;                      // a6xx.xml.h:9844
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_COLOR_SWAP__MASK = 0x00000c00;             // a6xx.xml.h:9849
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_COLOR_SWAP__SHIFT = 10;                    // a6xx.xml.h:9850
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_SRGB = 0x00002000;                         // a6xx.xml.h:9856
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_SAMPLES__MASK = 0x0000c000;                // a6xx.xml.h:9857
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_SAMPLES__SHIFT = 14;                       // a6xx.xml.h:9858
constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_SAMPLES(a3xx_msaa_samples v) { return field(v, A6XX_SP_PS_2D_SRC_INFO_SAMPLES__SHIFT, A6XX_SP_PS_2D_SRC_INFO_SAMPLES__MASK); }
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_FILTER = 0x00010000;                       // a6xx.xml.h:9863
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_SAMPLES_AVERAGE = 0x00040000;              // a6xx.xml.h:9865
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_UNK20 = 0x00100000;                        // a6xx.xml.h:9867
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_INFO_UNK22 = 0x00400000;                        // a6xx.xml.h:9869
inline constexpr uint32_t REG_A6XX_SP_PS_2D_SRC_SIZE = 0xb4c1;                              // a6xx.xml.h:9880
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_SIZE_WIDTH__MASK = 0x00007fff;                  // a6xx.xml.h:9881
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_SIZE_HEIGHT__MASK = 0x3fff8000;                 // a6xx.xml.h:9887
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_SIZE_HEIGHT__SHIFT = 15;                        // a6xx.xml.h:9888
inline constexpr uint32_t REG_A6XX_SP_PS_2D_SRC = 0xb4c2;                                   // a6xx.xml.h:9895
inline constexpr uint32_t REG_A6XX_SP_PS_2D_SRC_PITCH = 0xb4c4;                             // a6xx.xml.h:9898
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_PITCH_PITCH__MASK = 0x00fffe00;                 // a6xx.xml.h:9905
inline constexpr uint32_t A6XX_SP_PS_2D_SRC_PITCH_PITCH__SHIFT = 9;                         // a6xx.xml.h:9906
inline constexpr uint32_t REG_A6XX_SP_PS_2D_SRC_FLAGS = 0xb4ca;                             // a6xx.xml.h:10017
static_assert(REG_A6XX_SP_PS_2D_SRC_SIZE == REG_A6XX_SP_PS_2D_SRC_INFO + 1 && REG_A6XX_SP_PS_2D_SRC == REG_A6XX_SP_PS_2D_SRC_INFO + 2 &&
                  REG_A6XX_SP_PS_2D_SRC_PITCH == REG_A6XX_SP_PS_2D_SRC_INFO + 4 && REG_A6XX_GRAS_2D_DST_TL == REG_A6XX_GRAS_2D_SRC_BR_Y + 1,
              "INFO, SIZE, BASE lo, hi, PITCH are one pkt4; the four source coordinates run on into GRAS_2D_DST_TL");
constexpr uint32_t gras2dSrc(uint32_t v) { return field(v, A6XX_GRAS_2D_SRC_TL_X__SHIFT, A6XX_GRAS_2D_SRC_TL_X__MASK); }
constexpr uint32_t sp2dSrcInfo(uint32_t fmt, uint32_t swap, bool srgb, bool filter) {
    return field(fmt, 0, A6XX_SP_PS_2D_SRC_INFO_COLOR_FORMAT__MASK) | field(TILE6_LINEAR, A6XX_SP_PS_2D_SRC_INFO_TILE_MODE__SHIFT, A6XX_SP_PS_2D_SRC_INFO_TILE_MODE__MASK) |
           field(swap, A6XX_SP_PS_2D_SRC_INFO_COLOR_SWAP__SHIFT, A6XX_SP_PS_2D_SRC_INFO_COLOR_SWAP__MASK) | (srgb ? A6XX_SP_PS_2D_SRC_INFO_SRGB : 0u) |
           (filter ? A6XX_SP_PS_2D_SRC_INFO_FILTER : 0u) | A6XX_SP_PS_2D_SRC_INFO_UNK20 | A6XX_SP_PS_2D_SRC_INFO_UNK22;
}
constexpr uint32_t sp2dSrcSize(uint32_t w, uint32_t h) {
    return field(w, 0, A6XX_SP_PS_2D_SRC_SIZE_WIDTH__MASK) | field(h, A6XX_SP_PS_2D_SRC_SIZE_HEIGHT__SHIFT, A6XX_SP_PS_2D_SRC_SIZE_HEIGHT__MASK);
}
constexpr uint32_t sp2dSrcPitch(uint32_t pitchBytes) { return field(pitchBytes >> 6, A6XX_SP_PS_2D_SRC_PITCH_PITCH__SHIFT, A6XX_SP_PS_2D_SRC_PITCH_PITCH__MASK); }
// the blit word of a copy (r2d_setup_common with clear false)
constexpr uint32_t rb2dBlitCntlCopy(uint32_t fmt, uint32_t ifmt) {
    return A6XX_RB_2D_BLIT_CNTL_COLOR_FORMAT(fmt) | A6XX_RB_2D_BLIT_CNTL_MASK(0xf) | A6XX_RB_2D_BLIT_CNTL_IFMT(ifmt);
}

// --- a UBO descriptor: what an ldc reads through (fd6_emit_ubos, fd6_const.cc:266-296) ------------------------------------
// two dwords per UBO, loaded as ST6_UBO into the stage's shader block from the packet (8 bytes a unit): the address,
// and in the high dword the size in vec4 (A6XX_UBO_1_SIZE, bits 17..31 above the address's 17 high bits)
inline constexpr uint32_t A6XX_UBO_1_SIZE__MASK = 0xfffe0000;                                // a6xx.xml.h:11544
inline constexpr uint32_t A6XX_UBO_1_SIZE__SHIFT = 17;                                       // a6xx.xml.h:11545
constexpr uint32_t A6XX_UBO_1_SIZE(uint32_t v) { return field(v, A6XX_UBO_1_SIZE__SHIFT, A6XX_UBO_1_SIZE__MASK); }
inline constexpr uint32_t kUboMaxVec4 = A6XX_UBO_1_SIZE__MASK >> A6XX_UBO_1_SIZE__SHIFT;   // not-in-headers: the 15-bit field's largest size
constexpr void uboDescriptor(uint64_t address, uint32_t sizeVec4, uint32_t out[2]) {
    out[0] = uint32_t(address);
    out[1] = uint32_t(address >> 32) | A6XX_UBO_1_SIZE(sizeVec4);
}

}  // namespace hakios::metal::a6xx
