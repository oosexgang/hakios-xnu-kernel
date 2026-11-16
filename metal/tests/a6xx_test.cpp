// The compute dispatch's words against numbers worked out by hand from Mesa's generated headers (a6xx.xml.h,
// adreno_pm4.xml.h; the field positions are in a6xx.hpp next to each constant) and, for the packet headers, from the
// command processor's parity rule as pm4.hpp states it: a field's bit is 1 when the field has an even number of ones.
// tools/check-a6xx.py checks the constants themselves against the headers; this checks what is built from them.
#include <cfloat>
#include <cstring>

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

TEST(a6xx_texture2d_descriptor) {
    // RGBA8Unorm, 64 x 64, at 0x10000000, laid out as fdl6_layout would (pitch = align(64 * 4, 64 * 4) = 256,
    // layer = align(align(64, 4) * 256, 4096) = 16384), as the TP samples it:
    //   dword 0: TILE6_LINEAR 0 | FMT(FMT6_8_8_8_8_UNORM = 48) << 22 = 0x0c000000 | SAMPLES(MSAA_ONE) 0 | SWAP(WZYX) 0
    //            | SWIZ X 0 << 4, Y 1 << 7 = 0x80, Z 2 << 10 = 0x800, W 3 << 13 = 0x6000 (= 0x6880) | MIPLVLS 0
    //   dword 1: WIDTH 64 | HEIGHT 64 << 15 = 0x200000
    //   dword 2: PITCHALIGN log2(4) = 2 | PITCH 256 << 7 = 0x8000 | TYPE(A6XX_TEX_2D = 1) << 29 = 0x20000000
    //   dword 3: ARRAY_PITCH 16384 >> 12 = 4
    //   dword 4: the base, raw;  dword 5: BASE_HI 0 | DEPTH(1) << 17 = 0x20000;  dword 6: MIN_LOD_CLAMP 0
    TexConst d = texture2d(0x10000000ull, 64, 64, 256, 2, FMT6_8_8_8_8_UNORM, WZYX, true, false);
    CHECK_EQ(d.dword[0], 0x0c006880u);
    CHECK_EQ(d.dword[1], 0x00200040u);
    CHECK_EQ(d.dword[2], 0x20008002u);
    CHECK_EQ(d.dword[3], 0x4u);
    CHECK_EQ(d.dword[4], 0x10000000u);
    CHECK_EQ(d.dword[5], 0x00020000u);
    for (int i = 6; i < 16; i++) {
        CHECK_EQ(d.dword[i], 0u);
    }
    // the storage (IBO) view of the same texture: dword 2 without PITCHALIGN, everything else the same (SAMPLES and
    // MIPLVLS are 0 in both for a one-level, one-sample texture)
    TexConst s = image2d(0x10000000ull, 64, 64, 256, FMT6_8_8_8_8_UNORM, WZYX, true, false);
    CHECK_EQ(s.dword[0], 0x0c006880u);
    CHECK_EQ(s.dword[1], 0x00200040u);
    CHECK_EQ(s.dword[2], 0x20008000u);
    CHECK_EQ(s.dword[3], 0x4u);
    CHECK_EQ(s.dword[4], 0x10000000u);
    CHECK_EQ(s.dword[5], 0x00020000u);
    // dword 0 of the other served formats, each from its FMT and swizzle: without alpha SWIZ_W reads ONE (5 << 13 =
    // 0xa000, so the swizzle is 0xa880); BGRA8 is the RGBA8 format with SWAP(WXYZ = 1) << 30; sRGB adds bit 2
    CHECK_EQ(texture2d(0, 1, 1, 64, 0, FMT6_8_UNORM, WZYX, false, false).dword[0], 0x00c0a880u);            // R8: 3 << 22
    CHECK_EQ(texture2d(0, 1, 1, 128, 1, FMT6_8_8_UNORM, WZYX, false, false).dword[0], 0x03c0a880u);         // RG8: 15 << 22
    CHECK_EQ(texture2d(0, 1, 1, 256, 2, FMT6_8_8_8_8_UNORM, WXYZ, true, false).dword[0], 0x4c006880u);     // BGRA8
    CHECK_EQ(texture2d(0, 1, 1, 256, 2, FMT6_8_8_8_8_UNORM, WZYX, true, true).dword[0], 0x0c006884u);      // RGBA8 sRGB
    CHECK_EQ(texture2d(0, 1, 1, 256, 2, FMT6_32_UINT, WZYX, false, false).dword[0], 0x12c0a880u);           // R32Uint: 75 << 22
    CHECK_EQ(texture2d(0, 1, 1, 256, 2, FMT6_32_FLOAT, WZYX, false, false).dword[0], 0x1280a880u);          // R32Float: 74 << 22
    CHECK_EQ(texture2d(0, 1, 1, 512, 3, FMT6_16_16_16_16_FLOAT, WZYX, true, false).dword[0], 0x18806880u);  // RGBA16F: 98 << 22
    CHECK_EQ(texture2d(0, 1, 1, 1024, 4, FMT6_32_32_32_32_FLOAT, WZYX, true, false).dword[0], 0x20806880u); // RGBA32F: 130 << 22
    CHECK_EQ(texture2d(0, 1, 1, 1024, 4, FMT6_32_32_32_32_UINT, WZYX, true, false).dword[0], 0x20c06880u);  // RGBA32Uint: 131 << 22
    // the layer size: 7 rows are counted as 8 and the layer padded to 4 KiB; 64 rows of 256 are exactly 4 pages
    CHECK_EQ(layerBytes2d(7, 512), 4096u);
    CHECK_EQ(layerBytes2d(9, 512), 8192u);
    CHECK_EQ(layerBytes2d(64, 256), 16384u);
    CHECK_EQ(layerBytes2d(32, 256), 8192u);
    // a 49-bit address: the high word's low 17 bits, and the whole low word (fd6_view.c writes it raw)
    TexConst far = texture2d(0x1'2345'6780ull, 1, 1, 64, 0, FMT6_8_UNORM, WZYX, false, false);
    CHECK_EQ(far.dword[4], 0x23456780u);
    CHECK_EQ(far.dword[5], 0x00020001u);
    // the field encoders by themselves
    CHECK_EQ(A6XX_TEX_CONST_0_SWAP(WXYZ), 0x40000000u);
    CHECK_EQ(A6XX_TEX_CONST_0_SAMPLES(MSAA_FOUR), 0x00200000u);
    CHECK_EQ(A6XX_TEX_CONST_2_PITCHALIGN(4), 0x4u);
    CHECK_EQ(A6XX_TEX_CONST_2_PITCHALIGN(16), 0u);   // 4 bits
    CHECK_EQ(A6XX_TEX_CONST_6_MIN_LOD_CLAMP(0x1234), 0x234u);
    CHECK_EQ(uint32_t(FMT6_NONE), 255u);
    CHECK_EQ(uint32_t(FUNC_ALWAYS), 7u);
}

