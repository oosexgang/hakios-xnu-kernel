#include "hakios-metal/compiler.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef HAKIOS_METAL_MESA
// Mesa's headers carry their own C++ guards (and templates behind them: no extern "C" around these) and are not clean
// under -Wextra, so the Makefile hands them in as system headers (tools/compiler.mk)
#include "nir.h"
#include "nir_air.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "common/freedreno_dev_info.h"
#include "util/ralloc.h"
#include "hakios-metal/a6xx.hpp"
#endif

namespace hakios::metal {

#ifndef HAKIOS_METAL_MESA

bool hasCompiler() { return false; }

std::unique_ptr<CompiledKernel> compileAir(const void*, size_t, std::string* why, const FunctionConstants&) {
    *why = "no compiler in this build: it is linked in with MESA_BUILD= (tools/compiler.mk), and was not";
    return nullptr;
}

std::unique_ptr<CompiledStage> compileStage(const void*, size_t, std::string* why, const FunctionConstants&, uint32_t) {
    *why = "no compiler in this build: it is linked in with MESA_BUILD= (tools/compiler.mk), and was not";
    return nullptr;
}

std::unique_ptr<CompiledProgram> linkProgram(std::shared_ptr<CompiledStage>, std::shared_ptr<CompiledStage>, PixelFormat, std::string* why) {
    *why = "no compiler in this build: it is linked in with MESA_BUILD= (tools/compiler.mk), and was not";
    return nullptr;
}

bool argumentBufferLayout(const void*, size_t, uint32_t, std::vector<std::pair<uint32_t, uint32_t>>*, std::string* why) {
    *why = "no compiler in this build: it is linked in with MESA_BUILD= (tools/compiler.mk), and was not";
    return false;
}

#else

namespace {

std::mutex gCompilerLock;   // ir3 keeps state in the compiler object; one compilation at a time

// one compiler for the chip, as the kernel names it: Mesa's table resolves 0x06030500 to the Adreno 7c+ Gen 3 row
ir3_compiler* theCompiler() {
    static ir3_compiler* compiler = [] {
        fd_dev_id id{};
        id.gpu_id = 0;
        id.chip_id = a6xx::kChipId;
        // A framebuffer fetch (nir_texop_txf_ms_fb) through the FS's own texture table, at the slot after its last
        // texture, as turnip has it (tu_device.cc: -1): the zero default would make it a bindless read of descriptor
        // set 0, which this driver never binds
        ir3_compiler_options options{};
        options.bindless_fb_read_descriptor = -1;
        // the GLSL type cache, which ir3 uses for the driver UBOs' types (get_driver_ubo_type: a module's constant table
        // is read through one); held for the life of the process, as the compiler is
        glsl_type_singleton_init_or_ref();
        return ir3_compiler_create(nullptr, &id, fd_dev_info_raw(&id), &options);
    }();
    return compiler;
}

bool refuse(std::string* why, const std::string& text) {
    *why = text;
    return false;
}

// KernelTextureDim from the NIR's sampler dimension; false, with the dimension's name, for the ones no Texture can be
// made of yet (a buffer texture, a rectangle, a multisampled or subpass image): the dispatch would otherwise bind a 2D
// texture to a txf_ms and read numbers neither right nor obviously wrong
bool textureDim(glsl_sampler_dim dim, uint8_t* out, const char** name) {
    switch (dim) {
    case GLSL_SAMPLER_DIM_1D: *out = kTexDim1D; return true;
    case GLSL_SAMPLER_DIM_2D: *out = kTexDim2D; return true;
    case GLSL_SAMPLER_DIM_3D: *out = kTexDim3D; return true;
    case GLSL_SAMPLER_DIM_CUBE: *out = kTexDimCube; return true;
    case GLSL_SAMPLER_DIM_RECT: *name = "a rectangle"; return false;
    case GLSL_SAMPLER_DIM_BUF: *name = "a texture buffer"; return false;
    case GLSL_SAMPLER_DIM_MS: *out = kTexDimMS; return true;   // texture2d_ms.read(coord, sample): a txf_ms, the sample in its own source
    default: *name = "of a dimension no texture here has"; return false;
    }
}

// The element class Apple DECLARED for a texture argument, read off air.arg_type_name ("texture2d<half, write>",
// "texture2d<uint, read>", "texture2d_array<ushort, sample>" ...): the scalar type between the first '<' and the next ','
// or '>'. False when the name has no such type (a sampler, a buffer, an unknown spelling).
bool declaredElement(const std::string& typeName, uint8_t* element) {
    size_t lt = typeName.find('<');
    if (lt == std::string::npos) return false;
    size_t b = lt + 1;
    while (b < typeName.size() && typeName[b] == ' ') b++;
    size_t e = b;
    while (e < typeName.size() && typeName[e] != ',' && typeName[e] != '>' && typeName[e] != ' ') e++;
    std::string t = typeName.substr(b, e - b);
    if (t == "half" || t == "float" || t == "bfloat") *element = kElemFloat;
    else if (t == "uint" || t == "ushort" || t == "uchar") *element = kElemUint;
    else if (t == "int" || t == "short" || t == "char") *element = kElemInt;
    else return false;
    return true;
}

uint8_t elementOf(nir_alu_type t) {
    switch (nir_alu_type_get_base_type(t)) {
    case nir_type_uint: return kElemUint;
    case nir_type_int: return kElemInt;
    default: return kElemFloat;
    }
}

// What the kernel's instructions do with each Metal texture and sampler index, read off the NIR the front end produced,
// before ir3 lowers and consumes it (ir3 clones it: ir3_context.c:68). This is the source of Kernel::textures,
// numTextureSlots and samplerMask (kernel.hpp says why ir3's own num_samp cannot be): a nir_tex_instr addresses
// texture_index through the texture table (Metal's [[texture(n)]], air_intrinsics.c:2536) and, for the ops that
// sample, sampler_index through the sampler table ([[sampler(m)]], :2537; a txf names 0 and reads no sampler:
// nir_tex_instr_need_sampler); an image intrinsic addresses src[0], a constant the front end wrote from the binding
// (air_intrinsics.c:2306-2347), through IBO slot numSsbos + index (ir3_image.c:50-52). The front end emits no deref,
// no handle and no texture_offset, so a non-constant index is refused by name rather than guessed at.
bool walkTextures(nir_shader* nir, Kernel& k, uint32_t* highestSampler, std::string* why) {
    const std::string name = nir->info.name ? nir->info.name : "the kernel";
    *highestSampler = 0;
    bool anySampler = false;
    auto slot = [&](uint32_t index, const char* what, KernelTexture** out) -> bool {
        if (index >= kMaxTextureSlots) {
            return refuse(why, name + " addresses " + what + " " + std::to_string(index) + ": the tables hold " + std::to_string(kMaxTextureSlots) +
                                   " (0 to " + std::to_string(kMaxTextureSlots - 1) + ")");
        }
        *out = &k.textures[index];
        return true;
    };
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type == nir_instr_type_tex) {
                    nir_tex_instr* tex = nir_instr_as_tex(instr);
                    if (nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) >= 0 || nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset) >= 0 ||
                        nir_tex_instr_src_index(tex, nir_tex_src_texture_handle) >= 0 || nir_tex_instr_src_index(tex, nir_tex_src_sampler_handle) >= 0 ||
                        nir_tex_instr_src_index(tex, nir_tex_src_texture_deref) >= 0 || nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref) >= 0) {
                        return refuse(why, name + " samples a texture or sampler chosen at run time: only Metal's own [[texture(n)]] and [[sampler(m)]] indices are served");
                    }
                    KernelTexture* t = nullptr;
                    if (!slot(tex->texture_index, "texture", &t)) return false;
                    const char* dimName = nullptr;
                    uint8_t dim = 0;
                    if (!textureDim(tex->sampler_dim, &dim, &dimName)) {
                        return refuse(why, name + ": texture " + std::to_string(tex->texture_index) + " is " + dimName + ": not served yet");
                    }
                    if (t->kind == kTexImage) {
                        // the front end makes an index a TEXTURE or an IMAGE resource, never both (air_to_nir.c:2205-2223)
                        return refuse(why, name + " both samples and stores through texture " + std::to_string(tex->texture_index));
                    }
                    t->kind = kTexSampled;
                    t->dim = dim;
                    t->isArray = tex->is_array ? 1 : 0;
                    // a query's result is an integer whatever the texture holds (txs, query_levels...): the class comes
                    // from what reads its texels, as the fixture writer takes it
                    if (!nir_tex_instr_is_query(tex)) t->element = elementOf(tex->dest_type);
                    t->bits = 0;
                    t->read = 1;
                    if (nir_tex_instr_need_sampler(tex)) {
                        if (tex->sampler_index >= kMaxTextureSlots) {
                            return refuse(why, name + " addresses sampler " + std::to_string(tex->sampler_index) + ": the tables hold " +
                                                   std::to_string(kMaxTextureSlots) + " (0 to " + std::to_string(kMaxTextureSlots - 1) + ")");
                        }
                        k.samplerMask |= 1u << tex->sampler_index;
                        if (!anySampler || tex->sampler_index > *highestSampler) *highestSampler = tex->sampler_index;
                        anySampler = true;
                    }
                } else if (instr->type == nir_instr_type_intrinsic) {
                    nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
                    bool load = in->intrinsic == nir_intrinsic_image_load, store = in->intrinsic == nir_intrinsic_image_store;
                    bool size = in->intrinsic == nir_intrinsic_image_size || in->intrinsic == nir_intrinsic_image_levels || in->intrinsic == nir_intrinsic_image_samples;
                    if (in->intrinsic == nir_intrinsic_image_atomic || in->intrinsic == nir_intrinsic_image_atomic_swap) {
                        return refuse(why, name + " uses an image atomic: not served yet");
                    }
                    if (!load && !store && !size) continue;
                    if (!nir_src_is_const(in->src[0])) {
                        return refuse(why, name + " stores or loads through an image chosen at run time: only Metal's own [[texture(n)]] indices are served");
                    }
                    uint32_t index = uint32_t(nir_src_as_uint(in->src[0]));
                    KernelTexture* t = nullptr;
                    if (!slot(index, "image", &t)) return false;
                    const char* dimName = nullptr;
                    uint8_t dim = 0;
                    if (!textureDim(nir_intrinsic_image_dim(in), &dim, &dimName)) {
                        return refuse(why, name + ": image " + std::to_string(index) + " is " + dimName + ": not served yet");
                    }
                    if (t->kind == kTexSampled) {
                        return refuse(why, name + " both samples and stores through texture " + std::to_string(index));
                    }
                    t->kind = kTexImage;
                    t->dim = dim;
                    t->isArray = nir_intrinsic_image_array(in) ? 1 : 0;
                    // the width is the value's own (the front end's src_type/dest_type carry the class without a size:
                    // "src_type=uint" in the listings); a half image stores a 16-bit value
                    if (store) {
                        t->element = elementOf(nir_intrinsic_src_type(in));
                        t->bits = uint8_t(nir_src_bit_size(in->src[3]));
                        t->write = 1;
                    } else if (load) {
                        t->element = elementOf(nir_intrinsic_dest_type(in));
                        t->bits = uint8_t(in->def.bit_size);
                        t->read = 1;
                    }
                }
            }
        }
    }
    k.numTextureSlots = 0;
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        if (k.textures[i].kind == kTexSampled) k.numTextureSlots = i + 1;
    }
    return true;
}

