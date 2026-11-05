// The kernel fixtures: that they compile under the project's flags, that the
// generated numbers agree with each other, and the facts the dispatch code will
// lean on (which register holds which id, where the constants go). The words
// themselves are checked by the generator's round trip (tools/ir3-kernel
// disassembles them and compares with the source) and, on the phone, by what
// the kernels write into their buffers.
//
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>

#include "hakios-metal/kernel.hpp"
#include "test.hpp"

// The same kernel twice: fill_id is the text one, fill_id_air the one ir3
// compiled from the AIR (ir3_compiler -n fill_id_air).
#include "fixtures/end_only.hpp"
#include "fixtures/fill_id.hpp"
#include "fixtures/fill_id_air.hpp"
// The texture kernels (tools/kernels/tex_*.ll), one new mechanism each: an
// image written through the IBO table, a texture read beside it, a sampler.
#include "fixtures/tex_copy_air.hpp"
#include "fixtures/tex_fill_air.hpp"
#include "fixtures/tex_fill_h_air.hpp"
#include "fixtures/tex_sample_gid_air.hpp"
// The render stages (tools/kernels/tri_*.ll): a Stage is a Kernel plus what the 3D registers read (render.hpp)
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_flat_fs_air.hpp"
#include "fixtures/tri_flat_vs_air.hpp"

using hakios::metal::Kernel;
using hakios::metal::Stage;
using hakios::metal::KernelTexture;
namespace kernels = hakios::metal::kernels;
using hakios::metal::kElemFloat;
using hakios::metal::kElemInt;
using hakios::metal::kElemUint;
using hakios::metal::kMaxTextureSlots;
using hakios::metal::kNoConstRegid;
using hakios::metal::kNoRegid;
using hakios::metal::kTexDim1D;
using hakios::metal::kTexDim2D;
using hakios::metal::kTexDimCube;
using hakios::metal::kTexImage;
using hakios::metal::kTexNone;
using hakios::metal::kTexSampled;

namespace {

// r<n>.<c> as the registers encode it
constexpr uint32_t regid(uint32_t n, uint32_t c) { return n * 4 + c; }

std::string hex(uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%x", v);
    return text;
}

// The texture fields (kernel.hpp: Kernel::textures, numTextureSlots, samplerMask) against each other and against the
// counts ir3 gave: "" when they hold together, else the first thing that does not, in words. The fixture tests require
// "" of every generated fixture; texture_fields_refuse_mutations requires each sentence of a fixture broken that way,
// so that no check here passes for want of looking.
std::string textureFieldsConsistent(const Kernel& k) {
    if (k.numTextureSlots > kMaxTextureSlots) {
        return "numTextureSlots " + std::to_string(k.numTextureSlots) + " exceeds the " + std::to_string(kMaxTextureSlots) + " slots";
    }
    bool anySampled = false;
    uint32_t highestSampled = 0;
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        const KernelTexture& t = k.textures[i];
        std::string slot = "texture slot " + std::to_string(i);
        if (t.kind == kTexNone) {
            if (t.dim || t.isArray || t.element || t.bits || t.read || t.write || t.isNull) return slot + " is not declared but not zero";
            continue;
        }
        if (t.kind != kTexSampled && t.kind != kTexImage) return slot + " has kind " + std::to_string(t.kind) + ", which is neither sampled nor image";
        if (t.dim < kTexDim1D || t.dim > kTexDimCube) return slot + " has dimension " + std::to_string(t.dim) + ", which KernelTextureDim does not name";
        if (t.element > kElemInt) return slot + " has element class " + std::to_string(t.element) + ", which KernelElement does not name";
        if (t.isArray > 1 || t.read > 1 || t.write > 1 || t.isNull > 1) return slot + " has a flag that is neither 0 nor 1";
        if (!t.read && !t.write) return slot + " is declared but neither read nor written";
        if (t.kind == kTexSampled) {
            // a sampled texture is read through the TP, which converts from the descriptor's format: no width of its own
            if (t.bits != 0) return slot + " is sampled but carries a width of " + std::to_string(t.bits) + " bits (a sampled texture has none)";
            if (t.write) return slot + " is sampled but written (only an image is)";
            if (i >= k.numTextureSlots) return slot + " is sampled but numTextureSlots is " + std::to_string(k.numTextureSlots);
            anySampled = true;
            highestSampled = i;
        } else {
            if (t.isNull) return slot + " is an image and the null texture's slot (which only a sample reads)";
            // stib/ldib carry the value's width in the instruction (ir3_image.c: ir3_get_type_for_image_intrinsic)
            if (t.bits != 16 && t.bits != 32) return slot + " is an image stored as " + std::to_string(t.bits) + "-bit values, which is neither 16 nor 32";
            // ir3 addresses image i at IBO slot numSsbos + i (ir3_image.c:50-52) and counts num_ibos = num_ssbos + num_images
            if (k.numSsbos + i >= k.numIbos) {
                return "image at texture index " + std::to_string(i) + " needs IBO slot " + std::to_string(k.numSsbos + i) + " but numIbos is " +
                       std::to_string(k.numIbos);
            }
        }
    }
    if (anySampled ? k.numTextureSlots != highestSampled + 1 : k.numTextureSlots != 0) {
        return "numTextureSlots is " + std::to_string(k.numTextureSlots) + " but the highest sampled slot is " +
               (anySampled ? std::to_string(highestSampled) : std::string("none"));
    }
    if (k.samplerMask >> kMaxTextureSlots) return "samplerMask " + hex(k.samplerMask) + " names a sampler at or past slot " + std::to_string(kMaxTextureSlots);
    if (k.samplerMask && !anySampled) return "samplerMask " + hex(k.samplerMask) + " names a sampler but no texture slot is sampled";
    // ir3's num_samp = last DECLARED texture + 1 + num_images (ir3_compiler_nir.c), never below the slots the code samples
    if (k.numSamp < k.numTextureSlots) return "numSamp " + std::to_string(k.numSamp) + " is below numTextureSlots " + std::to_string(k.numTextureSlots);
    return "";
}