TEST(a6xx_sampler_words) {
    // Metal's default sampler (nearest, clamp to edge on every axis, not mipmapped, normalized, lod 0..FLT_MAX), as
    // Gallium encodes the same PIPE state (fd6_texture.cc:270-303):
    //   dword 0: XY_MAG(NEAREST) 0 | XY_MIN 0 | WRAP_S(CLAMP_TO_EDGE = 1) << 5 = 0x20 | WRAP_T 1 << 8 = 0x100
    //            | WRAP_R 1 << 11 = 0x800 | ANISO_1 0 | LOD_BIAS 0                                       = 0x920
    //   dword 1: MIPFILTER_LINEAR_FAR 0x40 (mip filter NONE) | MIN_LOD 0 | MAX_LOD clamped to 4095 << 8 = 0xfff00
    //                                                                                                        = 0x000fff40
    //   dword 2: REDUCTION_MODE(AVERAGE) 0 | BCOLOR 0;  dword 3: 0
    SamplerFields f;
    f.wrapS = f.wrapT = f.wrapR = A6XX_TEX_CLAMP_TO_EDGE;
    f.mipLinearFar = true;
    TexSamp s = texSamp(f);
    CHECK_EQ(s.dword[0], 0x920u);
    CHECK_EQ(s.dword[1], 0x000fff40u);
    CHECK_EQ(s.dword[2], 0u);
    CHECK_EQ(s.dword[3], 0u);
    // linear min and mag: XY_MAG(LINEAR = 1) << 1 = 2 | XY_MIN 1 << 3 = 8 -> 0x92a, and CHROMA_LINEAR (0x20) in dword 2
    f.mag = f.min = A6XX_TEX_LINEAR;
    f.chromaLinear = true;
    s = texSamp(f);
    CHECK_EQ(s.dword[0], 0x92au);
    CHECK_EQ(s.dword[1], 0x000fff40u);
    CHECK_EQ(s.dword[2], 0x20u);
    // coord::pixel: UNNORM_COORDS is bit 5 of dword 1
    f.unnormCoords = true;
    CHECK_EQ(texSamp(f).dword[1], 0x000fff60u);
    // the fields alone: repeat is 0 on every axis; a border clamp on S alone is 3 << 5; LOD clamps at 1.5 are 0x180
    CHECK_EQ(texSamp(SamplerFields{}).dword[0], 0u);
    CHECK_EQ(texSamp(SamplerFields{}).dword[1], 0x000fff00u);
    CHECK_EQ(A6XX_TEX_SAMP_0_WRAP_S(A6XX_TEX_CLAMP_TO_BORDER), 0x60u);
    CHECK_EQ(A6XX_TEX_SAMP_0_WRAP_T(A6XX_TEX_MIRROR_REPEAT), 0x200u);
    CHECK_EQ(A6XX_TEX_SAMP_0_WRAP_R(A6XX_TEX_MIRROR_CLAMP), 0x2000u);
    CHECK_EQ(A6XX_TEX_SAMP_0_ANISO(A6XX_TEX_ANISO_16), 0x10000u);
    CHECK_EQ(A6XX_TEX_SAMP_0_LOD_BIAS(-256), 0xf8000000u);    // -1.0 in 5.8 is 0x1f00 in 13 bits: the five integer bits set, << 19
    CHECK_EQ(A6XX_TEX_SAMP_0_LOD_BIAS(256), 0x08000000u);     // +1.0: 0x100 << 19
    CHECK_EQ(A6XX_TEX_SAMP_1_MIN_LOD(0x180), 0x18000000u);
    CHECK_EQ(A6XX_TEX_SAMP_1_MAX_LOD(0x180), 0x18000u);
    CHECK_EQ(A6XX_TEX_SAMP_1_MAX_LOD(0x1000), 0u);            // 12 bits: 16.0 does not fit, the caller clamps first
    CHECK_EQ(A6XX_TEX_SAMP_1_COMPARE_FUNC(FUNC_LESS), 0x2u);
    CHECK_EQ(A6XX_TEX_SAMP_2_BCOLOR(1), 0x80u);
    CHECK_EQ(A6XX_TEX_SAMP_2_REDUCTION_MODE(A6XX_REDUCTION_MODE_MAX), 0x2u);
    f = SamplerFields{};
    f.compare = true;
    f.compareFunc = FUNC_LEQUAL;
    CHECK_EQ(texSamp(f).dword[1], 0x000fff06u);
    f.compare = false;   // the function is written only with compare on, as Gallium's compare_mode gates it
    CHECK_EQ(texSamp(f).dword[1], 0x000fff00u);
    CHECK_EQ(kLodMaxFixed, 4095u);
    CHECK_EQ(kLodFractionBits, 8u);
    CHECK_EQ(sizeof(TexSamp), size_t(16));
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
    // the tail: fd6's conservative-ras zero, then turnip's RB_ALPHA_CONTROL 0 (the render contract's batch start)
    CHECK_EQ(sizeof(kStaticStateTail) / sizeof(kStaticStateTail[0]), size_t(2));
    CHECK_EQ(kStaticStateTail[0].reg, REG_A6XX_PC_DGEN_SU_CONSERVATIVE_RAS_CNTL);
    CHECK_EQ(kStaticStateTail[1].reg, 0x8864u);
    CHECK_EQ(kStaticStateTail[1].reg, REG_A6XX_RB_ALPHA_CONTROL);
    CHECK_EQ(kStaticStateTail[1].value, 0u);
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

// --- the render path ----------------------------------------------------------------------------------------------------
// The words of the first draw (critic.md's recommended first step: one flat triangle into a 64x64 linear RGBA8Unorm
// texture, pitch 256, in sysmem mode), each worked out by hand from the field positions above it in a6xx.hpp, and
// beside each a value a wrong shift, scale or sign would give, so that the check can fail.

TEST(a6xx_render_opcodes_events_and_enums) {
    CHECK_EQ(uint32_t(CP_DRAW_INDX_OFFSET), 56u);
    CHECK_EQ(uint32_t(CP_SKIP_IB2_ENABLE_GLOBAL), 29u);
    CHECK_EQ(uint32_t(CP_SKIP_IB2_ENABLE_LOCAL), 35u);
    CHECK_EQ(uint32_t(CP_SET_VISIBILITY_OVERRIDE), 100u);
    CHECK_EQ(uint32_t(CP_CONTEXT_REG_BUNCH), 92u);
    CHECK_EQ(uint32_t(CP_WAIT_MEM_WRITES), 18u);
    CHECK_EQ(uint32_t(CP_WAIT_FOR_ME), 19u);
    // the type-7 headers, by pm4.hpp's parity rule (a field's bit is 1 when the field has an even number of ones):
    // CP_DRAW_INDX_OFFSET 0x38 = 011 1000, three ones: bit 23 is 0; count 3 = 11, two ones: bit 15 is 1
    CHECK_EQ(pm4::type7(CP_DRAW_INDX_OFFSET, 3), 0x70388003u);
    // CP_SKIP_IB2_ENABLE_GLOBAL 0x1d = 001 1101, four ones: bit 23 is 1; count 1: 0
    CHECK_EQ(pm4::type7(CP_SKIP_IB2_ENABLE_GLOBAL, 1), 0x709d0001u);
    // CP_SET_VISIBILITY_OVERRIDE 0x64 = 110 0100, three ones: 0
    CHECK_EQ(pm4::type7(CP_SET_VISIBILITY_OVERRIDE, 1), 0x70640001u);
    // CP_SKIP_IB2_ENABLE_LOCAL 0x23 = 010 0011, three ones: 0
    CHECK_EQ(pm4::type7(CP_SKIP_IB2_ENABLE_LOCAL, 1), 0x70230001u);
    // CP_CONTEXT_REG_BUNCH 0x5c = 101 1100, four ones: bit 23 is 1; count 2: one bit, 0
    CHECK_EQ(pm4::type7(CP_CONTEXT_REG_BUNCH, 2), 0x70dc0002u);
    // the events the pass begin and tail write (1-word: the invalidates and LRZ_FLUSH; 4-word: the two flushes)
    CHECK_EQ(uint32_t(PC_CCU_FLUSH_COLOR_TS), 29u);
    CHECK_EQ(uint32_t(PC_CCU_FLUSH_DEPTH_TS), 28u);
    CHECK_EQ(uint32_t(PC_CCU_INVALIDATE_COLOR), 25u);
    CHECK_EQ(uint32_t(PC_CCU_INVALIDATE_DEPTH), 24u);
    CHECK_EQ(uint32_t(LRZ_FLUSH), 38u);
    CHECK_EQ(uint32_t(LRZ_CLEAR), 37u);
    CHECK_EQ(eventWrite0(PC_CCU_FLUSH_COLOR_TS), 0x1du);   // no TIMESTAMP bit: the 28 October lesson holds for the render tail too
    CHECK_EQ(eventWrite0(LRZ_FLUSH), 0x26u);
    // the marker and the state blocks of the two stages' loads
    CHECK_EQ(uint32_t(RM6_DIRECT_RENDER), 1u);
    CHECK_EQ(setMarker0(RM6_DIRECT_RENDER), 0x1u);
    CHECK_EQ(uint32_t(SB6_VS_SHADER), 8u);
    CHECK_EQ(uint32_t(SB6_FS_SHADER), 12u);
    CHECK_EQ(uint32_t(SB6_VS_TEX), 0u);
    CHECK_EQ(uint32_t(SB6_FS_TEX), 4u);
    CHECK_EQ(uint32_t(SB6_IBO), 14u);
    // the VS's program load: ST6_SHADER (0) | SS6_INDIRECT (2) << 16 | SB6_VS_SHADER (8) << 18 = 0x200000 | NUM_UNIT 1 << 22
    CHECK_EQ(loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_VS_SHADER, 1), 0x00620000u);
    // the FS's: SB6_FS_SHADER (12) << 18 = 0x300000
    CHECK_EQ(loadState6_0(0, ST6_SHADER, SS6_INDIRECT, SB6_FS_SHADER, 1), 0x00720000u);
    // the draw enums
    CHECK_EQ(uint32_t(DI_PT_TRILIST), 4u);
    CHECK_EQ(uint32_t(DI_PT_TRISTRIP), 6u);
    CHECK_EQ(uint32_t(DI_PT_POINTLIST), 9u);
    CHECK_EQ(uint32_t(DI_PT_LINELIST), 2u);
    CHECK_EQ(uint32_t(DI_PT_LINESTRIP), 3u);
    CHECK_EQ(uint32_t(DI_SRC_SEL_AUTO_INDEX), 2u);
    CHECK_EQ(uint32_t(DI_SRC_SEL_DMA), 0u);
    CHECK_EQ(uint32_t(IGNORE_VISIBILITY), 0u);
    CHECK_EQ(uint32_t(USE_VISIBILITY), 1u);
    // the register enums
    CHECK_EQ(uint32_t(RENDERING_PASS), 0u);
    CHECK_EQ(uint32_t(BUFFERS_IN_SYSMEM), 3u);
    CHECK_EQ(uint32_t(BUFFERS_IN_GMEM), 0u);
    CHECK_EQ(uint32_t(LRZ_FEEDBACK_EARLY_Z_OR_EARLY_LRZ_LATE_Z), 3u);
    CHECK_EQ(uint32_t(DEPTH6_NONE), 0u);
    CHECK_EQ(uint32_t(DEPTH6_32), 4u);
    CHECK_EQ(uint32_t(POLYMODE6_TRIANGLES), 3u);
    CHECK_EQ(uint32_t(INTERP_SMOOTH), 0u);
    CHECK_EQ(uint32_t(INTERP_FLAT), 1u);
    CHECK_EQ(uint32_t(INTERP_ONE), 3u);
    CHECK_EQ(uint32_t(PS_REPL_NONE), 0u);
    CHECK_EQ(uint32_t(A6XX_EARLY_Z), 0u);
    CHECK_EQ(uint32_t(A6XX_LATE_Z), 1u);
    CHECK_EQ(uint32_t(ROP_CLEAR), 0u);
    CHECK_EQ(uint32_t(ROP_COPY), 12u);
    CHECK_EQ(uint32_t(NO_FLUSH), 0u);
    CHECK_EQ(uint32_t(FRAGCOORD_CENTER), 0u);
    CHECK_EQ(uint32_t(RECTANGULAR), 1u);
    CHECK_EQ(uint32_t(BRESENHAM), 0u);
    CHECK_EQ(uint32_t(FMT6_32_32_FLOAT), 103u);
    CHECK_EQ(uint32_t(FACTOR_ONE_MINUS_SRC_ALPHA), 7u);
    CHECK_EQ(uint32_t(BLEND_MAX_DST_SRC), 4u);
    CHECK_EQ(uint32_t(STENCIL_DECR_WRAP), 7u);
    CHECK_EQ(uint32_t(IJ_LINEAR_CENTROID), 5u);   // ir3_bary: LINEAR_CENTROID comes before LINEAR_SAMPLE
    CHECK_EQ(uint32_t(IJ_LINEAR_SAMPLE), 6u);
    CHECK_EQ(uint32_t(IJ_COUNT), 7u);
}

