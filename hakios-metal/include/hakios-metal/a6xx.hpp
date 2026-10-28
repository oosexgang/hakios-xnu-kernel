// The Adreno A6xx registers, packet words and descriptor fields a compute dispatch writes, for the Adreno 7c+ Gen 3
// (the A635 family, chip id 0x06030500: Mesa's "a6xx gen4", the same generation as the A660).
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
// Gallium's a6xx/fd6_compute.cc; that order lives with the code that emits it, not here.
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

enum a6xx_format : uint32_t {
    FMT6_32_UINT = 75,             // a6xx.xml.h:106  a raw buffer: one dword per element
    FMT6_32_32_UINT = 104,         // a6xx.xml.h:122
    FMT6_32_32_32_32_UINT = 131,   // a6xx.xml.h:134
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
    A6XX_TEX_ONE = 5,              // a6xx.xml.h:2367
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
// same number; only the A6XX ones a compute dispatch and its barriers use are here.
enum vgt_event_type : uint32_t {
    CACHE_FLUSH_TS = 4,            // adreno_pm4.xml.h:60   flush the caches, then write the timestamp (Gallium's FD_CACHE_CLEAN)
    CACHE_FLUSH = 6,               // adreno_pm4.xml.h:62
    START_COMPUTE_CTRS = 15,       // adreno_pm4.xml.h:85
    STOP_COMPUTE_CTRS = 16,        // adreno_pm4.xml.h:86
    CACHE_FLUSH_AND_INV_EVENT = 22, // adreno_pm4.xml.h:76  the A2xx name of the same value
    RB_DONE_TS = 22,               // adreno_pm4.xml.h:77   the GPU is done with everything before it; timestamp (FD_RB_DONE)
    PC_CCU_INVALIDATE_DEPTH = 24,  // adreno_pm4.xml.h:91
    PC_CCU_INVALIDATE_COLOR = 25,  // adreno_pm4.xml.h:92
    PC_CCU_FLUSH_DEPTH_TS = 28,    // adreno_pm4.xml.h:94
    PC_CCU_FLUSH_COLOR_TS = 29,    // adreno_pm4.xml.h:95
    CACHE_INVALIDATE = 49,         // adreno_pm4.xml.h:108  invalidate UCHE: before reading what the CPU or a blit wrote
    LABEL = 63,                    // adreno_pm4.xml.h:109  a no-op event; Gallium's dummy to roll the register context
};

enum a6xx_marker : uint32_t {
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

// The type-7 opcodes a dispatch uses (pm4.hpp has the ones every stream uses, under its own names).
enum Opcode : uint32_t {
    CP_WAIT_MEM_GTE = 0x14,        // adreno_pm4.xml.h:326  wait until a memory word >= REF (signed)
    CP_WAIT_FOR_IDLE = 0x26,       // adreno_pm4.xml.h:219
    CP_LOAD_STATE6_GEOM = 0x32,    // adreno_pm4.xml.h:308
    CP_EXEC_CS = 0x33,             // adreno_pm4.xml.h:287
    CP_LOAD_STATE6_FRAG = 0x34,    // adreno_pm4.xml.h:309  what compute uses, for its shader, constants, IBOs and textures
    CP_LOAD_STATE6 = 0x36,         // adreno_pm4.xml.h:310
    CP_WAIT_REG_MEM = 0x3c,        // adreno_pm4.xml.h:220
    CP_REG_TO_MEM = 0x3e,          // adreno_pm4.xml.h:228
    CP_EXEC_CS_INDIRECT = 0x41,    // adreno_pm4.xml.h:286
    CP_EVENT_WRITE = 0x46,         // adreno_pm4.xml.h:234
    CP_SET_MODE = 0x63,            // adreno_pm4.xml.h:307  Gallium writes 1 so that CP_SET_DRAW_STATE runs at once
    CP_SET_MARKER = 0x65,          // adreno_pm4.xml.h:290
    CP_MEM_TO_MEM = 0x73,          // adreno_pm4.xml.h:304
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
// Gallium sets TIMESTAMP with every 4-word event; computerator does not and its timestamps land all the same
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

// --- the texture descriptor, as a buffer view (an IBO) uses it ------------------------------------------------------------
// 16 dwords, REG_A6XX_TEX_CONST_0 to _15; a buffer sets 0, 1, 2, 4, 5 and leaves the rest 0.

inline constexpr uint32_t REG_A6XX_TEX_CONST_0 = 0x0;                                   // a6xx.xml.h:11294
inline constexpr uint32_t REG_A6XX_TEX_CONST_1 = 0x1;                                   // a6xx.xml.h:11354
inline constexpr uint32_t REG_A6XX_TEX_CONST_2 = 0x2;                                   // a6xx.xml.h:11370
inline constexpr uint32_t REG_A6XX_TEX_CONST_3 = 0x3;                                   // a6xx.xml.h:11403
inline constexpr uint32_t REG_A6XX_TEX_CONST_4 = 0x4;                                   // a6xx.xml.h:11422
inline constexpr uint32_t REG_A6XX_TEX_CONST_5 = 0x5;                                   // a6xx.xml.h:11432
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
inline constexpr uint32_t A6XX_TEX_CONST_0_MIPLVLS__MASK = 0x000f0000;                  // a6xx.xml.h:11326
inline constexpr uint32_t A6XX_TEX_CONST_0_MIPLVLS__SHIFT = 16;                         // a6xx.xml.h:11327
inline constexpr uint32_t A6XX_TEX_CONST_0_SAMPLES__MASK = 0x00300000;                  // a6xx.xml.h:11334
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
constexpr uint32_t A6XX_TEX_CONST_0_FMT(a6xx_format v) { return field(v, A6XX_TEX_CONST_0_FMT__SHIFT, A6XX_TEX_CONST_0_FMT__MASK); }

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

constexpr uint32_t eventWrite0(vgt_event_type event, bool timestamp) {
    return CP_EVENT_WRITE_0_EVENT(event) | (timestamp ? CP_EVENT_WRITE_0_TIMESTAMP : 0u);
}

constexpr uint32_t setMarker0(a6xx_marker mode) { return A6XX_CP_SET_MARKER_0_MODE(mode); }

constexpr uint32_t waitRegMem0(cp_cond_function function, poll_memory_type poll) {
    return CP_WAIT_REG_MEM_0_FUNCTION(function) | CP_WAIT_REG_MEM_0_POLL(poll);
}

}  // namespace hakios::metal::a6xx