// --- the road from bitcode to an assembled variant, shared by compileAir and compileStage --------------------------------
// The front end's output and everything ir3 built on it, freed together when the compile is over (the Kernel and the
// Stage copy what they keep: the words, the immediates, the tables).
struct Translation {
    void* mem = nullptr;                  // ralloc context: the function constants, the NIR, the front end's info
    nir_shader* nir = nullptr;
    air_shader_info info{};
    ir3_shader* shader = nullptr;         // the hand-built shader; its variant hangs off it
    ir3_shader_variant* v = nullptr;
    uint32_t* bin = nullptr;              // the assembled binary (ralloc'd under the variant)
    uint8_t fbTargets[kMaxTextureSlots] = {};   // lowerFbRead's slots: n + 1 where colour attachment n is read
    Translation() = default;
    Translation(const Translation&) = delete;
    Translation& operator=(const Translation&) = delete;
    ~Translation() {
        if (shader != nullptr) ralloc_free(shader);
        if (mem != nullptr) ralloc_free(mem);
    }
    const char* name() const { return nir != nullptr && nir->info.name ? nir->info.name : "the function"; }
};

// AIR to NIR: the front end, with the module's function constants as the pipeline supplied them (the rest not
// supplied: the front end tells the module so, and reads them as zero)
bool translate(const void* bitcode, size_t size, Translation& t, std::string* why, const FunctionConstants& supplied) {
    t.mem = ralloc_context(nullptr);
    air_constant_value* constants = nullptr;
    unsigned numConstants = air_module_function_constants(t.mem, bitcode, size, &constants);
    for (const FunctionConstant& f : supplied) {
        for (unsigned i = 0; i < numConstants; i++) {
            if (constants[i].index == f.index) {
                constants[i].bits = f.bits;
                constants[i].supplied = true;
            }
        }
    }
    air_to_nir_options options{};
    options.constants = constants;
    options.num_constants = numConstants;
    const char* error = nullptr;
    t.nir = air_to_nir(t.mem, bitcode, size, nullptr, &options, ir3_get_compiler_options(theCompiler()), &t.info, &error);
    if (t.nir == nullptr) return refuse(why, std::string("the front end refused it: ") + (error ? error : "no reason given"));
    return true;
}

// IEEE half bits to float, for the lod clamps air_sampler_state keeps as halves
float floatOfHalf(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ffu;
    uint32_t bits;
    if (exp == 0) {
        float v = float(mant) / 16777216.0f;   // subnormal: mant * 2^-24
        return (h & 0x8000u) ? -v : v;
    }
    bits = exp == 31 ? sign | 0x7f800000u | (mant << 13) : sign | ((exp + 112) << 23) | (mant << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Each constexpr sampler the function declares, as Metal's descriptor (air_sampler_state's fields onto MTLSampler.h's
// numbering: AIR's clamp_to_zero 0 / clamp_to_edge 1 / repeat 2 / mirrored_repeat 3 / clamp_to_border 4 are Metal's
// 4 / 0 / 2 / 3 / 5; the filters, the mip filter and the border colour share Metal's numbers; coord::pixel is the
// descriptor's normalizedCoordinates false) and made into its TEX_SAMP words as the API's samplers are (makeSampler).
// Refused by name: what makeSampler does not serve.
bool inlineSamplersOf(const Translation& t, std::vector<std::pair<uint32_t, Sampler>>* out, std::string* why) {
    static const uint32_t kAddress[5] = {4, 0, 2, 3, 5};
    for (unsigned i = 0; i < t.info.num_resources; i++) {
        const air_resource& r = t.info.resources[i];
        if (r.kind != AIR_RESOURCE_SAMPLER || !r.inline_sampler) continue;
        const air_sampler_state& st = r.sampler;
        const std::string what = std::string(t.name()) + "'s constexpr sampler at index " + std::to_string(r.index);
        SamplerDesc d;
        for (int a = 0; a < 3; a++) {
            if (unsigned(st.address[a]) > 4) return refuse(why, what + " has address mode " + std::to_string(unsigned(st.address[a])) + ", which AIR does not have");
        }
        d.sAddress = kAddress[st.address[0]];
        d.tAddress = kAddress[st.address[1]];
        d.rAddress = kAddress[st.address[2]];
        // (bicubic: the TP's cubic filter, makeSampler's 2)
        d.minFilter = st.min_filter == AIR_SAMPLER_BICUBIC ? 2 : st.min_filter == AIR_SAMPLER_LINEAR ? 1 : 0;
        d.magFilter = st.mag_filter == AIR_SAMPLER_BICUBIC ? 2 : st.mag_filter == AIR_SAMPLER_LINEAR ? 1 : 0;
        d.mipFilter = unsigned(st.mip_filter);
        d.normalizedCoordinates = !st.pixel_coord;
        // AIR keeps MSL's compare_func numbering (none 0, less .. always 1 to 7, never 8 -- CoreAnimation's constexpr
        // samplers all carry 8): none and never are a sampler that does not compare, which is what an ordinary sample()
        // reads it as; a real comparison sampler is not served
        if (st.compare_func != 0 && st.compare_func != 8) {
            return refuse(why, what + " compares (compare_func " + std::to_string(st.compare_func) + " in MSL's numbering): comparison samplers are not served yet");
        }
        d.compareFunction = 0;
        d.maxAnisotropy = st.max_anisotropy == 0 ? 1 : st.max_anisotropy;
        d.lodMinClamp = floatOfHalf(st.lod_min);
        d.lodMaxClamp = floatOfHalf(st.lod_max);
        d.borderColor = st.border_color;
        Sampler s;
        std::string swhy;
        if (!makeSampler(d, &s, &swhy)) {
            return refuse(why, what + ": " + swhy);
        }
        out->push_back({r.index, s});
    }
    return true;
}

// The slot the front end reserves for air.get_null_texture_*, which the runtime owes a 1x1 texture of its own
// (nir_air.h). A draw binds one (CommandQueue::nullTexture); a dispatch does not yet, so a kernel asking for it is
// refused here rather than at every dispatch. A stage's slot is marked in the kernel's table for the draw to find.
bool nullTextures(const Translation& t, Kernel& k, bool served, std::string* why) {
    for (unsigned i = 0; i < t.info.num_resources; i++) {
        const air_resource& r = t.info.resources[i];
        if (r.kind != AIR_RESOURCE_TEXTURE || !r.null_texture) continue;
        if (!served) {
            return refuse(why, std::string(t.name()) + " asks for Metal's null texture at index " + std::to_string(r.index) + ": not served in a dispatch yet");
        }
        // (a slot no instruction samples stays kTexNone: nothing is bound there, as for any declared and unread texture)
        if (r.index < kMaxTextureSlots && k.textures[r.index].kind == kTexSampled) k.textures[r.index].isNull = 1;
    }
    return true;
}

// Which order the lowering runs in, because the two consumers Mesa has for this NIR disagree and the fixtures follow
// one each (tools/mesa/ir3-dump.patch runs the same two orders, so a fixture and an in-process compile agree):
//   kCompute   what ir3_cmdline.c's AIR path does and this file always did for a kernel: nir_lower_io BEFORE
//              ir3_nir_lower_io_to_temporaries. A kernel has no IO variables, so the order is moot for it, and the
//              fill_id/tex_* fixtures were written by it; it stays so that they do not move.
//   kGraphics  turnip's (tu_shader.cc:2527, then ir3_nir_post_finalize's nir_lower_io, ir3_nir.c:755-757):
//              ir3_nir_lower_io_to_temporaries on the IO VARIABLES, nir_lower_io inside ir3_nir_post_finalize with
//              nir_lower_io_lower_64bit_to_32 added. Run the other way round, io_to_temporaries finds no variables to
//              copy and leaves orphan `out@position-temp` temporaries whose undef stores only nir_opt_undef removes
//              later (air-vs-fs.md, the logged tri_vary_vs NIR) -- the right output by luck, and a second store_output
//              to one slot would win in ir3's setup_output (ir3_compiler_nir.c:5239-5242).
enum class PassOrder { kCompute, kGraphics };

// NIR to an assembled ir3 variant: the lowering the consumer owes, the hand-built shader and variant as Mesa's own
// command-line compiler builds them (ir3_cmdline.c:491-507) with what alloc_variant would have set (ir3_shader.c:
// 279-355), ir3_compile_shader_nir, one ir3_shader_assemble (it frees the constant data it appends to the binary, so
// there is no second call)
// A framebuffer fetch lowered here, before ir3_nir_post_finalize would do it, so that the layer and the sample it reads
// can be what they always are on this road: 0. nir_lower_fb_read builds txf_ms_fb's coordinate from load_layer_id
// (which ir3 then makes a flat varying of VARYING_SLOT_LAYER, which no vertex stage here writes: layered targets are
// refused) and its sample from load_sample_id (which makes ir3 load the sample id: MSAA is refused too). Only reads the
// lowering itself added are zeroed: one the shader made of its own is left for the draw to refuse by name.
// tools/mesa/ir3-dump.patch runs the same, so that a fixture and a compile here agree.
namespace {
struct FbReadCounts {
    unsigned layer = 0, sample = 0;
};
bool countLayerSample(nir_builder*, nir_intrinsic_instr* in, void* data) {
    auto* c = static_cast<FbReadCounts*>(data);
    c->layer += in->intrinsic == nir_intrinsic_load_layer_id;
    c->sample += in->intrinsic == nir_intrinsic_load_sample_id;
    return false;
}
bool zeroLayerSample(nir_builder* b, nir_intrinsic_instr* in, void* data) {
    const auto* own = static_cast<const FbReadCounts*>(data);
    if (!((in->intrinsic == nir_intrinsic_load_layer_id && own->layer == 0) || (in->intrinsic == nir_intrinsic_load_sample_id && own->sample == 0))) {
        return false;
    }
    b->cursor = nir_before_instr(&in->instr);
    nir_def_rewrite_uses(&in->def, nir_imm_intN_t(b, 0, in->def.bit_size));
    nir_instr_remove(&in->instr);
    return true;
}
}  // namespace

// Varyings cross the VPC at 32 bits. ir3 keeps a 16-bit vertex output in a half register, and SP_VS_OUT_REG's 8-bit
// regid has no room for ir3's HALF_REG_ID, so the VPC read the full register of that number -- another register in the
// merged file: probe 9d2's half4 colour came out 255 0 0 where tri_color's was due. turnip never meets this (its vertex
// outputs are 32-bit); CoreAnimation's fixed_vert writes its colour and texcoords as half. So after nir_lower_io a
// 16-bit store_output is widened, and a fragment stage's 16-bit input is read at 32 bits and narrowed, the VPC holding
// 32 bits on both sides. The fixture writer (tools/mesa/ir3-dump.patch: widen_half_varyings) runs the same pass.
bool widenHalfVarying(nir_builder* b, nir_intrinsic_instr* intr, void*) {
    const bool store = intr->intrinsic == nir_intrinsic_store_output && b->shader->info.stage == MESA_SHADER_VERTEX;
    const bool load = (intr->intrinsic == nir_intrinsic_load_interpolated_input || intr->intrinsic == nir_intrinsic_load_input) &&
                      b->shader->info.stage == MESA_SHADER_FRAGMENT;
    if (store) {
        if (nir_src_bit_size(intr->src[0]) != 16) return false;
        const nir_alu_type t = nir_intrinsic_src_type(intr);
        const nir_alu_type base = nir_alu_type_get_base_type(t);
        b->cursor = nir_before_instr(&intr->instr);
        nir_def* w = base == nir_type_float ? nir_f2f32(b, intr->src[0].ssa) : base == nir_type_int ? nir_i2i32(b, intr->src[0].ssa) : nir_u2u32(b, intr->src[0].ssa);
        nir_src_rewrite(&intr->src[0], w);
        nir_intrinsic_set_src_type(intr, nir_alu_type(base | 32));
        return true;
    }
    if (load) {
        if (intr->def.bit_size != 16) return false;
        const nir_alu_type base = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr));
        nir_intrinsic_instr* wide = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
        wide->def.bit_size = 32;
        nir_intrinsic_set_dest_type(wide, nir_alu_type(base | 32));
        b->cursor = nir_before_instr(&intr->instr);
        nir_builder_instr_insert(b, &wide->instr);
        nir_def* n = base == nir_type_float ? nir_f2f16(b, &wide->def) : nir_u2u16(b, &wide->def);
        nir_def_rewrite_uses(&intr->def, n);
        nir_instr_remove(&intr->instr);
        return true;
    }
    return false;
}