TEST(a6xx_render_register_offsets_are_mesas) {
    CHECK_EQ(REG_A6XX_GRAS_CL_CNTL, 0x8000u);
    CHECK_EQ(REG_A6XX_GRAS_CL_VPORT_XOFFSET, 0x8010u);
    CHECK_EQ(REG_A6XX_GRAS_CL_VPORT_YSCALE, 0x8013u);
    CHECK_EQ(REG_A6XX_GRAS_SU_CNTL, 0x8090u);
    CHECK_EQ(REG_A6XX_GRAS_BIN_CONTROL, 0x80a1u);
    CHECK_EQ(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL, 0x80f0u);
    CHECK_EQ(REG_A6XX_GRAS_SC_WINDOW_SCISSOR_BR, 0x80f1u);
    CHECK_EQ(REG_A6XX_RB_BIN_CONTROL, 0x8800u);
    CHECK_EQ(REG_A6XX_RB_RENDER_CNTL, 0x8801u);
    CHECK_EQ(REG_A6XX_RB_MRT_CONTROL, 0x8820u);
    CHECK_EQ(REG_A6XX_RB_MRT_BUF_INFO, 0x8822u);
    CHECK_EQ(REG_A6XX_RB_MRT_BASE_GMEM, 0x8827u);
    CHECK_EQ(REG_A6XX_RB_MRT_FLAG_BUFFER_ADDR, 0x8903u);
    CHECK_EQ(REG_A6XX_RB_DEPTH_BUFFER_INFO, 0x8872u);
    CHECK_EQ(REG_A6XX_RB_BLEND_CNTL, 0x8865u);
    CHECK_EQ(REG_A6XX_RB_WINDOW_OFFSET, 0x8890u);
    CHECK_EQ(REG_A6XX_RB_WINDOW_OFFSET2, 0x88d4u);
    CHECK_EQ(REG_A6XX_PC_PRIMITIVE_CNTL_0, 0x9b00u);
    CHECK_EQ(REG_A6XX_PC_VS_OUT_CNTL, 0x9b01u);
    CHECK_EQ(REG_A6XX_PC_RESTART_INDEX, 0x9803u);
    CHECK_EQ(REG_A6XX_VFD_CONTROL_0, 0xa000u);
    CHECK_EQ(REG_A6XX_VFD_CONTROL_6, 0xa006u);
    CHECK_EQ(REG_A6XX_VFD_INDEX_OFFSET, 0xa00eu);
    CHECK_EQ(REG_A6XX_VPC_VS_PACK, 0x9301u);
    CHECK_EQ(REG_A6XX_VPC_CNTL_0, 0x9304u);
    CHECK_EQ(REG_A6XX_VPC_VS_CLIP_CNTL_V2, 0x9311u);
    CHECK_EQ(REG_A6XX_VPC_VS_LAYER_CNTL_V2, 0x9314u);
    CHECK_EQ(REG_A6XX_SP_VS_CTRL_REG0, 0xa800u);
    CHECK_EQ(REG_A6XX_SP_VS_OUT_REG, 0xa803u);
    CHECK_EQ(REG_A6XX_SP_VS_VPC_DST_REG, 0xa813u);
    CHECK_EQ(REG_A6XX_SP_VS_OBJ_FIRST_EXEC_OFFSET, 0xa81bu);
    CHECK_EQ(REG_A6XX_SP_VS_OBJ_START, 0xa81cu);
    CHECK_EQ(REG_A6XX_SP_VS_CONFIG, 0xa823u);
    CHECK_EQ(REG_A6XX_SP_FS_OBJ_FIRST_EXEC_OFFSET, 0xa982u);
    CHECK_EQ(REG_A6XX_SP_FS_OUTPUT_REG, 0xa98eu);
    CHECK_EQ(REG_A6XX_SP_FS_MRT_REG, 0xa996u);
    CHECK_EQ(REG_A6XX_SP_FS_PREFETCH_CNTL, 0xa99eu);
    CHECK_EQ(REG_A6XX_SP_FS_CONFIG, 0xab04u);
    CHECK_EQ(REG_A6XX_SP_IBO, 0xab1au);
    CHECK_EQ(REG_A6XX_SP_WINDOW_OFFSET, 0xb4d1u);
    CHECK_EQ(REG_A6XX_SP_TP_WINDOW_OFFSET, 0xb307u);
    CHECK_EQ(REG_A6XX_HLSQ_VS_CNTL, 0xb800u);
    CHECK_EQ(REG_A6XX_HLSQ_FS_CNTL, 0xbb10u);
    CHECK_EQ(REG_A6XX_HLSQ_CONTROL_1_REG, 0xb982u);
    CHECK_EQ(REG_A6XX_HLSQ_CONTROL_4_REG, 0xb985u);
    // the array entries: RB_MRT[1].PITCH = 0x8823 + 8; GRAS_CL_VPORT[1].YSCALE = 0x8013 + 6; SP_VS_OUT_REG[3] = 0xa803 + 3;
    // VPC_VARYING_PS_REPL_MODE[7] = 0x9208 + 7; VFD_DECODE[2].INSTR = 0xa090 + 4; VFD_FETCH[1].STRIDE = 0xa013 + 4
    CHECK_EQ(REG_A6XX_RB_MRT__STRIDE, 8u);
    CHECK_EQ(regRbMrt(REG_A6XX_RB_MRT_PITCH, 1), 0x882bu);
    CHECK_EQ(regRbMrt(REG_A6XX_RB_MRT_BUF_INFO, 7), 0x885au);
    CHECK_EQ(regRbMrtFlagBuffer(REG_A6XX_RB_MRT_FLAG_BUFFER_PITCH, 1), 0x8908u);
    CHECK_EQ(regGrasClVport(REG_A6XX_GRAS_CL_VPORT_YSCALE, 1), 0x8019u);
    CHECK_EQ(regGrasClZClamp(REG_A6XX_GRAS_CL_Z_CLAMP_MAX, 1), 0x8073u);
    CHECK_EQ(regGrasScScreenScissor(REG_A6XX_GRAS_SC_SCREEN_SCISSOR_BR, 1), 0x80b3u);
    CHECK_EQ(regGrasScViewportScissor(REG_A6XX_GRAS_SC_VIEWPORT_SCISSOR_TL, 2), 0x80d4u);
    CHECK_EQ(regSpVsOutReg(3), 0xa806u);
    CHECK_EQ(regSpVsVpcDstReg(1), 0xa814u);
    CHECK_EQ(regSpFsOutputReg(7), 0xa995u);
    CHECK_EQ(regSpFsMrtReg(7), 0xa99du);
    CHECK_EQ(regVpcVaryingInterpMode(7), 0x9207u);
    CHECK_EQ(regVpcVaryingPsReplMode(7), 0x920fu);
    CHECK_EQ(regVpcVarDisable(3), 0x9215u);
    CHECK_EQ(regVfdDecodeInstr(2), 0xa094u);
    CHECK_EQ(regVfdDecodeStepRate(2), 0xa095u);
    CHECK_EQ(regVfdDestCntlInstr(5), 0xa0d5u);
    CHECK_EQ(regVfdFetchBase(1), 0xa014u);
    CHECK_EQ(regVfdFetchStride(1), 0xa017u);
    CHECK_EQ(kMrtCount, 8u);
    CHECK_EQ(kVpcVaryingRegs, 8u);
    CHECK_EQ(kVpcVarDisableRegs, 4u);
    CHECK_EQ(kViewportWords, REG_A6XX_GRAS_CL_VPORT__STRIDE);
    // the facts the refusals read
    CHECK_EQ(kPrimAllocThreshold, 7u);
    CHECK_EQ(kMaxConstPipeline, 512u);
    CHECK_EQ(kVsConstlenFloor, 8u);
    CHECK_EQ(kProgramAlign, 128u);
    CHECK_EQ(kMrtBaseAlign, 64u);
    CHECK_EQ(kMrtPitchAlign, 64u);
    CHECK_EQ(kLocNone, 0xffu);
}

