// The compiler inside the driver against the compiler outside it: each tools/kernels/*.ll compiled here must be, field
// for field and word for word, the fixture tools/mesa/ir3-dump.patch had Mesa's ir3_compiler write (tests/fixtures/
// *_air.hpp) -- the texture fields included, which hold the core's walk of the NIR (src/compiler.cpp) to the patch's.
// Without Mesa's libraries in the build (no MESA_BUILD) the compiler is a refusal, and that is checked instead.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hakios-metal/compiler.hpp"
#include "fixtures/fill_id_air.hpp"
#include "fixtures/tex_copy_air.hpp"
#include "fixtures/tex_fill_air.hpp"
#include "fixtures/tex_sample_gid_air.hpp"
#include "fixtures/tex_fill_h_air.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
// the bitcode llvm-as made of tools/kernels/NAME.ll (tools/kernels.mk: out/host/kernels/NAME.bc), when it is there
std::vector<uint8_t> bitcodeOf(const char* name) {
    std::vector<uint8_t> bytes;
    std::string path = std::string("out/host/kernels/") + name + ".bc";
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return bytes;
    uint8_t buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    fclose(f);
    return bytes;
}

// NAME.ll compiled here against its fixture B: every field of the Kernel, the words, and what the compiler reported
// about the resources; NEEDLE is an instruction the listing must carry. Returns the compiled kernel for more checks.
std::unique_ptr<CompiledKernel> compareWithFixture(const char* name, const Kernel& b, const char* entry, const char* needle) {
    std::vector<uint8_t> bc = bitcodeOf(name);
    // a build with the compiler and no bitcode is a broken build, not a pass
    CHECK(!bc.empty());
    if (bc.empty()) {
        std::printf("       out/host/kernels/%s.bc is missing (make test MESA=1 assembles it)\n", name);
        return nullptr;
    }
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why);
    CHECK(k != nullptr);
    if (k == nullptr) {
        std::printf("       %s: %s\n", name, why.c_str());
        return nullptr;
    }
    const Kernel& a = k->kernel;
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
    // the texture fields: the compiler's walk of the NIR against the fixture writer's, slot by slot
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        const KernelTexture& x = a.textures[i];
        const KernelTexture& y = b.textures[i];
        bool same = x.kind == y.kind && x.dim == y.dim && x.isArray == y.isArray && x.element == y.element && x.bits == y.bits &&
                    x.read == y.read && x.write == y.write && x.pad == y.pad;
        CHECK(same);
        if (!same) {
            std::fprintf(stderr, "  %s: texture slot %u compiled {%u, %u, %u, %u, %u, %u, %u, %u}, fixture {%u, %u, %u, %u, %u, %u, %u, %u}\n", name, i,
                         x.kind, x.dim, x.isArray, x.element, x.bits, x.read, x.write, x.pad, y.kind, y.dim, y.isArray, y.element, y.bits, y.read,
                         y.write, y.pad);
        }
    }
    CHECK_EQ(a.numTextureSlots, b.numTextureSlots);
    CHECK_EQ(a.samplerMask, b.samplerMask);
    // the resources the front end reported (what was declared) against what the fixture says the code does with them:
    // a declared texture is the slot's sampled kind, a declared image its image kind, a declared sampler a bit of the mask
    for (const Resource& r : k->resources) {
        if (r.kind == Resource::kTexture || r.kind == Resource::kImage) {
            CHECK(r.index < kMaxTextureSlots);
            if (r.index >= kMaxTextureSlots) continue;
            const KernelTexture& t = b.textures[r.index];
            CHECK_EQ(t.kind, r.kind == Resource::kTexture ? kTexSampled : kTexImage);
            CHECK_EQ(r.textureDim, t.dim);
            CHECK_EQ(r.textureArray, t.isArray);
            CHECK_EQ(r.element, t.element);
            CHECK(!r.nullTexture);
            CHECK(!r.typeName.empty());
        } else if (r.kind == Resource::kSampler) {
            CHECK((b.samplerMask >> r.index) & 1);
            CHECK(!r.inlineSampler);
        }
    }
    CHECK(k->name == entry);
    CHECK(k->listing.find(needle) != std::string::npos);
    return k;
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
    compareWithFixture("fill_id", kernels::fill_id_air, "fill_id", "stib");
    // and a module that is not a kernel is refused by name, not compiled into something else
    uint8_t garbage[16] = {'B', 'C', 0xc0, 0xde, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(compileAir(garbage, sizeof garbage, &why) == nullptr);
    CHECK(why.find("refused") != std::string::npos);
}

