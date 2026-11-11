/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air_run: draw with a Metal shader and read the pixels back.
 *
 * This is the half of M1 that compiling cannot give. The shader comes out of a
 * .metallib, through air_to_nir, into a Gallium pipe_shader_state as
 * PIPE_SHADER_IR_NIR -- which both drivers of interest accept natively: ir3
 * is a NIR compiler, and softpipe takes NIR through nir_to_tgsi. One quad is
 * drawn into a 32-bit-float render target and one pixel is printed, so the
 * result is a number and not a picture.
 *
 * On this machine the screen is softpipe, so the whole path can be run with no
 * GPU at all. On the phone the same source wants the freedreno screen that
 * tools/mesa/freedreno-darwin.patch already creates inside the port's Mesa
 * (osmesa_create_gpu_screen), and nothing else about it changes.
 *
 * usage: air_run FILE[.metallib] [FUNCTION]
 *        air_run --library FILE.metallib FUNCTION      one air.visible function, wrapped
 *        air_run --stitch "f(in0); g(n0)" FILE.metallib   a graph of them, stitched
 */

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"
#include "air_module.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"
#include "target-helpers/inline_sw_helper.h"
#include "null/null_sw_winsys.h"
#ifdef GALLIUM_FREEDRENO
#include "freedreno/drm/freedreno_drm_public.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include <xf86drm.h>
#endif
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <signal.h>
/* <sys/ucontext.h>, not <ucontext.h>: the latter refuses to be included
 * without _XOPEN_SOURCE on the iOS SDK, and userland/src/inject.c takes the
 * same route for the same reason.
 */
#include <sys/ucontext.h>
#endif

#define WIDTH 4
#define HEIGHT 4

/* The texture bound for every sampled texture the shader declares: four by
 * four, 32-bit floats, each texel a number that says which texel it is. The
 * sampler is nearest with no mip filter and clamped addressing, and the
 * coordinate the vertex shader hands over is the centre of texel (1, 1), so
 * a correct sample is an exact equality and not a tolerance -- no filtering
 * weights, no wrap arithmetic, nothing to round.
 *
 * This verifies the texture operations whose sampler the API chooses (nine
 * modules of the corpus) and those that take no sampler at all (twenty-nine,
 * which fetch by integer coordinate). It cannot verify the two whose sampler
 * the shader declares itself: that needs Metal's 64-bit sampler word decoded,
 * which it is not.
 */
#define TEX_SIZE 4
#define TEX_X 1
#define TEX_Y 1

/* Slot is in the value on purpose. Binding one texture to every slot makes a
 * shader that reads the wrong slot return the right texel, which is a test
 * passing for the wrong reason -- the third time that shape of mistake has
 * turned up here, after the 2D texture bound for a 3D sampler and the colour
 * attachment nothing stored to. Looked for deliberately this time rather than
 * tripped over: slot n adds a thousand times n, so a misbinding is visible.
 */
static void
texel_value(unsigned slot, unsigned x, unsigned y, unsigned z, float out[4])
{
   out[0] = 100.0f + x + 1000.0f * slot;
   out[1] = 200.0f + y;
   out[2] = 300.0f + x + y + z;
   out[3] = 400.0f;
}

#ifdef __APPLE__
/*
 * A program that dies on this port leaves nothing behind: the kernel says
 * "Failed to send exception EXC_CORPSE_NOTIFY" and that is the end of it. So
 * print the fault before dying. These two functions are userland/src/inject.c's
 * say_the_fault()/watch_for_faults() (branch sileo2, 6683521), with the slide
 * taken from this program's own image rather than from the shared cache, since
 * the pc of interest is in air_run or in libOSMesa and not in iOS.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
   ucontext_t *context = context_in;
   uintptr_t slide = (uintptr_t)_dyld_get_image_vmaddr_slide(0);
   uintptr_t pc = 0, lr = 0, fp = 0;

   if (context != NULL && context->uc_mcontext != NULL) {
      pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
      lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
      fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
   }
   /* who called, by the frame pointers: on arm64 each frame holds the previous
    * at [fp] and its return address at [fp + 8]
    */
   char frames[8 * 20 + 1] = "";
   size_t used = 0;
   for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
      uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
      if (ret == 0)
         break;
      used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
      fp = next > fp ? next : 0;   /* the chain only ever goes up the stack */
   }
   fprintf(stderr, "air_run: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n",
           signal_number, info != NULL ? info->si_addr : NULL, pc - slide, lr - slide,
           frames);
   fflush(stderr);
   signal(signal_number, SIG_DFL);
   raise(signal_number);
}

static void
watch_for_faults(void)
{
   struct sigaction action;

   memset(&action, 0, sizeof(action));
   action.sa_sigaction = say_the_fault;
   action.sa_flags = SA_SIGINFO | SA_RESETHAND;
   sigaction(SIGSEGV, &action, NULL);
   sigaction(SIGBUS, &action, NULL);
}
#else
static void watch_for_faults(void) {}
#endif

/* set by --library: the fill below is then floats, for the reason given there */
static bool library_fill = false;

/* set by --stitch: a function-stitching graph, as air_stitch.c reads it */
static const char *stitch_text = NULL;

/* set by --zero: every buffer the shader reads is zeroed instead of patterned.
 *
 * For the 14 kernels whose loops leave on a count or a sentinel read out of a
 * buffer, the pattern is not an input at all -- the loop never ends and the GPU
 * runs forever (docs/air.md section 5, tools/air/kstruct.py). Zeros are a
 * degenerate input but a *defined* one: a count of zero means the loop runs no
 * times, so the kernel reaches its end and the hardware answers. What that
 * proves is that the translated shader runs on the Adreno without hanging; it
 * proves nothing about what the kernel computes, and the harness says so rather
 * than printing the zeros as a result.
 */
static bool zero_fill = false;

/* set by --ones: every byte is 0xff.
 *
 * The other half of the same idea. A loop that counts up to a value from a
 * buffer ends at once when that value is zero; a loop that walks until it meets
 * a sentinel -- MetalFaceMask's span tables leave on -1 -- never meets it in a
 * buffer of zeros, and ends at once in a buffer of 0xff. Neither fill is an
 * input the kernel was written for, and between them they let most of these
 * kernels reach their end instead of running forever. Which of the two a kernel
 * terminates under is itself a fact about its shape.
 */
static bool ones_fill = false;

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

/* the pattern cpuref.py and air2nir --eval fill their buffers with, so that
 * all three agree on what the shader was given
 */
static uint8_t
pattern_byte(unsigned i)
{
   if (zero_fill)
      return 0;
   if (ones_fill)
      return 0xff;
   return library_fill ? library_byte(i) : (i * 7 + 13) & 0xff;
}

/* --buffer N@OFF=HEX puts those bytes into buffer N at that offset, over
 * whatever fill the rest of it keeps.
 *
 * It exists because the kernels that never finish do not need a whole buffer
 * chosen for them: they need a span table in one slot and a row index in
 * another, and the value their exit test compares against is a field or two
 * (tools/air/kinput.py works out which, and prints these arguments). A file is
 * one fill for every buffer, so it cannot say "this slot and not that one".
 * Offsets are decimal so that a job line stays readable; init.c splits a line
 * on blanks and knows no variables, so each kernel has to be one line.
 */
struct buffer_patch {
   unsigned index;
   unsigned off;
   unsigned len;
   uint8_t *bytes;
};
static struct buffer_patch patches[64];
static unsigned num_patches;

/* Parses "N@OFF=HEX". Returns false when the argument is not that shape, so
 * that --buffer keeps meaning a file name and a typo is not read as one.
 */
static bool
add_buffer_patch(const char *spec)
{
   char *end;
   unsigned long index = strtoul(spec, &end, 10);
   if (end == spec || *end != '@')
      return false;
   const char *p = end + 1;
   unsigned long off = strtoul(p, &end, 10);
   if (end == p || *end != '=')
      return false;

   const char *hex = end + 1;
   size_t n = strlen(hex);
   if (!n || n % 2)
      return false;
   for (size_t i = 0; i < n; i++)
      if (!isxdigit((unsigned char)hex[i]))
         return false;

   if (num_patches == ARRAY_SIZE(patches)) {
      fprintf(stderr, "air_run: more than %zu --buffer patches\n", ARRAY_SIZE(patches));
      exit(2);
   }
   struct buffer_patch *q = &patches[num_patches++];
   q->index = index;
   q->off = off;
   q->len = n / 2;
   q->bytes = malloc(q->len);
   for (unsigned i = 0; i < q->len; i++) {
      char byte[3] = { hex[2 * i], hex[2 * i + 1], 0 };
      q->bytes[i] = strtoul(byte, NULL, 16);
   }
   return true;
}

/* Writes the patches for one buffer and returns how many bytes they wrote.
 *
 * A patch that does not fit is refused out loud rather than clipped: these
 * bytes are what makes a loop end, and a kernel that silently did not get its
 * sentinel hangs the GPU, which the next boot pays for.
 */
static unsigned
apply_buffer_patches(unsigned index, uint8_t *bits, unsigned bytes)
{
   unsigned written = 0;
   for (unsigned i = 0; i < num_patches; i++) {
      const struct buffer_patch *q = &patches[i];
      if (q->index != index)
         continue;
      if (q->off + q->len > bytes) {
         printf("air_run: --buffer %u@%u= is %u bytes past the end of this %u-byte "
                "buffer and was NOT written\n", q->index, q->off,
                q->off + q->len - bytes, bytes);
         continue;
      }
      memcpy(bits + q->off, q->bytes, q->len);
      written += q->len;
   }
   return written;
}

/* Fill a buffer the shader is about to read.
 *
 * AIR_RUN_BUFFER=FILE puts that file's bytes there instead of the pattern,
 * which is what lets a test choose the arguments an operation is called on.
 * The reason it has to be a buffer and not a constant in the shader: ir3
 * folds, so a shader that computes air.fast_round(0.5) from an immediate is
 * answered by the compiler and the Adreno never runs the instruction -- the
 * test would pass without the hardware having been asked. A value loaded from
 * a buffer cannot be folded, and the harness knows what it put there.
 *
 * Returns how many bytes came from the file, so the caller can say so rather
 * than claim a fill it did not do. A file shorter than the buffer leaves the
 * rest as the pattern.
 */
static unsigned
fill_buffer(unsigned index, uint8_t *bits, unsigned bytes)
{
   for (unsigned k = 0; k < bytes; k++)
      bits[k] = pattern_byte(k);

   unsigned patched = apply_buffer_patches(index, bits, bytes);
   if (patched)
      printf("air_run: buffer at index %u, %u of its %u bytes from --buffer %u@...\n",
             index, patched, bytes, index);

   const char *path = getenv("AIR_RUN_BUFFER");
   if (!path || !*path)
      return 0;

   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "air_run: AIR_RUN_BUFFER=%s: %s\n", path, strerror(errno));
      return 0;
   }
   size_t got = fread(bits, 1, bytes, f);
   fclose(f);
   /* the file is the coarser statement -- one fill for every buffer -- so the
    * per-slot patches go on top of it and not under it
    */
   apply_buffer_patches(index, bits, bytes);
   return got;
}

/* A vertex shader for the quad: it takes the position as attribute 0 and
 * passes it through. Written here rather than translated, because what is
 * under test is the fragment shader; when the .metallib has a vertex shader of
 * its own that is a separate run.
 */
static nir_shader *
quad_vertex_shader(const nir_shader_compiler_options *options, unsigned num_varyings)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "quad");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "pos_in");
   in->data.location = VERT_ATTRIB_GENERIC0;
   in->data.driver_location = 0;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "pos_out");
   out->data.location = VARYING_SLOT_POS;
   out->data.driver_location = 0;
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

   /* One constant coordinate per varying the fragment shader reads: the
    * centre of texel (TEX_X, TEX_Y), so every fragment samples the same
    * texel and the expected value is a single known one.
    */
   float c = (TEX_X + 0.5f) / TEX_SIZE;
   float d = (TEX_Y + 0.5f) / TEX_SIZE;
   for (unsigned i = 0; i < num_varyings; i++) {
      nir_variable *v = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "varying");
      v->data.location = VARYING_SLOT_VAR0 + i;
      v->data.driver_location = 1 + i;
      v->data.interpolation = INTERP_MODE_SMOOTH;
      nir_store_var(&b, v, nir_imm_vec4(&b, c, d, c, 1.0f), 0xf);
   }
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 1 + num_varyings;
   return b.shader;
}

/* What a Gallium driver expects of a shader it is handed: NIR's own system
 * values lowered to the hardware's, and I/O as intrinsics rather than
 * variables. The same three passes are in tools/mesa/air-ir3.patch; when a
 * fourth place needs them they should become one function of the frontend.
 */
