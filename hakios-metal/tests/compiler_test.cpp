// The compiler inside the driver against the compiler outside it: each tools/kernels/*.ll compiled here must be, field
// for field and word for word, the fixture tools/mesa/ir3-dump.patch had Mesa's ir3_compiler write (tests/fixtures/
// *_air.hpp) -- the texture fields included, which hold the core's walk of the NIR (src/compiler.cpp) to the patch's.
// Without Mesa's libraries in the build (no MESA_BUILD) the compiler is a refusal, and that is checked instead.
#include <atomic>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hakios-metal/compiler.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/gpu.hpp"
#include "fixtures/argbuf_res_air.hpp"
#include "hakios-metal/hakios_metal.h"
#include "fixtures/fill_id_air.hpp"
#include "fixtures/tex_copy_air.hpp"
#include "fixtures/tex_fill_air.hpp"
#include "fixtures/tex_size_h_air.hpp"
#include "fixtures/tg_arg_air.hpp"
#include "fixtures/ptr_phi_air.hpp"
#include "fixtures/tex_sample_gid_air.hpp"
#include "fixtures/tex_fill_h_air.hpp"
#include "fixtures/tri_dbuf_fs_bc.hpp"
#include "fixtures/tri_dwrite_fs_bc.hpp"
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_cflat_fs_air.hpp"
#include "fixtures/tri_tex_fs_air.hpp"
#include "fixtures/tri_null_fs_air.hpp"
#include "fixtures/tri_mrt_fs_air.hpp"
#include "fixtures/tri_fb_fs_air.hpp"
#include "fixtures/tri_upos_vs_air.hpp"
#include "fixtures/tri_dpos_vs_air.hpp"
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
    CHECK_EQ(a.constantDataUbo, b.constantDataUbo);
    // the texture fields: the compiler's walk of the NIR against the fixture writer's, slot by slot
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        const KernelTexture& x = a.textures[i];
        const KernelTexture& y = b.textures[i];
        bool same = x.kind == y.kind && x.dim == y.dim && x.isArray == y.isArray && x.element == y.element && x.bits == y.bits &&
                    x.read == y.read && x.write == y.write && x.isNull == y.isNull;
        CHECK(same);
        if (!same) {
            std::fprintf(stderr, "  %s: texture slot %u compiled {%u, %u, %u, %u, %u, %u, %u, %u}, fixture {%u, %u, %u, %u, %u, %u, %u, %u}\n", name, i,
                         x.kind, x.dim, x.isArray, x.element, x.bits, x.read, x.write, x.isNull, y.kind, y.dim, y.isArray, y.element, y.bits, y.read,
                         y.write, y.isNull);
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

// A size query is a tex too (txs), its result an integer whatever the texture holds: the texture's class comes from the
// sample, not from the query that follows it in tex_size_h -- else a half texture is called int and the kernel refused
// for a disagreement with its own declaration (mediaserverd's splitFg_rgba, which samples a texture and asks its size)
TEST(a_size_query_after_a_sample_leaves_the_texture_its_class) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the query typing did not run)\n");
        return;
    }
    auto k = compareWithFixture("tex_size_h", kernels::tex_size_h_air, "tex_size_h", "getsize");
    if (k) {
        CHECK_EQ(k->kernel.textures[0].element, uint8_t(kElemFloat));
        CHECK_EQ(k->kernel.textures[0].kind, uint8_t(kTexSampled));
    }
}

// A [[threadgroup(0)]] argument starts past the kernel's static threadgroup arrays: tg_arg's s[64] is 256 bytes from 0,
// so the argument is at 256 and the dispatch is given 256 + the encoder's length. At 0 (as it was) the two alias.
TEST(a_threadgroup_argument_starts_past_the_static_arrays) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the threadgroup layout did not run)\n");
        return;
    }
    auto k = compareWithFixture("tg_arg", kernels::tg_arg_air, "tg_arg", "l[r1.z+256]");
    if (k) {
        CHECK(k->hasThreadgroupArg);
        CHECK_EQ(k->threadgroupArgBase, 256u);
        CHECK_EQ(k->kernel.sharedSize, 256u);
    }
    // the twin: a kernel with no threadgroup argument says so, and keeps its base at 0
    auto fill = compareWithFixture("tex_fill", kernels::tex_fill_air, "tex_fill", "stib");
    if (fill) CHECK(!fill->hasThreadgroupArg);
}

