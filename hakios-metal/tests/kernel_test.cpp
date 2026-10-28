// The kernel fixtures: that they compile under the project's flags, that the
// generated numbers agree with each other, and the facts the dispatch code will
// lean on (which register holds which id, where the constants go). The words
// themselves are checked by the generator's round trip (tools/ir3-kernel
// disassembles them and compares with the source) and, on the phone, by what
// the kernels write into their buffers.
//
// SPDX-License-Identifier: MIT
#include <cstring>

#include "hakios-metal/kernel.hpp"
#include "test.hpp"

// The same kernel twice: fill_id is the text one, fill_id_air the one ir3
// compiled from the AIR (ir3_compiler -n fill_id_air).
#include "fixtures/end_only.hpp"
#include "fixtures/fill_id.hpp"
#include "fixtures/fill_id_air.hpp"

using hakios::metal::Kernel;
namespace kernels = hakios::metal::kernels;
using hakios::metal::kNoConstRegid;
using hakios::metal::kNoRegid;

namespace {

// r<n>.<c> as the registers encode it
constexpr uint32_t regid(uint32_t n, uint32_t c) { return n * 4 + c; }

// What every kernel must satisfy, whichever path produced it.
void checkInvariants(const Kernel& k) {
    CHECK(k.words != nullptr);
    CHECK(k.numWords > 0);
    // SP_CS_INSTRLEN counts 16-instruction (128-byte) groups; the binary is at least that long
    CHECK(k.instrlen >= 1);
    CHECK(k.numWords * 4 >= k.instrlen * 128);
    // HLSQ_CS_CNTL.CONSTLEN is in vec4 and ir3 aligns it to 4 on gen 4+
    CHECK_EQ(k.constlen % 4, 0u);
    // the register footprints are counts minus one, so -1 means none
    CHECK(k.maxReg >= -1);
    CHECK(k.maxHalfReg >= -1);
    // gen 6: ir3 compiles everything for the merged register file
    CHECK(k.mergedRegs);
    CHECK(k.numUboRanges <= hakios::metal::kMaxUboRanges);
    CHECK(k.numBufs <= hakios::metal::kMaxBufs);
    // the constants the kernel carries all fit in constlen
    if (k.numImmediates > 0) {
        CHECK(k.immediates != nullptr);
        CHECK(k.immediatesOffsetVec4 * 4 + k.numImmediates <= k.constlen * 4);
    } else {
        CHECK(k.immediates == nullptr);
    }
    if (k.driverParamsOffsetVec4 >= 0) {
        CHECK(uint32_t(k.driverParamsOffsetVec4) < k.constlen);
    }
    for (uint32_t i = 0; i < k.numUboRanges; i++) {
        CHECK(k.uboRanges[i].sizeDwords > 0);
        CHECK_EQ(k.uboRanges[i].sizeDwords % 4, 0u);
        CHECK(k.uboRanges[i].offsetVec4 * 4 + k.uboRanges[i].sizeDwords <= k.constlen * 4);
    }
    if (k.constantDataSize > 0) {
        CHECK(k.constantDataOffset + k.constantDataSize <= k.numWords * 4);
    }
    CHECK(k.name != nullptr && k.name[0] != '\0');
    CHECK(k.source != nullptr && k.source[0] != '\0');
}

}  // namespace

TEST(end_only_fixture) {
    const Kernel& k = kernels::end_only;
    checkInvariants(k);
    CHECK_EQ(sizeof(kernels::end_only_words) / 4, k.numWords);
    // one `end`, padded to the 128 bytes of one instruction group; the first
    // instruction is `end` (cat0, opcode 3 in the high dword), the rest zeros
    CHECK_EQ(k.numWords, 32u);
    CHECK_EQ(k.instrlen, 1u);
    CHECK_EQ(k.words[0], 0u);
    CHECK_EQ(k.words[1], 0x03000000u);
    CHECK_EQ(k.constlen, 0u);
    CHECK_EQ(k.maxReg, -1);
    CHECK_EQ(k.localSize[0], 1u);
    CHECK(!k.localSizeVariable);
    CHECK_EQ(k.localIdRegid, kNoRegid);
    CHECK_EQ(k.workGroupIdRegid, kNoRegid);
    CHECK_EQ(k.numImmediates, 0u);
    CHECK_EQ(k.driverParamsOffsetVec4, -1);
    CHECK_EQ(k.numBufs, 0u);
    CHECK_EQ(k.numwgConstRegid, kNoConstRegid);
    CHECK_EQ(std::strcmp(k.name, "end_only"), 0);
}

