// The compute dispatch's words against numbers worked out by hand from Mesa's generated headers (a6xx.xml.h,
// adreno_pm4.xml.h; the field positions are in a6xx.hpp next to each constant) and, for the packet headers, from the
// command processor's parity rule as pm4.hpp states it: a field's bit is 1 when the field has an even number of ones.
// tools/check-a6xx.py checks the constants themselves against the headers; this checks what is built from them.
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/pm4.hpp"
#include "test.hpp"

using namespace hakios::metal::a6xx;
namespace pm4 = hakios::metal::pm4;

TEST(a6xx_type7_headers_for_the_dispatch_packets) {
    // CP_LOAD_STATE6_FRAG 0x34 = 011 0100, three ones: its bit (23) is 0. count 3 = 11, two ones: bit 15 is 1.
    CHECK_EQ(pm4::type7(CP_LOAD_STATE6_FRAG, 3), 0x70348003u);
    // count 7 = 111, three ones: bit 15 is 0
    CHECK_EQ(pm4::type7(CP_LOAD_STATE6_FRAG, 7), 0x70340007u);
    // CP_EXEC_CS 0x33 = 011 0011, four ones: bit 23 is 1 (0x00800000). count 4 = 100, one bit: bit 15 is 0.
    CHECK_EQ(pm4::type7(CP_EXEC_CS, 4), 0x70b30004u);
    // CP_EVENT_WRITE 0x46 = 100 0110, three ones: 0. count 1: one bit: 0. count 4: 0.
    CHECK_EQ(pm4::type7(CP_EVENT_WRITE, 1), 0x70460001u);
    CHECK_EQ(pm4::type7(CP_EVENT_WRITE, 4), 0x70460004u);
    // CP_SET_MARKER 0x65 = 110 0101, four ones: bit 23 is 1. count 1: 0.
    CHECK_EQ(pm4::type7(CP_SET_MARKER, 1), 0x70e50001u);
    // CP_WAIT_REG_MEM 0x3c = 011 1100, four ones: bit 23 is 1. count 6 = 110, two ones: bit 15 is 1 (0x8000).
    CHECK_EQ(pm4::type7(CP_WAIT_REG_MEM, 6), 0x70bc8006u);
    // CP_WAIT_MEM_GTE 0x14 = 001 0100, two ones: bit 23 is 1. count 4: 0.
    CHECK_EQ(pm4::type7(CP_WAIT_MEM_GTE, 4), 0x70940004u);
    // and a register write: SP_CS_OBJ_START 0xa9b4 = 1010 1001 1011 0100, eight ones: bit 27 is 1 (0x08000000);
    // the two words of a 64-bit address: count 2, one bit: bit 7 is 0
    CHECK_EQ(pm4::type4(REG_A6XX_SP_CS_OBJ_START, 2), 0x48a9b402u);
}

TEST(a6xx_register_offsets_are_mesas) {
    // a few of the offsets, so that a wrong header edit fails here before the script is run
    CHECK_EQ(REG_A6XX_SP_CS_CTRL_REG0, 0xa9b0u);
    CHECK_EQ(REG_A6XX_SP_CS_CONFIG, 0xa9bbu);
    CHECK_EQ(REG_A6XX_SP_CS_IBO_COUNT, 0xaa00u);
    CHECK_EQ(REG_A6XX_SP_MODE_CONTROL, 0xab00u);
    CHECK_EQ(REG_A6XX_HLSQ_CS_CNTL, 0xb987u);
    CHECK_EQ(REG_A6XX_HLSQ_CS_NDRANGE_0, 0xb990u);
    CHECK_EQ(REG_A6XX_HLSQ_CS_KERNEL_GROUP_Z, 0xb99bu);
    CHECK_EQ(REG_A6XX_HLSQ_CS_CTRL_REG1, 0xb9d0u);
    CHECK_EQ(REG_A6XX_HLSQ_INVALIDATE_CMD, 0xbb08u);
    CHECK_EQ(uint32_t(SB6_CS_SHADER), 13u);
    CHECK_EQ(uint32_t(FMT6_32_UINT), 75u);
}