static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
finalize_for_gallium(nir_shader *nir, struct pipe_screen *screen)
{
   /* Does this screen's own backend lower I/O from the variables? softpipe's
    * does -- its shader state is built by nir_to_tgsi, which calls nir_lower_io
    * with this very mask and this very flag -- and ir3's does not.
    *
    * Gated on the screen's NAME, as the scratch block at the end of this
    * function is and for the same reason: there is no pipe cap that says "this
    * driver lowers I/O itself". PIPE_SHADER_CAP_SUPPORTED_IRS cannot stand in
    * for one, because freedreno advertises TGSI too (through tgsi_to_nir, which
    * is the other direction), and get_compiler_options returns a
    * nir_shader_compiler_options with no field for it. So the name is what
    * there is, and naming it is honest where inferring it from a cap that means
    * something else would not be.
    */
   const bool backend_lowers_io =
      screen->get_name && !strcmp(screen->get_name(screen), "softpipe");

   /* A fragment shader's [[position]]: the frontend hands it over as a system-value
    * variable, because that is the one form both of these drivers can be served from
    * (the long version is beside air.position in air_to_nir.c). freedreno sets
    * pipe_caps.fs_position_is_sysval and wants the intrinsic; softpipe does not set it
    * and reads (0,0,0,0) from the intrinsic, because nothing fills TGSI's system values
    * for a fragment shader. For such a screen the variable becomes an input varying
    * here -- and then needs a driver_location, which nothing else would give it.
    */
   if (nir->info.stage == MESA_SHADER_FRAGMENT && !screen->caps.fs_position_is_sysval) {
      static const struct nir_lower_sysvals_to_varyings_options pos_opts = {
         .frag_coord = true,
      };
      if (nir_lower_sysvals_to_varyings(nir, &pos_opts)) {
         nir_foreach_shader_in_variable(var, nir)
            if (var->data.location == VARYING_SLOT_POS)
               var->data.driver_location = nir->num_inputs++;
      }
   }
   NIR_PASS(_, nir, nir_lower_system_values);
   /* An options struct of all-false and not NULL, which are not the same thing
    * here and the difference is a trap. nir_lower_system_values *introduces*
    * load_base_workgroup_id when it lowers SYSTEM_VALUE_WORKGROUP_ID, and the
    * only thing that removes it again is
    *
    *     if (options && !options->has_base_workgroup_id)
    *        return nir_imm_zero(...);
    *
    * so NULL reads as "the driver has base workgroup ids" -- the opposite of
    * what a harness wants -- and the intrinsic survives into the backend. With
    * softpipe that is an assertion in nir_to_tgsi rather than a message, and it
    * stopped every kernel that asks for its workgroup id. Zero is also the
    * right answer here: this dispatches one workgroup from the origin.
    */
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
   /*
    * nir_lower_io_use_interpolated_input_intrinsics, and WITHOUT it every varying
    * was FLAT on the Adreno while softpipe interpolated it. Mesa's own comment on the
    * flag says exactly why: without it nir_lower_io "generates regular load_input
    * intrinsics and interpolation information must be inferred from the list of input
    * nir_variables" -- and the two consumers infer it from different places.
    * nir_to_tgsi declares each FS input from its variable's data.interpolation (SMOOTH,
    * as the frontend sets it), so softpipe was right by accident; ir3's setup_input
    * does `so->inputs[n].flat = !coord` and only builds `coord` for
    * load_interpolated_input, so a plain load_input is flat, full stop.
    *
    * What that cost: the one draw in this project whose varying actually varies --
    * Apple's texturedVertex2D handing a texcoord to texturedFragment2D* -- read the
    * provoking vertex's texcoord at every fragment on the phone, and the picture looked
    * VERTICALLY FLIPPED for three boots (docs/metal.md 10.26, 10.32). It is not flipped.
    * The quad's pixel (0,0) sits in the triangle whose last vertex carries uv (0,1), so
    * flat shading hands it texel (0,3) -- `100 203 303 400`, bit for bit the number the
    * device printed, and reproduced here on softpipe by forcing INTERP_MODE_FLAT.
    *
    * Why nothing caught it: the 255 fragment shaders compared pixel by pixel are drawn
    * over the generated vertex stage, which gives every vertex the SAME texcoord -- flat
    * and smooth are identical over a constant. And ir3_compiler, the tool the corpus's
    * "compiles through ir3" column comes from, passes this flag already
    * (tools/mesa/air-ir3.patch): the tool was lowering correctly while the two harnesses
    * that actually draw were not.
    *
    * ... and it is skipped entirely for a backend that lowers I/O itself, which
    * is what made this harness unable to draw a fragment shader that reads a
    * varying. nir_to_tgsi lowers I/O a SECOND time, from the variables, with
    * the same mask, the same flag and a type_size that is type_size_vec4 word
    * for word -- and it opens with
    * nir_remove_dead_variables(nir_var_shader_in). Lowering here first leaves a
    * shader whose inputs are intrinsics and whose input variables are held up
    * by nothing but the orphaned deref_var instructions nir_lower_io does not
    * delete; the scratch block below then runs nir_opt_dce, those go, ntt
    * deletes the variables, and ntt_setup_inputs sizes input_index_map by the
    * driver_locations of the variables that are left -- none. The
    * load_interpolated_input this pass already emitted then swizzles a
    * TGSI_FILE_NULL register and tgsi_ureg asserts, before any format, texture
    * or sampler is reached.
    *
    * That abort was read as softpipe's limit in two places (pixfmtcheck's 30
    * SAMPLED cases, metalcheck's draw sweep) and it is this file's. Counted:
    * 134 of the corpus's 400 fragment entry points have an input variable on
    * this screen -- 125 read a [[stage_in]] varying and nine read only
    * [[position]], which the block above turns into one -- and 87 of them are
    * QuartzCore's, the library every pixel of the screen is drawn with. Two of
    * the 134 drew before this and both drew the WRONG pixel: the register
    * ntt swizzled was whatever the heap held under a zero-length
    * input_index_map, so the abort was the loud outcome and not the only one.
    * air_metal.c runs the same passes without the dce and draws those shaders
    * through the same softpipe, which is what said the limit was not softpipe's.
    *
    * Not lowering is preferred over keeping the derefs alive past the dce
    * because it leaves ONE lowering of I/O in the pipeline instead of one and a
    * half: ntt also runs nir_lower_indirect_derefs(nir_var_shader_in) before it
    * lowers, which is a no-op on a shader whose derefs are already gone and is
    * there because ureg splits input declarations to elements.
    */
   if (!backend_lowers_io)
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
               type_size_vec4, nir_lower_io_use_interpolated_input_intrinsics);

   /* SCRATCH, for a screen that has none.
    *
    * An `alloca` becomes load_scratch and store_scratch, which ir3 compiles
    * and nir_to_tgsi has no case for at all: softpipe printed
    * "Unknown intrinsic: @store_scratch" and then asserted inside ureg_swizzle
    * on the undefined register it had made nothing for.
    *
    * WHAT IT ACTUALLY UNLOCKS, measured and not assumed: tools/air/callptr.ll,
    * which is the whole reason it is here -- a shader with an alloca in it and
    * nothing else nir_to_tgsi lacks, so LLVM on this machine and this draw can
    * be compared and they agree on all four lanes. It does NOT make Apple's own
    * RenderBox modules draw here: all fourteen of the ones that hand a shape
    * function a `ptr` still refuse on softpipe, now for a reason that has
    * nothing to do with scratch (`u2u32` from 16 bits asserts `src_64` in
    * ntt_emit_alu, and these shaders are full of halves). The scratch is off
    * the list of reasons; the list is not empty.
    *
    * nir_lower_scratch_to_var puts the whole scratch space back into one local
    * variable, which nir_lower_vars_to_ssa then turns into ordinary values.
    * That is sound exactly where this harness runs it: one invocation per
    * fragment, no addresses taken across a call (nir_inline_functions has
    * already folded every callee in), and nothing else reading the space.
    *
    * Gated on the screen by NAME and not run unconditionally, because the
    * point of this harness is to put the SAME NIR through two drivers: doing
    * it for ir3 as well would mean the Adreno column measured a shader with
    * the scratch lowered away, which is not the shader the phone runs. There
    * is no pipe cap that says "this driver has no scratch", so the name is
    * what there is, and it is named here rather than guessed at from a cap
    * that means something else.
    *
    * The nir_opt_dce at the end of this block is the pass that used to orphan
    * every [[stage_in]] variable, and it is kept: with I/O left unlowered above
    * for exactly the screens this block runs on, a load_deref still references
    * each one, so there is nothing dead to remove. That is a coupling between
    * two gates that test the same screen for different reasons, and the thing
    * that would notice it coming apart is a test rather than a comment --
    * tools/air/pixfmtcheck.py's last case draws one of Apple's varying-reading
    * shaders and fails if the abort comes back.
    */
   if (screen->get_name && !strcmp(screen->get_name(screen), "softpipe")) {
      NIR_PASS(_, nir, nir_lower_scratch_to_var);
      NIR_PASS(_, nir, nir_split_var_copies);
      NIR_PASS(_, nir, nir_lower_var_copies);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, nir, nir_opt_dce);
   }
}

/* -- compute ---------------------------------------------------------------
 *
 * A kernel writes through pointers instead of returning anything, so what is
 * read back is a device buffer. Two rules decide whether that readback is
 * evidence, and both come from the same mistake made three times with
 * textures: a test can pass for the wrong reason, and the way to stop it is
 * to make every input distinguishable so any confusion gives a wrong number
 * rather than the right one.
 *
 *   - the grid is more than one thread, and every element of the output is
 *     printed, not just the first. With one thread, or one element looked at,
 *     a wrong workgroup index or a wrong global id reads correct.
 *   - the input buffers vary with position (the same (i * 7 + 13) & 0xff
 *     pattern), so a thread reading the wrong offset produces a wrong value
 *     and not a plausible one.
 *
 * A kernel of Apple's that uses atomics and produces sensible numbers here is
 * evidence that it ran and no evidence about atomicity: a sum written by
 * threads that never raced looks exactly like a sum written by threads that
 * did. The kernel that can tell them apart is written for the purpose rather
 * than Apple's -- tools/air/atomiccheck.py, which makes each thread report the
 * value its own fetch_add returned, so the sixteen of them must be 0..15 with
 * nothing repeated whatever order the threads ran in. Even that is only
 * evidence of atomicity where the threads really do run at once, which here
 * they need not.
 */
#define GRID 4

/* The threadgroup this harness dispatches. 4x4x1 by default, because a grid with more than
 * one thread in two dimensions is what makes a wrong index show, and --block X,Y,Z because
 * some kernels are written for a particular one and say so in their own code: both fbs_pcg_*
 * kernels end with `if (tid == 0)` summing 128 entries of a threadgroup array, which is a
 * 128x1x1 reduction. Under 4x4x1 four threads have tid == 0 -- Metal's scalar
 * thread_position_in_threadgroup is the x component, not the flattened index -- so four of
 * them run the whole reduction over the four entries this shape writes, and the word they
 * add up is a property of the dispatch rather than of the kernel (docs/air.md section 5).
 */
static unsigned block[3] = { GRID, GRID, 1 };

#define MAX_IMG 16

/* The shape of each image a kernel touches, read off the intrinsics the
 * frontend emitted rather than off the Metal type name.
 *
 * air.read_texture_* and air.write_texture_* become nir_image_load and
 * nir_image_store (air_intrinsics.c), not texture instructions, so they want
 * PIPE_BIND_SHADER_IMAGE and set_shader_images and not a sampler view -- which
 * is why the draw path's texture code cannot simply be reused here.
 *
 * Per binding and not one format for the kernel: BlitLumaKernel_Unorm reads a
 * uint image and writes a half one, so a single format would be right for one
 * of its two images and quietly wrong for the other. The binding is src[0] and
 * the frontend always emits it as an immediate, so it can be read here.
 */
static void
image_shapes(nir_shader *cs, enum pipe_format fmt[MAX_IMG], bool isf[MAX_IMG],
             enum glsl_sampler_dim dim[MAX_IMG], bool seen[MAX_IMG], unsigned *num)
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            nir_alu_type t;
            if (in->intrinsic == nir_intrinsic_image_load)
               t = nir_intrinsic_dest_type(in);
            else if (in->intrinsic == nir_intrinsic_image_store)
               t = nir_intrinsic_src_type(in);
            else
               continue;
            if (!nir_src_is_const(in->src[0]))
               continue;   /* a computed binding; refused by the caller's count */
            unsigned b = nir_src_as_uint(in->src[0]);
            if (b >= MAX_IMG || seen[b])
               continue;
            /* The WIDTH as well as the base type.
             *
             * A 32-bit format for a 16-bit store is not a conversion, it is a
             * descriptor that disagrees with the instruction: ir3 emits
             * `stib.b.typed.2d.u16.4` from air.write_texture_2d.v4f16 and the
             * image it is given says 32 bits per component. MetalFaceMask_DrawSpans
             * is that case -- it declares texture2d<half, write> -- and it is the
             * kernel whose dispatch took a whole boot with it in
             * logs/070755-run.log, printing its bindings and then nothing at all.
             * 22 kernels of the corpus emit a narrow typed image store, so this is
             * not one shader's accident. GL has no 16-bit image store, which is why
             * neither Mesa nor this harness had met it; the draw path already picks
             * the narrow format when a fragment shader returns halves.
             */
            unsigned bits = nir_alu_type_get_type_size(t);

            switch (nir_alu_type_get_base_type(t)) {
            case nir_type_int:
               fmt[b] = bits == 16 ? PIPE_FORMAT_R16G16B16A16_SINT
                      : bits == 8  ? PIPE_FORMAT_R8G8B8A8_SINT
                                   : PIPE_FORMAT_R32G32B32A32_SINT;
               isf[b] = false;
               break;
            case nir_type_uint:
               fmt[b] = bits == 16 ? PIPE_FORMAT_R16G16B16A16_UINT
                      : bits == 8  ? PIPE_FORMAT_R8G8B8A8_UINT
                                   : PIPE_FORMAT_R32G32B32A32_UINT;
               isf[b] = false;
               break;
            default:
               fmt[b] = bits == 16 ? PIPE_FORMAT_R16G16B16A16_FLOAT
                                   : PIPE_FORMAT_R32G32B32A32_FLOAT;
               isf[b] = true;
               break;
            }
            dim[b] = nir_intrinsic_image_dim(in);
            seen[b] = true;
            *num = MAX2(*num, b + 1);
         }
      }
   }
}

/* The same, for the read-only textures a kernel fetches from. A texture is an
 * AIR_RESOURCE_IMAGE only when it is written (air_to_nir.c): a read-only one
 * stays a texture and Metal's read() on it becomes a txf, a texture fetch, so
 * it needs a sampler view even inside a compute shader. BlitKernel_Unorm has
 * one of each -- a txf from its source and an image_store to its destination --
 * and binding only the image left the fetch reading nothing.
 */
static void
texture_shapes(nir_shader *cs, enum pipe_format fmt[MAX_IMG], bool isf[MAX_IMG],
               enum glsl_sampler_dim dim[MAX_IMG], bool seen[MAX_IMG], unsigned *num)
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            unsigned b = t->texture_index;
            if (b >= MAX_IMG || seen[b])
               continue;
            switch (nir_alu_type_get_base_type(t->dest_type)) {
            case nir_type_int:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_SINT; isf[b] = false; break;
            case nir_type_uint:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_UINT; isf[b] = false; break;
            default:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_FLOAT; isf[b] = true; break;
            }
            dim[b] = t->sampler_dim;
            seen[b] = true;
            *num = MAX2(*num, b + 1);
         }
      }
   }
}

/* Wait for the work to finish, and say so when it does not.
 *
 * This matters more than it looks. On 20 October a dispatch hung the GPU --
 * our own driver said "gpu: no answer: submission 2 is the last, the core has
 * done 1" -- and every dispatch for the rest of that boot was accepted,
 * returned, and did nothing. air_run did not wait on a fence, so it printed
 * the buffer it had filled itself as though the GPU had answered: a page of
 * numbers that were a statement about the harness and not about the shader.
 * 21 of the 47 kernels of one run were vacuous that way before anyone noticed
 * (docs/air.md section 5).
 *
 * The hang itself turned out to be this harness too: the kernels that did it
 * loop until a buffer value they were never given, and the fill is a byte
 * pattern. So the message below leads with that rather than with the GPU.
 *
 * So the fence is waited on with a timeout, and a timeout is reported as
 * loudly as a crash. A dispatch nobody waited for is not a measurement.
 */