// Pointer phis into one buffer at run-time offsets (a branch's two GEPs, a pointer stepped round a loop) translate:
// the front end carries the offset in a register; it refused both before (tools/air/chosencheck.sh holds the twins
// that are still refused: two buffers, and a loop whose back edge swaps the buffer)
TEST(a_pointer_phi_into_one_buffer_compiles) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the pointer phis did not run)\n");
        return;
    }
    auto k = compareWithFixture("ptr_phi", kernels::ptr_phi_air, "ptr_phi", "ldib");
    if (k) CHECK_EQ(k->kernel.numSsbos, 2u);
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
    for (uint32_t i = 0; i < kMaxSamplerPrefetch; i++) {
        const SamplerPrefetch& x = a.prefetch[i];
        const SamplerPrefetch& y = b.prefetch[i];
        bool same = x.src == y.src && x.sampId == y.sampId && x.texId == y.texId && x.dst == y.dst && x.wrmask == y.wrmask && x.half == y.half &&
                    x.cmd == y.cmd && x.bindless == y.bindless;
        CHECK(same);
        if (!same) {
            std::fprintf(stderr, "  %s: prefetch %u compiled {0x%02x, %u, %u, 0x%02x, 0x%x, %u, %u, %u}, fixture {0x%02x, %u, %u, 0x%02x, 0x%x, %u, %u, %u}\n", name, i,
                         x.src, x.sampId, x.texId, x.dst, x.wrmask, x.half, x.cmd, x.bindless, y.src, y.sampId, y.texId, y.dst, y.wrmask, y.half, y.cmd, y.bindless);
        }
    }
    CHECK_EQ(a.prefetchEndOfQuad, b.prefetchEndOfQuad);
    // the pads a fixture writes as zeros
    for (uint32_t i = 0; i < 3; i++) CHECK_EQ(a.pad0[i], 0u);
    for (uint32_t i = 0; i < 3; i++) CHECK_EQ(a.pad1[i], 0u);
    for (uint32_t i = 0; i < 2; i++) CHECK_EQ(a.pad2[i], 0u);
    for (uint32_t i = 0; i < 3; i++) CHECK_EQ(a.pad3[i], 0u);
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
    // the constant-buffer fragment stage: its pushed range compiled here as the fixture writer wrote it (block 0, 16 dwords at c0)
    auto cflatFs = compareStageWithFixture("tri_cflat_fs", kernels::tri_cflat_fs_air, "tri_cflat_fs", "mov.u32u32 r0.x, c0.x");
    if (cflatFs) {
        CHECK_EQ(cflatFs->stage.k.numUboRanges, 1u);
        CHECK_EQ(cflatFs->stage.k.uboRanges[0].block, 0u);
        CHECK_EQ(cflatFs->stage.k.uboRanges[0].sizeDwords, 16u);
        CHECK_EQ(cflatFs->stage.k.numUbos, 0u);
    }
    // the sampling fragment stage: one texture, one sampler, and the sample made a prefetch (into r0.z, all four
    // components, TEX_PREFETCH_SAM) -- the shape tri_tex_fs.ll was written to produce
    // a constexpr sampler: compiled into the stage's own sampler at the index the front end gave it (0, past the API's
    // none), its words those makeSampler gives Metal's repeat/repeat/clamp-to-edge, nearest, lod [0, 65504]
    {
        std::vector<uint8_t> bc = bitcodeOf("tri_ctex_fs");
        std::string why;
        auto s = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why);
        CHECK(s != nullptr);
        if (s == nullptr) std::printf("       tri_ctex_fs: %s\n", why.c_str());
        if (s != nullptr) {
            CHECK_EQ(s->inlineSamplers.size(), size_t(1));
            SamplerDesc d;
            d.sAddress = 2;
            d.tAddress = 2;
            d.rAddress = 0;
            d.lodMaxClamp = 65504.0f;
            Sampler want;
            CHECK(makeSampler(d, &want, &why));
            if (!s->inlineSamplers.empty()) {
                CHECK_EQ(s->inlineSamplers[0].first, 0u);
                CHECK(memcmp(s->inlineSamplers[0].second.words, want.words, 16) == 0);
            }
            CHECK_EQ(s->stage.k.samplerMask, 1u);
            CHECK_EQ(s->stage.numSamplerPrefetch, 0u);   // a computed coordinate: an ordinary sam
        }
    }
    // vertex pulling from a device buffer: an ldib through IBO 0 of the 3D table
    auto dposVs = compareStageWithFixture("tri_dpos_vs", kernels::tri_dpos_vs_air, "tri_dpos_vs", "ldib");
    if (dposVs) {
        CHECK_EQ(dposVs->stage.k.numSsbos, 1u);
        CHECK_EQ(dposVs->stage.k.numIbos, 1u);
    }
    // vertex pulling from a constant buffer: an ldc through UBO 0, nothing pushed
    auto uposVs = compareStageWithFixture("tri_upos_vs", kernels::tri_upos_vs_air, "tri_upos_vs", "ldc");
    if (uposVs) {
        CHECK_EQ(uposVs->stage.k.numUbos, 1u);
        CHECK_EQ(uposVs->stage.k.numUboRanges, 0u);
    }
    auto texFs = compareStageWithFixture("tri_tex_fs", kernels::tri_tex_fs_air, "tri_tex_fs", "bary.f");
    if (texFs) {
        CHECK_EQ(texFs->stage.k.numTextureSlots, 1u);
        CHECK_EQ(texFs->stage.k.samplerMask, 1u);
        CHECK_EQ(texFs->stage.numSamplerPrefetch, 1u);
        CHECK_EQ(texFs->stage.prefetch[0].dst, 0x02u);
        CHECK_EQ(texFs->stage.prefetch[0].wrmask, 0xfu);
        CHECK_EQ(texFs->stage.prefetch[0].cmd, 1u);
        CHECK_EQ(texFs->stage.colorRegid[0], 0x02u);   // the colour output IS the prefetch's destination
    }
    // the null texture's slot: the 2D sample a prefetch, the 3D one a sam.3d, both through slot 0, which is marked
    auto nullFs = compareStageWithFixture("tri_null_fs", kernels::tri_null_fs_air, "tri_null_fs", "sam.3d");
    if (nullFs) {
        CHECK_EQ(nullFs->stage.k.numTextureSlots, 1u);
        CHECK_EQ(uint32_t(nullFs->stage.k.textures[0].isNull), 1u);
        CHECK_EQ(nullFs->stage.numSamplerPrefetch, 1u);
    }
    // two colour targets: [[color(0)]] in r0, [[color(1)]] in r1, and the pipeline links (MRT is the draw's to serve now)
    auto mrtFs = compareStageWithFixture("tri_mrt_fs", kernels::tri_mrt_fs_air, "tri_mrt_fs", "mov.u32u32 r1.w, 0x3f400000");
    if (mrtFs) {
        CHECK_EQ(mrtFs->stage.numColorOutputs, 2u);
        CHECK_EQ(mrtFs->stage.colorRegid[1], 0x04u);
        if (flatVs) {
            std::string why;
            auto linked = linkProgram(flatVs, mrtFs, PixelFormat::kRGBA8Unorm, &why);
            CHECK(linked != nullptr);
            if (linked == nullptr) std::printf("       %s\n", why.c_str());
            if (linked) CHECK_EQ(linked->program.link.mrtComponents, 0xffu);
        }
    }
    // a framebuffer fetch: lowerFbRead's texel fetch (isaml) of slot 0, marked as colour attachment 0's, the layer and
    // the sample folded to 0, so no VARYING_SLOT_LAYER input and no sample id are left for the draw to refuse
    auto fbFs = compareStageWithFixture("tri_fb_fs", kernels::tri_fb_fs_air, "tri_fb_fs", "isaml (f32)(xyz)");
    if (fbFs) {
        CHECK_EQ(uint32_t(fbFs->stage.fbRead), 1u);
        CHECK_EQ(uint32_t(fbFs->stage.k.textures[0].fbTarget), 1u);
        CHECK_EQ(fbFs->stage.k.numSamp, 1u);
        CHECK_EQ(fbFs->stage.sampleIdRegid, uint32_t(a6xx::kRegidNone));
        bool layer = false;
        for (uint32_t i = 0; i < fbFs->stage.numInputs; i++) layer |= fbFs->stage.inputs[i].slot == 22;   // VARYING_SLOT_LAYER
        CHECK(!layer);
    }
    // a framebuffer fetch beside the front end's null texture (CoreAnimation's fixed_frag_lph_cpf): the null slot sits
    // high in the table, and the fetch takes the lowest free slot, not one past the highest -- which ran past 16
    {
        std::vector<uint8_t> bc = bitcodeOf("tri_nullfb_fs");
        std::string why;
        std::shared_ptr<CompiledStage> s = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why);
        CHECK(s != nullptr);
        if (s == nullptr) std::printf("       tri_nullfb_fs: %s\n", why.c_str());
        if (s) {
            int nullSlot = -1, fbSlot = -1;
            for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
                if (s->stage.k.textures[i].kind != kTexSampled) continue;
                if (s->stage.k.textures[i].isNull) nullSlot = int(i);
                if (s->stage.k.textures[i].fbTarget == 1) fbSlot = int(i);
            }
            CHECK(nullSlot >= 0 && fbSlot >= 0 && nullSlot != fbSlot);
            if (!(nullSlot >= 0 && fbSlot >= 0 && nullSlot != fbSlot)) std::printf("       null slot %d, framebuffer slot %d\n", nullSlot, fbSlot);
            CHECK(s->stage.k.numTextureSlots <= kMaxTextureSlots);
        }
    }
    // [[point_coord]]: lowered to a VARYING_SLOT_PNTC input (ir3 has no case for the system value and stops the process),
    // which the link feeds from the point sprite -- PS_REPL S at its first location, T at its second -- with no VS register
    {
        std::vector<uint8_t> bc = bitcodeOf("tri_pntc_fs");
        std::string why;
        std::shared_ptr<CompiledStage> s = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why);
        CHECK(s != nullptr);
        if (s == nullptr) std::printf("       tri_pntc_fs: %s\n", why.c_str());
        if (s && flatVs) {
            int pntc = -1;
            for (uint32_t i = 0; i < s->stage.numInputs; i++) {
                if (s->stage.inputs[i].slot == 25 && s->stage.inputs[i].bary) pntc = int(i);
            }
            CHECK(pntc >= 0);
            auto linked = linkProgram(flatVs, s, PixelFormat::kRGBA8Unorm, &why);
            CHECK(linked != nullptr);
            if (linked == nullptr) std::printf("       %s\n", why.c_str());
            if (linked && pntc >= 0) {
                const uint32_t loc = s->stage.inputs[pntc].inloc;
                const uint32_t mode = linked->program.link.psReplMode[loc / 16] >> ((loc % 16) * 2);
                CHECK_EQ(mode & 0xfu, uint32_t(a6xx::PS_REPL_S | a6xx::PS_REPL_T << 2));
            }
        }
    }
    // a fragment stage's device buffers moved past the vertex stage's (compileStage's iboBase): compiled with base 1,
    // tri_dbuf_fs counts 2 SSBO slots and addresses the second; with base 0 one, and the first
    {
        std::vector<uint8_t> bc = bitcodeOf("tri_dbuf_fs");
        std::string why;
        auto plain = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why);
        auto based = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why, {}, 1);
        CHECK(plain != nullptr && based != nullptr);
        if (plain == nullptr || based == nullptr) std::printf("       tri_dbuf_fs: %s\n", why.c_str());
        if (plain && based) {
            CHECK_EQ(plain->stage.k.numSsbos, 1u);
            CHECK_EQ(plain->iboBase, 0u);
            CHECK_EQ(based->stage.k.numSsbos, 2u);
            CHECK_EQ(based->iboBase, 1u);
            CHECK(plain->listing != based->listing);   // the IBO index in the load is not the same
        }
    }
    // a vertex stage that writes [[point_size]] links with it after the position, and linkProgram's check against
    // ir3_link_shaders (plus turnip's point size) agrees: CoreAnimation's fixed_vert_lph_gen + fixed_frag_lph_cpf was refused
    // by that check once the size was linked
    {
        std::vector<uint8_t> bc = bitcodeOf("tri_pt_vs");
        std::string why;
        std::shared_ptr<CompiledStage> pt = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why);
        CHECK(pt != nullptr);
        if (pt != nullptr && flatFs != nullptr) {
            auto linked = linkProgram(pt, flatFs, PixelFormat::kRGBA8Unorm, &why);
            CHECK(linked != nullptr);
            if (linked == nullptr) std::printf("       %s\n", why.c_str());
            if (linked) CHECK(linked->program.link.psizeLoc != 0xffu);
        }
    }
    // AIR 2.3 writes no air.address_space on a buffer (MapKit's Clear::clear_vertex was refused at "a buffer in address
    // space 0"): the pointer's own address space says which kind it is. The constant one first, then its device twin,
    // which differs from it in the pointer alone -- a front end defaulting to either kind gets one of them wrong.
    for (const auto& [name, kind] : {std::pair<const char*, Resource::Kind>{"tri_oldcb_vs", Resource::kConstantBuffer}, {"tri_olddb_vs", Resource::kDeviceBuffer}}) {
        std::vector<uint8_t> bc = bitcodeOf(name);
        std::string why;
        std::shared_ptr<CompiledStage> old = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why);
        CHECK(old != nullptr);
        if (old == nullptr) {
            std::printf("       %s: %s\n", name, why.c_str());
            continue;
        }
        CHECK_EQ(old->resources.size(), size_t(1));
        if (!old->resources.empty()) {
            CHECK_EQ(uint32_t(old->resources[0].kind), uint32_t(kind));
            CHECK_EQ(old->resources[0].index, 1u);
        }
        CHECK_EQ(old->stage.k.numSsbos != 0, kind == Resource::kDeviceBuffer);
    }
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
    m->stage.numAttributes = 1;   // attributes link: the vertex descriptor is the draw's to judge
    CHECK(link(m, fs) != nullptr);
    m = copyOf(vs);
    m->stage.k.numSsbos = 1;   // a device buffer links: the draw loads the 3D IBO table (and refuses it unbound)
    CHECK(link(m, fs) != nullptr);
    m = copyOf(fs);
    m->stage.k.numUboRanges = 1;   // a pushed range links: the draw serves it (and refuses it there if nothing is bound)
    CHECK(link(vs, m) != nullptr);
    m = copyOf(fs);
    m->stage.k.numUbos = 1;   // an indexed constant buffer links: the draw loads its UBO descriptor (and refuses it unbound)
    CHECK(link(vs, m) != nullptr);
    m = copyOf(fs);
    m->stage.k.textures[3].kind = kTexSampled;   // a sampled texture links in a fragment stage: the draw binds it
    m->stage.k.numTextureSlots = 4;
    CHECK(link(vs, m) != nullptr);
    m = copyOf(fs);
    m->stage.k.textures[1].kind = kTexImage;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs stores through texture 1 as an image: images in a draw (the 3D IBO table) are not served");
    m = copyOf(vs);
    m->stage.k.textures[0].kind = kTexSampled;   // and in a vertex stage (SB6_VS_TEX); an image there is refused as in the FS
    m->stage.k.numTextureSlots = 1;
    m->stage.k.samplerMask = 1;
    CHECK(link(m, fs) != nullptr);
    m->stage.k.textures[0].kind = kTexImage;
    CHECK(link(m, fs) == nullptr);
    expectRefusal(why, "the vertex stage tri_flat_vs stores through texture 0 as an image: images in a draw (the 3D IBO table) are not served");
    m = copyOf(fs);
    m->stage.numColorOutputs = 2;   // [[color(1)]] counted and not written: a gap, which links (the draw serves gaps)
    CHECK(link(vs, m) != nullptr);
    m = copyOf(fs);
    m->stage.numColorOutputs = 9;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes 9 colour output(s): 8 at most are served (the draw's rule)");
    m = copyOf(fs);
    m->stage.depthRegid = 0;   // a stage writing depth links: the draw gives its register to the depth test
    CHECK(link(vs, m) != nullptr);
    m = copyOf(fs);
    m->stage.stencilRefRegid = 4;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes a stencil reference: no stencil attachment is served");
    m = copyOf(fs);
    m->stage.sampleMaskRegid = 4;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs writes a sample mask: no multisampling is served");
    m = copyOf(fs);
    m->stage.fbRead = 1;   // a framebuffer fetch is the draw's to serve now (its slot is checked there): the pipeline links
    CHECK(link(vs, m) != nullptr);
    m = copyOf(fs);
    m->stage.perSamp = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs runs per sample: no multisampling is served");
    m = copyOf(fs);
    m->stage.dualSrcBlend = 1;   // links: the draw takes the second colour as the blend's second source
    CHECK(link(vs, m) != nullptr);
    m = copyOf(fs);
    m->stage.readsPrimid = 1;
    CHECK(link(vs, m) == nullptr);
    expectRefusal(why, "the fragment stage tri_flat_fs reads the primitive id: not served");
    m = copyOf(fs);
    m->stage.numSamplerPrefetch = 1;   // a prefetch links: the draw writes its command (and judges its slots there)
    CHECK(link(vs, m) != nullptr);
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

// The pipeline's function constants reach the module: tri_fc_fs's colour is four literals once they are folded, and
// each set of constants gives its own (the .ll's table). "Supplied as zero" and "not supplied" must differ -- the
// guarded constant's is_function_constant_defined says which -- and a constant the module does not declare is ignored.
TEST(compiler_specialises_a_stage_by_the_pipelines_function_constants) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("tri_fc_fs");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    // the four mov immediates into r0.x..w, in the listing's own words
    auto colour = [&](const FunctionConstants& c, std::string* error) -> std::vector<std::string> {
        std::string why;
        auto s = compileStage(bc.data(), bc.size(), &why, c);
        if (s == nullptr) {
            *error = why;
            return {};
        }
        std::vector<std::string> out;
        for (const char* r : {"r0.x, ", "r0.y, ", "r0.z, ", "r0.w, "}) {
            size_t at = s->listing.find(std::string("mov.u32u32 ") + r);
            out.push_back(at == std::string::npos ? "?" : s->listing.substr(at + 11 + strlen(r), s->listing.find('\n', at) - at - 11 - strlen(r)));
        }
        return out;
    };
    struct Case {
        FunctionConstants constants;
        std::vector<std::string> want;
        const char* what;
    };
    const Case cases[] = {
        {{}, {"0", "0", "0x3e800000", "0x3e800000"}, "nothing supplied"},
        // (7 / 255 folded as 7 * (1 / 255), which is one ulp above the quotient, 0x3ce0e0e1: the store still makes it 7)
        {{{0, 7}, {1, 1}}, {"0x3ce0e0e2", "0x3f800000", "0x3e800000", "0x3f800000"}, "0 = 7, 1 = true"},
        {{{0, 0}}, {"0", "0x3f800000", "0x3e800000", "0x3e800000"}, "0 = 0"},
        {{{1, 1}, {9, 123}}, {"0", "0", "0x3e800000", "0x3f800000"}, "1 = true, and 9, which the module does not declare"},
    };
    for (const Case& c : cases) {
        std::string error;
        std::vector<std::string> got = colour(c.constants, &error);
        CHECK(error.empty());
        CHECK(got == c.want);
        if (got != c.want) {
            std::printf("       %s: %s", c.what, error.c_str());
            for (const auto& g : got) std::printf(" [%s]", g.c_str());
            std::printf("\n");
        }
    }
}

