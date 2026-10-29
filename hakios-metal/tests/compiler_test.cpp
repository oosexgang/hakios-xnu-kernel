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
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_flat_vs_air.hpp"
#include "hakios-metal/a6xx.hpp"
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

// every field of a Kernel A compiled here against its fixture B, the words included (NAME for the messages)
void compareKernelFields(const char* name, const Kernel& a, const Kernel& b) {
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
    compareKernelFields(name, a, b);
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

// --- the vertex and fragment stages ----------------------------------------------------------------------------------------
namespace {
// NAME.ll compiled here as a Stage against its fixture B (tools/mesa/ir3-dump.patch's emit_stage_fields): the Kernel part
// field for field as above, then every field of render.hpp's Stage. Returns the compiled stage, shared, for the link tests.
std::shared_ptr<CompiledStage> compareStageWithFixture(const char* name, const Stage& b, const char* entry, const char* needle) {
    std::vector<uint8_t> bc = bitcodeOf(name);
    CHECK(!bc.empty());
    if (bc.empty()) {
        std::printf("       out/host/kernels/%s.bc is missing (make test MESA=1 assembles it)\n", name);
        return nullptr;
    }
    std::string why;
    std::shared_ptr<CompiledStage> s = compileStage(bc.data(), bc.size(), &why);
    CHECK(s != nullptr);
    if (s == nullptr) {
        std::printf("       %s: %s\n", name, why.c_str());
        return nullptr;
    }
    const Stage& a = s->stage;
    compareKernelFields(name, a.k, b.k);
    CHECK_EQ(a.kind, b.kind);
    CHECK_EQ(a.numInputs, b.numInputs);
    for (uint32_t i = 0; i < kMaxStageInputs; i++) {
        const StageInput& x = a.inputs[i];
        const StageInput& y = b.inputs[i];
        bool same = x.slot == y.slot && x.regid == y.regid && x.compmask == y.compmask && x.inloc == y.inloc && x.sysval == y.sysval && x.bary == y.bary &&
                    x.flat == y.flat && x.half == y.half;
        CHECK(same);
        if (!same) {
            std::fprintf(stderr, "  %s: input %u compiled {%u, 0x%02x, 0x%x, %u, %u, %u, %u, %u}, fixture {%u, 0x%02x, 0x%x, %u, %u, %u, %u, %u}\n", name, i,
                         x.slot, x.regid, x.compmask, x.inloc, x.sysval, x.bary, x.flat, x.half, y.slot, y.regid, y.compmask, y.inloc, y.sysval, y.bary,
                         y.flat, y.half);
        }
    }
    CHECK_EQ(a.numOutputs, b.numOutputs);
    for (uint32_t i = 0; i < kMaxStageOutputs; i++) {
        const StageOutput& x = a.outputs[i];
        const StageOutput& y = b.outputs[i];
        bool same = x.slot == y.slot && x.regid == y.regid && x.half == y.half && x.pad == y.pad;
        CHECK(same);
        if (!same) {
            std::fprintf(stderr, "  %s: output %u compiled {%u, 0x%02x, %u, %u}, fixture {%u, 0x%02x, %u, %u}\n", name, i, x.slot, x.regid, x.half, x.pad,
                         y.slot, y.regid, y.half, y.pad);
        }
    }
    CHECK_EQ(a.numAttributes, b.numAttributes);
    CHECK_EQ(a.vertexIdRegid, b.vertexIdRegid);
    CHECK_EQ(a.instanceIdRegid, b.instanceIdRegid);
    CHECK_EQ(a.positionRegid, b.positionRegid);
    CHECK_EQ(a.pointSizeRegid, b.pointSizeRegid);
    CHECK_EQ(a.clipMask, b.clipMask);
    CHECK_EQ(a.cullMask, b.cullMask);
    CHECK_EQ(a.writesLayer, b.writesLayer);
    CHECK_EQ(a.writesViewport, b.writesViewport);
    CHECK_EQ(a.totalIn, b.totalIn);
    CHECK_EQ(a.fragCoordRegid, b.fragCoordRegid);
    CHECK_EQ(a.fragcoordCompmask, b.fragcoordCompmask);
    for (uint32_t i = 0; i < kIjCount; i++) CHECK_EQ(a.ijRegid[i], b.ijRegid[i]);
    CHECK_EQ(a.faceRegid, b.faceRegid);
    CHECK_EQ(a.sampleIdRegid, b.sampleIdRegid);
    CHECK_EQ(a.sampleMaskInRegid, b.sampleMaskInRegid);
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        CHECK_EQ(a.colorRegid[i], b.colorRegid[i]);
        CHECK_EQ(a.colorHalf[i], b.colorHalf[i]);
    }
    CHECK_EQ(a.numColorOutputs, b.numColorOutputs);
    CHECK_EQ(a.depthRegid, b.depthRegid);
    CHECK_EQ(a.sampleMaskRegid, b.sampleMaskRegid);
    CHECK_EQ(a.stencilRefRegid, b.stencilRefRegid);
    CHECK_EQ(a.writesPos, b.writesPos);
    CHECK_EQ(a.writesSmask, b.writesSmask);
    CHECK_EQ(a.writesStencilref, b.writesStencilref);
    CHECK_EQ(a.dualSrcBlend, b.dualSrcBlend);
    CHECK_EQ(a.hasKill, b.hasKill);
    CHECK_EQ(a.perSamp, b.perSamp);
    CHECK_EQ(a.readsPrimid, b.readsPrimid);
    CHECK_EQ(a.fbRead, b.fbRead);
    CHECK_EQ(a.needPixlod, b.needPixlod);
    CHECK_EQ(a.needFullQuad, b.needFullQuad);
    CHECK_EQ(a.numSamplerPrefetch, b.numSamplerPrefetch);
    // the pads a fixture writes as zeros
    for (uint32_t i = 0; i < 3; i++) CHECK_EQ(a.pad0[i], 0u);
    for (uint32_t i = 0; i < 3; i++) CHECK_EQ(a.pad1[i], 0u);
    for (uint32_t i = 0; i < 2; i++) CHECK_EQ(a.pad2[i], 0u);
    CHECK(s->name == entry);
    CHECK(s->listing.find(needle) != std::string::npos);
    if (s->listing.find(needle) == std::string::npos) std::printf("       %s: no \"%s\" in the listing\n", name, needle);
    return s;
}

// a copy of a compiled stage to mutate, its Kernel's pointers re-aimed at the copy's own vectors
std::shared_ptr<CompiledStage> copyOf(const std::shared_ptr<CompiledStage>& s) {
    auto c = std::make_shared<CompiledStage>(*s);
    c->stage.k.words = c->words.data();
    c->stage.k.immediates = c->immediates.empty() ? nullptr : c->immediates.data();
    c->stage.k.name = c->name.c_str();
    return c;
}

void expectRefusal(const std::string& got, const char* want) {
    CHECK(got == want);
    if (got != want) std::fprintf(stderr, "  got      \"%s\"\n  expected \"%s\"\n", got.c_str(), want);
}

// the four stages compiled once for the tests below (each test re-checks it has them)
std::shared_ptr<CompiledStage>& stage(const char* name) {
    static std::shared_ptr<CompiledStage> flatVs, flatFs, colorVs, colorFs;
    if (!std::strcmp(name, "tri_flat_vs")) return flatVs;
    if (!std::strcmp(name, "tri_flat_fs")) return flatFs;
    if (!std::strcmp(name, "tri_color_vs")) return colorVs;
    return colorFs;
}

bool stagesCompiled() {
    bool all = true;
    for (const char* name : {"tri_flat_vs", "tri_flat_fs", "tri_color_vs", "tri_color_fs"}) {
        if (stage(name) == nullptr) {
            std::vector<uint8_t> bc = bitcodeOf(name);
            std::string why;
            if (!bc.empty()) stage(name) = compileStage(bc.data(), bc.size(), &why);
            if (stage(name) == nullptr) std::printf("       %s: %s\n", name, bc.empty() ? "no bitcode" : why.c_str());
        }
        all = all && stage(name) != nullptr;
    }
    CHECK(all);
    return all;
}
}  // namespace