TEST(a6xx_fui_is_the_floats_bits) {
    // the constexpr float-to-bits against memcpy, for the values the viewport and clamps use and the awkward ones
    const float values[] = {0.0f, 1.0f, -1.0f, 32.0f, -32.0f, 0.5f, 3.14159f, 1e-40f, FLT_MIN, FLT_MAX, 65535.0f, 1.0e-38f, 2.0f, 1.5f,
                            -0.75f, 1023.99f, 16777217.0f, 1.17549435e-38f, 7.0e-45f};
    for (float f : values) {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        CHECK_EQ(fui(f), bits);
    }
    // the signed zero, NaN and the infinities
    uint32_t bits = 0;
    const float negZero = -0.0f;
    std::memcpy(&bits, &negZero, 4);
    CHECK_EQ(fui(negZero), bits);
    CHECK_EQ(fui(negZero), 0x80000000u);
    CHECK_EQ(fui(__builtin_nanf("")), 0x7fc00000u);
    CHECK_EQ(fui(__builtin_inff()), 0x7f800000u);
    CHECK_EQ(fui(-__builtin_inff()), 0xff800000u);
    static_assert(fui(32.0f) == 0x42000000u, "32.0 is 0x42000000");
    static_assert(fui(-32.0f) == 0xc2000000u, "-32.0 is 0xc2000000");
    static_assert(fui(1.0f) == 0x3f800000u, "1.0 is 0x3f800000");
    // the twin: the sign is a different word, so a viewport scale of the wrong sign cannot pass as the right one
    CHECK(fui(32.0f) != fui(-32.0f));
    CHECK_EQ(fui(32.0f) ^ fui(-32.0f), 0x80000000u);
}

