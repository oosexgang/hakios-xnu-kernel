/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air2nir: the frontend from the command line, as spirv2nir is for SPIR-V.
 *
 *   air2nir FILE [FUNCTION]          translate and print the NIR
 *   air2nir --ll FILE [FUNCTION]     print what the bitcode reader read, as LLVM assembly
 *   air2nir --list FILE.metallib     the functions of a container
 *   air2nir --report FILE...         one line per module, for tools/air/corpus.py
 *   air2nir --interface FILE [FUNC]  what the shader expects of the runtime
 *                                    (air_shader_info), key=value per line
 *
 * FILE is a .metallib (then FUNCTION picks one; without it, every function)
 * or one module's bitcode, wrapped or not. --report forks for every module,
 * so that an assertion in one is a line in the report and not the end of it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <inttypes.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"
#include "air_module.h"
#include "util/ralloc.h"

/* --library: translate an air.visible library function inside a generated
 * entry point (nir_air.h). Off by default, because a wrapped function is not
 * an entry point of the corpus and must not be counted as one.
 */
static bool library_wrapper = false;

static const nir_shader_compiler_options default_options = {
   .lower_fdiv = false,
};

static void *
map_file(const char *path, size_t *size)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0) {
      fprintf(stderr, "air2nir: %s: %s\n", path, strerror(errno));
      return NULL;
   }
   struct stat st;
   fstat(fd, &st);
   *size = st.st_size;
   void *p = mmap(NULL, st.st_size ? st.st_size : 1, PROT_READ, MAP_PRIVATE, fd, 0);
   close(fd);
   return p == MAP_FAILED ? NULL : p;
}

struct module_ref {
   const char *name;
   int container_type;
   const void *bitcode;
   size_t size;
};

/* every module of FILE (one, unless it is a container) */
static unsigned
modules_of(void *ctx, const char *path, const char *only, struct module_ref **out)
{
   size_t size;
   const uint8_t *data = map_file(path, &size);
   if (!data)
      return 0;
   if (size >= 4 && !memcmp(data, "MTLB", 4)) {
      struct air_metallib lib;
      const char *err;
      if (!air_metallib_parse(ctx, data, size, &lib, &err)) {
         fprintf(stderr, "air2nir: %s: %s\n", path, err);
         return 0;
      }
      *out = rzalloc_array(ctx, struct module_ref, lib.num_functions ? lib.num_functions : 1);
      unsigned n = 0;
      for (unsigned i = 0; i < lib.num_functions; i++) {
         if (only && strcmp(only, lib.functions[i].name))
            continue;
         (*out)[n++] = (struct module_ref) {
            lib.functions[i].name, lib.functions[i].type,
            lib.functions[i].bitcode, lib.functions[i].bitcode_size,
         };
      }
      return n;
   }
   *out = rzalloc(ctx, struct module_ref);
   (*out)[0] = (struct module_ref) { path, -1, data, size };
   return 1;
}

static const char *
stage_name(int stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX: return "vertex";
   case MESA_SHADER_FRAGMENT: return "fragment";
   case MESA_SHADER_COMPUTE: return "kernel";
   case MESA_SHADER_NONE: return "visible";
   default: return "unknown";
   }
}

/* -- evaluating a shader by folding it -------------------------------------
 *
 * For the class of shader that only reads buffers and computes -- 256 of the
 * corpus's 271 fragment shaders -- the whole thing folds to constants once the
 * buffer loads are replaced by the bytes actually in the buffers. No
 * interpreter is needed: NIR's own constant folding is the interpreter, and
 * what comes out can be compared against LLVM's answer for the same bytes
 * (tools/air/cpuref.py). This is the check docs/air.md section 4 calls the
 * strongest one, and it needs no GPU.
 */

/* The argument buffer of a wrapped library function holds floats, not the byte
 * pattern.
 *
 * Every one of these functions is compiled with no-nans-fp-math and
 * no-infs-fp-math: the module promises its inputs are finite. Arbitrary bytes
 * are mostly not -- they are infinities, NaNs and denormals -- so the inputs
 * would land outside the contract the shader was compiled under, where
 * 0 * inf may legally be either NaN or inf and a disagreement means nothing.
 * One case did disagree exactly there (folded NaN, softpipe +inf) and said
 * nothing about either side.
 *
 * Word w is ((w * 13) % 29 - 14) / 4: finite, exactly representable in
 * binary32, and different for 29 words running -- so a harness that mixed up
 * two slots, or two lanes of one slot, would get a different answer. cpuref.py
 * fills the same words.
 */