// The four render stages compiled in-process against the fixtures the patched ir3_compiler wrote: the same words, the
// same tables, the same regids -- the fixture writer and compileStage run the same passes in the same order (turnip's)
TEST(compiler_matches_the_stage_fixtures) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the stage fixture comparison did not run)\n");
        return;
    }
    auto flatVs = compareStageWithFixture("tri_flat_vs", kernels::tri_flat_vs_air, "tri_flat_vs", "sel.b32");
    auto flatFs = compareStageWithFixture("tri_flat_fs", kernels::tri_flat_fs_air, "tri_flat_fs", "mov.u32u32 r0.x, 0x3e008081");
    auto colorVs = compareStageWithFixture("tri_color_vs", kernels::tri_color_vs_air, "tri_color_vs", "sel.b32");
    auto colorFs = compareStageWithFixture("tri_color_fs", kernels::tri_color_fs_air, "tri_color_fs", "bary.f");
    stage("tri_flat_vs") = flatVs;
    stage("tri_flat_fs") = flatFs;
    stage("tri_color_vs") = colorVs;
    stage("tri_color_fs") = colorFs;
    // no buffer, texture or sampler in any of them: the first draw's shape
    for (const auto& s : {flatVs, flatFs, colorVs, colorFs}) {
        if (s == nullptr) continue;
        CHECK(s->resources.empty());
        CHECK_EQ(s->stage.k.numUboRanges, 0u);
        CHECK_EQ(s->stage.k.numSsbos, 0u);
        CHECK_EQ(s->stage.k.numUbos, 0u);
        CHECK_EQ(s->stage.k.numTextureSlots, 0u);
    }
    // the varyings' names: the VS side carries the mangled name the front end recorded, VAR0 first; the FS side is
    // left empty because air_shader_info only has air.arg_name ("color") for it, which cannot match the VS's string
    // (src/compiler.cpp says so; air-vs-fs.md's varying-names gap)
    if (colorVs != nullptr) {
        CHECK_EQ(colorVs->varyingNames.size(), size_t(1));
        if (!colorVs->varyingNames.empty()) CHECK(colorVs->varyingNames[0] == "generated(5colorDv4_f)");
    }
    if (flatVs != nullptr) CHECK(flatVs->varyingNames.empty());
    if (colorFs != nullptr) CHECK(colorFs->varyingNames.empty());
    if (flatFs != nullptr) CHECK(flatFs->varyingNames.empty());
}

