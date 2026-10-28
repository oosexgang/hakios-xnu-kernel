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

}  // namespace

bool hasCompiler() { return theCompiler() != nullptr; }

std::unique_ptr<CompiledKernel> compileAir(const void* bitcode, size_t size, std::string* why) {
    std::lock_guard<std::mutex> g(gCompilerLock);
    ir3_compiler* compiler = theCompiler();
    if (compiler == nullptr) {
        refuse(why, "ir3 has no compiler for chip 0x06030500");
        return nullptr;
    }
    void* mem = ralloc_context(nullptr);
    // --- AIR to NIR: the front end, with the module's function constants at their defaults ---
    air_constant_value* constants = nullptr;
    unsigned numConstants = air_module_function_constants(mem, bitcode, size, &constants);
    air_to_nir_options options{};
    options.constants = constants;
    options.num_constants = numConstants;
    air_shader_info info{};
    const char* error = nullptr;
    nir_shader* nir = air_to_nir(mem, bitcode, size, nullptr, &options, ir3_get_compiler_options(compiler), &info, &error);
    if (nir == nullptr) {
        refuse(why, std::string("the front end refused it: ") + (error ? error : "no reason given"));
        ralloc_free(mem);
        return nullptr;
    }
    if (nir->info.stage != MESA_SHADER_COMPUTE) {
        refuse(why, std::string(info.entry_point ? info.entry_point : "the function") + " is not a compute kernel; only those run here yet");
        ralloc_free(mem);
        return nullptr;
    }
    // --- the textures, images and samplers as the instructions address them, before any lowering touches them ---
    auto out = std::make_unique<CompiledKernel>();
    Kernel& k = out->kernel;
    uint32_t highestSampler = 0;
    if (!walkTextures(nir, k, &highestSampler, why)) {
        ralloc_free(mem);
        return nullptr;
    }
    for (unsigned i = 0; i < info.num_resources; i++) {
        // two things the dispatch cannot serve yet, refused here rather than at every dispatch: the slot the front end
        // reserves for air.get_null_texture_* (the runtime would owe it a 1x1 zero texture, nir_air.h) and a constexpr
        // sampler (the runtime would owe it a TEX_SAMP from air_sampler_state at the index the front end assigned)
        const air_resource& r = info.resources[i];
        if (r.kind == AIR_RESOURCE_TEXTURE && r.null_texture) {
            refuse(why, std::string(nir->info.name ? nir->info.name : "the kernel") + " asks for Metal's null texture at index " + std::to_string(r.index) + ": not served yet");
            ralloc_free(mem);
            return nullptr;
        }
        if (r.kind == AIR_RESOURCE_SAMPLER && r.inline_sampler) {
            refuse(why, std::string(nir->info.name ? nir->info.name : "the kernel") + " declares a constexpr sampler at index " + std::to_string(r.index) + ": not served yet");
            ralloc_free(mem);
            return nullptr;
        }
    }
    // --- what the consumer owes before ir3: the front end emits NIR's own system values, as the SPIR-V one does ---
    nir_lower_system_values(nir);
    nir_lower_compute_system_values(nir, nullptr);
    nir_lower_io(nir, nir_variable_mode(nir_var_shader_in | nir_var_shader_out), ir3_glsl_type_size,
                 nir_lower_io_use_interpolated_input_intrinsics);
    ir3_shader_nir_options nirOptions{};
    ir3_nir_lower_io_to_temporaries(nir);
    ir3_finalize_nir(compiler, &nirOptions, nir);

    // --- the shader and its one variant, built by hand as Mesa's own command-line compiler builds them ---
    auto* shader = static_cast<ir3_shader*>(rzalloc_size(nullptr, sizeof(ir3_shader)));
    shader->compiler = compiler;
    shader->type = nir->info.stage;
    shader->nir = nir;
    ir3_nir_post_finalize(shader);
    auto* v = static_cast<ir3_shader_variant*>(rzalloc_size(shader, sizeof(ir3_shader_variant)));
    v->type = shader->type;
    v->compiler = compiler;
    v->const_state = static_cast<struct ir3_const_state*>(rzalloc_size(v, sizeof(struct ir3_const_state)));
    // what ir3_shader.c's alloc_variant sets and a hand-built variant must too: the merged register file every gen-6
    // driver compiles for (SP_CS_CTRL_REG0.MERGEDREGS says what the code was allocated for), the IBO counts the
    // dispatch programs into SP_CS_CONFIG, and the name
    v->mergedregs = compiler->gen >= 6;
    v->num_ssbos = nir->info.num_ssbos;
    v->num_ibos = nir->info.num_ssbos + nir->info.num_images;
    v->name = ralloc_strdup(v, nir->info.name);
    shader->variants = v;
    shader->variant_count = 1;
    if (ir3_compile_shader_nir(compiler, shader, v) != 0) {
        refuse(why, std::string("ir3 refused ") + (nir->info.name ? nir->info.name : "the kernel"));
        ralloc_free(shader);
        ralloc_free(mem);
        return nullptr;
    }
    // assembled once: ir3_shader_assemble frees the constant data as it appends it to the binary
    auto* bin = static_cast<uint32_t*>(ir3_shader_assemble(v));
    if (bin == nullptr) {
        refuse(why, "ir3 compiled the kernel and could not assemble it");
        ralloc_free(shader);
        ralloc_free(mem);
        return nullptr;
    }
    const struct ir3_const_state* cs = ir3_const_state(v);
    if (cs->ubo_state.num_enabled > kMaxUboRanges) {
        refuse(why, std::to_string(cs->ubo_state.num_enabled) + " pushed constant ranges: the Kernel holds " + std::to_string(kMaxUboRanges));
        ralloc_free(shader);
        ralloc_free(mem);
        return nullptr;
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
        refuse(why, std::string(nir->info.name ? nir->info.name : "the kernel") + " counts " + std::to_string(numSamp) + " textures and images: NSAMP holds 31");
        ralloc_free(shader);
        ralloc_free(mem);
        return nullptr;
    }

    // --- the Kernel, field for field as tools/mesa/ir3-dump.patch writes a fixture ---
    out->name = nir->info.name ? nir->info.name : "kernel";
    out->threadgroupBytes = info.threadgroup_bytes;
    for (unsigned i = 0; i < info.num_resources; i++) {
        const air_resource& r = info.resources[i];
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
            const KernelTexture& t = k.textures[r.index];
            if ((kind == Resource::kTexture && t.kind == kTexSampled) || (kind == Resource::kImage && t.kind == kTexImage)) {
                res.textureDim = t.dim;
                res.textureArray = t.isArray;
                res.element = t.element;
            }
        }
        out->resources.push_back(std::move(res));
    }
    out->words.assign(bin, bin + v->info.size / 4);
    out->immediates.assign(cs->immediates, cs->immediates + cs->immediates_count);
    {
        char* text = nullptr;
        size_t length = 0;
        FILE* f = open_memstream(&text, &length);
        if (f != nullptr) {
            ir3_shader_disasm(v, bin, f);
            fclose(f);
            out->listing.assign(text, length);
            free(text);
        }
    }
    k.words = out->words.data();
    k.numWords = uint32_t(out->words.size());
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
    k.immediates = out->immediates.empty() ? nullptr : out->immediates.data();
    k.numImmediates = uint32_t(out->immediates.size());
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
    k.name = out->name.c_str();
    k.source = "";
    ralloc_free(shader);
    ralloc_free(mem);
    return out;
}

#endif

}  // namespace hakios::metal