TEST(a6xx_sp_cs_ctrl_reg0) {
    // 8 full registers, 2 half, branch stack 3, 128 threads, merged registers:
    //   THREADSIZE(THREAD128)  = 1 << 20 = 0x00100000
    //   FULLREGFOOTPRINT(8)    = 8 << 7  = 0x00000400
    //   HALFREGFOOTPRINT(2)    = 2 << 1  = 0x00000004
    //   BRANCHSTACK(3)         = 3 << 14 = 0x0000c000
    //   MERGEDREGS                       = 0x80000000
    CHECK_EQ(spCsCtrlReg0(8, 2, 3, THREAD128), 0x8010c404u);
    // 4 full, 1 half, no branches, 64 threads, not merged: 4 << 7 | 1 << 1
    CHECK_EQ(spCsCtrlReg0(4, 1, 0, THREAD64, false), 0x00000202u);
    // the fields alone, and their limits: 6 bits of footprint, 6 of branch stack
    CHECK_EQ(A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(63), 0x1f80u);
    CHECK_EQ(A6XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(64), 0u);
    CHECK_EQ(A6XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(63), 0x7eu);
    CHECK_EQ(A6XX_SP_CS_CTRL_REG0_BRANCHSTACK(63), 0xfc000u);
    CHECK_EQ(A6XX_SP_CS_CTRL_REG0_THREADMODE(SINGLE), 1u);
}

TEST(a6xx_hlsq_cs_cntl_constlen) {
    // constlen 8 vec4: the field counts in 4 vec4, so 8 >> 2 = 2; ENABLED is bit 8
    CHECK_EQ(A6XX_HLSQ_CS_CNTL_CONSTLEN(8), 2u);
    CHECK_EQ(hlsqCsCntl(8), 0x102u);
    // 6 vec4 is rounded up to 8 first (Mesa aligns constlen to 4 before encoding)
    CHECK_EQ(hlsqCsCntl(6), 0x102u);
    CHECK_EQ(hlsqCsCntl(0), 0x100u);
    // the field is 8 bits: 1020 vec4 is the most it holds
    CHECK_EQ(A6XX_HLSQ_CS_CNTL_CONSTLEN(1020), 0xffu);
}

TEST(a6xx_sp_cs_ctrl_reg1_shared_and_constlen) {
    // 4096 bytes of shared memory: (4096 - 1) / 1024 = 3, meaning (3 + 1) KiB enabled; constlen 64 vec4 fits the
    // 128 split, whose mode is 0: the register is just 3
    CHECK_EQ(sharedSizeField(4096), 3u);
    CHECK_EQ(constRamMode(64), CONSTLEN_128);
    CHECK_EQ(spCsCtrlReg1(4096, 64), 0x3u);
    // constlen 200 needs the 256 split: CONSTANTRAMMODE(2) = 2 << 5 = 0x40
    CHECK_EQ(spCsCtrlReg1(4096, 200), 0x43u);
    // the ladder's edges, as a6xx.cc has them
    CHECK_EQ(constRamMode(128), CONSTLEN_128);
    CHECK_EQ(constRamMode(129), CONSTLEN_192);
    CHECK_EQ(constRamMode(192), CONSTLEN_192);
    CHECK_EQ(constRamMode(193), CONSTLEN_256);
    CHECK_EQ(constRamMode(256), CONSTLEN_256);
    CHECK_EQ(constRamMode(257), CONSTLEN_512);
    // the shared size never goes below Mesa's floor of 1, and 32 KiB is 31, the most 5 bits hold
    CHECK_EQ(sharedSizeField(0), 1u);
    CHECK_EQ(sharedSizeField(1024), 1u);
    CHECK_EQ(sharedSizeField(2048), 1u);
    CHECK_EQ(sharedSizeField(2049), 2u);
    CHECK_EQ(sharedSizeField(32768), 31u);
    // HLSQ_CS_CTRL_REG1 has the same layout
    CHECK_EQ(A6XX_HLSQ_CS_CTRL_REG1_SHARED_SIZE(3) | A6XX_HLSQ_CS_CTRL_REG1_CONSTANTRAMMODE(CONSTLEN_256), 0x43u);
}