static bool
finish_or_say_why(struct pipe_screen *screen, struct pipe_context *pipe, const char *what)
{
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (!fence) {
      printf("air_run: the driver gave no fence for the %s, so nothing here waited for "
             "the GPU and the numbers below may be the fill\n", what);
      return true;
   }
   bool done = screen->fence_finish(screen, NULL, fence, 2000000000ull); /* 2 s */
   screen->fence_reference(screen, &fence, NULL);
   if (!done)
      printf("air_run: THE GPU DID NOT FINISH the %s within two seconds. Everything below is "
             "what this harness wrote, not what the shader computed. The likeliest reason is "
             "this harness: a kernel whose buffers carry counts, indices or sentinels is being "
             "given (i * 7 + 13) & 0xff, and a loop that leaves on a value it was never given "
             "does not leave at all -- both kernels measured this way turned out to be exactly "
             "that (docs/air.md section 5). And one timeout wedges this GPU for the rest of the "
             "boot, so every later run in it is vacuous too.\n", what);
   return done;
}

/* What a shader's own sampler says, in one line. Printed rather than assumed:
 * the ten modules of the corpus that carry one have no reference to be checked
 * against, so the only thing a reader can verify is that the harness bound
 * what the word says.
 */
static void
print_own_sampler(unsigned slot, const struct air_sampler_state *a, bool compares)
{
   static const char *const addr[] = { "clamp_to_zero", "clamp_to_edge", "repeat",
                                       "mirrored_repeat", "clamp_to_border" };
   static const char *const filt[] = { "nearest", "linear", "bicubic" };
   static const char *const mip[] = { "none", "nearest", "linear" };
   /* Metal's compare_func, in its own order: `none` at 0 and `never` at 8, which is
    * the value Apple's default carries (docs/air.md 3.3).
    */
   static const char *const cmp[] = { "none", "less", "less_equal", "greater",
                                      "greater_equal", "equal", "not_equal", "always",
                                      "never" };
   printf("air_run: sampler %u is the shader's own: %s/%s filtering, mip %s, %s "
          "addressing, %s coordinates, compare_func::%s%s\n", slot,
          filt[a->mag_filter % ARRAY_SIZE(filt)], filt[a->min_filter % ARRAY_SIZE(filt)],
          mip[a->mip_filter % ARRAY_SIZE(mip)],
          addr[a->address[0] % ARRAY_SIZE(addr)],
          a->pixel_coord ? "pixel" : "normalized",
          a->compare_func < ARRAY_SIZE(cmp) ? cmp[a->compare_func] : "?",
          compares ? " APPLIED (a shadow sample reads this slot)"
                   : " not read (nothing samples this slot with a comparison)");
}

/*
 * THE PACKED 16-BIT PROBE: one known word, both directions, on whatever screen this is.
 *
 * air_metal.c has to decide which pipe_format each of Metal's four packed 16-bit names
 * means, and that decision has two halves. Which Metal name goes with which Gallium name
 * is Apple's convention and only Apple's own compositor can settle it. Where the fields
 * of a given GALLIUM name actually land is a fact about the driver and the hardware, and
 * that is what this measures -- on the Adreno, which is the only place it matters and
 * the one place `air_metal_test --pixel-formats` cannot run, because that program is not
 * on the device and air_run is.
 *
 * TWO FLAGS AND NOT ONE, because a round trip measures nothing. Sampling a packed
 * texture and rendering into a packed target of the same format is unpack then pack with
 * the same layout, which is the identity whatever that layout is: it would catch a read
 * path disagreeing with a write path and could never catch both being wrong together.
 * So the two directions are separate runs with a known value at the end that is NOT
 * packed:
 *
 *   --packed16 FORMAT 0xWORD   every texel of the bound texture is that 16-bit word,
 *                              with one field set; the target is the ordinary float one,
 *                              so the four numbers printed say which CHANNEL the field
 *                              became. The SAMPLED direction.
 *   --packed16-rt FORMAT       the colour target is that packed format and the shader
 *                              supplies the colour, so the word printed says which FIELD
 *                              each channel became. The WRITTEN direction, and it wants
 *                              a module whose output is a constant -- four floats could
 *                              not show it, because they would have come back out
 *                              through the same unpacking the write used.
 *
 * FORMAT is one of the eight Gallium names the two readings of Metal's four can mean. A
 * name and not a number, so the job line says which layout it is asking about and a
 * reader needs nothing else.
 */
static const struct { const char *name; enum pipe_format fmt; } packed16_names[] = {
   { "R5G6B5_UNORM",   PIPE_FORMAT_R5G6B5_UNORM },
   { "B5G6R5_UNORM",   PIPE_FORMAT_B5G6R5_UNORM },
   { "A1R5G5B5_UNORM", PIPE_FORMAT_A1R5G5B5_UNORM },
   { "A1B5G5R5_UNORM", PIPE_FORMAT_A1B5G5R5_UNORM },
   { "R5G5B5A1_UNORM", PIPE_FORMAT_R5G5B5A1_UNORM },
   { "B5G5R5A1_UNORM", PIPE_FORMAT_B5G5R5A1_UNORM },
   { "A4R4G4B4_UNORM", PIPE_FORMAT_A4R4G4B4_UNORM },
   { "A4B4G4R4_UNORM", PIPE_FORMAT_A4B4G4R4_UNORM },
   /* the control, and the census is why it is here: the window server makes a
    * BGRA8 texture of exactly the shape it makes the packed one
    */
   { "B8G8R8A8_UNORM", PIPE_FORMAT_B8G8R8A8_UNORM },
};
static enum pipe_format packed16_fmt = PIPE_FORMAT_NONE;   /* the sampled texture */
static enum pipe_format packed16_rt = PIPE_FORMAT_NONE;    /* ... or the colour target */
static enum pipe_format miplevel_fmt = PIPE_FORMAT_NONE;   /* ... or a mip pyramid */
/*
 * --clear: what a framebuffer fetch will read, PER RENDER TARGET.
 *
 * Softpipe's TGSI interpreter has no FBFETCH at all, so the only driver here that
 * can answer a fetch is the one on the phone -- and there, a fetch in a single
 * draw reads what the clear put down. Chosen numbers turn that into a
 * measurement: a shader that hands the fetch back must return the clear, and
 * zero is then distinguishable from a fetch that read nothing.
 *
 * Per target and not one colour for all of them, because the question that
 * matters is WHICH ATTACHMENT was read. The window server's own passes carry two
 * colour attachments and mask one of them off entirely; a fetch that read the
 * other one would be invisible if both held the same numbers.
 */
#define MAX_CLEARS 8
static bool halftex;
static union pipe_color_union clear_colour[MAX_CLEARS];
static unsigned clear_count;
static bool clear_given;
static const char *packed16_name;
static unsigned packed16_word;

/*
 * THE MIP PYRAMID, at the shape the window server actually asks for.
 *
 * The texture census of a whole boot (userland/src/redwoodmetal.m's texture line)
 * found exactly ONE packed texture in the entire system: MTLPixelFormatBGR5A1Unorm,
 * 2D, shaderRead|renderTarget, TEN MIP LEVELS, 776x24. Nothing else packed is ever
 * created -- not formats 40, 41 or 42, and SpringBoard asks for none at all. So a
 * probe on a single-level 4x4 texture is a probe of something the system does not
 * have, and a mip chain is where a two-byte format differs most from the
 * four-byte one everything else uses.
 *
 * AND THE SAME CENSUS CARRIES ITS OWN CONTROL: a format 80 (BGRA8Unorm) texture of
 * the IDENTICAL shape -- type 2, usage 0x5, mips 10, 776x24. One logical object in
 * two formats. Run through this probe, BGRA8 is the line that says whether a
 * 776x24 pyramid works at all on this GPU, so a failure confined to the packed
 * formats cannot be blamed on the shape, the usage, the level count or the
 * harness.
 *
 * THE LEVEL IS IN THE VALUE, which is this harness's rule everywhere else: level L
 * is filled with a texel whose four channels are the binary code of L + 1, each
 * channel either all-ones or all-zeros. Three things follow. A sample that read
 * the wrong level names the level it actually read, rather than merely
 * disagreeing. Every expectation is exactly 0.0 or exactly 1.0 whatever the field
 * widths are -- a 5-bit field at 9/31 would not be, and an expectation needing a
 * tolerance cannot tell a near miss from a hit. And L + 1 RATHER THAN L, so that
 * no level's code is zero: with L the first level would read 0 0 0 0, which is
 * also what an unwritten level and an untouched allocation read, and the one
 * outcome this is built to catch would have been indistinguishable from the
 * healthy one.
 */
/*
 * --halftex: a 256x1 RGBA16Float lookup table, written as RAW HALF BIT PATTERNS.
 *
 * CoreAnimation's gradient shader -- axial_shading_frag_lpf, 47 NIR instructions
 * and one air.sample_texture_2d -- does nothing but sample a table of this exact
 * shape and write the result, and the census found it: format 115, 2D, 256x1,
 * shaderRead only, so the 256 entries are built on the CPU and uploaded. That
 * upload path's half conversion has never been measured here.
 *
 * THE WORDS ARE CHOSEN AS INTEGERS AND WRITTEN AS INTEGERS, with no float-to-half
 * conversion anywhere in this program, because a test that samples a table it
 * also converted would pass with both halves wrong. The expectation is decoded
 * from the same integers by a different implementation of binary16 on the host,
 * so the two sides share nothing but the bit patterns.
 *
 * Texel i carries four patterns chosen to span what binary16 can be and nothing
 * it cannot:
 *
 *   r  (i << 7) & 0x7bff   a NORMAL of rising exponent; masked so the exponent
 *                          never reaches 31, which would be an infinity or a NaN
 *   g  r | 0x8000          the same magnitude NEGATIVE, so a sign dropped shows
 *   b  i                   a DENORMAL for every i, and exactly zero at i = 0
 *   a  0x3c00              exactly 1.0, so one channel is a constant a wrong
 *                          channel order cannot hide in
 */
#define HALFTEX_W 256

static void
halftex_texel(unsigned i, uint16_t out[4])
{
   out[0] = (uint16_t)((i << 7) & 0x7bff);
   out[1] = (uint16_t)(out[0] | 0x8000);
   out[2] = (uint16_t)i;
   out[3] = 0x3c00;
}

#define MIPTEX_W 776
#define MIPTEX_H 24
#define MIPTEX_LEVELS 10

static void
miplevel_texel(unsigned level, float out[4])
{
   for (unsigned c = 0; c < 4; c++)
      out[c] = ((level + 1) >> c) & 1 ? 1.0f : 0.0f;
}

static enum pipe_format
packed16_by_name(const char *want, const char **name)
{
   for (unsigned i = 0; i < ARRAY_SIZE(packed16_names); i++)
      if (!strcmp(want, packed16_names[i].name)) {
         *name = packed16_names[i].name;
         return packed16_names[i].fmt;
      }
   fprintf(stderr, "air_run: --packed16 wants one of these names:");
   for (unsigned i = 0; i < ARRAY_SIZE(packed16_names); i++)
      fprintf(stderr, " %s", packed16_names[i].name);
   fprintf(stderr, "\n");
   return PIPE_FORMAT_NONE;
}

/* The depth texel a shadow sample reads, which is the only pattern that fits.
 *
 * A depth format is normalised to [0, 1] and this harness's colour pattern starts at
 * 100, so the two cannot be the same rule. (x + TEX_SIZE*y) / 16 gives sixteen
 * distinct values over a 4x4 texture, every one exactly representable in binary32 and
 * every one a different answer -- so a comparison that read the wrong texel answers
 * differently, and a reference landing between two of them is exact on both sides.
 * tools/air/sampcheck.py computes its expectations from this same expression.
 */
static float
depth_texel_value(unsigned x, unsigned y)
{
   return (float)(x + TEX_SIZE * y) / 16.0f;
}

/* Which sampler slots a shadow texture instruction reads, out of the translated
 * shader itself.
 *
 * Metal decides AT THE SAMPLE CALL that a comparison happens: sample() and
 * sample_compare() are two instructions over one sampler, and the sampler word
 * only says WHICH comparison. Gallium decides it in the sampler state, through
 * pipe_sampler_state.compare_mode, and softpipe compares on every sample of a
 * sampler that has it set (sp_tex_sample.c: `if (compare_mode != NONE)
 * sample_compare(...)`). So switching it on for a slot a plain sample() reads
 * would turn every texel into a 0 or a 1, and leaving it off for a slot a
 * sample_compare() reads would return the texel instead of the comparison.
 * Exactly these slots, and the shader is the only thing that knows which.
 *
 * On the Adreno the same two lines mean something slightly different and the
 * answer is the same: a6xx has no compare-enable bit at all, only
 * A6XX_TEX_SAMP_1_COMPARE_FUNC, which fd6_sampler_state_create writes only when
 * compare_mode is set -- and the hardware reads it only for the shadow form of
 * the sample instruction, which ir3 selects from nir_tex_instr::is_shadow
 * (IR3_INSTR_S). So the field has to be programmed for these slots, and
 * programming it for the others would be harmless there and wrong here.
 */
static void
compare_sampled_slots(nir_shader *s, bool *slot, unsigned count)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            if (t->is_shadow && t->sampler_index < count)
               slot[t->sampler_index] = true;
         }
      }
   }
}

/* A comparison through a sampler whose own word says `compare_func::none`,
 * refused by name and in the same words air_metal.c's bind_stage refuses it.
 *
 * MTLCompareFunction has eight values and no "not configured", so `none` --
 * value 0 of the shader word's nine -- has no API spelling and arrives at a
 * driver as Never, which is MTLSamplerDescriptor's own default and is right for
 * the plain sample that is all either can do. A COMPARISON is where the two
 * part company: AGX keeps its NONE and its NEVER apart (AGXCompilerCore's
 * _AGCCompareFunction), and what it does differently is not readable from here.
 * Answering such a sample as `never` would be a harness quietly agreeing with
 * itself where the library this is a reference for says no.
 */
static bool
compare_without_a_function(const struct air_shader_info *info,
                           const bool *compare_slot, unsigned count,
                           const char *name)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind != AIR_RESOURCE_SAMPLER || !r->inline_sampler)
         continue;
      if (r->index >= count || !compare_slot[r->index])
         continue;
      if (r->sampler.compare_func != 0)
         continue;
      fprintf(stderr, "air_run: %s samples sampler %u with a comparison and its own "
                      "word says compare_func::none, which is not one of "
                      "MTLCompareFunction's eight: a sampler that was not made to "
                      "compare\n", name, r->index);
      return true;
   }
   return false;
}