static uint8_t
library_byte(unsigned i)
{
   float f = (float)((int)((i / 4 * 13) % 29) - 14) / 4.0f;
   uint32_t u;
   memcpy(&u, &f, sizeof(u));
   return (u >> (8 * (i % 4))) & 0xff;
}

/* the same pattern cpuref.py fills its buffers with */
static uint8_t
pattern_byte(unsigned i)
{
   return library_wrapper ? library_byte(i) : (i * 7 + 13) & 0xff;
}

struct eval_state {
   struct air_shader_info *info;
   bool folded_a_load;
};

static bool
fold_buffer_loads(struct nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct eval_state *st = data;
   if (intr->intrinsic != nir_intrinsic_load_ubo && intr->intrinsic != nir_intrinsic_load_ssbo)
      return false;

   unsigned isrc = 0, osrc = 1;
   if (!nir_src_is_const(intr->src[isrc]) || !nir_src_is_const(intr->src[osrc]))
      return false;
   unsigned index = nir_src_as_uint(intr->src[isrc]);
   unsigned offset = nir_src_as_uint(intr->src[osrc]);

   /* only a buffer the interface actually declared, and only within its size */
   unsigned size = 0;
   bool found = false;
   for (unsigned i = 0; i < st->info->num_resources; i++) {
      const struct air_resource *r = &st->info->resources[i];
      bool want = intr->intrinsic == nir_intrinsic_load_ubo
                     ? r->kind == AIR_RESOURCE_CONSTANT_BUFFER
                     : r->kind == AIR_RESOURCE_DEVICE_BUFFER;
      if (want && r->index == index) {
         size = r->size ? r->size : 256;
         found = true;
         break;
      }
   }
   if (!found)
      return false;

   unsigned bytes = intr->def.num_components * intr->def.bit_size / 8;
   if (offset + bytes > size)
      return false;

   nir_const_value v[NIR_MAX_VEC_COMPONENTS] = { 0 };
   for (unsigned c = 0; c < intr->def.num_components; c++) {
      uint64_t bits = 0;
      unsigned nb = intr->def.bit_size / 8;
      for (unsigned k = 0; k < nb; k++)
         bits |= (uint64_t)pattern_byte(offset + c * nb + k) << (8 * k);
      v[c].u64 = bits;
   }

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *imm = nir_build_imm(b, intr->def.num_components, intr->def.bit_size, v);
   nir_def_replace(&intr->def, imm);
   st->folded_a_load = true;
   return true;
}

/* print one stored output value, if it folded to a constant */
static void
print_output(nir_variable *var, nir_def *value)
{
   printf("  %-24s ", var->name ? var->name : "?");
   if (!nir_src_is_const(nir_src_for_ssa(value))) {
      printf("did not fold to a constant\n");
      return;
   }
   const struct glsl_type *t = glsl_without_array(var->type);
   bool is_float = glsl_get_base_type(t) == GLSL_TYPE_FLOAT ||
                   glsl_get_base_type(t) == GLSL_TYPE_FLOAT16;
   for (unsigned c = 0; c < value->num_components; c++) {
      uint64_t bits = nir_src_comp_as_uint(nir_src_for_ssa(value), c);
      if (is_float && value->bit_size == 32) {
         float f;
         uint32_t u = bits;
         memcpy(&f, &u, 4);
         printf("%s%.9g", c ? " " : "", (double)f);
      } else if (is_float && value->bit_size == 16) {
         printf("%s0x%04" PRIx64, c ? " " : "", bits);
      } else {
         printf("%s%" PRIu64, c ? " " : "", bits);
      }
   }
   printf("\n");
}

/* A wrapped library function returns its value by storing it to a buffer, not
 * to a shader output, so --eval has to read it from there. The type name comes
 * from Apple's own air.visible_output metadata, carried on the resource: how
 * to print the bytes is not something this can infer from a store.
 */