TEST(a6xx_first_draw_target_words) {
    // 64x64 RGBA8Unorm, linear, pitch 256 (align(64 * 4, 64 * 4)), layer align(64 * 256, 4096) = 16384, at 0x1'2000'0000:
    //   RB_MRT_BUF_INFO    COLOR_FORMAT(FMT6_8_8_8_8_UNORM = 48) | COLOR_TILE_MODE(LINEAR = 0) << 8 | COLOR_SWAP(WZYX = 0) << 13 = 0x30
    //   RB_MRT_PITCH       256 >> 6 = 4;  RB_MRT_ARRAY_PITCH 16384 >> 6 = 0x100
    //   SP_FS_MRT_REG      COLOR_FORMAT 48 = 0x30;  GRAS_LRZ_MRT_BUF_INFO_0 the same
    CHECK_EQ(rbMrtBufInfo(FMT6_8_8_8_8_UNORM, TILE6_LINEAR, WZYX), 0x30u);
    CHECK_EQ(rbMrtPitch(256), 4u);
    CHECK_EQ(rbMrtArrayPitch(16384), 0x100u);
    CHECK_EQ(spFsMrtReg(FMT6_8_8_8_8_UNORM, false, false), 0x30u);
    CHECK_EQ(A6XX_GRAS_LRZ_MRT_BUF_INFO_0_COLOR_FORMAT(FMT6_8_8_8_8_UNORM), 0x30u);
    // the twins: the pitch written in bytes would be 256, which the field does not hold; a BGRA target swaps
    // (WXYZ = 1 << 13 = 0x2000); an integer target sets UINT (0x200) in the SP's copy only
    CHECK(rbMrtPitch(256) != 256u);
    CHECK_EQ(A6XX_RB_MRT_PITCH__SCALE, 6u);
    CHECK_EQ(rbMrtBufInfo(FMT6_8_8_8_8_UNORM, TILE6_LINEAR, WXYZ), 0x2030u);
    CHECK_EQ(spFsMrtReg(FMT6_32_UINT, false, true), 0x24bu);
    CHECK_EQ(spFsMrtReg(FMT6_32_UINT, true, false), 0x14bu);
    // the pitch precondition: a multiple of 64 bytes within 16 bits of units (a6xx.xml:3702 high=15)
    CHECK(rbMrtPitchValid(256));
    CHECK(rbMrtPitchValid(64));
    CHECK(rbMrtPitchValid(0xffffu << 6));
    CHECK(!rbMrtPitchValid(0));
    CHECK(!rbMrtPitchValid(200));
    CHECK(!rbMrtPitchValid(0x10000u << 6));
    CHECK_EQ(kMrtPitchMax, 0x3fffc0u);
    // the depth attachment that is not there, and the flag buffer pitch's two scales
    CHECK_EQ(A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_NONE), 0u);
    CHECK_EQ(A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_32), 4u);
    CHECK_EQ(A6XX_RB_DEPTH_BUFFER_PITCH(256), 4u);
    CHECK_EQ(A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH(64), 1u);
    CHECK_EQ(A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH(128), 1u << 11);
    // the components and the target count, SP and RB alike
    CHECK_EQ(renderComponents(1), 0xfu);
    CHECK_EQ(renderComponents(2), 0xffu);
    CHECK_EQ(renderComponents(8), 0xffffffffu);
    CHECK_EQ(renderComponents(0), 0u);
    CHECK_EQ(A6XX_SP_FS_RENDER_COMPONENTS_RT0(0xf), 0xfu);
    CHECK_EQ(A6XX_RB_RENDER_COMPONENTS_RT1(0xf), 0xf0u);
    CHECK_EQ(A6XX_SP_FS_OUTPUT_CNTL1_MRT(1), 1u);
    CHECK_EQ(A6XX_RB_FS_OUTPUT_CNTL1_MRT(1), 1u);
    // RB_RENDER_CNTL 0x10 and GRAS_SC_CNTL 2 for a sysmem pass (r3d writes RB_RENDER_CNTL 0: render::Options::renderCntlZero)
    CHECK_EQ(rbRenderCntlSysmem(), 0x10u);
    CHECK_EQ(grasScCntlSysmem(), 0x2u);
    CHECK_EQ(A6XX_GRAS_SC_CNTL_SINGLE_PRIM_MODE(FLUSH_PER_OVERLAP), 0x18u);
    // MSAA off: MSAA_DISABLE is bit 2 of the three DEST registers
    CHECK_EQ(A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE, 0x4u);
    CHECK_EQ(A6XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE, 0x4u);
    CHECK_EQ(A6XX_SP_TP_DEST_MSAA_CNTL_MSAA_DISABLE, 0x4u);
    CHECK_EQ(A6XX_RB_RAS_MSAA_CNTL_SAMPLES(MSAA_FOUR), 0x2u);
    CHECK_EQ(A6XX_RB_BLIT_GMEM_MSAA_CNTL_SAMPLES(MSAA_FOUR), 0x10u);
}

TEST(a6xx_first_draw_pass_and_viewport_words) {
    // the scissors of a 64x64 target: BR is inclusive, X(63) | Y(63) << 16 = 0x003f003f in both the 14-bit (window)
    // and the 16-bit (screen, viewport) forms; the window offsets are regXY(0, 0) = 0
    CHECK_EQ(regXY(63, 63), 0x003f003fu);
    CHECK_EQ(scissorXY(63, 63), 0x003f003fu);
    CHECK_EQ(regXY(0, 0), 0u);
    // the twins: the 14-bit form drops bit 14, the 16-bit form keeps it; y lands at 16, not 14
    CHECK_EQ(regXY(0x4000, 0), 0u);
    CHECK_EQ(scissorXY(0x4000, 0), 0x4000u);
    CHECK_EQ(regXY(0, 1), 0x10000u);
    CHECK_EQ(scissorXY(0xffff, 0xffff), 0xffffffffu);
    CHECK_EQ(regXY(0x3fff, 0x3fff), 0x3fff3fffu);
    // GRAS/RB_BIN_CONTROL for a sysmem pass: BUFFERS_LOCATION(3) << 22 = 0x00c00000; turnip's LRZ_FEEDBACK_ZMODE_MASK(3) << 24
    // adds 0x03000000 (render::Options::binControlLrzFeedback)
    CHECK_EQ(binControlSysmem(false), 0x00c00000u);
    CHECK_EQ(binControlSysmem(true), 0x03c00000u);
    CHECK_EQ(binControlSysmem(false), A6XX_RB_BIN_CONTROL_BUFFERS_LOCATION(BUFFERS_IN_SYSMEM));
    // a binning pass with 32x16 bins, for the twin: BINW(32) = 32 >> 5 = 1, BINH(16) = 16 >> 4 = 1 << 8, RENDER_MODE(1) << 18
    CHECK_EQ(binControl(BINNING_PASS, BUFFERS_IN_GMEM, LRZ_FEEDBACK_NONE, 32, 16), 0x00040101u);
    CHECK_EQ(A6XX_GRAS_BIN_CONTROL_BINW__SCALE, 5u);
    CHECK_EQ(A6XX_GRAS_BIN_CONTROL_BINH__SCALE, 4u);
    CHECK_EQ(A6XX_RB_BIN_CONTROL2_BINW(64), 2u);
    // the viewport {0, 0, 64, 64, 0, 1}: XOFFSET 32.0 = 0x42000000, XSCALE 32.0, YOFFSET 32.0, YSCALE -32.0 = 0xc2000000
    // (Metal: NDC y up, row 0 at the top), ZOFFSET 0.0, ZSCALE 1.0 = 0x3f800000
    ViewportWords vp = viewportWords(0, 0, 64, 64, 0, 1);
    CHECK_EQ(vp.xoffset, 0x42000000u);
    CHECK_EQ(vp.xscale, 0x42000000u);
    CHECK_EQ(vp.yoffset, 0x42000000u);
    CHECK_EQ(vp.yscale, 0xc2000000u);
    CHECK_EQ(vp.zoffset, 0u);
    CHECK_EQ(vp.zscale, 0x3f800000u);
    // the twin: Vulkan's +32.0 would be 0x42000000 in YSCALE, which is what a dropped sign gives
    CHECK(vp.yscale != 0x42000000u);
    CHECK_EQ(vp.yscale, fui(-32.0f));
    // a half-width viewport at x = 32 (the probe's 9c control): XOFFSET 32 + 16 = 48.0 = 0x42400000, XSCALE 16.0 = 0x41800000;
    // a depth range [0.25, 0.75]: ZOFFSET 0.25 = 0x3e800000, ZSCALE 0.5 = 0x3f000000
    ViewportWords half = viewportWords(32, 0, 32, 64, 0.25, 0.75);
    CHECK_EQ(half.xoffset, 0x42400000u);
    CHECK_EQ(half.xscale, 0x41800000u);
    CHECK_EQ(half.yscale, 0xc2000000u);
    CHECK_EQ(half.zoffset, 0x3e800000u);
    CHECK_EQ(half.zscale, 0x3f000000u);
    // the guardband of the 64x64 viewport: gb_adj = min(1025, 1022.97) needs exponent 10 > 8, so MAX_GB in both axes:
    // HORZ(0x1ff) | VERT(0x1ff) << 10 = 0x0007fdff
    CHECK_EQ(calcGuardband(32, 32), 0x1ffu);
    CHECK_EQ(guardbandClipAdj(32, 32, 32, 32), 0x0007fdffu);
    CHECK_EQ(guardbandClipAdj(32, 32, 32, -32), 0x0007fdffu);   // the scale's sign does not matter (fabsf)
    // the twins, by fd_calc_guardband's own arithmetic: offset = scale = 4096 gives gb_adj = min(9, 6.99976) = 6.99976 =
    // 0.87497 * 2^3: (3 - 1) << 6 | (trunc(0.87497 * 128) = 111) - 64 = 0xaf; 256 -> 0x1be, 512 -> 0x17d, 1024 -> 0x13b;
    // a viewport the guardband cannot hold (16384, 16384: gb_adj 0.9999 < 1) and a zero scale are MAX_GB
    CHECK_EQ(calcGuardband(4096, 4096), 0xafu);
    CHECK_EQ(calcGuardband(256, 256), 0x1beu);
    CHECK_EQ(calcGuardband(512, 512), 0x17du);
    CHECK_EQ(calcGuardband(1024, 1024), 0x13bu);
    CHECK_EQ(calcGuardband(16384, 16384), 0x1ffu);
    CHECK_EQ(calcGuardband(0, 0), 0x1ffu);
    CHECK_EQ(guardbandClipAdj(4096, 4096, 256, 256), 0x0006f8afu);
    CHECK_EQ(kMaxGuardband, 0x1ffu);
    // the depth clamp and bounds words are plain floats: 0.0 and 1.0
    CHECK_EQ(fui(0.0f), 0u);
    CHECK_EQ(fui(1.0f), 0x3f800000u);
}