// clz: the front end's uclz, which ir3 cannot translate -- it reported "Unhandled ALU op: uclz" and ended the process,
// this test binary with it. Compiled now through 31 - ufind_msb, and the words carry no uclz left for ir3 to meet.
TEST(a_kernel_counting_leading_zeros_compiles) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: clz did not run)\n");
        return;
    }
    std::vector<uint8_t> bc = bitcodeOf("clz_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) std::printf("       %s\n", why.c_str());
}

// A word taken apart into its bytes and put back (bitcast 1 x 32 bits <-> 4 x 8), as mediaserverd's image kernels do:
// the front end refused it and MPS aborted mediaserverd on the refusal, in a loop. It compiles now, through ir3 too.
// simd_is_uniform (air.is_uniform, mediaserverd's CNN kernels): the front end refused it and MPS aborted the
// process. It compiles through ir3 as a vote now; the probe runs it on the GPU.
TEST(a_kernel_asking_whether_a_value_is_uniform_compiles) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("uniform_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) std::printf("       %s\n", why.c_str());
}

// pack_float_to_unorm4x8 over a half4 (air.pack.unorm4x8.v4f16, VideoToolbox's scaler that the live wallpaper runs):
// refused by the front end until now, and ir3 lowers the pack itself; the probe runs it on the GPU.
TEST(a_kernel_packing_halves_into_bytes_compiles) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("pack_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) std::printf("       %s\n", why.c_str());
}