static unsigned
print_buffer_stores(nir_shader *nir, const struct air_shader_info *info)
{
   unsigned printed = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_ssbo)
               continue;
            if (!nir_src_is_const(intr->src[1]))
               continue;
            unsigned index = nir_src_as_uint(intr->src[1]);
            const char *type_name = NULL;
            for (unsigned i = 0; i < info->num_resources; i++) {
               const struct air_resource *r = &info->resources[i];
               if (r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->index == index && r->write)
                  type_name = r->type_name;
            }
            nir_def *value = intr->src[0].ssa;
            printf("  buffer%u%*s", index, 18, "");
            if (!nir_src_is_const(nir_src_for_ssa(value))) {
               printf("did not fold to a constant\n");
               printed++;
               continue;
            }
            bool is_float = !type_name || strstr(type_name, "float") ||
                            strstr(type_name, "half");
            for (unsigned c = 0; c < value->num_components; c++) {
               uint64_t bits = nir_src_comp_as_uint(nir_src_for_ssa(value), c);
               if (is_float && value->bit_size == 32) {
                  float f;
                  uint32_t u = bits;
                  memcpy(&f, &u, 4);
                  printf("%s%.9g", c ? " " : "", (double)f);
               } else if (is_float && value->bit_size == 16) {
                  printf("%s0x%04" PRIx64, c ? " " : "", bits);
               } else {
                  printf("%s%" PRIu64, c ? " " : "", bits);
               }
            }
            printf("\n");
            printed++;
         }
      }
   }
   return printed;
}

static int
eval_module(const struct module_ref *mod)
{
   void *ctx = ralloc_context(NULL);
   const char *err = NULL;
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, mod->bitcode, mod->size, &fc);
   struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
   struct air_shader_info info;
   nir_shader *nir = air_to_nir(ctx, mod->bitcode, mod->size, NULL, &options,
                                &default_options, &info, &err);
   if (!nir) {
      fprintf(stderr, "air2nir: %s: %s\n", mod->name, err);
      ralloc_free(ctx);
      return 1;
   }

   printf("eval: %s (%s), buffers filled with (i * 7 + 13) & 0xff\n", info.entry_point,
          gl_shader_stage_name(info.stage));
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER || r->kind == AIR_RESOURCE_DEVICE_BUFFER)
         printf("  %s buffer at index %u, %u bytes\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, r->size);
   }

   struct eval_state st = { .info = &info };
   nir_shader_intrinsics_pass(nir, fold_buffer_loads, nir_metadata_control_flow, &st);

   /* Fold what the loads made foldable, and nothing else: no algebraic
    * rewriting, so the arithmetic is IEEE and comparable with LLVM's.
    *
    * Control flow is simplified, though, and that is not a rewriting of any
    * arithmetic: once a comparison has folded, the branch under it is dead and
    * the phi below it has one live source. Without this, 20 of the corpus's
    * library functions had no folded value at all -- every one of them is
    * straight-line arithmetic behind one `if` -- and a shader with a branch
    * could never be compared against LLVM, which has no such trouble.
    */
   bool progress;
   unsigned rounds = 0;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
   } while (progress && ++rounds < 16);

   printf("eval: the outputs this shader stores:\n");
   unsigned printed = 0;
   printed += print_buffer_stores(nir, &info);
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
            if (!deref || deref->deref_type != nir_deref_type_var)
               continue;
            print_output(deref->var, intr->src[1].ssa);
            printed++;
         }
      }
   }
   if (!printed)
      printf("  (none: every store was removed, or none folded)\n");
   ralloc_free(ctx);
   return 0;
}

static void
one_line(const char *s, FILE *fp)
{
   for (; *s; s++)
      fputc(*s == '\n' || *s == '\t' ? ' ' : *s, fp);
}

/* --interface: what air_shader_info says the shader expects of the runtime,
 * which nir_print_shader cannot show because it is not in the NIR. Its own
 * mode rather than a line added to the others: the report's lines are parsed
 * by tools/air/corpus.py and the translated NIR by tools/air/fbfetchcheck.py,
 * and a field added to either would move numbers that are a baseline.
 *
 * One `key=value` per line, tab-separated, so a check can read it.
 */
