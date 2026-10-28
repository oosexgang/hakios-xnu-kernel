// hakios-metal: a compute kernel as the dispatch registers want it.
//
// SPDX-License-Identifier: MIT
//
// A Kernel is plain data: the machine code of one ir3 shader variant and every
// layout field the a6xx compute dispatch takes from it. It is what
// tools/ir3-kernel writes into tests/fixtures/*.hpp from a text kernel
// (tools/kernels/*.asm), and what the ir3_compiler patch in tools/mesa writes
// from an AIR kernel, so that the driver programs both the same way:
// Mesa's computerator (src/freedreno/computerator/a6xx.cc) is the reference
// for which register reads which field, and each field below names it.
//
// Register/packet names are the a6xx ones (adreno a6xx.xml); the A635 in this
// phone is gen 6 with LPAC, so both the HLSQ_CS_* and the SP_CS_CNTL_* copies
// of the id registers are written, as computerator does for has_lpac.

#pragma once

#include <cstdint>

namespace hakios::metal {

// One UBO range ir3 lowered to pushed constants (ir3_ubo_range): the driver
// copies sizeDwords dwords from byte 4*startDword of UBO `block` into the const
// file at vec4 offsetVec4, with CP_LOAD_STATE6_FRAG (ST6_CONSTANTS, SB6_CS_SHADER,
// DST_OFF = offsetVec4). Text kernels never have these; AIR kernels have one per
// `constant` buffer argument whose loads ir3 could turn into const reads.
struct KernelUboRange {
    uint32_t block;       // the UBO index as the kernel sees it (Metal's [[buffer(n)]] for an AIR kernel)
    uint32_t offsetVec4;  // where in the const file (vec4 units) the range lands
    uint32_t startDword;  // first dword of the UBO that is pushed
    uint32_t sizeDwords;  // how many dwords (a multiple of 4)
};

// One @buf of a text kernel: an IBO the driver creates (a FMT6_32_UINT buffer
// descriptor of sizeBytes/4 elements, computerator's cs_ibo_emit) and binds at
// index i through CP_LOAD_STATE6_FRAG (ST6_IBO, SB6_CS_SHADER) and SP_CS_IBO /
// SP_CS_IBO_COUNT. When constRegid is not kNoConstRegid the buffer's 64-bit GPU
// address is also written into the immediates at dword constRegid (lo) and
// constRegid+1 (hi) before they are uploaded, for kernels that use ldg/stg.
struct KernelBuf {
    uint32_t sizeBytes;
    uint32_t constRegid;  // a dword index into the const file (regid of c<n>.<c>: n*4+c), or kNoConstRegid
};

// regid(63, 0): what ir3 and the registers use for "no register"
// (HLSQ_CS_CNTL_0_LOCALIDREGID and friends take exactly this value when unused).
constexpr uint32_t kNoRegid = 0xfc;
// "no const register" for KernelBuf::constRegid and Kernel::numwgConstRegid.
constexpr uint32_t kNoConstRegid = 0xff;
// Sizes of the fixed arrays below. kMaxBufs is the assembler's MAX_BUFS.
constexpr uint32_t kMaxUboRanges = 8;
constexpr uint32_t kMaxBufs = 4;

struct Kernel {
    // The shader binary: instructions, their padding to instrlen, and the NIR
    // constant data appended at constantDataOffset. Uploaded whole to a GPU
    // buffer whose address goes in SP_CS_OBJ_START (and is preloaded through
    // CP_LOAD_STATE6_FRAG ST6_SHADER SS6_INDIRECT, NUM_UNIT = min(instrlen, instr_cache_size)).
    const uint32_t* words;
    uint32_t numWords;

    uint32_t instrlen;           // SP_CS_INSTRLEN and the second dword of SP_CS_CONFIG: 128-byte units (16 instructions)
    uint32_t constlen;           // HLSQ_CS_CNTL.CONSTLEN: vec4 units, already aligned to 4 (ir3 aligns it on gen 4+)

    int32_t maxReg;              // SP_CS_CTRL_REG0.FULLREGFOOTPRINT = maxReg + 1; -1 when no full register is used
    int32_t maxHalfReg;          // SP_CS_CTRL_REG0.HALFREGFOOTPRINT = maxHalfReg + 1; -1 when none

    bool doubleThreadsize;       // SP_CS_CTRL_REG0.THREADSIZE and HLSQ_CS_CNTL_1.THREADSIZE: THREAD128 when set, else THREAD64
    bool mergedRegs;             // SP_CS_CTRL_REG0.MERGEDREGS (always set by ir3 on gen 6)
    bool earlyPreamble;          // SP_CS_CTRL_REG0.EARLYPREAMBLE