TEST(a6xx_first_draw_program_words) {
    // HLSQ_xS_CNTL: CONSTLEN in units of 4 vec4 | ENABLED (0x100). The VS has ir3's floor of 8: 8 >> 2 = 2 -> 0x102; an FS
    // with no constants: 0x100; with 4: 0x101; 6 is rounded up to 8 first
    CHECK_EQ(hlsqVsCntl(8), 0x102u);
    CHECK_EQ(hlsqFsCntl(0), 0x100u);
    CHECK_EQ(hlsqFsCntl(4), 0x101u);
    CHECK_EQ(hlsqVsCntl(6), 0x102u);
    CHECK_EQ(hlsqFsCntl(5), 0x102u);
    // the twin: the field holds the length >> 2, not the length (8 would be 0x108)
    CHECK(hlsqVsCntl(8) != 0x108u);
    CHECK_EQ(A6XX_HLSQ_VS_CNTL_CONSTLEN__SCALE, 2u);
    CHECK_EQ(A6XX_HLSQ_FS_CNTL_CONSTLEN__SCALE, 2u);
    // SP_xS_CONFIG with no textures, samplers or IBOs: ENABLED = 0x100; with one texture and sampler: | 1 << 9 | 1 << 17
    CHECK_EQ(spXsConfig(0, 0, 0), 0x100u);
    CHECK_EQ(spXsConfig(1, 1, 0), 0x00020300u);
    CHECK_EQ(spXsConfig(0, 0, 2), 0x00800100u);
    CHECK_EQ(spXsConfig(0, 0, 2), spCsConfig(2, 0, 0));
    // SP_VS_CTRL_REG0 for max_reg 1 (2 full registers), max_half_reg -1 (0 half), no branches, merged registers:
    // FULLREGFOOTPRINT(2) << 7 = 0x100 | MERGEDREGS bit 20 = 0x00100100
    CHECK_EQ(spVsCtrlReg0(2, 0, 0), 0x00100100u);
    CHECK_EQ(spVsCtrlReg0(2, 0, 0, false), 0x100u);
    CHECK_EQ(spVsCtrlReg0(2, 1, 3), 0x0010c102u);
    // the twin: the VS's MERGEDREGS is NOT the FS's bit 31
    CHECK_EQ(A6XX_SP_VS_CTRL_REG0_MERGEDREGS, 0x00100000u);
    CHECK_EQ(A6XX_SP_FS_CTRL_REG0_MERGEDREGS, 0x80000000u);
    CHECK(spVsCtrlReg0(2, 0, 0) != (0x100u | 0x80000000u));
    // SP_FS_CTRL_REG0 for max_reg 0 (1 full), no half, no branches, THREAD128, no varyings: FULLREGFOOTPRINT(1) << 7 = 0x80
    // | THREADSIZE(1) << 20 = 0x100000 | UNK24 0x1000000 | MERGEDREGS 0x80000000 = 0x81100080; with a varying | 0x400000
    CHECK_EQ(spFsCtrlReg0(1, 0, 0, THREAD128, false, false, false), 0x81100080u);
    CHECK_EQ(spFsCtrlReg0(1, 0, 0, THREAD128, true, false, false), 0x81500080u);
    CHECK_EQ(spFsCtrlReg0(1, 0, 0, THREAD64, false, true, true), 0x85800080u);
    CHECK_EQ(A6XX_SP_FS_CTRL_REG0_UNK24, 0x01000000u);
    // ir3_shader_branchstack_hw: ceil(min(n, 64) / 2)
    CHECK_EQ(branchstackHw(0), 0u);
    CHECK_EQ(branchstackHw(1), 1u);
    CHECK_EQ(branchstackHw(3), 2u);
    CHECK_EQ(branchstackHw(64), 32u);
    CHECK_EQ(branchstackHw(100), 32u);
    // HLSQ_FS_CNTL_0: THREADSIZE | VARYINGS (bit 1)
    CHECK_EQ(hlsqFsCntl0(THREAD128, false), 0x1u);
    CHECK_EQ(hlsqFsCntl0(THREAD128, true), 0x3u);
    CHECK_EQ(hlsqFsCntl0(THREAD64, false), 0u);
    // the stage loads: NUM_UNIT is capped at the instruction cache, as compute's is
    CHECK_EQ(CP_LOAD_STATE6_0_NUM_UNIT(kInstrCacheSize), 127u << 22);
    // the private memory words are the compute ones' twins (same scales)
    CHECK_EQ(A6XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM(512), 1u);
    CHECK_EQ(A6XX_SP_FS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(0x1000), 1u);
    CHECK_EQ(A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET_OFFSET(0x800), 1u);
}