// A sampler prefetch's sampler is the one the function samples its texture with. ir3 writes the texture's index there
// instead (get_tex_samp_tex_src, non-bindless: info.samp_idx = tex->texture_index), which in GL, where the two are
// always one index, is the same number; Metal binds textures and samplers apart, and QuartzCore's home_affordance_base
// samples [[texture(3)]] through the constexpr sampler at 0 -- its prefetch named sampler 3, which nothing was bound
// at. The pairs are read off the NIR before ir3's variant lowering (it works on a clone); a texture sampled through two
// different samplers cannot be told apart here and is refused. The fixture writer runs the same correction.
bool fixPrefetchSamplers(nir_shader* nir, ir3_shader_variant* v, std::string* name, std::string* why) {
    uint8_t samplerOf[32];
    memset(samplerOf, 0xfe, sizeof samplerOf);   // 0xfe: not sampled; 0xff: through more than one sampler
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type != nir_instr_type_tex) continue;
                nir_tex_instr* tex = nir_instr_as_tex(instr);
                if (!nir_tex_instr_need_sampler(tex) || tex->texture_index >= 32) continue;
                uint8_t& s = samplerOf[tex->texture_index];
                s = s == 0xfe ? uint8_t(tex->sampler_index) : s == tex->sampler_index ? s : 0xff;
            }
        }
    }
    for (unsigned i = 0; i < v->num_sampler_prefetch; i++) {
        ir3_sampler_prefetch& p = v->sampler_prefetch[i];
        if (p.bindless || p.tex_id >= 32 || samplerOf[p.tex_id] == 0xfe) continue;
        if (samplerOf[p.tex_id] == 0xff) {
            return refuse(why, *name + " prefetches texture " + std::to_string(p.tex_id) + ", which it samples through more than one sampler");
        }
        p.samp_id = samplerOf[p.tex_id];
    }
    return true;
}

// A 16-bit screen-space derivative is taken at 32 bits. ir3 emits dsx and dsy at the source's own size, and on the
// phone dsx.f16 returned the y derivative: probe 9u2's fwidth of y / 64, whose x derivative is 0, came out 1/32 in half
// and 1/64 in float. CoreAnimation's rounded-rect coverage is fwidth of a half texcoord, and with the wrong one its
// edges were noise. The value is widened, its derivative taken, and narrowed again; the same pass is in the fixture
// writer (tools/mesa/ir3-dump.patch: lower_half_derivatives), so the fixtures are what compileStage makes.
// an SSBO access's buffer index plus *DATA (compileStage's iboBase): the index source of load_ssbo, store_ssbo and the
// two atomics, the forms the AIR front end emits (air_to_nir.c)
bool shiftSsbo(nir_builder* b, nir_intrinsic_instr* intr, void* data) {
    int src;
    switch (intr->intrinsic) {
    case nir_intrinsic_load_ssbo:
    case nir_intrinsic_ssbo_atomic:
    case nir_intrinsic_ssbo_atomic_swap:
    case nir_intrinsic_get_ssbo_size: src = 0; break;
    case nir_intrinsic_store_ssbo: src = 1; break;
    default: return false;
    }
    b->cursor = nir_before_instr(&intr->instr);
    nir_src_rewrite(&intr->src[src], nir_iadd_imm(b, intr->src[src].ssa, *static_cast<const uint32_t*>(data)));
    return true;
}

// load_point_coord as a load of a vec2 shader input at VARYING_SLOT_PNTC, one variable for every read of it
bool lowerPointCoord(nir_builder* b, nir_intrinsic_instr* intr, void*) {
    if (intr->intrinsic != nir_intrinsic_load_point_coord) return false;
    nir_variable* pc = nir_find_variable_with_location(b->shader, nir_var_shader_in, VARYING_SLOT_PNTC);
    if (pc == nullptr) {
        pc = nir_create_variable_with_location(b->shader, nir_var_shader_in, VARYING_SLOT_PNTC, glsl_vec_type(2));
    }
    b->cursor = nir_before_instr(&intr->instr);
    nir_def_rewrite_uses(&intr->def, nir_load_var(b, pc));
    nir_instr_remove(&intr->instr);
    return true;
}

// uclz (count leading zeros) has no ir3 translation: ir3 reports "Unhandled ALU op: uclz" and ends the process --
// Resident Evil 4's CubemapFilterCS, from AIR's air.clz. As 31 - ufind_msb(x), which ir3 has: ufind_msb of 0 is -1, so
// zero gives 32 with no select. A narrower operand is counted in 32 bits and the difference taken off; a 64-bit one
// is its high half's count, or 32 and the low half's when the high half is 0.
bool lowerUclz(nir_builder* b, nir_instr* instr, void*) {
    if (instr->type != nir_instr_type_alu) return false;
    nir_alu_instr* alu = nir_instr_as_alu(instr);
    if (alu->op != nir_op_uclz) return false;
    b->cursor = nir_before_instr(instr);
    nir_def* x = nir_ssa_for_alu_src(b, alu, 0);
    auto clz32 = [&](nir_def* v) { return nir_isub(b, nir_imm_int(b, 31), nir_ufind_msb(b, v)); };
    nir_def* r;
    if (x->bit_size == 64) {
        nir_def* hi = nir_unpack_64_2x32_split_y(b, x);
        nir_def* lo = nir_unpack_64_2x32_split_x(b, x);
        r = nir_bcsel(b, nir_ieq_imm(b, hi, 0), nir_iadd_imm(b, clz32(lo), 32), clz32(hi));
    } else if (x->bit_size < 32) {
        r = nir_iadd_imm(b, clz32(nir_u2u32(b, x)), -int64_t(32 - x->bit_size));
    } else {
        r = clz32(x);
    }
    nir_def_replace(&alu->def, r);
    return true;
}

bool lowerHalfDerivative(nir_builder* b, nir_intrinsic_instr* intr, void*) {
    switch (intr->intrinsic) {
    case nir_intrinsic_ddx: case nir_intrinsic_ddx_fine: case nir_intrinsic_ddx_coarse:
    case nir_intrinsic_ddy: case nir_intrinsic_ddy_fine: case nir_intrinsic_ddy_coarse:
        break;
    default:
        return false;
    }
    if (intr->def.bit_size != 16) return false;
    b->cursor = nir_before_instr(&intr->instr);
    nir_def* wide = nir_f2f32(b, intr->src[0].ssa);
    nir_intrinsic_instr* d = nir_intrinsic_instr_create(b->shader, intr->intrinsic);
    d->num_components = intr->num_components;
    d->src[0] = nir_src_for_ssa(wide);
    nir_def_init(&d->instr, &d->def, intr->num_components, 32);
    nir_builder_instr_insert(b, &d->instr);
    nir_def_rewrite_uses(&intr->def, nir_f2f16(b, &d->def));
    nir_instr_remove(&intr->instr);
    return true;
}