// a pointer split into a phi per path, each undef over the other's edge (MPS's cnnConvArray_*, which took mediaserverd
// down): the undef is no resource of its own, so the phi is its one defined operand; the probe runs it on the GPU.
TEST(a_kernel_whose_pointer_phis_are_undef_over_an_edge_compiles) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("undef_phi_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) std::printf("       %s\n", why.c_str());
}

// an output declared and never written, ahead of the position (hole_vs, as Resident Evil 4's VSGUI): a hole in ir3's
// output table, slot 0 and no register, which is not the position and not an output to link
TEST(an_unwritten_output_is_not_the_position) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("hole_vs");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto s = compileStage(bc.data(), bc.size(), &why);
    CHECK(s != nullptr);
    if (s == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK(s->stage.positionRegid != a6xx::kRegidNone);
    CHECK_EQ(s->stage.numOutputs, 2u);
    for (uint32_t i = 0; i < s->stage.numOutputs; i++) CHECK(s->stage.outputs[i].regid != a6xx::kRegidNone);
}

// ten constant buffers read at known offsets: ten pushed ranges, past the eight a Kernel held (Resident Evil 4's
// tonemap pushes 11); every one of them kept, with the buffer it comes from
TEST(a_kernel_pushing_ten_constant_ranges_compiles) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("many_ubo_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK_EQ(k->kernel.numUboRanges, 10u);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < k->kernel.numUboRanges; i++) seen |= 1u << k->kernel.uboRanges[i].block;
    CHECK_EQ(seen, 0x7feu);   // buffers 1 to 10
}