/* The sampler state a shader declared for itself, as Gallium wants it.
 *
 * Metal's clamp_to_zero is a wrap mode and not a border colour: outside the
 * texture it reads transparent black, in every channel including alpha, and
 * the border colour is not configurable for it. Gallium has no such mode, so
 * it becomes CLAMP_TO_BORDER with a zero border -- which is the same thing and
 * is worth writing down, because the obvious reading of that enum value is
 * clamp_to_border with a default border, and one of Apple's own binaries
 * prints it that way (nir_air.h).
 *
 * Fields with no Gallium equivalent, or none this harness can honour, are left
 * at the default and named by the caller rather than silently dropped:
 * bicubic filtering and the reduction mode AGX itself does not read.
 *
 * The comparison function is honoured, and only for the slots a shadow
 * instruction reads -- `compares`, from compare_sampled_slots above. Its own
 * enumeration is not MTLCompareFunction's and not Gallium's: in the word 0 is
 * `none` and `never` is 8, which is the value Apple's own default carries, so
 * every `constexpr sampler` that names no compare_func arrives here with 8 and
 * must mean no comparison rather than an unknown one.
 */
static struct pipe_sampler_state
sampler_from_air(const struct air_sampler_state *a, bool compares)
{
   static const unsigned wrap[] = {
      [AIR_SAMPLER_CLAMP_TO_ZERO] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
      [AIR_SAMPLER_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      [AIR_SAMPLER_REPEAT] = PIPE_TEX_WRAP_REPEAT,
      [AIR_SAMPLER_MIRRORED_REPEAT] = PIPE_TEX_WRAP_MIRROR_REPEAT,
      [AIR_SAMPLER_CLAMP_TO_BORDER] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
   };
   static const unsigned filt[] = {
      [AIR_SAMPLER_NEAREST] = PIPE_TEX_FILTER_NEAREST,
      [AIR_SAMPLER_LINEAR] = PIPE_TEX_FILTER_LINEAR,
      [AIR_SAMPLER_BICUBIC] = PIPE_TEX_FILTER_LINEAR,
   };
   static const unsigned mip[] = {
      [AIR_SAMPLER_MIP_NONE] = PIPE_TEX_MIPFILTER_NONE,
      [AIR_SAMPLER_MIP_NEAREST] = PIPE_TEX_MIPFILTER_NEAREST,
      [AIR_SAMPLER_MIP_LINEAR] = PIPE_TEX_MIPFILTER_LINEAR,
   };
   struct pipe_sampler_state st = {
      .wrap_s = wrap[a->address[0] % ARRAY_SIZE(wrap)],
      .wrap_t = wrap[a->address[1] % ARRAY_SIZE(wrap)],
      .wrap_r = wrap[a->address[2] % ARRAY_SIZE(wrap)],
      .mag_img_filter = filt[a->mag_filter % ARRAY_SIZE(filt)],
      .min_img_filter = filt[a->min_filter % ARRAY_SIZE(filt)],
      .min_mip_filter = mip[a->mip_filter % ARRAY_SIZE(mip)],
      .unnormalized_coords = a->pixel_coord,
      .max_anisotropy = a->max_anisotropy > 1 ? a->max_anisotropy : 0,
      .min_lod = _mesa_half_to_float(a->lod_min),
      .max_lod = _mesa_half_to_float(a->lod_max),
   };
   /* The comparison, where the shader asked for one. The table is the one in
    * air_metal.c's sampler_state_from_air -- MTLCompareFunction and
    * PIPE_FUNC_x run in the same order, so one table serves both -- and it is
    * written out here rather than cast because 2 `less_equal` would arrive as
    * PIPE_FUNC_EQUAL and 3 `greater` as PIPE_FUNC_LEQUAL, which is the
    * opposite comparison and still a picture.
    */
   if (compares) {
      static const unsigned func[] = {
         [0] = PIPE_FUNC_NEVER,     /* none: no MTLCompareFunction has a name for it */
         [1] = PIPE_FUNC_LESS,      /* less */
         [2] = PIPE_FUNC_LEQUAL,    /* less_equal */
         [3] = PIPE_FUNC_GREATER,   /* greater */
         [4] = PIPE_FUNC_GEQUAL,    /* greater_equal */
         [5] = PIPE_FUNC_EQUAL,     /* equal */
         [6] = PIPE_FUNC_NOTEQUAL,  /* not_equal */
         [7] = PIPE_FUNC_ALWAYS,    /* always */
         [8] = PIPE_FUNC_NEVER,     /* never */
      };
      st.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
      st.compare_func = func[a->compare_func % ARRAY_SIZE(func)];
   }

   /* Metal's border_color: 0 transparent black, 1 opaque black, 2 opaque white.
    * clamp_to_zero does not read it -- it is transparent black by definition.
    */
   if (a->address[0] == AIR_SAMPLER_CLAMP_TO_BORDER ||
       a->address[1] == AIR_SAMPLER_CLAMP_TO_BORDER ||
       a->address[2] == AIR_SAMPLER_CLAMP_TO_BORDER) {
      float v = a->border_color == 2 ? 1.0f : 0.0f;
      st.border_color.f[0] = st.border_color.f[1] = st.border_color.f[2] = v;
      st.border_color.f[3] = a->border_color == 0 ? 0.0f : 1.0f;
   }
   return st;
}

/* The state for sampler slot `idx`: the shader's own if it declared one there,
 * and this harness's nearest/clamp-to-edge otherwise -- which is what the API
 * would have bound and what every measurement before inline samplers were
 * decoded was made with.
 */
static struct pipe_sampler_state
sampler_for_slot(const struct air_shader_info *info, unsigned idx, bool unnormalized,
                 bool compares, const struct air_sampler_state **from_shader)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_SAMPLER && r->inline_sampler && r->index == idx) {
         if (from_shader)
            *from_shader = &r->sampler;
         return sampler_from_air(&r->sampler, compares);
      }
   }
   if (from_shader)
      *from_shader = NULL;
   /* The API's sampler, and a comparison only if the shader asked for one.
    * MTLSamplerDescriptor's own compareFunction defaults to Never, so this is
    * the API's default sampler said completely rather than left out.
    */
   return (struct pipe_sampler_state){
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      .unnormalized_coords = unnormalized,
      .compare_mode = compares ? PIPE_TEX_COMPARE_R_TO_TEXTURE
                               : PIPE_TEX_COMPARE_NONE,
      .compare_func = PIPE_FUNC_NEVER,
   };
}