// Each fetch then becomes an ordinary texel fetch (txf, lod 0) of a texture slot of its target's own: the lowest slot
// the function does not sample, taken in the order the targets are first read. (Not one past the highest sampled slot:
// the front end's null texture sits high in the table, and CoreAnimation's fixed_frag_lph_cpf, which samples it and
// reads its target, would then need a 17th.) ir3's own non-bindless fetch gives every fetch
// INSTRUCTION a slot of its own (num_samp, then num_samp++), so two reads of one target took two slots and a read of
// [[color(1)]] could not be told from one of [[color(0)]]; here two reads of a target share a slot and each target has
// one, which the draw fills with that attachment's view (KernelTexture::fbTarget). FB_TARGETS: n + 1 at each such slot.
// False with *WHY when the slots would run past the table.
bool lowerFbRead(nir_shader* nir, uint8_t fbTargets[kMaxTextureSlots], std::string* why) {
    if (nir->info.stage != MESA_SHADER_FRAGMENT || !nir->info.fs.uses_fbfetch_output) return true;
    FbReadCounts own;
    nir_shader_intrinsics_pass(nir, countLayerSample, nir_metadata_all, &own);
    nir_lower_fb_read(nir);
    nir_shader_intrinsics_pass(nir, zeroLayerSample, nir_metadata_control_flow, &own);
    int slotOf[kMaxTextureSlots];   // a target's slot, -1 before its first read
    for (int& x : slotOf) x = -1;
    unsigned nextFree = 0;
    bool ok = true;
    nir_foreach_function_impl(impl, nir) {
        nir_builder b = nir_builder_create(impl);
        nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
                if (instr->type != nir_instr_type_tex) continue;
                nir_tex_instr* fb = nir_instr_as_tex(instr);
                if (fb->op != nir_texop_txf_ms_fb) continue;
                const int h = nir_tex_instr_src_index(fb, nir_tex_src_texture_handle), c = nir_tex_instr_src_index(fb, nir_tex_src_coord);
                const unsigned target = h >= 0 && nir_src_is_const(fb->src[h].src) ? unsigned(nir_src_as_uint(fb->src[h].src)) : 0;
                if (target >= kMaxTextureSlots || c < 0) {
                    ok = false;
                    continue;
                }
                if (slotOf[target] < 0) {
                    while (nextFree < kMaxTextureSlots && BITSET_TEST(nir->info.textures_used, nextFree)) nextFree++;
                    if (nextFree >= kMaxTextureSlots) {
                        ok = false;
                        continue;
                    }
                    slotOf[target] = int(nextFree++);
                }
                const unsigned slot = unsigned(slotOf[target]);
                b.cursor = nir_before_instr(instr);
                nir_def* coord = fb->src[c].src.ssa;
                nir_tex_instr* tx = nir_tex_instr_create(nir, 2);
                tx->op = nir_texop_txf;
                tx->sampler_dim = GLSL_SAMPLER_DIM_2D;
                tx->coord_components = 2;
                tx->dest_type = nir_type_float32;
                tx->texture_index = slot;
                tx->sampler_index = 0;
                tx->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_trim_vector(&b, coord, 2));
                tx->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(&b, 0));
                nir_def_init(&tx->instr, &tx->def, 4, 32);
                nir_builder_instr_insert(&b, &tx->instr);
                nir_def_rewrite_uses(&fb->def, &tx->def);
                nir_instr_remove(instr);
                BITSET_SET(nir->info.textures_used, slot);
                fbTargets[slot] = uint8_t(target + 1);
            }
        }
        nir_metadata_preserve(impl, nir_metadata_control_flow);
    }
    if (!ok) return refuse(why, std::string(nir->info.name ? nir->info.name : "the function") + " reads its render targets through slots past the 16 the table holds");
    return true;
}

// Whether a fragment stage needs its 2x2 quads whole -- helper lanes for the pixels its primitive does not cover --
// as nir_shader_gather_info would say (gather_tex_info, gather_intrinsic_info): a sample with an implicit level, or a
// derivative. ir3 sets need_pixlod and need_full_quad from it (SP_FS_CTRL_REG0's PIXLODENABLE, LODPIXMASK); the AIR front
// end does not run gather_info, and without the flag a quad straddling two triangles of one mesh worked its level from
// lanes that were never shaded: sokol's mipmap sample drew a line of the smallest levels along every quad's diagonal,
// probe 9lod 128 pixels of levels 2 to 6 on its quad's diagonal where all are level 0.
bool needsQuadHelpers(nir_shader* nir) {
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type == nir_instr_type_tex && nir_tex_instr_has_implicit_derivative(nir_instr_as_tex(instr))) return true;
                if (instr->type != nir_instr_type_intrinsic) continue;
                switch (nir_instr_as_intrinsic(instr)->intrinsic) {
                case nir_intrinsic_ddx: case nir_intrinsic_ddx_fine: case nir_intrinsic_ddx_coarse:
                case nir_intrinsic_ddy: case nir_intrinsic_ddy_fine: case nir_intrinsic_ddy_coarse:
                    return true;
                default:
                    break;
                }
            }
        }
    }
    return false;
}

bool compileVariant(Translation& t, PassOrder order, std::string* why) {
    ir3_compiler* compiler = theCompiler();
    nir_shader* nir = t.nir;
    // what the consumer owes before ir3: the front end emits NIR's own system values, as the SPIR-V one does
    nir_lower_system_values(nir);
    nir_lower_compute_system_values(nir, nullptr);   // a no-op outside compute, run for every stage as ir3_cmdline.c runs it
    // [[point_coord]] is a varying the hardware fills for a point sprite (VPC_VARYING_PS_REPL_MODE), not a system value
    // ir3 knows: left as load_point_coord, ir3 reports "Unhandled intrinsic" and then reaches unreachable(), which a
    // release build turns into a trap in the application -- backboardd, for CoreAnimation's fixed_frag_lph_cpf. turnip
    // has it as a VARYING_SLOT_PNTC input (nir_lower_sysvals_to_varyings, which takes the system-value variable; the
    // front end emits the intrinsic itself, hence lowerPointCoord); the link gives that input its replacement.
    if (nir->info.stage == MESA_SHADER_FRAGMENT) {
        nir_shader_intrinsics_pass(nir, lowerPointCoord, nir_metadata_control_flow, nullptr);
    }
    if (order == PassOrder::kCompute) {
        nir_lower_io(nir, nir_variable_mode(nir_var_shader_in | nir_var_shader_out), ir3_glsl_type_size,
                     nir_lower_io_use_interpolated_input_intrinsics);
    }
    ir3_shader_nir_options nirOptions{};
    nir_shader_intrinsics_pass(nir, lowerHalfDerivative, nir_metadata_control_flow, nullptr);
    nir_shader_instructions_pass(nir, lowerUclz, nir_metadata_control_flow, nullptr);
    ir3_nir_lower_io_to_temporaries(nir);
    ir3_finalize_nir(compiler, &nirOptions, nir);
    if (!lowerFbRead(nir, t.fbTargets, why)) return false;

    t.shader = static_cast<ir3_shader*>(rzalloc_size(nullptr, sizeof(ir3_shader)));
    t.shader->compiler = compiler;
    t.shader->type = nir->info.stage;
    t.shader->nir = nir;
    ir3_nir_post_finalize(t.shader);
    // (and what the widening left -- f2f16 of an f2f32 -- folded as ir3's own passes leave nothing to fold: its variant
    // lowering asserts that the loop has no more to do)
    if (nir_shader_intrinsics_pass(nir, widenHalfVarying, nir_metadata_control_flow, nullptr)) {
        ir3_optimize_loop(compiler, &t.shader->options.nir_options, nir);
    }
    ir3_shader_variant* v = static_cast<ir3_shader_variant*>(rzalloc_size(t.shader, sizeof(ir3_shader_variant)));
    t.v = v;
    v->type = t.shader->type;   // the key stays zero: no ucp, msaa, sample shading or rasterflat (ir3_cmdline.c:336, 504)
    v->compiler = compiler;
    v->const_state = static_cast<struct ir3_const_state*>(rzalloc_size(v, sizeof(struct ir3_const_state)));
    // the driver UBOs unassigned, as alloc_variant leaves them (ir3_shader.c:347-350): ir3_get_driver_ubo takes the next
    // free UBO index for one only when it reads -1 -- zeroed, the constant data's (a module constant table) asserted
    v->const_state->consts_ubo.idx = -1;
    v->const_state->driver_params_ubo.idx = -1;
    v->const_state->primitive_map_ubo.idx = -1;
    v->const_state->primitive_param_ubo.idx = -1;
    // what ir3_shader.c's alloc_variant sets and a hand-built variant must too: the merged register file every gen-6
    // driver compiles for (SP_xS_CTRL_REG0.MERGEDREGS says what the code was allocated for), the IBO counts the
    // dispatch programs into SP_xS_CONFIG, the name, and for a fragment stage the four fs fields it copies from
    // nir->info.fs (ir3_shader.c:322-326): early tests, dual-source colour, framebuffer fetch and its coherence
    v->mergedregs = compiler->gen >= 6;
    v->num_ssbos = nir->info.num_ssbos;
    v->num_ibos = nir->info.num_ssbos + nir->info.num_images;
    v->name = ralloc_strdup(v, nir->info.name);
    if (v->type == MESA_SHADER_FRAGMENT) {
        v->fs.early_fragment_tests = nir->info.fs.early_fragment_tests;
        v->fs.color_is_dual_source = nir->info.fs.color_is_dual_source;
        v->fs.uses_fbfetch_output = nir->info.fs.uses_fbfetch_output;
        v->fs.fbfetch_coherent = nir->info.fs.fbfetch_coherent;
    }
    t.shader->variants = v;
    t.shader->variant_count = 1;
    if (nir->info.stage == MESA_SHADER_FRAGMENT && needsQuadHelpers(nir)) nir->info.fs.needs_quad_helper_invocations = true;
    if (ir3_compile_shader_nir(compiler, t.shader, v) != 0) return refuse(why, std::string("ir3 refused ") + t.name());
    {
        std::string name = t.name();
        if (!fixPrefetchSamplers(nir, v, &name, why)) return false;
    }
    t.bin = static_cast<uint32_t*>(ir3_shader_assemble(v));
    if (t.bin == nullptr) return refuse(why, std::string("ir3 compiled ") + t.name() + " and could not assemble it");
    return true;
}

// what a CompiledKernel and a CompiledStage both own around their Kernel
struct KernelHome {
    Kernel& k;
    std::vector<uint32_t>& words;
    std::vector<uint32_t>& immediates;
    std::vector<Resource>& resources;
    std::string& name;
    std::string& listing;
};