// a SIMD-group ballot (Resident Evil 4's culling passes elect a lane with one): refused by the front end until the
// group's width was the driver's to promise; the probe checks the mask on the GPU
TEST(a_kernel_taking_a_ballot_compiles) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("ballot_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) std::printf("       %s\n", why.c_str());
}

// [[thread_execution_width]] as a kernel input (Resident Evil 4's mainCS): the SIMD-group's width; the probe checks 64
TEST(a_kernel_taking_its_execution_width_compiles) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("width_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) std::printf("       %s\n", why.c_str());
}

// a fragment stage writing [[depth(any)]] (Resident Evil 4's CopyDepthPS): its depth has a register of its own, and the
// stage links with a vertex stage, where it was refused as a depth this did not serve
TEST(a_fragment_stage_writing_depth_compiles_and_links) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> f = bitcodeOf("depth_fs"), v = bitcodeOf("tri_pos_vs");
    CHECK(!f.empty() && !v.empty());
    if (f.empty() || v.empty()) return;
    std::string why;
    std::shared_ptr<CompiledStage> fs = compileStage(f.data(), f.size(), &why), vs = compileStage(v.data(), v.size(), &why);
    CHECK(fs != nullptr && vs != nullptr);
    if (fs == nullptr || vs == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK(fs->stage.depthRegid != a6xx::kRegidNone);
    CHECK(linkProgram(vs, fs, PixelFormat::kRGBA8Unorm, &why) != nullptr);
}

