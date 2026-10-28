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
// the bitcode llvm-as made of tools/kernels/NAME.ll (tools/kernels.mk: out/host/kernels/NAME.bc), when it is there
std::vector<uint8_t> bitcodeOf(const char* name) {
    std::vector<uint8_t> bytes;
    FILE* f = fopen((std::string("out/host/kernels/") + name + ".bc").c_str(), "rb");
    if (f == nullptr) return bytes;
    uint8_t buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    fclose(f);
    return bytes;
}

// compiled in this process, or nullptr with the reason printed (a build with the compiler and no bitcode is a broken
// build, not a pass: the caller CHECKs the result)
std::unique_ptr<CompiledKernel> compiled(const char* name) {
    std::vector<uint8_t> bc = bitcodeOf(name);
    CHECK(!bc.empty());
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why);
    if (k == nullptr) std::printf("       %s: %s\n", name, why.c_str());
    return k;
}

const Resource* resourceAt(const CompiledKernel& k, Resource::Kind kind, uint32_t index) {
    for (const Resource& r : k.resources) {
        if (r.kind == kind && r.index == index) return &r;
    }
    return nullptr;
}
}  // namespace

TEST(compiler_without_mesa_refuses_by_name_and_with_it_matches_the_fixture) {
    std::vector<uint8_t> bc = bitcodeOf("fill_id");
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
    CHECK_EQ(a.numTextureSlots, b.numTextureSlots);
    CHECK_EQ(a.samplerMask, b.samplerMask);
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        CHECK_EQ(a.textures[i].kind, b.textures[i].kind);
        CHECK_EQ(a.textures[i].dim, b.textures[i].dim);
        CHECK_EQ(a.textures[i].isArray, b.textures[i].isArray);
        CHECK_EQ(a.textures[i].element, b.textures[i].element);
        CHECK_EQ(a.textures[i].bits, b.textures[i].bits);
        CHECK_EQ(a.textures[i].read, b.textures[i].read);
        CHECK_EQ(a.textures[i].write, b.textures[i].write);
    }
    CHECK(k->name == "fill_id");
    CHECK(k->listing.find("stib") != std::string::npos);
    // and a module that is not a kernel is refused by name, not compiled into something else
    uint8_t garbage[16] = {'B', 'C', 0xc0, 0xde, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(compileAir(garbage, sizeof garbage, &why) == nullptr);
    CHECK(why.find("refused") != std::string::npos);
}

// --- the texture kernels: what the NIR walk reads off tools/kernels/tex_*.ll, against the listings -------------------------
// The numbers are the listings' (tools/kernels/*.ll head comments; the readers' drafts): tex_fill writes image 0 with a
// 32-bit uint stib and has no sampled texture; tex_copy reads texture 1 with isaml (t#1, s#0: a txf names no sampler)
// and writes image 0; tex_sample_gid samples texture 0 with sampler 0 (saml s#0 t#0) and stores to SSBO 0. ir3's num_samp
// is textures_used's last bit plus the image count: 1, 3 and 1.

TEST(compiler_reads_the_image_only_kernel_off_the_nir) {
    if (!hasCompiler()) return;
    auto k = compiled("tex_fill");
    CHECK(k != nullptr);
    if (k == nullptr) return;
    const Kernel& a = k->kernel;
    CHECK(k->name == "tex_fill");
    CHECK_EQ(a.textures[0].kind, uint8_t(kTexImage));
    CHECK_EQ(a.textures[0].dim, uint8_t(kTexDim2D));
    CHECK_EQ(a.textures[0].isArray, 0u);
    CHECK_EQ(a.textures[0].element, uint8_t(kElemUint));
    CHECK_EQ(a.textures[0].bits, 32u);
    CHECK_EQ(a.textures[0].read, 0u);
    CHECK_EQ(a.textures[0].write, 1u);
    for (uint32_t i = 1; i < kMaxTextureSlots; i++) CHECK_EQ(a.textures[i].kind, uint8_t(kTexNone));
    CHECK_EQ(a.numTextureSlots, 0u);   // no sampled texture: no texture state at all, as Gallium builds none
    CHECK_EQ(a.samplerMask, 0u);
    CHECK_EQ(a.numSsbos, 0u);
    CHECK_EQ(a.numIbos, 1u);           // the image at IBO slot numSsbos + 0 = 0
    CHECK_EQ(a.numSamp, 1u);           // 0 textures + 1 image
    CHECK_EQ(a.numUbos, 0u);           // c is pushed
    CHECK_EQ(a.numUboRanges, 1u);
    CHECK(k->listing.find("stib.b.typed.2d.u32.4") != std::string::npos);
    // the resources carry the same shape, and Apple's declaration beside it
    const Resource* img = resourceAt(*k, Resource::kImage, 0);
    CHECK(img != nullptr);
    if (img != nullptr) {
        CHECK_EQ(img->textureDim, uint8_t(kTexDim2D));
        CHECK_EQ(img->textureArray, 0u);
        CHECK_EQ(img->element, uint8_t(kElemUint));
        CHECK(img->write && !img->read);
        CHECK(!img->nullTexture && !img->inlineSampler);
        CHECK(img->typeName == "texture2d<uint, write>");
    }
    CHECK(resourceAt(*k, Resource::kConstantBuffer, 0) != nullptr);
    CHECK_EQ(k->resources.size(), size_t(2));
}