// The Kernel, field for field as tools/mesa/ir3-dump.patch writes a fixture (its emit_kernel_fields), the owned
// vectors it points into, the resources as the front end declared them with the instructions' shape beside each, and
// the listing. HIGHEST_SAMPLER is walkTextures' (see numSamp below). The compute-only fields of a vertex or fragment
// stage come out at their zeros / kNoRegid / kNoConstRegid from the same lines, as render.hpp says a Stage's do.
bool fillKernel(Translation& t, const KernelHome& out, uint32_t highestSampler, std::string* why) {
    ir3_shader_variant* v = t.v;
    Kernel& k = out.k;
    const struct ir3_const_state* cs = ir3_const_state(v);
    if (cs->ubo_state.num_enabled > kMaxUboRanges) {
        return refuse(why, std::to_string(cs->ubo_state.num_enabled) + " pushed constant ranges: the Kernel holds " + std::to_string(kMaxUboRanges));
    }
    // ir3's num_samp is BITSET_LAST_BIT(textures_used) + num_images (ir3_compiler_nir.c:5394-5395): the texture count
    // with the images added, never a sampler count. NTEX = NSAMP = num_samp is what Gallium and computerator program,
    // so it is kept; a sampler index at or past it is only reasoned about (ir3_cp.c:586 keeps s# as its own field, so
    // the GPU sees the index whatever NSAMP says) and nothing has measured what NSAMP gates: raise NSAMP to cover the
    // highest sampler named, which costs nothing when, as in every kernel so far, the texture count already covers it.
    uint32_t numSamp = uint32_t(v->num_samp);
    if (k.samplerMask != 0 && highestSampler + 1 > numSamp) numSamp = highestSampler + 1;
    if (numSamp > 31) {
        // SP_CS_CONFIG.NSAMP is 5 bits (a6xx.hpp: A6XX_SP_CS_CONFIG_NSAMP__MASK) and field() masks silently
        return refuse(why, std::string(t.name()) + " counts " + std::to_string(numSamp) + " textures and images: NSAMP holds 31");
    }
    out.name = t.nir->info.name ? t.nir->info.name : "kernel";
    for (unsigned i = 0; i < t.info.num_resources; i++) {
        const air_resource& r = t.info.resources[i];
        Resource::Kind kind = r.kind == AIR_RESOURCE_CONSTANT_BUFFER   ? Resource::kConstantBuffer
                              : r.kind == AIR_RESOURCE_DEVICE_BUFFER   ? Resource::kDeviceBuffer
                              : r.kind == AIR_RESOURCE_THREADGROUP_BUFFER ? Resource::kThreadgroupBuffer
                              : r.kind == AIR_RESOURCE_TEXTURE         ? Resource::kTexture
                              : r.kind == AIR_RESOURCE_IMAGE           ? Resource::kImage
                                                                       : Resource::kSampler;
        Resource res{};
        res.kind = kind; res.index = r.index; res.sizeBytes = r.size; res.read = r.read; res.write = r.write; res.irWrite = r.ir_write;
        res.typeName = r.type_name ? r.type_name : "";
        res.nullTexture = r.null_texture;
        res.inlineSampler = r.inline_sampler;
        if (r.argbuf_set != ~0u) {
            res.argbufSet = r.argbuf_set;
            res.argbufOffset = r.argbuf_offset;
        }
        // the shape the instructions address the index with, from the walk; a declared texture no instruction touches
        // keeps its zeros (kTexDimNone), which the dispatch then does not check a binding against
        if ((kind == Resource::kTexture || kind == Resource::kImage) && r.index < kMaxTextureSlots) {
            const KernelTexture& tx = k.textures[r.index];
            if ((kind == Resource::kTexture && tx.kind == kTexSampled) || (kind == Resource::kImage && tx.kind == kTexImage)) {
                res.textureDim = tx.dim;
                res.textureArray = tx.isArray;
                res.element = tx.element;
            }
        }
        // The declaration and the instructions must name the same class. They did not for every half or float image
        // until 29 October: the front end typed a texture write by the call's return type (void, so uint), ir3 emitted
        // stib.b.typed.*.u16/u32 for half and float values, and a typed store converts its source as its type says. The
        // dispatch judges a bound format by the instructions' class, so a disagreement here would refuse the right
        // format or convert the wrong way; it is refused at compile time, naming both.
        uint8_t declared = 0;
        if ((kind == Resource::kTexture || kind == Resource::kImage) && res.textureDim != 0 && declaredElement(res.typeName, &declared) &&
            declared != res.element) {
            static const char* const names[] = {"float", "uint", "int"};
            return refuse(why, out.name + " declares texture " + std::to_string(r.index) + " as " + res.typeName + " but its instructions move " +
                                   names[res.element] + " values through it: the front end's typing of the access is off");
        }
        out.resources.push_back(std::move(res));
    }
    out.words.assign(t.bin, t.bin + v->info.size / 4);
    out.immediates.assign(cs->immediates, cs->immediates + cs->immediates_count);
    {
        char* text = nullptr;
        size_t length = 0;
        FILE* f = open_memstream(&text, &length);
        if (f != nullptr) {
            ir3_shader_disasm(v, t.bin, f);
            fclose(f);
            out.listing.assign(text, length);
            free(text);
        }
    }
    k.words = out.words.data();
    k.numWords = uint32_t(out.words.size());
    k.instrlen = v->instrlen;
    k.constlen = v->constlen;
    k.maxReg = v->info.max_reg;
    k.maxHalfReg = v->info.max_half_reg;
    k.doubleThreadsize = v->info.double_threadsize;
    k.mergedRegs = v->mergedregs;
    k.earlyPreamble = v->early_preamble;
    k.branchstackHw = ir3_shader_branchstack_hw(v);
    k.sharedSize = v->shared_size;
    k.pvtmemSize = v->pvtmem_size;
    k.pvtmemPerWave = v->pvtmem_per_wave;
    for (int i = 0; i < 3; i++) k.localSize[i] = v->local_size[i];
    k.localSizeVariable = v->local_size_variable;
    k.localIdRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
    k.workGroupIdRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID);
    k.immediates = out.immediates.empty() ? nullptr : out.immediates.data();
    k.numImmediates = uint32_t(out.immediates.size());
    k.immediatesOffsetVec4 = cs->allocs.max_const_offset_vec4;
    k.driverParamsOffsetVec4 = ir3_const_can_upload(&cs->allocs, IR3_CONST_ALLOC_DRIVER_PARAMS, v->constlen)
                                   ? int32_t(cs->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4)
                                   : -1;
    k.numUbos = cs->num_ubos;
    k.numSsbos = v->num_ssbos;
    k.numIbos = v->num_ibos;
    k.numSamp = numSamp;
    for (uint32_t i = 0; i < kMaxUboRanges; i++) {
        if (i < cs->ubo_state.num_enabled) {
            const struct ir3_ubo_range& r = cs->ubo_state.range[i];
            k.uboRanges[i] = KernelUboRange{r.ubo.block, r.offset / 16, r.start / 4, (r.end - r.start) / 4};
        } else {
            k.uboRanges[i] = KernelUboRange{0, 0, 0, 0};
        }
    }
    k.numUboRanges = cs->ubo_state.num_enabled;
    k.numBufs = 0;   // a compiled kernel binds its buffers as SSBOs and UBOs, and reads its grid from the driver params
    for (uint32_t i = 0; i < kMaxBufs; i++) k.bufs[i] = KernelBuf{0, kNoConstRegid};
    k.numwgConstRegid = kNoConstRegid;
    k.constantDataOffset = v->info.constant_data_offset;
    k.constantDataSize = v->constant_data_size;
    k.constantDataUbo = cs->consts_ubo.idx >= 0 ? uint32_t(cs->consts_ubo.idx) + 1 : 0;
    k.name = out.name.c_str();
    k.source = "";
    return true;
}

// --- the vertex and fragment stage ---------------------------------------------------------------------------------------
// The class of each output's declared type, read off the NIR's shader_out variables BEFORE lowering (nir_lower_io
// turns them into store_output intrinsics and ir3's outputs[] then knows a register and a slot, not whether the bits
// were meant as a float or as an integer -- which is what a colour target's format must agree with). The front end
// types a float or half output as a float vector and EVERY integer output as GLSL_TYPE_INT (air_to_nir.c make_io_var:
// uint4 and int4 are both <4 x i32> in LLVM, and it does not read air.arg_type_name for outputs), so the classes are
// float (kElemFloat) and integer (kElemUint, the class of the Uint formats, the only integer formats served). Indexed
// by location: gl_varying_slot for a VS, gl_frag_result for an FS. The same walk is the fixture writer's
// walk_output_classes, and the class travels in StageOutput::pad until render.hpp names the field.
constexpr uint32_t kOutputLocations = 96;
struct OutputClasses {
    uint8_t cls[kOutputLocations] = {};
    bool known[kOutputLocations] = {};
};

// whether a stage writes memory a draw replayed tile by tile would write again: a store or an atomic on a device buffer
// or on global memory, as the front end emits them (images are refused at the link)
bool writesMemoryOf(nir_shader* nir) {
    nir_foreach_function_impl(impl, nir) {
        nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
                if (instr->type != nir_instr_type_intrinsic) continue;
                switch (nir_instr_as_intrinsic(instr)->intrinsic) {
                case nir_intrinsic_store_ssbo: case nir_intrinsic_ssbo_atomic: case nir_intrinsic_ssbo_atomic_swap: case nir_intrinsic_store_global:
                case nir_intrinsic_global_atomic: case nir_intrinsic_global_atomic_swap: case nir_intrinsic_store_deref: {
                    if (nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_store_deref) {
                        nir_deref_instr* d = nir_src_as_deref(nir_instr_as_intrinsic(instr)->src[0]);
                        if (!nir_deref_mode_may_be(d, nir_var_mem_ssbo | nir_var_mem_global)) break;
                    }
                    return true;
                }
                case nir_intrinsic_deref_atomic: case nir_intrinsic_deref_atomic_swap: {
                    nir_deref_instr* d = nir_src_as_deref(nir_instr_as_intrinsic(instr)->src[0]);
                    if (nir_deref_mode_may_be(d, nir_var_mem_ssbo | nir_var_mem_global)) return true;
                    break;
                }
                default: break;
                }
            }
        }
    }
    return false;
}