TEST(a6xx_first_draw_link_and_vfd_words) {
    // VFD with no attributes: VFD_CONTROL_0 = 0; VFD_CONTROL_1 with the vertex id in r0.x (regid 0) and the other three
    // registers "none": 0 | 0xfc << 8 | 0xfc << 16 | 0xfc << 24 = 0xfcfcfc00; the HS/DS/GS registers all "none"
    CHECK_EQ(A6XX_VFD_CONTROL_0_FETCH_CNT(0) | A6XX_VFD_CONTROL_0_DECODE_CNT(0), 0u);
    CHECK_EQ(A6XX_VFD_CONTROL_0_FETCH_CNT(1) | A6XX_VFD_CONTROL_0_DECODE_CNT(1), 0x101u);
    CHECK_EQ(vfdControl1(0, kRegidNone, kRegidNone, kRegidNone), 0xfcfcfc00u);
    CHECK_EQ(vfdControl1(4, 8, kRegidNone, kRegidNone), 0xfcfc0804u);
    CHECK_EQ(vfdControl2None(), 0xfcfcu);
    CHECK_EQ(vfdControl3None(), 0xfcfcfcfcu);
    CHECK_EQ(vfdControl4None(), 0xfcu);
    CHECK_EQ(vfdControl5None(), 0xfcfcu);
    // the twin: a regid in the wrong byte
    CHECK(vfdControl1(0, kRegidNone, kRegidNone, kRegidNone) != 0xfcfc00fcu);
    // step two's float2 attribute at binding 0, offset 0: IDX 0 | OFFSET 0 | FORMAT(FMT6_32_32_FLOAT = 103) << 20 = 0x6700000
    // | SWAP(WZYX) 0 | UNK30 0x40000000 | FLOAT 0x80000000 = 0xc6700000; its destination r1.xy: WRITEMASK 0x3 | REGID(4) << 4
    CHECK_EQ(vfdDecodeInstr(0, 0, false, FMT6_32_32_FLOAT, WZYX, true), 0xc6700000u);
    CHECK_EQ(vfdDecodeInstr(1, 8, true, FMT6_32_UINT, WZYX, false), 0x44b20101u);
    CHECK_EQ(vfdDestCntlInstr(0x3, 4), 0x43u);
    // the link of a position-only pair (cnt 1, max_loc 4, position at loc 0, pos regid 0):
    //   SP_VS_OUT_REG(0)      A_REGID(0) | A_COMPMASK(0xf) << 8 = 0xf00
    //   SP_VS_VPC_DST_REG(0)  OUTLOC0(0) = 0
    //   VPC_VS_PACK           STRIDE_IN_VPC(4) | POSITIONLOC(0) << 8 | PSIZELOC(0xff) << 16 = 0x00ff0004
    //   PC_VS_OUT_CNTL        STRIDE_IN_VPC(4) = 4;  SP_VS_PRIMITIVE_CNTL OUT(1) = 1
    //   VPC_VS_CLIP_CNTL      CLIP_MASK 0 | 0xff << 8 | 0xff << 16 = 0x00ffff00;  VPC_VS_LAYER_CNTL 0xff | 0xff << 8 | 0xff << 16
    //   VPC_CNTL_0            NUMNONPOSVAR 0 | PRIMIDLOC 0xff << 8 | VIEWIDLOC 0xff << 24 = 0xff00ff00
    //   VPC_VAR_DISABLE[i]    ~0 = 0xffffffff
    CHECK_EQ(spVsOutReg(0, 0xf, 0, 0), 0xf00u);
    CHECK_EQ(spVsVpcDstReg(0, 0, 0, 0), 0u);
    CHECK_EQ(vpcVsPack(4, 0), 0x00ff0004u);
    CHECK_EQ(pcVsOutCntl(4), 0x4u);
    CHECK_EQ(spVsPrimitiveCntl(1), 0x1u);
    CHECK_EQ(vpcVsClipCntl(0), 0x00ffff00u);
    CHECK_EQ(vpcVsLayerCntl(), 0x00ffffffu);
    CHECK_EQ(grasVsClCntl(0, 0), 0u);
    CHECK_EQ(vpcCntl0(0, kLocNone, false, kLocNone), 0xff00ff00u);
    CHECK_EQ(vpcVarDisable(0), 0xffffffffu);
    // with one float4 varying at inloc 0 and the position after it (var[0] = {colour regid 4, 0xf, loc 0}, var[1] =
    // {pos regid 0, 0xf, loc 4}, cnt 2, max_loc 8): SP_VS_OUT_REG(0) = 4 | 0xf << 8 | 0 << 16 | 0xf << 24;
    // SP_VS_VPC_DST_REG(0) = 0 | 4 << 8; VPC_VS_PACK 0x00ff0408; PC_VS_OUT_CNTL 8; OUT 2; VPC_CNTL_0 = 4 | VARYING 0x10000
    // | 0xff00ff00 = 0xff01ff04; VPC_VAR_DISABLE[0] = ~0xf
    CHECK_EQ(spVsOutReg(4, 0xf, 0, 0xf), 0x0f000f04u);
    CHECK_EQ(spVsVpcDstReg(0, 4, 0, 0), 0x400u);
    CHECK_EQ(vpcVsPack(8, 4), 0x00ff0408u);
    CHECK_EQ(pcVsOutCntl(8), 0x8u);
    CHECK_EQ(spVsPrimitiveCntl(2), 0x2u);
    CHECK_EQ(vpcCntl0(4, kLocNone, true, kLocNone), 0xff01ff04u);
    CHECK_EQ(vpcVarDisable(0xf), 0xfffffff0u);
    // the twins: a position location in the wrong byte, a stride that is not the location count
    CHECK(vpcVsPack(4, 0) != 0x00ff0400u);
    CHECK(vpcCntl0(4, kLocNone, true, kLocNone) != 0xff00ff04u);
    CHECK_EQ(pcVsOutCntl(4, true, true, true, true, 0xff), 0x00ff0f04u);
    CHECK_EQ(vpcVsClipCntl(0x3, 0, 0xff), 0x00ff0003u);
    // the interpolation mode of a flat input at VPC location 17: register 1, bits 2..3 of it
    CHECK_EQ(vpcVaryingInterpModeReg(17), 1u);
    CHECK_EQ(vpcVaryingInterpModeBits(17, INTERP_FLAT), 0x4u);
    CHECK_EQ(vpcVaryingInterpModeBits(0, INTERP_FLAT), 0x1u);
    CHECK_EQ(vpcVaryingInterpModeBits(15, INTERP_ONE), 0xc0000000u);
    CHECK_EQ(vpcVaryingInterpModeBits(3, INTERP_SMOOTH), 0u);
}