TEST(a6xx_load_state6_word_0) {
    // ST6_SHADER (0) << 14 | SS6_INDIRECT (2) << 16 = 0x20000 | SB6_CS_SHADER (13) << 18 = 0x340000 | NUM_UNIT 2 << 22 = 0x800000
    CHECK_EQ(loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_CS_SHADER, 2), 0x00b60000u);
    // 3 vec4 of constants at vec4 4, in the packet: 4 | ST6_CONSTANTS (1) << 14 = 0x4000 | 0x340000 | 3 << 22 = 0xc00000
    CHECK_EQ(loadState6_0(4, ST6_CONSTANTS, SS6_DIRECT, SB6_CS_SHADER, 3), 0x00f44004u);
    // one IBO descriptor: ST6_IBO (3) << 14 = 0xc000 | 0x20000 | 0x340000 | 1 << 22 = 0x400000
    CHECK_EQ(loadState6_0(0, ST6_IBO, SS6_INDIRECT, SB6_CS_SHADER, 1), 0x0076c000u);
    // the shader preload is capped at the instruction cache, and that fits the 10-bit NUM_UNIT
    CHECK_EQ(CP_LOAD_STATE6_0_NUM_UNIT(kInstrCacheSize), 127u << 22);
    CHECK_EQ(kInstrCacheSize, 127u);
    // the source address: the low word keeps bits 31:2, the high word is whole
    CHECK_EQ(CP_LOAD_STATE6_1_EXT_SRC_ADDR(0x1000), 0x1000u);
    CHECK_EQ(CP_LOAD_STATE6_1_EXT_SRC_ADDR(0x1003), 0x1000u);
    CHECK_EQ(CP_LOAD_STATE6_2_EXT_SRC_ADDR_HI(0x1), 0x1u);
}

TEST(a6xx_buffer_ibo_descriptor) {
    // 1024 dwords at 0x1'0000'0020: the base is rounded down to 64 bytes (0x1'0000'0000) and the 0x20 bytes left
    // over are 8 dwords of STARTOFFSETTEXELS
    //   dword 0: FMT(FMT6_32_UINT = 75) << 22 = 0x12c00000, TILE6_LINEAR = 0, no swizzle
    //   dword 1: WIDTH(1024 & 0x7fff) = 0x400, HEIGHT(1024 >> 15) = 0
    //   dword 2: STRUCTSIZETEXELS(1) << 4 = 0x10 | STARTOFFSETTEXELS(8) << 16 = 0x80000 | TYPE(A6XX_TEX_BUFFER = 4) << 29 = 0x80000000
    //   dword 3: 0 (no array pitch)
    //   dword 4: BASE_LO = 0x0000'0000 (bits 31:5 of the low word)
    //   dword 5: BASE_HI = 0x1, DEPTH = 0
    //   dwords 6 to 15: 0
    TexConst d = bufferIbo(0x100000020ull, 1024);
    CHECK_EQ(d.dword[0], 0x12c00000u);
    CHECK_EQ(d.dword[1], 0x00000400u);
    CHECK_EQ(d.dword[2], 0x80080010u);
    CHECK_EQ(d.dword[3], 0u);
    CHECK_EQ(d.dword[4], 0x00000000u);
    CHECK_EQ(d.dword[5], 0x00000001u);
    for (int i = 6; i < 16; i++) {
        CHECK_EQ(d.dword[i], 0u);
    }
    // 64-byte aligned: the address goes straight in, as computerator's cs_ibo_emit writes it, and no offset
    d = bufferIbo(0x100000040ull, 1024);
    CHECK_EQ(d.dword[2], 0x80000010u);
    CHECK_EQ(d.dword[4], 0x00000040u);
    CHECK_EQ(d.dword[5], 0x00000001u);
    // a count that spills from WIDTH into HEIGHT: 0x12345 = WIDTH 0x2345 | HEIGHT 2 << 15 = 0x10000, which is the
    // count itself (turnip writes the count as the whole word for that reason); one dword in: STARTOFFSETTEXELS 1
    d = bufferIbo(0x200000004ull, 0x12345);
    CHECK_EQ(d.dword[1], 0x00012345u);
    CHECK_EQ(d.dword[2], 0x80010010u);
    CHECK_EQ(d.dword[4], 0u);
    CHECK_EQ(d.dword[5], 0x2u);
    // the field encoders by themselves
    CHECK_EQ(A6XX_TEX_CONST_4_BASE_LO(0x1f), 0u);
    CHECK_EQ(A6XX_TEX_CONST_4_BASE_LO(0x20), 0x20u);
    CHECK_EQ(A6XX_TEX_CONST_4_BASE_LO(0xffffffff), 0xffffffe0u);
    CHECK_EQ(A6XX_TEX_CONST_5_BASE_HI(0x3ffff), 0x1ffffu);         // 17 bits: a 49-bit address
    CHECK_EQ(A6XX_TEX_CONST_5_DEPTH(1), 0x20000u);
    CHECK_EQ(A6XX_TEX_CONST_1_HEIGHT(1), 0x8000u);
    CHECK_EQ(A6XX_TEX_CONST_2_TYPE(A6XX_TEX_BUFFER), 0x80000000u);
    CHECK_EQ(A6XX_TEX_CONST_0_SWIZ_W(A6XX_TEX_ONE), 5u << 13);
    CHECK_EQ(A6XX_TEX_CONST_3_ARRAY_PITCH(0x1000), 1u);
    CHECK_EQ(sizeof(TexConst), size_t(64));
}