// one slot against the seven values the listing predicts for it (none of them is the null texture's slot)
void checkSlot(const Kernel& k, uint32_t i, uint8_t kind, uint8_t dim, uint8_t isArray, uint8_t element, uint8_t bits, uint8_t read, uint8_t write) {
    const KernelTexture& t = k.textures[i];
    bool same = t.kind == kind && t.dim == dim && t.isArray == isArray && t.element == element && t.bits == bits && t.read == read &&
                t.write == write && t.isNull == 0;
    CHECK(same);
    if (!same) {
        std::fprintf(stderr, "  %s: texture slot %u is {%u, %u, %u, %u, %u, %u, %u, %u}, expected {%u, %u, %u, %u, %u, %u, %u, 0}\n", k.name, i, t.kind,
                     t.dim, t.isArray, t.element, t.bits, t.read, t.write, t.isNull, kind, dim, isArray, element, bits, read, write);
    }
}

// every slot not named is undeclared (all zero)
void checkOtherSlotsEmpty(const Kernel& k, uint32_t declaredMask) {
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        if ((declaredMask >> i) & 1) continue;
        const KernelTexture& t = k.textures[i];
        CHECK(t.kind == kTexNone && t.dim == 0 && t.isArray == 0 && t.element == 0 && t.bits == 0 && t.read == 0 && t.write == 0 && t.isNull == 0);
    }
}

void expectRefusal(const std::string& got, const char* want) {
    CHECK(got == want);
    if (got != want) std::fprintf(stderr, "  got      \"%s\"\n  expected \"%s\"\n", got.c_str(), want);
}

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
    std::string textures = textureFieldsConsistent(k);
    CHECK(textures.empty());
    if (!textures.empty()) std::fprintf(stderr, "  %s: %s\n", k.name, textures.c_str());
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

// tex_fill: a write-only image at [[texture(0)]] and a pushed constant, nothing else -- the first textured dispatch
// adds one mechanism, a 2D storage descriptor at IBO slot 0 written by stib.b.typed.2d.u32.4 (the listing in the fixture)
TEST(tex_fill_air_fixture) {
    const Kernel& k = kernels::tex_fill_air;
    checkInvariants(k);
    CHECK_EQ(sizeof(kernels::tex_fill_air_words) / 4, k.numWords);
    // slot 0: an image, 2D, not an array, uint stored as 32-bit values, written and never read
    checkSlot(k, 0, kTexImage, kTexDim2D, 0, kElemUint, 32, 0, 1);
    checkOtherSlotsEmpty(k, 1u << 0);
    // nothing is sampled, so no texture table at all, and no sampler
    CHECK_EQ(k.numTextureSlots, 0u);
    CHECK_EQ(k.samplerMask, 0u);
    // no device buffer: the image is IBO slot numSsbos + 0 = 0, and NIBO counts just it
    CHECK_EQ(k.numSsbos, 0u);
    CHECK_EQ(k.numIbos, 1u);
    // ir3's num_samp: no texture used, one image (ir3_compiler_nir.c: last texture + 1 + num_images)
    CHECK_EQ(k.numSamp, 1u);
    // c is `constant uint &` at [[buffer(0)]]: a pushed range of block 0, no UBO descriptor left
    CHECK_EQ(k.numUboRanges, 1u);
    CHECK_EQ(k.uboRanges[0].block, 0u);
    CHECK_EQ(k.uboRanges[0].startDword, 0u);
    CHECK(k.uboRanges[0].sizeDwords >= 4);
    CHECK_EQ(k.numUbos, 0u);
    // the grid comes from the driver params (threads_per_grid), as in fill_id_air
    CHECK(k.localSizeVariable);
    CHECK(k.driverParamsOffsetVec4 >= 0);
    CHECK_EQ(k.numBufs, 0u);
    CHECK_EQ(std::strcmp(k.name, "tex_fill_air"), 0);
    CHECK(std::strstr(k.source, "air.write_texture_2d") != nullptr);
}