bool walkOutputClasses(nir_shader* nir, OutputClasses& oc, std::string* why) {
    nir_foreach_shader_out_variable(var, nir) {
        unsigned loc = var->data.location;
        if (loc >= kOutputLocations) {
            return refuse(why, std::string(nir->info.name ? nir->info.name : "the function") + " writes an output at location " + std::to_string(loc) +
                                   ", past the " + std::to_string(kOutputLocations) + " the Stage indexes");
        }
        oc.cls[loc] = glsl_type_is_float_16_32_64(glsl_without_array(var->type)) ? kElemFloat : kElemUint;
        oc.known[loc] = true;
    }
    return true;
}

// The fields of render.hpp's Stage after its Kernel, from the variant the way turnip and fd6 read them (the field
// comments in render.hpp name the register each feeds), with the semantics of tools/mesa/ir3-dump.patch's
// emit_stage_fields line for line: compiler_test holds the two to each other field for field. The slot namespaces
// differ by stage -- VARYING_SLOT_POS and FRAG_RESULT_DEPTH are both 0, PSIZ and STENCIL both 1 -- so the vertex-only
// fields are read from a VS only and the fragment-only ones from an FS only, the others staying at a6xx::kRegidNone / 0; a
// sysval lookup (ir3_find_sysval_regid filters on the sysval bit, gl_system_value numbering) is the same for both.
// ir3 places an output at its driver_location (setup_output: outputs[base]), and the front end gives every declared
// output one before NIR has seen which are written, so an output the function never writes (Resident Evil 4's VSGUI
// leaves its TexCoord1 undef) is a hole in the table: zeroed, slot 0, no register. Slot 0 is VARYING_SLOT_POS for a
// vertex stage and FRAG_RESULT_DEPTH for a fragment one, and ir3_find_output_regid takes the first match -- the hole --
// so VSGUI was refused as writing no position. A hole is not an output; these lookups pass over it.
int writtenOutput(const ir3_shader_variant* v, unsigned slot) {
    for (unsigned i = 0; i < v->outputs_count; i++) {
        if (v->outputs[i].slot == slot && v->outputs[i].regid != a6xx::kRegidNone) return int(i);
    }
    return -1;
}

uint32_t writtenOutputRegid(const ir3_shader_variant* v, unsigned slot) {
    const int k = writtenOutput(v, slot);
    return k < 0 ? a6xx::kRegidNone : ir3_get_output_regid(&v->outputs[k]);
}

bool fillStage(Translation& t, const OutputClasses& oc, Stage& s, std::string* why) {
    const ir3_shader_variant* v = t.v;
    const bool vs = v->type == MESA_SHADER_VERTEX;
    s.kind = vs ? kStageVertex : kStageFragment;
    if (v->inputs_count > kMaxStageInputs) {
        return refuse(why, std::string(t.name()) + " has " + std::to_string(v->inputs_count) + " inputs: the Stage holds " + std::to_string(kMaxStageInputs));
    }
    if (v->outputs_count > kMaxStageOutputs) {
        return refuse(why, std::string(t.name()) + " has " + std::to_string(v->outputs_count) + " outputs: the Stage holds " + std::to_string(kMaxStageOutputs));
    }
    for (unsigned i = 0; i < v->inputs_count; i++) {
        const auto& in = v->inputs[i];
        s.inputs[i] = StageInput{in.slot, in.regid, in.compmask, in.inloc, uint8_t(in.sysval), uint8_t(in.bary), uint8_t(in.flat), uint8_t(in.half)};
    }
    s.numInputs = v->inputs_count;
    s.numOutputs = 0;
    for (unsigned i = 0; i < v->outputs_count; i++) {
        const ir3_shader_output& o = v->outputs[i];
        if (o.regid == a6xx::kRegidNone) continue;   // a hole (writtenOutput), not an output to link
        uint8_t cls = o.slot < kOutputLocations && oc.known[o.slot] ? oc.cls[o.slot] : 0;
        s.outputs[s.numOutputs++] = StageOutput{o.slot, o.regid, uint8_t(o.half), cls};
    }
    // --- a vertex stage ---
    s.numAttributes = 0;
    s.positionRegid = a6xx::kRegidNone;
    s.pointSizeRegid = a6xx::kRegidNone;
    s.writesLayer = s.writesViewport = 0;
    if (vs) {
        for (unsigned i = 0; i < v->inputs_count; i++) {
            if (!v->inputs[i].sysval && v->inputs[i].regid != a6xx::kRegidNone) s.numAttributes++;
        }
        s.positionRegid = writtenOutputRegid(v, VARYING_SLOT_POS);
        s.pointSizeRegid = writtenOutputRegid(v, VARYING_SLOT_PSIZ);
        s.writesLayer = writtenOutput(v, VARYING_SLOT_LAYER) >= 0;
        s.writesViewport = writtenOutput(v, VARYING_SLOT_VIEWPORT) >= 0;
    }
    s.vertexIdRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_VERTEX_ID);
    s.instanceIdRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_INSTANCE_ID);
    s.clipMask = v->clip_mask;
    s.cullMask = v->cull_mask;
    // --- a fragment stage ---
    s.totalIn = v->total_in;
    s.fragCoordRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_FRAG_COORD);
    s.fragcoordCompmask = v->fragcoord_compmask;
    for (uint32_t i = 0; i < kIjCount; i++) s.ijRegid[i] = ir3_find_sysval_regid(v, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + i);   // ir3's IJ order
    s.faceRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_FRONT_FACE);
    s.sampleIdRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_SAMPLE_ID);
    s.sampleMaskInRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_SAMPLE_MASK_IN);
    s.numColorOutputs = 0;
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        s.colorRegid[i] = a6xx::kRegidNone;
        s.colorHalf[i] = 0;
        if (!vs) {
            int k = writtenOutput(v, gl_varying_slot(FRAG_RESULT_DATA0 + i));
            if (k >= 0) {
                // ir3_get_output_regid's HALF_REG_ID, split into the two fields
                s.colorRegid[i] = v->outputs[k].regid;
                s.colorHalf[i] = v->outputs[k].half;
                s.numColorOutputs = i + 1;
            }
        }
    }
    s.depthRegid = s.sampleMaskRegid = s.stencilRefRegid = a6xx::kRegidNone;
    if (!vs) {
        s.depthRegid = writtenOutputRegid(v, FRAG_RESULT_DEPTH);
        s.sampleMaskRegid = writtenOutputRegid(v, FRAG_RESULT_SAMPLE_MASK);
        s.stencilRefRegid = writtenOutputRegid(v, FRAG_RESULT_STENCIL);
    }
    s.writesPos = v->writes_pos;
    s.writesSmask = v->writes_smask;
    s.writesStencilref = v->writes_stencilref;
    s.dualSrcBlend = v->dual_src_blend;
    s.hasKill = v->has_kill;
    s.perSamp = v->per_samp;
    s.readsPrimid = v->reads_primid;
    s.fbRead = v->fb_read;
    s.needPixlod = v->need_pixlod;
    s.needFullQuad = v->need_full_quad;
    s.numSamplerPrefetch = v->num_sampler_prefetch;
    for (unsigned i = 0; i < v->num_sampler_prefetch && i < kMaxSamplerPrefetch; i++) {
        const ir3_sampler_prefetch& p = v->sampler_prefetch[i];
        s.prefetch[i] = SamplerPrefetch{p.src, p.samp_id, p.tex_id, p.dst, p.wrmask, p.half_precision, uint8_t(p.tex_opc == OPC_SAM ? 1 : 0),
                                        uint8_t(p.bindless ? 1 : 0)};
    }
    s.prefetchEndOfQuad = v->prefetch_end_of_quad;
    return true;
}

std::string stageName(const CompiledStage& s) {
    return std::string(s.stage.kind == kStageVertex ? "the vertex stage " : "the fragment stage ") + s.name;
}

// the class a format holds its channels as, in the words the refusal uses
const char* className(uint8_t element) { return element == kElemFloat ? "floats" : "integers"; }

}  // namespace

bool hasCompiler() { return theCompiler() != nullptr; }

std::unique_ptr<CompiledKernel> compileAir(const void* bitcode, size_t size, std::string* why, const FunctionConstants& constants) {
    std::lock_guard<std::mutex> g(gCompilerLock);
    if (theCompiler() == nullptr) {
        refuse(why, "ir3 has no compiler for chip 0x06030500");
        return nullptr;
    }
    Translation t;
    if (!translate(bitcode, size, t, why, constants)) return nullptr;
    if (t.nir->info.stage != MESA_SHADER_COMPUTE) {
        refuse(why, std::string(t.info.entry_point ? t.info.entry_point : "the function") +
                        " is not a compute kernel; a vertex or fragment function goes through compileStage");
        return nullptr;
    }
    // the textures, images and samplers as the instructions address them, before any lowering touches them
    auto out = std::make_unique<CompiledKernel>();
    uint32_t highestSampler = 0;
    if (!walkTextures(t.nir, out->kernel, &highestSampler, why)) return nullptr;
    if (!nullTextures(t, out->kernel, false, why)) return nullptr;
    if (!inlineSamplersOf(t, &out->inlineSamplers, why)) return nullptr;
    if (!compileVariant(t, PassOrder::kCompute, why)) return nullptr;
    KernelHome home{out->kernel, out->words, out->immediates, out->resources, out->name, out->listing};
    if (!fillKernel(t, home, highestSampler, why)) return nullptr;
    out->threadgroupBytes = t.info.threadgroup_bytes;
    out->hasThreadgroupArg = t.info.has_threadgroup_arg;
    out->threadgroupArgBase = t.info.threadgroup_arg_base;
    return out;
}

namespace {
std::unique_ptr<CompiledStage> compileStageOnce(const void* bitcode, size_t size, std::string* why, const FunctionConstants& constants, uint32_t iboBase);
}  // namespace