TEST(a6xx_private_memory_fields_are_scaled) {
    // MEMSIZEPERITEM in 512-byte units, TOTALPVTMEMSIZE in 4 KiB, the stack offset in 2 KiB
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(512), 1u);
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_PARAM_MEMSIZEPERITEM(0x1000), 8u);
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_PARAM_HWSTACKSIZEPERTHREAD(2), 2u << 24);
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(0x1000), 1u);
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(0x3ffff000), 0x3ffffu);
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET(0x800), 1u);
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_HW_STACK_OFFSET_OFFSET(0x10000), 0x20u);
    // this chip: 4096 fibers per SP, 2 SPs; a 512-byte private memory per fiber is 2 MiB per SP, 4 MiB in all
    CHECK_EQ(kFibersPerSp, 4096u);
    CHECK_EQ(kNumSpCores, 2u);
    CHECK_EQ(A6XX_SP_CS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(512 * kFibersPerSp), 512u);
}

TEST(a6xx_dispatch_words) {
    // 64 x 1 x 1 in three dimensions: KERNELDIM 3 | LOCALSIZEX(63) << 2 = 0xfc
    CHECK_EQ(ndrange0(3, 64, 1, 1), 0xffu);
    // 8 x 8 x 1 in two: 2 | 7 << 2 = 0x1c | 7 << 12 = 0x7000
    CHECK_EQ(ndrange0(2, 8, 8, 1), 0x701eu);
    // 1024 x 1 x 1 is the most LOCALSIZEX holds (10 bits of 1023)
    CHECK_EQ(ndrange0(1, 1024, 1, 1), 0xffdu);
    // SP_MODE_CONTROL as both emitters write it: CONSTANT_DEMOTION_ENABLE | ISAMMODE(ISAMMODE_GL = 2) << 1
    CHECK_EQ(A6XX_SP_MODE_CONTROL_CONSTANT_DEMOTION_ENABLE | A6XX_SP_MODE_CONTROL_ISAMMODE(ISAMMODE_GL), 0x5u);
    // HLSQ_INVALIDATE_CMD with every stage, CS_IBO and GFX_IBO: bits 0 to 7
    CHECK_EQ(A6XX_HLSQ_INVALIDATE_CMD_VS_STATE | A6XX_HLSQ_INVALIDATE_CMD_HS_STATE | A6XX_HLSQ_INVALIDATE_CMD_DS_STATE |
                 A6XX_HLSQ_INVALIDATE_CMD_GS_STATE | A6XX_HLSQ_INVALIDATE_CMD_FS_STATE | A6XX_HLSQ_INVALIDATE_CMD_CS_STATE |
                 A6XX_HLSQ_INVALIDATE_CMD_CS_IBO | A6XX_HLSQ_INVALIDATE_CMD_GFX_IBO,
             0xffu);
    // SP_CS_CONFIG: enabled, 2 IBOs: 0x100 | 2 << 22
    CHECK_EQ(spCsConfig(2, 0, 0), 0x00800100u);
    // the id registers: work group id in c4 (regid 0x10), local id in r1.x (regid 4), the rest unread (0xfc)
    CHECK_EQ(csCntl0(0x10, 0x4), 0x04fcfc10u);
    CHECK_EQ(csCntl1(THREAD128), 0x2fcu);
    CHECK_EQ(csCntl1(THREAD64), 0xfcu);
    CHECK_EQ(kRegidNone, 0xfcu);
    // the events and waits of the flush at the end
    CHECK_EQ(eventWrite0(RB_DONE_TS, true), 0x40000016u);
    CHECK_EQ(eventWrite0(CACHE_FLUSH_TS, true), 0x40000004u);
    CHECK_EQ(eventWrite0(CACHE_INVALIDATE, false), 0x31u);
    CHECK_EQ(waitRegMem0(WRITE_EQ, POLL_MEMORY), 0x13u);
    CHECK_EQ(setMarker0(RM6_COMPUTE), 0x8u);
    CHECK_EQ(CP_REG_TO_MEM_0_64B | CP_REG_TO_MEM_0_REG(0xae0f), 0x4000ae0fu);
}

