#include "hakios-metal/compiler.hpp"

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

std::unique_ptr<CompiledKernel> compileAir(const void*, size_t, std::string* why) {
    *why = "no compiler in this build: it is linked in with MESA_BUILD= (tools/compiler.mk), and was not";
    return nullptr;
}

std::unique_ptr<CompiledStage> compileStage(const void*, size_t, std::string* why) {
    *why = "no compiler in this build: it is linked in with MESA_BUILD= (tools/compiler.mk), and was not";
    return nullptr;
}

std::unique_ptr<CompiledProgram> linkProgram(std::shared_ptr<CompiledStage>, std::shared_ptr<CompiledStage>, PixelFormat, std::string* why) {
    *why = "no compiler in this build: it is linked in with MESA_BUILD= (tools/compiler.mk), and was not";
    return nullptr;
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
        ir3_compiler_options options{};
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
    case GLSL_SAMPLER_DIM_MS: *name = "multisampled"; return false;
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
                    t->element = elementOf(tex->dest_type);
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
    Translation() = default;
    Translation(const Translation&) = delete;
    Translation& operator=(const Translation&) = delete;
    ~Translation() {
        if (shader != nullptr) ralloc_free(shader);
        if (mem != nullptr) ralloc_free(mem);
    }
    const char* name() const { return nir != nullptr && nir->info.name ? nir->info.name : "the function"; }
};

// AIR to NIR: the front end, with the module's function constants at their defaults
bool translate(const void* bitcode, size_t size, Translation& t, std::string* why) {
    t.mem = ralloc_context(nullptr);
    air_constant_value* constants = nullptr;
    unsigned numConstants = air_module_function_constants(t.mem, bitcode, size, &constants);
    air_to_nir_options options{};
    options.constants = constants;
    options.num_constants = numConstants;
    const char* error = nullptr;
    t.nir = air_to_nir(t.mem, bitcode, size, nullptr, &options, ir3_get_compiler_options(theCompiler()), &t.info, &error);
    if (t.nir == nullptr) return refuse(why, std::string("the front end refused it: ") + (error ? error : "no reason given"));
    return true;
}