static int
dispatch_kernel(struct pipe_screen *screen, const struct air_shader_info *info,
                nir_shader *cs, const char *name, const char *how)
{
   /* Metal's threadgroup size is a dispatch argument, not a shader constant,
    * so the frontend leaves it variable (docs/air.md section 7.4) and whoever
    * dispatches has to fix it. air_run is that whoever. It is not optional:
    * softpipe reads the block size from TGSI_PROPERTY_CS_FIXED_BLOCK_*, and
    * with a variable size those are zero, so it runs ZERO threads and the
    * output buffer comes back exactly as it went in. That looked like a
    * kernel that did nothing, and was only visible because the buffer was
    * filled with a distinctive pattern -- had it been zeroed, a kernel named
    * InitMinMax writing zeros would have been indistinguishable from this.
    */
   cs->info.workgroup_size[0] = block[0];
   cs->info.workgroup_size[1] = block[1];
   cs->info.workgroup_size[2] = block[2];
   cs->info.workgroup_size_variable = false;

   finalize_for_gallium(cs, screen);
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      fprintf(stderr, "air_run: no context for a kernel\n");
      return 1;
   }
   printf("air_run: %s on %s (%s), as a compute kernel\n", name,
          screen->get_name(screen), how);

   /* every buffer the kernel declares; the device ones are what it writes */
   struct pipe_resource *bufs[32] = { 0 };
   /* The bytes this harness wrote, kept until the readback. The dump used to
    * recompute them from pattern_byte(), which --buffer N@OFF=HEX makes a lie:
    * a patched word looked changed by the kernel. Keeping the fill is also the
    * only way to say which words a kernel touched when it touches them past
    * the sixteen the dump prints -- and that is exactly what a prediction out
    * of tools/air/kinput.py has to be compared against.
    */
   uint8_t *before[32] = { 0 };
   unsigned sizes[32] = { 0 };
   bool writable[32] = { false };
   bool img_read[MAX_IMG] = { false }, img_write[MAX_IMG] = { false };
   unsigned num_img = 0;
   bool tex_used[MAX_IMG] = { false };
   unsigned num_tex = 0;
   unsigned num_ssbo = 0, num_written = 0;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      unsigned bytes = r->size ? r->size : 4096;
      if (r->index >= ARRAY_SIZE(bufs))
         continue;
      uint8_t *bits = calloc(1, bytes);
      bool kept = false;
      unsigned from_file = fill_buffer(r->index, bits, bytes);
      if (from_file)
         printf("air_run: buffer at index %u, %u bytes, %u of them from "
                "AIR_RUN_BUFFER\n", r->index, bytes, from_file);

      if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER) {
         /* A transform matrix filled with the byte pattern sends every sample
          * somewhere arbitrary, so what the shader computes says nothing about
          * what it is for. The identity is the one distinguished value a matrix
          * has, and the resource says it is one: air.arg_type_name is Apple's
          * own ("float4x4"), carried through from the module's metadata. Any
          * other constant buffer keeps the pattern -- this is the only place
          * the harness pretends to know what a buffer means, and it says so.
          */
         if (r->type_name && !strncmp(r->type_name, "float4x4", 8) && bytes >= 64) {
            float *m = (float *)bits;
            memset(m, 0, 64);
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            printf("air_run: constant buffer %u is a %s, so it holds the identity and "
                   "not the fill\n", r->index, r->type_name);
         }
         struct pipe_resource *b = pipe_buffer_create(screen, PIPE_BIND_CONSTANT_BUFFER,
                                                      PIPE_USAGE_DEFAULT, bytes);
         pipe_buffer_write(pipe, b, 0, bytes, bits);
         pipe->set_constant_buffer(pipe, PIPE_SHADER_COMPUTE, r->index, false,
                                   &(struct pipe_constant_buffer){
                                      .buffer = b, .buffer_size = bytes });
      } else if (r->kind == AIR_RESOURCE_DEVICE_BUFFER) {
         bufs[r->index] = pipe_buffer_create(screen, PIPE_BIND_SHADER_BUFFER,
                                             PIPE_USAGE_STAGING, bytes);
         pipe_buffer_write(pipe, bufs[r->index], 0, bytes, bits);
         sizes[r->index] = bytes;
         before[r->index] = bits;
         kept = true;
         writable[r->index] = r->write;
         num_ssbo = MAX2(num_ssbo, r->index + 1);
         num_written += r->write ? 1 : 0;
      } else if (r->kind == AIR_RESOURCE_IMAGE) {
         if (r->index >= MAX_IMG) {
            free(bits);
            fprintf(stderr, "air_run: %s binds an image at %u, past the %u this harness "
                            "keeps room for\n", name, r->index, MAX_IMG);
            return 1;
         }
         img_read[r->index] = r->read;
         img_write[r->index] = r->write;
         num_img = MAX2(num_img, r->index + 1);
      } else if (r->kind == AIR_RESOURCE_TEXTURE) {
         if (r->index >= MAX_IMG) {
            free(bits);
            fprintf(stderr, "air_run: %s binds a texture at %u, past the %u this harness "
                            "keeps room for\n", name, r->index, MAX_IMG);
            return 1;
         }
         tex_used[r->index] = true;
         num_tex = MAX2(num_tex, r->index + 1);
      }
      if (!kept)
         free(bits);
   }
   unsigned num_img_written = 0;
   for (unsigned i = 0; i < num_img; i++)
      num_img_written += img_write[i] ? 1 : 0;
   if (!num_written && !num_img_written) {
      fprintf(stderr, "air_run: %s writes no device buffer and no image, so a dispatch "
                      "would leave nothing to read back\n", name);
      return 1;
   }

   struct pipe_shader_buffer sb[32] = { 0 };
   unsigned mask = 0;
   for (unsigned i = 0; i < num_ssbo; i++) {
      sb[i].buffer = bufs[i];
      sb[i].buffer_size = sizes[i];
      if (writable[i])
         mask |= 1u << i;
   }
   pipe->set_shader_buffers(pipe, PIPE_SHADER_COMPUTE, 0, num_ssbo, sb, mask);

   /* The images, one resource each, with the format the kernel's own intrinsics
    * declare. A readable one is filled with the same texel pattern the draw
    * path uses -- texel (x,y,z) of slot n is (100+x+1000n, 200+y, 300+x+y+z,
    * 400) -- so that a kernel reading the wrong slot or the wrong texel cannot
    * come back with the right answer. A write-only one is left as the pattern
    * bytes so that a kernel which writes nothing is distinguishable from one
    * that writes zeros.
    */
   enum pipe_format ifmt[MAX_IMG];
   bool iisf[MAX_IMG];
   enum glsl_sampler_dim idim[MAX_IMG];
   bool iseen[MAX_IMG] = { false };
   unsigned nshape = 0;
   for (unsigned i = 0; i < MAX_IMG; i++) {
      ifmt[i] = PIPE_FORMAT_R32G32B32A32_FLOAT;
      iisf[i] = true;
      idim[i] = GLSL_SAMPLER_DIM_2D;
   }
   image_shapes(cs, ifmt, iisf, idim, iseen, &nshape);
   struct pipe_resource *imgs[MAX_IMG] = { 0 };
   struct pipe_image_view ivs[MAX_IMG] = { 0 };
   for (unsigned i = 0; i < num_img; i++) {
      if (idim[i] != GLSL_SAMPLER_DIM_2D && idim[i] != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s uses an image of a kind this harness does not bind "
                         "(dim %u at %u)\n", name, idim[i], i);
         return 1;
      }
      unsigned depth = idim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      struct pipe_resource itempl = {
         .target = idim[i] == GLSL_SAMPLER_DIM_3D ? PIPE_TEXTURE_3D : PIPE_TEXTURE_2D,
         .format = ifmt[i],
         .width0 = TEX_SIZE, .height0 = TEX_SIZE, .depth0 = depth, .array_size = 1,
         .bind = PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW,
      };
      imgs[i] = screen->resource_create(screen, &itempl);
      if (!imgs[i]) {
         fprintf(stderr, "air_run: the driver would not make image %u (%s)\n", i,
                 util_format_short_name(ifmt[i]));
         return 1;
      }
      float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX_SIZE; y++)
            for (unsigned x = 0; x < TEX_SIZE; x++) {
               unsigned k = ((z * TEX_SIZE + y) * TEX_SIZE + x) * 4;
               texel_value(i, x, y, z, &texels[k]);
               for (unsigned c = 0; c < 4; c++)
                  itexels[k + c] = (int32_t)texels[k + c];
            }
      struct pipe_box box = { .width = TEX_SIZE, .height = TEX_SIZE, .depth = depth };
      pipe->texture_subdata(pipe, imgs[i], 0, PIPE_MAP_WRITE, &box,
                            iisf[i] ? (const void *)texels : (const void *)itexels,
                            TEX_SIZE * 4 * sizeof(float),
                            TEX_SIZE * TEX_SIZE * 4 * sizeof(float));
      ivs[i] = (struct pipe_image_view){
         .resource = imgs[i],
         .format = ifmt[i],
         .access = (img_read[i] ? PIPE_IMAGE_ACCESS_READ : 0) |
                   (img_write[i] ? PIPE_IMAGE_ACCESS_WRITE : 0),
         .shader_access = (img_read[i] ? PIPE_IMAGE_ACCESS_READ : 0) |
                          (img_write[i] ? PIPE_IMAGE_ACCESS_WRITE : 0),
         .u.tex.level = 0, .u.tex.first_layer = 0,
         .u.tex.last_layer = depth - 1,
      };
   }
   if (num_img)
      pipe->set_shader_images(pipe, PIPE_SHADER_COMPUTE, 0, num_img, 0, ivs);

   /* and the read-only textures, as sampler views with the same texel pattern */
   enum pipe_format tfmt[MAX_IMG];
   bool tisf[MAX_IMG];
   enum glsl_sampler_dim tdim[MAX_IMG];
   bool tseen[MAX_IMG] = { false };
   unsigned ntshape = 0;
   for (unsigned i = 0; i < MAX_IMG; i++) {
      tfmt[i] = PIPE_FORMAT_R32G32B32A32_FLOAT;
      tisf[i] = true;
      tdim[i] = GLSL_SAMPLER_DIM_2D;
   }
   texture_shapes(cs, tfmt, tisf, tdim, tseen, &ntshape);
   struct pipe_resource *texes[MAX_IMG] = { 0 };
   struct pipe_sampler_view *views[MAX_IMG] = { 0 };
   void *csampler = NULL;
   for (unsigned i = 0; i < num_tex; i++) {
      if (!tex_used[i])
         continue;
      if (tdim[i] != GLSL_SAMPLER_DIM_2D && tdim[i] != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s fetches a texture of a kind this harness does not "
                         "bind (dim %u at %u)\n", name, tdim[i], i);
         return 1;
      }
      unsigned depth = tdim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      struct pipe_resource ttempl = {
         .target = tdim[i] == GLSL_SAMPLER_DIM_3D ? PIPE_TEXTURE_3D : PIPE_TEXTURE_2D,
         .format = tfmt[i],
         .width0 = TEX_SIZE, .height0 = TEX_SIZE, .depth0 = depth, .array_size = 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      texes[i] = screen->resource_create(screen, &ttempl);
      if (!texes[i]) {
         fprintf(stderr, "air_run: the driver would not make texture %u (%s)\n", i,
                 util_format_short_name(tfmt[i]));
         return 1;
      }
      float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX_SIZE; y++)
            for (unsigned x = 0; x < TEX_SIZE; x++) {
               unsigned k = ((z * TEX_SIZE + y) * TEX_SIZE + x) * 4;
               texel_value(i, x, y, z, &texels[k]);
               for (unsigned c = 0; c < 4; c++)
                  itexels[k + c] = (int32_t)texels[k + c];
            }
      struct pipe_box tbox = { .width = TEX_SIZE, .height = TEX_SIZE, .depth = depth };
      pipe->texture_subdata(pipe, texes[i], 0, PIPE_MAP_WRITE, &tbox,
                            tisf[i] ? (const void *)texels : (const void *)itexels,
                            TEX_SIZE * 4 * sizeof(float),
                            TEX_SIZE * TEX_SIZE * 4 * sizeof(float));
      struct pipe_sampler_view vtempl = { 0 };
      u_sampler_view_default_template(&vtempl, texes[i], texes[i]->format);
      views[i] = pipe->create_sampler_view(pipe, texes[i], &vtempl);
   }
   if (num_tex) {
      /* one entry per texture, not the address of one pointer. bind_sampler_states
       * reads num_tex pointers from what it is given, so passing &csampler made
       * the driver take whatever followed that single variable on the stack and
       * store it as samplers[1..]; fd6_texture_state then dereferenced those for
       * ->seqno and died at a wild address. It only showed with two or more
       * textures, which is why 22 kernels bound images here without trouble and
       * four did not. The draw path below always did this correctly.
       *
       * One state per slot, and not one shared: a slot's sampler is whatever
       * that slot declares, and a shader may mix its own with the API's.
       */
      void *csamplers[MAX_IMG] = { 0 };
      bool compare_slot[MAX_IMG] = { 0 };
      compare_sampled_slots(cs, compare_slot, MAX_IMG);
      if (compare_without_a_function(info, compare_slot, MAX_IMG, name))
         return 1;
      for (unsigned i = 0; i < num_tex && i < MAX_IMG; i++) {
         const struct air_sampler_state *own = NULL;
         struct pipe_sampler_state st = sampler_for_slot(info, i, true,
                                                         compare_slot[i], &own);
         csamplers[i] = pipe->create_sampler_state(pipe, &st);
         csampler = csamplers[i];
         if (own)
            print_own_sampler(i, own, compare_slot[i]);
      }
      pipe->bind_sampler_states(pipe, PIPE_SHADER_COMPUTE, 0, num_tex, csamplers);
      pipe->set_sampler_views(pipe, PIPE_SHADER_COMPUTE, 0, num_tex, 0, false, views);
   }

   void *cso = pipe->create_compute_state(pipe, &(struct pipe_compute_state){
      .ir_type = PIPE_SHADER_IR_NIR, .prog = cs,
      .static_shared_mem = cs->info.shared_size });
   if (!cso) {
      fprintf(stderr, "air_run: the driver would not make a compute state of it\n");
      return 1;
   }
   pipe->bind_compute_state(pipe, cso);

   /* more than one thread, and the whole grid, so that a wrong index shows */
   struct pipe_grid_info grid = {
      .work_dim = 3,
      .block = { block[0], block[1], block[2] },
      .grid = { 1, 1, 1 },
   };
   printf("air_run: dispatching %ux%ux%u threads in one workgroup; %u device buffer%s bound, "
          "%u of them written, filled with %s\n",
          block[0], block[1], block[2], num_ssbo, num_ssbo == 1 ? "" : "s", num_written,
          zero_fill ? "ZEROS (--zero: a degenerate input, see docs/air.md section 5)"
                    : ones_fill ? "0xff BYTES (--ones: a degenerate input)"
                    : library_fill ? "the floats ((w * 13) % 29 - 14) / 4"
                                   : "(i * 7 + 13) & 0xff");
   if (num_img || num_tex)
      printf("air_run: %u image%s bound (%u written) and %u texture%s, %ux%u, texel (x,y,z) of "
             "slot n = (100+x+1000n, 200+y, 300+x+y+z, 400)\n", num_img,
             num_img == 1 ? "" : "s", num_img_written, num_tex, num_tex == 1 ? "" : "s",
             TEX_SIZE, TEX_SIZE);
   pipe->launch_grid(pipe, &grid);
   bool finished = finish_or_say_why(screen, pipe, "dispatch");

   for (unsigned i = 0; i < num_ssbo; i++) {
      if (!writable[i])
         continue;
      struct pipe_transfer *xfer;
      uint32_t *p = pipe_buffer_map(pipe, bufs[i], PIPE_MAP_READ, &xfer);
      if (!p) {
         fprintf(stderr, "air_run: device buffer %u could not be read back\n", i);
         return 1;
      }
      unsigned words = MIN2(sizes[i] / 4, block[0] * block[1] * block[2]);
      printf("air_run: device buffer %u after the dispatch, first %u words:\n", i, words);
      for (unsigned w = 0; w < words; w++)
         printf("  [%2u] %11u  0x%08x\n", w, p[w], p[w]);

      /* A buffer the shader writes that comes back bit-identical to the fill
       * is not a result, and the only thing worse than no number is a number
       * that looks like one.
       *
       * This is not hypothetical and not rare. On the phone, a boot survives
       * about two dozen dispatches that bind textures or images; after that
       * every dispatch still "succeeds", every readback still prints, and
       * nothing the GPU writes is ever visible again -- measured on
       * 20 October (logs/182513-run.log: writes land for the 13th, 15th and
       * 23rd kernel of the run and for nothing after it, while the same
       * kernels and four probe kernels all write correctly in a short run,
       * logs/182805-run.log). The same boot later refuses an allocation
       * outright with "bo_allocate: alloc failed". So the words below can be
       * a statement about how long the run had been going rather than about
       * the shader, and whoever reads them has to be told which.
       */
      /* and then every word of the whole buffer that differs from what went in,
       * capped so that a kernel which rewrites all 1024 of them does not push
       * the rest of a run out of the boot log (the loader keeps the first
       * 256 KB and the last 1 MB and drops the middle).
       */
      unsigned changed = 0, shown = 0;
      const uint32_t *was = (const uint32_t *)before[i];
      for (unsigned w = 0; was && w < sizes[i] / 4; w++) {
         if (p[w] == was[w])
            continue;
         changed++;
         if (shown < 24) {
            printf("  changed [%4u] 0x%08x -> 0x%08x\n", w, was[w], p[w]);
            shown++;
         }
      }
      if (was && changed)
         printf("air_run: device buffer %u: %u of its %u words differ from what this "
                "harness wrote%s\n", i, changed, sizes[i] / 4,
                shown < changed ? " (the first 24 are above)" : "");
      else if (was)
         printf("air_run: device buffer %u came back bit-identical to the fill, so this "
                "dispatch wrote nothing visible. Either the shader writes elsewhere, or "
                "the GPU has stopped making writes visible -- which it does after a few "
                "dozen dispatches in one boot (docs/air.md section 4).\n", i);
      pipe_buffer_unmap(pipe, xfer);
   }

   if (!finished) {
      fprintf(stderr, "air_run: %s: the dispatch did not finish, so no number here is "
                      "evidence\n", name);
      return 1;
   }

   /* and every image the kernel wrote: without this a dispatch that writes only
    * an image -- which is most of the corpus's kernels -- would have nothing to
    * read back, and air_run would have reported a successful run of something
    * it had not looked at.
    */
   for (unsigned i = 0; i < num_img; i++) {
      if (!img_write[i])
         continue;
      struct pipe_transfer *xfer;
      unsigned depth = idim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      void *pix = pipe->texture_map(pipe, imgs[i], 0, PIPE_MAP_READ,
                                    &(struct pipe_box){ .width = TEX_SIZE,
                                                        .height = TEX_SIZE,
                                                        .depth = depth }, &xfer);
      if (!pix) {
         fprintf(stderr, "air_run: image %u could not be read back\n", i);
         return 1;
      }
      printf("air_run: image %u after the dispatch, texels (0,0) and (1,1):\n", i);
      for (unsigned t = 0; t < 2; t++) {
         unsigned x = t, y = t;
         for (unsigned c = 0; c < 4; c++) {
            unsigned k = (y * (xfer->stride / (4 * sizeof(float))) + x) * 4 + c;
            if (iisf[i])
               printf("  (%u,%u)[%u] %.9g\n", x, y, c, (double)((float *)pix)[k]);
            else
               printf("  (%u,%u)[%u] %11d\n", x, y, c, ((int32_t *)pix)[k]);
         }
      }
      pipe->texture_unmap(pipe, xfer);
   }

   pipe->destroy(pipe);
   screen->destroy(screen);
   return 0;
}

/* -- function constant values ---------------------------------------------
 *
 * --fc INDEX=VALUE supplies one function constant, and it exists because
 * nothing in this tree ever supplied one. air_module_function_constants()
 * registers every constant a module declares with its index and leaves
 * air_constant_value.bits at zero, so air.is_function_constant_defined folds
 * to true and every load yields zero -- for a constant whose global is an
 * `externally_initialized ... undef` in section "air.fc_initializer" there is
 * no default to fall back on, and zero is then not a default but a value this
 * compiler invented. 34 of RenderBox's 47 modules declare such a constant and
 * 25 of them derive their resource interface and their control flow from it in
 * an "air.static_init" constructor, so zero picks one arbitrary specialization
 * out of a state machine and the picture cannot say which.
 *
 * An index the module does not declare is refused rather than ignored: an
 * option that silently sets nothing would make every run agree with every
 * other one for the wrong reason.
 */
/* 64 and not 32, which is the number the corpus asks for rather than a round
 * one: fixed_frag_lph_cph and fixed_frag_lph_cpf declare 58 function constants
 * each, so the old array could not carry one pipeline's worth of them and the
 * refusal was "more than 32 function constants" -- on the module this matters
 * for most. The same bound is RW_MAX_CONSTANTS in air_metal.h, raised to 64
 * for the same reason.
 */
static struct {
   unsigned index;
   uint64_t bits;
} fc_set[64];
static unsigned num_fc_set;

static bool
apply_fc_values(struct air_constant_value *fc, unsigned num_fc)
{
   for (unsigned i = 0; i < num_fc_set; i++) {
      unsigned j;
      for (j = 0; j < num_fc; j++) {
         if (fc[j].index == fc_set[i].index) {
            fc[j].bits = fc_set[i].bits;
            fc[j].supplied = true;
            printf("air_run: function constant %u = 0x%llx\n", fc_set[i].index,
                   (unsigned long long)fc_set[i].bits);
            break;
         }
      }
      if (j == num_fc) {
         fprintf(stderr, "air_run: this module declares no function constant %u"
                         " (it declares %u)\n", fc_set[i].index, num_fc);
         return false;
      }
   }
   return true;
}