    uint32_t branchstackHw;      // SP_CS_CTRL_REG0.BRANCHSTACK, already through ir3_shader_branchstack_hw()

    uint32_t sharedSize;         // bytes of threadgroup (local) memory: SP_CS_CTRL_REG1.SHARED_SIZE = max((sharedSize-1)/1024, 1)
    uint32_t pvtmemSize;         // bytes of private memory per fiber: SP_CS_PVT_MEM_PARAM.MEMSIZEPERITEM; 0 = no pvtmem
    bool pvtmemPerWave;          // SP_CS_PVT_MEM_SIZE.PERWAVEMEMLAYOUT

    uint32_t localSize[3];       // HLSQ_CS_NDRANGE_0.LOCALSIZEX/Y/Z = localSize - 1, and the KERNEL_GROUP_X/Y/Z counts come from grid / localSize
    bool localSizeVariable;      // the kernel did not fix its threadgroup size (every AIR kernel): the driver picks localSize at dispatch and MUST upload it in the driver params

    uint32_t localIdRegid;       // HLSQ_CS_CNTL_0.LOCALIDREGID and SP_CS_CNTL_0.LOCALIDREGID (regid: r<n>.<c> = n*4+c); kNoRegid when the kernel has no @invocationid
    uint32_t workGroupIdRegid;   // HLSQ_CS_CNTL_0.WGIDCONSTID and SP_CS_CNTL_0.WGIDCONSTID; a shared register (r48+) on gen 6; kNoRegid when absent

    // The constants the kernel carries: @const vec4s and buffer-address /
    // numwg slots of a text kernel, the compiler's immediates of an AIR kernel.
    // Uploaded with CP_LOAD_STATE6_FRAG (ST6_CONSTANTS, SS6_DIRECT, SB6_CS_SHADER)
    // at DST_OFF = immediatesOffsetVec4, NUM_UNIT = ceil(numImmediates/4),
    // truncated to constlen as computerator's cs_const_emit does.
    const uint32_t* immediates;
    uint32_t numImmediates;          // dwords
    uint32_t immediatesOffsetVec4;   // vec4 offset in the const file (ir3_const_allocations.max_const_offset_vec4; 0 for text kernels)

    // Where ir3 expects struct ir3_driver_params_cs (ir3_shader.h: num_work_groups
    // xyz, work_dim, base_group xyz, subgroup_size, local_group_size xyz,
    // subgroup_id_shift, workgroup_id xyz, pad: 4 vec4), as a vec4 offset for
    // the same CP_LOAD_STATE6_FRAG constants packet; -1 when the kernel reads
    // none. A text kernel never does; an AIR kernel does as soon as it uses
    // thread_position_in_grid, because its threadgroup size is not a constant.
    int32_t driverParamsOffsetVec4;

    uint32_t numUbos;    // UBOs the kernel addresses (ir3_const_state.num_ubos): descriptors the driver must bind for any load_ubo ir3 did NOT push to consts
    uint32_t numSsbos;   // SSBOs (ir3_shader_variant.num_ssbos): the first numSsbos IBO slots
    uint32_t numIbos;    // SSBOs + images: SP_CS_CONFIG.NIBO and SP_CS_IBO_COUNT for an AIR kernel (a text kernel's are its @bufs: numBufs)
    uint32_t numSamp;    // SP_CS_CONFIG.NTEX and .NSAMP

    KernelUboRange uboRanges[kMaxUboRanges];
    uint32_t numUboRanges;

    uint32_t numBufs;    // text kernels only: SP_CS_CONFIG.NIBO and SP_CS_IBO_COUNT, one IBO descriptor each (see KernelBuf)
    KernelBuf bufs[kMaxBufs];

    uint32_t numwgConstRegid;    // text kernels' @numwg(c<n>.x): dword index in the const file where the driver writes the grid's workgroup counts x, y, z into the immediates before upload; kNoConstRegid when absent

    uint32_t constantDataOffset; // byte offset inside `words` of the NIR constant data (AIR kernels with constant arrays); the driver uploads it from there with CP_LOAD_STATE6_FRAG SS6_INDIRECT when constantDataSize > 0
    uint32_t constantDataSize;   // bytes; 0 when the kernel has none

    const char* name;    // the kernel's name: the .asm file's stem, or the AIR entry point
    const char* source;  // the kernel text it was built from, for the record (the .asm, or the .ll when the patch was given one)
};

} // namespace hakios::metal
