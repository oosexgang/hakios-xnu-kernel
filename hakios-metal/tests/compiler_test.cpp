// The compiler inside the driver against the compiler outside it: fill_id.ll compiled here must be, field for field
// and word for word, the fixture tools/mesa/ir3-dump.patch had Mesa's ir3_compiler write (tests/fixtures/fill_id_air.hpp).
// Without Mesa's libraries in the build (no MESA_BUILD) the compiler is a refusal, and that is checked instead.
#include <cstdio>
#include <cstring>
#include <vector>

#include "hakios-metal/compiler.hpp"
#include "fixtures/fill_id_air.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
// the bitcode llvm-as made of tools/kernels/fill_id.ll (tools/kernels.mk: out/host/kernels/fill_id.bc), when it is there
std::vector<uint8_t> fillIdBitcode() {
    std::vector<uint8_t> bytes;
    FILE* f = fopen("out/host/kernels/fill_id.bc", "rb");
    if (f == nullptr) return bytes;
    uint8_t buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    fclose(f);
    return bytes;
}
}  // namespace

TEST(compiler_without_mesa_refuses_by_name_and_with_it_matches_the_fixture) {
    std::vector<uint8_t> bc = fillIdBitcode();
    std::string why;
    if (!hasCompiler()) {
        auto k = compileAir(bc.data(), bc.size(), &why);
        CHECK(k == nullptr);
        CHECK(why.find("MESA_BUILD") != std::string::npos);
        std::printf("       (no compiler in this build: the fixture comparison did not run)\n");
        return;
    }
    // a build with the compiler and no bitcode is a broken build, not a pass
    CHECK(!bc.empty());
    auto k = compileAir(bc.data(), bc.size(), &why);
    CHECK(k != nullptr);
    if (k == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    const Kernel& a = k->kernel;
    const Kernel& b = kernels::fill_id_air;
    CHECK_EQ(a.numWords, b.numWords);
    CHECK(memcmp(a.words, b.words, 4 * std::min(a.numWords, b.numWords)) == 0);
    CHECK_EQ(a.instrlen, b.instrlen);
    CHECK_EQ(a.constlen, b.constlen);
    CHECK_EQ(a.maxReg, b.maxReg);
    CHECK_EQ(a.maxHalfReg, b.maxHalfReg);
    CHECK_EQ(a.doubleThreadsize, b.doubleThreadsize);
    CHECK_EQ(a.mergedRegs, b.mergedRegs);
    CHECK_EQ(a.earlyPreamble, b.earlyPreamble);
    CHECK_EQ(a.branchstackHw, b.branchstackHw);
    CHECK_EQ(a.sharedSize, b.sharedSize);
    CHECK_EQ(a.pvtmemSize, b.pvtmemSize);
    CHECK_EQ(a.pvtmemPerWave, b.pvtmemPerWave);
    for (int i = 0; i < 3; i++) CHECK_EQ(a.localSize[i], b.localSize[i]);
    CHECK_EQ(a.localSizeVariable, b.localSizeVariable);
    CHECK_EQ(a.localIdRegid, b.localIdRegid);
    CHECK_EQ(a.workGroupIdRegid, b.workGroupIdRegid);
    CHECK_EQ(a.numImmediates, b.numImmediates);
    CHECK(memcmp(a.immediates, b.immediates, 4 * std::min(a.numImmediates, b.numImmediates)) == 0);
    CHECK_EQ(a.immediatesOffsetVec4, b.immediatesOffsetVec4);
    CHECK_EQ(a.driverParamsOffsetVec4, b.driverParamsOffsetVec4);
    CHECK_EQ(a.numUbos, b.numUbos);
    CHECK_EQ(a.numSsbos, b.numSsbos);
    CHECK_EQ(a.numIbos, b.numIbos);
    CHECK_EQ(a.numSamp, b.numSamp);
    CHECK_EQ(a.numUboRanges, b.numUboRanges);
    for (uint32_t i = 0; i < a.numUboRanges && i < b.numUboRanges; i++) {
        CHECK_EQ(a.uboRanges[i].block, b.uboRanges[i].block);
        CHECK_EQ(a.uboRanges[i].offsetVec4, b.uboRanges[i].offsetVec4);
        CHECK_EQ(a.uboRanges[i].startDword, b.uboRanges[i].startDword);
        CHECK_EQ(a.uboRanges[i].sizeDwords, b.uboRanges[i].sizeDwords);
    }
    CHECK_EQ(a.constantDataOffset, b.constantDataOffset);
    CHECK_EQ(a.constantDataSize, b.constantDataSize);
    CHECK(k->name == "fill_id");
    CHECK(k->listing.find("stib") != std::string::npos);
    // and a module that is not a kernel is refused by name, not compiled into something else
    uint8_t garbage[16] = {'B', 'C', 0xc0, 0xde, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(compileAir(garbage, sizeof garbage, &why) == nullptr);
    CHECK(why.find("refused") != std::string::npos);
}