TEST(a6xx_batch_start_words) {
    // HLSQ_INVALIDATE_CMD with everything: the six stage bits 0x3f, CS_IBO 0x40, GFX_IBO 0x80, GFX_SHARED_CONST 0x100,
    // CS_BINDLESS 0x1f << 9 = 0x3e00, GFX_BINDLESS 0x1f << 14 = 0x7c000, CS_SHARED_CONST 0x80000: every bit up to 19
    CHECK_EQ(hlsqInvalidateCmdAll(), 0xfffffu);
    // SP_TP_MODE_CNTL: Gallium's 0xa0 is UNK3(0x28) = 0x28 << 2, plus ISAMMODE(ISAMMODE_GL = 2) in bits 0..1
    CHECK_EQ(spTpModeCntl(), 0xa2u);
    // GRAS_SC_CNTL: CCUSINGLECACHELINESIZE(2) in bits 0..2
    CHECK_EQ(grasScCntl(), 0x2u);
    // HLSQ_CONTROL_5_REG: both register ids "none" (0xfc), the second one << 8
    CHECK_EQ(hlsqControl5Reg(), 0xfcfcu);
    // SP_PERFCTR_ENABLE: the six stage bits
    CHECK_EQ(spPerfctrEnableAll(), 0x3fu);
    // RB_CCU_CNTL for rendering to memory on this chip: depth cache at 0, color cache after it at
    // num_ccu (2) * 64 KiB = 0x20000 bytes, in 4 KiB units: 0x20 << 23 = 0x10000000; both cache sizes FULL (0);
    // the chip has fast clear (bit 0 clear) and no concurrent resolve (bit 2 clear)
    CHECK_EQ(rbCcuCntlSysmem(), 0x10000000u);
    CHECK_EQ(A6XX_RB_CCU_CNTL_COLOR_OFFSET(0x20000), 0x10000000u);
    CHECK_EQ(A6XX_RB_CCU_CNTL_DEPTH_OFFSET(0x1000), 0x1000u);
    CHECK_EQ(A6XX_RB_CCU_CNTL_COLOR_OFFSET_HI(1), 0x200u);
    CHECK_EQ(A6XX_RB_CCU_CNTL_DEPTH_CACHE_SIZE(CCU_CACHE_SIZE_QUARTER), 0x800u);
    // CP_SET_DRAW_STATE word 0 as the batch start sends it: only DISABLE_ALL_GROUPS (bit 18)
    CHECK_EQ(setDrawState0(0, CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS, 0), 0x00040000u);
    CHECK_EQ(setDrawState0(5, 0, 3), 0x03000005u);
    // CP_SET_AMBLE word 2: 0x123 dwords of a BIN_PREAMBLE (type 1 << 20)
    CHECK_EQ(CP_SET_AMBLE_2_DWORDS(0x123) | CP_SET_AMBLE_2_TYPE(BIN_PREAMBLE_AMBLE_TYPE), 0x00100123u);
    // the array registers: VFD_FETCH[31].SIZE = 0xa012 + 4 * 31 = 0xa08e; CP_SCRATCH_REG[7] = 0x883 + 7
    CHECK_EQ(vfdFetchSize(0), 0xa012u);
    CHECK_EQ(vfdFetchSize(31), 0xa08eu);
    CHECK_EQ(cpScratchReg(7), 0x88au);
    // the headers of the packets around the static state
    // CP_SET_MODE 0x63 = 110 0011, four ones: bit 23 is 1. count 1: 0.
    CHECK_EQ(pm4::type7(CP_SET_MODE, 1), 0x70e30001u);
    // CP_SET_DRAW_STATE 0x43 = 100 0011, three ones: 0. count 3 = 11, two ones: bit 15 is 1.
    CHECK_EQ(pm4::type7(CP_SET_DRAW_STATE, 3), 0x70438003u);
    // CP_SET_AMBLE 0x55 = 101 0101, four ones: bit 23 is 1. count 3: bit 15 is 1.
    CHECK_EQ(pm4::type7(CP_SET_AMBLE, 3), 0x70d58003u);
    // CP_INDIRECT_BUFFER 0x3f = 011 1111, six ones: bit 23 is 1. count 3: bit 15 is 1.
    CHECK_EQ(pm4::type7(CP_INDIRECT_BUFFER, 3), 0x70bf8003u);
    CHECK_EQ(uint32_t(CP_INDIRECT_BUFFER), uint32_t(pm4::kIndirectBuffer));
    // the tables: Gallium's order, first and last entries, and the values that are not plain zeros
    constexpr size_t n = sizeof(kStaticState) / sizeof(kStaticState[0]);
    CHECK_EQ(n, size_t(53));
    CHECK_EQ(kStaticState[0].reg, 0x8e04u);
    CHECK_EQ(kStaticState[0].value, 0x04100000u);
    CHECK_EQ(kStaticState[1].reg, REG_A6XX_SP_FLOAT_CNTL);
    CHECK_EQ(kStaticState[1].value, 0x8u);
    CHECK_EQ(kStaticState[5].reg, REG_A6XX_TPL1_DBG_ECO_CNTL);
    CHECK_EQ(kStaticState[5].value, 0x05008000u);
    CHECK_EQ(kStaticState[19].reg, REG_A6XX_SP_MODE_CONTROL);
    CHECK_EQ(kStaticState[19].value, 0x5u);
    CHECK_EQ(kStaticState[23].reg, REG_A6XX_PC_MODE_CNTL);
    CHECK_EQ(kStaticState[23].value, 0x1fu);
    CHECK_EQ(kStaticState[49].reg, REG_A6XX_SP_TP_MODE_CNTL);
    CHECK_EQ(kStaticState[49].value, 0xa2u);
    CHECK_EQ(kStaticState[n - 1].reg, REG_A6XX_VFD_MULTIVIEW_CNTL);
    CHECK_EQ(sizeof(kStaticStateAfterDrawState) / sizeof(kStaticStateAfterDrawState[0]), size_t(7));
    CHECK_EQ(kStaticStateAfterDrawState[0].reg, REG_A6XX_VPC_SO_STREAM_CNTL);
    CHECK_EQ(kStaticStateAfterDrawState[6].reg, REG_A6XX_RB_LRZ_CNTL);
    CHECK_EQ(sizeof(kStaticStateTail) / sizeof(kStaticStateTail[0]), size_t(1));
    // every register in the tables is in the A6xx register space, and the magic values are the dev-info decimals
    for (const RegWrite& w : kStaticState) {
        CHECK(w.reg >= 0x0e12 && w.reg <= 0xbe04);
    }
    CHECK_EQ(kMagicTpl1DbgEcoCntl, 83918848u);
    CHECK_EQ(kMagicRbDbgEcoCntl, 68157440u);
    CHECK_EQ(kMagicVpcDbgEcoCntl, 33554432u);
    CHECK_EQ(kMagicSpChickenBits, 5120u);
    CHECK_EQ(kMagicUcheClientPf, 132u);
    CHECK_EQ(kBorderColorBytes, 32768u);
}