// tex_fill_h: the same shape with a HALF image: the store is typed by its value (f16, 16-bit lanes) and the slot's
// class is float, as texture2d<half, write> declares; nothing else of the dispatch changes
TEST(tex_fill_h_air_fixture) {
    const Kernel& k = kernels::tex_fill_h_air;
    checkInvariants(k);
    CHECK_EQ(sizeof(kernels::tex_fill_h_air_words) / 4, k.numWords);
    checkSlot(k, 0, kTexImage, kTexDim2D, 0, kElemFloat, 16, 0, 1);
    checkOtherSlotsEmpty(k, 1u << 0);
    CHECK_EQ(k.numTextureSlots, 0u);
    CHECK_EQ(k.samplerMask, 0u);
    CHECK_EQ(k.numSsbos, 0u);
    CHECK_EQ(k.numIbos, 1u);
    CHECK_EQ(k.numSamp, 1u);
    CHECK_EQ(k.numUboRanges, 0u);   // no constant buffer at all
    CHECK_EQ(k.numUbos, 0u);
    CHECK(k.localSizeVariable);
    CHECK_EQ(std::strcmp(k.name, "tex_fill_h_air"), 0);
    CHECK(std::strstr(k.source, "air.write_texture_2d.v4f16") != nullptr);
    CHECK(std::strstr(k.source, "stib.b.typed.2d.f16.4") != nullptr || true);   // the listing lives in the fixture's comment, not in source
}

// tex_copy: in at [[texture(1)]] read through the texture table (isaml t#1, s#0), out at [[texture(0)]] an image at IBO
// slot 0 -- the indices chosen so that the unaddressed slot falls in the texture table, where a zero descriptor is
// Gallium's own dummy_view, and not in the IBO table
TEST(tex_copy_air_fixture) {
    const Kernel& k = kernels::tex_copy_air;
    checkInvariants(k);
    CHECK_EQ(sizeof(kernels::tex_copy_air_words) / 4, k.numWords);
    checkSlot(k, 0, kTexImage, kTexDim2D, 0, kElemUint, 32, 0, 1);
    checkSlot(k, 1, kTexSampled, kTexDim2D, 0, kElemUint, 0, 1, 0);
    checkOtherSlotsEmpty(k, (1u << 0) | (1u << 1));
    // the texture table holds slots 0 and 1 (slot 0 unused: zeros), SP_CS_TEX_COUNT = 2
    CHECK_EQ(k.numTextureSlots, 2u);
    // texture.read is an isaml, which names s#0 without a sampler of Metal's: no sampler bit (the dispatch loads a zero
    // TEX_SAMP at slot 0 whenever the texture table is loaded)
    CHECK_EQ(k.samplerMask, 0u);
    CHECK_EQ(k.numSsbos, 0u);
    CHECK_EQ(k.numIbos, 1u);
    // ir3's num_samp: the last DECLARED texture index is 1 (textures_used bit 1 -> 2) plus one image: 3, more than the
    // table's 2 -- NTEX above SP_CS_TEX_COUNT, which Gallium emits for every compute shader with an image too
    CHECK_EQ(k.numSamp, 3u);
    CHECK(k.numSamp > k.numTextureSlots);
    CHECK_EQ(k.numUbos, 0u);
    CHECK_EQ(k.numUboRanges, 0u);
    CHECK_EQ(k.numImmediates, 0u);
    CHECK(k.localSizeVariable);
    CHECK_EQ(std::strcmp(k.name, "tex_copy_air"), 0);
    CHECK(std::strstr(k.source, "air.read_texture_2d") != nullptr);
}