TEST(compiler_reads_the_copy_kernel_off_the_nir) {
    if (!hasCompiler()) return;
    auto k = compiled("tex_copy");
    CHECK(k != nullptr);
    if (k == nullptr) return;
    const Kernel& a = k->kernel;
    // in [[texture(1)]] is sampled (isaml through the texture table), out [[texture(0)]] is an image (stib through IBO 0)
    CHECK_EQ(a.textures[1].kind, uint8_t(kTexSampled));
    CHECK_EQ(a.textures[1].dim, uint8_t(kTexDim2D));
    CHECK_EQ(a.textures[1].element, uint8_t(kElemUint));
    CHECK_EQ(a.textures[1].bits, 0u);
    CHECK_EQ(a.textures[1].read, 1u);
    CHECK_EQ(a.textures[1].write, 0u);
    CHECK_EQ(a.textures[0].kind, uint8_t(kTexImage));
    CHECK_EQ(a.textures[0].element, uint8_t(kElemUint));
    CHECK_EQ(a.textures[0].bits, 32u);
    CHECK_EQ(a.textures[0].write, 1u);
    CHECK_EQ(a.textures[0].read, 0u);
    CHECK_EQ(a.numTextureSlots, 2u);   // highest sampled index + 1: slot 0 of the texture table is a zero dummy
    CHECK_EQ(a.samplerMask, 0u);       // a txf names s#0 but reads no sampler: the dispatch still loads one zero TEX_SAMP
    CHECK_EQ(a.numSsbos, 0u);
    CHECK_EQ(a.numIbos, 1u);
    CHECK_EQ(a.numSamp, 3u);           // textures_used's last bit (2) + 1 image
    CHECK_EQ(a.numUbos, 0u);
    CHECK(k->listing.find("isaml") != std::string::npos);
    CHECK(k->listing.find("s#0, t#1") != std::string::npos);
    CHECK(k->listing.find("stib.b.typed.2d.u32.4.imm") != std::string::npos);
    const Resource* in = resourceAt(*k, Resource::kTexture, 1);
    const Resource* out = resourceAt(*k, Resource::kImage, 0);
    CHECK(in != nullptr && out != nullptr);
    if (in != nullptr && out != nullptr) {
        CHECK_EQ(in->textureDim, uint8_t(kTexDim2D));
        CHECK_EQ(in->element, uint8_t(kElemUint));
        CHECK(in->read && !in->write);
        CHECK(in->typeName == "texture2d<uint, read>");
        CHECK_EQ(out->textureDim, uint8_t(kTexDim2D));
        CHECK_EQ(out->element, uint8_t(kElemUint));
        CHECK(out->typeName == "texture2d<uint, write>");
    }
}

TEST(compiler_reads_the_sampling_kernel_off_the_nir) {
    if (!hasCompiler()) return;
    auto k = compiled("tex_sample_gid");
    CHECK(k != nullptr);
    if (k == nullptr) return;
    const Kernel& a = k->kernel;
    CHECK_EQ(a.textures[0].kind, uint8_t(kTexSampled));
    CHECK_EQ(a.textures[0].dim, uint8_t(kTexDim2D));
    CHECK_EQ(a.textures[0].isArray, 0u);
    CHECK_EQ(a.textures[0].element, uint8_t(kElemFloat));
    CHECK_EQ(a.textures[0].read, 1u);
    CHECK_EQ(a.textures[0].write, 0u);
    for (uint32_t i = 1; i < kMaxTextureSlots; i++) CHECK_EQ(a.textures[i].kind, uint8_t(kTexNone));
    CHECK_EQ(a.samplerMask, 1u);       // saml s#0
    CHECK_EQ(a.numTextureSlots, 1u);
    CHECK_EQ(a.numSsbos, 1u);          // out [[buffer(0)]]
    CHECK_EQ(a.numIbos, 1u);
    CHECK_EQ(a.numSamp, 1u);
    CHECK_EQ(a.numUbos, 0u);
    CHECK(k->listing.find("saml") != std::string::npos);
    CHECK(k->listing.find("s#0, t#0") != std::string::npos);
    const Resource* tex = resourceAt(*k, Resource::kTexture, 0);
    const Resource* samp = resourceAt(*k, Resource::kSampler, 0);
    const Resource* buf = resourceAt(*k, Resource::kDeviceBuffer, 0);
    CHECK(tex != nullptr && samp != nullptr && buf != nullptr);
    if (tex != nullptr && samp != nullptr) {
        CHECK_EQ(tex->textureDim, uint8_t(kTexDim2D));
        CHECK_EQ(tex->element, uint8_t(kElemFloat));
        CHECK(tex->typeName == "texture2d<float, sample>");
        CHECK(!samp->inlineSampler);
        CHECK(samp->typeName == "sampler");
        CHECK_EQ(samp->textureDim, uint8_t(kTexDimNone));
    }
    CHECK_EQ(k->resources.size(), size_t(3));
}
