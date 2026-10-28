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

    // --- the Kernel, field for field as tools/mesa/ir3-dump.patch writes a fixture ---
    auto out = std::make_unique<CompiledKernel>();
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
        // the texture/sampler shape fields stay zero here; the core fills them from the NIR walk (Kernel::textures) in its step
        Resource res{};
        res.kind = kind; res.index = r.index; res.sizeBytes = r.size; res.read = r.read; res.write = r.write; res.irWrite = r.ir_write;
        res.typeName = r.type_name ? r.type_name : "";
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
    Kernel& k = out->kernel;
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
    k.numSamp = uint32_t(v->num_samp);
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