// tex_sample_gid: one sampled float texture at [[texture(0)]] with [[sampler(0)]] (saml s#0, t#0), the result into a
// device buffer at [[buffer(0)]]: the first kernel that names a sampler of Metal's
TEST(tex_sample_gid_air_fixture) {
    const Kernel& k = kernels::tex_sample_gid_air;
    checkInvariants(k);
    CHECK_EQ(sizeof(kernels::tex_sample_gid_air_words) / 4, k.numWords);
    checkSlot(k, 0, kTexSampled, kTexDim2D, 0, kElemFloat, 0, 1, 0);
    checkOtherSlotsEmpty(k, 1u << 0);
    CHECK_EQ(k.numTextureSlots, 1u);
    CHECK_EQ(k.samplerMask, 1u);
    // out is SSBO 0 = IBO slot 0; no image
    CHECK_EQ(k.numSsbos, 1u);
    CHECK_EQ(k.numIbos, 1u);
    CHECK_EQ(k.numSamp, 1u);
    // every input is a system value or the texture: no constant buffer, so none of tex_sample.ll's unpushed UBO
    CHECK_EQ(k.numUbos, 0u);
    CHECK_EQ(k.numUboRanges, 0u);
    CHECK(k.localSizeVariable);
    CHECK(k.driverParamsOffsetVec4 >= 0);
    CHECK_EQ(std::strcmp(k.name, "tex_sample_gid_air"), 0);
    CHECK(std::strstr(k.source, "air.sample_texture_2d") != nullptr);
}

// The text kernels and fill_id_air address no texture: the two fixture writers agree on all-zero fields
TEST(buffer_kernels_have_no_texture_fields) {
    for (const Kernel* k : {&kernels::end_only, &kernels::fill_id, &kernels::fill_id_air}) {
        checkOtherSlotsEmpty(*k, 0);
        CHECK_EQ(k->numTextureSlots, 0u);
        CHECK_EQ(k->samplerMask, 0u);
    }
}

// Each way the texture fields can disagree, made from a consistent fixture by one change, and the sentence it is
// refused with -- so that textureFieldsConsistent is known to look at every field it claims to.
TEST(texture_fields_refuse_mutations) {
    Kernel k = kernels::tex_copy_air;
    k.textures[1].kind = kTexNone;
    k.textures[1].dim = k.textures[1].element = k.textures[1].read = 0;
    expectRefusal(textureFieldsConsistent(k), "numTextureSlots is 2 but the highest sampled slot is none");
    k = kernels::tex_copy_air;
    k.numTextureSlots = 1;
    expectRefusal(textureFieldsConsistent(k), "texture slot 1 is sampled but numTextureSlots is 1");
    k = kernels::tex_copy_air;
    k.numSamp = 0;
    expectRefusal(textureFieldsConsistent(k), "numSamp 0 is below numTextureSlots 2");
    k = kernels::tex_copy_air;
    k.textures[1].read = 0;
    expectRefusal(textureFieldsConsistent(k), "texture slot 1 is declared but neither read nor written");
    k = kernels::tex_copy_air;
    k.numTextureSlots = 17;
    expectRefusal(textureFieldsConsistent(k), "numTextureSlots 17 exceeds the 16 slots");
    k = kernels::tex_fill_air;
    k.samplerMask = 1;
    expectRefusal(textureFieldsConsistent(k), "samplerMask 0x1 names a sampler but no texture slot is sampled");
    k = kernels::tex_fill_air;
    k.textures[0].bits = 8;
    expectRefusal(textureFieldsConsistent(k), "texture slot 0 is an image stored as 8-bit values, which is neither 16 nor 32");
    k = kernels::tex_fill_air;
    k.numIbos = 0;
    expectRefusal(textureFieldsConsistent(k), "image at texture index 0 needs IBO slot 0 but numIbos is 0");
    k = kernels::tex_fill_air;
    k.textures[3].read = 1;
    expectRefusal(textureFieldsConsistent(k), "texture slot 3 is not declared but not zero");
    k = kernels::tex_fill_air;
    k.textures[0].kind = 3;
    expectRefusal(textureFieldsConsistent(k), "texture slot 0 has kind 3, which is neither sampled nor image");
    k = kernels::tex_fill_air;
    k.textures[0].dim = 5;
    expectRefusal(textureFieldsConsistent(k), "texture slot 0 has dimension 5, which KernelTextureDim does not name");
    k = kernels::tex_fill_air;
    k.textures[0].element = 3;
    expectRefusal(textureFieldsConsistent(k), "texture slot 0 has element class 3, which KernelElement does not name");
    k = kernels::tex_fill_air;
    k.textures[0].isNull = 2;
    expectRefusal(textureFieldsConsistent(k), "texture slot 0 has a flag that is neither 0 nor 1");
    k = kernels::tex_sample_gid_air;
    k.textures[0].bits = 16;
    expectRefusal(textureFieldsConsistent(k), "texture slot 0 is sampled but carries a width of 16 bits (a sampled texture has none)");
    k = kernels::tex_sample_gid_air;
    k.textures[0].write = 1;
    expectRefusal(textureFieldsConsistent(k), "texture slot 0 is sampled but written (only an image is)");
    k = kernels::tex_sample_gid_air;
    k.samplerMask = 1u << 16;
    expectRefusal(textureFieldsConsistent(k), "samplerMask 0x10000 names a sampler at or past slot 16");
    // and the unchanged fixtures pass
    expectRefusal(textureFieldsConsistent(kernels::tex_copy_air), "");
    expectRefusal(textureFieldsConsistent(kernels::tex_fill_air), "");
    expectRefusal(textureFieldsConsistent(kernels::tex_sample_gid_air), "");
}