// a second source colour, [[color(0), index(1)]] (Resident Evil 4's dual-source blends): ir3 takes it as DATA1 and
// marks the stage dual-source, the second colour has a register, and the stage links
TEST(a_dual_source_fragment_stage_compiles_and_links) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> f = bitcodeOf("dual_fs"), v = bitcodeOf("tri_color_vs");
    CHECK(!f.empty() && !v.empty());
    if (f.empty() || v.empty()) return;
    std::string why;
    std::shared_ptr<CompiledStage> fs = compileStage(f.data(), f.size(), &why), vs = compileStage(v.data(), v.size(), &why);
    CHECK(fs != nullptr && vs != nullptr);
    if (fs == nullptr || vs == nullptr) {
        std::printf("       %s\n", why.c_str());
        return;
    }
    CHECK(fs->stage.dualSrcBlend != 0);
    CHECK(fs->stage.colorRegid[0] != a6xx::kRegidNone && fs->stage.colorRegid[1] != a6xx::kRegidNone);
    CHECK(linkProgram(vs, fs, PixelFormat::kRGBA8Unorm, &why) != nullptr);
}

TEST(a_kernel_taking_a_word_into_bytes_compiles) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: bytes_id did not run)\n");
        return;
    }
    std::vector<uint8_t> bc = bitcodeOf("bytes_id");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::string why;
    auto k = compileAir(bc.data(), bc.size(), &why, {});
    CHECK(k != nullptr);
    if (k == nullptr) std::printf("       %s\n", why.c_str());
}