int
main(int argc, char **argv)
{
   /* --disasm: ask ir3 to print the Adreno instructions it generates. The
    * screen's name proves fd_screen_create won; it does not prove that the
    * shader itself went through ir3 rather than some later software path, and
    * the four floats look identical either way. ir3's own disassembly in the
    * log is the positive proof. IR3_SHADER_DEBUG is read once, at compiler
    * creation, so it has to be set before the screen is made.
    */
   bool want_disasm = false;
   /* --library runs an air.visible library function through the generated
    * wrapper (nir_air.h): there is nothing to dispatch otherwise, so these 135
    * modules were never run at all.
    */
   bool want_library = false;
   /* --buffer FILE is AIR_RUN_BUFFER as an argument, and it exists because the
    * device has no shell to set one with. iOS 16.5's own root carries no sh at
    * all -- /bin holds df and ps -- and the job list that runs this is read by
    * our init, which splits a line on blanks and has no notion of a variable
    * (userland/src/init.c, run_command). So a generated suite whose every
    * module reads its own bytes could not be run on the phone until this
    * option existed: with it each module is one ordinary job line.
    */
   while (argc > 1 && argv[1][0] == '-') {
      if (!strcmp(argv[1], "--disasm")) {
         want_disasm = true;
         setenv("IR3_SHADER_DEBUG", "disasm", 1);
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--zero")) {
         zero_fill = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--ones")) {
         ones_fill = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--packed16") && argc > 3) {
         packed16_fmt = packed16_by_name(argv[2], &packed16_name);
         if (packed16_fmt == PIPE_FORMAT_NONE)
            return 2;
         packed16_word = (unsigned)strtoul(argv[3], NULL, 0) & 0xffff;
         argv += 3;
         argc -= 3;
      } else if (!strcmp(argv[1], "--clear") && argc > 2) {
         /* R,G,B,A for target 0, then ':' and four more for target 1, and so on.
          * A colon and not a semicolon: these reach the phone as job lines, and
          * the init that reads them splits on blanks and promises nothing about
          * shell syntax -- a separator that a shell WOULD have meant something by
          * is one nobody can be sure about by reading the line.
          */
         const char *spec = argv[2];
         clear_count = 0;
         while (spec && *spec && clear_count < MAX_CLEARS) {
            float c[4] = { 0, 0, 0, 0 };
            if (sscanf(spec, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) != 4) {
               fprintf(stderr, "air_run: --clear wants R,G,B,A per target, ':' between "
                               "targets\n");
               return 2;
            }
            for (unsigned i = 0; i < 4; i++)
               clear_colour[clear_count].f[i] = c[i];
            clear_count++;
            const char *sep = strchr(spec, ':');
            spec = sep ? sep + 1 : NULL;
         }
         clear_given = true;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--halftex")) {
         halftex = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--miplevels") && argc > 2) {
         miplevel_fmt = packed16_by_name(argv[2], &packed16_name);
         if (miplevel_fmt == PIPE_FORMAT_NONE)
            return 2;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--packed16-rt") && argc > 2) {
         packed16_rt = packed16_by_name(argv[2], &packed16_name);
         if (packed16_rt == PIPE_FORMAT_NONE)
            return 2;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--library")) {
         want_library = library_fill = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--stitch") && argc > 2) {
         /* A graph of air.visible functions, dispatched as one kernel. The
          * argument buffer is filled the wrapper's way for the same reason the
          * stitched entry point reads the wrapper's slots: a one-node graph
          * then has to write exactly what --library writes for that function.
          */
         stitch_text = argv[2];
         library_fill = true;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--block") && argc > 2) {
         unsigned x = 0, y = 0, z = 0;

         if (sscanf(argv[2], "%u,%u,%u", &x, &y, &z) != 3 || !x || !y || !z) {
            fprintf(stderr, "air_run: --block wants X,Y,Z and all three above zero\n");
            return 2;
         }
         block[0] = x, block[1] = y, block[2] = z;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--fc") && argc > 2) {
         unsigned idx = 0;
         uint64_t val = 0;

         if (sscanf(argv[2], "%u=0x%llx", &idx, (unsigned long long *)&val) != 2 &&
             sscanf(argv[2], "%u=%llu", &idx, (unsigned long long *)&val) != 2) {
            fprintf(stderr, "air_run: --fc wants INDEX=VALUE, decimal or 0x\n");
            return 2;
         }
         if (num_fc_set >= ARRAY_SIZE(fc_set)) {
            fprintf(stderr, "air_run: more than %u function constants\n",
                    (unsigned)ARRAY_SIZE(fc_set));
            return 2;
         }
         fc_set[num_fc_set].index = idx;
         fc_set[num_fc_set].bits = val;
         num_fc_set++;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--buffer") && argc > 2) {
         if (!add_buffer_patch(argv[2]))
            setenv("AIR_RUN_BUFFER", argv[2], 1);
         argv += 2;
         argc -= 2;
      } else {
         break;
      }
   }
   if (argc < 2) {
      fprintf(stderr, "usage: air_run [--disasm] [--library] [--block X,Y,Z] "
                      "[--buffer FILE | --buffer N@OFF=HEX] "
                      "[--fc INDEX=VALUE] "
                      "[--zero | --ones] FILE[.metallib] [FUNCTION]\n");
      return 2;
   }

   watch_for_faults();
   glsl_type_singleton_init_or_ref();
   void *ctx = ralloc_context(NULL);

   /* the module */
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   size_t size = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *data = ralloc_size(ctx, size);
   if (fread(data, 1, size, f) != size) {
      fprintf(stderr, "air_run: short read of %s\n", argv[1]);
      return 1;
   }
   fclose(f);

   const void *bitcode = data;
   size_t bitcode_size = size;
   const char *want = argc > 2 ? argv[2] : NULL;
   const char *name = argv[1];

   /* A stitched graph is still handed to air_to_nir as one module plus the
    * graph, and that module is the OUTPUT node's: its own visible function is
    * one node like any other.
    */
   const struct air_stitch_graph *graph = NULL;
   struct air_stitch_function *stitch_fns = NULL;
   unsigned num_stitch_fns = 0;
   if (stitch_text) {
      char why[256] = { 0 };
      graph = air_stitch_parse(ctx, stitch_text, why, sizeof(why));
      if (graph)
         num_stitch_fns = air_stitch_functions_of(ctx, graph, data, size, &stitch_fns,
                                                  why, sizeof(why));
      if (!graph || !num_stitch_fns) {
         fprintf(stderr, "air_run: --stitch: %s\n", why[0] ? why : "no functions");
         return 2;
      }
      want = graph->nodes[graph->output_node].function;
   }

   if (size >= 4 && !memcmp(data, "MTLB", 4)) {
      struct air_metallib lib;
      const char *err;
      if (!air_metallib_parse(ctx, data, size, &lib, &err)) {
         fprintf(stderr, "air_run: %s: %s\n", argv[1], err);
         return 1;
      }
      unsigned pick = lib.num_functions;
      for (unsigned i = 0; i < lib.num_functions; i++) {
         if (!want || !strcmp(want, lib.functions[i].name)) {
            pick = i;
            break;
         }
      }
      if (pick == lib.num_functions) {
         fprintf(stderr, "air_run: no function %s\n", want ? want : "(any)");
         return 1;
      }
      bitcode = lib.functions[pick].bitcode;
      bitcode_size = lib.functions[pick].bitcode_size;
      name = lib.functions[pick].name;
   }

   /* The screen: the GPU's where the kernel has one to drive, softpipe
    * otherwise. The same source therefore runs on this machine (no msm
    * device, so it falls back and draws in software) and on the phone, where
    * it opens the Adreno exactly as the port's own Mesa does
    * (osmesa_create_gpu_screen in tools/mesa/freedreno-darwin.patch).
    * AIR_RUN_SOFTWARE=1 forces software even where a GPU is present.
    */
   struct pipe_screen *screen = NULL;
   const char *how = "software";
#ifdef GALLIUM_FREEDRENO
   if (!debug_get_bool_option("AIR_RUN_SOFTWARE", false)) {
      int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
      if (fd >= 0) {
         /* The driver option cache has to be real, not a zeroed struct:
          * fd_screen_create passes it straight to driParseConfigFiles, which
          * dereferences it. A zeroed one died at address 0x10 on the phone --
          * the first thing this program ever did on a machine that has a GPU,
          * and invisible on any machine that has not. This is the sequence
          * osmesa_create_gpu_screen() uses in tools/mesa/freedreno-darwin.patch,
          * which is the one CoreAnimation goes through on this device daily.
          */
         static const driOptionDescription freedreno_options[] = {
#include "freedreno/driinfo_freedreno.h"
         };
         static driOptionCache info, cache;
         static struct pipe_screen_config config;

         driParseOptionInfo(&info, freedreno_options, ARRAY_SIZE(freedreno_options));
         driParseConfigFiles(&cache, &info, 0, "msm", NULL, NULL, NULL, 0, NULL, 0);
         config.options = &cache;
         config.options_info = &info;

         screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
         if (screen)
            how = "the GPU, through the kernel's msm device";
         else
            fprintf(stderr, "air_run: the msm device opened but gave no screen\n");
      }
   }
#endif
   if (!screen)
      screen = sw_screen_create(null_sw_create());
   if (!screen) {
      fprintf(stderr, "air_run: no gallium screen\n");
      return 1;
   }
   printf("air_run: %s on %s (%s)\n", name, screen->get_name(screen), how);

   const void *options =
      screen->get_compiler_options(screen, PIPE_SHADER_IR_NIR, PIPE_SHADER_FRAGMENT);
   if (!options) {
      fprintf(stderr, "air_run: this screen takes no NIR\n");
      return 1;
   }

   /* the shader */
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, bitcode, bitcode_size, &fc);
   if (!apply_fc_values(fc, num_fc))
      return 1;
   struct air_to_nir_options air_options = { .constants = fc, .num_constants = num_fc,
                                            .library_wrapper = want_library,
                                            .stitch = graph,
                                            .stitch_functions = stitch_fns,
                                            .num_stitch_functions = num_stitch_fns };
   struct air_shader_info info;
   const char *err;
   nir_shader *fs = air_to_nir(ctx, bitcode, bitcode_size, NULL, &air_options, options,
                               &info, &err);
   if (!fs) {
      fprintf(stderr, "air_run: %s: %s\n", name, err);
      return 1;
   }
   if (info.stage == MESA_SHADER_COMPUTE)
      return dispatch_kernel(screen, &info, fs, name, how);
   if (info.stage != MESA_SHADER_FRAGMENT) {
      fprintf(stderr, "air_run: %s is a %s shader; only fragment shaders are drawn here\n",
              name, gl_shader_stage_name(info.stage));
      return 1;
   }
   /* The render target's format follows the shader's own output type. A Metal
    * fragment shader returning uint4 writes integers, and reading those back
    * through a float target gives the right bits read the wrong way -- which
    * looks exactly like a translation bug and is not one.
    */
   enum pipe_format rt_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   bool rt_is_float = true;
   bool rt_is_half = false;
   /* --packed16 renders INTO the same format it sampled, which is the written half of
    * the probe: the pixel is packed back into 16 bits by the GPU and read out raw.
    */
   bool rt_is_packed16 = packed16_rt != PIPE_FORMAT_NONE;
   /* Which attachment to read back: the lowest one the shader writes, not
    * target 0. clearFragment04 writes FRAG_RESULT_DATA1 alone, and target 0
    * then holds the clear colour -- a correct result that looks exactly like
    * a shader which stored nothing.
    */
   unsigned read_rt = ~0u;
   nir_foreach_variable_with_modes(var, fs, nir_var_shader_out) {
      if (var->data.location < FRAG_RESULT_DATA0)
         continue;
      unsigned idx = var->data.location - FRAG_RESULT_DATA0;
      if (idx >= read_rt)
         continue;
      read_rt = idx;
      enum glsl_base_type base = glsl_get_base_type(glsl_without_array(var->type));
      if (base != GLSL_TYPE_FLOAT && base != GLSL_TYPE_FLOAT16 &&
          base != GLSL_TYPE_DOUBLE) {
         /* unsigned, not signed: an LLVM integer carries no signedness, so
          * both this and tools/air/cpuref.py print the bits unsigned and the
          * two can be compared
          */
         rt_format = PIPE_FORMAT_R32G32B32A32_UINT;
         rt_is_float = false;
      } else if (base == GLSL_TYPE_FLOAT16) {
         /* A half return needs a half target. Through a 32-bit one the half is
          * widened the moment it is written, and NIR then folds the conversion
          * that produced it into a 32-bit one: the generated Adreno code for
          * air.convert.f.f16.u.i32 was four cov.u32f32 and not one cov.u32f16,
          * so the shader computed f32(x) while the reference computed f16(x)
          * and the comparison meant nothing. Ten declarations of the corpus
          * return half and every one of them was being reported as agreeing on
          * the strength of a case that was never compared (docs/air.md,
          * section 4 item 4).
          *
          * The chain is still u32 -> f32 -> f16 rather than a single
          * conversion wherever NIR widens before the store, so a value needing
          * to round at both steps can differ from one direct conversion by
          * double rounding. None of the pool's values does, but a future one
          * might, and that is a property of the harness and not of the GPU.
          */
         /* As of now nothing reaches this branch: tools/air/intrincheck.py packs
          * every float-ish result into a float4, so no generated shader declares
          * a half output. It is here because the target was one of the two things
          * missing and is the half that belongs on this side; the other half needs
          * a half render target in the generator, whose air.arg_type_name the
          * corpus does not attest (docs/air.md, section 4 item 4).
          */
         rt_format = PIPE_FORMAT_R16G16B16A16_FLOAT;
         rt_is_float = true;
         rt_is_half = true;
      } else {
         rt_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
         rt_is_float = true;
      }
   }
   if (rt_is_packed16) {
      rt_format = packed16_rt;
      rt_is_float = false;
      rt_is_half = false;
   }
   if (read_rt == ~0u) {
      fprintf(stderr, "air_run: %s writes no colour attachment\n", name);
      return 1;
   }

   /* A shader may declare a render target and never store to it:
    * texturedFragment3DDepth returns {undef, depth} and its only real output
    * is FRAG_RESULT_DEPTH. Reading the colour then gives the clear value,
    * which is a number that means nothing -- worse than no number, because it
    * looks like a result. This harness binds no depth attachment, so say so.
    */
   bool stores_colour = false;
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
            if (d && d->deref_type == nir_deref_type_var &&
                d->var->data.location == (int)(FRAG_RESULT_DATA0 + read_rt))
               stores_colour = true;
         }
      }
   }
   if (!stores_colour) {
      fprintf(stderr, "air_run: %s stores nothing to a colour attachment -- its output is "
                      "depth or stencil, which this harness does not read back\n", name);
      return 1;
   }

   /* A shader that discards cannot be run by this screen, and that is
    * softpipe's limit rather than the frontend's. air.discard_fragment becomes
    * NIR's `demote` (air_intrinsics.c says at length why it is demote and not
    * terminate); nir_to_tgsi sends it to TGSI_OPCODE_DEMOTE, which the
    * interpreter does not implement -- tgsi_exec.c has a case for KILL and
    * none for DEMOTE, so exec_instruction falls through to its `assert(0)` and
    * the whole run aborts at tgsi_exec.c:5943. An abort inside a check reads
    * like a crash in the thing being checked, so it is refused here by name,
    * the way the multisample textures below are.
    *
    * ir3 has both instructions and compiles these, so what is open is the
    * VALUE a discarding shader leaves in the framebuffer, and that is a
    * question for the phone.
    */
   /*
    * On SOFTPIPE only, and that qualification is the whole point of the comment above:
    * "ir3 has both instructions and compiles these, so what is open is the VALUE a
    * discarding shader leaves in the framebuffer, and that is a question for the phone."
    * The gate was written without it and asked only whether the module discards, so on
    * the phone -- where the question was supposed to be answered -- fifteen of
    * RenderBox's forty-seven modules were refused for a limitation of a screen that was
    * not in use. `mask_color_fragment` is one of them, and it is what draws a widget.
    *
    * A refusal that names the wrong wall reads as principled when it is merely absent,
    * which is the defect this file's own log line demonstrated: it printed "this screen's
    * TGSI interpreter" over a run whose every line said "on Adreno 7c+ Gen 3".
    */
   if (fs->info.fs.uses_discard && screen->get_name &&
       !strcmp(screen->get_name(screen), "softpipe")) {
      fprintf(stderr, "air_run: %s discards fragments, and THIS screen's TGSI "
                      "interpreter has no DEMOTE case at all (tgsi_exec.c), so it is "
                      "not run rather than run against the wrong thing. On the Adreno "
                      "it runs: ir3 has the instruction\n", name);
      return 1;
   }

   /* And the rest of the family this interpreter does not have.
    *
    * `grep -c TGSI_OPCODE_VOTE_ALL tgsi_exec.c` is 0, and so are BALLOT,
    * READ_FIRST, READ_INVOC, READ_HELPER and DEMOTE: nir_to_tgsi emits all of
    * them and softpipe's exec_instruction implements none, so it falls through
    * to its own assert(0) -- an abort in the middle of a check, which reads
    * like a crash in the thing being checked rather than like a screen that
    * cannot do this. The quad swaps are worse again: nir_to_tgsi has no case
    * for them either, prints "Unknown intrinsic" and then asserts inside
    * ureg_swizzle on the register it never made.
    *
    * So a shader that asks for a cross-lane value is refused here by name. It
    * is not a frontend limit and not a hardware one -- ir3 has real
    * instructions for every one of these, and compiles them -- which is why
    * the refusal names the screen.
    */
   const char *no_lanes = NULL;
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_op op = nir_instr_as_intrinsic(instr)->intrinsic;
            switch (op) {
            case nir_intrinsic_vote_all:
            case nir_intrinsic_vote_any:
            case nir_intrinsic_vote_feq:
            case nir_intrinsic_vote_ieq:
            case nir_intrinsic_ballot:
            case nir_intrinsic_read_first_invocation:
            case nir_intrinsic_read_invocation:
            case nir_intrinsic_reduce:
            case nir_intrinsic_inclusive_scan:
            case nir_intrinsic_exclusive_scan:
            case nir_intrinsic_shuffle:
            case nir_intrinsic_shuffle_xor:
            case nir_intrinsic_shuffle_up:
            case nir_intrinsic_shuffle_down:
            case nir_intrinsic_quad_broadcast:
            case nir_intrinsic_quad_swap_horizontal:
            case nir_intrinsic_quad_swap_vertical:
            case nir_intrinsic_quad_swap_diagonal:
            case nir_intrinsic_is_helper_invocation:
               no_lanes = nir_intrinsic_infos[op].name;
               break;
            default:
               break;
            }
            if (no_lanes)
               break;
         }
         if (no_lanes)
            break;
      }
      if (no_lanes)
         break;
   }
   if (no_lanes) {
      fprintf(stderr, "air_run: %s reads another lane (NIR's %s), and this screen's "
                      "TGSI interpreter implements none of the subgroup or quad "
                      "opcodes nir_to_tgsi emits for them, so it is not run rather "
                      "than aborting inside softpipe\n", name, no_lanes);
      return 1;
   }

   finalize_for_gallium(fs, screen);

   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);

   /* One target per render target the shader writes: a shader whose outputs
    * outnumber the framebuffer's attachments draws nothing at all, which
    * looks like a translation that produced no stores.
    */
   unsigned num_rt = MAX2(info.num_render_targets, read_rt + 1);
   if (num_rt > PIPE_MAX_COLOR_BUFS) {
      fprintf(stderr, "air_run: %u render targets, more than this can bind\n", num_rt);
      return 1;
   }
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = rt_format,
      .width0 = WIDTH,
      .height0 = HEIGHT,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
   };
   struct pipe_resource *target = NULL;
   struct pipe_framebuffer_state fb = {
      .width = WIDTH, .height = HEIGHT, .nr_cbufs = num_rt,
   };
   for (unsigned i = 0; i < num_rt; i++) {
      struct pipe_resource *res = screen->resource_create(screen, &templ);
      fb.cbufs[i] = pipe->create_surface(pipe, res,
         &(struct pipe_surface){ .format = templ.format });
      if (i == read_rt)
         target = res;
   }
   pipe->set_framebuffer_state(pipe, &fb);

   pipe->set_viewport_states(pipe, 0, 1, &(struct pipe_viewport_state){
      /* The y scale is NEGATIVE, and that is Metal's rule rather than a convention picked
       * here. Metal's render-target origin is the top left and its NDC y points up, so clip
       * y = +1 belongs in memory row 0; a positive y scale puts clip y = -1 there, which is
       * OpenGL's drawable rule and what this code did until now (docs/metal.md 10.28). Mesa
       * negates y in exactly this place, for exactly this reason, when it draws to a window.
       *
       * It is landed only now because until the varying was fixed (10.32) nothing could see
       * it: every draw of the corpus either writes a constant or samples one fixed texel, and
       * over a constant an orientation is invisible. The sixteen-pixel grid of 10.32 is what
       * it is landed against -- it must turn from (100+x, 200+y, ...) into
       * (100+x, 200+(3-y), ...), the texture mirrored vertically, on both back ends.
       */
      .scale = { WIDTH / 2.0f, -(HEIGHT / 2.0f), 1.0f },
      .translate = { WIDTH / 2.0f, HEIGHT / 2.0f, 0.0f },
   });

   void *blend = pipe->create_blend_state(pipe, &(struct pipe_blend_state){
      .rt[0].colormask = PIPE_MASK_RGBA,
   });
   pipe->bind_blend_state(pipe, blend);
   void *dsa = pipe->create_depth_stencil_alpha_state(pipe,
      &(struct pipe_depth_stencil_alpha_state){ 0 });
   pipe->bind_depth_stencil_alpha_state(pipe, dsa);
   void *rast = pipe->create_rasterizer_state(pipe, &(struct pipe_rasterizer_state){
      .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
      .depth_clip_near = 1, .depth_clip_far = 1,
   });
   pipe->bind_rasterizer_state(pipe, rast);

   /* Every buffer the shader reads, filled with the pattern.
    *
    * A `constant` buffer and a `device` buffer are TWO BINDINGS and not one.
    * The frontend translates a load from the first into load_ubo and from the
    * second into load_ssbo (docs/air.md section 3), and this path used to bind
    * both with set_constant_buffer -- so a draw whose shader reads a device
    * buffer read ZEROS, and read them silently: the pixel came back, four
    * numbers wide, with nothing anywhere saying a binding had been missed. The
    * compute path above has always bound the two separately; only the draw path
    * had them confused. Found by tools/air/callptr.ll, whose last two lanes are
    * a device buffer and whose first two are not, so the picture said which
    * binding was wrong rather than that something was.
    */
   struct pipe_shader_buffer fsb[32] = { 0 };
   unsigned num_fs_ssbo = 0;
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER)
         continue;
      unsigned bytes = r->size ? r->size : 256;
      uint8_t *bits = malloc(bytes);
      unsigned from_file = fill_buffer(r->index, bits, bytes);
      bool device = r->kind == AIR_RESOURCE_DEVICE_BUFFER;
      struct pipe_resource *buf =
         pipe_buffer_create(screen,
                            device ? PIPE_BIND_SHADER_BUFFER : PIPE_BIND_CONSTANT_BUFFER,
                            PIPE_USAGE_DEFAULT, bytes);
      pipe_buffer_write(pipe, buf, 0, bytes, bits);
      if (device) {
         if (r->index < ARRAY_SIZE(fsb)) {
            fsb[r->index].buffer = buf;
            fsb[r->index].buffer_size = bytes;
            num_fs_ssbo = MAX2(num_fs_ssbo, r->index + 1);
         } else {
            fprintf(stderr, "air_run: %s binds a device buffer at %u, past the %zu this "
                            "path keeps room for\n", name, r->index, ARRAY_SIZE(fsb));
            free(bits);
            return 1;
         }
      } else {
         pipe->set_constant_buffer(pipe, PIPE_SHADER_FRAGMENT, r->index, false,
                                   &(struct pipe_constant_buffer){
                                      .buffer = buf, .buffer_size = bytes,
                                   });
      }
      if (from_file)
         printf("air_run: %s buffer at index %u, %u bytes, %u of them from "
                "AIR_RUN_BUFFER\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, bytes, from_file);
      else
         printf("air_run: %s buffer at index %u, %u bytes of (i * 7 + 13) & 0xff\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, bytes);
      free(bits);
   }
   if (num_fs_ssbo)
      pipe->set_shader_buffers(pipe, PIPE_SHADER_FRAGMENT, 0, num_fs_ssbo, fsb, 0);

   /* every sampled texture the shader declares, and a sampler for it */
   unsigned num_tex = 0;
   for (unsigned i = 0; i < info.num_resources; i++)
      if (info.resources[i].kind == AIR_RESOURCE_TEXTURE)
         num_tex = MAX2(num_tex, info.resources[i].index + 1);
   if (num_tex) {
      /* The texture's format follows what the shader samples into: a Metal
       * texture2d<uint> read out of a float texture gives numbers that are
       * neither right nor obviously wrong.
       */
      enum pipe_format tex_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      bool tex_is_float = true;
      /* and the target, which has to match too: a 3D sample from a 2D
       * texture can come back right by accident, which is not evidence
       */
      enum pipe_texture_target target = PIPE_TEXTURE_2D;
      enum glsl_sampler_dim dim = GLSL_SAMPLER_DIM_2D;
      bool is_array = false;
      /* A SHADOW SAMPLE WANTS A DEPTH TEXTURE, and binding a colour one is not a
       * harmless approximation: GL and Vulkan both leave a shadow lookup on a
       * non-depth texture undefined, and the Adreno says so by ignoring the
       * comparison. The first device run of these cases came back with the TEXEL
       * -- 101 where a comparison must answer 0 or 1, and for `always` and `never`
       * too, which no compare function and no clamp can explain -- against
       * r32g32b32a32_float bound for a Metal depth2d<float>, which is the one thing
       * in that run that was this harness's own doing (docs/air.md 3.3).
       *
       * So a slot a shadow instruction reads gets a real depth texture, and its
       * texels are the only pattern that fits: (x + TEX*y) / 16, which is sixteen
       * distinct values, every one exactly representable in binary32 and inside the
       * [0, 1] a depth format is normalised to. The colour pattern cannot be used --
       * 100 does not fit in a depth texel at all.
       */
      bool shadow = false;
      nir_foreach_function_impl(impl, fs) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_tex &&
                   nir_instr_as_tex(instr)->is_shadow)
                  shadow = true;
            }
         }
      }
      nir_foreach_function_impl(impl, fs) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_tex)
                  continue;
               nir_tex_instr *t = nir_instr_as_tex(instr);
               nir_alu_type base = nir_alu_type_get_base_type(t->dest_type);
               if (base == nir_type_int) {
                  tex_format = PIPE_FORMAT_R32G32B32A32_SINT;
                  tex_is_float = false;
               } else if (base == nir_type_uint) {
                  tex_format = PIPE_FORMAT_R32G32B32A32_UINT;
                  tex_is_float = false;
               }
               dim = t->sampler_dim;
               is_array = t->is_array;
               if (dim == GLSL_SAMPLER_DIM_3D)
                  target = PIPE_TEXTURE_3D;
               else if (dim == GLSL_SAMPLER_DIM_MS)
                  target = PIPE_TEXTURE_2D;   /* refused below */
               goto have_format;
            }
         }
      }
   have_format:;
      /* and a depth format where a comparison reads it, whatever the dest_type said:
       * a shadow sample's result is a comparison and not a texel, so the element type
       * the instruction carries says nothing about the texture's format.
       */
      if (halftex) {
         if (dim != GLSL_SAMPLER_DIM_2D || is_array) {
            fprintf(stderr, "air_run: --halftex binds a 2D table and %s samples "
                            "something else\n", name);
            return 1;
         }
         tex_format = PIPE_FORMAT_R16G16B16A16_FLOAT;
         tex_is_float = true;
      }
      if (packed16_fmt != PIPE_FORMAT_NONE || miplevel_fmt != PIPE_FORMAT_NONE) {
         if (dim != GLSL_SAMPLER_DIM_2D || is_array) {
            fprintf(stderr, "air_run: --packed16 and --miplevels sample a 2D texture "
                            "and %s is not one\n", name);
            return 1;
         }
         tex_format = packed16_fmt != PIPE_FORMAT_NONE ? packed16_fmt : miplevel_fmt;
         tex_is_float = true;
      }
      if (shadow) {
         if (dim != GLSL_SAMPLER_DIM_2D || is_array) {
            fprintf(stderr, "air_run: %s compares against a texture this harness makes no "
                            "depth version of (dim %u%s); it is not run rather than run "
                            "against a colour texture, which is what the Adreno answers "
                            "with the texel\n", name, dim, is_array ? ", array" : "");
            return 1;
         }
         tex_format = PIPE_FORMAT_Z32_FLOAT;
         tex_is_float = true;
      }
      if (dim == GLSL_SAMPLER_DIM_MS) {
         fprintf(stderr, "air_run: %s samples a multisample texture; this harness binds "
                         "none, so it is not run rather than run against the wrong thing\n",
                 name);
         return 1;
      }
      if (dim != GLSL_SAMPLER_DIM_1D && dim != GLSL_SAMPLER_DIM_2D &&
          dim != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s samples a texture of a kind this harness does not "
                         "bind (dim %u)\n", name, dim);
         return 1;
      }

      /* An ARRAY texture is a different resource and not a 2D one sampled with
       * a third coordinate, so the harness has to make one or the check would
       * be about nothing: a layer index handed to a 2D texture is ignored, and
       * every layer would answer with layer 0's texels. TEX_SIZE layers, filled
       * by the same rule as a 3D texture's slices -- texel (x, y, z) of slot n
       * is (100+x+1000n, 200+y, 300+x+y+z, 400) -- so a lowering that dropped
       * the layer, or put it in the wrong component, comes back with a number
       * from the wrong slice rather than a plausible one.
       *
       * Gallium addresses an array's layers through pipe_box.z and .depth, the
       * same fields a 3D texture's slices use, while the RESOURCE carries them
       * in array_size rather than depth0 -- which is why depth0 and the box's
       * depth part company here.
       */
      if (is_array)
         target = dim == GLSL_SAMPLER_DIM_1D ? PIPE_TEXTURE_1D_ARRAY
                                             : PIPE_TEXTURE_2D_ARRAY;
      else if (dim == GLSL_SAMPLER_DIM_1D)
         target = PIPE_TEXTURE_1D;
      unsigned height = dim == GLSL_SAMPLER_DIM_1D ? 1 : TEX_SIZE;
      unsigned slices = (target == PIPE_TEXTURE_3D || is_array) ? TEX_SIZE : 1;
      unsigned tex_z = slices > 1 ? TEX_Y : 0;

      struct pipe_resource ttempl = {
         .target = target,
         .format = tex_format,
         .width0 = TEX_SIZE, .height0 = height,
         .depth0 = is_array ? 1 : slices,
         .array_size = is_array ? slices : 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      if (halftex) {
         ttempl.width0 = HALFTEX_W;
         ttempl.height0 = 1;
      }
      if (miplevel_fmt != PIPE_FORMAT_NONE) {
         /* the shape the census found, not this harness's 4x4: a two-byte format
          * differs from a four-byte one most in how its levels are laid out, and
          * 776x24 is where the window server asks for it
          */
         ttempl.width0 = MIPTEX_W;
         ttempl.height0 = MIPTEX_H;
         ttempl.last_level = MIPTEX_LEVELS - 1;
         ttempl.bind |= PIPE_BIND_RENDER_TARGET;
      }
      struct pipe_box box = { .width = TEX_SIZE, .height = height, .depth = slices };
      struct pipe_resource *texes[PIPE_MAX_SAMPLERS] = { 0 };
      for (unsigned slot = 0; slot < num_tex && slot < PIPE_MAX_SAMPLERS; slot++) {
         float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
         int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
         texes[slot] = screen->resource_create(screen, &ttempl);
         if (halftex) {
            uint16_t words[HALFTEX_W * 4];
            for (unsigned i = 0; i < HALFTEX_W; i++)
               halftex_texel(i, &words[i * 4]);
            struct pipe_box hb = { .width = HALFTEX_W, .height = 1, .depth = 1 };
            pipe->texture_subdata(pipe, texes[slot], 0, PIPE_MAP_WRITE, &hb, words,
                                  HALFTEX_W * 4 * sizeof(uint16_t),
                                  HALFTEX_W * 4 * sizeof(uint16_t));
            printf("air_run: %ux1 r16g16b16a16_float table, texel i holding the half "
                   "BIT PATTERNS ((i<<7)&0x7bff, that|0x8000, i, 0x3c00) written as "
                   "integers -- no float-to-half conversion anywhere in this program\n",
                   HALFTEX_W);
            continue;
         }
         if (miplevel_fmt != PIPE_FORMAT_NONE) {
            /* every level filled with the binary code of its own number, packed
             * through util_format so the harness never writes bits by hand
             */
            for (unsigned lv = 0; lv < MIPTEX_LEVELS; lv++) {
               unsigned w = MAX2(1u, MIPTEX_W >> lv), h = MAX2(1u, MIPTEX_H >> lv);
               unsigned texel = util_format_get_blocksize(tex_format);
               uint8_t *rows = calloc((size_t)w * h, texel);
               float v[4];
               miplevel_texel(lv, v);
               /* ONE texel packed and then repeated. util_format_pack_rgba reads w
                * TEXELS from its source, not one -- handing it a single colour and a
                * width of 776 reads 776 texels out of a four-float array, which is
                * how the first version of this segfaulted.
                */
               uint8_t one[16] = { 0 };
               util_format_pack_rgba(tex_format, one, v, 1);
               for (size_t k = 0; k < (size_t)w * h; k++)
                  memcpy(rows + k * texel, one, texel);
               struct pipe_box lb = { .width = w, .height = h, .depth = 1 };
               pipe->texture_subdata(pipe, texes[slot], lv, PIPE_MAP_WRITE, &lb, rows,
                                     w * texel, (size_t)w * h * texel);
               free(rows);
            }
            printf("air_run: %ux%u %s pyramid, %u levels, level L filled with the "
                   "binary code of L (r=bit0 g=bit1 b=bit2 a=bit3)\n", MIPTEX_W,
                   MIPTEX_H, util_format_short_name(tex_format), MIPTEX_LEVELS);
            continue;
         }
         if (packed16_fmt != PIPE_FORMAT_NONE) {
            /* one 16-bit word in every texel, so no coordinate, filter or address
             * mode can change which channel answers
             */
            uint16_t words[TEX_SIZE * TEX_SIZE];
            for (unsigned k = 0; k < TEX_SIZE * TEX_SIZE; k++)
               words[k] = (uint16_t)packed16_word;
            pipe->texture_subdata(pipe, texes[slot], 0, PIPE_MAP_WRITE, &box, words,
                                  TEX_SIZE * sizeof(uint16_t),
                                  TEX_SIZE * TEX_SIZE * sizeof(uint16_t));
            continue;
         }
         if (shadow) {
            /* one float per texel, not four, and a depth row is a quarter as wide */
            for (unsigned y = 0; y < TEX_SIZE; y++)
               for (unsigned x = 0; x < TEX_SIZE; x++)
                  texels[y * TEX_SIZE + x] = depth_texel_value(x, y);
            pipe->texture_subdata(pipe, texes[slot], 0, PIPE_MAP_WRITE, &box, texels,
                                  TEX_SIZE * sizeof(float),
                                  TEX_SIZE * TEX_SIZE * sizeof(float));
            continue;
         }
         for (unsigned z = 0; z < slices; z++) {
            for (unsigned y = 0; y < height; y++) {
               for (unsigned x = 0; x < TEX_SIZE; x++) {
                  unsigned i = ((z * height + y) * TEX_SIZE + x) * 4;
                  texel_value(slot, x, y, z, &texels[i]);
                  for (unsigned c = 0; c < 4; c++)
                     itexels[i + c] = (int32_t)texels[i + c];
               }
            }
         }
         pipe->texture_subdata(pipe, texes[slot], 0, PIPE_MAP_WRITE, &box,
                               tex_is_float ? (const void *)texels : (const void *)itexels,
                               TEX_SIZE * 4 * sizeof(float),
                               height * TEX_SIZE * 4 * sizeof(float));
      }

      /* the API's sampler where the shader declares none, and its own where it
       * does -- per slot, since a shader may mix the two
       */

      struct pipe_sampler_view *views[PIPE_MAX_SHADER_SAMPLER_VIEWS] = { 0 };
      void *samplers[PIPE_MAX_SAMPLERS] = { 0 };
      bool compare_slot[PIPE_MAX_SAMPLERS] = { 0 };
      compare_sampled_slots(fs, compare_slot, PIPE_MAX_SAMPLERS);
      if (compare_without_a_function(&info, compare_slot, PIPE_MAX_SAMPLERS, name))
         return 1;
      for (unsigned i = 0; i < num_tex && i < PIPE_MAX_SAMPLERS; i++) {
         struct pipe_sampler_view vtempl = { 0 };
         u_sampler_view_default_template(&vtempl, texes[i], texes[i]->format);
         views[i] = pipe->create_sampler_view(pipe, texes[i], &vtempl);
         const struct air_sampler_state *own = NULL;
         struct pipe_sampler_state sst = sampler_for_slot(&info, i, false,
                                                          compare_slot[i], &own);
         samplers[i] = pipe->create_sampler_state(pipe, &sst);
         if (own)
            print_own_sampler(i, own, compare_slot[i]);
      }
      pipe->set_sampler_views(pipe, PIPE_SHADER_FRAGMENT, 0, num_tex, 0, false, views);
      pipe->bind_sampler_states(pipe, PIPE_SHADER_FRAGMENT, 0, num_tex, samplers);

      float want[4];
      texel_value(0, TEX_X, TEX_Y, tex_z, want);
      if (miplevel_fmt != PIPE_FORMAT_NONE)
         printf("air_run: %u texture%s bound: %ux%u %s, %u mip levels, level L holding "
                "the binary code of L+1 -- so the four numbers below NAME the level "
                "that answered, and 0 0 0 0 is no level at all\n", num_tex, num_tex == 1 ? "" : "s",
                MIPTEX_W, MIPTEX_H, util_format_short_name(tex_format), MIPTEX_LEVELS);
      else if (shadow)
         printf("air_run: %u DEPTH texture%s bound: %ux%u %s, texel (x,y) = (x + 4y)/16, "
                "because a shadow sample of a colour texture is undefined and the Adreno "
                "answers it with the texel\n", num_tex, num_tex == 1 ? "" : "s",
                TEX_SIZE, TEX_SIZE, util_format_short_name(tex_format));
      else
      printf("air_run: %u texture%s bound: %ux%ux%u %s%s, texel (x,y,z) of slot n = "
             "(100+x+1000n, 200+y, 300+x+y+z, 400), nearest, clamped\n",
             num_tex, num_tex == 1 ? "" : "s", TEX_SIZE, height, slices,
             util_format_short_name(tex_format),
             is_array ? " array (z is the layer)" : "");
      if (!shadow && miplevel_fmt == PIPE_FORMAT_NONE &&
          packed16_fmt == PIPE_FORMAT_NONE)
         printf("air_run: expected %.9g %.9g %.9g %.9g  (the centre of texel %u,%u,%u "
                "sampled once)\n", (double)want[0], (double)want[1], (double)want[2],
                (double)want[3], TEX_X, TEX_Y, tex_z);
      /* A comparison's result is not a texel, so no "expected" line is printed for one:
       * the first device run of these cases printed the colour pattern's texel there and
       * the GPU then answered WITH that texel, which read as agreement until the numbers
       * were looked at. The line said what the comparison reads and the device said what
       * it returned, and they were the same sentence.
       */
      for (unsigned i = 0; i < num_tex && i < PIPE_MAX_SAMPLERS; i++)
         if (compare_slot[i]) {
            printf("air_run: sampler %u COMPARES against depth texel (%u,%u) = %.9g, so "
                   "the result is a 0, a 1 or a filtered fraction of them and never a "
                   "texel\n", i, TEX_X, TEX_Y,
                   (double)depth_texel_value(TEX_X, TEX_Y));
            break;
         }
   }

   /* the quad */
   static const float verts[] = {
      -1, -1, 0, 1,   1, -1, 0, 1,   -1, 1, 0, 1,   1, 1, 0, 1,
   };
   struct pipe_resource *vbuf = pipe_buffer_create(screen, PIPE_BIND_VERTEX_BUFFER,
                                                   PIPE_USAGE_DEFAULT, sizeof(verts));
   pipe_buffer_write(pipe, vbuf, 0, sizeof(verts), verts);
   pipe->set_vertex_buffers(pipe, 1, &(struct pipe_vertex_buffer){
      .buffer.resource = vbuf, .buffer_offset = 0,
   });
   void *velem = pipe->create_vertex_elements_state(pipe, 1,
      &(struct pipe_vertex_element){
         .src_offset = 0, .src_stride = 16, .vertex_buffer_index = 0,
         .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      });
   pipe->bind_vertex_elements_state(pipe, velem);

   nir_shader *vs = quad_vertex_shader(options, info.num_varyings);
   finalize_for_gallium(vs, screen);
   void *vs_cso = pipe->create_vs_state(pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = vs,
   });
   pipe->bind_vs_state(pipe, vs_cso);
   void *fs_cso = pipe->create_fs_state(pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = fs,
   });
   pipe->bind_fs_state(pipe, fs_cso);

   /* The clear colour is the PREVIOUS CONTENT of the attachment, and that is the
    * only thing on this machine a framebuffer fetch can read: softpipe's TGSI
    * interpreter has no FBFETCH at all, so the only driver here that can answer is
    * the one on the phone, and there a fetch in a single draw reads what the clear
    * put down. --clear turns the one number a fetch could return into a number
    * CHOSEN, so a shader that returns the fetch unchanged is distinguishable from
    * one that returns zero, which is what a fetch that read nothing returns.
    */
   union pipe_color_union clear = { .ui = { 0, 0, 0, 0 } };
   if (!clear_given) {
      pipe->clear(pipe, PIPE_CLEAR_COLOR, NULL, &clear, 0.0, 0);
   } else {
      for (unsigned i = 0; i < num_rt; i++) {
         const union pipe_color_union *c =
            &clear_colour[MIN2(i, clear_count - 1)];
         pipe->clear_render_target(pipe, fb.cbufs[i], c, 0, 0, WIDTH, HEIGHT, false);
         printf("air_run: render target %u cleared to %.9g %.9g %.9g %.9g, which is "
                "what a framebuffer fetch of it in this draw will read\n", i,
                (double)c->f[0], (double)c->f[1], (double)c->f[2], (double)c->f[3]);
      }
   }

   struct pipe_draw_info draw = {
      .mode = MESA_PRIM_TRIANGLE_STRIP, .index_bounds_valid = true,
      .max_index = 3, .instance_count = 1,
   };
   struct pipe_draw_start_count_bias sc = { .start = 0, .count = 4 };
   pipe->draw_vbo(pipe, &draw, 0, NULL, &sc, 1);
   if (!finish_or_say_why(screen, pipe, "draw")) {
      fprintf(stderr, "air_run: %s: the draw did not finish, so no pixel here is "
                      "evidence\n", name);
      return 1;
   }

   /* the pixel */
   struct pipe_transfer *xfer;
   void *pix = pipe->texture_map(pipe, target, 0, PIPE_MAP_READ,
                                 &(struct pipe_box){ .width = WIDTH, .height = HEIGHT,
                                                     .depth = 1 }, &xfer);
   if (!pix) {
      fprintf(stderr, "air_run: the render target could not be read back\n");
      return 1;
   }
   /* Named again here, beside the numbers: the line at the top says which
    * screen was created, this one says which screen the pixel just came out
    * of. Neither is proof that no stage in between went software -- --disasm
    * is, by making ir3 print what it compiled.
    */
   printf("air_run: read back from %s (%s)%s\n", screen->get_name(screen), how,
          want_disasm ? ", ir3's instructions are above" : "");
   printf("air_run: the pixel at (0, 0) of render target %u came back as:\n",
          read_rt);
   if (rt_is_packed16) {
      printf("air_run: packed16-rt %s: the shader's colour came back as the word "
             "0x%04x\n", packed16_name, ((uint16_t *)pix)[0]);
      pipe->texture_unmap(pipe, xfer);
      printf("air_run: drew %ux%u into %s, %u render target%s\n", WIDTH, HEIGHT,
             util_format_short_name(rt_format), num_rt, num_rt == 1 ? "" : "s");
      pipe->destroy(pipe);
      screen->destroy(screen);
      ralloc_free(ctx);
      glsl_type_singleton_decref();
      return 0;
   }
   for (unsigned c = 0; c < 4; c++) {
      if (rt_is_half)
         printf("  %.9g\n", (double)_mesa_half_to_float(((uint16_t *)pix)[c]));
      else if (rt_is_float)
         printf("  %.9g\n", (double)((float *)pix)[c]);
      else
         printf("  %u\n", ((uint32_t *)pix)[c]);
   }
   pipe->texture_unmap(pipe, xfer);

   printf("air_run: drew %ux%u into %s, %u render target%s\n",
          WIDTH, HEIGHT, util_format_short_name(rt_format), num_rt,
          num_rt == 1 ? "" : "s");

   pipe->destroy(pipe);
   screen->destroy(screen);
   ralloc_free(ctx);
   glsl_type_singleton_decref();
   return 0;
}