static int
interface_module(const struct module_ref *mod)
{
   void *ctx = ralloc_context(NULL);
   const char *err = NULL;
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, mod->bitcode, mod->size, &fc);
   struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
   struct air_shader_info info;
   nir_shader *nir = air_to_nir(ctx, mod->bitcode, mod->size, NULL, &options,
                                &default_options, &info, &err);
   if (!nir) {
      fprintf(stderr, "air2nir: %s: %s\n", mod->name, err);
      ralloc_free(ctx);
      return 1;
   }

   printf("entry=%s\tstage=%s\n", info.entry_point, gl_shader_stage_name(info.stage));
   printf("inputs_read=0x%" PRIx64 "\n", (uint64_t)nir->info.inputs_read);
   printf("vertex_attributes=%u\tvertex_attributes_read=0x%x\n",
          info.num_vertex_attributes, info.vertex_attributes_read);
   for (unsigned i = 0; i < info.num_vertex_attributes; i++) {
      const struct air_vertex_attribute *a = &info.vertex_attributes[i];
      printf("attribute\tindex=%u\tlocation=%s\tdriver_location=%d\tcomponents=%u"
             "\tbits=%u\ttype=%s\tname=%s\tdecl=%s\tused=%s\tfc=%s\n",
             a->index, gl_vert_attrib_name((gl_vert_attrib)a->location),
             a->driver_location == ~0u ? -1 : (int)a->driver_location, a->components,
             a->bit_size,
             a->base_type == AIR_ATTRIB_FLOAT ? "float" :
             a->base_type == AIR_ATTRIB_UINT ? "uint" : "int",
             a->name ? a->name : "-", a->type_name ? a->type_name : "-",
             a->used ? "yes" : "no",
             a->function_constant ? a->function_constant : "-");
   }
   for (unsigned i = 0; i < info.num_varyings; i++)
      printf("varying\tslot=%u\tcomponents=%u\tname=%s\n", info.varyings[i].slot,
             info.varyings[i].components,
             info.varyings[i].name ? info.varyings[i].name : "-");
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      /* set and id are the Metal name of an argument-buffer member, which is
       * the one case where index is this frontend's number and not Metal's --
       * so a check can see both halves of the mapping and not just the one it
       * would have guessed (tools/air/argbufcheck.py)
       */
      printf("resource\tkind=%d\tindex=%u\tsize=%u\tname=%s\tset=%d\tid=%d\n",
             r->kind, r->index, r->size, r->name ? r->name : "-",
             r->argbuf_set == ~0u ? -1 : (int)r->argbuf_set,
             r->argbuf_id == ~0u ? -1 : (int)r->argbuf_id);
   }
   for (unsigned i = 0; i < info.num_notes; i++) {
      fputs("note\t", stdout);
      one_line(info.notes[i], stdout);
      fputc('\n', stdout);
   }
   ralloc_free(ctx);
   return 0;
}

/* tools/air/corpus.py reads these lines: tab-separated key=value */
static void
report_one(const struct module_ref *mod)
{
   void *ctx = ralloc_context(NULL);
   const char *err = NULL, *calls = NULL;

   printf("module=%s\tcontainer=%s", mod->name,
          mod->container_type >= 0 ? air_metallib_type_name(mod->container_type) : "-");

   struct air_module *m = air_module_read(ctx, mod->bitcode, mod->size, &err);
   if (!m) {
      printf("\treader=fail\terror=");
      one_line(err, stdout);
      printf("\n");
      ralloc_free(ctx);
      return;
   }
   printf("\treader=ok");

   int stage = air_module_stage(ctx, mod->bitcode, mod->size, &calls, &err);
   printf("\tstage=%s\tcalls=", stage_name(stage));
   for (const char *c = calls; c && *c; c += strlen(c) + 1)
      printf("%s%s", c == calls ? "" : ",", c);

   /* Function constants are values the API supplies at pipeline creation. The
    * report has no API, so it compiles with every one set to zero and says so:
    * a module that needs values is not a module the translator cannot handle.
    */
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, mod->bitcode, mod->size, &fc);
   struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
   if (num_fc)
      printf("\tfc=%u", num_fc);

   struct air_shader_info info;
   nir_shader *nir = air_to_nir(ctx, mod->bitcode, mod->size, NULL, &options, &default_options,
                                &info, &err);
   if (!nir) {
      printf("\ttranslate=fail\terror=");
      one_line(err, stdout);
      printf("\n");
      ralloc_free(ctx);
      return;
   }
   nir_validate_shader(nir, "after air_to_nir");
   unsigned instrs = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block)
            instrs++;
      }
   }
   printf("\ttranslate=ok\tnir_instrs=%u", instrs);
   /* What was translated with a caveat: the shader was accepted and something
    * other than exactly what it asked for was done (air_shader_info.notes).
    * On the report line rather than on stderr, because the report is the only
    * output anything parses, and a caveat nobody can count is the silence this
    * channel exists to end.
    */
   for (unsigned i = 0; i < info.num_notes; i++) {
      fputs(i ? ";" : "\tnotes=", stdout);
      one_line(info.notes[i], stdout);
   }
   printf("\n");
   ralloc_free(nir);
   ralloc_free(ctx);
}