// A stage compiled as turnip compiles it, unless that spills: ir3's preamble pass hoists what is uniform into a
// prologue run once a draw, and the registers the prologue holds count against every fiber of the stage.
// CoreAnimation's uber shader (fixed_frag_lph_cpf: uniform branches over 102 samples) came out so at the 48 full
// registers the ISA addresses and still spilled to private memory -- 512 bytes a fiber, 144 reloads; without the
// preamble the same function needs 21 registers and spills nothing. A stage that spills is compiled again without
// it, and that one is taken when it spills less.
std::unique_ptr<CompiledStage> compileStage(const void* bitcode, size_t size, std::string* why, const FunctionConstants& constants, uint32_t iboBase) {
    std::lock_guard<std::mutex> g(gCompilerLock);
    if (theCompiler() == nullptr) {
        refuse(why, "ir3 has no compiler for chip 0x06030500");
        return nullptr;
    }
    std::unique_ptr<CompiledStage> with = compileStageOnce(bitcode, size, why, constants, iboBase);
    if (with == nullptr || with->stage.k.pvtmemSize == 0 || !theCompiler()->has_preamble) return with;
    theCompiler()->has_preamble = false;
    std::string whyNot;
    std::unique_ptr<CompiledStage> without = compileStageOnce(bitcode, size, &whyNot, constants, iboBase);
    theCompiler()->has_preamble = true;
    return without != nullptr && preferWithoutPreamble(with->stage.k, without->stage.k) ? std::move(without) : std::move(with);
}



namespace {
std::unique_ptr<CompiledStage> compileStageOnce(const void* bitcode, size_t size, std::string* why, const FunctionConstants& constants, uint32_t iboBase) {
    Translation t;
    if (!translate(bitcode, size, t, why, constants)) return nullptr;
    // the stage is the module's named metadata (air.vertex / air.fragment / air.kernel: air_to_nir.c module_stage)
    const std::string name = t.info.entry_point ? t.info.entry_point : "the function";
    switch (t.nir->info.stage) {
    case MESA_SHADER_VERTEX:
    case MESA_SHADER_FRAGMENT:
        break;
    case MESA_SHADER_COMPUTE:
    case MESA_SHADER_KERNEL:
        refuse(why, name + " is a kernel: use compileAir");
        return nullptr;
    case MESA_SHADER_NONE:
        refuse(why, name + " is an air.visible function, not a vertex or fragment stage");
        return nullptr;
    default:
        refuse(why, name + " is a " + gl_shader_stage_name(t.nir->info.stage) + " shader: only vertex and fragment stages are compiled here");
        return nullptr;
    }
    auto out = std::make_unique<CompiledStage>();
    // read off the NIR before any lowering touches it: the textures the instructions address and the outputs' classes
    uint32_t highestSampler = 0;
    if (!walkTextures(t.nir, out->stage.k, &highestSampler, why)) return nullptr;
    OutputClasses classes;
    if (!walkOutputClasses(t.nir, classes, why)) return nullptr;
    out->writesMemory = writesMemoryOf(t.nir);
    if (!nullTextures(t, out->stage.k, true, why)) return nullptr;
    if (!inlineSamplersOf(t, &out->inlineSamplers, why)) return nullptr;
    // a fragment stage's device buffers past the vertex stage's in the one IBO table: every SSBO index the front end
    // emitted plus IBO_BASE, and ir3's count of them (num_ssbos, the table's extent) as far again
    if (iboBase != 0 && t.nir->info.stage == MESA_SHADER_FRAGMENT && t.nir->info.num_ssbos != 0) {
        nir_shader_intrinsics_pass(t.nir, shiftSsbo, nir_metadata_control_flow, &iboBase);
        t.nir->info.num_ssbos += iboBase;
        out->iboBase = iboBase;
    }
    if (!compileVariant(t, PassOrder::kGraphics, why)) return nullptr;
    KernelHome home{out->stage.k, out->words, out->immediates, out->resources, out->name, out->listing};
    if (!fillKernel(t, home, highestSampler, why)) return nullptr;
    if (!fillStage(t, classes, out->stage, why)) return nullptr;
    // the fetch slots lowerFbRead made: sampled 2D floats the draw fills with the targets, and a stage that reads its
    // targets (the barrier before its draws keys on fbRead, which ir3 no longer sees: it has no txf_ms_fb left)
    for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
        if (t.fbTargets[i] == 0) continue;
        KernelTexture& kt = out->stage.k.textures[i];
        kt = KernelTexture{};
        kt.kind = kTexSampled;
        kt.dim = kTexDim2D;
        kt.element = kElemFloat;
        kt.read = 1;
        kt.fbTarget = t.fbTargets[i];
        out->stage.k.numTextureSlots = std::max(out->stage.k.numTextureSlots, i + 1);
        out->stage.fbRead = 1;
    }
    // The varyings' names, VAR0 first. The front end records the VERTEX side as the mangled string after the
    // air.vertex_output key ("generated(5colorDv4_f)", air_to_nir.c:2571-2573) and the FRAGMENT side as air.arg_name
    // ("color", :2499-2511) -- two different strings for one varying, and air_shader_info has no field for the FS's
    // mangled name (nir_air.h: air_varying holds one name). A by-name link needs the same string on both sides, so the
    // FS's names are left EMPTY here rather than filled with a name that cannot match (the gap air-vs-fs.md names:
    // the front end would have to record md_str(node, key + 1) for an air.fragment_input). With one varying both
    // sides land on VAR0 by order and the link is by slot (render::link), which is what the milestone needs.
    if (out->stage.kind == kStageVertex) {
        for (unsigned i = 0; i < t.info.num_varyings; i++) {
            const air_varying& var = t.info.varyings[i];
            if (var.slot < VARYING_SLOT_VAR0 || var.slot >= VARYING_SLOT_VAR0 + 32) continue;
            size_t n = var.slot - VARYING_SLOT_VAR0;
            if (out->varyingNames.size() <= n) out->varyingNames.resize(n + 1);
            out->varyingNames[n] = var.name ? var.name : "";
        }
    }
    return out;
}
}  // namespace