// What MTLFunction's -newArgumentEncoderWithBufferIndex: is answered with: argbuf_res's set at [[buffer(0)]] has
// four members, ids 0 to 3, eight bytes apart from 0 (the module's air.struct_type_info). There is no set at
// [[buffer(1)]] -- the function's other buffers are the set's own members, not sets -- and that is refused by name,
// as an index Metal would answer with its assertion. Before this, every such call aborted Resident Evil 4.
// The same layout asked from eight threads at once while kernels compile on them too, as Resident Evil 4 does: every
// answer the single-threaded one. The front end and ir3's compiler object are not reentrant; the layout once ran
// outside the compilers' lock and came back with members missing and set numbers made of garbage.
TEST(argument_buffer_layouts_and_compiles_on_many_threads_agree) {
    if (!hasCompiler()) return;
    std::vector<uint8_t> bc = bitcodeOf("argbuf_res"), k = bitcodeOf("fill_id");
    CHECK(!bc.empty() && !k.empty());
    if (bc.empty() || k.empty()) return;
    const std::vector<std::pair<uint32_t, uint32_t>> want = {{0, 0}, {1, 8}, {2, 16}, {3, 24}};
    std::atomic<unsigned> wrong{0}, compiled{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < 12; j++) {
                if ((i + j) % 3 == 0) {
                    std::string why;
                    if (compileAir(k.data(), k.size(), &why, {}) != nullptr) compiled++;
                    continue;
                }
                std::vector<std::pair<uint32_t, uint32_t>> members;
                std::string why;
                if (!argumentBufferLayout(bc.data(), bc.size(), 0, &members, &why) || members != want) wrong++;
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(wrong.load(), 0u);
    CHECK_EQ(compiled.load(), 32u);
}

TEST(the_layout_of_an_argument_buffer_comes_from_the_metadata) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: argument buffer layouts did not run)\n");
        return;
    }
    std::vector<uint8_t> bc = bitcodeOf("argbuf_res");
    CHECK(!bc.empty());
    if (bc.empty()) return;
    std::vector<std::pair<uint32_t, uint32_t>> members;
    std::string why;
    CHECK(argumentBufferLayout(bc.data(), bc.size(), 0, &members, &why));
    const std::vector<std::pair<uint32_t, uint32_t>> want = {{0, 0}, {1, 8}, {2, 16}, {3, 24}};
    CHECK(members == want);
    CHECK(!argumentBufferLayout(bc.data(), bc.size(), 1, &members, &why));
    expectRefusal(why, "argbuf_res has no argument buffer at buffer index 1 (its sets: [0]; 4 resources read)");
    // the C API, with arrays shorter than the set: the count says so
    unsigned ids[2] = {9, 9}, offsets[2] = {9, 9};
    CHECK_EQ(hakios_metal_argument_buffer_layout(bc.data(), bc.size(), 0, ids, offsets, 2), 4);
    CHECK(ids[0] == 0 && ids[1] == 1 && offsets[0] == 0 && offsets[1] == 8);
    CHECK_EQ(hakios_metal_argument_buffer_layout(bc.data(), bc.size(), 1, ids, offsets, 2), -1);
}