int
main(int argc, char **argv)
{
   enum { TRANSLATE, LL, LIST, REPORT, EVAL, INTERFACE } mode = TRANSLATE;
   int i = 1;
   while (i < argc && !strcmp(argv[i], "--library"))
      library_wrapper = true, i++;
   if (i < argc && !strcmp(argv[i], "--ll"))
      mode = LL, i++;
   else if (i < argc && !strcmp(argv[i], "--list"))
      mode = LIST, i++;
   else if (i < argc && !strcmp(argv[i], "--report"))
      mode = REPORT, i++;
   else if (i < argc && !strcmp(argv[i], "--eval"))
      mode = EVAL, i++;
   else if (i < argc && !strcmp(argv[i], "--interface"))
      mode = INTERFACE, i++;
   if (i >= argc) {
      fprintf(stderr,
              "usage: air2nir [--library] "
              "[--ll | --list | --report | --eval | --interface] "
              "FILE [FUNCTION]\n");
      return 2;
   }

   glsl_type_singleton_init_or_ref();
   void *ctx = ralloc_context(NULL);
   int status = 0;

   if (mode == REPORT) {
      setvbuf(stdout, NULL, _IOLBF, 0);
      for (; i < argc; i++) {
         struct module_ref *mods;
         unsigned n = modules_of(ctx, argv[i], NULL, &mods);
         for (unsigned k = 0; k < n; k++) {
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
               alarm(60);
               report_one(&mods[k]);
               fflush(stdout);
               _exit(0);
            }
            int ws;
            waitpid(pid, &ws, 0);
            if (!WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
               printf("module=%s\tcrash=%s %d\n", mods[k].name,
                      WIFSIGNALED(ws) ? "signal" : "exit",
                      WIFSIGNALED(ws) ? WTERMSIG(ws) : WEXITSTATUS(ws));
         }
      }
      ralloc_free(ctx);
      glsl_type_singleton_decref();
      return 0;
   }

   struct module_ref *mods;
   unsigned n = modules_of(ctx, argv[i], i + 1 < argc ? argv[i + 1] : NULL, &mods);
   if (n == 0)
      return 1;

   for (unsigned k = 0; k < n; k++) {
      const char *err = NULL;
      if (mode == EVAL) {
         status |= eval_module(&mods[k]);
         continue;
      }
      if (mode == INTERFACE) {
         status |= interface_module(&mods[k]);
         continue;
      }
      if (mode == LIST) {
         printf("%-60s %-12s %zu\n", mods[k].name,
                air_metallib_type_name(mods[k].container_type), mods[k].size);
         continue;
      }
      if (mode == LL) {
         struct air_module *m = air_module_read(ctx, mods[k].bitcode, mods[k].size, &err);
         if (!m) {
            fprintf(stderr, "air2nir: %s: %s\n", mods[k].name, err);
            status = 1;
            continue;
         }
         air_module_print(m, stdout);
         continue;
      }
      struct air_constant_value *fc;
      unsigned num_fc = air_module_function_constants(ctx, mods[k].bitcode, mods[k].size, &fc);
      struct air_to_nir_options options = { .constants = fc, .num_constants = num_fc,
                                        .library_wrapper = library_wrapper };
      struct air_shader_info info;
      nir_shader *nir = air_to_nir(ctx, mods[k].bitcode, mods[k].size, NULL, &options,
                                   &default_options, &info, &err);
      if (!nir) {
         fprintf(stderr, "air2nir: %s: %s\n", mods[k].name, err);
         status = 1;
         continue;
      }
      nir_print_shader(nir, stdout);
   }

   ralloc_free(ctx);
   glsl_type_singleton_decref();
   return status;
}