// a kernel through compileStage and a stage through compileAir: each refused by name, not compiled into the other shape
TEST(compiler_refuses_a_kernel_as_a_stage_and_a_stage_as_a_kernel) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> kernel = bitcodeOf("fill_id"), vertex = bitcodeOf("tri_flat_vs"), fragment = bitcodeOf("tri_color_fs");
    CHECK(!kernel.empty() && !vertex.empty() && !fragment.empty());
    if (kernel.empty() || vertex.empty() || fragment.empty()) return;
    std::string why;
    CHECK(compileStage(kernel.data(), kernel.size(), &why) == nullptr);
    expectRefusal(why, "fill_id is a kernel: use compileAir");
    CHECK(compileAir(vertex.data(), vertex.size(), &why) == nullptr);
    expectRefusal(why, "tri_flat_vs is not a compute kernel; a vertex or fragment function goes through compileStage");
    CHECK(compileAir(fragment.data(), fragment.size(), &why) == nullptr);
    expectRefusal(why, "tri_color_fs is not a compute kernel; a vertex or fragment function goes through compileStage");
    // and a module that is not AIR at all
    uint8_t garbage[16] = {'B', 'C', 0xc0, 0xde, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(compileStage(garbage, sizeof garbage, &why) == nullptr);
    CHECK(why.find("the front end refused it") != std::string::npos);
}