std::unique_ptr<CompiledProgram> linkProgram(std::shared_ptr<CompiledStage> vs, std::shared_ptr<CompiledStage> fs, PixelFormat color0,
                                             std::string* why) {
    if (vs == nullptr || fs == nullptr) {
        refuse(why, "linkProgram needs a vertex and a fragment stage; one is missing");
        return nullptr;
    }
    const Stage& V = vs->stage;
    const Stage& F = fs->stage;
    if (V.kind != kStageVertex) {
        refuse(why, vs->name + " is not a vertex stage");
        return nullptr;
    }
    if (F.kind != kStageFragment) {
        refuse(why, fs->name + " is not a fragment stage");
        return nullptr;
    }
    // --- what the first draw does not serve, refused by name before any link (render.hpp names the list) ---
    // (a VS's [[stage_in]] attributes link: what they are in memory is the vertex descriptor's, judged by the draw)
    (void)V;
    for (const CompiledStage* s : {vs.get(), fs.get()}) {
        const Kernel& k = s->stage.k;
        // pushed constant ranges are served (render.cpp: pushedRanges, the compute rule); a device buffer (the 3D IBO table)
        // and an indexed constant buffer (a UBO descriptor) are not yet
        // device buffers link: the draw loads the 3D IBO table and refuses there what it cannot (two stages disagreeing
        // about an index, nothing bound); images in a draw are refused below with the textures' rule
        // sampled textures are served in both stages (render.cpp: the VS_TEX and FS_TEX groups, the FS's prefetch); an
        // image in either is not yet -- render::encode says the same at the draw
        for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
            if (k.textures[i].kind == kTexImage) {
                refuse(why, stageName(*s) + " stores through texture " + std::to_string(i) + " as an image: images in a draw (the 3D IBO table) are not served");
                return nullptr;
            }
        }
    }
    // [[color(0)]] and up to seven after it, without a gap: the draw's rule (render.cpp), said here at the pipeline's
    // creation; the class of each output past 0 is held to its attachment's format by the draw, which knows the format
    if (F.numColorOutputs > kMaxColorOutputs) {
        refuse(why, stageName(*fs) + " writes " + std::to_string(F.numColorOutputs) + " colour output(s): 8 at most are served (the draw's rule)");
        return nullptr;
    }
    // (a stage writing depth is served: the draw gives its register to the depth test, render.cpp)
    if (F.stencilRefRegid != a6xx::kRegidNone || F.writesStencilref) {
        refuse(why, stageName(*fs) + " writes a stencil reference: no stencil attachment is served");
        return nullptr;
    }
    if (F.sampleMaskRegid != a6xx::kRegidNone || F.writesSmask) {
        refuse(why, stageName(*fs) + " writes a sample mask: no multisampling is served");
        return nullptr;
    }
    if (F.perSamp) {
        refuse(why, stageName(*fs) + " runs per sample: no multisampling is served");
        return nullptr;
    }
    // (a dual-source stage is served: the draw takes its second colour as the blend's second source, render.cpp)
    if (F.readsPrimid) {
        refuse(why, stageName(*fs) + " reads the primitive id: not served");
        return nullptr;
    }
    // --- the colour output's class against the target's format: a float4 into a Unorm/Float format, an integer vector
    // into a Uint one; the hardware converts the FS's raw register as the format says, so a mismatch is garbage, not an
    // error, on the phone
    // Pixel format 0 (Invalid) is a pipeline with no colour attachment 0 -- a depth-only one, or one whose first
    // target is a later index: [[color(0)]], if written, goes nowhere (render.cpp's fsDrawn), so there is no class to match.
    const FormatInfo* format = formatInfo(color0);
    if (format == nullptr && uint32_t(color0) != 0) {
        refuse(why, "pixel format " + std::to_string(uint32_t(color0)) + " is not served");
        return nullptr;
    }
    uint8_t fsClass = kElemFloat;
    for (uint32_t i = 0; i < F.numOutputs; i++) {
        if (F.outputs[i].slot == FRAG_RESULT_DATA0) fsClass = F.outputs[i].pad;
    }
    const uint8_t formatClass = format == nullptr || format->element == kElemFloat ? kElemFloat : kElemUint;
    if (format != nullptr && fsClass != formatClass) {
        refuse(why, stageName(*fs) + " returns " + (fsClass == kElemFloat ? "floats" : "integers") + " into " + format->name + ", which holds " +
                        className(formatClass));
        return nullptr;
    }
    // --- the pipeline's constant budget: ir3_compiler.c:185 max_const_pipeline = 512 vec4 for gen 6 (ir3_trim_constlen
    // in tu_pipeline.cc:2207 trims when the stages' sum exceeds it; here the sum is refused instead of trimmed)
    constexpr uint32_t kMaxConstPipeline = 512;
    if (V.k.constlen + F.k.constlen > kMaxConstPipeline) {
        refuse(why, "the two stages need " + std::to_string(V.k.constlen + F.k.constlen) + " vec4 of constants together (" + std::to_string(V.k.constlen) +
                        " + " + std::to_string(F.k.constlen) + "): the pipeline has " + std::to_string(kMaxConstPipeline) + " (ir3_compiler.c max_const_pipeline)");
        return nullptr;
    }
    // --- the link, ours, then held to Mesa's own ir3_link_shaders over the same two tables ---
    auto out = std::make_unique<CompiledProgram>();
    if (!render::link(V, F, &out->program.link, why)) return nullptr;
    {
        // ir3_link_shaders (ir3_shader.h:1311-1361) reads inputs[], inputs_count, outputs[], outputs_count and total_in,
        // which the Stage copied from the real variants verbatim (fillStage; compiler_test holds the copies to the
        // fixture writer's). The variants themselves are not kept past compileStage (CompiledStage has no field for
        // them), so two variants are rebuilt from the Stages for the cross-check, as turnip calls it (tu_pipeline.cc:
        // 889: pack_vs_out true) followed by the position appended at max_loc (:931-942). A disagreement is a bug in
        // src/link.cpp's port and is refused by the field that differs.
        auto fill = [](const Stage& s, ir3_shader_variant& v) {
            v.type = s.kind == kStageVertex ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;
            v.inputs_count = s.numInputs;
            for (uint32_t i = 0; i < s.numInputs; i++) {
                v.inputs[i].slot = s.inputs[i].slot;
                v.inputs[i].regid = s.inputs[i].regid;
                v.inputs[i].compmask = s.inputs[i].compmask;
                v.inputs[i].inloc = s.inputs[i].inloc;
                v.inputs[i].sysval = s.inputs[i].sysval;
                v.inputs[i].bary = s.inputs[i].bary;
                v.inputs[i].flat = s.inputs[i].flat;
                v.inputs[i].half = s.inputs[i].half;
            }
            v.outputs_count = s.numOutputs;
            for (uint32_t i = 0; i < s.numOutputs; i++) {
                v.outputs[i].slot = s.outputs[i].slot;
                v.outputs[i].regid = s.outputs[i].regid;
                v.outputs[i].half = s.outputs[i].half;
            }
            v.total_in = s.totalIn;
        };
        static_assert(sizeof(ir3_shader_variant) < 65536, "two variants fit on the stack");
        ir3_shader_variant mv{}, mf{};
        fill(V, mv);
        fill(F, mf);
        ir3_shader_linkage ml{};
        ir3_link_shaders(&ml, &mv, &mf, true);
        // the varmask as fd6 takes it, before the position (fd6_program.cc:654-658; src/link.cpp says why)
        uint32_t varmask[4] = {ml.varmask[0], ml.varmask[1], ml.varmask[2], ml.varmask[3]};
        uint32_t positionLoc = ml.max_loc;
        ir3_link_add(&ml, VARYING_SLOT_POS, uint8_t(V.positionRegid), 0xf, uint8_t(ml.max_loc));
        // and the point size after it, which turnip adds itself (tu6_emit_vpc, tu_pipeline.cc:944-947), not ir3_link_shaders
        if (V.pointSizeRegid != a6xx::kRegidNone) {
            ir3_link_add(&ml, VARYING_SLOT_PSIZ, uint8_t(V.pointSizeRegid), 0x1, uint8_t(ml.max_loc));
        }
        const Linkage& l = out->program.link;
        std::string differs;
        if (ml.cnt != l.cnt) differs = "cnt " + std::to_string(l.cnt) + " vs Mesa's " + std::to_string(ml.cnt);
        else if (ml.max_loc != l.maxLoc) differs = "maxLoc " + std::to_string(l.maxLoc) + " vs Mesa's " + std::to_string(ml.max_loc);
        else if (positionLoc != l.positionLoc) differs = "positionLoc " + std::to_string(l.positionLoc) + " vs Mesa's " + std::to_string(positionLoc);
        for (uint32_t i = 0; differs.empty() && i < 4; i++) {
            if (varmask[i] != l.varmask[i]) differs = "varmask[" + std::to_string(i) + "]";
        }
        for (uint32_t i = 0; differs.empty() && i < l.cnt; i++) {
            if (ml.var[i].slot != l.var[i].slot || ml.var[i].regid != l.var[i].regid || ml.var[i].compmask != l.var[i].compmask ||
                ml.var[i].loc != l.var[i].loc) {
                differs = "var[" + std::to_string(i) + "]";
            }
        }
        if (!differs.empty()) {
            refuse(why, "render::link disagrees with ir3_link_shaders on " + differs + " for " + vs->name + "+" + fs->name);
            return nullptr;
        }
    }
    out->vs = vs;
    out->fs = fs;
    out->name = vs->name + "+" + fs->name;
    out->program.vs = V;
    out->program.fs = F;
    out->program.fsIboBase = fs->iboBase;
    out->program.ssboWrites = uint8_t((vs->writesMemory ? 1 : 0) | (fs->writesMemory ? 2 : 0));
    out->program.name = out->name.c_str();
    return out;
}

bool argumentBufferLayout(const void* bitcode, size_t size, uint32_t set, std::vector<std::pair<uint32_t, uint32_t>>* members, std::string* why) {
    // under the compilers' lock as well: the front end and theCompiler() are the same ones compileAir uses, and Resident
    // Evil 4 asks for encoders on one thread while it compiles on others -- unlocked, the same function's layout came
    // back with 23 members, then 12, its set numbers garbage, and the encoders built from it wrote nothing
    std::lock_guard<std::mutex> g(gCompilerLock);
    // the interface alone: a body the front end cannot translate yet does not keep a set's layout from being known
    Translation t;
    t.mem = ralloc_context(nullptr);
    air_to_nir_options options{};
    options.interface_only = true;
    const char* error = nullptr;
    t.nir = air_to_nir(t.mem, bitcode, size, nullptr, &options, ir3_get_compiler_options(theCompiler()), &t.info, &error);
    if (t.nir == nullptr) return refuse(why, std::string("the front end refused its interface: ") + (error ? error : "no reason given"));
    members->clear();
    for (unsigned i = 0; i < t.info.num_resources; i++) {
        const air_resource& r = t.info.resources[i];
        if (r.argbuf_set == set) members->emplace_back(r.argbuf_id, r.argbuf_offset);
    }
    if (members->empty()) {
        // which sets it has, to tell an index asked wrong from a set the metadata does not describe
        std::string sets;
        for (unsigned i = 0; i < t.info.num_resources; i++) {
            const unsigned at = t.info.resources[i].argbuf_set;
            if (at != ~0u && sets.find("[" + std::to_string(at) + "]") == std::string::npos) sets += "[" + std::to_string(at) + "]";
        }
        return refuse(why, std::string(t.name()) + " has no argument buffer at buffer index " + std::to_string(set) + " (its sets: " +
                               (sets.empty() ? "none" : sets) + "; " + std::to_string(t.info.num_resources) + " resources read)");
    }
    std::sort(members->begin(), members->end());
    return true;
}

#endif

bool bindArgumentBuffers(const std::vector<Resource>& resources, Bindings* b, std::vector<std::shared_ptr<const void>>* kept, std::string* why) {
    // the sets as the caller bound them: a member's allocated index may be a set's own Metal index (the front end
    // allocates above the direct arguments, and a set is not one), so a member bound there must not change where the
    // next member's set is read from
    const std::vector<Binding> sets = b->buffers;
    for (const Resource& r : resources) {
        if (r.argbufSet == ~0u) continue;
        // a member names nothing until its word does: the slot is emptied first, so that a set bound at the same index
        // is not what a member left unset reads
        switch (r.kind) {
        case Resource::kConstantBuffer:
        case Resource::kDeviceBuffer:
            if (r.index < b->buffers.size()) b->buffers[r.index] = Binding{};
            break;
        case Resource::kTexture:
        case Resource::kImage:
            if (r.index < b->textures.size()) b->textures[r.index] = nullptr;
            break;
        case Resource::kSampler:
            if (r.index < b->samplers.size()) b->samplers[r.index] = nullptr;
            break;
        default:
            break;
        }
        if (r.argbufSet >= sets.size() || sets[r.argbufSet].buffer == nullptr) {
            *why = "argument buffer " + std::to_string(r.argbufSet) + " (" + r.typeName + "'s set) is not bound";
            return false;
        }
        const Binding& set = sets[r.argbufSet];
        if (set.offset + r.argbufOffset + 8 > set.buffer->length() || set.buffer->contents() == nullptr) {
            *why = "argument buffer " + std::to_string(r.argbufSet) + ": its member at byte " + std::to_string(r.argbufOffset) + " lies past the " +
                   std::to_string(set.buffer->length() - set.offset) + " bytes bound";
            return false;
        }
        uint64_t word = 0;
        memcpy(&word, static_cast<const uint8_t*>(set.buffer->contents()) + set.offset + r.argbufOffset, 8);
        if (word == 0) continue;
        switch (r.kind) {
        case Resource::kConstantBuffer:
        case Resource::kDeviceBuffer: {
            uint64_t offset = 0;
            std::shared_ptr<Buffer> buffer = bufferAt(word, &offset);
            if (buffer == nullptr) continue;
            if (b->buffers.size() <= r.index) b->buffers.resize(r.index + 1);
            b->buffers[r.index] = Binding{buffer.get(), offset};
            kept->push_back(buffer);
            break;
        }
        case Resource::kTexture:
        case Resource::kImage: {
            std::shared_ptr<Texture> t = Texture::byResourceId(word);
            if (t == nullptr) continue;
            if (b->textures.size() <= r.index) b->textures.resize(r.index + 1, nullptr);
            b->textures[r.index] = t.get();
            kept->push_back(t);
            break;
        }
        case Resource::kSampler: {
            const Sampler* s = samplerById(word);
            if (s == nullptr) continue;
            if (b->samplers.size() <= r.index) b->samplers.resize(r.index + 1, nullptr);
            b->samplers[r.index] = s;
            break;
        }
        default:
            break;
        }
    }
    return true;
}

bool preferWithoutPreamble(const Kernel& with, const Kernel& without) {
    return without.pvtmemSize < with.pvtmemSize || (without.pvtmemSize == with.pvtmemSize && without.maxReg < with.maxReg);
}

}  // namespace hakios::metal