// Argument buffers: argbuf_res takes its output buffer, a sampler and two textures as the members of one set at
// [[buffer(0)]]. The set holds the output's GPU address and the sampler's and textures' resource ids, eight bytes a
// member; bindArgumentBuffers puts each at the index the front end allocated -- the output at buffer 0, which is also
// the set's own Metal index, so the set has to be read from the bindings as given. Twins: the two texture words
// swapped bind the textures swapped; a zero word leaves its slot empty (not the set); a set not bound is refused.
TEST(argument_buffer_members_are_bound_from_the_words_in_their_set) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: argument buffers did not run)\n");
        return;
    }
    auto k = compareWithFixture("argbuf_res", kernels::argbuf_res_air, "argbuf_res", "isam");
    if (!k) return;
    Gpu gpu(std::make_unique<FakeGpu>());
    std::string why;
    auto set = newBuffer(gpu, 64, &why);
    auto out = newBuffer(gpu, 4096, &why);
    TextureDesc td;
    td.format = PixelFormat::kR32Float;
    td.width = td.height = 4;
    auto t0 = Texture::make(gpu, td, &why), t1 = Texture::make(gpu, td, &why);
    Sampler s;
    CHECK(makeSampler(SamplerDesc{}, &s, &why));
    CHECK(set != nullptr && out != nullptr && t0 != nullptr && t1 != nullptr);
    if (set == nullptr || out == nullptr || t0 == nullptr || t1 == nullptr) return;
    uint64_t* w = static_cast<uint64_t*>(set->contents());
    auto encode = [&](uint64_t a, uint64_t b, uint64_t c, uint64_t d) { w[0] = a; w[1] = b; w[2] = c; w[3] = d; };
    auto resolve = [&](Bindings* b, std::string* err) {
        b->buffers = {Binding{set.get(), 0}};
        std::vector<std::shared_ptr<const void>> kept;
        return bindArgumentBuffers(k->resources, b, &kept, err);
    };
    encode(out->gpuAddress() + 64, s.id, t0->resourceId(), t1->resourceId());
    Bindings b;
    CHECK(resolve(&b, &why));
    CHECK(b.buffers.size() >= 1 && b.buffers[0].buffer == out.get() && b.buffers[0].offset == 64);
    CHECK(b.textures.size() >= 2 && b.textures[0] == t0.get() && b.textures[1] == t1.get());
    CHECK(b.samplers.size() >= 1 && b.samplers[0] != nullptr && memcmp(b.samplers[0]->words, s.words, 16) == 0);
    encode(out->gpuAddress(), s.id, t1->resourceId(), t0->resourceId());
    Bindings swapped;
    CHECK(resolve(&swapped, &why));
    CHECK(swapped.textures.size() >= 2 && swapped.textures[0] == t1.get() && swapped.textures[1] == t0.get());
    encode(0, s.id, t0->resourceId(), t1->resourceId());
    Bindings none;
    CHECK(resolve(&none, &why));
    CHECK(none.buffers[0].buffer == nullptr);   // emptied: not the set bound at the same index
    Bindings unbound;
    std::vector<std::shared_ptr<const void>> kept;
    CHECK(!bindArgumentBuffers(k->resources, &unbound, &kept, &why));
    CHECK(why.find("argument buffer 0") != std::string::npos);
}

// compileStage's second compilation without ir3's preamble is taken only when it spills less, or as little with fewer
// registers: CoreAnimation's uber shader's numbers, and the ties and losses that keep the first
TEST(a_stage_compiled_without_the_preamble_is_taken_only_when_it_spills_less) {
    Kernel with{}, without{};
    with.pvtmemSize = 512, with.maxReg = 47;
    without.pvtmemSize = 0, without.maxReg = 20;
    CHECK(preferWithoutPreamble(with, without));
    without.pvtmemSize = 512, without.maxReg = 47;
    CHECK(!preferWithoutPreamble(with, without));   // the same: the first
    without.maxReg = 40;
    CHECK(preferWithoutPreamble(with, without));    // as much spilled, fewer registers
    without.pvtmemSize = 1024, without.maxReg = 10;
    CHECK(!preferWithoutPreamble(with, without));   // more spilled, whatever its registers
}

// whether a stage writes its device buffers, which decides whether its draws may be replayed tile by tile in GMEM:
// tri_dbuf_fs only reads one, tri_dwrite_fs (its twin) stores into it too
TEST(a_stage_that_writes_its_device_buffers_is_told_from_one_that_reads_them) {
    if (!hasCompiler()) {
        std::printf("       (no compiler in this build: the device-buffer writes did not run)\n");
        return;
    }
    std::string why;
    auto reads = compileStage(kernels::tri_dbuf_fs_bc, kernels::tri_dbuf_fs_bc_size, &why);
    CHECK(reads != nullptr);
    auto writes = compileStage(kernels::tri_dwrite_fs_bc, kernels::tri_dwrite_fs_bc_size, &why);
    CHECK(writes != nullptr);
    if (writes == nullptr) std::printf("       %s\n", why.c_str());
    CHECK(reads != nullptr && !reads->writesMemory);
    CHECK(writes != nullptr && writes->writesMemory);
}