// linkProgram over the four: the Linkage air-vs-fs.md worked by hand, held to Mesa's ir3_link_shaders inside
TEST(compiler_links_the_two_programs) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the link did not run)\n");
        return;
    }
    if (!stagesCompiled()) return;
    std::string why;
    auto flat = linkProgram(stage("tri_flat_vs"), stage("tri_flat_fs"), PixelFormat::kRGBA8Unorm, &why);
    CHECK(flat != nullptr);
    if (flat == nullptr) {
        std::printf("       tri_flat: %s\n", why.c_str());
    } else {
        const Linkage& l = flat->program.link;
        CHECK(flat->name == "tri_flat_vs+tri_flat_fs");
        CHECK(std::strcmp(flat->program.name, "tri_flat_vs+tri_flat_fs") == 0);
        CHECK_EQ(flat->program.vs.kind, uint8_t(kStageVertex));
        CHECK_EQ(flat->program.fs.kind, uint8_t(kStageFragment));
        CHECK_EQ(l.cnt, 1u);
        CHECK_EQ(l.maxLoc, 4u);
        CHECK_EQ(l.positionLoc, 0u);
        CHECK_EQ(l.var[0].slot, 0u);
        CHECK_EQ(l.var[0].regid, flat->program.vs.positionRegid);
        CHECK_EQ(l.var[0].compmask, 0xfu);
        CHECK_EQ(l.var[0].loc, 0u);
        for (uint32_t i = 0; i < 4; i++) CHECK_EQ(l.varmask[i], 0u);
        CHECK_EQ(l.mrtComponents, 0xfu);
        // the program's stages are the compiled ones, words and all
        CHECK(flat->program.vs.k.words == stage("tri_flat_vs")->words.data());
        CHECK(flat->vs == stage("tri_flat_vs") && flat->fs == stage("tri_flat_fs"));
    }
    auto color = linkProgram(stage("tri_color_vs"), stage("tri_color_fs"), PixelFormat::kRGBA8Unorm, &why);
    CHECK(color != nullptr);
    if (color == nullptr) {
        std::printf("       tri_color: %s\n", why.c_str());
    } else {
        const Linkage& l = color->program.link;
        CHECK_EQ(l.cnt, 2u);
        CHECK_EQ(l.maxLoc, 8u);
        CHECK_EQ(l.positionLoc, 4u);
        CHECK_EQ(l.var[0].slot, 32u);   // VAR0 at the FS's inloc 0, from the VS's colour register
        CHECK_EQ(l.var[0].regid, color->program.vs.outputs[1].regid);
        CHECK_EQ(l.var[0].compmask, 0xfu);
        CHECK_EQ(l.var[0].loc, 0u);
        CHECK_EQ(l.var[1].slot, 0u);    // the position after it
        CHECK_EQ(l.var[1].regid, color->program.vs.positionRegid);
        CHECK_EQ(l.var[1].compmask, 0xfu);
        CHECK_EQ(l.var[1].loc, 4u);
        CHECK_EQ(l.varmask[0], 0xfu);
        CHECK_EQ(l.varmask[1], 0u);
        for (uint32_t i = 0; i < 8; i++) CHECK_EQ(l.interpMode[i], 0u);
        for (uint32_t i = 0; i < 8; i++) CHECK_EQ(l.psReplMode[i], 0u);
        CHECK_EQ(l.mrtComponents, 0xfu);
    }
    // an unused varying is fine; a varying the VS does not write is not
    auto unused = linkProgram(stage("tri_color_vs"), stage("tri_flat_fs"), PixelFormat::kRGBA8Unorm, &why);
    CHECK(unused != nullptr);
    if (unused != nullptr) CHECK_EQ(unused->program.link.cnt, 1u);
    CHECK(linkProgram(stage("tri_flat_vs"), stage("tri_color_fs"), PixelFormat::kRGBA8Unorm, &why) == nullptr);
    expectRefusal(why, "the fragment stage reads varying slot 32 (VAR0), which the vertex stage does not write");
    // the formats: a float4 into the float formats, not into the integer ones
    CHECK(linkProgram(stage("tri_flat_vs"), stage("tri_flat_fs"), PixelFormat::kRGBA16Float, &why) != nullptr);
    CHECK(linkProgram(stage("tri_flat_vs"), stage("tri_flat_fs"), PixelFormat::kRGBA32Float, &why) != nullptr);
    CHECK(linkProgram(stage("tri_flat_vs"), stage("tri_flat_fs"), PixelFormat::kR32Uint, &why) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs returns floats into R32Uint, which holds integers");
    CHECK(linkProgram(stage("tri_flat_vs"), stage("tri_flat_fs"), PixelFormat::kRGBA32Uint, &why) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs returns floats into RGBA32Uint, which holds integers");
    CHECK(linkProgram(stage("tri_flat_vs"), stage("tri_flat_fs"), PixelFormat(999), &why) == nullptr);
    expectRefusal(why, "pixel format 999 is not served");
    // the stages the wrong way round
    CHECK(linkProgram(stage("tri_flat_fs"), stage("tri_flat_fs"), PixelFormat::kRGBA8Unorm, &why) == nullptr);
    expectRefusal(why, "tri_flat_fs is not a vertex stage");
    CHECK(linkProgram(stage("tri_flat_vs"), stage("tri_flat_vs"), PixelFormat::kRGBA8Unorm, &why) == nullptr);
    expectRefusal(why, "tri_flat_vs is not a fragment stage");
    CHECK(linkProgram(nullptr, stage("tri_flat_fs"), PixelFormat::kRGBA8Unorm, &why) == nullptr);
    expectRefusal(why, "linkProgram needs a vertex and a fragment stage; one is missing");
}

// Each thing the first draw does not serve, made from a compiled stage by one change, and the sentence linkProgram
// refuses it with -- so that every refusal render.hpp names is known to be looked at
TEST(compiler_link_refuses_what_the_first_draw_does_not_serve) {
    if (!hasCompiler()) return;
    if (!stagesCompiled()) return;
    std::string why;
    const auto& vs = stage("tri_flat_vs");
    const auto& fs = stage("tri_flat_fs");
    auto link = [&](const std::shared_ptr<CompiledStage>& v, const std::shared_ptr<CompiledStage>& f, PixelFormat format = PixelFormat::kRGBA8Unorm) {
        return linkProgram(v, f, format, &why);
    };
    // the unchanged pair links: every refusal below is the one change
    CHECK(link(vs, fs) != nullptr);
    auto m = copyOf(vs);
    m->stage.numAttributes = 1;
    CHECK(link(m, fs) == nullptr);
    expectRefusal(why, "the vertex stage tri_flat_vs reads 1 [[stage_in]] attribute(s): vertex descriptors are not served yet");
    m = copyOf(vs);
    m->stage.k.numSsbos = 1;
    CHECK(link(m, fs) == nullptr);
    expectRefusal(why, "the vertex stage tri_flat_vs binds buffers (0 pushed constant range(s), 1 device buffer(s), 0 constant buffer(s)): the first draw binds none in either stage");
    m = copyOf(fs);
    m->stage.k.numUboRanges = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs binds buffers (1 pushed constant range(s), 0 device buffer(s), 0 constant buffer(s)): the first draw binds none in either stage");
    m = copyOf(fs);
    m->stage.k.numUbos = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs binds buffers (0 pushed constant range(s), 0 device buffer(s), 1 constant buffer(s)): the first draw binds none in either stage");
    m = copyOf(fs);
    m->stage.k.textures[3].kind = kTexSampled;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs samples or stores through textures: textures and samplers in a draw are step two");
    m = copyOf(vs);
    m->stage.k.samplerMask = 1;
    CHECK(link(m, fs) == nullptr);
    expectRefusal(why, "the vertex stage tri_flat_vs samples or stores through textures: textures and samplers in a draw are step two");
    m = copyOf(fs);
    m->stage.numColorOutputs = 2;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes 2 colour output(s): one target, [[color(0)]], is served");
    m = copyOf(fs);
    m->stage.colorRegid[0] = a6xx::kRegidNone;   // [[color(1)]] alone would look like this: one output, not at 0
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes 1 colour output(s) and not [[color(0)]]: one target, [[color(0)]], is served");
    m = copyOf(fs);
    m->stage.depthRegid = 0;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes depth: no depth attachment is served");
    m = copyOf(fs);
    m->stage.writesPos = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes depth: no depth attachment is served");
    m = copyOf(fs);
    m->stage.stencilRefRegid = 4;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes a stencil reference: no stencil attachment is served");
    m = copyOf(fs);
    m->stage.sampleMaskRegid = 4;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes a sample mask: no multisampling is served");
    m = copyOf(fs);
    m->stage.fbRead = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs reads its render target ([[color(n)]] as an argument, framebuffer fetch): not served");
    m = copyOf(fs);
    m->stage.perSamp = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs runs per sample: no multisampling is served");
    m = copyOf(fs);
    m->stage.dualSrcBlend = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes a dual-source blend output: blending is off in the first draw");
    m = copyOf(fs);
    m->stage.readsPrimid = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs reads the primitive id: not served");
    m = copyOf(fs);
    m->stage.numSamplerPrefetch = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs has 1 texture prefetch(es): the draw does not emit SP_FS_PREFETCH_CMD yet");
    // the colour class the other way: an integer output (what a uint4 return reads as) into the float format, and into R32Uint
    m = copyOf(fs);
    m->stage.outputs[0].pad = kElemUint;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs returns integers into RGBA8Unorm, which holds floats");
    CHECK(link(vs, m, PixelFormat::kR32Uint) != nullptr);
    // the constant budget: 512 vec4 for the two stages together (ir3_compiler.c:185), the sum at the edge passes
    m = copyOf(vs);
    m->stage.k.constlen = 512 - fs->stage.k.constlen;
    CHECK(link(m, fs) != nullptr);
    m->stage.k.constlen = 512 - fs->stage.k.constlen + 4;
    CHECK(link(m, fs) == nullptr);
    expectRefusal(why, ("the two stages need 516 vec4 of constants together (" + std::to_string(512 - fs->stage.k.constlen + 4) + " + " +
                        std::to_string(fs->stage.k.constlen) + "): the pipeline has 512 (ir3_compiler.c max_const_pipeline)")
                           .c_str());
    // and what render::link refuses comes through with its own sentence
    m = copyOf(vs);
    m->stage.positionRegid = a6xx::kRegidNone;
    CHECK(link(m, fs) == nullptr);
    expectRefusal(why, "tri_flat_vs writes no position: nothing to rasterize");
}