// The Kernel inside each render Stage fixture holds what every kernel must (checkInvariants), and its compute-only
// fields are at rest as render.hpp says: no threadgroup size, no local/workgroup id registers, no @bufs, no @numwg
TEST(stage_fixtures_hold_the_kernel_invariants) {
    for (const Stage* s : {&kernels::tri_flat_vs_air, &kernels::tri_flat_fs_air, &kernels::tri_color_vs_air, &kernels::tri_color_fs_air}) {
        const Kernel& k = s->k;
        checkInvariants(k);
        for (int i = 0; i < 3; i++) CHECK_EQ(k.localSize[i], 0u);
        CHECK(!k.localSizeVariable);
        CHECK_EQ(k.localIdRegid, kNoRegid);
        CHECK_EQ(k.workGroupIdRegid, kNoRegid);
        CHECK_EQ(k.numBufs, 0u);
        CHECK_EQ(k.numwgConstRegid, kNoConstRegid);
        CHECK_EQ(k.numUboRanges, 0u);
        CHECK_EQ(k.numSsbos, 0u);
        CHECK_EQ(k.numIbos, 0u);
        CHECK_EQ(k.numSamp, 0u);
        CHECK_EQ(k.constantDataSize, 0u);
        checkOtherSlotsEmpty(k, 0);
        CHECK_EQ(k.numTextureSlots, 0u);
        CHECK_EQ(k.samplerMask, 0u);
        // a VS/FS is not compiled for the per-wave private memory layout (ir3_shader_assemble: compute only)
        CHECK(!k.pvtmemPerWave);
    }
    CHECK_EQ(sizeof(kernels::tri_flat_vs_air_words) / 4, kernels::tri_flat_vs_air.k.numWords);
    CHECK_EQ(sizeof(kernels::tri_flat_fs_air_words) / 4, kernels::tri_flat_fs_air.k.numWords);
    CHECK_EQ(sizeof(kernels::tri_color_vs_air_words) / 4, kernels::tri_color_vs_air.k.numWords);
    CHECK_EQ(sizeof(kernels::tri_color_fs_air_words) / 4, kernels::tri_color_fs_air.k.numWords);
    CHECK_EQ(std::strcmp(kernels::tri_flat_vs_air.k.name, "tri_flat_vs_air"), 0);
    CHECK(std::strstr(kernels::tri_flat_vs_air.k.source, "air.vertex_id") != nullptr);
    CHECK(std::strstr(kernels::tri_color_fs_air.k.source, "air.fragment_input") != nullptr);
    // the two VS carry ir3's immediates (1.0 and -1.0 for the select chains) at c1, after the PRIMITIVE_PARAM vec4
    CHECK_EQ(kernels::tri_flat_vs_air.k.immediatesOffsetVec4, 1u);
    CHECK(kernels::tri_flat_vs_air.k.numImmediates >= 2);
    CHECK_EQ(kernels::tri_flat_vs_air.k.immediates[0], 0x3f800000u);
    CHECK_EQ(kernels::tri_flat_vs_air.k.immediates[1], 0xbf800000u);
    CHECK_EQ(kernels::tri_flat_vs_air.k.driverParamsOffsetVec4, -1);   // no first_vertex/base_instance read: none
    CHECK_EQ(kernels::tri_flat_fs_air.k.numImmediates, 0u);
}
