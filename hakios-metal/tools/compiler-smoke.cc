// compiler-smoke: does Mesa's compiler link into a program of ours, and does
// it compile an AIR kernel in-process -- the path src/compiler.cpp takes.
//
// SPDX-License-Identifier: MIT
//
//   compiler-smoke [--chip-id 0x06030500] kernel.bc
//
// It does, in order, what the driver will do: look the chip up in Mesa's
// device table, create an ir3 compiler for it, translate the bitcode with the
// AIR front end (air_to_nir, with the options the patched ir3_cmdline.c uses in
// load_air), print the NIR, then lower it as ir3_cmdline.c does and compile and
// assemble it with ir3, printing the listing. Every library of the link is
// exercised: libair, libnir, libcompiler, libfreedreno_ir3 with its encoder and
// decoder, libfreedreno_common, libmesa_util.
//
// The same source builds for the host (tools/compiler.mk: compiler-smoke) and
// for iOS (ios-compiler-smoke), so it uses nothing but the C library and
// Mesa; it does not run on the phone from here, the link is what is checked.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nir_air.h"

#include "common/freedreno_dev_info.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "util/ralloc.h"

namespace {

[[noreturn]] void die(const char* what, const char* detail = nullptr)
{
    fprintf(stderr, "compiler-smoke: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

std::string readFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        die("cannot open", path);
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        s.append(buf, n);
    fclose(f);
    return s;
}

const char* resourceKind(enum air_resource_kind k)
{
    switch (k) {
    case AIR_RESOURCE_CONSTANT_BUFFER: return "constant buffer (UBO)";
    case AIR_RESOURCE_DEVICE_BUFFER: return "device buffer (SSBO)";
    case AIR_RESOURCE_THREADGROUP_BUFFER: return "threadgroup buffer";
    case AIR_RESOURCE_TEXTURE: return "texture";
    case AIR_RESOURCE_IMAGE: return "image";
    case AIR_RESOURCE_SAMPLER: return "sampler";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv)
{
    uint64_t chipId = 0x06030500;
    const char* path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip-id") && i + 1 < argc)
            chipId = strtoull(argv[++i], nullptr, 0);
        else if (!path)
            path = argv[i];
        else
            die("usage: compiler-smoke [--chip-id ID] kernel.bc");
    }
    if (!path)
        die("usage: compiler-smoke [--chip-id ID] kernel.bc");

    // 1. The device row (libfreedreno_common).
    struct fd_dev_id devId = {};
    devId.chip_id = chipId;
    const struct fd_dev_info* devInfo = fd_dev_info_raw(&devId);
    if (!devInfo)
        die("chip id not in Mesa's device table");
    printf("device: %s, chip id 0x%llx, gen %u, threadsize_base %u, %u SP cores\n", fd_dev_name(&devId),
           static_cast<unsigned long long>(chipId), devInfo->chip, devInfo->threadsize_base,
           devInfo->num_sp_cores);

    // 2. The compiler (libfreedreno_ir3).
    struct ir3_compiler_options copts = {};
    struct ir3_compiler* compiler = ir3_compiler_create(nullptr, &devId, devInfo, &copts);
    if (!compiler)
        die("ir3_compiler_create failed");
    printf("ir3 compiler: gen %u, max_const_compute %u, branchstack %u\n", compiler->gen,
           compiler->max_const_compute, compiler->branchstack_size);

    // 3. AIR -> NIR (libair), as load_air in the patched ir3_cmdline.c: the
    // module's function constants taken as zero, and the library wrapper on,
    // which is inert for a kernel entry point (it only wraps air.visible functions).
    std::string bc = readFile(path);
    struct air_constant_value* fc = nullptr;
    unsigned numFc = air_module_function_constants(nullptr, bc.data(), bc.size(), &fc);
    struct air_to_nir_options aopts = {};
    aopts.constants = fc;
    aopts.num_constants = numFc;
    aopts.library_wrapper = true;
    struct air_shader_info info = {};
    const char* err = nullptr;
    nir_shader* nir = air_to_nir(nullptr, bc.data(), bc.size(), nullptr, &aopts, ir3_get_compiler_options(compiler),
                                 &info, &err);
    if (!nir)
        die("air_to_nir refused it", err);

    printf("AIR: entry %s, %u function constants, %u resources, %u varyings, %u render targets\n",
           info.entry_point, numFc, info.num_resources, info.num_varyings, info.num_render_targets);
    for (unsigned i = 0; i < info.num_resources; i++) {
        const struct air_resource* r = &info.resources[i];
        printf("  resource %u: %s at index %u, size %u, %s%s%s\n", i, resourceKind(r->kind), r->index, r->size,
               r->read ? "read" : "", r->read && r->write ? "+" : "", r->write ? "write" : "");
    }
    printf("NIR: %s \"%s\", workgroup %u x %u x %u%s, %u ubos, %u ssbos, %u images, %u textures\n",
           _mesa_shader_stage_to_string(nir->info.stage), nir->info.name ? nir->info.name : "",
           nir->info.workgroup_size[0], nir->info.workgroup_size[1], nir->info.workgroup_size[2],
           nir->info.workgroup_size_variable ? " (variable)" : "", nir->info.num_ubos, nir->info.num_ssbos,
           nir->info.num_images, nir->info.num_textures);
    nir_print_shader(nir, stdout);

    // 4. NIR -> ir3, the lowering ir3_cmdline.c applies after load_air, then the
    // variant as ir3_shader.c's alloc_variant would set it up.
    nir_lower_system_values(nir);
    nir_lower_compute_system_values(nir, nullptr);
    nir_lower_io(nir, static_cast<nir_variable_mode>(nir_var_shader_in | nir_var_shader_out), ir3_glsl_type_size,
                 nir_lower_io_use_interpolated_input_intrinsics);
    const struct ir3_shader_nir_options nopts = {};
    ir3_nir_lower_io_to_temporaries(nir);
    ir3_finalize_nir(compiler, &nopts, nir);

    struct ir3_shader* shader = static_cast<struct ir3_shader*>(rzalloc_size(nullptr, sizeof(*shader)));
    shader->compiler = compiler;
    shader->type = nir->info.stage;
    shader->nir = nir;
    ir3_nir_post_finalize(shader);

    struct ir3_shader_variant* v = static_cast<struct ir3_shader_variant*>(rzalloc_size(shader, sizeof(*v)));
    v->type = shader->type;
    v->compiler = compiler;
    v->const_state = static_cast<struct ir3_const_state*>(rzalloc_size(v, sizeof(*v->const_state)));
    v->mergedregs = compiler->gen >= 6;
    v->num_ssbos = nir->info.num_ssbos;
    v->num_ibos = nir->info.num_ssbos + nir->info.num_images;
    v->name = ralloc_strdup(v, nir->info.name);
    shader->variants = v;
    shader->variant_count = 1;

    if (ir3_compile_shader_nir(compiler, shader, v))
        die("ir3_compile_shader_nir failed");
    uint32_t* bin = static_cast<uint32_t*>(ir3_shader_assemble(v));
    if (!bin)
        die("ir3_shader_assemble failed");

    const struct ir3_const_state* cs = ir3_const_state(v);
    printf("ir3: %u bytes (%u instruction dwords), instrlen %u, constlen %u vec4, maxReg %d, "
           "local id r%u.%c, workgroup id r%u.%c, %u immediates at vec4 %u, driver params %s, %u UBO ranges, "
           "%u ssbos, %u ibos\n",
           v->info.size, v->info.sizedwords, v->instrlen, v->constlen, v->info.max_reg,
           ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID) >> 2,
           "xyzw"[ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID) & 3],
           ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID) >> 2,
           "xyzw"[ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID) & 3], cs->immediates_count,
           cs->allocs.max_const_offset_vec4,
           ir3_const_can_upload(&cs->allocs, IR3_CONST_ALLOC_DRIVER_PARAMS, v->constlen) ? "yes" : "no",
           cs->ubo_state.num_enabled, v->num_ssbos, v->num_ibos);
    ir3_shader_disasm(v, bin, stdout);

    ralloc_free(shader);
    ir3_compiler_destroy(compiler);
    puts("compiler-smoke: ok");
    return 0;
}