// The three texture kernels: the fixture writer's walk of the NIR (tools/mesa/ir3-dump.patch) and the core's
// (src/compiler.cpp) must name the same slots, dimensions, element classes, widths and samplers, or the dispatch
// would bind a kernel compiled on the phone otherwise than the one the fixture describes
TEST(compiler_matches_the_texture_fixtures) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the texture fixture comparison did not run)\n");
        return;
    }
    auto fill = compareWithFixture("tex_fill", kernels::tex_fill_air, "tex_fill", "stib.b.typed.2d.u32.4");
    if (fill) {
        // the image is the one resource with a texture kind; c is a constant buffer
        unsigned images = 0, textures = 0, samplers = 0;
        for (const Resource& r : fill->resources) {
            images += r.kind == Resource::kImage;
            textures += r.kind == Resource::kTexture;
            samplers += r.kind == Resource::kSampler;
        }
        CHECK_EQ(images, 1u);
        CHECK_EQ(textures, 0u);
        CHECK_EQ(samplers, 0u);
    }
    auto copy = compareWithFixture("tex_copy", kernels::tex_copy_air, "tex_copy", "isaml");
    if (copy) {
        bool inAt1 = false, outAt0 = false;
        for (const Resource& r : copy->resources) {
            inAt1 = inAt1 || (r.kind == Resource::kTexture && r.index == 1 && r.read && !r.write);
            outAt0 = outAt0 || (r.kind == Resource::kImage && r.index == 0 && r.write);
        }
        CHECK(inAt1);
        CHECK(outAt0);
        CHECK(copy->listing.find("t#1") != std::string::npos);
    }
    auto sample = compareWithFixture("tex_sample_gid", kernels::tex_sample_gid_air, "tex_sample_gid", "saml");
    if (sample) {
        bool samplerAt0 = false;
        for (const Resource& r : sample->resources) samplerAt0 = samplerAt0 || (r.kind == Resource::kSampler && r.index == 0);
        CHECK(samplerAt0);
        CHECK(sample->listing.find("s#0, t#0") != std::string::npos);
    }
    // the half image: the store typed by its VALUE (f16), the slot's class float as declared, 16-bit values
    auto half = compareWithFixture("tex_fill_h", kernels::tex_fill_h_air, "tex_fill_h", "stib.b.typed.2d.f16.4");
    if (half) {
        CHECK_EQ(half->kernel.textures[0].element, uint8_t(kElemFloat));
        CHECK_EQ(half->kernel.textures[0].bits, 16u);
        bool declared = false;
        for (const Resource& r : half->resources) declared = declared || (r.kind == Resource::kImage && r.typeName == "texture2d<half, write>");
        CHECK(declared);
    }
}

// What Apple declared and what the instructions do must name the same class: until 29 October the front end typed
// every texture write by the call's void return type, so a texture2d<half, write> stored u16 and the dispatch refused
// RGBA16Float for it. The compiler refuses such a kernel now, naming both; here the disagreement is made on purpose by
// rewriting the declaration in the module's own bytes (the type name is a plain string in the bitcode's string table).
TEST(compiler_refuses_a_declaration_that_disagrees_with_the_instructions) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("tex_fill_h");
    CHECK(!bc.empty());
    const std::string from = "texture2d<half, write>", to = "texture2d<uint, write>";
    auto it = std::search(bc.begin(), bc.end(), from.begin(), from.end());
    CHECK(it != bc.end());   // the declaration must be there as bytes, or this test checks nothing
    if (it == bc.end()) return;
    std::copy(to.begin(), to.end(), it);
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why);
    CHECK(k == nullptr);
    CHECK(why.find("declares texture 0 as texture2d<uint, write> but its instructions move float values") != std::string::npos);
    if (k != nullptr || why.find("declares texture 0") == std::string::npos) std::printf("       %s\n", why.c_str());
}

// --- the texture kernels a second way: the NIR walk's numbers as the listings predict them, with no fixture in between ---
namespace {
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