// two things the dispatch cannot serve yet, refused here rather than at every dispatch: the slot the front end reserves
// for air.get_null_texture_* (the runtime would owe it a 1x1 zero texture, nir_air.h) and a constexpr sampler (the
// runtime would owe it a TEX_SAMP from air_sampler_state at the index the front end assigned)
bool refuseUnservedResources(const Translation& t, std::string* why) {
    for (unsigned i = 0; i < t.info.num_resources; i++) {
        const air_resource& r = t.info.resources[i];
        if (r.kind == AIR_RESOURCE_TEXTURE && r.null_texture) {
            return refuse(why, std::string(t.name()) + " asks for Metal's null texture at index " + std::to_string(r.index) + ": not served yet");
        }
        if (r.kind == AIR_RESOURCE_SAMPLER && r.inline_sampler) {
            return refuse(why, std::string(t.name()) + " declares a constexpr sampler at index " + std::to_string(r.index) + ": not served yet");
        }
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
bool compileVariant(Translation& t, PassOrder order, std::string* why) {
    ir3_compiler* compiler = theCompiler();
    nir_shader* nir = t.nir;
    // what the consumer owes before ir3: the front end emits NIR's own system values, as the SPIR-V one does
    nir_lower_system_values(nir);
    nir_lower_compute_system_values(nir, nullptr);   // a no-op outside compute, run for every stage as ir3_cmdline.c runs it
    if (order == PassOrder::kCompute) {
        nir_lower_io(nir, nir_variable_mode(nir_var_shader_in | nir_var_shader_out), ir3_glsl_type_size,
                     nir_lower_io_use_interpolated_input_intrinsics);
    }
    ir3_shader_nir_options nirOptions{};
    ir3_nir_lower_io_to_temporaries(nir);
    ir3_finalize_nir(compiler, &nirOptions, nir);

    t.shader = static_cast<ir3_shader*>(rzalloc_size(nullptr, sizeof(ir3_shader)));
    t.shader->compiler = compiler;
    t.shader->type = nir->info.stage;
    t.shader->nir = nir;
    ir3_nir_post_finalize(t.shader);
    ir3_shader_variant* v = static_cast<ir3_shader_variant*>(rzalloc_size(t.shader, sizeof(ir3_shader_variant)));
    t.v = v;
    v->type = t.shader->type;   // the key stays zero: no ucp, msaa, sample shading or rasterflat (ir3_cmdline.c:336, 504)
    v->compiler = compiler;
    v->const_state = static_cast<struct ir3_const_state*>(rzalloc_size(v, sizeof(struct ir3_const_state)));
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
    if (ir3_compile_shader_nir(compiler, t.shader, v) != 0) return refuse(why, std::string("ir3 refused ") + t.name());
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
    for (unsigned i = 0; i < v->outputs_count; i++) {
        const ir3_shader_output& o = v->outputs[i];
        uint8_t cls = o.slot < kOutputLocations && oc.known[o.slot] ? oc.cls[o.slot] : 0;
        s.outputs[i] = StageOutput{o.slot, o.regid, uint8_t(o.half), cls};
    }
    s.numOutputs = v->outputs_count;
    // --- a vertex stage ---
    s.numAttributes = 0;
    s.positionRegid = a6xx::kRegidNone;
    s.pointSizeRegid = a6xx::kRegidNone;
    s.writesLayer = s.writesViewport = 0;
    if (vs) {
        for (unsigned i = 0; i < v->inputs_count; i++) {
            if (!v->inputs[i].sysval && v->inputs[i].regid != a6xx::kRegidNone) s.numAttributes++;
        }
        s.positionRegid = ir3_find_output_regid(v, VARYING_SLOT_POS);
        s.pointSizeRegid = ir3_find_output_regid(v, VARYING_SLOT_PSIZ);
        s.writesLayer = ir3_find_output(v, VARYING_SLOT_LAYER) >= 0;
        s.writesViewport = ir3_find_output(v, VARYING_SLOT_VIEWPORT) >= 0;
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
            int k = ir3_find_output(v, gl_varying_slot(FRAG_RESULT_DATA0 + i));
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
        s.depthRegid = ir3_find_output_regid(v, FRAG_RESULT_DEPTH);
        s.sampleMaskRegid = ir3_find_output_regid(v, FRAG_RESULT_SAMPLE_MASK);
        s.stencilRefRegid = ir3_find_output_regid(v, FRAG_RESULT_STENCIL);
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

std::unique_ptr<CompiledKernel> compileAir(const void* bitcode, size_t size, std::string* why) {
    std::lock_guard<std::mutex> g(gCompilerLock);
    if (theCompiler() == nullptr) {
        refuse(why, "ir3 has no compiler for chip 0x06030500");
        return nullptr;
    }
    Translation t;
    if (!translate(bitcode, size, t, why)) return nullptr;
    if (t.nir->info.stage != MESA_SHADER_COMPUTE) {
        refuse(why, std::string(t.info.entry_point ? t.info.entry_point : "the function") +
                        " is not a compute kernel; a vertex or fragment function goes through compileStage");
        return nullptr;
    }
    // the textures, images and samplers as the instructions address them, before any lowering touches them
    auto out = std::make_unique<CompiledKernel>();
    uint32_t highestSampler = 0;
    if (!walkTextures(t.nir, out->kernel, &highestSampler, why)) return nullptr;
    if (!refuseUnservedResources(t, why)) return nullptr;
    if (!compileVariant(t, PassOrder::kCompute, why)) return nullptr;
    KernelHome home{out->kernel, out->words, out->immediates, out->resources, out->name, out->listing};
    if (!fillKernel(t, home, highestSampler, why)) return nullptr;
    out->threadgroupBytes = t.info.threadgroup_bytes;
    return out;
}

std::unique_ptr<CompiledStage> compileStage(const void* bitcode, size_t size, std::string* why) {
    std::lock_guard<std::mutex> g(gCompilerLock);
    if (theCompiler() == nullptr) {
        refuse(why, "ir3 has no compiler for chip 0x06030500");
        return nullptr;
    }
    Translation t;
    if (!translate(bitcode, size, t, why)) return nullptr;
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
    if (!refuseUnservedResources(t, why)) return nullptr;
    if (!compileVariant(t, PassOrder::kGraphics, why)) return nullptr;
    KernelHome home{out->stage.k, out->words, out->immediates, out->resources, out->name, out->listing};
    if (!fillKernel(t, home, highestSampler, why)) return nullptr;
    if (!fillStage(t, classes, out->stage, why)) return nullptr;
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
        if (k.numSsbos != 0 || k.numUbos != 0) {
            refuse(why, stageName(*s) + " binds " + std::to_string(k.numSsbos) + " device buffer(s) and " + std::to_string(k.numUbos) +
                            " constant buffer(s) it indexes: only constant buffers read at fixed offsets (pushed ranges) are served in a draw yet");
            return nullptr;
        }
        // sampled textures are served in a fragment stage (render.cpp: the FS_TEX group, the prefetch); a vertex stage's
        // textures and an image in either stage are not yet -- render::encode says the same at the draw
        bool textures = k.numTextureSlots != 0 || k.samplerMask != 0;
        for (uint32_t i = 0; i < kMaxTextureSlots; i++) textures = textures || k.textures[i].kind != kTexNone;
        if (textures && s->stage.kind == kStageVertex) {
            refuse(why, stageName(*s) + " samples or reads textures: textures in a vertex stage (SB6_VS_TEX) are not served yet");
            return nullptr;
        }
        for (uint32_t i = 0; i < kMaxTextureSlots; i++) {
            if (k.textures[i].kind == kTexImage) {
                refuse(why, stageName(*s) + " stores through texture " + std::to_string(i) + " as an image: images in a draw (the 3D IBO table) are not served");
                return nullptr;
            }
        }
    }
    if (F.numColorOutputs != 1 || F.colorRegid[0] == a6xx::kRegidNone) {
        refuse(why, stageName(*fs) + " writes " + std::to_string(F.numColorOutputs) + " colour output(s)" +
                        (F.numColorOutputs != 0 && F.colorRegid[0] == a6xx::kRegidNone ? " and not [[color(0)]]" : "") + ": one target, [[color(0)]], is served");
        return nullptr;
    }
    if (F.depthRegid != a6xx::kRegidNone || F.writesPos) {
        refuse(why, stageName(*fs) + " writes depth: no depth attachment is served");
        return nullptr;
    }
    if (F.stencilRefRegid != a6xx::kRegidNone || F.writesStencilref) {
        refuse(why, stageName(*fs) + " writes a stencil reference: no stencil attachment is served");
        return nullptr;
    }
    if (F.sampleMaskRegid != a6xx::kRegidNone || F.writesSmask) {
        refuse(why, stageName(*fs) + " writes a sample mask: no multisampling is served");
        return nullptr;
    }
    if (F.fbRead) {
        refuse(why, stageName(*fs) + " reads its render target ([[color(n)]] as an argument, framebuffer fetch): not served");
        return nullptr;
    }
    if (F.perSamp) {
        refuse(why, stageName(*fs) + " runs per sample: no multisampling is served");
        return nullptr;
    }
    if (F.dualSrcBlend) {
        refuse(why, stageName(*fs) + " writes a dual-source blend output: blending is off in the first draw");
        return nullptr;
    }
    if (F.readsPrimid) {
        refuse(why, stageName(*fs) + " reads the primitive id: not served");
        return nullptr;
    }
    // --- the colour output's class against the target's format: a float4 into a Unorm/Float format, an integer vector
    // into a Uint one; the hardware converts the FS's raw register as the format says, so a mismatch is garbage, not an
    // error, on the phone
    const FormatInfo* format = formatInfo(color0);
    if (format == nullptr) {
        refuse(why, "pixel format " + std::to_string(uint32_t(color0)) + " is not served");
        return nullptr;
    }
    uint8_t fsClass = kElemFloat;
    for (uint32_t i = 0; i < F.numOutputs; i++) {
        if (F.outputs[i].slot == FRAG_RESULT_DATA0) fsClass = F.outputs[i].pad;
    }
    const uint8_t formatClass = format->element == kElemFloat ? kElemFloat : kElemUint;
    if (fsClass != formatClass) {
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
    out->program.name = out->name.c_str();
    return out;
}

#endif

}  // namespace hakios::metal