TEST(a6xx_first_draw_fs_input_output_and_raster_words) {
    // the FS input block for a constant-colour FS (no system values, no barycentrics): HLSQ_CONTROL_1_REG 7,
    // HLSQ_CONTROL_2..4_REG 0xfcfcfcfc, HLSQ_CONTROL_5_REG 0xfcfc; with a smooth varying the perspective pixel
    // barycentric is in r0.x: HLSQ_CONTROL_3_REG 0xfcfcfc00
    CHECK_EQ(hlsqControl1Reg(), 0x7u);
    CHECK_EQ(hlsqControl2Reg(kRegidNone, kRegidNone, kRegidNone, kRegidNone), 0xfcfcfcfcu);
    CHECK_EQ(hlsqControl3Reg(kRegidNone, kRegidNone, kRegidNone, kRegidNone), 0xfcfcfcfcu);
    CHECK_EQ(hlsqControl3Reg(0, kRegidNone, kRegidNone, kRegidNone), 0xfcfcfc00u);
    CHECK_EQ(hlsqControl4Reg(kRegidNone, kRegidNone, kRegidNone, kRegidNone), 0xfcfcfcfcu);
    CHECK_EQ(hlsqControl4Reg(kRegidNone, kRegidNone, 4, 6), 0x0604fcfcu);
    CHECK_EQ(hlsqControl5Reg(), 0xfcfcu);
    // GRAS_CNTL and RB_RENDER_CONTROL0 from the ij regids in ir3_bary order: none -> 0; the perspective pixel one ->
    // IJ_PERSP_PIXEL (bit 0), and RB_RENDER_CONTROL0 adds UNK10 (0x400) for the varyings -> 0x401
    uint32_t none[IJ_COUNT] = {kRegidNone, kRegidNone, kRegidNone, kRegidNone, kRegidNone, kRegidNone, kRegidNone};
    uint32_t persp[IJ_COUNT] = {0, kRegidNone, kRegidNone, kRegidNone, kRegidNone, kRegidNone, kRegidNone};
    CHECK_EQ(ijBits(none), 0u);
    CHECK_EQ(ijBits(persp), 0x1u);
    CHECK_EQ(grasCntl(ijBits(none), 0), 0u);
    CHECK_EQ(grasCntl(ijBits(persp), 0), 0x1u);
    CHECK_EQ(rbRenderControl0(ijBits(none), 0, false), 0u);
    CHECK_EQ(rbRenderControl0(ijBits(persp), 0, true), 0x401u);
    CHECK_EQ(rbRenderControl1(false, false, false, false), 0u);
    CHECK_EQ(rbRenderControl1(true, false, false, false), 0x4u);
    CHECK_EQ(rbRenderControl1(false, true, true, true, FRAGCOORD_SAMPLE), 0x79u);
    // the twins: a linear centroid regid lands on bit 4 (ir3_bary index 5, not 6), gl_FragCoord's size adds LINEAR_PIXEL,
    // the fragcoord mask sits at bit 6
    uint32_t linc[IJ_COUNT] = {kRegidNone, kRegidNone, kRegidNone, kRegidNone, kRegidNone, 2, kRegidNone};
    CHECK_EQ(ijBits(linc), 0x10u);
    CHECK_EQ(ijBits(none, true), 0x8u);
    CHECK_EQ(ijBits(none, false, true), 0x20u);
    CHECK_EQ(grasCntl(0, 0xf), 0x3c0u);
    CHECK_EQ(rbRenderControl0(0, 0x3, false), 0xc0u);
    // SP_FS_PREFETCH_CNTL: no prefetch; IJ_WRITE_DISABLE (bit 3) without the perspective pixel barycentric
    CHECK_EQ(spFsPrefetchCntl(0, true), 0x8u);
    CHECK_EQ(spFsPrefetchCntl(0, false), 0u);
    CHECK_EQ(spFsPrefetchCntl(2, false, true), 0x12u);
    // the FS outputs: SP_FS_OUTPUT_CNTL0 with no depth/mask/stencil output = 0xfcfcfc00; SP_FS_OUTPUT_REG(0) = the colour's
    // regid (r0.x = 0), HALF_PRECISION 0x100 for a half output; RB_FS_OUTPUT_CNTL0 = 0
    CHECK_EQ(spFsOutputCntl0(kRegidNone, kRegidNone, kRegidNone), 0xfcfcfc00u);
    CHECK_EQ(spFsOutputCntl0(kRegidNone, kRegidNone, kRegidNone, true), 0xfcfcfc01u);
    CHECK_EQ(spFsOutputReg(0, false), 0u);
    CHECK_EQ(spFsOutputReg(4, true), 0x104u);
    CHECK_EQ(spFsOutputReg(0x3c, false), 0x3cu);
    CHECK_EQ(A6XX_RB_FS_OUTPUT_CNTL0_FRAG_WRITES_Z, 0x2u);
    // the rasterizer: GRAS_SU_CNTL = LINEHALFWIDTH(0.5 * 4 = 2) << 3 = 0x10 | LINE_MODE(RECTANGULAR) << 13 = 0x2000 -> 0x2010;
    // GRAS_CL_CNTL = ZERO_GB_SCALE_Z 0x40 | VP_CLIP_CODE_IGNORE 0x80 = 0xc0; the point words 0xffc00001 and 0x10
    CHECK_EQ(grasSuCntl(false, false, false), 0x2010u);
    CHECK_EQ(grasSuCntl(false, true, true), 0x2016u);
    CHECK_EQ(grasSuCntl(true, false, false, 2, BRESENHAM), 0x11u);
    CHECK_EQ(A6XX_GRAS_SU_CNTL_LINEHALFWIDTH__MUL, 4u);
    CHECK_EQ(grasClCntl(), 0xc0u);
    CHECK_EQ(grasClCntl(false, true, true, true, true), 0xa6u);
    CHECK_EQ(A6XX_GRAS_CL_CNTL_VP_XFORM_DISABLE | A6XX_GRAS_CL_CNTL_PERSP_DIVISION_DISABLE | A6XX_GRAS_CL_CNTL_CLIP_DISABLE |
                 A6XX_GRAS_CL_CNTL_VP_CLIP_CODE_IGNORE,
             0x381u);   // r3d's window-space form (tu_clear_blit.cc:946-951), for the record
    CHECK_EQ(grasSuPointMinmax(1, 0xffc0), 0xffc00001u);
    CHECK_EQ(grasSuPointSize(16), 0x10u);
    CHECK_EQ(A6XX_GRAS_SU_POINT_SIZE__MUL, 16u);
    CHECK_EQ(A6XX_GRAS_SU_POINT_MINMAX_MAX__MUL, 16u);
    // the twin: the line half width is in quarter pixels, so 0.5 pixels is 2, not 1 (0x8) and not 0.5 (0)
    CHECK(grasSuCntl(false, false, false, 1) != 0x2010u);
    CHECK_EQ(grasSuCntl(false, false, false, 1), 0x2008u);
    // polygon modes, depth plane, primitive control
    CHECK_EQ(A6XX_VPC_POLYGON_MODE_MODE(POLYMODE6_TRIANGLES), 0x3u);
    CHECK_EQ(A6XX_PC_POLYGON_MODE_MODE(POLYMODE6_TRIANGLES), 0x3u);
    CHECK_EQ(A6XX_RB_DEPTH_PLANE_CNTL_Z_MODE(A6XX_LATE_Z), 0x1u);
    CHECK_EQ(A6XX_GRAS_SU_DEPTH_PLANE_CNTL_Z_MODE(A6XX_EARLY_Z), 0u);
    CHECK_EQ(A6XX_PC_PRIMITIVE_CNTL_0_PROVOKING_VTX_LAST, 0x2u);
    // blending off: RB_MRT_CONTROL(0) = COMPONENT_ENABLE(0xf) << 7 = 0x780 (ROP_CODE 0, turnip's); RB_BLEND_CNTL =
    // INDEPENDENT_BLEND 0x100 | SAMPLE_MASK(0xffff) << 16 = 0xffff0100 (r3d: 0xffff0000); SP_BLEND_CNTL UNK8 = 0x100
    CHECK_EQ(rbMrtControl(0xf), 0x780u);
    CHECK_EQ(rbMrtControl(0x1), 0x80u);
    CHECK_EQ(rbBlendCntl(true), 0xffff0100u);
    CHECK_EQ(rbBlendCntl(false), 0xffff0000u);
    CHECK_EQ(A6XX_SP_BLEND_CNTL_UNK8, 0x100u);
    // the twin: fd6's ROP_COPY in the inert field would be 0x7e0; the fake must not judge ROP_CODE
    CHECK_EQ(rbMrtControl(0xf) | A6XX_RB_MRT_CONTROL_ROP_CODE(ROP_COPY), 0x7e0u);
    CHECK_EQ(A6XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(FACTOR_SRC_ALPHA) | A6XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(FACTOR_ONE_MINUS_SRC_ALPHA) |
                 A6XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(BLEND_DST_PLUS_SRC),
             0x00000706u);
    CHECK_EQ(A6XX_RB_STENCIL_CONTROL_ZPASS_BF(STENCIL_DECR_WRAP), 0x1c000000u);
    CHECK_EQ(A6XX_RB_ALPHA_CONTROL_ALPHA_TEST_FUNC(FUNC_ALWAYS), 0xe00u);
    // the draw packet's word 0: PRIM_TYPE(TRILIST = 4) | SOURCE_SELECT(AUTO_INDEX = 2) << 6 = 0x80 | VIS_CULL(IGNORE = 0) = 0x84;
    // with USE_VISIBILITY (1 << 8): 0x184 (render::Options::visCullUse); a strip: 6 | 0x80 = 0x86
    CHECK_EQ(drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_AUTO_INDEX, IGNORE_VISIBILITY), 0x84u);
    CHECK_EQ(drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_AUTO_INDEX, USE_VISIBILITY), 0x184u);
    CHECK_EQ(drawIndxOffset0(DI_PT_TRISTRIP, DI_SRC_SEL_AUTO_INDEX, IGNORE_VISIBILITY), 0x86u);
    CHECK_EQ(drawIndxOffset0(DI_PT_TRILIST, DI_SRC_SEL_DMA, IGNORE_VISIBILITY) | CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(INDEX4_SIZE_16_BIT), 0x404u);
    CHECK_EQ(CP_DRAW_INDX_OFFSET_1_NUM_INSTANCES(1), 1u);
    CHECK_EQ(CP_DRAW_INDX_OFFSET_2_NUM_INDICES(3), 3u);
    CHECK_EQ(CP_DRAW_INDX_OFFSET_0_GS_ENABLE, 0x10000u);
}