TEST(fill_id_text_fixture) {
    const Kernel& k = kernels::fill_id;
    checkInvariants(k);
    CHECK_EQ(sizeof(kernels::fill_id_words) / 4, k.numWords);
    CHECK_EQ(sizeof(kernels::fill_id_immediates) / 4, k.numImmediates);
    // the ids the hardware loads: local id in r0.xyz, workgroup id in r48.xyz (a shared register)
    CHECK_EQ(k.localIdRegid, regid(0, 0));
    CHECK_EQ(k.workGroupIdRegid, regid(48, 0));
    // one workgroup of 64 along x, fixed by the kernel
    CHECK_EQ(k.localSize[0], 64u);
    CHECK_EQ(k.localSize[1], 1u);
    CHECK_EQ(k.localSize[2], 1u);
    CHECK(!k.localSizeVariable);
    // r0..r3 used, no half registers
    CHECK_EQ(k.maxReg, 3);
    CHECK_EQ(k.maxHalfReg, -1);
    CHECK(!k.doubleThreadsize);  // 64 invocations: one wave of 64
    // c sits in c0.x: the first immediate, at vec4 0, and the default is not 0
    CHECK_EQ(k.immediatesOffsetVec4, 0u);
    CHECK_EQ(k.numImmediates, 4u);
    CHECK_EQ(k.immediates[0], 0x1234u);
    CHECK_EQ(k.constlen, 4u);
    // a text kernel reads no driver params and pushes no UBOs: its one buffer is an IBO
    CHECK_EQ(k.driverParamsOffsetVec4, -1);
    CHECK_EQ(k.numUboRanges, 0u);
    CHECK_EQ(k.numUbos, 0u);
    CHECK_EQ(k.numBufs, 1u);
    CHECK_EQ(k.bufs[0].sizeBytes, 4096u);
    CHECK_EQ(k.bufs[0].constRegid, kNoConstRegid);
    CHECK_EQ(k.numwgConstRegid, kNoConstRegid);
    CHECK_EQ(k.constantDataSize, 0u);
    CHECK_EQ(std::strcmp(k.name, "fill_id"), 0);
    CHECK(std::strstr(k.source, "@localsize 64, 1, 1") != nullptr);
}

TEST(fill_id_air_fixture) {
    const Kernel& k = kernels::fill_id_air;
    checkInvariants(k);
    CHECK_EQ(sizeof(kernels::fill_id_air_words) / 4, k.numWords);
    // the same ids in the same registers as the text kernel: the dispatch code
    // programs HLSQ_CS_CNTL_0 the same way for both
    CHECK_EQ(k.localIdRegid, regid(0, 0));
    CHECK_EQ(k.workGroupIdRegid, regid(48, 0));
    // but a Metal kernel's threadgroup size is the dispatch's: ir3 reads it
    // from the driver params, so they must be uploaded
    CHECK(k.localSizeVariable);
    CHECK(k.driverParamsOffsetVec4 >= 0);
    // `constant uint &c` at [[buffer(1)]] became a pushed UBO range of block 1,
    // and no UBO descriptor is left to bind
    CHECK_EQ(k.numUboRanges, 1u);
    CHECK_EQ(k.uboRanges[0].block, 1u);
    CHECK_EQ(k.uboRanges[0].startDword, 0u);
    CHECK(k.uboRanges[0].sizeDwords >= 4);
    CHECK_EQ(k.numUbos, 0u);
    // `device uint *out` at [[buffer(0)]] is SSBO 0: one IBO
    CHECK_EQ(k.numSsbos, 1u);
    CHECK_EQ(k.numIbos, 1u);
    CHECK_EQ(k.numBufs, 0u);
    // the compiler's immediates (K) come after the driver params and the UBO range
    CHECK(k.numImmediates >= 1);
    CHECK(k.immediatesOffsetVec4 > uint32_t(k.driverParamsOffsetVec4));
    CHECK(k.immediatesOffsetVec4 > k.uboRanges[0].offsetVec4);
    CHECK_EQ(k.constantDataSize, 0u);
    CHECK_EQ(std::strcmp(k.name, "fill_id_air"), 0);
    CHECK(std::strstr(k.source, "air.thread_position_in_grid") != nullptr);
}
