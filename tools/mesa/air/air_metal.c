/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * libredwoodair: air_run's compute path, as a library a Metal driver calls.
 *
 * The contract is air_metal.h and nothing else -- no NIR, no Gallium, no ir3 in the
 * caller's view -- because the Metal side (userland/src/redwoodmetal.m) must not grow a
 * dependency on Mesa's headers and this side must stay free to change how it compiles.
 *
 * Almost everything here is lifted from air_run.c (tools/mesa/air/air_run.c), which has
 * done all of it as a test program since the GPU first took work: the screen, the
 * translation, the compute state, the grid and the fence. The comments that say *why*
 * came with the code and are kept, because every one of them is a mistake someone
 * already made. What stayed in air_run is what makes it a harness and not a driver:
 * filling buffers with a pattern so a wrong offset shows, binding a texture to every
 * slot, printing the result, --zero/--ones, drawing a quad.
 *
 * What a driver gets that air_run does not: failure as a sentence rather than a line on
 * stdout. A library in another process's address space must not print; it also must not
 * return a code nobody can read out loud, because the caller's next move is to put the
 * reason in an NSError that an application will show a person.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC visibility push(default)   /* the contract, out of libOSMesa (tools/mesa/air-osmesa.patch) */
#include "air_metal.h"
#pragma GCC visibility pop

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/blend.h"
#include "target-helpers/inline_sw_helper.h"
#include "null/null_sw_winsys.h"
#ifdef GALLIUM_PANFROST
#include "panfrost/drm/panfrost_drm_public.h"
#include "frontend/winsys_handle.h"     /* rwa_texture_from_surface, as for freedreno below */
#include "drm-uapi/drm_fourcc.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include <xf86drm.h>
#endif
#ifdef GALLIUM_FREEDRENO
#include "freedreno/drm/freedreno_drm_public.h"
/* for rwa_texture_from_surface: a resource over memory somebody else allocated is a
 * winsys_handle, and the one modifier an IOSurface of this port's kernel can carry is
 * linear (MINISURF has no tiling and no compression at all). */
#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include <xf86drm.h>
#endif
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/os_time.h"
#include "util/u_inlines.h"
#include "util/u_gen_mipmap.h"
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include "util/u_sampler.h"
#include "util/u_debug.h"

/* Two seconds, the same wait air_run uses, and for the same reason: a dispatch nobody
 * waited for is not a result, and on this GPU one timeout wedges the core for the rest
 * of the boot, so every later dispatch would be vacuous too (docs/air.md section 5).
 */
#define RWA_FENCE_TIMEOUT_NS 2000000000ull

/* How many texture slots a dispatch fills, and the bound is not Gallium's 32: sixteen
 * is where air_run stops, by name, and that refusal is what keeps freedreno's own
 * view_seqno[16] out of reach of its textures[32] -- a fragility a peer session found
 * in fd6_texture_state and which this path must not become the one to reach
 * (docs/air.md section 5). No kernel of the corpus binds a tenth texture, let alone a
 * seventeenth.
 */
#define RWA_MAX_TEXTURES 16

struct rwa_device {
   struct pipe_screen *screen;
   struct pipe_context *pipe;

   /* NIR's options for this screen, asked once: the translator needs them to know what
    * the consumer can be handed (what it may leave for the backend to lower).
    */
   const struct nir_shader_compiler_options *nir_options;

   /* what the screen says about dispatching, read once (see rwa_device_open) */
   unsigned max_threads_per_group;
   unsigned thread_execution_width;

   /* One sampler for the slots nothing was bound to, made when a dispatch first needs
    * it. See rwa_dispatch: a kernel's read() is a texture fetch and ignores the
    * sampler, but softpipe dereferences the state of every unit a shader touches.
    */
   void *default_sampler;

   /* Zero textures for the slots a shader samples and the caller left empty, one per
    * target and per depth-ness, made when first wanted (blank_view). CoreAnimation's
    * uber shaders declare and sample up to sixteen textures behind branches its uniforms
    * keep from running, and bind four: Apple's GPU reads zeros from an empty slot, so a
    * draw refused for it is a layer missing from the screen -- the black rectangles of
    * 25 October -- where Metal would have drawn it.
    */
   struct pipe_resource *blank_res[PIPE_MAX_TEXTURE_TYPES][2];
   struct pipe_sampler_view *blank_view[PIPE_MAX_TEXTURE_TYPES][2];

   /* Whether this is the rasterizer in plain C rather than the Adreno. Kept because one
    * refusal has to name it: a software screen HAS a resource_from_handle, and it is the
    * winsys under it that imports nothing, so a failed import there would otherwise come
    * back wearing the GPU's sentence about pitch alignment -- which is not what went
    * wrong (rwa_texture_from_surface).
    */
   bool software;

   /* What this screen requires of a buffer offset, per binding, read once. Only an
    * argument buffer's member has an offset at all here (rwa_argbuf_encode_buffer), and
    * these are what it is checked against: a6xx wants 64 bytes for a UBO and 64 for an
    * SSBO, softpipe 16 and 4. Metal's own rule is looser than either, so an offset it
    * allows has to be refused by name rather than rounded -- Gallium's drivers program
    * the register with what they are given.
    */
   unsigned ubo_offset_align, ssbo_offset_align;

   /* Every buffer that holds an argument-buffer encoding, so that destroying a texture,
    * a sampler or a buffer can scrub it out of the records that name it. Short by
    * construction: a buffer joins only when something is encoded into it.
    */
   struct rwa_buffer *encoded;
};

/*
 * WHAT AN MTLArgumentEncoder LEAVES BEHIND, which is a record and not bytes.
 *
 * Metal's encoder writes an 8-byte handle per member into the destination buffer; this
 * library binds by index and the frontend has already resolved every member to one, so
 * there is nothing to write and the handles would say nothing if there were (air_metal.h,
 * docs/air.md 8.2). So a -setTexture:atIndex: lands here: one slot per Metal `[[id(n)]]`,
 * in one encoding, in the DESTINATION BUFFER -- because Metal's object model allows one
 * buffer to hold several encodings at several offsets and to reuse each across many
 * draws, so neither the encoder nor the pipeline is a place a record could live.
 *
 * `destroyed` is the one thing here that Metal does not have. An argument buffer there
 * holds raw handles and keeping the resources alive is the application's business, which
 * is what -useResource: is for; a raw pointer in a record would be a read of freed memory
 * at the next draw instead. So the three destroy calls scrub themselves out of every
 * record that names them and leave this flag, and the draw then refuses BY NAME -- the
 * same answer, one frame earlier, with the resource's own number in it.
 */
struct rwa_argbuf_slot {
   bool set;                      /* this member was encoded at all */
   bool destroyed;                /* ... and the resource it named has been destroyed */
   unsigned kind;                 /* enum rwa_argbuf_kind: which call set it */
   struct rwa_buffer *buffer;     /* RWA_ARGBUF_BUFFER, with Metal's own offset */
   size_t offset;
   struct rwa_texture *texture;   /* RWA_ARGBUF_TEXTURE */
   struct rwa_sampler *sampler;   /* RWA_ARGBUF_SAMPLER */
};

/* One encoding: everything -setArgumentBuffer:offset: named as a destination, until the
 * buffer is destroyed. The slot array grows to the highest id encoded rather than being
 * RWA_MAX_ARGBUF_MEMBERS long, because an application re-encodes a two-member set
 * thousands of times in a frame and sixty-four slots of each would be memory nothing
 * reads. The list is walked linearly: a draw looks an offset up once per set, which is
 * cheap beside the draw itself, and an application with thousands of encodings in one
 * buffer would be the thing to measure before making this a table.
 */
struct rwa_argbuf_encoding {
   struct rwa_argbuf_encoding *next;
   size_t offset;
   unsigned slots;                /* how many the array holds */
   unsigned members;              /* how many of them have been set */
   struct rwa_argbuf_slot *slot;
};

struct rwa_buffer {
   struct rwa_device *device;
   struct pipe_resource *res;
   struct pipe_transfer *xfer;
   void *map;
   size_t size;

   /* The encodings an MTLArgumentEncoder recorded into this buffer, and this buffer's
    * place in the device's list of buffers that hold any -- joined when the first member
    * is encoded, so a buffer that is only ever a buffer costs nothing.
    */
   struct rwa_argbuf_encoding *encodings;
   struct rwa_buffer *next_encoded;
   bool listed;
};

/* A texture, with the shape Metal asked for kept as it was given rather than as it was
 * translated: every sentence this file writes about a texture has to be one a Metal
 * programmer recognises, so a refusal names MTLPixelFormatRGBA32Float and not
 * R32G32B32A32_FLOAT.
 */
struct rwa_texture {
   struct rwa_device *device;
   struct pipe_resource *res;
   struct pipe_sampler_view *view;  /* made when a dispatch first samples it, then kept */
   struct rwa_texture_shape shape;
   const char *format_name;         /* the MTLPixelFormat, spelled out */
   unsigned layers;                 /* array slices, or the six faces of a cube */

   /* what MTLTextureUsage allowed, which is checked at every dispatch: a kernel that
    * writes a texture the application declared read-only is an application's mistake,
    * and Metal itself catches it -- so this library has to as well, or it is the
    * lenient one.
    */
   bool shader_read, shader_write;

   /* its render surface, made at its first draw and kept for its lifetime: the Mali
    * folds draws into one pass only for the SAME surface pointers (panfrost_get_batch
    * keys on them), and a surface made per draw gave every draw a pass of its own --
    * the whole picture loaded into the tiles and written back out, dozens of times a
    * frame */
   struct pipe_surface *rt;
};

/* A sampler is a Gallium state object and nothing more. The state it was built from is
 * not kept: a refusal about a sampler happens while it is being created, where the
 * caller's struct is still in hand.
 *
 * TWO state objects, because Metal and Gallium disagree about who decides that a
 * comparison happens. In Metal the INSTRUCTION decides -- sample() and sample_compare()
 * read the same MTLSamplerState, and the sampler only says WHICH comparison -- while in
 * Gallium the sampler decides, through pipe_sampler_state.compare_mode, and softpipe
 * compares on every sample of a sampler that has it set. One state object cannot serve
 * both, so each sampler carries the pair and a stage binds `cso_compare` into exactly
 * the slots a shadow texture instruction reads (compare_sampled_slots below). Both are
 * built at create time: a sampler state is cheap, and building the second one lazily
 * would mean keeping the caller's struct alive for it, which is the thing the comment
 * above says this does not do.
 *
 * This struct is private to this file -- air_metal.h declares `struct rwa_sampler`
 * without a body -- so growing it is not an ABI change and no driver reads it.
 */
struct rwa_sampler {
   struct rwa_device *device;
   void *cso;           /* no comparison: what a plain sample() wants */
   void *cso_compare;   /* the same state with the comparison switched on */
};

/* What a kernel does with texture slot n, from both of the frontend's answers.
 *
 * air_shader_info says whether the slot is sampled or stored and whether the kernel
 * reads or writes it; the instructions themselves say what shape and what element type
 * it expects there, and that is the half air_run reads too (image_shapes(),
 * texture_shapes()). The Metal type name in the metadata is a string, and the
 * instruction is what the GPU will execute.
 */
struct rwa_slot {
   bool sampled;                 /* AIR_RESOURCE_TEXTURE: wants a sampler view */
   bool stored;                  /* AIR_RESOURCE_IMAGE: wants an image view */
   bool read, write;
   bool shape_known;             /* an instruction with a constant binding was found */
   bool shadow;                  /* a shadow instruction reads it, so Metal's depth2d */
   nir_alu_type type;            /* its base type: float, int or uint */
   bool type_known;              /* ... and whether that type means anything: see below */
   enum glsl_sampler_dim dim;
   bool is_array;

   /* The element type Apple's own metadata declares, out of air.arg_type_name, and the
    * word it wrote it in. This is what a binding is checked against where it exists,
    * because it is the only statement of the element type that did not come through our
    * translation -- and the translation does not always carry one (declared_element).
    */
   nir_alu_type declared_type;
   const char *declared_element;   /* "half", "uint", ...; NULL when none was read */
   unsigned declared_bits;         /* 16 for half/short/ushort, 32 for the rest */
};

struct rwa_compute {
   struct rwa_device *device;
   void *mem_ctx;                 /* the translation: NIR, info, and the strings in it */
   nir_shader *nir;               /* untouched; a dispatch compiles a clone */
   struct air_shader_info info;
   struct rwa_compute_info caps;

   bool dynamic_threadgroup;      /* a threadgroup buffer sized by the dispatch */

   /* the texture slots, as above, and the states built for the samplers the shader
    * declares for itself: those are nothing for a driver to create (rwa_compute_info
    * does not count them), so this library creates them, once, and keeps them with the
    * pipeline whose shader asked for them.
    */
   struct rwa_slot slots[RWA_MAX_TEXTURES];
   void *own_sampler[RWA_MAX_TEXTURES];

   /* and which sampler slots a shadow instruction reads, so a stage binds the
    * comparing state object into those and the plain one everywhere else: struct
    * rwa_sampler says why there are two of each. Read off the NIR by slot_shapes,
    * once, with everything else that comes from the instructions.
    */
   bool compare_sampler[RWA_MAX_TEXTURES];

   /* One compiled state, not a cache. The threadgroup size is a dispatch argument in
    * Metal and a shader constant to both Gallium drivers here, so the state belongs to
    * a size; a caller that alternates two sizes recompiles each time, which is a cost
    * no caller here pays and cheaper than an eviction policy a library cannot choose.
    */
   void *cso;
   unsigned cso_block[3];
};

/* The argument-buffer recorder, whose own block is further down beside the encode calls
 * but which the three destroy functions above it have to reach: a resource destroyed
 * after it was encoded has to be scrubbed out of the records that name it.
 */
static void argbuf_scrub(struct rwa_device *device, const struct rwa_buffer *buffer,
                         const struct rwa_texture *texture,
                         const struct rwa_sampler *sampler);
static void argbuf_forget_all(struct rwa_buffer *dest);
/* and the mask of Metal buffer indices a module's sets are bound at, which both
 * pipeline-creating calls report and whose definition sits beside check_bindings */
static int argbuf_mask_of(const struct air_shader_info *info, const char *name,
                          unsigned *mask, char *why, size_t why_size);

/* PRINTFLIKE, so that a sentence whose arguments do not match it is a build error.
 * These strings are edited often -- they are the product -- and nothing else checks them.
 */
static void say(char *why, size_t why_size, const char *fmt, ...) PRINTFLIKE(3, 4);

static void
say(char *why, size_t why_size, const char *fmt, ...)
{
   if (!why || !why_size)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(why, why_size, fmt, ap);
   va_end(ap);
}

/* What the screen answers about compute, as one number. Returns false when it declines,
 * which softpipe does for most of them.
 *
 * The scratch is four words wide and not one: get_compute_param writes before it
 * returns the width, and some of these caps are a vector of three (the maximum block
 * size), so a one-word buffer would be written past by a cap nobody here asks for yet.
 */
static bool
compute_cap(struct pipe_screen *screen, enum pipe_compute_cap cap, uint64_t *out)
{
   uint64_t v[4] = { 0 };

   if (!screen->get_compute_param)
      return false;

   int bytes = screen->get_compute_param(screen, PIPE_SHADER_IR_NIR, cap, v);
   if (bytes == sizeof(uint64_t)) {
      *out = v[0];
   } else if (bytes == sizeof(uint32_t)) {
      /* memcpy and not a cast: some of these caps are 32 bits wide and some 64, and
       * reading a word of one through a pointer to the other is what the compiler is
       * allowed to assume never happens.
       */
      uint32_t narrow;
      memcpy(&narrow, v, sizeof(narrow));
      *out = narrow;
   } else {
      return false;
   }
   return true;
}

/* What a Gallium driver expects of a shader it is handed: NIR's own system values
 * lowered to the hardware's, and I/O as intrinsics rather than variables. This is
 * air_run.c's finalize_for_gallium, and the same three passes are in
 * tools/mesa/air-ir3.patch; a fourth copy would be the point at which they should
 * become one function of the frontend.
 */
static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* A driver that takes shader I/O as variables and lowers it itself: panfrost. */
static bool
screen_lowers_io(struct pipe_screen *screen)
{
   const char *name = screen->get_name(screen);
   return name && strstr(name, "Panfrost") != NULL;
}

static void
finalize_for_gallium(nir_shader *nir, struct pipe_screen *screen)
{
   /* A fragment shader's [[position]]: the frontend hands it over as a system-value
    * variable, because that is the one form both of the drivers behind this library can
    * be served from (the long version is beside air.position in air_to_nir.c and in
    * docs/metal.md 10.28). freedreno sets pipe_caps.fs_position_is_sysval and wants the
    * intrinsic; softpipe does not set it and reads (0,0,0,0) from the intrinsic, because
    * nothing fills TGSI's system values for a fragment shader. For such a screen the
    * variable becomes an input varying here -- and then needs a driver_location, which
    * nothing else would give it.
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
   /* An options struct of all-false and not NULL, which are not the same thing here and
    * the difference is a trap. nir_lower_system_values *introduces*
    * load_base_workgroup_id when it lowers SYSTEM_VALUE_WORKGROUP_ID, and the only
    * thing that removes it again is
    *
    *     if (options && !options->has_base_workgroup_id)
    *        return nir_imm_zero(...);
    *
    * so NULL reads as "the driver has base workgroup ids" -- the opposite of what is
    * wanted -- and the intrinsic survives into the backend. With softpipe that is an
    * assertion in nir_to_tgsi rather than a message, and it stopped every kernel that
    * asks for its workgroup id. Zero is also the right answer here: rwa_dispatch
    * launches its grid from the origin, as Metal's own dispatch does.
    */
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
   /*
    * nir_lower_io_use_interpolated_input_intrinsics, and WITHOUT it every varying this
    * library drew was FLAT on the Adreno while softpipe interpolated it -- the mistake
    * that looked like a vertically flipped picture for three boots (docs/metal.md 10.26,
    * 10.32). Mesa's own comment on the flag says why: without it nir_lower_io "generates
    * regular load_input intrinsics and interpolation information must be inferred from
    * the list of input nir_variables", and the two consumers infer it from different
    * places. nir_to_tgsi reads each FS input's variable (INTERP_MODE_SMOOTH, as the
    * frontend sets it); ir3's setup_input does `so->inputs[n].flat = !coord` and builds
    * `coord` only for load_interpolated_input, so a plain load_input is flat.
    *
    * Nothing caught it because the only draws whose varying VARIES are Apple's
    * texturedVertex2D/3D feeding texturedFragment*: the generated vertex stage hands
    * every vertex the same texture coordinate (RWA_TEX_* below), and over a constant
    * flat and smooth are the same number.
    */
   /* panfrost lowers I/O itself, as it is handed it by Mesa's GL state tracker:
    * bifrost_preprocess_nir runs nir_lower_viewport_transform on the vertex
    * shader's position VARIABLE before its own nir_lower_io, so I/O already
    * lowered here leaves the position untransformed -- clip space taken as
    * pixels, a quad that covers the pixels around (0, 0) and nothing else.
    * On the Mali only target 0 at pixel (0, 0) ever drew (tools/mesa/air-panfrost.patch).
    */
   if (!screen_lowers_io(screen))
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
               type_size_vec4, nir_lower_io_use_interpolated_input_intrinsics);
   /* What the shader reads and writes, in nir->info. softpipe and ir3 work it
    * out for themselves; panfrost takes inputs_read and outputs_written as they
    * come (pan_shader.c), and with them zero the vertex shader fetched no
    * position and nothing reached a pixel on the Mali (tools/mesa/air-panfrost.patch).
    */
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

/*
 * One caller at a time, whatever its thread. A Gallium context is not to be used from two threads
 * at once, and the window server does: CoreAnimation builds pipelines on dispatch's workers while
 * its render thread draws, and lets go of textures and pipelines on whichever thread it is on. Two
 * of them in the context at once ended it in panfrost_update_shader_state and jm_launch_draw,
 * opening and leaving applications above all, where most pipelines are built (24 October). Every
 * entry point holds this lock until it returns; it is recursive because some call others.
 */
#include <pthread.h>

static pthread_mutex_t rwa_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static inline void
rwa_unlock_at_return(int *held)
{
   (void)held;
   pthread_mutex_unlock(&rwa_lock);
}

/* pipelines made, and their time in AIR -> NIR and in the driver's compiler ("rwm pipelines") */
static uint64_t rwa_pipelines_made, rwa_pipelines_translate_ns, rwa_pipelines_compile_ns;

#define RWA_GUARD() \
   int rwa_held __attribute__((cleanup(rwa_unlock_at_return), unused)) = pthread_mutex_lock(&rwa_lock)

struct rwa_device *
rwa_device_open(char *why, size_t why_size)
{
   RWA_GUARD();
   struct rwa_device *device = calloc(1, sizeof(*device));
   if (!device) {
      say(why, why_size, "out of memory making a device");
      return NULL;
   }

   /* NIR's type system is a process-wide singleton with a reference count; the
    * translator builds types, so it has to be up before air_to_nir and may go down
    * again when the last device closes.
    */
   glsl_type_singleton_init_or_ref();

   /* RWA_SOFTWARE=1 asks for softpipe -- a rasterizer in plain C that needs nothing of
    * the machine but its processor. It is how this library is tested without the phone
    * (tools/mesa/air/air_metal_test.c) and it is air_run's AIR_RUN_SOFTWARE under
    * another name. A driver never sets it: on the device the only screen worth opening
    * is the Adreno's, and if there is none, saying so is more use than quietly
    * computing the answer on the processor at a thousandth of the speed.
    */
   bool software = debug_get_bool_option("RWA_SOFTWARE", false);
   device->software = software;

#ifdef GALLIUM_FREEDRENO
   if (!software) {
      int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
      if (fd < 0) {
         /* The ordinary case on this port, and not an error anybody should be
          * surprised by: without gpu=1 the kernel starts no RedwoodGPU service, so
          * there is no msm device for the shim to open (tools/mesa/redwood-drm).
          */
         say(why, why_size, "there is no GPU here: no msm device to open, which is what "
                            "a kernel booted without gpu=1 looks like");
         goto fail;
      }

      /* The driver option cache has to be real, not a zeroed struct: fd_screen_create
       * passes it straight to driParseConfigFiles, which dereferences it. A zeroed one
       * died at address 0x10 on the phone -- the first thing air_run ever did on a
       * machine that has a GPU, and invisible on any machine that has not. This is the
       * sequence osmesa_create_gpu_screen() uses in tools/mesa/freedreno-darwin.patch,
       * which is the one CoreAnimation goes through on this device daily.
       *
       * static, because fd_screen keeps the cache it was given rather than copying it.
       * One screen per process is what a Metal device amounts to; a second device would
       * share these, which is why the parse below is done once.
       */
      static const driOptionDescription freedreno_options[] = {
#include "freedreno/driinfo_freedreno.h"
      };
      static driOptionCache info, cache;
      static struct pipe_screen_config config;
      static bool options_parsed = false;

      if (!options_parsed) {
         driParseOptionInfo(&info, freedreno_options, ARRAY_SIZE(freedreno_options));
         driParseConfigFiles(&cache, &info, 0, "msm", NULL, NULL, NULL, 0, NULL, 0);
         config.options = &cache;
         config.options_info = &info;
         options_parsed = true;
      }

      device->screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
      if (!device->screen) {
         say(why, why_size, "the kernel's msm device opened but freedreno made no screen "
                            "for it, so this GPU is present and unusable");
         goto fail;
      }
   }
#elif defined(GALLIUM_PANFROST)
   /* The Infinix's Mali (Bifrost v7): the kernel's RedwoodMali service behind the
    * "panfrost" device of tools/mesa/redwood-drm, opened as osmesa_create_gpu_screen()
    * opens it in tools/mesa/panfrost-darwin.patch. Its option cache is static for the
    * reason freedreno's is above.
    */
   if (!software) {
      int fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
      if (fd < 0) {
         say(why, why_size, "there is no GPU here: no panfrost device to open, which is "
                            "what a kernel booted without mali=1 looks like");
         goto fail;
      }
      static bool options_parsed = false;
      static const driOptionDescription panfrost_options[] = {
#include "panfrost/driinfo_panfrost.h"
      };
      static driOptionCache info, cache;
      static struct pipe_screen_config config;

      if (!options_parsed) {
         driParseOptionInfo(&info, panfrost_options, ARRAY_SIZE(panfrost_options));
         driParseConfigFiles(&cache, &info, 0, "panfrost", NULL, NULL, NULL, 0, NULL, 0);
         config.options = &cache;
         config.options_info = &info;
         options_parsed = true;
      }
      device->screen = panfrost_drm_screen_create(fd, &config);
      if (!device->screen) {
         say(why, why_size, "the kernel's panfrost device opened but panfrost made no "
                            "screen for it, so this GPU is present and unusable");
         goto fail;
      }
   }
#else
   if (!software) {
      say(why, why_size, "this build of the library has no GPU driver in it, only "
                         "software rasterisation");
      goto fail;
   }
#endif

   if (!device->screen)
      device->screen = sw_screen_create(null_sw_create());
   if (!device->screen) {
      say(why, why_size, "no Gallium screen could be created at all");
      goto fail;
   }

   device->nir_options =
      device->screen->get_compiler_options(device->screen, PIPE_SHADER_IR_NIR,
                                           PIPE_SHADER_COMPUTE);
   if (!device->nir_options) {
      say(why, why_size, "%s takes no NIR, so no Metal shader can be given to it",
          device->screen->get_name(device->screen));
      goto fail;
   }

   /* The context, one per device: it is what compiles a shader, binds buffers, launches
    * a grid and hands out fences. Flags zero on purpose -- not PIPE_CONTEXT_PREFER_THREADED,
    * so there is no Gallium worker thread between this library and the driver. A map held
    * across a dispatch (rwa_buffer_map) is a direct mapping of the kernel's buffer only
    * while that is true.
    */
   device->pipe = device->screen->context_create(device->screen, NULL, 0);
   if (!device->pipe) {
      say(why, why_size, "%s gave no context, so nothing can be dispatched on it",
          device->screen->get_name(device->screen));
      goto fail;
   }

   /* The two numbers Metal requires of every pipeline and its own base class answers
    * with nothing (docs/metal.md 10.8). They are properties of the screen, asked once.
    *
    * max_threads_per_group: freedreno answers 1024, which is threadsize_base (64) times
    * max_waves (16) for the a635, and softpipe answers 1024 as well.
    *
    * thread_execution_width: freedreno answers 32 and Mesa's own comment beside that
    * number is a TODO. The a635's wave is 64 threads, doubled to 128 for a variant ir3
    * compiles double-wide -- fd6_get_compute_state_info reads exactly that, but only
    * off a compiled state object, which needs the threadgroup size a pipeline has not
    * been given yet. So what a driver reports as -threadExecutionWidth here is Mesa's
    * belief about the chip rather than the chip; it is a hint for choosing a
    * threadgroup size, and the dispatch below checks the size it is actually given
    * against the compiled variant, where the number is the hardware's.
    */
   uint64_t v;
   device->max_threads_per_group =
      compute_cap(device->screen, PIPE_COMPUTE_CAP_MAX_THREADS_PER_BLOCK, &v) ? v : 0;
   device->thread_execution_width =
      compute_cap(device->screen, PIPE_COMPUTE_CAP_SUBGROUP_SIZES, &v) ? v : 1;

   /* And what a bound buffer's offset has to be a multiple of, which an argument
    * buffer's member is the only thing here to have one. Taken from the screen and not
    * from a table of this library's: it is the driver underneath that programs the
    * register, and freedreno answers 64 for both on a6xx where softpipe answers 16 and 4.
    * A zero would mean "no constraint", so it is read as one byte rather than used as a
    * divisor.
    */
   device->ubo_offset_align = MAX2(device->screen->caps.constant_buffer_offset_alignment, 1);
   device->ssbo_offset_align = MAX2(device->screen->caps.shader_buffer_offset_alignment, 1);

   return device;

fail:
   if (device->pipe)
      device->pipe->destroy(device->pipe);
   if (device->screen)
      device->screen->destroy(device->screen);
   free(device);
   glsl_type_singleton_decref();
   return NULL;
}

void
rwa_device_close(struct rwa_device *device)
{
   RWA_GUARD();
   if (!device)
      return;
   if (device->default_sampler)
      device->pipe->delete_sampler_state(device->pipe, device->default_sampler);
   for (unsigned t = 0; t < PIPE_MAX_TEXTURE_TYPES; t++) {
      for (unsigned d = 0; d < 2; d++) {
         pipe_sampler_view_reference(&device->blank_view[t][d], NULL);
         pipe_resource_reference(&device->blank_res[t][d], NULL);
      }
   }
   device->pipe->destroy(device->pipe);
   device->screen->destroy(device->screen);
   free(device);
   glsl_type_singleton_decref();
}

const char *
rwa_device_name(struct rwa_device *device)
{
   RWA_GUARD();
   /* Mesa's own name for the chip it opened ("Adreno 7c+ Gen 3"), which is where
    * -[RedwoodMetalDevice name] gets the half it does not read out of the kernel.
    */
   const char *name = device->screen->get_name(device->screen);
   return name ? name : "unknown GPU";
}

struct rwa_buffer *
rwa_buffer_create(struct rwa_device *device, size_t size, char *why, size_t why_size)
{
   RWA_GUARD();
   if (size == 0 || size > UINT32_MAX) {
      say(why, why_size, "a buffer of %zu bytes: Gallium sizes a buffer in 32 bits and "
                         "zero is not a buffer", size);
      return NULL;
   }

   struct rwa_buffer *buffer = calloc(1, sizeof(*buffer));
   if (!buffer) {
      say(why, why_size, "out of memory making a buffer");
      return NULL;
   }
   buffer->device = device;
   buffer->size = size;

   /* All three bindings, because which one this buffer is cannot be known yet: Metal's
    * [[buffer(n)]] is one index space and the shader decides whether the parameter at n
    * is `device` (an SSBO) or `constant` (a UBO), and rwa_dispatch binds whichever the
    * module declared. A driver allocating a buffer has no shader in hand at all.
    *
    * PIPE_BIND_INDEX_BUFFER for the same reason one step further out: an application's
    * MTLBuffer becomes an index buffer at the draw that names it one
    * (-drawIndexedPrimitives:...indexBuffer:), not at the allocation, so the bind this
    * resource was created with has to allow it. Neither back end here places a buffer
    * differently for it -- freedreno's buffers are linear whatever the bind is -- but a
    * resource that does not declare what it is used for is a resource the next driver
    * is entitled to put somewhere a draw cannot read.
    *
    * PIPE_USAGE_STAGING: this is memory the processor reads and writes as much as the
    * GPU does, which is what a Metal buffer is, and it is what air_run asks for the
    * buffers it fills and reads back.
    */
   buffer->res = pipe_buffer_create(device->screen,
                                    PIPE_BIND_SHADER_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
                                    PIPE_BIND_INDEX_BUFFER,
                                    PIPE_USAGE_STAGING, size);
   if (!buffer->res) {
      say(why, why_size, "the GPU driver would not allocate %zu bytes. After a few dozen "
                         "allocations in one boot this driver starts refusing outright "
                         "(docs/air.md section 4)", size);
      free(buffer);
      return NULL;
   }

   /* Mapped once, for the buffer's whole life, because the contract has no unmap: the
    * driver holds one pointer and writes through it whenever it likes. That is cheap
    * rather than clever here -- for a linear buffer freedreno's map is the kernel's own
    * mapping of the GEM object, write combined, with no cache of this processor between
    * the program and what the GPU reads (docs/metal.md 10.11), so there is nothing to
    * flush and nothing to copy, and -[MTLBuffer didModifyRange:] has nothing to do.
    *
    * READ as well as WRITE: it is what tells the driver this mapping must be the real
    * buffer and not a write-only staging copy it would upload later.
    */
   buffer->map = pipe_buffer_map(device->pipe, buffer->res,
                                 PIPE_MAP_READ | PIPE_MAP_WRITE, &buffer->xfer);
   if (!buffer->map) {
      say(why, why_size, "%zu bytes were allocated but could not be mapped for the "
                         "processor", size);
      pipe_resource_reference(&buffer->res, NULL);
      free(buffer);
      return NULL;
   }
   return buffer;
}

void *
rwa_buffer_map(struct rwa_buffer *buffer)
{
   RWA_GUARD();
   return buffer->map;
}

size_t
rwa_buffer_size(struct rwa_buffer *buffer)
{
   RWA_GUARD();
   return buffer->size;
}

void
rwa_buffer_destroy(struct rwa_buffer *buffer)
{
   RWA_GUARD();
   if (!buffer)
      return;
   /* Out of every argument-buffer record that named it, and then its own records go:
    * see argbuf_scrub -- a pointer left in a record is memory this library would
    * dereference at the next draw, where Metal's own stale handle would at worst be
    * read by the GPU.
    */
   argbuf_scrub(buffer->device, buffer, NULL, NULL);
   argbuf_forget_all(buffer);
   pipe_buffer_unmap(buffer->device->pipe, buffer->xfer);
   pipe_resource_reference(&buffer->res, NULL);
   free(buffer);
}

/* Metal's own numbers, which the contract hands over raw (air_metal.h) and which this
 * file is the only one to have to know. Spelled out rather than compared as literals,
 * because a refusal that says "type 9" helps nobody and the names are the vocabulary
 * the caller used.
 */
enum {
   RWA_TYPE_1D = 0, RWA_TYPE_1D_ARRAY = 1,
   RWA_TYPE_2D = 2, RWA_TYPE_2D_ARRAY = 3, RWA_TYPE_2D_MULTISAMPLE = 4,
   RWA_TYPE_CUBE = 5, RWA_TYPE_CUBE_ARRAY = 6,
   RWA_TYPE_3D = 7, RWA_TYPE_2D_MULTISAMPLE_ARRAY = 8, RWA_TYPE_TEXTURE_BUFFER = 9,
};

enum {   /* MTLTextureUsage, a mask */
   RWA_USAGE_SHADER_READ = 0x1, RWA_USAGE_SHADER_WRITE = 0x2,
   RWA_USAGE_RENDER_TARGET = 0x4, RWA_USAGE_PIXEL_FORMAT_VIEW = 0x10,
};

enum {   /* MTLSamplerAddressMode -- and see sampler_state_from_air about its numbering */
   RWA_ADDRESS_CLAMP_TO_EDGE = 0, RWA_ADDRESS_MIRROR_CLAMP_TO_EDGE = 1,
   RWA_ADDRESS_REPEAT = 2, RWA_ADDRESS_MIRROR_REPEAT = 3,
   RWA_ADDRESS_CLAMP_TO_ZERO = 4, RWA_ADDRESS_CLAMP_TO_BORDER = 5,
};

enum {   /* MTLSamplerMinMagFilter, MTLSamplerMipFilter */
   RWA_FILTER_NEAREST = 0, RWA_FILTER_LINEAR = 1,
   RWA_MIP_NOT_MIPMAPPED = 0, RWA_MIP_NEAREST = 1, RWA_MIP_LINEAR = 2,
};

static const char *
texture_type_name(unsigned type)
{
   static const char *const names[] = {
      "MTLTextureType1D", "MTLTextureType1DArray", "MTLTextureType2D",
      "MTLTextureType2DArray", "MTLTextureType2DMultisample", "MTLTextureTypeCube",
      "MTLTextureTypeCubeArray", "MTLTextureType3D", "MTLTextureType2DMultisampleArray",
      "MTLTextureTypeTextureBuffer",
   };
   return type < ARRAY_SIZE(names) ? names[type] : NULL;
}

/*
 * MTLPixelFormat in Gallium's terms, and only where the two name the same bytes.
 *
 * Every entry is a format whose channels are whole bytes or whole 16- or 32-bit words,
 * so the translation is the identity on memory and there is nothing in it to get subtly
 * wrong.
 *
 * What is deliberately absent is everything packed into a 32-bit word -- RGB10A2Unorm,
 * RG11B10Float, RGB9E5, the XR formats. Those names order their channels by one
 * convention and Gallium's by another, and a mistake there does not fail: it shifts a
 * channel by two bits in a texture that still looks like a texture. docs/air.md has
 * both shapes of that already -- zero printed as clamp_to_border by one of Apple's own
 * binaries while it means clamp_to_zero (3.3), and three shaders that "passed" against
 * a 2D texture bound where they sample a 3D one (5). A declined format is one sentence;
 * an almost-right one is a wrong picture nobody reports.
 *
 * THE FOUR PACKED 16-BIT FORMATS ARE HERE, and they were the same refusal until the
 * convention on each side was read rather than assumed. The window server asks for one
 * of them -- MTLPixelFormatBGR5A1Unorm, 43 -- twice a boot, for the home screen.
 *
 * Gallium's side is mechanical: the generated unpackers in u_format_table.c say which
 * bits each name means, and on a little-endian host the FIRST channel of a packed name
 * is at the LEAST significant bits. PIPE_FORMAT_B5G6R5_UNORM reads blue out of bits
 * 0-4, green out of 5-10 and red out of 11-15; PIPE_FORMAT_B5G5R5A1_UNORM reads blue
 * 0-4, green 5-9, red 10-14 and alpha at bit 15.
 *
 * Metal's side is one bit of information: does MTLPixelFormat list a packed format's
 * COLOUR channels from the least significant end or the most? Gallium happens to carry
 * both spellings of every one of these, so the two readings are both expressible and
 * they EXCHANGE RED AND BLUE, leaving the alpha where the name puts it:
 *
 *    Metal                 colours from the LOW end   colours from the HIGH end
 *    40 B5G6R5Unorm        B5G6R5_UNORM               R5G6B5_UNORM
 *    41 A1BGR5Unorm        A1B5G5R5_UNORM             A1R5G5B5_UNORM
 *    42 ABGR4Unorm         A4B4G4R4_UNORM             A4R4G4B4_UNORM
 *    43 BGR5A1Unorm        B5G5R5A1_UNORM             R5G5B5A1_UNORM
 *
 * A THIRD reading -- the whole name from the high end, alpha included, so that 43 would
 * be A1R5G5B5_UNORM -- is the one a reader reaches for next and it is REFUTED, not
 * merely unchosen: moving the one-bit alpha from one end to the other shifts the three
 * five-bit fields by a bit, and re-rendering the measured screen through it puts
 * horizontal striping across every icon. The real screen has none.
 *
 * THE RIGHT COLUMN IS THE ONE TAKEN, and it was a boot that chose it. The left column
 * was an inference -- from Metal's packed 32-bit names, whose layouts other standards
 * fix, and from its array names -- and the inference was wrong. Apple's own compositor,
 * drawing its own home screen through these formats, is what said so: see
 * packed16_served below for the pixel and the arithmetic.
 *
 * The half that is decidable HERE is still measured, and still worth measuring, because
 * it is the half that catches a typo rather than a convention: `air_metal_test
 * --pixel-formats` writes a word with exactly one field set, draws a shader that samples
 * it, and reads which channel came back 1. 15 probes, 15 in the channel the table
 * claims. What that cannot see is which of the two columns Apple meant, because both
 * ends of it are ours -- and that is exactly the part the screen settled.
 */
struct rwa_pixel_format {
   unsigned mtl;
   const char *name;
   enum pipe_format pipe;
};

static const struct rwa_pixel_format rwa_pixel_formats[] = {
   {   1, "MTLPixelFormatA8Unorm",         PIPE_FORMAT_A8_UNORM },
   {  10, "MTLPixelFormatR8Unorm",         PIPE_FORMAT_R8_UNORM },
   {  11, "MTLPixelFormatR8Unorm_sRGB",    PIPE_FORMAT_R8_SRGB },
   {  12, "MTLPixelFormatR8Snorm",         PIPE_FORMAT_R8_SNORM },
   {  13, "MTLPixelFormatR8Uint",          PIPE_FORMAT_R8_UINT },
   {  14, "MTLPixelFormatR8Sint",          PIPE_FORMAT_R8_SINT },
   {  20, "MTLPixelFormatR16Unorm",        PIPE_FORMAT_R16_UNORM },
   {  22, "MTLPixelFormatR16Snorm",        PIPE_FORMAT_R16_SNORM },
   {  23, "MTLPixelFormatR16Uint",         PIPE_FORMAT_R16_UINT },
   {  24, "MTLPixelFormatR16Sint",         PIPE_FORMAT_R16_SINT },
   {  25, "MTLPixelFormatR16Float",        PIPE_FORMAT_R16_FLOAT },
   {  30, "MTLPixelFormatRG8Unorm",        PIPE_FORMAT_R8G8_UNORM },
   {  31, "MTLPixelFormatRG8Unorm_sRGB",   PIPE_FORMAT_R8G8_SRGB },
   {  32, "MTLPixelFormatRG8Snorm",        PIPE_FORMAT_R8G8_SNORM },
   {  33, "MTLPixelFormatRG8Uint",         PIPE_FORMAT_R8G8_UINT },
   {  34, "MTLPixelFormatRG8Sint",         PIPE_FORMAT_R8G8_SINT },

   /* The packed 16-bit four, by the reading argued above: the first channel of the
    * Metal name is at the least significant bits, which is also Gallium's rule, so the
    * two names are the same letters in the same order.
    */
   {  40, "MTLPixelFormatB5G6R5Unorm",     PIPE_FORMAT_R5G6B5_UNORM },
   {  41, "MTLPixelFormatA1BGR5Unorm",     PIPE_FORMAT_A1R5G5B5_UNORM },
   {  42, "MTLPixelFormatABGR4Unorm",      PIPE_FORMAT_A4R4G4B4_UNORM },
   {  43, "MTLPixelFormatBGR5A1Unorm",     PIPE_FORMAT_R5G5B5A1_UNORM },
   {  53, "MTLPixelFormatR32Uint",         PIPE_FORMAT_R32_UINT },
   {  54, "MTLPixelFormatR32Sint",         PIPE_FORMAT_R32_SINT },
   {  55, "MTLPixelFormatR32Float",        PIPE_FORMAT_R32_FLOAT },
   {  60, "MTLPixelFormatRG16Unorm",       PIPE_FORMAT_R16G16_UNORM },
   {  62, "MTLPixelFormatRG16Snorm",       PIPE_FORMAT_R16G16_SNORM },
   {  63, "MTLPixelFormatRG16Uint",        PIPE_FORMAT_R16G16_UINT },
   {  64, "MTLPixelFormatRG16Sint",        PIPE_FORMAT_R16G16_SINT },
   {  65, "MTLPixelFormatRG16Float",       PIPE_FORMAT_R16G16_FLOAT },
   {  70, "MTLPixelFormatRGBA8Unorm",      PIPE_FORMAT_R8G8B8A8_UNORM },
   {  71, "MTLPixelFormatRGBA8Unorm_sRGB", PIPE_FORMAT_R8G8B8A8_SRGB },
   {  72, "MTLPixelFormatRGBA8Snorm",      PIPE_FORMAT_R8G8B8A8_SNORM },
   {  73, "MTLPixelFormatRGBA8Uint",       PIPE_FORMAT_R8G8B8A8_UINT },
   {  74, "MTLPixelFormatRGBA8Sint",       PIPE_FORMAT_R8G8B8A8_SINT },
   {  80, "MTLPixelFormatBGRA8Unorm",      PIPE_FORMAT_B8G8R8A8_UNORM },
   {  81, "MTLPixelFormatBGRA8Unorm_sRGB", PIPE_FORMAT_B8G8R8A8_SRGB },
   { 103, "MTLPixelFormatRG32Uint",        PIPE_FORMAT_R32G32_UINT },
   { 104, "MTLPixelFormatRG32Sint",        PIPE_FORMAT_R32G32_SINT },
   { 105, "MTLPixelFormatRG32Float",       PIPE_FORMAT_R32G32_FLOAT },
   { 110, "MTLPixelFormatRGBA16Unorm",     PIPE_FORMAT_R16G16B16A16_UNORM },
   { 112, "MTLPixelFormatRGBA16Snorm",     PIPE_FORMAT_R16G16B16A16_SNORM },
   { 113, "MTLPixelFormatRGBA16Uint",      PIPE_FORMAT_R16G16B16A16_UINT },
   { 114, "MTLPixelFormatRGBA16Sint",      PIPE_FORMAT_R16G16B16A16_SINT },
   { 115, "MTLPixelFormatRGBA16Float",     PIPE_FORMAT_R16G16B16A16_FLOAT },
   { 123, "MTLPixelFormatRGBA32Uint",      PIPE_FORMAT_R32G32B32A32_UINT },
   { 124, "MTLPixelFormatRGBA32Sint",      PIPE_FORMAT_R32G32B32A32_SINT },
   { 125, "MTLPixelFormatRGBA32Float",     PIPE_FORMAT_R32G32B32A32_FLOAT },

   /* Depth and stencil. These are in the same table because a depth texture is a
    * texture (air_metal.h) and every caller asks for one the same way; what the format
    * changes is which binding it can have and how its texels are read, and both of
    * those are decided from the format itself below, never from a second list.
    *
    * The four are the ones iOS has and Gallium names the same bytes for. The depth
    * aspect of each is the identity on memory -- 16-bit normalised, 32-bit float -- and
    * Z32_FLOAT_S8X24_UINT is Gallium's spelling of "32-bit depth, 8-bit stencil, the
    * rest padding", which is Metal's Depth32Float_Stencil8 exactly. What this library
    * never does is pack or unpack those by hand: rwa_depth_read goes through
    * util_format's own unpackers, because the one thing worse than declining a depth
    * format is returning a depth that is nearly right.
    */
   { 250, "MTLPixelFormatDepth16Unorm",    PIPE_FORMAT_Z16_UNORM },
   { 252, "MTLPixelFormatDepth32Float",    PIPE_FORMAT_Z32_FLOAT },
   { 253, "MTLPixelFormatStencil8",        PIPE_FORMAT_S8_UINT },
   { 260, "MTLPixelFormatDepth32Float_Stencil8", PIPE_FORMAT_Z32_FLOAT_S8X24_UINT },
};

/* Whether a format carries each aspect, asked of Mesa's own description rather than of
 * the MTLPixelFormat number: one reader for "has this a depth?" means a format added to
 * the table above cannot be half-handled.
 */
static bool
format_has_depth(enum pipe_format f)
{
   return util_format_has_depth(util_format_description(f));
}

static bool
format_has_stencil(enum pipe_format f)
{
   return util_format_has_stencil(util_format_description(f));
}

static bool
format_is_depth_stencil(enum pipe_format f)
{
   return format_has_depth(f) || format_has_stencil(f);
}

/*
 * THE FOUR PACKED 16-BIT FORMATS ARE OFF BY DEFAULT, and that is not timidity: the
 * table above says which pipe_format each Metal name means under the reading taken,
 * and `air_metal_test --pixel-formats` measures that THIS LIBRARY implements that
 * reading exactly -- 15 probes, a word with one field set, a real draw, 15 in the
 * channel the reading claims. What no measurement on this machine can reach is whether
 * the reading is APPLE's. Nothing here states the bit positions: Apple's own
 * MTLPixelFormat.h carries no comment on them and Metal.framework's binary carries the
 * names and the values and no layout. The remaining support is an inference from the
 * packed 32-bit formats whose layouts other standards fix -- RGB9E5Float, RG11B10Float,
 * RGB10A2Unorm, all of which put the FIRST-NAMED channel in the LOW bits -- and an
 * inference is not a measurement.
 *
 * The experiment that settles it needs Apple's own code to display a word this library
 * wrote, which is a boot and not a test here: under the other reading red and blue are
 * exchanged, so a surface this port draws cyan would be yellow and the other way round.
 * SERVED BY DEFAULT NOW, and the order is the MIRROR of what it was. The boot that
 * served them under the first reading drew the home screen's wallpaper layer -- flat,
 * unblended, the bottom of the stack and 8 004 sampled pixels of it -- as exactly
 * (255, 12, 12) where the wallpaper is blue. That is the sign this file wrote down in
 * advance: under the other reading red and blue are exchanged. Inverting that pixel
 * through our own packing and back out through each candidate layout, the only
 * readings that make it a blue are the ones with red and blue exchanged, and of those
 * only the PURE EXCHANGE -- same field offsets, same widths -- leaves the rest of the
 * picture clean: moving the one-bit alpha as well misaligns the five-bit fields and
 * shows as visible striping across the icons, which the real screen does not have.
 * Re-rendered through the exchange, the whole page reads as iOS: a solid blue ground,
 * white labels, a blue-to-violet Weather icon with a white cloud (logs/packed.png).
 *
 * So Gallium's names and Metal's run in OPPOSITE directions over the colour channels,
 * with the alpha where the name puts it -- Metal's B5G6R5 is Gallium's R5G6B5, Metal's
 * BGR5A1 is Gallium's R5G5B5A1. All four of the first mappings were made by one rule
 * and one measurement falsified the rule, so all four move together.
 *
 * `RWA_PACKED16=0` declines them again, which is how the boot before this one ran and
 * is what to set if they ever need taking out of the picture; nothing else is a knob
 * here.
 *
 * Read once, because a texture is created per frame and getenv is not free.
 */
static bool
packed16_served(void)
{
   static int on = -1;
   if (on < 0) {
      const char *v = getenv("RWA_PACKED16");
      on = !(v && *v == '0');
   }
   return on != 0;
}

static const struct rwa_pixel_format *
pixel_format(unsigned mtl)
{
   if (mtl >= 40 && mtl <= 43 && !packed16_served())
      return NULL;
   for (unsigned i = 0; i < ARRAY_SIZE(rwa_pixel_formats); i++)
      if (rwa_pixel_formats[i].mtl == mtl)
         return &rwa_pixel_formats[i];
   return NULL;
}

/* What a format this library declines is, so the refusal can say it instead of
 * printing a number. The ranges are Metal's own enumeration, grouped the way Metal
 * groups them, and each carries the reason it is not in the table above -- the reason
 * differs per family, and "unsupported" would hide which of them it is.
 */
static const char *
pixel_format_refusal(unsigned f)
{
   if (f == 0)
      return "MTLPixelFormatInvalid, which is what a descriptor nobody filled in holds";
   if (f >= 40 && f <= 43)
      return "one of Metal's 16-bit packed colour formats (B5G6R5, A1BGR5, ABGR4, BGR5A1), "
             "which this library serves by default and RWA_PACKED16=0 has taken out of the "
             "picture. Declining them costs the home screen its icons, so this is a "
             "measurement switch and not a setting";
   if (f == 44)
      return "MTLPixelFormatBGRA4Unorm, which Metal.framework's own enumeration carries at "
             "44 and this device's MTLPixelFormat.h does not declare at all: a format no "
             "public header names is one whose layout nothing here can read";
   if (f >= 90 && f <= 94)
      return "one of Metal's formats packed into a 32-bit word (RGB10A2, RG11B10, RGB9E5, "
             "BGR10A2), whose field order and bias this library will not guess: an error of "
             "two bits in a channel is a picture, not a failure";
   if (f >= 130 && f <= 153)
      return "one of the BC compressed formats, whose texels come in blocks this library "
             "cannot make and whose rows are not rows of pixels";
   if (f >= 160 && f <= 167)
      return "one of the PVRTC compressed formats, whose texels come in blocks this library "
             "cannot make";
   if (f >= 170 && f <= 183)
      return "one of the EAC or ETC2 compressed formats, whose texels come in blocks this "
             "library cannot make";
   if (f >= 186 && f <= 236)
      return "one of the ASTC compressed formats, whose texels come in blocks this library "
             "cannot make";
   if (f == 240 || f == 241)
      return "one of Metal's packed 4:2:2 video formats (GBGR422, BGRG422), where one block "
             "holds two texels and a region in texels is not a region in bytes";
   /* The four depth and stencil formats iOS has are in the table above; these are the
    * rest of Metal's enumeration between them, and each is declined for its own reason
    * rather than for being a depth format.
    */
   if (f == 255)
      return "MTLPixelFormatDepth24Unorm_Stencil8, which Metal's own header marks "
             "API_UNAVAILABLE(ios) -- this device answers NO to "
             "-isDepth24Stencil8PixelFormatSupported, and a format the device says it "
             "does not have must not be allocatable through it";
   if (f == 261 || f == 262)
      return "one of the X*_Stencil8 formats, which is not a texture but a view of the "
             "stencil aspect of one made as Depth32Float_Stencil8: it comes from "
             "-newTextureViewWithPixelFormat:, which this library does not do, and "
             "rwa_depth_read asks for an aspect of the texture itself instead";
   if (f >= 250 && f <= 262)
      return "a depth or stencil format of Metal's enumeration that this library has no "
             "Gallium equivalent for by name";
   if (f >= 552 && f <= 555)
      return "one of Apple's extended-range XR formats, which has no Gallium equivalent at "
             "all";
   return "not a value of MTLPixelFormat this library knows: either a format newer than the "
          "iOS 16.5 SDK's or none at all";
}

/* Where an imported texture's memory comes from: an IOSurface by its ID, with the offset
 * and the row length the kernel reported about it. NULL everywhere below means "allocate",
 * which is rwa_texture_create's whole difference from rwa_texture_from_surface.
 */
struct rwa_surface_import {
   int id;
   unsigned offset, row_bytes;
};

static struct rwa_texture *
texture_of_shape(struct rwa_device *device, const struct rwa_texture_shape *shape,
                 const struct rwa_surface_import *import, char *why, size_t why_size)
{
   if (!shape) {
      say(why, why_size, "a texture with no descriptor");
      return NULL;
   }

   const struct rwa_pixel_format *fmt = pixel_format(shape->pixel_format);
   if (!fmt) {
      say(why, why_size, "pixel format %u is %s", shape->pixel_format,
          pixel_format_refusal(shape->pixel_format));
      return NULL;
   }

   /* Metal's descriptor defaults these to 1 and never to 0, so a zero is a caller's
    * struct that was filled in only as far as it cared about. Taken as one rather than
    * refused: a texture with no texels at all is still refused below, on the width,
    * which is the field no descriptor leaves out.
    */
   unsigned width = shape->width;
   unsigned height = shape->height ? shape->height : 1;
   unsigned depth = shape->depth ? shape->depth : 1;
   unsigned layers = shape->array_length ? shape->array_length : 1;
   unsigned mips = shape->mip_levels ? shape->mip_levels : 1;
   unsigned samples = shape->sample_count ? shape->sample_count : 1;

   const char *type_name = texture_type_name(shape->type);
   if (!type_name) {
      say(why, why_size, "texture type %u is not one of MTLTextureType's ten", shape->type);
      return NULL;
   }
   /* Multisample is a property of the TYPE in Metal and of the sample count in Gallium,
    * and the two have to be made to agree in both directions (below, and in
    * attachment_shape_fits for a pass). This is the Metal side of that.
    */
   bool multisample = shape->type == RWA_TYPE_2D_MULTISAMPLE ||
                      shape->type == RWA_TYPE_2D_MULTISAMPLE_ARRAY;

   /*
    * What an imported surface can be, and every one of these is a refusal rather than a
    * silent adjustment. The surface is row-major memory somebody else laid out, and all
    * freedreno is told about that layout is one pitch (fd_resource_from_handle sets
    * layout.pitch0 = handle->stride and slice 0's offset, and nothing else). So a mip
    * chain, an array, a cube or a second sample would need this side and the allocator to
    * have agreed on where the extra levels live -- and no such agreement exists. A
    * drawable is none of those things: -[CAMetalLayer nextDrawable] asks for a plain 2D
    * texture, mipmapped: NO, one slice (docs/metal.md 10.46).
    */
   if (import) {
      if (shape->type != RWA_TYPE_2D) {
         say(why, why_size, "a %s over an IOSurface: an imported surface is row-major "
                            "memory with one pitch, and only a MTLTextureType2D can be laid "
                            "over it without an agreement about where further levels live",
             type_name);
         return NULL;
      }
      if (mips != 1) {
         say(why, why_size, "%u mip levels over an IOSurface: the import is told one pitch "
                            "and nothing about smaller levels, so there is nowhere for them "
                            "to be", mips);
         return NULL;
      }
      if (layers != 1 || depth != 1) {
         say(why, why_size, "%u slices and a depth of %u over an IOSurface: the import is "
                            "one plane of one image", layers, depth);
         return NULL;
      }
      if (import->row_bytes == 0) {
         say(why, why_size, "an IOSurface with no bytes a row: the import takes the pitch "
                            "from the caller and cannot guess it");
         return NULL;
      }
   }

   enum pipe_texture_target target;
   unsigned array_size = 1;
   switch (shape->type) {
   case RWA_TYPE_1D:
   case RWA_TYPE_1D_ARRAY:
      if (height != 1 || depth != 1) {
         say(why, why_size, "a %s of %ux%ux%u: a 1D texture has one row and one slice",
             type_name, width, height, depth);
         return NULL;
      }
      target = shape->type == RWA_TYPE_1D ? PIPE_TEXTURE_1D : PIPE_TEXTURE_1D_ARRAY;
      array_size = shape->type == RWA_TYPE_1D ? 1 : layers;
      break;
   case RWA_TYPE_2D:
   case RWA_TYPE_2D_ARRAY:
      if (depth != 1) {
         say(why, why_size, "a %s with a depth of %u: a 2D texture has one slice, and an "
                            "array of them is arrayLength and not depth", type_name, depth);
         return NULL;
      }
      target = shape->type == RWA_TYPE_2D ? PIPE_TEXTURE_2D : PIPE_TEXTURE_2D_ARRAY;
      array_size = shape->type == RWA_TYPE_2D ? 1 : layers;
      break;
   case RWA_TYPE_CUBE:
   case RWA_TYPE_CUBE_ARRAY:
      if (width != height || depth != 1) {
         say(why, why_size, "a %s of %ux%ux%u: a cube's faces are square", type_name, width,
             height, depth);
         return NULL;
      }
      /* Gallium counts the six faces in array_size, so a cube array is six slices per
       * cube. Metal counts them in the type instead, which is why this is a
       * translation and not a copy.
       */
      target = shape->type == RWA_TYPE_CUBE ? PIPE_TEXTURE_CUBE : PIPE_TEXTURE_CUBE_ARRAY;
      array_size = 6 * (shape->type == RWA_TYPE_CUBE ? 1 : layers);
      break;
   case RWA_TYPE_3D:
      if (shape->array_length > 1) {
         say(why, why_size, "a %s with an arrayLength of %u: Metal has no array of 3D "
                            "textures", type_name, shape->array_length);
         return NULL;
      }
      target = PIPE_TEXTURE_3D;
      break;
   case RWA_TYPE_2D_MULTISAMPLE:
   case RWA_TYPE_2D_MULTISAMPLE_ARRAY:
      /*
       * Gallium has no multisample TARGET. A multisample texture is PIPE_TEXTURE_2D (or
       * _2D_ARRAY) with nr_samples above one, and every per-sample thing hangs off that
       * one number: freedreno multiplies the layout's bytes per texel by it
       * (fd_resource_layout_init, "layout->cpp *= fd_resource_nr_samples"), and a
       * framebuffer's sample count is read back off the attachment's resource rather
       * than taken from the framebuffer state (util_framebuffer_get_num_samples). So
       * the target is a plain 2D texture's and the sample count below is the whole of
       * the difference.
       */
      if (depth != 1) {
         say(why, why_size, "a %s with a depth of %u: a 2D texture has one slice, and an "
                            "array of them is arrayLength and not depth", type_name, depth);
         return NULL;
      }
      target = shape->type == RWA_TYPE_2D_MULTISAMPLE ? PIPE_TEXTURE_2D
                                                      : PIPE_TEXTURE_2D_ARRAY;
      array_size = shape->type == RWA_TYPE_2D_MULTISAMPLE ? 1 : layers;
      break;
   case RWA_TYPE_TEXTURE_BUFFER:
      /* The frontend refuses a texture buffer by name as well (docs/air.md section 5);
       * it is a buffer with a texture's interface, and rwa_buffer_create is where one
       * comes from.
       */
      say(why, why_size, "a %s: this is a buffer behind a texture's interface, which the "
                         "frontend refuses on the shader side too", type_name);
      return NULL;
   default:
      unreachable("texture_type_name admitted a type this switch does not");
   }

   if (width == 0) {
      say(why, why_size, "a %s that is no texels wide", type_name);
      return NULL;
   }
   /*
    * The sample count, by Metal's own rules about it -- and these are quoted rather than
    * deduced. The sentences are in Metal itself, in the __cstring of
    * /System/Library/Frameworks/Metal.framework/Metal (0x181b6915f and 0x181b690c5 in
    * this device's shared cache), not in MetalTools: they are what every device enforces
    * and not what the validation layer adds.
    *
    *   "MTLTextureDescriptor sampleCount must be > 1 for multisample textures."
    *   "MTLTextureDescriptor mipmapLevelCount must equal 1 for multisample textures."
    *
    * A one-sample multisample texture is the tempting thing to allow, because Gallium
    * would make one happily -- nr_samples 0 and 1 are the same resource -- and it would
    * let the six multisample fragment shaders of AppleMetalGLRenderer run with their
    * loop over samples executing once. It is refused because that is exactly a pass for
    * the wrong reason: with one sample the sample index can only ever be 0, so a shader
    * reading sample i and a shader ignoring i entirely print the same pixel, and the
    * descriptor is one Metal would have refused.
    */
   if (!multisample && samples != 1) {
      say(why, why_size, "a %s with a sampleCount of %u: only a multisample type has more "
                         "than one sample per texel", type_name, samples);
      return NULL;
   }
   if (multisample && samples < 2) {
      say(why, why_size, "a %s with a sampleCount of %u: Metal's own rule is "
                         "\"MTLTextureDescriptor sampleCount must be > 1 for multisample "
                         "textures\", and a texture of one sample is a MTLTextureType2D",
          type_name, shape->sample_count);
      return NULL;
   }
   if (multisample && mips != 1) {
      say(why, why_size, "a %s with a mipmapLevelCount of %u: Metal's own rule is "
                         "\"MTLTextureDescriptor mipmapLevelCount must equal 1 for "
                         "multisample textures\", and there is no filtered fetch from a "
                         "multisample texture for a smaller level to serve", type_name,
          mips);
      return NULL;
   }

   /* A mip chain no larger than the texture: with more levels than halvings the extra
    * ones are 1x1 of nothing, and a shader asking for level 7 of a 4x4 texture is a
    * shader that believes something else about the image it was given.
    */
   unsigned halvings = 1;
   for (unsigned e = MAX3(width, height, depth); e > 1; e >>= 1)
      halvings++;
   if (mips > halvings) {
      say(why, why_size, "%u mip levels of a %ux%ux%u texture, which has %u", mips, width,
          height, depth, halvings);
      return NULL;
   }

   /* MTLTextureUsage, which decides one thing here: whether the GPU is told this
    * texture can be stored to. MTLTextureUsageUnknown (zero) is Metal's "assume
    * anything", so it means both; a usage with neither read nor write in it
    * (PixelFormatView alone) still has to be allocatable and readable back, so it is
    * given the sampled binding.
    */
   unsigned known = RWA_USAGE_SHADER_READ | RWA_USAGE_SHADER_WRITE |
                    RWA_USAGE_RENDER_TARGET | RWA_USAGE_PIXEL_FORMAT_VIEW;
   if (shape->usage & ~known) {
      say(why, why_size, "usage bits 0x%x are not MTLTextureUsage's, and this library will "
                         "not guess what they allow", shape->usage & ~known);
      return NULL;
   }
   bool read = shape->usage == 0 || (shape->usage & RWA_USAGE_SHADER_READ);
   bool write = shape->usage == 0 || (shape->usage & RWA_USAGE_SHADER_WRITE);

   /* A depth or stencil format turns the same usage bits into a different binding, and
    * the difference is not a convention: Gallium has PIPE_BIND_DEPTH_STENCIL where it
    * has PIPE_BIND_RENDER_TARGET for colour, and freedreno answers for a Z format only
    * when asked about that one (a6xx/fd6_screen.cc hands the format to fd6_pipe2depth
    * under PIPE_BIND_DEPTH_STENCIL and nothing else). Asked as a render target, every
    * depth format would be declined with a sentence about the GPU that is really about
    * the question.
    *
    * MTLTextureUsageShaderWrite on one is refused rather than dropped: Metal has no
    * depth format a shader stores to and Gallium has no image binding for one, so the
    * texture could only be bound where it would not be written -- which is the shape of
    * failure this library exists to avoid. With usage 0, Metal's "assume anything", the
    * write is simply not assumed.
    */
   /* A multisample texture is the other kind a shader cannot store to, and both sides
    * say so. Metal's MSL has no writable multisample texture at all -- texture2d_ms
    * takes access::read and nothing else -- and freedreno refuses
    * PIPE_BIND_SHADER_IMAGE outright for any sample count above zero
    * (a6xx/fd6_screen.cc, "if (sample_count > 0) return false"), which is the check
    * that once held eighteen kernels on the wrong question (docs/metal.md 10.21). So an
    * explicit MTLTextureUsageShaderWrite is refused by name, and usage 0 -- Metal's
    * "assume anything" -- simply does not assume the write.
    */
   if (multisample && (shape->usage & RWA_USAGE_SHADER_WRITE)) {
      say(why, why_size, "a %s with MTLTextureUsageShaderWrite: Metal has no multisample "
                         "texture a shader stores to (texture2d_ms takes access::read and "
                         "nothing else) and neither has this GPU, which declines an image "
                         "binding for any sample count above one, so there is no binding "
                         "this could be given", type_name);
      return NULL;
   }
   if (multisample)
      write = false;

   bool depth_stencil = format_is_depth_stencil(fmt->pipe);
   if (depth_stencil && (shape->usage & RWA_USAGE_SHADER_WRITE)) {
      say(why, why_size, "a %s of %s with MTLTextureUsageShaderWrite: no shader stores to "
                         "a depth or stencil texture, in Metal or on this GPU, so there is "
                         "no binding this could be given", type_name, fmt->name);
      return NULL;
   }
   if (depth_stencil)
      write = false;

   unsigned bind = (read ? PIPE_BIND_SAMPLER_VIEW : 0) |
                   (write ? PIPE_BIND_SHADER_IMAGE : 0) |
                   ((shape->usage & RWA_USAGE_RENDER_TARGET) || (depth_stencil && !shape->usage)
                       ? (depth_stencil ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET)
                       : 0);
   if (!bind) {
      bind = PIPE_BIND_SAMPLER_VIEW;
      read = true;
   }

   /* Gallium counts a texture that is not multisampled as zero samples, not one,
    * everywhere a sample count is asked for -- and the difference is not cosmetic here.
    * freedreno's fd6_screen_is_format_supported refuses PIPE_BIND_SHADER_IMAGE outright
    * when the sample count is above zero (a6xx/fd6_screen.cc: "if (sample_count > 0)
    * return false"), so asking with one -- which is what `samples` holds for every
    * texture this far, the multisample types having been refused above -- made the
    * Adreno decline RGBA32Uint, RGBA32Float, R32*, RGBA8Unorm and every other format as
    * a texture the shader writes. That was measured on the device: five candidates, all
    * five refused with this file's own sentence, and no image kernel could be served.
    * air_run never meets it because it never asks is_format_supported and creates its
    * resources with nr_samples left at zero (air_run.c, image_shapes). So a
    * non-multisample texture is zero samples to both the query and resource_create,
    * and a multisample one is its own count to both.
    */
   unsigned query_samples = samples > 1 ? samples : 0;

   /* Asked before it is allocated, so that a format the driver does not do for this
    * binding is a sentence naming both rather than a NULL from resource_create naming
    * neither. WHICH sample counts exist is the back end's answer and not a list here:
    * freedreno's a6xx takes 1, 2 and 4 and declines 8 (a6xx/fd6_screen.cc,
    * valid_sample_count, where 8 is commented out because it moves the LRZ width),
    * while softpipe declines every count above one for every format and binding there
    * is (softpipe/sp_screen.c: "if (sample_count > 1) return false", and
    * caps->fake_sw_msaa, which asks the frontend to pretend with one sample).
    */
   if (!device->screen->is_format_supported(device->screen, fmt->pipe, target,
                                            query_samples, query_samples, bind)) {
      char ms[48] = "";
      if (query_samples)
         snprintf(ms, sizeof(ms), " of %u samples per texel", query_samples);
      say(why, why_size, "%s cannot give %s as a %s%s%s%s", rwa_device_name(device),
          fmt->name, type_name, ms,
          write ? " the shader writes" : " the shader samples",
          !(bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL)) ? ""
             : depth_stencil ? " and tests depth in" : " and renders to");
      return NULL;
   }

   struct rwa_texture *texture = calloc(1, sizeof(*texture));
   if (!texture) {
      say(why, why_size, "out of memory making a texture");
      return NULL;
   }
   texture->device = device;
   texture->shape = *shape;
   texture->format_name = fmt->name;
   texture->layers = array_size;
   texture->shader_read = read;
   texture->shader_write = write;

   struct pipe_resource templ = {
      .target = target, .format = fmt->pipe,
      .width0 = width, .height0 = height, .depth0 = depth,
      .array_size = array_size, .last_level = mips - 1, .nr_samples = query_samples,
      .bind = bind, .usage = PIPE_USAGE_DEFAULT };

   if (import) {
      /*
       * Memory somebody else allocated, by the one route this port has: the ID goes where
       * Linux puts a dmabuf file descriptor, and freedreno's import on Darwin already
       * means exactly that (freedreno_bo.c, fd_bo_from_dmabuf_drm: "what comes in here is
       * an IOSurface, and the file descriptor its ID", and the kernel answers it as
       * kRedwoodSurface). PIPE_BIND_SHARED is deliberately NOT added to the template:
       * fd_resource_from_handle sets is_shared on the resource itself, which is the flag
       * that matters, and a bind bit the is_format_supported query above was not made with
       * would be a question asked twice and answered once.
       *
       * The modifier is linear and that is not a fallback: MINISURF_ROW_ALIGN is 64, the
       * surface has no tiling and no UBWC metadata, and fd6_layout_resource_for_modifier
       * on DRM_FORMAT_MOD_LINEAR only notes "not UBWC" and returns success.
       */
      struct winsys_handle whandle = {
         .type = WINSYS_HANDLE_TYPE_FD,
         .handle = (unsigned) import->id,
         .stride = import->row_bytes,
         .offset = import->offset,
         .modifier = DRM_FORMAT_MOD_LINEAR,
      };

      /*
       * freedreno's own two conditions, checked here so that a failure names the number
       * that failed rather than leaving a NULL to be guessed at. Both are in
       * fd_resource_from_handle: the pitch must be at least width x bytes-per-texel, and
       * fdl_pitch over it must equal it -- which for a6xx means already aligned to
       * 1 << pitchalign, and pitchalign is at least 6. Sixty-four bytes, and the kernel's
       * own surfaces are aligned to exactly that (MINISURF_ROW_ALIGN), so this is an
       * agreement between two trees and not a hurdle.
       */
      unsigned texel = util_format_get_blocksize(fmt->pipe);
      if (import->row_bytes < width * texel) {
         say(why, why_size, "IOSurface %d has %u bytes a row, and %u texels of %u bytes "
                            "need at least %u: this GPU's import refuses a pitch narrower "
                            "than the image", import->id, import->row_bytes, width, texel,
             width * texel);
         free(texture);
         return NULL;
      }
      if (import->row_bytes % 64 != 0) {
         say(why, why_size, "IOSurface %d has %u bytes a row, which is not a multiple of "
                            "64: this GPU lays out a row aligned to that and its import "
                            "refuses a pitch it would have to change (the kernel's own "
                            "surfaces are aligned to 64 already)", import->id,
             import->row_bytes);
         free(texture);
         return NULL;
      }

      /*
       * The machine, AFTER the request has been checked and not before. softpipe HAS a
       * resource_from_handle; the winsys under it is null_sw_winsys, whose
       * displaytarget_from_handle is "return NULL" with no reason given, so an import
       * there fails for a reason that has nothing to do with the surface -- and if this
       * came first it would hide every refusal above it, so a build machine could never
       * exercise them and the sentence's own claim that the rest is fine would be a
       * claim about something nobody looked at.
       */
      if (device->software || !device->screen->resource_from_handle) {
         say(why, why_size, "%s imports no memory: a texture over an IOSurface is the "
                            "GPU's own import path (fd_resource_from_handle over the "
                            "kernel's kRedwoodSurface), and a rasterizer in plain C has "
                            "nowhere to put somebody else's pages. Everything else about "
                            "this request was checked and is fine: a %ux%u %s of %s with "
                            "%u bytes a row", rwa_device_name(device), width, height,
             type_name, fmt->name, import->row_bytes);
         free(texture);
         return NULL;
      }

      texture->res = device->screen->resource_from_handle(device->screen, &templ, &whandle,
                                                          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
      if (!texture->res) {
         /*
          * Everything this side could check has been checked, so no reason is invented
          * here. What is left, named as what is left: the kernel declining to hand the
          * surface's memory over -- which it does for a surface built around memory of a
          * process's own -- and a surface ID the kernel's table does not hold.
          */
         say(why, why_size, "%s would not take IOSurface %d as a %ux%u %s of %s with %u "
                            "bytes a row from offset %u, and the pitch and the shape are "
                            "both acceptable, so what is left is the surface itself: the "
                            "kernel will not give the GPU a surface whose pixels are "
                            "memory of a process's own, and it knows nothing of an ID that "
                            "was never made shareable", rwa_device_name(device), import->id,
             width, height, type_name, fmt->name, import->row_bytes, import->offset);
         free(texture);
         return NULL;
      }
      return texture;
   }

   texture->res = device->screen->resource_create(device->screen, &templ);
   if (!texture->res) {
      say(why, why_size, "the GPU driver would not allocate a %ux%ux%u %s of %s. After a few "
                         "dozen allocations in one boot this driver starts refusing outright "
                         "(docs/air.md section 4)", width, height, depth, type_name,
          fmt->name);
      free(texture);
      return NULL;
   }
   return texture;
}

/* Where a draw's time goes (rwa_render_draw): validation and the index scan, the binding
 * checks, the framebuffer and fixed state, the stages' resources, draw_vbo, the unbinding;
 * and the count. Read and cleared by rwa_draw_profile. */
static uint64_t rwa_draw_ns[7];

void
rwa_draw_profile(uint64_t out[7])
{
   RWA_GUARD();
   memcpy(out, rwa_draw_ns, sizeof(rwa_draw_ns));
   memset(rwa_draw_ns, 0, sizeof(rwa_draw_ns));
}

struct rwa_texture *
rwa_texture_create(struct rwa_device *device, const struct rwa_texture_shape *shape,
                   char *why, size_t why_size)
{
   /* Without the lock (26 October): texture_of_shape asks only the screen, which Gallium has
    * thread-safe, and a texture made took ~5 ms that the draws of the worker waited for -- 14 to
    * 20 times every five seconds (run 200, "rwm lock waits"). */
   return texture_of_shape(device, shape, NULL, why, why_size);
}

struct rwa_texture *
rwa_texture_from_surface(struct rwa_device *device, const struct rwa_texture_shape *shape,
                         int surface_id, unsigned offset, unsigned row_bytes, char *why,
                         size_t why_size)
{
   /* (no lock, as rwa_texture_create: the screen's resource_from_handle alone) */
   /*
    * A surface ID of zero is not a surface. IOSurfaceGetID answers 0 for one that was
    * never made shareable, and the kernel's table has nothing at 0 either, so this would
    * reach freedreno as an import of nothing and come back as the pitch sentence above --
    * which would name the wrong thing.
    */
   if (surface_id <= 0) {
      say(why, why_size, "IOSurface %d is not a surface: an ID of zero is what a surface "
                         "that was never made shareable answers, and there is nothing for "
                         "the kernel to look up", surface_id);
      return NULL;
   }
   const struct rwa_surface_import import = {
      .id = surface_id, .offset = offset, .row_bytes = row_bytes };

   return texture_of_shape(device, shape, &import, why, why_size);
}

void
rwa_texture_destroy(struct rwa_texture *texture)
{
   RWA_GUARD();
   if (!texture)
      return;
   argbuf_scrub(texture->device, NULL, texture, NULL);
   /* The view first: it holds a reference of its own on the resource. Nothing has to
    * be unbound here, because rwa_dispatch unbinds every texture it bound once its
    * fence has come back -- a context left holding this would be holding freed memory
    * the moment the last reference went, which is the same trap delete_compute_state
    * below is about.
    */
   if (texture->view)
      texture->device->pipe->sampler_view_destroy(texture->device->pipe, texture->view);
   /* by reference and not surface_destroy: a pass not yet sent still holds it */
   pipe_surface_reference(&texture->rt, NULL);
   pipe_resource_reference(&texture->res, NULL);
   free(texture);
}

/*
 * A region of a texture, as Gallium wants it, and everything that will not fit in one
 * refused.
 *
 * Two conventions meet here and only one of them is the obvious one: a slice of a 2D
 * array or a face of a cube goes in box.z, and a slice of a 1D ARRAY goes in box.y.
 * softpipe asserts exactly that (sp_texture.c, softpipe_transfer_map), so a 1D array
 * written through z lands on the wrong slice -- and in a release build, where that
 * assertion is compiled out, it lands there quietly.
 */
static bool
texture_box(const struct rwa_texture *texture, unsigned level, unsigned layer,
            const unsigned origin[3], const unsigned size[3], struct pipe_box *box,
            char *why, size_t why_size)
{
   const struct pipe_resource *res = texture->res;
   /* origin and size may both be left out, which is the whole level: a caller filling
    * a texture it has just made has nothing else to say, and -replaceRegion: with the
    * texture's own region is what it would write.
    */
   unsigned o[3] = { origin ? origin[0] : 0, origin ? origin[1] : 0, origin ? origin[2] : 0 };

   if (level > res->last_level) {
      say(why, why_size, "level %u of a texture with %u", level, res->last_level + 1);
      return false;
   }

   bool volume = res->target == PIPE_TEXTURE_3D;
   bool rows = res->target != PIPE_TEXTURE_1D && res->target != PIPE_TEXTURE_1D_ARRAY;
   unsigned w = u_minify(res->width0, level);
   unsigned h = rows ? u_minify(res->height0, level) : 1;
   unsigned d = volume ? u_minify(res->depth0, level) : 1;
   unsigned s[3] = { size ? size[0] : w, size ? size[1] : h, size ? size[2] : d };

   if (layer >= texture->layers) {
      say(why, why_size, "slice %u of a texture with %u", layer, texture->layers);
      return false;
   }
   if (!s[0] || !s[1] || !s[2]) {
      say(why, why_size, "a region of %ux%ux%u texels has no texels in it", s[0], s[1], s[2]);
      return false;
   }
   if (o[0] + s[0] > w || o[1] + s[1] > h || o[2] + s[2] > d) {
      say(why, why_size, "a region of %ux%ux%u texels at (%u, %u, %u) of a level that is "
                         "%ux%ux%u", s[0], s[1], s[2], o[0], o[1], o[2], w, h, d);
      return false;
   }

   *box = (struct pipe_box){ .x = o[0], .width = s[0] };
   if (!rows) {
      box->y = layer;
      box->height = 1;
      box->z = 0;
      box->depth = 1;
   } else {
      box->y = o[1];
      box->height = s[1];
      box->z = volume ? o[2] : layer;
      box->depth = volume ? s[2] : 1;
   }
   return true;
}

/* How wide a row of the caller's memory is, with Metal's convention that zero means
 * tightly packed, and the check that it is at least a row: a stride shorter than the
 * region would make every row after the first read from inside the one before it,
 * which produces an image rather than an error.
 */
static bool
caller_stride(const struct rwa_texture *texture, const struct pipe_box *box, size_t stride,
              size_t *out, char *why, size_t why_size)
{
   unsigned texel = util_format_get_blocksize(texture->res->format);
   size_t least = (size_t)box->width * texel;
   if (stride == 0) {
      *out = least;
      return true;
   }
   if (stride < least) {
      say(why, why_size, "%zu bytes per row for %u texels of %s, which need %zu", stride,
          box->width, texture->format_name, least);
      return false;
   }
   *out = stride;
   return true;
}

int
rwa_texture_write(struct rwa_texture *texture, unsigned level, unsigned layer,
                  const unsigned origin[3], const unsigned size[3], const void *bytes,
                  size_t stride, char *why, size_t why_size)
{
   RWA_GUARD();
   struct pipe_box box;
   size_t row;

   if (!bytes) {
      say(why, why_size, "no bytes to write into a texture");
      return -1;
   }
   /* A depth or stencil texture is refused here and read through rwa_depth_read, and
    * the two go together on purpose: a texel of Depth32Float_Stencil8 holds two values
    * of two kinds, so a call whose only argument is "bytes" cannot say which of them
    * the caller means. Nothing in this path needs to write one -- a depth attachment is
    * filled by the pass's clear and by the draw -- so this declines rather than
    * inventing an aspect argument no caller has asked for.
    */
   if (format_is_depth_stencil(texture->res->format)) {
      say(why, why_size, "writing texels into a %s texture: a depth or stencil texel has "
                         "an aspect and this call has nowhere to name it. A depth "
                         "attachment is filled by the render pass's clearDepth and by what "
                         "the draw writes", texture->format_name);
      return -1;
   }
   /*
    * And a multisample texture is refused for a harder reason than an argument the call
    * lacks: there is no way to write one from the processor at all, on either side.
    *
    * Metal forbids it twice. A multisample texture cannot be MTLStorageModeShared on
    * this device -- Metal's own "MTLTextureDescriptor: Multisample textures cannot be
    * allocated with MTLStorageModeShared on this device" -- and -replaceRegion: writes
    * only a texture the processor can see, so the one call that would do it cannot be
    * given a texture to do it to.
    *
    * Gallium cannot either. freedreno lays a multisample texel out as nr_samples texels'
    * worth of bytes (fd_resource_layout_init: "layout->cpp *= fd_resource_nr_samples")
    * in an order that is the hardware's, so bytes copied through a map would land in no
    * defined sample -- and they would land SILENTLY, which is the shape of failure this
    * library exists to avoid.
    *
    * What fills a multisample texture is a render pass into it, which is also the only
    * thing that fills one in Metal: rwa_render_create with this sample count and
    * rwa_render_draw with this texture as an attachment.
    */
   if (texture->res->nr_samples > 1) {
      say(why, why_size, "writing texels into a %s of %u samples per texel: Metal writes "
                         "no multisample texture from the processor (one cannot be "
                         "MTLStorageModeShared on this device, and -replaceRegion: writes "
                         "only what the processor can see) and this GPU lays the samples of "
                         "a texel out in an order of its own, so these bytes would land in "
                         "no defined sample. A multisample texture is filled by rendering "
                         "into it", texture->format_name, texture->res->nr_samples);
      return -1;
   }
   if (!texture_box(texture, level, layer, origin, size, &box, why, why_size) ||
       !caller_stride(texture, &box, stride, &row, why, why_size))
      return -1;

   /* The slice stride is the region's own rows, because the contract has one stride: a
    * caller handing over a 3D region hands over a packed stack of its rows, which is
    * what -replaceRegion:...bytesPerRow:bytesPerImage: is given when bytesPerImage is
    * the obvious product.
    */
   texture->device->pipe->texture_subdata(texture->device->pipe, texture->res, level,
                                          PIPE_MAP_WRITE, &box, bytes, row,
                                          row * box.height);
   return 0;
}

/*
 * MTLBlitCommandEncoder, which this driver did not have until 26 October: -blitCommandEncoder
 * answered nil, and every copy CoreAnimation made through one was not made -- above all the
 * snapshot of an application, a copy of the frame into a surface SpringBoard reads, which came
 * out black (or, before surface locks waited for the GPU, whatever the memory held: the parts of
 * other frames over the closing Settings in the user's video). A texture is copied by the GPU
 * (pipe->blit), and where its format cannot be drawn into, by resource_copy_region.
 */
int
rwa_texture_copy(struct rwa_texture *dst, unsigned dst_level, unsigned dst_slice,
                 const unsigned dst_origin[3], struct rwa_texture *src, unsigned src_level,
                 unsigned src_slice, const unsigned src_origin[3], const unsigned size[3],
                 char *why, size_t why_size)
{
   RWA_GUARD();
   if (!dst || !src || !dst->res || !src->res) {
      say(why, why_size, "a texture copy without two textures");
      return -1;
   }
   struct pipe_context *pipe = dst->device->pipe;
   struct pipe_screen *screen = dst->device->screen;
   unsigned depth = MAX2(size[2], 1);
   struct pipe_box from;

   u_box_3d(src_origin[0], src_origin[1], src_slice + src_origin[2], size[0], size[1], depth, &from);
   bool drawable = screen->is_format_supported(screen, dst->res->format, dst->res->target,
                                               dst->res->nr_samples, dst->res->nr_storage_samples,
                                               util_format_is_depth_or_stencil(dst->res->format)
                                                  ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET);
   if (drawable && src->res->nr_samples <= 1) {
      struct pipe_blit_info blit = { 0 };

      blit.dst.resource = dst->res;
      blit.dst.level = dst_level;
      blit.dst.format = dst->res->format;
      u_box_3d(dst_origin[0], dst_origin[1], dst_slice + dst_origin[2], size[0], size[1], depth, &blit.dst.box);
      blit.src.resource = src->res;
      blit.src.level = src_level;
      blit.src.format = src->res->format;
      blit.src.box = from;
      blit.mask = util_format_get_mask(dst->res->format);
      blit.filter = PIPE_TEX_FILTER_NEAREST;
      pipe->blit(pipe, &blit);
      return 0;
   }
   if (util_format_get_blocksize(dst->res->format) != util_format_get_blocksize(src->res->format)) {
      say(why, why_size, "a copy from %s into %s: texels of different sizes", src->format_name,
          dst->format_name);
      return -1;
   }
   pipe->resource_copy_region(pipe, dst->res, dst_level, dst_origin[0], dst_origin[1],
                              dst_slice + dst_origin[2], src->res, src_level, &from);
   return 0;
}

/*
 * A render pass that clears its attachments and draws nothing (26 October: such a pass did
 * nothing at all -- the clear was a property of a draw, and there was none). The attachments
 * become the framebuffer, are cleared, and stay bound for whatever comes next.
 */
int
rwa_render_clear(struct rwa_device *device, struct rwa_texture *const *color, unsigned color_count,
                 const float *clear_color, struct rwa_texture *depth, const float *clear_depth,
                 const unsigned *clear_stencil, char *why, size_t why_size)
{
   RWA_GUARD();
   struct pipe_context *pipe = device->pipe;
   struct pipe_framebuffer_state fb = { 0 };
   unsigned mask = 0;
   union pipe_color_union clear = { 0 };

   for (unsigned i = 0; i < color_count && i < PIPE_MAX_COLOR_BUFS; i++) {
      struct rwa_texture *t = color[i];

      if (!t || !t->res)
         continue;
      if (!t->rt)
         t->rt = pipe->create_surface(pipe, t->res, &(struct pipe_surface){ .format = t->res->format });
      if (!t->rt) {
         say(why, why_size, "no render surface of colour attachment %u to clear", i);
         return -1;
      }
      fb.cbufs[i] = t->rt;
      fb.nr_cbufs = i + 1;
      fb.width = t->res->width0;
      fb.height = t->res->height0;
   }
   if (depth && depth->res) {
      if (!depth->rt)
         depth->rt = pipe->create_surface(pipe, depth->res, &(struct pipe_surface){ .format = depth->res->format });
      fb.zsbuf = depth->rt;
      if (!fb.width) {
         fb.width = depth->res->width0;
         fb.height = depth->res->height0;
      }
   }
   if (!fb.width)
      return 0;
   if (clear_color && fb.nr_cbufs) {
      mask |= PIPE_CLEAR_COLOR;
      for (unsigned c = 0; c < 4; c++)
         clear.f[c] = clear_color[c];
   }
   if (clear_depth && fb.zsbuf && format_has_depth(depth->res->format))
      mask |= PIPE_CLEAR_DEPTH;
   if (clear_stencil && fb.zsbuf && format_has_stencil(depth->res->format))
      mask |= PIPE_CLEAR_STENCIL;
   pipe->set_framebuffer_state(pipe, &fb);
   if (mask)
      pipe->clear(pipe, mask, NULL, &clear, clear_depth ? (double)*clear_depth : 0.0,
                  clear_stencil ? *clear_stencil & 0xff : 0);
   return 0;
}

int
rwa_buffer_copy(struct rwa_buffer *dst, size_t dst_offset, struct rwa_buffer *src,
                size_t src_offset, size_t size, char *why, size_t why_size)
{
   RWA_GUARD();
   if (!dst || !src || dst_offset + size > dst->size || src_offset + size > src->size) {
      say(why, why_size, "a buffer copy of %zu bytes outside its buffers", size);
      return -1;
   }
   struct pipe_box from;

   u_box_1d((int)src_offset, (int)size, &from);
   dst->device->pipe->resource_copy_region(dst->device->pipe, dst->res, 0, (unsigned)dst_offset, 0, 0,
                                           src->res, 0, &from);
   return 0;
}

int
rwa_buffer_fill(struct rwa_buffer *buffer, size_t offset, size_t size, uint8_t value, char *why,
                size_t why_size)
{
   RWA_GUARD();
   if (!buffer || offset + size > buffer->size) {
      say(why, why_size, "a fill of %zu bytes outside its buffer", size);
      return -1;
   }
   buffer->device->pipe->clear_buffer(buffer->device->pipe, buffer->res, (unsigned)offset, (unsigned)size,
                                      &value, 1);
   return 0;
}

/*
 * -generateMipmapsForTexture:, every level below the base made from the one above it by the
 * GPU's own filtering (util_gen_mipmap: pipe->blit, one level at a time, linear). Until 27
 * October nothing was done, and CoreAnimation, which asks it of the window server's textures,
 * sampled levels nobody had written: black or the last frame's, wherever a layer was drawn
 * smaller than it is (run 226).
 */
int
rwa_texture_mipmaps(struct rwa_texture *texture, char *why, size_t why_size)
{
   RWA_GUARD();
   struct pipe_resource *res = texture ? texture->res : NULL;

   if (!res) {
      say(why, why_size, "mipmaps for no texture");
      return -1;
   }
   if (res->last_level == 0)
      return 0;
   unsigned last_layer = res->target == PIPE_TEXTURE_3D ? 0 : util_max_layer(res, 0);
   if (!util_gen_mipmap(texture->device->pipe, res, res->format, 0, res->last_level, 0, last_layer,
                        PIPE_TEX_FILTER_LINEAR)) {
      say(why, why_size, "the driver made no mipmaps for a %s texture", texture->format_name);
      return -1;
   }
   return 0;
}

int
rwa_texture_read(struct rwa_texture *texture, unsigned level, unsigned layer,
                 const unsigned origin[3], const unsigned size[3], void *bytes,
                 size_t stride, char *why, size_t why_size)
{
   RWA_GUARD();
   struct pipe_context *pipe = texture->device->pipe;
   struct pipe_box box;
   size_t row;

   if (!bytes) {
      say(why, why_size, "nowhere to read a texture into");
      return -1;
   }
   if (format_is_depth_stencil(texture->res->format)) {
      say(why, why_size, "reading texels out of a %s texture: it has %s, and the one answer "
                         "this call can give would be whichever aspect the caller did not "
                         "mean. rwa_depth_read takes a float per texel, a byte per texel, "
                         "or both", texture->format_name,
          format_has_depth(texture->res->format) && format_has_stencil(texture->res->format)
             ? "a depth and a stencil aspect"
             : format_has_depth(texture->res->format) ? "a depth aspect"
                                                      : "a stencil aspect");
      return -1;
   }
   /* A multisample texture has no one texel to give back: it holds nr_samples values
    * per texel and this call's answer is one of them, which is the same shape of
    * refusal as a depth texel's two aspects -- except that there is nowhere to add a
    * sample argument either, because Metal has no call that reads one sample of a
    * multisample texture to the processor (-getBytes: refuses one for the storage-mode
    * reason rwa_texture_write gives). The two things that read one in Metal are a
    * shader (read(coord, sample), which is what this library binds it for) and a
    * resolve into a one-sample texture, which this library has not got.
    */
   if (texture->res->nr_samples > 1) {
      say(why, why_size, "reading texels out of a %s of %u samples per texel: it holds %u "
                         "values per texel and the one answer this call can give would be "
                         "whichever sample the caller did not mean. Metal reads one with a "
                         "shader's read(coord, sample) or with a resolve, and neither is "
                         "this call", texture->format_name, texture->res->nr_samples,
          texture->res->nr_samples);
      return -1;
   }
   if (!texture_box(texture, level, layer, origin, size, &box, why, why_size) ||
       !caller_stride(texture, &box, stride, &row, why, why_size))
      return -1;

   struct pipe_transfer *xfer = NULL;
   const uint8_t *map = pipe->texture_map(pipe, texture->res, level, PIPE_MAP_READ, &box,
                                          &xfer);
   if (!map) {
      say(why, why_size, "the GPU driver would not map a %ux%u region of this texture for "
                         "the processor", box.width, box.height);
      return -1;
   }

   /* Row by row, because the driver's rows are as far apart as its own layout says and
    * the caller's are as far apart as the caller said. They are equal often enough
    * that a single memcpy would pass every test here and then tear on the phone, where
    * freedreno's pitch is aligned and softpipe's is not.
    */
   size_t texel_row = (size_t)box.width * util_format_get_blocksize(texture->res->format);
   uint8_t *dst = bytes;
   for (int z = 0; z < box.depth; z++)
      for (int y = 0; y < box.height; y++)
         memcpy(dst + ((size_t)z * box.height + y) * row,
                map + (size_t)z * xfer->layer_stride + (size_t)y * xfer->stride, texel_row);

   pipe->texture_unmap(pipe, xfer);
   return 0;
}

/*
 * What a depth/stencil texture holds, one aspect at a time (air_metal.h).
 *
 * The decoding is Mesa's own for the format and not arithmetic written here. That is
 * the same decision as the colour format table being identities only, and it is what
 * makes Depth16Unorm and Depth32Float comparable at all: both come back as a float in
 * [0, 1], 65535 and 1.0f being the same depth. util_format also knows the layouts this
 * library would otherwise have to guess -- freedreno keeps Z32_FLOAT_S8X24_UINT as two
 * resources and its transfer helper interleaves them for a map
 * (U_TRANSFER_HELPER_SEPARATE_Z32S8 in freedreno_resource.c), so what arrives here is
 * the packed format whichever back end it came from.
 */
int
rwa_depth_read(struct rwa_texture *texture, unsigned level, unsigned layer,
               const unsigned origin[3], const unsigned size[3], float *depth,
               uint8_t *stencil, char *why, size_t why_size)
{
   RWA_GUARD();
   struct pipe_context *pipe = texture->device->pipe;
   enum pipe_format format = texture->res->format;
   struct pipe_box box;

   if (!depth && !stencil) {
      say(why, why_size, "a depth read that asks for neither aspect");
      return -1;
   }
   if (!format_is_depth_stencil(format)) {
      say(why, why_size, "%s is a colour format and has no depth or stencil aspect; its "
                         "texels come back through rwa_texture_read", texture->format_name);
      return -1;
   }
   /* An aspect the format has not got is refused by name rather than zeroed: a caller
    * asking for the stencil of a Depth32Float texture believes it allocated a different
    * texture, and a buffer of zeros would be evidence for that belief.
    */
   if (depth && !format_has_depth(format)) {
      say(why, why_size, "the depth of a %s texture, which has only a stencil aspect",
          texture->format_name);
      return -1;
   }
   if (stencil && !format_has_stencil(format)) {
      say(why, why_size, "the stencil of a %s texture, which has only a depth aspect",
          texture->format_name);
      return -1;
   }
   /* A multisample depth attachment is refused here for rwa_texture_read's reason and
    * not for this one's: the aspect is named, the sample is not, and there is no Metal
    * call that reads one sample of one to the processor.
    */
   if (texture->res->nr_samples > 1) {
      say(why, why_size, "the depth of a %s of %u samples per texel: this call gives one "
                         "value per texel and such a texture holds %u, with no argument "
                         "here to say which. A multisample depth attachment is read by a "
                         "shader or resolved, and this library resolves nothing",
          texture->format_name, texture->res->nr_samples, texture->res->nr_samples);
      return -1;
   }
   if (!texture_box(texture, level, layer, origin, size, &box, why, why_size))
      return -1;

   const struct util_format_unpack_description *unpack =
      util_format_unpack_description(format);
   if ((depth && !unpack->unpack_z_float) || (stencil && !unpack->unpack_s_8uint)) {
      say(why, why_size, "Mesa has no unpacker for the %s aspect of %s, and this library "
                         "will not take the bits apart itself",
          depth && !unpack->unpack_z_float ? "depth" : "stencil", texture->format_name);
      return -1;
   }

   struct pipe_transfer *xfer = NULL;
   const uint8_t *map = pipe->texture_map(pipe, texture->res, level, PIPE_MAP_READ, &box,
                                          &xfer);
   if (!map) {
      say(why, why_size, "the GPU driver would not map a %ux%u region of this %s texture "
                         "for the processor", box.width, box.height, texture->format_name);
      return -1;
   }

   /* Row by row and slice by slice, for rwa_texture_read's reason: the driver's rows are
    * as far apart as its layout says. The caller's are tightly packed, one value per
    * texel, which is what the contract promises and is why there is no stride argument.
    */
   for (int z = 0; z < box.depth; z++) {
      for (int y = 0; y < box.height; y++) {
         const uint8_t *row = map + (size_t)z * xfer->layer_stride + (size_t)y * xfer->stride;
         size_t at = ((size_t)z * box.height + y) * box.width;
         if (depth)
            unpack->unpack_z_float(depth + at, 0, row, 0, box.width, 1);
         if (stencil)
            unpack->unpack_s_8uint(stencil + at, 0, row, 0, box.width, 1);
      }
   }

   pipe->texture_unmap(pipe, xfer);
   return 0;
}

/* MTLCompareFunction as PIPE_FUNC_x. It lives with the depth/stencil state further
 * down, because a depth test is where a compare function was first needed; the sampler
 * needs the same translation now that the shader's own `constexpr sampler` word carries
 * one, and the whole argument of docs/air.md 3.3 is that the two paths speak ONE
 * vocabulary. So it is declared here rather than copied.
 */
static bool compare_function_from_metal(unsigned f, unsigned *out, const char *what,
                                        char *why, size_t why_size);

/*
 * Metal's sampler in Gallium's terms. One function, for both of the two ways a sampler
 * reaches this library: the API's (MTLSamplerDescriptor, through rwa_sampler_state) and
 * the shader's own `constexpr sampler` word, which sampler_state_from_air turns into
 * the first. Keeping one mapping is what makes the two comparable, which is the whole
 * argument of air_metal.h's comment on rwa_sampler_state and of docs/air.md 3.3's 15
 * cases.
 *
 * Metal's clamp_to_zero is a wrap mode and not a border colour: outside the texture it
 * reads transparent black, in every channel including alpha, and the border colour is
 * not configurable for it. Gallium has no such mode, so it becomes CLAMP_TO_BORDER with
 * a zero border -- which is the same thing and is worth writing down, because the
 * obvious reading of that enum value is clamp_to_border with a default border, and one
 * of Apple's own binaries prints it that way (nir_air.h, docs/air.md 3.3).
 *
 * rwa_sampler_state carries no border colour, so clamp_to_border gets Metal's own
 * default, MTLSamplerBorderColorTransparentBlack -- the same thing clamp_to_zero reads.
 * A driver that wants opaque black or white has nothing in this contract to say it
 * with, and sampler_state_from_air refuses a shader's word that asks for one rather
 * than quietly sampling the wrong colour.
 */
static bool
sampler_state_from_metal(const struct rwa_sampler_state *st, struct pipe_sampler_state *out,
                         char *why, size_t why_size)
{
   static const unsigned wrap[] = {
      [RWA_ADDRESS_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      [RWA_ADDRESS_MIRROR_CLAMP_TO_EDGE] = PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE,
      [RWA_ADDRESS_REPEAT] = PIPE_TEX_WRAP_REPEAT,
      [RWA_ADDRESS_MIRROR_REPEAT] = PIPE_TEX_WRAP_MIRROR_REPEAT,
      [RWA_ADDRESS_CLAMP_TO_ZERO] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
      [RWA_ADDRESS_CLAMP_TO_BORDER] = PIPE_TEX_WRAP_CLAMP_TO_BORDER,
   };
   static const char *const axis[3] = { "s", "t", "r" };

   for (unsigned i = 0; i < 3; i++) {
      if (st->address[i] >= ARRAY_SIZE(wrap)) {
         say(why, why_size, "address mode %u on %s is not one of MTLSamplerAddressMode's six",
             st->address[i], axis[i]);
         return false;
      }
   }
   if (st->min_filter > RWA_FILTER_LINEAR || st->mag_filter > RWA_FILTER_LINEAR) {
      say(why, why_size, "filters %u and %u: MTLSamplerMinMagFilter is nearest or linear",
          st->min_filter, st->mag_filter);
      return false;
   }
   if (st->mip_filter > RWA_MIP_LINEAR) {
      say(why, why_size, "mip filter %u is not one of MTLSamplerMipFilter's three",
          st->mip_filter);
      return false;
   }
   unsigned compare_func;
   if (!compare_function_from_metal(st->compare_function, &compare_func,
                                    "the sampler's compare function", why, why_size))
      return false;
   if (st->max_anisotropy > 16) {
      say(why, why_size, "a maxAnisotropy of %u, where Metal's range is 1 to 16",
          st->max_anisotropy);
      return false;
   }
   if (!(st->lod_min <= st->lod_max)) {
      say(why, why_size, "a LOD clamp of [%g, %g]", (double)st->lod_min,
          (double)st->lod_max);
      return false;
   }

   /* MTLSamplerMipFilter and PIPE_TEX_MIPFILTER are not in the same order -- Gallium's
    * NONE is last and Metal's NotMipmapped is first -- so this is a table and not a
    * cast.
    */
   static const unsigned mip[] = {
      [RWA_MIP_NOT_MIPMAPPED] = PIPE_TEX_MIPFILTER_NONE,
      [RWA_MIP_NEAREST] = PIPE_TEX_MIPFILTER_NEAREST,
      [RWA_MIP_LINEAR] = PIPE_TEX_MIPFILTER_LINEAR,
   };

   *out = (struct pipe_sampler_state){
      .wrap_s = wrap[st->address[0]],
      .wrap_t = wrap[st->address[1]],
      .wrap_r = wrap[st->address[2]],
      .min_img_filter = st->min_filter == RWA_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR
                                                            : PIPE_TEX_FILTER_NEAREST,
      .mag_img_filter = st->mag_filter == RWA_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR
                                                            : PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = mip[st->mip_filter],
      /* Metal decides at the SAMPLE CALL whether a comparison happens
       * (sample_compare) and not in the sampler, so this state is the one a plain
       * sample() wants and rwa_sampler_create builds a second object out of it with
       * the comparison switched on (struct rwa_sampler). A stage binds that one into
       * the slots a shadow instruction reads and this one everywhere else -- which is
       * Metal's rule said in Gallium's words, and is why the function below is mapped
       * through a table whether a comparison happens or not.
       */
      .compare_mode = PIPE_TEX_COMPARE_NONE,
      .compare_func = compare_func,
      /* Gallium's field means the opposite of Metal's property, and both are called
       * something like "normalized": zero anisotropy is off, and unnormalized_coords
       * is pixel space.
       */
      .unnormalized_coords = !st->normalized_coordinates,
      .max_anisotropy = st->max_anisotropy > 1 ? st->max_anisotropy : 0,
      .min_lod = st->lod_min,
      .max_lod = st->lod_max,
   };
   /* transparent black, which is both Metal's default border colour and what
    * clamp_to_zero reads by definition. Written out rather than left to the zeroing
    * above, because it is a decision and not an omission.
    */
   out->border_color.f[0] = out->border_color.f[1] = 0.0f;
   out->border_color.f[2] = out->border_color.f[3] = 0.0f;

   /* Apple's own header says non-normalized coordinates "should only be used with 1D
    * and 2D textures with the ClampToEdge wrap mode, otherwise the results of sampling
    * are undefined" -- and this library does not enforce that, on purpose: one of
    * Apple's own shaders in the corpus is sampler(coord::pixel, filter::linear,
    * address::clamp_to_zero), and sampcheck.py measured what that must return against
    * Apple's rules, 15 cases exact (docs/air.md 3.3). Refusing it here would refuse a
    * shader that has been checked.
    */
   return true;
}

/* The shader's own `constexpr sampler`, said in the API's words first.
 *
 * The two enumerations are not the same, and that is the whole reason this goes through
 * rwa_sampler_state instead of mapping the word straight to Gallium: in the 64-bit word
 * a shader carries, 0 is clamp_to_zero and 1 clamp_to_edge (nir_air.h, docs/air.md
 * 3.3); in MTLSamplerAddressMode 0 is clampToEdge and 4 clampToZero. One table indexed
 * by "the address mode" would be right for one of them and silently wrong for the other
 * -- the commonest field in the corpus, and the same trap docs/air.md 3.3 records about
 * Apple's own printer.
 */
static bool
sampler_state_from_air(const struct air_sampler_state *a, unsigned index,
                       struct rwa_sampler_state *out, char *why, size_t why_size)
{
   static const unsigned address[] = {
      [AIR_SAMPLER_CLAMP_TO_ZERO] = RWA_ADDRESS_CLAMP_TO_ZERO,
      [AIR_SAMPLER_CLAMP_TO_EDGE] = RWA_ADDRESS_CLAMP_TO_EDGE,
      [AIR_SAMPLER_REPEAT] = RWA_ADDRESS_REPEAT,
      [AIR_SAMPLER_MIRRORED_REPEAT] = RWA_ADDRESS_MIRROR_REPEAT,
      [AIR_SAMPLER_CLAMP_TO_BORDER] = RWA_ADDRESS_CLAMP_TO_BORDER,
   };

   for (unsigned i = 0; i < 3; i++) {
      if (a->address[i] >= ARRAY_SIZE(address)) {
         say(why, why_size, "the shader's own sampler at index %u carries address mode %u, "
                            "which is none of the five its word can hold", index,
             a->address[i]);
         return false;
      }
      /* The word has a border colour and this contract has not, so a border that is
       * not Metal's default has nowhere to go. Refused rather than sampled as
       * transparent black: no module of the corpus attests clamp_to_border at all
       * (docs/air.md 3.3 lists it among the decoded and unexercised fields), so this
       * is a shader nobody has seen asking for a colour nobody can bind.
       */
      if (a->address[i] == AIR_SAMPLER_CLAMP_TO_BORDER && a->border_color != 0) {
         say(why, why_size, "the shader's own sampler at index %u clamps to a border colour "
                            "%u (opaque black or white), and nothing in this contract can "
                            "bind one: Metal's default, transparent black, is the only "
                            "border a sampler here has", index, a->border_color);
         return false;
      }
   }

   /* Bicubic is a Metal filter with no MTLSamplerMinMagFilter and no Gallium
    * equivalent. air_run samples it as linear and says so in its own line; a library
    * hands a driver a result, so it refuses instead. No module of the corpus carries
    * it (docs/air.md 3.3).
    */
   if (a->mag_filter == AIR_SAMPLER_BICUBIC || a->min_filter == AIR_SAMPLER_BICUBIC) {
      say(why, why_size, "the shader's own sampler at index %u asks for filter::bicubic, "
                         "which neither MTLSamplerDescriptor nor this GPU has", index);
      return false;
   }
   if (a->mag_filter > AIR_SAMPLER_BICUBIC || a->min_filter > AIR_SAMPLER_BICUBIC ||
       a->mip_filter > AIR_SAMPLER_MIP_LINEAR) {
      say(why, why_size, "the shader's own sampler at index %u carries filters %u/%u/%u, "
                         "which are not values its word can hold", index, a->mag_filter,
          a->min_filter, a->mip_filter);
      return false;
   }

   /*
    * The word's comparison function, which is NOT MTLCompareFunction's order and not
    * AGX's either -- the one field in this word where a cast would turn a comparison
    * into its opposite, so it is a table, and the table has three sources.
    *
    *   AIR, bits 16-19         MTLCompareFunction        AGX's _AGCCompareFunction
    *   0 none                  (Never: see below)        0 NONE
    *   1 less                  1 Less                    2 LESS
    *   2 less_equal            3 LessEqual               3 LEQUAL
    *   3 greater               4 Greater                 4 GREATER
    *   4 greater_equal         6 GreaterEqual            5 GEQUAL
    *   5 equal                 2 Equal                   6 EQUAL
    *   6 not_equal             5 NotEqual                7 NOTEQUAL
    *   7 always                7 Always                  8 ALWAYS
    *   8 never                 0 Never                   1 NEVER
    *
    * Casting the AIR value into MTLCompareFunction is wrong for five of the nine and
    * reverses the direction of two of them: 2 `less_equal` would arrive as Equal and 3
    * `greater` as LessEqual. Casting it into AGX's enumeration -- the other plausible
    * reading, since AGX is what Apple's own stack does with this field -- is wrong for
    * eight of the nine and, decisively, would make the DEFAULT mean `always`.
    *
    * Where the three columns come from:
    *
    *  - Apple's own metal_stdlib on the device declares `enum class compare_func` with
    *    these nine names in this order (metal_texture), and metal_config defines both
    *    __HAVE_NONE_COMPARE_FUNCTION__ and __HAVE_ALWAYS_NEVER_COMPARE_FUNCTION__ for
    *    this language version, so all nine exist.
    *  - the VALUES are clang's own, out of the macro table in libGPUCompilerImpl that
    *    defines __METAL_COMPARE_FUNC_*__: NOT_EQUAL 6, ALWAYS 7, NEVER 8, each value
    *    emitted beside its name.
    *  - and the mapping is confirmed by Apple's DECODER, AIRConstSampler::
    *    constructFlatReply in AGXCompilerCore, which reads this very field and
    *    translates it with a byte table -- `ubfx x8, x8, #16, #4; sub x8, #1;
    *    lsr x9, 0x0108070605040302, x8*8` -- so field 1 becomes AGC 2 (LESS), field 8
    *    becomes AGC 1 (NEVER), and field 0 falls out of range to AGC 0 (NONE). The
    *    same function reads the address modes at bits 0-8, the filters at 9-14 and the
    *    two LOD halves at bytes 3 and 5, which is how this reading of the whole word
    *    is checked and not only of this field.
    *
    * `never` AT 8 IS THE DEFAULT, AND THAT IS THE WHOLE POINT. _get_sampler_state() in
    * Apple's header initialises the field to __METAL_COMPARE_FUNC_NEVER__, so every
    * `constexpr sampler` whose source names no compare_func carries 8 -- including all
    * three words the 456-module corpus holds, whose every other unnamed field also
    * comes out equal to the header's default. The refusal this replaces tested for
    * `!= 0`, read `none` as the default, and so refused every inline sampler Apple's
    * compiler has ever emitted. In one boot of the window server that was 653 of 653
    * failed draws (logs/032854-run.log), all of them at value 8.
    *
    * `none` has no MTLCompareFunction: the API enumeration has eight values and no
    * "not configured". It becomes Never, which is MTLSamplerDescriptor's own default
    * for compareFunction, so a word that says `compare_func::none` and a descriptor
    * nobody touched arrive as the same sampler -- true for a plain sample, which is
    * the only thing either can do. What is NOT true is that they mean the same to a
    * comparison, where AGX keeps NONE and NEVER apart, so bind_stage refuses a shadow
    * instruction that reads a sampler whose word says `none` rather than comparing
    * with a function Apple spells differently.
    */
   static const unsigned compare[] = {
      [0] = 0,   /* none          -> MTLCompareFunctionNever, and see above */
      [1] = 1,   /* less          -> MTLCompareFunctionLess */
      [2] = 3,   /* less_equal    -> MTLCompareFunctionLessEqual */
      [3] = 4,   /* greater       -> MTLCompareFunctionGreater */
      [4] = 6,   /* greater_equal -> MTLCompareFunctionGreaterEqual */
      [5] = 2,   /* equal         -> MTLCompareFunctionEqual */
      [6] = 5,   /* not_equal     -> MTLCompareFunctionNotEqual */
      [7] = 7,   /* always        -> MTLCompareFunctionAlways */
      [8] = 0,   /* never         -> MTLCompareFunctionNever */
   };
   if (a->compare_func >= ARRAY_SIZE(compare)) {
      say(why, why_size, "the shader's own sampler at index %u carries compare function %u, "
                         "and Metal's compare_func has nine values: 0 none to 8 never",
          index, a->compare_func);
      return false;
   }

   static const unsigned filter[] = {
      [AIR_SAMPLER_NEAREST] = RWA_FILTER_NEAREST,
      [AIR_SAMPLER_LINEAR] = RWA_FILTER_LINEAR,
   };
   /* AIR_SAMPLER_MIP_* and MTLSamplerMipFilter happen to be in the same order; said as
    * a table anyway, so that the next field someone adds is not cast through by habit.
    */
   static const unsigned mip[] = {
      [AIR_SAMPLER_MIP_NONE] = RWA_MIP_NOT_MIPMAPPED,
      [AIR_SAMPLER_MIP_NEAREST] = RWA_MIP_NEAREST,
      [AIR_SAMPLER_MIP_LINEAR] = RWA_MIP_LINEAR,
   };

   *out = (struct rwa_sampler_state){
      .address = { address[a->address[0]], address[a->address[1]], address[a->address[2]] },
      .min_filter = filter[a->min_filter],
      .mag_filter = filter[a->mag_filter],
      .mip_filter = mip[a->mip_filter],
      .compare_function = compare[a->compare_func],
      .max_anisotropy = a->max_anisotropy,
      /* the word says coord::pixel; the descriptor says normalizedCoordinates = NO */
      .normalized_coordinates = !a->pixel_coord,
      /* halves, as their bits, which is how the word carries them: the corpus's
       * samplers all come out as [0, 65504], and that range is two separate 16-bit
       * fields a wrong layout would not land on (docs/air.md 3.3).
       */
      .lod_min = _mesa_half_to_float(a->lod_min),
      .lod_max = _mesa_half_to_float(a->lod_max),
   };
   return true;
}

struct rwa_sampler *
rwa_sampler_create(struct rwa_device *device, const struct rwa_sampler_state *state,
                   char *why, size_t why_size)
{
   RWA_GUARD();
   if (!state) {
      say(why, why_size, "a sampler with no state");
      return NULL;
   }

   struct pipe_sampler_state st;
   if (!sampler_state_from_metal(state, &st, why, why_size))
      return NULL;

   struct rwa_sampler *sampler = calloc(1, sizeof(*sampler));
   if (!sampler) {
      say(why, why_size, "out of memory making a sampler");
      return NULL;
   }
   sampler->device = device;
   sampler->cso = device->pipe->create_sampler_state(device->pipe, &st);
   /* and the same state once more with the comparison switched on, for the slots a
    * sample_compare reads: struct rwa_sampler says why there have to be two. The
    * function is already in `st` -- sampler_state_from_metal mapped it whether a
    * comparison happens or not -- so this is one field and not a second translation.
    */
   st.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
   sampler->cso_compare = device->pipe->create_sampler_state(device->pipe, &st);
   if (!sampler->cso || !sampler->cso_compare) {
      say(why, why_size, "%s would not make a sampler of that state",
          rwa_device_name(device));
      if (sampler->cso)
         device->pipe->delete_sampler_state(device->pipe, sampler->cso);
      if (sampler->cso_compare)
         device->pipe->delete_sampler_state(device->pipe, sampler->cso_compare);
      free(sampler);
      return NULL;
   }
   return sampler;
}

void
rwa_sampler_destroy(struct rwa_sampler *sampler)
{
   RWA_GUARD();
   if (!sampler)
      return;
   /* Nothing is unbound here: rwa_dispatch binds the samplers it needs and unbinds
    * them once its fence has come back, so nothing of this is in the context now. It
    * matters that it is not -- softpipe's delete_sampler_state is a plain FREE
    * (sp_state_sampler.c), so a state deleted while bound leaves the context holding
    * freed memory until something else is bound over it.
    */
   argbuf_scrub(sampler->device, NULL, NULL, sampler);
   sampler->device->pipe->delete_sampler_state(sampler->device->pipe, sampler->cso);
   sampler->device->pipe->delete_sampler_state(sampler->device->pipe,
                                               sampler->cso_compare);
   free(sampler);
}

/*
 * ---------------------------------------------------------------------------------------
 * THE ARGUMENT BUFFER RECORDER: MTLArgumentEncoder's three set calls, and nothing
 * written into the buffer.
 *
 * air_metal.h carries the whole of why. In one line: the frontend resolved every member
 * of every set to a binding while translating, Metal's 8-byte handles mean nothing to a
 * driver that binds by index, and so an encoder here records (resource, Metal id)
 * against (destination buffer, destination offset) and the draw binds what it finds.
 */

/* The encoding at `offset` of this buffer, or NULL. A buffer with no encodings -- which
 * is every buffer that is only ever a buffer -- answers in one comparison.
 */
static struct rwa_argbuf_encoding *
argbuf_find(const struct rwa_buffer *dest, size_t offset)
{
   for (struct rwa_argbuf_encoding *e = dest ? dest->encodings : NULL; e; e = e->next) {
      if (e->offset == offset)
         return e;
   }
   return NULL;
}

/* The encoding at `offset`, made if it is not there, with room for `id`.
 *
 * It is NOT cleared when it is found, and that is Metal's behaviour rather than a
 * shortcut: -setArgumentBuffer:offset: starts an encoding over a buffer whose BYTES are
 * still whatever the last encoding wrote there, so an application that sets three of
 * five members leaves the other two as they were. A record that cleared itself would
 * refuse a draw Metal would have made.
 */
static struct rwa_argbuf_encoding *
argbuf_encoding_for(struct rwa_buffer *dest, size_t offset, unsigned id, char *why,
                    size_t why_size)
{
   struct rwa_argbuf_encoding *e = argbuf_find(dest, offset);

   if (!e) {
      e = calloc(1, sizeof(*e));
      if (!e) {
         say(why, why_size, "out of memory recording an argument buffer");
         return NULL;
      }
      e->offset = offset;
      e->next = dest->encodings;
      dest->encodings = e;
      /* and the buffer joins the device's list of buffers that hold records, once, so
       * that destroying a resource can scrub itself out of them */
      if (!dest->listed) {
         dest->listed = true;
         dest->next_encoded = dest->device->encoded;
         dest->device->encoded = dest;
      }
   }

   if (id >= e->slots) {
      unsigned want = (id + 8) & ~7u;
      struct rwa_argbuf_slot *grown = realloc(e->slot, want * sizeof(*grown));

      if (!grown) {
         say(why, why_size, "out of memory recording member %u of an argument buffer", id);
         return NULL;
      }
      memset(grown + e->slots, 0, (want - e->slots) * sizeof(*grown));
      e->slot = grown;
      e->slots = want;
   }
   return e;
}

/* What every one of the three set calls checks before it records anything. `where` names
 * the call in the sentence, because "a texture" and "a buffer" are different mistakes.
 */
static struct rwa_argbuf_encoding *
argbuf_begin_member(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                    const char *what, char *why, size_t why_size)
{
   if (!dest) {
      say(why, why_size, "%s was encoded into no argument buffer at all: Metal's "
                         "-setArgumentBuffer:offset: has to name one first", what);
      return NULL;
   }
   if (id >= RWA_MAX_ARGBUF_MEMBERS) {
      say(why, why_size, "%s was encoded at id %u and this library records %u members of "
                         "a set", what, id, RWA_MAX_ARGBUF_MEMBERS);
      return NULL;
   }
   /* The handle Metal would write is eight bytes at dest_offset + 8 * id, and an
    * encoding that does not fit the buffer is an application that allocated it from
    * something other than -encodedLength. Nothing of ours would read those bytes, which
    * is exactly why it has to be said here: this library would otherwise be the only
    * thing in the system that did not notice.
    */
   size_t need = dest_offset + (size_t)8 * id + 8;
   if (need > dest->size) {
      say(why, why_size, "%s was encoded at id %u of an argument buffer at offset %zu, "
                         "which Metal would write %zu bytes into, and that buffer holds "
                         "%zu", what, id, dest_offset, need, dest->size);
      return NULL;
   }
   return argbuf_encoding_for(dest, dest_offset, id, why, why_size);
}

int
rwa_argbuf_encode_buffer(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                         struct rwa_buffer *buffer, size_t offset, char *why,
                         size_t why_size)
{
   RWA_GUARD();
   struct rwa_argbuf_encoding *e =
      argbuf_begin_member(dest, dest_offset, id, "a buffer", why, why_size);

   if (!e)
      return -1;
   if (!buffer) {
      say(why, why_size, "no buffer was encoded at id %u, and a member of a set cannot "
                         "be unbound: this library binds every member a shader declares "
                         "and refuses the draw otherwise", id);
      return -1;
   }
   if (buffer->device != dest->device) {
      say(why, why_size, "the buffer encoded at id %u belongs to another device", id);
      return -1;
   }
   if (offset > buffer->size) {
      say(why, why_size, "the buffer encoded at id %u is bound at offset %zu of %zu "
                         "bytes", id, offset, buffer->size);
      return -1;
   }
   if (!e->slot[id].set)
      e->members++;
   e->slot[id] = (struct rwa_argbuf_slot){ .set = true, .kind = RWA_ARGBUF_BUFFER,
                                           .buffer = buffer, .offset = offset };
   return 0;
}

int
rwa_argbuf_encode_texture(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                          struct rwa_texture *texture, char *why, size_t why_size)
{
   RWA_GUARD();
   struct rwa_argbuf_encoding *e =
      argbuf_begin_member(dest, dest_offset, id, "a texture", why, why_size);

   if (!e)
      return -1;
   if (!texture) {
      say(why, why_size, "no texture was encoded at id %u, and a member of a set cannot "
                         "be unbound", id);
      return -1;
   }
   if (texture->device != dest->device) {
      say(why, why_size, "the texture encoded at id %u belongs to another device", id);
      return -1;
   }
   if (!e->slot[id].set)
      e->members++;
   e->slot[id] = (struct rwa_argbuf_slot){ .set = true, .kind = RWA_ARGBUF_TEXTURE,
                                           .texture = texture };
   return 0;
}

int
rwa_argbuf_encode_sampler(struct rwa_buffer *dest, size_t dest_offset, unsigned id,
                          struct rwa_sampler *sampler, char *why, size_t why_size)
{
   RWA_GUARD();
   struct rwa_argbuf_encoding *e =
      argbuf_begin_member(dest, dest_offset, id, "a sampler", why, why_size);

   if (!e)
      return -1;
   if (!sampler) {
      say(why, why_size, "no sampler was encoded at id %u, and a member of a set cannot "
                         "be unbound", id);
      return -1;
   }
   if (sampler->device != dest->device) {
      say(why, why_size, "the sampler encoded at id %u belongs to another device", id);
      return -1;
   }
   if (!e->slot[id].set)
      e->members++;
   e->slot[id] = (struct rwa_argbuf_slot){ .set = true, .kind = RWA_ARGBUF_SAMPLER,
                                           .sampler = sampler };
   return 0;
}

unsigned
rwa_argbuf_encoded_members(const struct rwa_buffer *dest, size_t dest_offset)
{
   RWA_GUARD();
   const struct rwa_argbuf_encoding *e = argbuf_find(dest, dest_offset);

   return e ? e->members : 0;
}

/*
 * A resource destroyed after it was encoded, scrubbed out of every record that names it.
 *
 * Metal would let the handle go stale and the GPU read freed memory, which is what
 * -useResource: and MTLHeap residency exist to prevent on that side. Here the record
 * holds a pointer this library would dereference at the next draw, so the alternative to
 * scrubbing is not a wrong picture but a crash in this library -- and a flag left behind
 * turns it into a sentence naming the member instead.
 */
static void
argbuf_scrub(struct rwa_device *device, const struct rwa_buffer *buffer,
             const struct rwa_texture *texture, const struct rwa_sampler *sampler)
{
   for (struct rwa_buffer *b = device ? device->encoded : NULL; b; b = b->next_encoded) {
      for (struct rwa_argbuf_encoding *e = b->encodings; e; e = e->next) {
         for (unsigned i = 0; i < e->slots; i++) {
            struct rwa_argbuf_slot *sl = &e->slot[i];

            if (!sl->set || sl->destroyed)
               continue;
            if ((buffer && sl->buffer == buffer) ||
                (texture && sl->texture == texture) ||
                (sampler && sl->sampler == sampler)) {
               sl->destroyed = true;
               sl->buffer = NULL;
               sl->texture = NULL;
               sl->sampler = NULL;
            }
         }
      }
   }
}

/* And the records of a buffer that is itself going away. Called from
 * rwa_buffer_destroy, which also scrubs that buffer out of everybody else's records.
 */
static void
argbuf_forget_all(struct rwa_buffer *dest)
{
   struct rwa_argbuf_encoding *e = dest->encodings;

   while (e) {
      struct rwa_argbuf_encoding *next = e->next;

      free(e->slot);
      free(e);
      e = next;
   }
   dest->encodings = NULL;
   if (dest->listed) {
      struct rwa_buffer **link = &dest->device->encoded;

      while (*link && *link != dest)
         link = &(*link)->next_encoded;
      if (*link == dest)
         *link = dest->next_encoded;
      dest->listed = false;
      dest->next_encoded = NULL;
   }
}

/*
 * WHAT A BUFFER'S OFFSET HAS TO SATISFY, for a member of an argument buffer and for a
 * plain buffer argument alike -- one rule and one sentence, because they are the same
 * arithmetic: Metal's own `offset:` from -setFragmentBuffer:offset:atIndex: or from
 * -[MTLArgumentEncoder setBuffer:offset:atIndex:], and Gallium's binding underneath.
 *
 * `constant` (a UBO) is NOT refused for its alignment here, because bind_stage serves any
 * offset: an aligned one as the binding's own offset, and one this GPU cannot fetch from
 * as a `user_buffer`, which both back ends upload themselves. `device` (an SSBO) is
 * refused, and the asymmetry is the point rather than an omission -- the shader may WRITE
 * an SSBO, so the copy that serves a UBO would take the writes with it when it was thrown
 * away, and Gallium has no user_buffer on a shader buffer for that very reason.
 */
static int
buffer_offset_ok(const struct rwa_device *device, const struct air_resource *r,
                 const struct rwa_buffer *b, size_t offset, const char *name,
                 const char *what, char *why, size_t why_size)
{
   if (offset > b->size) {
      say(why, why_size, "%s is bound at offset %zu of a buffer that holds %zu bytes",
          what, offset, b->size);
      return -1;
   }
   if (r->kind == AIR_RESOURCE_DEVICE_BUFFER &&
       offset % device->ssbo_offset_align != 0) {
      say(why, why_size, "%s is bound at offset %zu, and %s declares it in the `device` "
                         "address space, whose offsets this GPU fetches in multiples of "
                         "%u bytes. A `constant` buffer at such an offset is uploaded "
                         "instead; a `device` one cannot be, because the shader may write "
                         "it and the copy would throw the writes away", what, offset,
          name, device->ssbo_offset_align);
      return -1;
   }
   return 0;
}

/*
 * THE OFFSET A DIRECT BUFFER ARGUMENT WAS BOUND AT, out of the stage's buffer table, and
 * the cross-check between the two tables a driver fills.
 *
 * `rwa_bindings.buffers` says which buffer is at index n and the table says where in it;
 * a driver fills both from the one table Metal has, so where both name a buffer they must
 * name the same one. Two that disagree are a driver filling one of them from the wrong
 * place -- and the alternative to refusing is binding one buffer's memory at the other
 * one's offset, which is a wrong picture and not an error.
 */
static int
direct_buffer_offset(const struct air_resource *r, const struct rwa_bindings *bindings,
                     const struct rwa_buffer_bind *table, unsigned table_count,
                     const char *name, size_t *offset, char *why, size_t why_size)
{
   *offset = 0;
   if (!table || r->index >= table_count || !table[r->index].buffer)
      return 0;
   if (bindings->buffers && r->index < bindings->buffer_count &&
       bindings->buffers[r->index] &&
       bindings->buffers[r->index] != table[r->index].buffer) {
      say(why, why_size, "the two buffer tables of this draw disagree about index %u: %s "
                         "reads a buffer there, and one table names one buffer and the "
                         "other names another", r->index, name);
      return -1;
   }
   *offset = table[r->index].offset;
   return 0;
}

/*
 * ONE MEMBER, AT THE DRAW: what is bound for the resource the frontend resolved out of a
 * set, and the five refusals that are the whole point of this feature.
 *
 * `r` is a resource whose `argbuf_set` is not ~0u: a member of the set bound at Metal
 * buffer index `r->argbuf_set`, named `r->argbuf_id` by the application and bound at
 * `r->index` by this library. What comes back is whichever of the three pointers the
 * member's kind asks for, or -1 and a sentence.
 */
static int
argbuf_member(const struct air_resource *r, const struct rwa_buffer_bind *table,
              unsigned table_count, const char *name, struct rwa_buffer **buffer,
              size_t *offset, struct rwa_texture **texture, struct rwa_sampler **sampler,
              char *why, size_t why_size)
{
   const char *kind = r->kind == AIR_RESOURCE_SAMPLER   ? "sampler"
                    : r->kind == AIR_RESOURCE_TEXTURE   ? "texture"
                    : r->kind == AIR_RESOURCE_IMAGE     ? "texture"
                                                        : "buffer";
   const char *member = r->name ? r->name : "an unnamed member";

   if (buffer)
      *buffer = NULL;
   if (texture)
      *texture = NULL;
   if (sampler)
      *sampler = NULL;
   if (offset)
      *offset = 0;

   /* Nothing bound at the set's own Metal index. The shader would read a descriptor out
    * of memory that is not there at all.
    */
   const struct rwa_buffer_bind *bind =
      table && r->argbuf_set < table_count ? &table[r->argbuf_set] : NULL;
   if (!bind || !bind->buffer) {
      say(why, why_size, "%s reads %s `%s` out of the argument buffer at Metal buffer "
                         "index %u and nothing is bound there (%u slot%s given)", name,
          kind, member, r->argbuf_set, table_count, table_count == 1 ? "" : "s");
      return -1;
   }

   /*
    * THE REFUSAL THIS FEATURE IS MOSTLY MADE OF: a buffer is bound there, and it was
    * never filled through an MTLArgumentEncoder.
    *
    * Metal's argument buffer holds 8-byte handles, and an application may put them there
    * by any means: the processor writing them, a compute kernel writing them, a blit
    * copying them from another buffer. This library binds a member from the RECORD an
    * encoder left and never from the buffer's contents, so for those three there is
    * nothing to bind -- and nothing distinguishes such a buffer from a correctly encoded
    * one except the absence of that record. The alternative to this sentence is not a
    * worse picture, it is drawing over whatever resource index 0 happens to hold.
    */
   if (rwa_argbuf_encoded_members(bind->buffer, bind->offset) == 0) {
      say(why, why_size, "the buffer bound as the argument buffer at Metal buffer index "
                         "%u (offset %zu) was never filled through an MTLArgumentEncoder, "
                         "so there is no record of what %s's %s `%s` (id %u) should be. "
                         "This driver binds an argument buffer's members from what the "
                         "encoder recorded and never from the handles in the buffer, "
                         "which are an Apple GPU's descriptors; one written by the "
                         "processor, by a kernel or by a blit cannot be read here at all",
          r->argbuf_set, bind->offset, name, kind, member, r->argbuf_id);
      return -1;
   }

   const struct rwa_argbuf_encoding *e = argbuf_find(bind->buffer, bind->offset);
   const struct rwa_argbuf_slot *sl =
      e && r->argbuf_id < e->slots ? &e->slot[r->argbuf_id] : NULL;

   /* Encoded, but not this member: Metal would have written a handle of zero there and
    * the GPU would read descriptor zero of the device, which is some other resource.
    */
   if (!sl || !sl->set) {
      say(why, why_size, "%s reads %s `%s` at id %u of the argument buffer at Metal "
                         "buffer index %u, and that id was never set on the encoder "
                         "(%u member%s encoded at offset %zu)", name, kind, member,
          r->argbuf_id, r->argbuf_set, e ? e->members : 0,
          e && e->members == 1 ? "" : "s", bind->offset);
      return -1;
   }
   if (sl->destroyed) {
      say(why, why_size, "the %s encoded at id %u of the argument buffer at Metal buffer "
                         "index %u has been destroyed since, and %s reads `%s` out of it",
          kind, r->argbuf_id, r->argbuf_set, name, member);
      return -1;
   }

   /* Encoded as the wrong kind of thing. The id is the same number in both namings, so
    * an application that sent -setTexture:atIndex: where the shader declares a buffer
    * has either the wrong shader or the wrong id, and the handle Metal wrote would be a
    * texture descriptor read as a pointer.
    */
   unsigned want = r->kind == AIR_RESOURCE_SAMPLER ? RWA_ARGBUF_SAMPLER
                 : r->kind == AIR_RESOURCE_TEXTURE ? RWA_ARGBUF_TEXTURE
                 : r->kind == AIR_RESOURCE_IMAGE   ? RWA_ARGBUF_TEXTURE
                                                   : RWA_ARGBUF_BUFFER;
   if (sl->kind != want) {
      static const char *const encoded_as[] = { "a buffer", "a texture", "a sampler" };

      say(why, why_size, "%s declares %s `%s` at id %u of the argument buffer at Metal "
                         "buffer index %u, and %s was encoded there", name, kind, member,
          r->argbuf_id, r->argbuf_set,
          sl->kind < 3 ? encoded_as[sl->kind] : "something else");
      return -1;
   }

   if (want == RWA_ARGBUF_BUFFER) {
      /* Metal's own offset:, honoured, by the rule a direct argument's offset follows:
       * see buffer_offset_ok, which is where both sentences live.
       */
      char what[192];

      snprintf(what, sizeof(what), "the buffer encoded at id %u of the argument buffer at "
                                   "Metal buffer index %u (member `%s`)", r->argbuf_id,
               r->argbuf_set, member);
      if (buffer_offset_ok(sl->buffer->device, r, sl->buffer, sl->offset, name, what, why,
                           why_size) != 0)
         return -1;
      if (buffer)
         *buffer = sl->buffer;
      if (offset)
         *offset = sl->offset;
   } else if (want == RWA_ARGBUF_TEXTURE) {
      if (texture)
         *texture = sl->texture;
   } else if (sampler) {
      *sampler = sl->sampler;
   }
   return 0;
}

/*
 * The element type of a texture argument as Apple's metadata declares it, onto the slot:
 * the word inside the angle brackets of air.arg_type_name, which is the Metal
 * programmer's own `texture2d<half, write>` or `texture2d<uint, sample>`.
 *
 * It is read from the name rather than from the shader's instructions because it is the
 * declaration a driver has to honour, and the only statement of the element type in
 * hand that did not come through this front end's translation. All three fields are
 * written or none: a name this does not recognise leaves declared_element NULL, which
 * is how every reader asks whether there is a declaration at all.
 */
static void
declare_slot_element(const char *type_name, struct rwa_slot *slot)
{
   const char *lt = type_name ? strchr(type_name, '<') : NULL;
   if (!lt)
      return;
   for (lt++; *lt == ' '; lt++)
      ;

   /* Every element type MSL allows a texture: 32- and 16-bit, and no 8-bit one -- a
    * texture of chars is spelled with a normalised format and read as float.
    */
   static const struct { const char *word; nir_alu_type type; unsigned bits; } words[] = {
      { "float",  nir_type_float, 32 }, { "half",   nir_type_float, 16 },
      { "int",    nir_type_int,   32 }, { "short",  nir_type_int,   16 },
      { "uint",   nir_type_uint,  32 }, { "ushort", nir_type_uint,  16 },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
      size_t n = strlen(words[i].word);
      /* the whole word and not a prefix of it: "ushort" must not read as "uint" */
      if (strncmp(lt, words[i].word, n) ||
          (lt[n] != ',' && lt[n] != '>' && lt[n] != ' '))
         continue;
      slot->declared_type = words[i].type;
      slot->declared_element = words[i].word;
      slot->declared_bits = words[i].bits;
      return;
   }
}

/* The shape of every texture slot a kernel touches, off its own instructions.
 *
 * This is air_run's image_shapes() and texture_shapes() in one pass, and the reason
 * they read instructions rather than the Metal type name in the metadata is theirs:
 * air.read_texture_* and air.write_texture_* become nir_image_load and nir_image_store
 * while a read-only texture's read() becomes a txf (air_intrinsics.c), so whether a
 * slot wants a sampler view or an image view, and what element type it is read as, are
 * properties of the instruction the GPU will execute. air_run reads them to invent a
 * texture; here they are read to check the one the caller brought, which is the same
 * knowledge pointed the other way.
 *
 * Per slot and not per shader: BlitLumaKernel_Unorm reads a uint image and writes a
 * half one, so one answer for the kernel would be right for one of its two images and
 * quietly wrong for the other.
 *
 * `compare_sampler` is the one answer here that is about SAMPLER slots and not texture
 * slots, and is optional. It records which samplers a shadow texture instruction reads,
 * which is the question struct rwa_sampler exists to answer: Metal decides at the
 * sample call that a comparison happens, Gallium decides it in the sampler state, and
 * the two only agree if the comparison is switched on for exactly these slots. Read off
 * the instructions for the same reason everything else here is: a `constexpr sampler`
 * carrying compare_func::less is still a plain sampler to every sample() that reads it.
 */
static void
slot_shapes(nir_shader *cs, struct rwa_slot slots[RWA_MAX_TEXTURES],
            bool compare_sampler[RWA_MAX_TEXTURES])
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            unsigned binding;
            nir_alu_type type;
            enum glsl_sampler_dim dim;
            bool is_array;

            bool is_tex = instr->type == nir_instr_type_tex;

            if (is_tex) {
               nir_tex_instr *t = nir_instr_as_tex(instr);
               binding = t->texture_index;
               type = t->dest_type;
               dim = t->sampler_dim;
               is_array = t->is_array;
               if (compare_sampler && t->is_shadow &&
                   t->sampler_index < RWA_MAX_TEXTURES)
                  compare_sampler[t->sampler_index] = true;
               if (t->is_shadow && binding < RWA_MAX_TEXTURES)
                  slots[binding].shadow = true;
            } else if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
               if (in->intrinsic == nir_intrinsic_image_load)
                  type = nir_intrinsic_dest_type(in);
               else if (in->intrinsic == nir_intrinsic_image_store)
                  type = nir_intrinsic_src_type(in);
               else
                  continue;
               /* a computed binding: there is nothing here to check a texture against,
                * and the slot stays unknown rather than wrongly shaped
                */
               if (!nir_src_is_const(in->src[0]))
                  continue;
               binding = nir_src_as_uint(in->src[0]);
               dim = nir_intrinsic_image_dim(in);
               is_array = nir_intrinsic_image_array(in);
            } else {
               continue;
            }

            if (binding >= RWA_MAX_TEXTURES)
               continue;   /* an index past the slots is refused by name in rwa_dispatch */
            if (!slots[binding].shape_known) {
               slots[binding].shape_known = true;
               slots[binding].dim = dim;
               slots[binding].is_array = is_array;
            }

            /* The element type is taken from a tex instruction and from nothing else.
             *
             * nir_tex_instr::dest_type is a real statement about the texture: a half
             * texture read through Metal's read() becomes a txf whose dest_type is
             * float16, and that is the type the slot holds. An image_load's dest_type
             * and an image_store's src_type are not: with format=none they mean "raw
             * bits, the bound image's format decides the conversion", so the front end
             * emits uint for both, and BlitKernel_Unorm -- whose dst is declared
             * texture2d<half, write> -- stores through src_type=uint. Reading that as a
             * claim about the texture refused 5 of AV1SW's 11 kernels when each was
             * bound the formats its own metadata asks for, measured with
             * air_metal_test: every one of the _Unorm kernels, whose destination is
             * half, and for binding a float format to what was taken for an integer
             * shader -- the opposite of what Apple declared.
             *
             * The shape (dim, array) is a different matter and is taken from either,
             * because an image intrinsic's image_dim does say which texture it is.
             */
            if (is_tex && !slots[binding].type_known) {
               slots[binding].type_known = true;
               slots[binding].type = nir_alu_type_get_base_type(type);
            }
         }
      }
   }
}

struct rwa_compute *
rwa_compute_create(struct rwa_device *device, const void *bitcode, size_t size,
                   const char *entry, struct rwa_compute_info *info,
                   char *why, size_t why_size)
{
   RWA_GUARD();
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return NULL;
   }

   struct rwa_compute *compute = calloc(1, sizeof(*compute));
   if (!compute) {
      say(why, why_size, "out of memory compiling a shader");
      return NULL;
   }
   compute->device = device;
   compute->mem_ctx = ralloc_context(NULL);

   /* Function constants are taken as zero. There is nothing to take them from: Metal's
    * specialization goes through -[MTLFunction newFunctionWithConstantValues:], which
    * is one of the methods that run inside Metal's own classes through a nil compiler
    * and return nil before a driver sees them (docs/metal.md 10.7). A module that needs
    * real values will say so -- the translator refuses what it cannot fold.
    */
   struct air_constant_value *constants;
   unsigned num_constants =
      air_module_function_constants(compute->mem_ctx, bitcode, size, &constants);

   /* library_wrapper is false, unlike air_run --library: a wrapped air.visible function
    * is a shader made up to be measurable, not a pipeline anyone can dispatch. Metal
    * stitches those into a caller at pipeline time (air.stitching_info), which this
    * frontend does not implement, and a driver asked to make a compute pipeline out of
    * one should be told that rather than handed a wrapper.
    */
   const struct air_to_nir_options options = {
      .constants = constants,
      .num_constants = num_constants,
   };
   const char *error;
   compute->nir = air_to_nir(compute->mem_ctx, bitcode, size, entry, &options,
                             device->nir_options, &compute->info, &error);
   if (!compute->nir) {
      /* The sentence the translator wrote, which is the one that matters: it begins
       * with its own category (reader, stage, interface, instruction, intrinsic) and
       * names what it met. 135 of the corpus's 456 modules are refused with one of
       * these, and a driver should repeat it to the application rather than swallow it.
       */
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      goto fail;
   }

   if (compute->info.stage != MESA_SHADER_COMPUTE) {
      /* Nothing reaches the first branch as of now: the translator refuses an
       * air.visible function itself, with "stage: air.visible, a library function (not
       * yet)", and never returns info for one. It is here because that refusal is the
       * frontend's own wording about its own limit, and the day the frontend translates
       * these without a wrapper this is the sentence a driver should get instead.
       */
      if (compute->info.stage == MESA_SHADER_NONE)
         say(why, why_size, "%s is an air.visible library function and not an entry "
                            "point, so it is not a compute pipeline: Metal stitches "
                            "these into a caller at pipeline time",
             compute->info.entry_point ? compute->info.entry_point : "the function");
      else
         /* the stage in words and not as its enumerator's name: this sentence ends up
          * in an NSError that an application shows a person
          */
         say(why, why_size, "%s is a %s shader, and this makes compute pipelines",
             compute->info.entry_point ? compute->info.entry_point : "the function",
             compute->info.stage >= MESA_SHADER_VERTEX &&
             compute->info.stage < MESA_SHADER_STAGES
                ? _mesa_shader_stage_to_string(compute->info.stage) : "non-compute");
      goto fail;
   }

   /* What the shader expects to be bound, counted the way Metal counts it: by the
    * highest index plus one, because the indices are the binding numbers and a kernel
    * using [[buffer(14)]] alone still needs fifteen slots to be reachable.
    */
   for (unsigned i = 0; i < compute->info.num_resources; i++) {
      const struct air_resource *r = &compute->info.resources[i];
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER:
         compute->caps.buffers = MAX2(compute->caps.buffers, r->index + 1);
         break;
      case AIR_RESOURCE_THREADGROUP_BUFFER:
         /* Metal sizes this one at dispatch, with
          * -setThreadgroupMemoryLength:atIndex:, and rwa_dispatch has no argument for
          * it. Recorded rather than refused here, so that a pipeline which is
          * perfectly compilable is not refused for the sake of a dispatch nobody has
          * asked for yet.
          */
         compute->dynamic_threadgroup = true;
         break;
      case AIR_RESOURCE_TEXTURE:
         compute->caps.textures = MAX2(compute->caps.textures, r->index + 1);
         /* and which slot it is, so that a dispatch can bind a sampler view here and
          * an image view at the next index: Metal gives a sampled texture and a
          * written one the same [[texture(n)]] index space, and a driver told only
          * "textures: 2" cannot tell which of the two it has.
          */
         if (r->index < RWA_MAX_TEXTURES) {
            compute->slots[r->index].sampled = true;
            compute->slots[r->index].read |= r->read;
            compute->slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &compute->slots[r->index]);
         }
         break;
      case AIR_RESOURCE_IMAGE:
         /* A texture the kernel writes. Metal gives it a [[texture(n)]] index like any
          * other, so it counts in the same total a driver has to fill; what differs is
          * that Gallium wants it as an image view and not a sampler view
          * (air_to_nir.c), which is why the two are counted apart as well.
          */
         compute->caps.textures = MAX2(compute->caps.textures, r->index + 1);
         if (r->index < RWA_MAX_TEXTURES) {
            compute->slots[r->index].stored = true;
            compute->slots[r->index].read |= r->read;
            compute->slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &compute->slots[r->index]);
         }
         break;
      case AIR_RESOURCE_SAMPLER:
         /* Only the ones the API has to bind. A `constexpr sampler` the shader declares
          * itself is already decoded (air_resource.sampler) and is nothing for a driver
          * to create; counting it here would tell the driver to bind a slot it must
          * leave alone.
          */
         if (!r->inline_sampler)
            compute->caps.samplers = MAX2(compute->caps.samplers, r->index + 1);
         break;
      }
   }

   slot_shapes(compute->nir, compute->slots, compute->compare_sampler);

   compute->caps.static_shared_bytes = compute->info.threadgroup_bytes;
   compute->caps.max_threads_per_group = device->max_threads_per_group;
   compute->caps.thread_execution_width = device->thread_execution_width;
   /* And which of those buffer indices are sets rather than arguments, so that a driver
    * knows which of its bound buffers to pass as rwa_dispatch_buffers' array.
    */
   if (argbuf_mask_of(&compute->info,
                      compute->info.entry_point ? compute->info.entry_point
                                                : "the function",
                      &compute->caps.argbuf_mask, why, why_size) != 0)
      goto fail;

   if (info)
      *info = compute->caps;
   return compute;

fail:
   ralloc_free(compute->mem_ctx);
   free(compute);
   return NULL;
}

/* Unbound before it is deleted, always. A Gallium context keeps a pointer to the state
 * bound in it, and softpipe asserts that the one being deleted is not that pointer
 * (sp_state_shader.c, softpipe_delete_compute_state) -- rightly, since the alternative
 * is a context holding freed memory until something else is bound. air_run never meets
 * this because a harness that runs one kernel and exits never deletes anything; a
 * library whose caller makes and drops pipelines all day meets it at once.
 *
 * Unbinding another pipeline's state in passing costs nothing: the device has one
 * context, and rwa_dispatch binds the state it is about to launch every time rather than
 * trusting what was bound before.
 */
static void
delete_compute_state(struct rwa_device *device, void *cso)
{
   device->pipe->bind_compute_state(device->pipe, NULL);
   device->pipe->delete_compute_state(device->pipe, cso);
}

void
rwa_compute_destroy(struct rwa_compute *compute)
{
   RWA_GUARD();
   if (!compute)
      return;
   if (compute->cso)
      delete_compute_state(compute->device, compute->cso);
   /* and the states built for the samplers the shader declared itself. Nothing is
    * unbound first, for the reason rwa_sampler_destroy gives: a dispatch leaves
    * nothing of its own bound.
    */
   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++)
      if (compute->own_sampler[i])
         compute->device->pipe->delete_sampler_state(compute->device->pipe,
                                                     compute->own_sampler[i]);
   ralloc_free(compute->mem_ctx);
   free(compute);
}

/* The compiled state for one threadgroup size, made on demand and kept.
 *
 * The size is baked in rather than left variable, and that is not optional: softpipe
 * reads the block size from TGSI_PROPERTY_CS_FIXED_BLOCK_*, and with a variable size
 * those are zero, so it runs ZERO threads and the output buffer comes back exactly as
 * it went in. That looked like a kernel that did nothing (docs/air.md section 7.4). It
 * is also the only path with evidence behind it: every number in docs/air.md was
 * measured through a fixed size, so this does what has been measured and not what the
 * hardware might also accept.
 *
 * A clone, because a Gallium driver takes the NIR it is given -- ir3 lowers I/O in it
 * and then steals it into the shader object -- and because the passes below fold the
 * threadgroup size into the code, so the shader as translated has to stay untouched for
 * the next size.
 */
static void *
compute_state_for(struct rwa_compute *compute, const unsigned block[3],
                  char *why, size_t why_size)
{
   struct rwa_device *device = compute->device;

   if (compute->cso && compute->cso_block[0] == block[0] &&
       compute->cso_block[1] == block[1] && compute->cso_block[2] == block[2])
      return compute->cso;

   if (compute->cso) {
      delete_compute_state(device, compute->cso);
      compute->cso = NULL;
   }

   nir_shader *cs = nir_shader_clone(NULL, compute->nir);
   if (!cs) {
      say(why, why_size, "out of memory compiling the shader");
      return NULL;
   }
   cs->info.workgroup_size[0] = block[0];
   cs->info.workgroup_size[1] = block[1];
   cs->info.workgroup_size[2] = block[2];
   cs->info.workgroup_size_variable = false;

   finalize_for_gallium(cs, device->screen);

   void *cso = device->pipe->create_compute_state(device->pipe,
      &(struct pipe_compute_state){
         .ir_type = PIPE_SHADER_IR_NIR, .prog = cs,
         .static_shared_mem = cs->info.shared_size });
   if (!cso) {
      /* The clone is not freed here: a driver that refused it may or may not have taken
       * it already, and neither of the two drivers in this build can reach this at all
       * (both return a state object and report a bad shader later, at launch).
       */
      say(why, why_size, "%s would not make a compute pipeline of %s",
          device->screen->get_name(device->screen),
          compute->info.entry_point ? compute->info.entry_point : "the shader");
      return NULL;
   }

   /* What the compiled variant really takes, where the driver will say: this is the
    * hardware's number and not the screen's guess, because it is read off the variant
    * ir3 produced (fd6_get_compute_state_info -> threadsize_base * max_waves, doubled
    * for a double-wide variant). Checked rather than reported, since the pipeline has
    * already told its caller what it believed at creation.
    */
   if (device->pipe->get_compute_state_info) {
      struct pipe_compute_state_object_info si = { 0 };
      device->pipe->get_compute_state_info(device->pipe, cso, &si);
      unsigned threads = block[0] * block[1] * block[2];
      if (si.max_threads && threads > si.max_threads) {
         say(why, why_size, "this dispatch asks for %u threads in a threadgroup and the "
                            "compiled shader takes at most %u: it uses too many "
                            "registers for a group that large", threads, si.max_threads);
         delete_compute_state(device, cso);
         return NULL;
      }
   }

   compute->cso = cso;
   compute->cso_block[0] = block[0];
   compute->cso_block[1] = block[1];
   compute->cso_block[2] = block[2];
   return cso;
}

/* Wait for the work, and say so when it does not come.
 *
 * This matters more than it looks. On 20 October a dispatch hung the GPU -- our own
 * driver said "gpu: no answer: submission 2 is the last, the core has done 1" -- and
 * every dispatch for the rest of that boot was accepted, returned, and did nothing.
 * air_run did not wait on a fence, so it printed the buffer it had filled itself as
 * though the GPU had answered: 21 of the 47 kernels of one run were vacuous that way
 * before anyone noticed (docs/air.md section 5). A driver that returned such a buffer
 * to an application would be worse off than air_run, because nobody is reading the
 * numbers.
 *
 * `required` is the difference between the two callers. After a launch_grid there is
 * work outstanding, so a driver that hands over no fence has left nobody able to tell
 * whether the GPU finished, and that is a failure. rwa_finish may be called with
 * nothing outstanding at all, and then no fence means there was nothing to wait for.
 */
static int
finish_or_say_why(struct rwa_device *device, bool required, const char *what,
                  char *why, size_t why_size)
{
   struct pipe_fence_handle *fence = NULL;
   device->pipe->flush(device->pipe, &fence, 0);
   if (!fence) {
      if (!required)
         return 0;
      say(why, why_size, "the GPU driver gave no fence for the %s, so nothing can tell "
                         "whether it finished; a dispatch nobody waited for is not a "
                         "result", what);
      return -1;
   }

   bool done = device->screen->fence_finish(device->screen, NULL, fence,
                                            RWA_FENCE_TIMEOUT_NS);
   device->screen->fence_reference(device->screen, &fence, NULL);
   if (!done) {
      say(why, why_size, "the GPU did not finish the %s within two seconds. Anything in "
                         "the buffers is what was put there, not what the shader "
                         "computed. The likeliest reason is the shader's input: a "
                         "kernel whose buffers carry counts, indices or sentinels, "
                         "given values it was never written for, loops on a condition "
                         "that never comes -- both kernels measured this way turned out "
                         "to be exactly that (docs/air.md section 5). And one timeout "
                         "wedges this GPU for the rest of the boot, so every later "
                         "dispatch in it is vacuous too", what);
      return -1;
   }
   return 0;
}

/* What the shader calls a texture slot, in the words a Metal programmer wrote it in,
 * so that a refusal about a mismatch names both sides in one vocabulary.
 */
static const char *
shader_texture_name(enum glsl_sampler_dim dim, bool is_array)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:   return is_array ? "texture1d_array" : "texture1d";
   case GLSL_SAMPLER_DIM_2D:   return is_array ? "texture2d_array" : "texture2d";
   case GLSL_SAMPLER_DIM_3D:   return "texture3d";
   case GLSL_SAMPLER_DIM_CUBE: return is_array ? "texturecube_array" : "texturecube";
   case GLSL_SAMPLER_DIM_MS:   return is_array ? "texture2d_ms_array" : "texture2d_ms";
   case GLSL_SAMPLER_DIM_BUF:  return "texture_buffer";
   default:                    return "a texture of a kind with no Metal name";
   }
}

static const char *
base_type_name(nir_alu_type type)
{
   switch (type) {
   case nir_type_int:   return "int";
   case nir_type_uint:  return "uint";
   case nir_type_float: return "float";
   default:             return "a type with no Metal name";
   }
}

/* The slot's kind as MTLTextureType, which is the number rwa_texture_shape.type takes.
 *
 * One table, read by the two callers that must agree: texture_fits, which refuses a
 * bound texture whose type is not this number, and rwa_module_textures, which reports
 * this number so a caller can ask for a texture that fits. The mismatch the sweep in
 * userland/src/metal_drv_test.m measured came from a caller having no way to ask; two
 * tables pointing opposite ways would have been the next way to get it wrong.
 *
 * False when the dimension has no Metal spelling at all -- a kind no AIR module can
 * declare, which is refused by name rather than compared against.
 */
static bool
metal_texture_type(enum glsl_sampler_dim dim, bool is_array, unsigned *out)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:
      *out = is_array ? RWA_TYPE_1D_ARRAY : RWA_TYPE_1D;
      return true;
   case GLSL_SAMPLER_DIM_2D:
      *out = is_array ? RWA_TYPE_2D_ARRAY : RWA_TYPE_2D;
      return true;
   case GLSL_SAMPLER_DIM_3D:
      *out = RWA_TYPE_3D;
      return true;
   case GLSL_SAMPLER_DIM_CUBE:
      *out = is_array ? RWA_TYPE_CUBE_ARRAY : RWA_TYPE_CUBE;
      return true;
   case GLSL_SAMPLER_DIM_MS:
      *out = is_array ? RWA_TYPE_2D_MULTISAMPLE_ARRAY : RWA_TYPE_2D_MULTISAMPLE;
      return true;
   case GLSL_SAMPLER_DIM_BUF:
      *out = RWA_TYPE_TEXTURE_BUFFER;
      return true;
   default:
      return false;
   }
}

/*
 * What element type the shader asks of a slot, and in whose words -- Apple's declaration
 * first, the translated instructions only where there is none.
 *
 * That order is the whole of the element check, and it is here rather than inline in it
 * because rwa_module_textures has to report the same answer the check will judge by: a
 * query that preferred the NIR would tell a caller to bind a format the check then
 * refuses. An image_store's src_type is a transport type -- "raw bits, the bound format
 * decides" -- so judging by it refuses every float or normalised image the corpus writes
 * (slot_shapes says what that cost); a txf's dest_type is a real statement and is the
 * fallback for a slot whose metadata carries no type name.
 *
 * False when neither exists, and that is the right way round: this exists to catch a
 * caller binding an integer texture where the shader reads floats, and a slot nothing
 * describes is a slot there are no grounds to refuse. `bits` may be NULL, and is 0 for
 * the NIR fallback, which carries the class and not the width.
 */
static bool
slot_element(const struct rwa_slot *s, nir_alu_type *type, const char **word,
             unsigned *bits)
{
   if (s->declared_element) {
      *type = s->declared_type;
      *word = s->declared_element;
      if (bits)
         *bits = s->declared_bits;
      return true;
   }
   if (s->type_known) {
      *type = s->type;
      *word = base_type_name(s->type);
      if (bits)
         *bits = 0;
      return true;
   }
   return false;
}

/*
 * Whether the texture the caller bound is the texture the shader declared.
 *
 * Metal checks this itself: a texture2d<float> parameter takes neither a 3D texture nor
 * an integer format, and a driver that let either through would be the lenient one. The
 * reason to check rather than trust is in docs/air.md section 5 -- with a 2D texture
 * bound where three shaders sample a 3D one, all three "passed", and only a real 4x4x4
 * texture turned their answers into evidence. A shape that is almost right is the
 * failure that reads like a result.
 */
static bool
texture_fits(const struct rwa_texture *t, const struct rwa_slot *s, const char *name,
             unsigned index, char *why, size_t why_size)
{
   if (!s->shape_known)
      return true;   /* a computed binding: nothing was declared to compare against */

   /*
    * A COMPARISON NEEDS A DEPTH TEXTURE, and this is Metal's own rule rather than a
    * caution: sample_compare is declared on depth2d and depth2d_array and on nothing
    * else, so a colour texture bound under one is a caller's mistake and Metal itself
    * catches it.
    *
    * It is checked here because the Adreno does not. The first device run of
    * tools/air/sampcheck.py's comparison cases put a shadow sample over an
    * r32g32b32a32_float texture, which is what the harness bound, and every case came
    * back with THE TEXEL -- 101 where a comparison must answer 0 or 1, and for
    * compare_func::always and compare_func::never too, which no compare function and no
    * clamped reference can explain. The chain above it was all correct and the device
    * log says so: ir3 emitted the shadow form (`samgq.s ... s#0, t#0`), air_run reported
    * `APPLIED (a shadow sample reads this slot)` for all 32, and fd6_sampler_state_create
    * writes A6XX_TEX_SAMP_1_COMPARE_FUNC whenever compare_mode is set. The texture unit
    * does the comparison in its depth path and a colour format never enters it.
    *
    * A shadow lookup that answers the texel instead of 0 or 1 is the exact shape of
    * wrongness this port refuses on principle, and the hardware will not say no, so the
    * library does.
    */
   if (s->shadow && !format_is_depth_stencil(t->res->format)) {
      say(why, why_size, "%s samples texture %u with a comparison and the texture bound "
                         "there is %s, which is a colour format: Metal declares "
                         "sample_compare on depth2d alone, and this GPU answers a "
                         "comparison of a colour texture with the texel instead of 0 or 1",
          name, index, t->format_name);
      return false;
   }

   /* Compared as MTLTextureType numbers, in the one direction metal_texture_type goes:
    * the bound texture's type is already Metal's own, so mapping the DECLARATION to
    * Metal rather than the binding to Gallium leaves one table instead of two, and that
    * table is the one rwa_module_textures reports from.
    */
   unsigned declared;
   if (!metal_texture_type(s->dim, s->is_array, &declared)) {
      say(why, why_size, "%s declares texture %u as %s, which has no MTLTextureType, so "
                         "there is no texture a caller could bind there", name, index,
          shader_texture_name(s->dim, s->is_array));
      return false;
   }
   if (declared != t->shape.type) {
      say(why, why_size, "%s declares texture %u as a %s and the texture bound there is a "
                         "%s", name, index, shader_texture_name(s->dim, s->is_array),
          texture_type_name(t->shape.type));
      return false;
   }

   /* The element type, which Metal writes into the shader's own type and the API into
    * the pixel format. Integer formats are exactly the ones a shader must read as
    * integers: the bits of a float read as a uint, or a uint fed through a format's
    * conversion, both come back as numbers. Which declaration counts, and why, is
    * slot_element's -- shared with the query so that what a caller is told to bind and
    * what it is refused for cannot be two different things.
    */
   nir_alu_type want = util_format_is_pure_uint(t->res->format) ? nir_type_uint
                     : util_format_is_pure_sint(t->res->format) ? nir_type_int
                                                                : nir_type_float;
   nir_alu_type have;
   const char *have_word;
   if (!slot_element(s, &have, &have_word, NULL))
      return true;
   if (want != have) {
      say(why, why_size, "%s reads texture %u as a %s<%s> and %s is %s format", name, index,
          shader_texture_name(s->dim, s->is_array), have_word, t->format_name,
          want == nir_type_float ? "a floating-point or normalised"
                                 : want == nir_type_uint ? "an unsigned integer"
                                                         : "a signed integer");
      return false;
   }
   return true;
}

/* The state for a texture slot no sampler reaches: nearest, clamped, in pixels.
 *
 * It is not a guess about what the shader wanted -- a slot gets this only when the
 * shader declared no sampler for it at all, which means every fetch from it is a Metal
 * read(), a txf that carries its own integer coordinate and ignores the sampler
 * entirely. What needs the state to exist is softpipe: set_shader_sampler() hands the
 * executor samplers[unit] for every unit the shader touches (sp_state_derived.c) and
 * the texture code dereferences it, so a NULL in a used slot is a crash and not a
 * default. These are also the values air_run binds in its compute path, so a number
 * measured here and a number measured there are comparable.
 */
static void *
default_sampler(struct rwa_device *device)
{
   if (!device->default_sampler) {
      device->default_sampler = device->pipe->create_sampler_state(device->pipe,
         &(struct pipe_sampler_state){
            .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .min_img_filter = PIPE_TEX_FILTER_NEAREST,
            .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
            .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
            .unnormalized_coords = true });
   }
   return device->default_sampler;
}

/*
 * Which Metal buffer indices a module declares ARGUMENT BUFFERS at, one bit each, and a
 * refusal when one is past the 32 a mask holds.
 *
 * A driver needs this for the reason it needs the vertex stream mask: Metal has one
 * buffer table per stage, an index in it is a stream, a buffer argument or a set, and
 * only the shader and the vertex descriptor together say which. Thirty-two is not a
 * narrowing in practice -- every module of both corpora carries
 * `air.max_device_buffers i32 31`, which is Metal's own limit on this device -- but a
 * set past it has to be named rather than dropped out of a mask, because an index
 * missing from this mask is an index a driver would hand over as a plain buffer.
 */
static int
argbuf_mask_of(const struct air_shader_info *info, const char *name, unsigned *mask,
               char *why, size_t why_size)
{
   *mask = 0;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];

      if (r->argbuf_set == ~0u)
         continue;
      if (r->argbuf_set >= 32) {
         say(why, why_size, "%s declares an argument buffer at Metal buffer index %u, and "
                            "this contract reports the first 32 indices", name,
             r->argbuf_set);
         return -1;
      }
      *mask |= 1u << r->argbuf_set;
   }
   return 0;
}

/*
 * Everything a stage declares has to be bound, of the right kind and big enough -- and
 * the refusals are the same sentences whether the stage is a dispatch's kernel or a
 * draw's fragment shader, so they are written once and pointed at a stage's own
 * resources and texture slots. The counterpart that actually binds is bind_stage below;
 * this one only decides whether it may.
 *
 * A stage dispatched with nothing in a slot it reads does not fail: it reads whatever
 * the slot holds and writes a plausible answer, which is the one outcome worse than an
 * error (docs/air.md section 4). The buffer sizes are checked against air.buffer_size,
 * which Apple's own metadata carries for most arguments -- a shader handed a buffer
 * shorter than it declares writes past the end of it, and on this GPU that is not a
 * fault but another shader's memory.
 */
static int
check_bindings(const struct air_shader_info *info, const struct rwa_slot *slots,
               const struct rwa_bindings *bindings,
               const struct rwa_buffer_bind *table, unsigned table_count,
               const char *name, char *why, size_t why_size)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];

      if (r->unused)
         continue;      /* specialized away: nothing need be bound (note_constant_reads) */

      /*
       * A MEMBER OF AN ARGUMENT BUFFER comes from the encoder's record and not from the
       * caller's arrays, and everything below this point is the same for both: the
       * member's own `index` is the Gallium slot either way, and what is checked about a
       * texture or a buffer does not depend on how the application named it.
       *
       * argbuf_member is where this feature's refusals live, the unencoded buffer among
       * them. A member resolved here is a resource of this device that exists.
       */
      struct rwa_buffer *mbuffer = NULL;
      struct rwa_texture *mtexture = NULL;
      size_t moffset = 0;
      bool member = r->argbuf_set != ~0u;

      if (member && argbuf_member(r, table, table_count, name, &mbuffer, &moffset,
                                  &mtexture, NULL, why, why_size) != 0)
         return -1;

      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER: {
         struct rwa_buffer *b = mbuffer;

         if (!member) {
            if (r->index >= bindings->buffer_count || !bindings->buffers[r->index]) {
               say(why, why_size, "%s reads buffer %u and %u buffer%s bound", name,
                   r->index, bindings->buffer_count,
                   bindings->buffer_count == 1 ? " was" : "s were");
               return -1;
            }
            b = bindings->buffers[r->index];
            /* METAL'S OWN `offset:` on a buffer argument, which this contract used to
             * have nowhere to put: an application that packs several uniform blocks
             * into one allocation and moves the offset per draw is the ordinary case,
             * and the window server of this port does it 55 times a boot.
             */
            char what[96];

            if (direct_buffer_offset(r, bindings, table, table_count, name, &moffset, why,
                                     why_size) != 0)
               return -1;
            snprintf(what, sizeof(what), "buffer %u of %s", r->index, name);
            if (buffer_offset_ok(b->device, r, b, moffset, name, what, why, why_size) != 0)
               return -1;
         }
         /* `r->size` is air.buffer_size, which a member never carries: the frontend
          * adds a member's resource with a size of zero because the nested descriptor
          * has no such key (air_to_nir.c's allocate_argbuf_indices). Where it IS
          * carried, what the shader can read has to be there AFTER the offset: a
          * buffer bound 1164 bytes in is 1164 bytes shorter for the shader than it is
          * for the application.
          */
         if (r->size && b->size - MIN2(b->size, moffset) < r->size) {
            say(why, why_size, "buffer %u is %zu bytes%s and %s declares it as %u",
                r->index, b->size - MIN2(b->size, moffset),
                moffset ? " after the offset it is bound at" : "", name, r->size);
            return -1;
         }
         break;
      }

      case AIR_RESOURCE_TEXTURE:
      case AIR_RESOURCE_IMAGE: {
         bool stored = r->kind == AIR_RESOURCE_IMAGE;
         if (r->index >= RWA_MAX_TEXTURES) {
            say(why, why_size, "%s binds a texture at index %u, past the %u slots this "
                               "library fills", name, r->index, RWA_MAX_TEXTURES);
            return -1;
         }
         if (!member && !stored && (r->index >= bindings->texture_count ||
                                    !bindings->textures[r->index]))
            break;      /* sampled from zeros: blank_view */
         if (!member && (r->index >= bindings->texture_count ||
                         !bindings->textures[r->index])) {
            say(why, why_size, "%s %s texture %u and %u texture%s bound", name,
                stored ? "writes" : "reads", r->index, bindings->texture_count,
                bindings->texture_count == 1 ? " was" : "s were");
            return -1;
         }
         const struct rwa_texture *t = member ? mtexture : bindings->textures[r->index];
         /* MTLTextureUsage, honoured rather than noted: Metal refuses to bind a
          * texture to a shader that writes it unless the application asked for
          * ShaderWrite, and a texture allocated without it may not even be laid out so
          * that a store can reach it.
          */
         if (stored && !t->shader_write) {
            say(why, why_size, "%s writes texture %u and that texture was made without "
                               "MTLTextureUsageShaderWrite", name, r->index);
            return -1;
         }
         if (!stored && !t->shader_read) {
            say(why, why_size, "%s samples texture %u and that texture was made without "
                               "MTLTextureUsageShaderRead", name, r->index);
            return -1;
         }
         if (!texture_fits(t, &slots[r->index], name, r->index, why, why_size))
            return -1;
         break;
      }

      case AIR_RESOURCE_SAMPLER:
         if (r->index >= RWA_MAX_TEXTURES) {
            say(why, why_size, "%s binds a sampler at index %u, past the %u slots this "
                               "library fills", name, r->index, RWA_MAX_TEXTURES);
            return -1;
         }
         /* A `constexpr sampler` the shader declares is nothing for the caller to bind
          * -- rwa_compute_info does not count it -- so this library builds it, below.
          */
         if (r->inline_sampler)
            break;
         /* A member's sampler was resolved above, and there is nothing further to check
          * about a sampler state: it is a created object of this device or the encoding
          * was refused.
          */
         if (member)
            break;
         /* an empty slot gets default_sampler in bind_stage, as Apple's GPU reads
          * something rather than fails; it goes with a texture that is not there either
          */
         break;

      case AIR_RESOURCE_THREADGROUP_BUFFER:
         break;   /* a dispatch refuses one before it reaches here; no draw stage has it */
      }
   }
   return 0;
}

/* What bind_stage bound, so unbind_stage can take it all back once the fence is in. The
 * buffers are not here on purpose: they are left bound and rebound by the next draw or
 * dispatch, because Gallium holds a reference on a bound resource -- it is the sampler
 * states and views, which it does not, that must not outlive the caller's next
 * rwa_sampler_destroy or rwa_texture_destroy.
 */
struct rwa_bound {
   unsigned num_ssbo, num_views, num_images, num_samplers;
};

/* A sampled slot nothing was bound to: a view of a 1x1 texture of zeros, of the target
 * the shader's instructions declare (a 2D one where they declare nothing) -- see
 * rwa_device.blank_view.
 */
static struct pipe_sampler_view *
blank_view(struct rwa_device *device, const struct rwa_slot *s)
{
   enum pipe_texture_target target = PIPE_TEXTURE_2D;
   bool depth = s && s->shape_known && s->shadow;

   if (s && s->shape_known) {
      switch (s->dim) {
      case GLSL_SAMPLER_DIM_1D:
         target = s->is_array ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
         break;
      case GLSL_SAMPLER_DIM_3D:
         target = PIPE_TEXTURE_3D;
         break;
      case GLSL_SAMPLER_DIM_CUBE:
         target = s->is_array ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
         break;
      default:
         target = s->is_array ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
         break;
      }
   }
   if (device->blank_view[target][depth])
      return device->blank_view[target][depth];

   struct pipe_context *pipe = device->pipe;
   unsigned layers = target == PIPE_TEXTURE_CUBE ? 6 : target == PIPE_TEXTURE_CUBE_ARRAY ? 6 : 1;
   struct pipe_resource tmpl = {
      .target = target,
      .format = depth ? PIPE_FORMAT_Z32_FLOAT : PIPE_FORMAT_R8G8B8A8_UNORM,
      .width0 = 1, .height0 = 1, .depth0 = 1, .array_size = layers,
      .bind = PIPE_BIND_SAMPLER_VIEW,
      .usage = PIPE_USAGE_DEFAULT,
   };
   struct pipe_resource *res = device->screen->resource_create(device->screen, &tmpl);
   if (!res)
      return NULL;
   static const uint32_t zeros[6] = { 0 };
   pipe->texture_subdata(pipe, res, 0, PIPE_MAP_WRITE,
                         &(struct pipe_box){ .width = 1, .height = 1, .depth = layers },
                         zeros, 4, 4);
   struct pipe_sampler_view vt = { 0 };
   u_sampler_view_default_template(&vt, res, res->format);
   device->blank_view[target][depth] = pipe->create_sampler_view(pipe, res, &vt);
   device->blank_res[target][depth] = res;
   return device->blank_view[target][depth];
}

/*
 * Bind one stage's resources, each as the kind its own instructions ask for, and record
 * what was bound. This is air_run's compute binding with the shader stage lifted out of
 * it, so the dispatch and a draw's fragment stage share it rather than keep two copies
 * that drift: a Metal [[buffer(n)]]/[[texture(n)]]/[[sampler(n)]] index means the same
 * thing in a kernel and in a fragment shader, and only the pipe_shader_type differs.
 *
 * `own_sampler` is the pipeline's cache of the samplers the shader declares for itself,
 * built once and kept; the caller passes the array that belongs to this stage.
 * `compare_sampler` is slot_shapes' answer for the same stage: which sampler slots a
 * shadow texture instruction reads, which is what decides, per slot, which of a
 * sampler's two state objects is bound.
 */
static int
bind_stage(struct rwa_device *device, enum pipe_shader_type stage,
           const struct air_shader_info *info, const struct rwa_slot *slots,
           void **own_sampler,
           const bool *compare_sampler,
           const struct rwa_bindings *bindings, const struct rwa_buffer_bind *table,
           unsigned table_count, const char *name, struct rwa_bound *bound, char *why,
           size_t why_size)
{
   struct pipe_context *pipe = device->pipe;

   /* Each buffer bound as whatever the shader declared the index to be. Metal has one
    * [[buffer(n)]] index space and Gallium has two, so a `constant` parameter is a UBO
    * at n and a `device` one is an SSBO at n, and binding a constant buffer as an SSBO
    * would leave the shader reading an unbound slot.
    */
   struct pipe_shader_buffer sb[PIPE_MAX_SHADER_BUFFERS] = { 0 };
   unsigned num_ssbo = 0, writable = 0;

   /* and each texture as the kind of view its own instructions ask for. A read-only
    * texture stays a texture: Metal's read() on one becomes a txf, a texture fetch, so
    * it needs a sampler view even inside a compute shader, while a written one wants
    * an image view (air_to_nir.c, air_run.c's texture_shapes). BlitKernel_Unorm has
    * one of each, and binding only the image left its fetch reading nothing.
    */
   struct pipe_sampler_view *views[RWA_MAX_TEXTURES] = { 0 };
   struct pipe_image_view images[RWA_MAX_TEXTURES] = { 0 };
   void *samplers[RWA_MAX_TEXTURES] = { 0 };
   unsigned num_views = 0, num_images = 0, num_samplers = 0;

   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      struct rwa_buffer *b = NULL;
      struct rwa_texture *t = NULL;
      struct rwa_sampler *sm = NULL;
      size_t boffset = 0;

      if (r->unused)
         continue;

      /* A member of an argument buffer comes from the encoder's record; everything
       * below binds it exactly as a direct argument, at the index the frontend gave it.
       * check_bindings has already resolved every member once and refused what could
       * not be -- this cannot fail where that succeeded, and it is asked again rather
       * than carried between the two because a lookup is cheaper than a table that
       * could disagree with itself.
       */
      if (r->argbuf_set != ~0u) {
         if (argbuf_member(r, table, table_count, name, &b, &boffset, &t, &sm, why,
                           why_size) != 0)
            return -1;
      } else {
         switch (r->kind) {
         case AIR_RESOURCE_DEVICE_BUFFER:
         case AIR_RESOURCE_CONSTANT_BUFFER:
            b = bindings->buffers[r->index];
            /* and Metal's own offset on it, which check_bindings has already judged */
            if (direct_buffer_offset(r, bindings, table, table_count, name, &boffset, why,
                                     why_size) != 0)
               return -1;
            break;
         case AIR_RESOURCE_TEXTURE:
         case AIR_RESOURCE_IMAGE:
            t = r->index < bindings->texture_count ? bindings->textures[r->index] : NULL;
            break;
         case AIR_RESOURCE_SAMPLER:
            if (!r->inline_sampler && r->index < bindings->sampler_count)
               sm = bindings->samplers[r->index];
            break;
         default:
            break;
         }
      }

      switch (r->kind) {
      case AIR_RESOURCE_DEVICE_BUFFER:
         if (r->index >= ARRAY_SIZE(sb)) {
            say(why, why_size, "%s binds a buffer at index %u, past the %u Gallium has "
                               "slots for", name, r->index, (unsigned)ARRAY_SIZE(sb));
            return -1;
         }
         sb[r->index].buffer = b->res;
         /* Metal's own `offset:`, for a member of a set and for a buffer argument
          * alike, and an offset this GPU cannot fetch from has already been refused by
          * name (buffer_offset_ok: a `device` buffer may be written, so the upload that
          * serves a `constant` one would lose the writes). The SIZE is what is left of
          * the buffer after the offset, so a shader reading past its own block reads
          * past its binding rather than into the next one.
          */
         sb[r->index].buffer_offset = (unsigned)boffset;
         sb[r->index].buffer_size = b->size - MIN2(b->size, boffset);
         num_ssbo = MAX2(num_ssbo, r->index + 1);
         if (r->write)
            writable |= 1u << r->index;
         break;

      case AIR_RESOURCE_CONSTANT_BUFFER: {
         if (r->index >= PIPE_MAX_CONSTANT_BUFFERS) {
            say(why, why_size, "%s binds a constant buffer at index %u, past the %u "
                               "Gallium has slots for", name, r->index,
                (unsigned)PIPE_MAX_CONSTANT_BUFFERS);
            return -1;
         }
         size_t left = b->size - MIN2(b->size, boffset);
         /*
          * AN OFFSET THIS GPU CANNOT FETCH FROM, served rather than refused.
          *
          * Metal allows any multiple of 4 and this screen wants more -- 64 on a6xx, 16
          * on softpipe -- and a Gallium driver programs the register with the offset it
          * is given, so passing an unaligned one would read the wrong block of the
          * right buffer. The answer is Gallium's own: a `user_buffer` is a pointer the
          * DRIVER uploads itself, and both back ends do (freedreno's upload_user_buffer
          * through u_upload_data at an alignment of 64, softpipe by wrapping the
          * pointer). The buffer is mapped for its whole life here, so the pointer is
          * simply the mapping plus the offset.
          *
          * What is uploaded is `air.buffer_size` where Apple's metadata carries one,
          * which is all the shader is able to read; the rest of the buffer otherwise.
          * A 48-byte uniform block 1164 bytes into a megabyte of uniforms costs 48
          * bytes a draw, which is what the window server of this port asks for.
          */
         struct pipe_constant_buffer cb = {
            .buffer_offset = (unsigned)boffset,
            .buffer_size = (unsigned)left,
         };
         /*
          * Always the mapping since 26 October, aligned or not: panfrost pushes a uniform it can
          * read on the processor (a user buffer's) into the shader's own registers, and loads any
          * other from memory for every pixel -- CoreAnimation's uniforms are in buffers 1 and up,
          * and its passes did up to 17 such loads a pixel, 12 to 17 ms of the Mali a pass
          * (pan_shader.c pushable_ubos). A buffer without a mapping still goes as itself.
          */
         if (b->map == NULL) {
            cb.buffer = b->res;
         } else {
            size_t want = r->size && r->size <= left ? r->size : left;
            /* and no more than the compiled shader reads (note_constant_reads) */
            if (r->reads && r->reads < want)
               want = r->reads;

            cb.user_buffer = (const char *)b->map + boffset;
            cb.buffer_offset = 0;
            cb.buffer_size = (unsigned)want;
         }
         pipe->set_constant_buffer(pipe, stage, r->index, false, &cb);
         break;
      }

      case AIR_RESOURCE_TEXTURE:
         if (!t) {
            /* check_bindings let an empty sampled slot through: zeros, as on Apple's GPU */
            views[r->index] = blank_view(device, slots ? &slots[r->index] : NULL);
            if (!views[r->index]) {
               say(why, why_size, "%s would not make a blank texture for slot %u",
                   rwa_device_name(device), r->index);
               return -1;
            }
            num_views = MAX2(num_views, r->index + 1);
            break;
         }
         /* The view is made once and kept with the texture: a device has one context,
          * so a view of this texture is a view for every pipeline that will sample it.
          */
         if (!t->view) {
            struct pipe_sampler_view tmpl = { 0 };
            u_sampler_view_default_template(&tmpl, t->res, t->res->format);
            t->view = pipe->create_sampler_view(pipe, t->res, &tmpl);
            if (!t->view) {
               say(why, why_size, "%s would not make a sampler view of the %s bound at "
                                  "texture %u", rwa_device_name(device), t->format_name,
                   r->index);
               return -1;
            }
         }
         views[r->index] = t->view;
         num_views = MAX2(num_views, r->index + 1);
         break;

      case AIR_RESOURCE_IMAGE:
         images[r->index] = (struct pipe_image_view){
            .resource = t->res,
            .format = t->res->format,
            /* what the shader does with it, from the metadata rather than assumed: a
             * read-write image told it is write-only reads zeros on some drivers.
             */
            .access = (r->read ? PIPE_IMAGE_ACCESS_READ : 0) |
                      (r->write ? PIPE_IMAGE_ACCESS_WRITE : 0),
            .shader_access = (r->read ? PIPE_IMAGE_ACCESS_READ : 0) |
                             (r->write ? PIPE_IMAGE_ACCESS_WRITE : 0),
            .u.tex.level = 0,
            .u.tex.first_layer = 0,
            .u.tex.last_layer = (t->shape.type == RWA_TYPE_3D ? t->res->depth0
                                                              : t->layers) - 1,
         };
         num_images = MAX2(num_images, r->index + 1);
         break;

      case AIR_RESOURCE_SAMPLER:
         if (r->inline_sampler) {
            /* The shader's own word, built once and kept with the pipeline. It goes
             * through Metal's vocabulary on the way (sampler_state_from_air), which is
             * what makes this sampler and one a driver creates from
             * MTLSamplerDescriptor the same object -- the argument of docs/air.md 3.3
             * and of air_metal.h.
             */
            bool compares = compare_sampler && compare_sampler[r->index];
            if (!own_sampler[r->index]) {
               struct rwa_sampler_state as_metal;
               struct pipe_sampler_state st;
               if (!sampler_state_from_air(&r->sampler, r->index, &as_metal, why, why_size) ||
                   !sampler_state_from_metal(&as_metal, &st, why, why_size))
                  return -1;
               /* One state object and not two, unlike an API sampler: this word
                * belongs to this shader, so whether a shadow instruction reads it is
                * already known and the other object would never be bound.
                */
               if (compares) {
                  /* `none` is the one value of the word that has no
                   * MTLCompareFunction, and it reached as_metal as Never --
                   * MTLSamplerDescriptor's default, which is right for the plain
                   * sample that is all either can do. A comparison is where they part
                   * company: AGX keeps its NONE and its NEVER apart, and what it does
                   * differently is not readable from here, so a shadow instruction
                   * through such a sampler is refused rather than compared with a
                   * function Apple spells differently (sampler_state_from_air).
                   */
                  if (r->sampler.compare_func == 0) {
                     say(why, why_size, "%s samples sampler %u with a comparison and its "
                                        "own word says compare_func::none, which is not "
                                        "one of MTLCompareFunction's eight: a sampler "
                                        "that was not made to compare", name, r->index);
                     return -1;
                  }
                  st.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
               }
               own_sampler[r->index] = pipe->create_sampler_state(pipe, &st);
               if (!own_sampler[r->index]) {
                  say(why, why_size, "%s would not make the sampler %s declares at index %u",
                      rwa_device_name(device), name, r->index);
                  return -1;
               }
            }
            samplers[r->index] = own_sampler[r->index];
         } else {
            /* Metal's sampler, and the instruction's choice of which of its two state
             * objects: struct rwa_sampler.
             */
            if (!sm)
               break;   /* left empty: default_sampler, below */
            samplers[r->index] = (compare_sampler && compare_sampler[r->index])
                                    ? sm->cso_compare : sm->cso;
         }
         num_samplers = MAX2(num_samplers, r->index + 1);
         break;

      case AIR_RESOURCE_THREADGROUP_BUFFER:
         break;
      }
   }

   /* A sampler in every slot a sampler view went into, whether a sampler was declared
    * for it or not: see default_sampler() -- softpipe dereferences the state of every
    * unit the shader touches, so a slot left empty is a crash rather than a default.
    */
   for (unsigned i = 0; i < num_views; i++) {
      if (!views[i] || samplers[i])
         continue;
      samplers[i] = default_sampler(device);
      if (!samplers[i]) {
         say(why, why_size, "%s would not make a sampler at all", rwa_device_name(device));
         return -1;
      }
      num_samplers = MAX2(num_samplers, i + 1);
   }

   if (num_ssbo)
      pipe->set_shader_buffers(pipe, stage, 0, num_ssbo, sb, writable);
   if (num_views)
      pipe->set_sampler_views(pipe, stage, 0, num_views, 0, false, views);
   if (num_images)
      pipe->set_shader_images(pipe, stage, 0, num_images, 0, images);
   if (num_samplers)
      pipe->bind_sampler_states(pipe, stage, 0, num_samplers, samplers);

   bound->num_ssbo = num_ssbo;
   bound->num_views = num_views;
   bound->num_images = num_images;
   bound->num_samplers = num_samplers;
   return 0;
}

/* Take back the views, images and samplers once the fence is in, which is why it can be
 * done at all: the caller's next call may be rwa_texture_destroy or rwa_sampler_destroy,
 * and a Gallium context that still holds a sampler state is holding a pointer the next
 * delete_sampler_state frees -- softpipe's is a plain FREE (sp_state_sampler.c). The
 * buffers are left bound (see struct rwa_bound).
 */
static void
unbind_stage(struct rwa_device *device, enum pipe_shader_type stage,
             const struct rwa_bound *bound)
{
   struct pipe_context *pipe = device->pipe;
   if (bound->num_views)
      pipe->set_sampler_views(pipe, stage, 0, 0, bound->num_views, false, NULL);
   if (bound->num_images)
      pipe->set_shader_images(pipe, stage, 0, 0, bound->num_images, NULL);
   if (bound->num_samplers) {
      void *none[RWA_MAX_TEXTURES] = { 0 };
      pipe->bind_sampler_states(pipe, stage, 0, bound->num_samplers, none);
   }
}

int
rwa_dispatch(struct rwa_compute *compute, const struct rwa_bindings *bindings,
             const unsigned groups[3], const unsigned group_size[3],
             char *why, size_t why_size)
{
   RWA_GUARD();
   /* No argument buffers, which is what every caller written before they existed means.
    * A kernel that declares one is refused by argbuf_member, naming the Metal buffer
    * index it wanted a set at and that nothing was given.
    */
   return rwa_dispatch_buffers(compute, bindings, NULL, 0, groups, group_size, why,
                               why_size);
}

int
rwa_dispatch_buffers(struct rwa_compute *compute, const struct rwa_bindings *bindings,
                     const struct rwa_buffer_bind *table, unsigned table_count,
                     const unsigned groups[3], const unsigned group_size[3],
                     char *why, size_t why_size)
{
   RWA_GUARD();
   struct rwa_device *device = compute->device;
   struct pipe_context *pipe = device->pipe;
   const char *name = compute->info.entry_point ? compute->info.entry_point : "the shader";

   /* A caller with nothing to bind may say so with NULL rather than with a zeroed
    * struct. A shader that declares anything is then refused by the counts below,
    * which is the same answer in the same words.
    */
   static const struct rwa_bindings nothing = { 0 };
   if (!bindings)
      bindings = &nothing;

   for (unsigned i = 0; i < 3; i++) {
      if (groups[i] == 0 || group_size[i] == 0) {
         /* Zero threads is the trap air_run walked into from the other side: a grid
          * that runs nothing leaves every buffer exactly as it was, which reads like a
          * kernel that computed nothing rather than like a dispatch that never ran.
          */
         say(why, why_size, "a grid of %ux%ux%u threadgroups of %ux%ux%u threads has no "
                            "threads in it, so it would leave every buffer as it is",
             groups[0], groups[1], groups[2],
             group_size[0], group_size[1], group_size[2]);
         return -1;
      }
   }

   unsigned threads = group_size[0] * group_size[1] * group_size[2];
   if (compute->caps.max_threads_per_group &&
       threads > compute->caps.max_threads_per_group) {
      say(why, why_size, "%ux%ux%u is %u threads in one threadgroup and this GPU takes "
                         "at most %u", group_size[0], group_size[1], group_size[2],
          threads, compute->caps.max_threads_per_group);
      return -1;
   }

   if (compute->dynamic_threadgroup) {
      say(why, why_size, "%s takes a threadgroup buffer whose size only the dispatch can "
                         "give (Metal's -setThreadgroupMemoryLength:atIndex:), and this "
                         "call has no argument for it", name);
      return -1;
   }

   /* Everything the kernel declares has to be bound, of the right kind and big enough.
    * The same check, in the same words, guards a draw's stages -- check_bindings.
    */
   if (check_bindings(&compute->info, compute->slots, bindings, table, table_count,
                      name, why, why_size) != 0)
      return -1;

   void *cso = compute_state_for(compute, group_size, why, why_size);
   if (!cso)
      return -1;
   pipe->bind_compute_state(pipe, cso);

   /* The binding is bind_stage's, shared with the fragment stage of a draw. On a
    * failure mid-bind nothing has been set yet (the set_* calls are at its end), so
    * there is nothing to take back; the compute state stays bound and is replaced by
    * the next dispatch or dropped at delete, as before.
    */
   struct rwa_bound bound = { 0 };
   if (bind_stage(device, PIPE_SHADER_COMPUTE, &compute->info, compute->slots,
                  compute->own_sampler,
                  compute->compare_sampler,
                  bindings, table, table_count, name, &bound, why, why_size) != 0)
      return -1;

   struct pipe_grid_info grid = {
      .work_dim = 3,
      .block = { group_size[0], group_size[1], group_size[2] },
      .grid = { groups[0], groups[1], groups[2] },
   };
   pipe->launch_grid(pipe, &grid);

   /* Waited for here, and not left to rwa_finish, because this is the call that can
    * say which dispatch did not come back -- and because the buffers are mapped the
    * whole time, so a caller that read them before the fence would read what it wrote
    * and call it a result. rwa_finish is for a caller that wants to wait again, or to
    * wait for work dispatched through another pipeline of the same device.
    */
   int rc = finish_or_say_why(device, true, "dispatch", why, why_size);
   unbind_stage(device, PIPE_SHADER_COMPUTE, &bound);
   return rc;
}

/*
 * The work so far sent off, not waited for: the number of the last job it made (0 without the
 * Mali's job queue), for rwa_wait_seq -- a command buffer of the window server's waits no longer,
 * its completion handlers do and the display does (MiniIOSurfaceDrawn).
 */
extern uint64_t redwood_drm_last_seq(void) __attribute__((weak_import));
extern int redwood_drm_wait_seq(uint64_t seq, int64_t relative_ns) __attribute__((weak_import));

/* panfrost's batch submissions since the last call: count and nanoseconds */
extern uint64_t redwood_pan_submit_ns, redwood_pan_submits, redwood_pan_blend_compiles __attribute__((weak_import));
extern uint64_t redwood_pan_variant_compiles, redwood_pan_variant_ns __attribute__((weak_import));

void
rwa_submit_profile(uint64_t out[5])
{
   {
      char line[200];

      if (rwa_pipelines_made) {
         snprintf(line, sizeof(line), "!rwm pipelines: %llu made, %llu ms translating, %llu ms compiling",
                  (unsigned long long)rwa_pipelines_made, (unsigned long long)(rwa_pipelines_translate_ns / 1000000),
                  (unsigned long long)(rwa_pipelines_compile_ns / 1000000));
         sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
         rwa_pipelines_made = rwa_pipelines_translate_ns = rwa_pipelines_compile_ns = 0;
      }
   }
   out[3] = &redwood_pan_variant_compiles ? redwood_pan_variant_compiles : 0;
   out[4] = &redwood_pan_variant_ns ? redwood_pan_variant_ns : 0;
   if (&redwood_pan_variant_compiles) {
      redwood_pan_variant_compiles = 0;
      redwood_pan_variant_ns = 0;
   }
   out[0] = &redwood_pan_submits ? redwood_pan_submits : 0;
   out[1] = &redwood_pan_submit_ns ? redwood_pan_submit_ns : 0;
   out[2] = &redwood_pan_blend_compiles ? redwood_pan_blend_compiles : 0;
   if (&redwood_pan_submits) {
      redwood_pan_submits = 0;
      redwood_pan_submit_ns = 0;
      redwood_pan_blend_compiles = 0;
   }
}

uint64_t
rwa_flush_seq(struct rwa_device *device)
{
   RWA_GUARD();
   if (!redwood_drm_last_seq)
      return 0;
   device->pipe->flush(device->pipe, NULL, 0);
   return redwood_drm_last_seq();
}

extern int redwood_drm_surface_seq(uint32_t surface, uint64_t seq) __attribute__((weak_import));

/* a surface's last job for the display, or ~0 for "drawn on another thread, not yet sent" */
int
rwa_surface_seq(uint32_t surface, uint64_t seq)
{
   return redwood_drm_surface_seq ? redwood_drm_surface_seq(surface, seq) : 0;
}

/* (no lock: it is a wait in the kernel, and nothing of the Gallium context is touched) */
int
rwa_wait_seq(uint64_t seq, int64_t timeout_ns)
{
   return redwood_drm_wait_seq ? redwood_drm_wait_seq(seq, timeout_ns) : 0;
}


int
rwa_finish(struct rwa_device *device, char *why, size_t why_size)
{
   RWA_GUARD();
   return finish_or_say_why(device, false, "work", why, why_size);
}

/*
 * ---------------------------------------------------------------------------------------
 * Drawing.
 *
 * air_run draws every fragment shader of the corpus and 255 of them have been compared
 * pixel by pixel (docs/air.md): a quad vertex shader built by hand in NIR plus the real
 * fragment shader, a framebuffer of plain textures, clear, draw_vbo of a TRIANGLE_STRIP
 * of four, and the pixel read back. This is that path as the three contract calls, and
 * the code and its WHY comments are lifted from air_run.c because each is a mistake
 * someone already made.
 *
 * Where it differs from a dispatch is only the stage: a draw binds the fragment shader's
 * resources with the very same bind_stage the dispatch uses, and when a vertex module is
 * supplied binds its resources too. The reconnaissance that fixed the contract's shape
 * is docs/metal.md 10.18 -- nothing in Apple's Metal reads MTLRenderPassDescriptor, the
 * render target is a plain texture, and the render path adds no kernel ABI over compute.
 */

/* A full-screen quad covering the whole attachment, in clip space. A TRIANGLE_STRIP of
 * these four is what air_run draws and what rwa_render_draw expects when the library
 * supplied the vertex stage.
 */
static const float rwa_quad_verts[] = {
   -1, -1, 0, 1,   1, -1, 0, 1,   -1, 1, 0, 1,   1, 1, 0, 1,
};

/* The texel the generated vertex stage puts under every fragment, as air_run does
 * (TEX_SIZE/TEX_X/TEX_Y there). It matters only to a fragment shader that samples a
 * texture: there is no real vertex stage to interpolate a coordinate, so one constant
 * coordinate is handed to every fragment, and a shader reading a texture reads the same
 * texel at every pixel. The value is air_run's exactly, so a pixel drawn here and one
 * drawn there are the same measurement; a driver that wants interpolated coordinates
 * across real geometry needs a real vertex shader, which is the vertex_bitcode path.
 */
#define RWA_TEX_SIZE 4
#define RWA_TEX_X 1
#define RWA_TEX_Y 1

/* A vertex shader for the quad: it takes the position as attribute 0 and passes it
 * through. Written here rather than translated, because what is under test is the
 * fragment shader; when a .metallib has a vertex shader of its own that is the
 * vertex_bitcode path and nothing is generated. This is air_run.c's quad_vertex_shader.
 */
static nir_shader *
quad_vertex_shader(const struct nir_shader_compiler_options *options, unsigned num_varyings)
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

   /* One constant coordinate per varying the fragment shader reads: the centre of texel
    * (TEX_X, TEX_Y), so every fragment samples the same texel and the expected value is
    * a single known one.
    */
   float c = (RWA_TEX_X + 0.5f) / RWA_TEX_SIZE;
   float d = (RWA_TEX_Y + 0.5f) / RWA_TEX_SIZE;
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

/* MTLPrimitiveType in Gallium's terms. The names are Metal's own enumeration. */
static bool
mesa_prim_from_metal(unsigned p, enum mesa_prim *out, char *why, size_t why_size)
{
   switch (p) {
   case 0: *out = MESA_PRIM_POINTS;         return true;
   case 1: *out = MESA_PRIM_LINES;          return true;
   case 2: *out = MESA_PRIM_LINE_STRIP;     return true;
   case 3: *out = MESA_PRIM_TRIANGLES;      return true;
   case 4: *out = MESA_PRIM_TRIANGLE_STRIP; return true;
   default:
      say(why, why_size, "primitive type %u is not one of MTLPrimitiveType's five", p);
      return false;
   }
}

/*
 * MTLIndexType in bytes. Metal has exactly two values, UInt16 = 0 and UInt32 = 1
 * (MTLStageInputOutputDescriptor.h), and there is no third: an eight-bit index type
 * exists on both of these back ends (`pc_di_index_size` has INDEX_SIZE_8_BIT, and the
 * draw module switches on eltSize 1) and in Metal it does not, so a value this does not
 * know is refused by name rather than guessed into a size. The number is in the refusal
 * because that is the one thing a caller whose struct layout does not match this
 * library's will show here (see rwa_draw's own comment).
 */
static bool
index_size_from_metal(unsigned type, unsigned *out, char *why, size_t why_size)
{
   switch (type) {
   case 0: *out = 2; return true;   /* MTLIndexTypeUInt16 */
   case 1: *out = 4; return true;   /* MTLIndexTypeUInt32 */
   default:
      say(why, why_size, "index type %u is neither of MTLIndexType's two values "
                         "(MTLIndexTypeUInt16 = 0, MTLIndexTypeUInt32 = 1)", type);
      return false;
   }
}

/* The name of an index type, for the sentences below. */
static const char *
index_type_name(unsigned type)
{
   return type == 0 ? "MTLIndexTypeUInt16" : "MTLIndexTypeUInt32";
}

/*
 * The smallest and largest index the draw will read, which has to be walked rather than
 * assumed, and this is the only reader of the index buffer on the processor.
 *
 * Mesa does the same walk in the same place and with the same convention
 * (u_vbuf_get_minmax_index: `count` elements from offset start * index_size, and the
 * values are the raw indices with no index_bias added), so these numbers mean what a
 * Gallium driver's min_index/max_index mean. Gallium's own helper for an indexed draw,
 * util_draw_elements, instead leaves index_bounds_valid false with max_index 0xffffffff,
 * which is legal and is what the draw module falls back to (draw_pt.c: min 0, max ~0);
 * walking is still worth the pass, for two reasons that have nothing to do with speed:
 * the generated vertex stage has a vertex buffer of exactly four elements and an index
 * past it must be refused rather than clamped, and the bounds let a driver's fetch stay
 * inside what it was told.
 *
 * The cost is honest and it is O(count) on the processor at every draw: an application
 * drawing a hundred thousand indices pays for a hundred thousand loads here. Mesa pays
 * the same cost in u_vbuf and caches it per buffer; nothing is cached here yet, and the
 * draw already waits for the GPU on this thread (rwa_render_draw's fence), which is the
 * larger of the two by far.
 */
static void
index_range(const uint8_t *indices, unsigned count, unsigned index_size, unsigned *min_out,
            unsigned *max_out)
{
   unsigned min = UINT32_MAX, max = 0;
   for (unsigned i = 0; i < count; i++) {
      unsigned v;
      if (index_size == 2) {
         uint16_t u;
         memcpy(&u, indices + (size_t)i * 2, sizeof(u));
         v = u;
      } else {
         uint32_t u;
         memcpy(&u, indices + (size_t)i * 4, sizeof(u));
         v = u;
      }
      if (v < min)
         min = v;
      if (v > max)
         max = v;
   }
   *min_out = min;
   *max_out = max;
}

/* What a stage expects to be bound, counted the way Metal counts it: by the highest
 * index plus one, because the indices are the binding numbers. This is rwa_compute_create's
 * own counting, pointed at a stage's resources, and it fills in the texture slots the
 * same way (sampled vs stored, read, write) so texture_fits has something to check a
 * bound texture against.
 */
static void
count_resources(const struct air_shader_info *info, struct rwa_slot slots[RWA_MAX_TEXTURES],
                unsigned *buffers, unsigned *textures, unsigned *samplers)
{
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER:
         *buffers = MAX2(*buffers, r->index + 1);
         break;
      case AIR_RESOURCE_TEXTURE:
         *textures = MAX2(*textures, r->index + 1);
         if (r->index < RWA_MAX_TEXTURES) {
            slots[r->index].sampled = true;
            slots[r->index].read |= r->read;
            slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &slots[r->index]);
         }
         break;
      case AIR_RESOURCE_IMAGE:
         *textures = MAX2(*textures, r->index + 1);
         if (r->index < RWA_MAX_TEXTURES) {
            slots[r->index].stored = true;
            slots[r->index].read |= r->read;
            slots[r->index].write |= r->write;
            declare_slot_element(r->type_name, &slots[r->index]);
         }
         break;
      case AIR_RESOURCE_SAMPLER:
         if (!r->inline_sampler)
            *samplers = MAX2(*samplers, r->index + 1);
         break;
      case AIR_RESOURCE_THREADGROUP_BUFFER:
         /* Metal has no threadgroup memory in a fragment or vertex stage; nothing to
          * count. A dispatch is where one is met, and refused there.
          */
         break;
      }
   }
}

/*
 * What textures a module declares (air_metal.h). Here rather than beside the texture
 * calls because it is count_resources and slot_shapes that answer it, and those are the
 * two halves a pipeline fills its own slots with: the same metadata and the same
 * instructions, read once more for a caller that has to bind before it has a pipeline.
 *
 * It takes a device and not just the bitcode on purpose. air_to_nir is handed
 * device->nir_options, exactly as rwa_compute_create and rwa_render_create hand it, so
 * the shader this reads is the shader that pipeline will hold. A query that translated
 * with options of its own would be a second translation with a second answer, and the
 * point of this call is that there is only one.
 *
 * It is NOT asked of a pipeline, which would avoid the second translation altogether and
 * was the first shape tried. A pipeline cannot answer for a module that gets none -- an
 * air.visible library function, or a fragment shader handed to the compute path -- and a
 * third of the corpus is air.visible, so the host-side table of what every function
 * declares would have holes exactly where a reader would most want a row. The cost of
 * the extra translation is one shader per query, which is nothing beside a draw.
 */
int
rwa_module_textures(struct rwa_device *device, const void *bitcode, size_t size,
                    const char *entry, struct rwa_texture_decl *out, unsigned max,
                    unsigned *count, char *why, size_t why_size)
{
   RWA_GUARD();
   if (count)
      *count = 0;
   if (!device) {
      say(why, why_size, "no device to read a module with");
      return -1;
   }
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return -1;
   }
   if (!out && max) {
      say(why, why_size, "nowhere to put %u texture declarations", max);
      return -1;
   }

   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx) {
      say(why, why_size, "out of memory reading a module");
      return -1;
   }

   /* Function constants taken as zero, which is rwa_compute_create's reason verbatim:
    * there is nothing to take them from, and the translator refuses what it cannot fold.
    */
   struct air_constant_value *constants;
   unsigned num_constants =
      air_module_function_constants(mem_ctx, bitcode, size, &constants);
   const struct air_to_nir_options options = {
      .constants = constants,
      .num_constants = num_constants,
   };
   struct air_shader_info info = { 0 };
   const char *error;
   nir_shader *nir = air_to_nir(mem_ctx, bitcode, size, entry, &options,
                                device->nir_options, &info, &error);
   if (!nir) {
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      ralloc_free(mem_ctx);
      return -1;
   }
   const char *name = info.entry_point ? info.entry_point : "the function";

   /* A slot past the sixteen is refused here and by name, on check_bindings' grounds: a
    * caller told "three textures" that cannot be told WHICH three would bind the wrong
    * ones and dispatch over them.
    */
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if ((r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE) &&
          r->index >= RWA_MAX_TEXTURES) {
         say(why, why_size, "%s binds a texture at index %u, past the %u slots this "
                            "library fills", name, r->index, RWA_MAX_TEXTURES);
         ralloc_free(mem_ctx);
         return -1;
      }
   }

   struct rwa_slot slots[RWA_MAX_TEXTURES] = { 0 };
   unsigned buffers = 0, textures = 0, samplers = 0;
   count_resources(&info, slots, &buffers, &textures, &samplers);
   slot_shapes(nir, slots, NULL);

   unsigned n = 0;
   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++) {
      if (!slots[i].sampled && !slots[i].stored)
         continue;
      if (n >= max) {
         say(why, why_size, "%s declares a texture at slot %u and there is room for %u "
                            "declaration%s", name, i, max, max == 1 ? "" : "s");
         ralloc_free(mem_ctx);
         return -1;
      }

      struct rwa_texture_decl *d = &out[n++];
      memset(d, 0, sizeof(*d));
      d->index = i;

      /* The usage bits check_bindings will insist on, and not a superset of them: a
       * texture made ShaderRead|ShaderWrite where the shader only samples is a texture
       * this GPU may lay out differently for nothing.
       */
      if (slots[i].sampled || slots[i].read)
         d->usage |= RWA_USAGE_SHADER_READ;
      if (slots[i].stored || slots[i].write)
         d->usage |= RWA_USAGE_SHADER_WRITE;

      if (slots[i].shape_known)
         d->type_known = metal_texture_type(slots[i].dim, slots[i].is_array, &d->type);

      nir_alu_type have;
      const char *word;
      unsigned bits;
      if (slot_element(&slots[i], &have, &word, &bits)) {
         d->element = have == nir_type_float ? RWA_ELEMENT_FLOAT
                    : have == nir_type_int   ? RWA_ELEMENT_INT
                    : have == nir_type_uint  ? RWA_ELEMENT_UINT
                                             : RWA_ELEMENT_UNKNOWN;
         if (d->element != RWA_ELEMENT_UNKNOWN)
            d->element_bits = bits;
      }

      /* Apple's own words for the whole type, copied: the strings live under mem_ctx,
       * which this call frees, and a caller printing a table wants the declaration as
       * its author wrote it rather than this library's reconstruction of it.
       */
      for (unsigned k = 0; k < info.num_resources; k++) {
         const struct air_resource *r = &info.resources[k];
         if ((r->kind != AIR_RESOURCE_TEXTURE && r->kind != AIR_RESOURCE_IMAGE) ||
             r->index != i || !r->type_name)
            continue;
         snprintf(d->metal_type, sizeof(d->metal_type), "%s", r->type_name);
         break;
      }
   }

   if (count)
      *count = n;
   ralloc_free(mem_ctx);
   return 0;
}

/*
 * What argument buffers a module declares, and the two numbers an application allocates
 * its buffer from (air_metal.h). Beside rwa_module_textures because it is the same kind
 * of call and has the same reason to exist: a driver answering
 * -[MTLFunction newArgumentEncoderWithBufferIndex:] holds a function's bitcode and
 * nothing else -- no pipeline, and no descriptor the layout could be read off.
 *
 * `encoded_length` is 8 bytes per member the set DECLARES, used by this shader or not,
 * and that is Apple's number rather than a convenience of ours: `air.arg_type_size` on
 * the set's own node says 8 x members in every one of Resident Evil 4's 7 857 sets, and
 * `air.arg_type_align_size` says 8 in all of them (docs/air.md 8.2.1). The frontend
 * keeps every member in the resource list for exactly this reason -- a numbering that
 * skipped the unread ones would differ between two shaders sharing one set, and an
 * application that allocated from the shorter number would overlap its own encodings.
 *
 * The density of the ids is CHECKED and not assumed, because it is what makes 8 x members
 * the same number as Apple's: Metal's encodedLength is over the highest `[[id(n)]]`, so a
 * set whose ids have a gap in them (an MSL struct with `[[id(0)]]` and `[[id(5)]]`) is
 * longer than its member count says. SPIRV-Cross numbers densely and every set in reach
 * is dense, so this refuses rather than guesses which of the two readings was meant.
 */
int
rwa_module_argbufs(struct rwa_device *device, const void *bitcode, size_t size,
                   const char *entry, struct rwa_argbuf_layout *out, unsigned max,
                   unsigned *count, char *why, size_t why_size)
{
   RWA_GUARD();
   if (count)
      *count = 0;
   if (!device) {
      say(why, why_size, "no device to read a module with");
      return -1;
   }
   if (!bitcode || size < 4) {
      say(why, why_size, "%zu bytes is not a shader module", size);
      return -1;
   }
   if (!out && max) {
      say(why, why_size, "nowhere to put %u argument buffer layouts", max);
      return -1;
   }

   void *mem_ctx = ralloc_context(NULL);
   if (!mem_ctx) {
      say(why, why_size, "out of memory reading a module");
      return -1;
   }

   /* Function constants taken as zero, which is rwa_module_textures' reason verbatim:
    * there is nothing to take them from, and the translator refuses what it cannot fold.
    */
   struct air_constant_value *constants;
   unsigned num_constants =
      air_module_function_constants(mem_ctx, bitcode, size, &constants);
   const struct air_to_nir_options options = {
      .constants = constants,
      .num_constants = num_constants,
   };
   struct air_shader_info info = { 0 };
   const char *error;
   nir_shader *nir = air_to_nir(mem_ctx, bitcode, size, entry, &options,
                                device->nir_options, &info, &error);
   if (!nir) {
      say(why, why_size, "%s", error ? error : "the module could not be translated");
      ralloc_free(mem_ctx);
      return -1;
   }
   const char *name = info.entry_point ? info.entry_point : "the function";

   /* The sets, in the order the module declares them, which is the order the frontend
    * lists their members in: one pass, appending a layout the first time a set's Metal
    * index is seen.
    */
   unsigned n = 0;
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];

      if (r->argbuf_set == ~0u)
         continue;

      struct rwa_argbuf_layout *l = NULL;
      for (unsigned k = 0; k < n; k++) {
         if (out[k].metal_index == r->argbuf_set) {
            l = &out[k];
            break;
         }
      }
      if (!l) {
         if (n >= max) {
            say(why, why_size, "%s declares an argument buffer at Metal buffer index %u "
                               "and there is room for %u layout%s", name, r->argbuf_set,
                max, max == 1 ? "" : "s");
            ralloc_free(mem_ctx);
            return -1;
         }
         l = &out[n++];
         memset(l, 0, sizeof(*l));
         l->metal_index = r->argbuf_set;
         l->alignment = 8;
      }

      if (l->member_count >= RWA_MAX_ARGBUF_MEMBERS) {
         say(why, why_size, "the argument buffer %s declares at Metal buffer index %u has "
                            "more than the %u members this library records", name,
             r->argbuf_set, RWA_MAX_ARGBUF_MEMBERS);
         ralloc_free(mem_ctx);
         return -1;
      }
      if (r->argbuf_id >= RWA_MAX_ARGBUF_MEMBERS) {
         say(why, why_size, "member `%s` of the argument buffer %s declares at Metal "
                            "buffer index %u is id %u, and this library records %u "
                            "members of a set", r->name ? r->name : "?", name,
             r->argbuf_set, r->argbuf_id, RWA_MAX_ARGBUF_MEMBERS);
         ralloc_free(mem_ctx);
         return -1;
      }

      struct rwa_argbuf_member *m = &l->members[l->member_count++];
      m->id = r->argbuf_id;
      m->index = r->index;
      m->write = r->write ? 1 : 0;
      m->kind = r->kind == AIR_RESOURCE_SAMPLER ? RWA_ARGBUF_SAMPLER
              : r->kind == AIR_RESOURCE_TEXTURE ? RWA_ARGBUF_TEXTURE
              : r->kind == AIR_RESOURCE_IMAGE   ? RWA_ARGBUF_TEXTURE
                                                : RWA_ARGBUF_BUFFER;
      /* Copied and not pointed at: the strings live under mem_ctx, which this call
       * frees before it returns. */
      snprintf(m->name, sizeof(m->name), "%s", r->name ? r->name : "");
   }

   for (unsigned k = 0; k < n; k++) {
      struct rwa_argbuf_layout *l = &out[k];

      /* The ids have to be 0..member_count-1 with nothing missing and nothing twice,
       * because that is what makes `8 x members` the length Metal would have answered.
       * A set that is not dense is refused by name: the alternative is an application
       * allocating its buffer too short and encoding one member over another.
       */
      uint64_t seen = 0;
      for (unsigned i = 0; i < l->member_count; i++) {
         if (l->members[i].id >= l->member_count) {
            say(why, why_size, "the argument buffer %s declares at Metal buffer index %u "
                               "has %u members and one of them (`%s`) is id %u: Metal's "
                               "encodedLength is over the highest id, so this library "
                               "will not answer a length for a set whose ids have a gap "
                               "in them", name, l->metal_index, l->member_count,
                l->members[i].name, l->members[i].id);
            ralloc_free(mem_ctx);
            return -1;
         }
         if (seen & (1ull << l->members[i].id)) {
            say(why, why_size, "two members of the argument buffer %s declares at Metal "
                               "buffer index %u are both id %u", name, l->metal_index,
                l->members[i].id);
            ralloc_free(mem_ctx);
            return -1;
         }
         seen |= 1ull << l->members[i].id;
      }
      l->encoded_length = 8 * l->member_count;
   }

   if (count)
      *count = n;
   ralloc_free(mem_ctx);
   return 0;
}

/*
 * -------------------------------------------------------------------------------------
 * Depth and stencil state: MTLDepthStencilDescriptor in Gallium's terms.
 *
 * A created object, as it is in Metal (-newDepthStencilStateWithDescriptor:), and for
 * the reason a sampler is one here: the Gallium state is compiled once and bound at
 * every draw, and a compare function Metal has no name for must be refused where the
 * caller's own struct is still in hand.
 */

/* The state it was built from is not kept, as a sampler's is not: a refusal about a
 * depth/stencil state happens while it is being created, where the caller's struct is
 * still in hand, and what Metal asks of the state object afterwards (-readsDepth and
 * its three neighbours) is answered by the driver from the descriptor it still holds.
 */
struct rwa_depth_stencil {
   struct rwa_device *device;
   void *cso;
};

/* MTLCompareFunction, spelled out so a refusal names what the caller passed. */
static const char *
compare_function_name(unsigned f)
{
   static const char *const names[] = {
      "MTLCompareFunctionNever", "MTLCompareFunctionLess", "MTLCompareFunctionEqual",
      "MTLCompareFunctionLessEqual", "MTLCompareFunctionGreater",
      "MTLCompareFunctionNotEqual", "MTLCompareFunctionGreaterEqual",
      "MTLCompareFunctionAlways",
   };
   return f < ARRAY_SIZE(names) ? names[f] : NULL;
}

/*
 * MTLCompareFunction as PIPE_FUNC_x.
 *
 * The two enumerations happen to run in the same order, so this table is the identity
 * -- and it is written out for exactly that reason. A cast would be a bet that two
 * enumerations nobody here controls stay aligned; the table is a statement that they
 * are, checkable by reading eight lines. The sampler path's compare function is passed
 * through as a number today (sampler_state_from_metal) and nothing reads it, which is
 * why that one was not worth a table and this one is: a depth test reads it every
 * fragment.
 */
static bool
compare_function_from_metal(unsigned f, unsigned *out, const char *what, char *why,
                            size_t why_size)
{
   static const unsigned func[] = {
      PIPE_FUNC_NEVER, PIPE_FUNC_LESS, PIPE_FUNC_EQUAL, PIPE_FUNC_LEQUAL,
      PIPE_FUNC_GREATER, PIPE_FUNC_NOTEQUAL, PIPE_FUNC_GEQUAL, PIPE_FUNC_ALWAYS,
   };
   if (f >= ARRAY_SIZE(func)) {
      say(why, why_size, "%s is %u, which is not one of MTLCompareFunction's eight", what, f);
      return false;
   }
   *out = func[f];
   return true;
}

/*
 * MTLStencilOperation as PIPE_STENCIL_OP_x, and this one is NOT the identity: Metal
 * counts IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap, and
 * Gallium counts INCR, DECR, INCR_WRAP, DECR_WRAP, INVERT. Invert is 5 in one and 7 in
 * the other, and the two wrapping operations sit where Metal keeps Invert. A cast would
 * turn "invert the stencil" into "increment it and wrap", which is a picture and not a
 * failure.
 */
static bool
stencil_op_from_metal(unsigned op, unsigned *out, const char *what, char *why,
                      size_t why_size)
{
   static const unsigned ops[] = {
      PIPE_STENCIL_OP_KEEP,        /* MTLStencilOperationKeep */
      PIPE_STENCIL_OP_ZERO,        /* ...Zero */
      PIPE_STENCIL_OP_REPLACE,     /* ...Replace */
      PIPE_STENCIL_OP_INCR,        /* ...IncrementClamp */
      PIPE_STENCIL_OP_DECR,        /* ...DecrementClamp */
      PIPE_STENCIL_OP_INVERT,      /* ...Invert -- 5 here, PIPE's 7 */
      PIPE_STENCIL_OP_INCR_WRAP,   /* ...IncrementWrap */
      PIPE_STENCIL_OP_DECR_WRAP,   /* ...DecrementWrap */
   };
   if (op >= ARRAY_SIZE(ops)) {
      say(why, why_size, "%s is %u, which is not one of MTLStencilOperation's eight", what,
          op);
      return false;
   }
   *out = ops[op];
   return true;
}

/* Whether a face of MTLStencilDescriptor asks for anything at all. Metal has no
 * "stencil enabled" flag and Gallium needs one, so it is derived from what Metal's own
 * defaults are: compare Always with every operation Keep tests nothing and writes
 * nothing, which is exactly Metal's documented "effectively skips the test". Deriving
 * it is the only honest option -- a flag in the contract would be a field the Metal
 * driver has nothing to fill it in from.
 */
static bool
stencil_face_does_anything(const struct rwa_stencil_face *f)
{
   return f->compare_function != 7 /* MTLCompareFunctionAlways */ ||
          f->stencil_fail_operation != 0 || f->depth_fail_operation != 0 ||
          f->depth_stencil_pass_operation != 0;
}

static bool
stencil_face_from_metal(const struct rwa_stencil_face *in, struct pipe_stencil_state *out,
                        const char *face, char *why, size_t why_size)
{
   char what[64];
   unsigned func, fail, zfail, zpass;

   snprintf(what, sizeof(what), "the %s face's stencilCompareFunction", face);
   if (!compare_function_from_metal(in->compare_function, &func, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's stencilFailureOperation", face);
   if (!stencil_op_from_metal(in->stencil_fail_operation, &fail, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's depthFailureOperation", face);
   if (!stencil_op_from_metal(in->depth_fail_operation, &zfail, what, why, why_size))
      return false;
   snprintf(what, sizeof(what), "the %s face's depthStencilPassOperation", face);
   if (!stencil_op_from_metal(in->depth_stencil_pass_operation, &zpass, what, why, why_size))
      return false;

   out->func = func;
   out->fail_op = fail;
   out->zfail_op = zfail;
   out->zpass_op = zpass;
   /* Metal's masks are uint32 over an eight-bit buffer, and its own default is
    * 0xFFFFFFFF; Gallium's fields are the eight bits that can select anything. So the
    * high bits are dropped and not refused -- refusing Metal's own default would refuse
    * every descriptor an application makes.
    */
   out->valuemask = in->read_mask & 0xff;
   out->writemask = in->write_mask & 0xff;
   return true;
}

struct rwa_depth_stencil *
rwa_depth_stencil_create(struct rwa_device *device,
                         const struct rwa_depth_stencil_state *state, char *why,
                         size_t why_size)
{
   RWA_GUARD();
   if (!state) {
      say(why, why_size, "a depth/stencil state with no descriptor");
      return NULL;
   }

   struct pipe_depth_stencil_alpha_state dsa = { 0 };
   unsigned func;
   if (!compare_function_from_metal(state->depth_compare_function, &func,
                                    "depthCompareFunction", why, why_size))
      return NULL;

   /* depth_enabled is on whenever a state exists, and the compare function carries the
    * whole meaning. Metal has no enable bit: MTLCompareFunctionAlways with no write IS
    * its off state, and PIPE_FUNC_ALWAYS with depth_writemask 0 is the same thing to
    * both drivers here. One fewer derived flag is one fewer place for the two to
    * disagree.
    */
   dsa.depth_enabled = 1;
   dsa.depth_func = func;
   dsa.depth_writemask = state->depth_write_enabled ? 1 : 0;

   if (!stencil_face_from_metal(&state->front, &dsa.stencil[0], "front", why, why_size) ||
       !stencil_face_from_metal(&state->back, &dsa.stencil[1], "back", why, why_size))
      return NULL;

   /* Gallium's two enable bits are not symmetrical and that is the trap here:
    * stencil[0].enabled means "stencil testing at all" and stencil[1].enabled means
    * "the back face has its own state" (p_state.h says so in as many words). Metal
    * always carries two faces, so the second bit is on only when they differ --
    * otherwise a driver would emit two-sided state for a descriptor that asked for one
    * behaviour, which is not wrong but is not what was asked either.
    */
   dsa.stencil[0].enabled = stencil_face_does_anything(&state->front) ||
                            stencil_face_does_anything(&state->back);
   dsa.stencil[1].enabled = dsa.stencil[0].enabled &&
                            memcmp(&state->front, &state->back, sizeof(state->front)) != 0;

   struct rwa_depth_stencil *ds = calloc(1, sizeof(*ds));
   if (!ds) {
      say(why, why_size, "out of memory making a depth/stencil state");
      return NULL;
   }
   ds->device = device;
   ds->cso = device->pipe->create_depth_stencil_alpha_state(device->pipe, &dsa);
   if (!ds->cso) {
      say(why, why_size, "%s would not make a depth/stencil state of %s%s",
          rwa_device_name(device), compare_function_name(state->depth_compare_function),
          state->depth_write_enabled ? " writing depth" : " not writing depth");
      free(ds);
      return NULL;
   }
   return ds;
}

void
rwa_depth_stencil_destroy(struct rwa_depth_stencil *state)
{
   RWA_GUARD();
   if (!state)
      return;
   /* Unbound before deleted, unlike a sampler and like a pipeline's own states: a draw
    * binds this one and leaves it bound, so the context is holding exactly this pointer
    * when the caller lets it go. softpipe's delete is a plain FREE
    * (sp_state_derived.c's bound pointer is not cleared for it), so deleting it bound
    * leaves the context pointing at freed memory until something else is bound over it
    * -- the same trap delete_compute_state is about.
    */
   state->device->pipe->bind_depth_stencil_alpha_state(state->device->pipe, NULL);
   state->device->pipe->delete_depth_stencil_alpha_state(state->device->pipe, state->cso);
   free(state);
}

/*
 * MTLVertexFormat in Gallium's terms, and what each format DELIVERS to the register.
 *
 * The numbers are Apple's own and were read out of the SDK this driver is built against
 * -- iPhoneOS16.5.sdk's Metal.framework/Headers/MTLVertexDescriptor.h -- rather than
 * remembered, and userland/src/redwoodmetal.m carries a _Static_assert per row against
 * those enumerators, so a disagreement between this table and Apple's header is a build
 * failure of the driver and not a wrong fetch on the phone. That SDK's enumeration ends
 * at Half (53): FloatRG11B10 and FloatRGB9E5 are iOS 17's, and nothing on this device
 * can name them.
 *
 * The mapping is one rule, as docs/air.md 3.1 says: Char/Short/Int and their unsigned
 * forms are R8/R16/R32 _SINT/_UINT of the same channel count, `...Normalized` is
 * _SNORM/_UNORM instead, Half is _FLOAT at 16 bits and Float at 32, and
 * UChar4Normalized_BGRA is B8G8R8A8_UNORM -- whose channel order rides in
 * VFD_DECODE_INSTR.swap, which fd6_vertex_swap() reads off the pipe_format itself.
 *
 * `delivers` is what the shader's register holds afterwards, and it is here because that
 * is what has to match the member's declared type: a _UINT format delivers an integer,
 * a _UNORM one delivers a float in [0, 1]. Gallium's _USCALED/_SSCALED -- an integer in
 * memory widened to a float without normalising -- has no Metal counterpart and is
 * deliberately absent; Metal's plain integer formats feed an integer member, which is
 * what this check is for.
 *
 * `bytes` is one element's size in memory, which no pipe_format query gives for a
 * three-component vertex format the way the fetch sees it (util_format_get_blocksize of
 * R32G32B32_FLOAT is 12, which is right, but writing the number down is what lets the
 * stream bounds check below say what it bounded).
 */
struct rwa_vertex_format {
   unsigned mtl;
   const char *name;
   enum pipe_format pipe;
   enum air_attribute_base_type delivers;
   unsigned bytes;
};

static const struct rwa_vertex_format rwa_vertex_formats[] = {
   {  1, "MTLVertexFormatUChar2", PIPE_FORMAT_R8G8_UINT, AIR_ATTRIB_UINT, 2 },
   {  2, "MTLVertexFormatUChar3", PIPE_FORMAT_R8G8B8_UINT, AIR_ATTRIB_UINT, 3 },
   {  3, "MTLVertexFormatUChar4", PIPE_FORMAT_R8G8B8A8_UINT, AIR_ATTRIB_UINT, 4 },
   {  4, "MTLVertexFormatChar2", PIPE_FORMAT_R8G8_SINT, AIR_ATTRIB_INT, 2 },
   {  5, "MTLVertexFormatChar3", PIPE_FORMAT_R8G8B8_SINT, AIR_ATTRIB_INT, 3 },
   {  6, "MTLVertexFormatChar4", PIPE_FORMAT_R8G8B8A8_SINT, AIR_ATTRIB_INT, 4 },
   {  7, "MTLVertexFormatUChar2Normalized", PIPE_FORMAT_R8G8_UNORM, AIR_ATTRIB_FLOAT, 2 },
   {  8, "MTLVertexFormatUChar3Normalized", PIPE_FORMAT_R8G8B8_UNORM, AIR_ATTRIB_FLOAT, 3 },
   {  9, "MTLVertexFormatUChar4Normalized", PIPE_FORMAT_R8G8B8A8_UNORM, AIR_ATTRIB_FLOAT, 4 },
   { 10, "MTLVertexFormatChar2Normalized", PIPE_FORMAT_R8G8_SNORM, AIR_ATTRIB_FLOAT, 2 },
   { 11, "MTLVertexFormatChar3Normalized", PIPE_FORMAT_R8G8B8_SNORM, AIR_ATTRIB_FLOAT, 3 },
   { 12, "MTLVertexFormatChar4Normalized", PIPE_FORMAT_R8G8B8A8_SNORM, AIR_ATTRIB_FLOAT, 4 },
   { 13, "MTLVertexFormatUShort2", PIPE_FORMAT_R16G16_UINT, AIR_ATTRIB_UINT, 4 },
   { 14, "MTLVertexFormatUShort3", PIPE_FORMAT_R16G16B16_UINT, AIR_ATTRIB_UINT, 6 },
   { 15, "MTLVertexFormatUShort4", PIPE_FORMAT_R16G16B16A16_UINT, AIR_ATTRIB_UINT, 8 },
   { 16, "MTLVertexFormatShort2", PIPE_FORMAT_R16G16_SINT, AIR_ATTRIB_INT, 4 },
   { 17, "MTLVertexFormatShort3", PIPE_FORMAT_R16G16B16_SINT, AIR_ATTRIB_INT, 6 },
   { 18, "MTLVertexFormatShort4", PIPE_FORMAT_R16G16B16A16_SINT, AIR_ATTRIB_INT, 8 },
   { 19, "MTLVertexFormatUShort2Normalized", PIPE_FORMAT_R16G16_UNORM, AIR_ATTRIB_FLOAT, 4 },
   { 20, "MTLVertexFormatUShort3Normalized", PIPE_FORMAT_R16G16B16_UNORM, AIR_ATTRIB_FLOAT, 6 },
   { 21, "MTLVertexFormatUShort4Normalized", PIPE_FORMAT_R16G16B16A16_UNORM, AIR_ATTRIB_FLOAT, 8 },
   { 22, "MTLVertexFormatShort2Normalized", PIPE_FORMAT_R16G16_SNORM, AIR_ATTRIB_FLOAT, 4 },
   { 23, "MTLVertexFormatShort3Normalized", PIPE_FORMAT_R16G16B16_SNORM, AIR_ATTRIB_FLOAT, 6 },
   { 24, "MTLVertexFormatShort4Normalized", PIPE_FORMAT_R16G16B16A16_SNORM, AIR_ATTRIB_FLOAT, 8 },
   { 25, "MTLVertexFormatHalf2", PIPE_FORMAT_R16G16_FLOAT, AIR_ATTRIB_FLOAT, 4 },
   { 26, "MTLVertexFormatHalf3", PIPE_FORMAT_R16G16B16_FLOAT, AIR_ATTRIB_FLOAT, 6 },
   { 27, "MTLVertexFormatHalf4", PIPE_FORMAT_R16G16B16A16_FLOAT, AIR_ATTRIB_FLOAT, 8 },
   { 28, "MTLVertexFormatFloat", PIPE_FORMAT_R32_FLOAT, AIR_ATTRIB_FLOAT, 4 },
   { 29, "MTLVertexFormatFloat2", PIPE_FORMAT_R32G32_FLOAT, AIR_ATTRIB_FLOAT, 8 },
   { 30, "MTLVertexFormatFloat3", PIPE_FORMAT_R32G32B32_FLOAT, AIR_ATTRIB_FLOAT, 12 },
   { 31, "MTLVertexFormatFloat4", PIPE_FORMAT_R32G32B32A32_FLOAT, AIR_ATTRIB_FLOAT, 16 },
   { 32, "MTLVertexFormatInt", PIPE_FORMAT_R32_SINT, AIR_ATTRIB_INT, 4 },
   { 33, "MTLVertexFormatInt2", PIPE_FORMAT_R32G32_SINT, AIR_ATTRIB_INT, 8 },
   { 34, "MTLVertexFormatInt3", PIPE_FORMAT_R32G32B32_SINT, AIR_ATTRIB_INT, 12 },
   { 35, "MTLVertexFormatInt4", PIPE_FORMAT_R32G32B32A32_SINT, AIR_ATTRIB_INT, 16 },
   { 36, "MTLVertexFormatUInt", PIPE_FORMAT_R32_UINT, AIR_ATTRIB_UINT, 4 },
   { 37, "MTLVertexFormatUInt2", PIPE_FORMAT_R32G32_UINT, AIR_ATTRIB_UINT, 8 },
   { 38, "MTLVertexFormatUInt3", PIPE_FORMAT_R32G32B32_UINT, AIR_ATTRIB_UINT, 12 },
   { 39, "MTLVertexFormatUInt4", PIPE_FORMAT_R32G32B32A32_UINT, AIR_ATTRIB_UINT, 16 },
   { 42, "MTLVertexFormatUChar4Normalized_BGRA", PIPE_FORMAT_B8G8R8A8_UNORM,
         AIR_ATTRIB_FLOAT, 4 },
   { 45, "MTLVertexFormatUChar", PIPE_FORMAT_R8_UINT, AIR_ATTRIB_UINT, 1 },
   { 46, "MTLVertexFormatChar", PIPE_FORMAT_R8_SINT, AIR_ATTRIB_INT, 1 },
   { 47, "MTLVertexFormatUCharNormalized", PIPE_FORMAT_R8_UNORM, AIR_ATTRIB_FLOAT, 1 },
   { 48, "MTLVertexFormatCharNormalized", PIPE_FORMAT_R8_SNORM, AIR_ATTRIB_FLOAT, 1 },
   { 49, "MTLVertexFormatUShort", PIPE_FORMAT_R16_UINT, AIR_ATTRIB_UINT, 2 },
   { 50, "MTLVertexFormatShort", PIPE_FORMAT_R16_SINT, AIR_ATTRIB_INT, 2 },
   { 51, "MTLVertexFormatUShortNormalized", PIPE_FORMAT_R16_UNORM, AIR_ATTRIB_FLOAT, 2 },
   { 52, "MTLVertexFormatShortNormalized", PIPE_FORMAT_R16_SNORM, AIR_ATTRIB_FLOAT, 2 },
   { 53, "MTLVertexFormatHalf", PIPE_FORMAT_R16_FLOAT, AIR_ATTRIB_FLOAT, 2 },
};

static const struct rwa_vertex_format *
vertex_format(unsigned mtl)
{
   for (unsigned i = 0; i < ARRAY_SIZE(rwa_vertex_formats); i++)
      if (rwa_vertex_formats[i].mtl == mtl)
         return &rwa_vertex_formats[i];
   return NULL;
}

/* What a vertex format this library declines IS, so the refusal names it rather than
 * printing a number. The four it names are the packed ones -- see the table above and
 * the pixel format table's own paragraph about packed channel orders -- and the rest is
 * a number Apple's enumeration does not carry at all, which on this device means a
 * caller's struct is a different shape from this one.
 */
static const char *
vertex_format_refusal(unsigned f)
{
   switch (f) {
   case 0:
      return "MTLVertexFormatInvalid, which is what an unconfigured attribute holds";
   case 40:
      return "MTLVertexFormatInt1010102Normalized: four channels packed in one word, and "
             "this library declines a packed format because Metal's channel order and "
             "Gallium's are two conventions -- getting it wrong shifts a channel by two "
             "bits in a picture that still looks like a picture. R10G10B10A2_SNORM does "
             "have an a6xx vertex entry, so what this needs is a measurement and not a "
             "format";
   case 41:
      return "MTLVertexFormatUInt1010102Normalized: the same packed word as 40, declined "
             "for the same unmeasured channel order";
   case 54:
      return "MTLVertexFormatFloatRG11B10, which the iOS 16.5 SDK's MTLVertexFormat has "
             "not got (it ends at Half, 53): three channels packed in one word, and "
             "declined for the channel order the same way";
   case 55:
      return "MTLVertexFormatFloatRGB9E5, which the iOS 16.5 SDK's MTLVertexFormat has "
             "not got: PIPE_FORMAT_R9G9B9E5_FLOAT is texture-only in a6xx's format "
             "table, so fd6_vertex_format() answers FMT6_NONE and the hardware cannot "
             "fetch it at all";
   case 43: case 44:
      return "a gap in Apple's own MTLVertexFormat enumeration";
   default:
      return "not a value of MTLVertexFormat";
   }
}

/* MTLVertexStepFunction's five, named for the refusals. */
static const char *
step_function_name(unsigned f)
{
   static const char *const names[] = {
      "MTLVertexStepFunctionConstant", "MTLVertexStepFunctionPerVertex",
      "MTLVertexStepFunctionPerInstance", "MTLVertexStepFunctionPerPatch",
      "MTLVertexStepFunctionPerPatchControlPoint",
   };
   return f < ARRAY_SIZE(names) ? names[f] : NULL;
}

static const char *
attribute_type_name(enum air_attribute_base_type t)
{
   switch (t) {
   case AIR_ATTRIB_FLOAT: return "floating-point";
   case AIR_ATTRIB_INT:   return "signed integer";
   case AIR_ATTRIB_UINT:  return "unsigned integer";
   }
   return "?";
}

/*
 * The vertex elements of one pipeline, built from the module's own interface and the
 * descriptor's layout, and the whole of why this is not a loop over the descriptor:
 *
 *   - GALLIUM MATCHES ELEMENTS TO INPUTS BY POSITION. ir3's setup_input indexes
 *     so->inputs[] by nir_intrinsic_base -- the driver_location nir_lower_io wrote --
 *     and fd6_vertex_state_create emits VFD_DECODE[i] from elements[i] while
 *     emit_vfd_dest emits VFD_DEST_CNTL_INSTR[i] from vs->inputs[i]. So
 *     elements[i] feeds the input whose driver_location is i, and those are numbered in
 *     the order AIR lists the attributes WITH THE UNUSED ONES SKIPPED. For
 *     std_vert0_lpf -- attributes 0, 1 (air.arg_unused) and 3 -- element 0 is attribute
 *     0 and element 1 is attribute 3. An array built by attribute index would put
 *     attribute 1's format, offset and buffer under attribute 3's fetch, and the
 *     geometry would be right while the colour came out of the wrong stream.
 *   - the table comes from air_shader_info AND NOT FROM ir3's own input table, because
 *     ir3 may delete the read: Resident Evil 4's PhotonSplattingVS loads
 *     in_var_POSITION and then writes a constant position, so after optimisation its
 *     input entry is zero-filled -- slot 0, cm 0, regid 63 -- while emit_vfd_dest still
 *     counts it. Reading the interface off that entry would describe an attribute the
 *     shader never declared.
 *
 * An attribute gated by a function-constant predicate that the descriptor leaves
 * unconfigured gets a PLACEHOLDER element rather than being skipped, because skipping it
 * would shift every later driver_location by one. The placeholder fetches from the
 * library's own quad buffer with a stride of 0, so it reads sixteen bytes this library
 * owns at every vertex instead of whatever address an empty VFD slot points at -- which
 * on this hardware is 0 (docs/metal.md 10.18 found what a NULL vertex state does).
 */
struct rwa_vertex_state {
   struct pipe_vertex_element element[16];   /* NIR's sixteen generic slots */
   unsigned count;
   unsigned attribute[16];                   /* Metal's [[attribute(n)]] per element */
   unsigned stream_mask;                     /* buffer indices fetched from */
   unsigned stream_count;                    /* the highest of them plus one */
   unsigned stride[RWA_MAX_VERTEX_BUFFERS];  /* per buffer index, for the draw's bounds */
   unsigned per_vertex;                      /* which of them step per vertex */
   unsigned bytes[RWA_MAX_VERTEX_BUFFERS];   /* the largest element end in each */
};

static bool
build_vertex_state(const struct air_shader_info *vinfo,
                   const struct rwa_render_shape *shape, const char *vname,
                   struct pipe_screen *screen, struct rwa_vertex_state *out,
                   char *why, size_t why_size)
{
   memset(out, 0, sizeof(*out));

   for (unsigned i = 0; i < vinfo->num_vertex_attributes; i++) {
      const struct air_vertex_attribute *a = &vinfo->vertex_attributes[i];

      /* An attribute nothing reads needs no fetch and no binding, and Metal does not
       * require the application's descriptor to configure one: eight of QuartzCore's 46
       * are air.arg_unused.
       */
      if (!a->used)
         continue;
      if (a->index >= RWA_MAX_VERTEX_ATTRIBUTES) {
         say(why, why_size, "%s reads vertex attribute %u and Metal has %u", vname,
             a->index, RWA_MAX_VERTEX_ATTRIBUTES);
         return false;
      }
      if (a->driver_location >= ARRAY_SIZE(out->element)) {
         say(why, why_size, "%s puts vertex attribute %u at driver location %u, past the "
                            "%u NIR has generic slots for", vname, a->index,
             a->driver_location, (unsigned)ARRAY_SIZE(out->element));
         return false;
      }

      const struct rwa_vertex_attribute *d = &shape->vertex_attribute[a->index];
      unsigned loc = a->driver_location;

      out->count = MAX2(out->count, loc + 1);
      out->attribute[loc] = a->index;

      if (d->format == 0) {
         /* Not configured. Legal only for an attribute the module gates with a
          * function-constant predicate, whose value is the pipeline's and not this
          * library's to know (docs/air.md 3.1): the parameter exists either way, so the
          * attribute is in vertex_attributes_read either way, and a pipeline whose
          * constants make the predicate false may configure nothing here.
          */
         if (!a->function_constant) {
            say(why, why_size, "%s reads vertex attribute %u (%s, %s) and the vertex "
                               "descriptor configures no format there, so the fetch "
                               "would read a slot nothing was set up for", vname,
                a->index, a->name ? a->name : "unnamed",
                a->type_name ? a->type_name : "?");
            return false;
         }
         /* The placeholder: this library's own quad buffer, read at stride 0. */
         out->element[loc] = (struct pipe_vertex_element){
            .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
            .src_offset = 0, .src_stride = 0,
            .vertex_buffer_index = RWA_MAX_VERTEX_BUFFERS,
         };
         continue;
      }

      const struct rwa_vertex_format *f = vertex_format(d->format);
      if (!f) {
         say(why, why_size, "vertex attribute %u of %s: format %u is %s", a->index, vname,
             d->format, vertex_format_refusal(d->format));
         return false;
      }
      if (f->delivers != a->base_type) {
         say(why, why_size, "vertex attribute %u of %s is %s, which wants %s components, "
                            "and %s delivers %s ones", a->index, vname,
             a->type_name ? a->type_name : (a->name ? a->name : "unnamed"),
             attribute_type_name(a->base_type), f->name,
             attribute_type_name(f->delivers));
         return false;
      }
      /* The polite question, asked of the screen rather than assumed: fd6_screen.cc
       * answers it from fd6_vertex_format() != FMT6_NONE, which is the same test
       * fd6_vertex_state_create then asserts on. Asking means a format this library
       * maps but that hardware cannot fetch is a sentence here instead of an assertion
       * there.
       */
      if (!screen->is_format_supported(screen, f->pipe, PIPE_BUFFER, 0, 0,
                                       PIPE_BIND_VERTEX_BUFFER)) {
         say(why, why_size, "%s cannot fetch %s (%s) as a vertex attribute",
             screen->get_name(screen), f->name, util_format_short_name(f->pipe));
         return false;
      }
      if (d->buffer_index >= RWA_MAX_VERTEX_BUFFERS) {
         say(why, why_size, "vertex attribute %u of %s is fetched from buffer index %u, "
                            "and Metal has %u", a->index, vname, d->buffer_index,
             RWA_MAX_VERTEX_BUFFERS);
         return false;
      }
      /* Twelve bits at shift 5 in VFD_DECODE_INSTR, set with a masking setter: 4096
       * would fetch offset 0. Metal allows more, so this is ours to refuse.
       */
      if (d->offset > 4095) {
         say(why, why_size, "vertex attribute %u of %s is at offset %u, and a6xx's "
                            "VFD_DECODE_INSTR.offset is twelve bits (4095): the setter "
                            "masks rather than asserts, so this would silently fetch "
                            "offset %u", a->index, vname, d->offset, d->offset & 4095);
         return false;
      }

      const struct rwa_vertex_layout *l = &shape->vertex_layout[d->buffer_index];
      const char *sf = step_function_name(l->step_function);
      unsigned rate = l->step_rate ? l->step_rate : 1;
      unsigned divisor = 0;
      unsigned stride = l->stride;

      if (!sf) {
         say(why, why_size, "buffer index %u of %s's vertex descriptor has step function "
                            "%u, which is not one of MTLVertexStepFunction's five",
             d->buffer_index, vname, l->step_function);
         return false;
      }
      switch (l->step_function) {
      case 0:   /* Constant: every vertex and every instance reads element 0 */
         stride = 0;
         break;
      case 1:   /* PerVertex */
         if (rate != 1) {
            say(why, why_size, "buffer index %u of %s's vertex descriptor steps per "
                               "vertex at a rate of %u: Gallium has a divisor for "
                               "instances and nothing to express a rate over vertices "
                               "with", d->buffer_index, vname, rate);
            return false;
         }
         out->per_vertex |= 1u << d->buffer_index;
         break;
      case 2:   /* PerInstance: Gallium's instance_divisor exactly */
         divisor = rate;
         break;
      default:
         say(why, why_size, "buffer index %u of %s's vertex descriptor steps with %s, "
                            "which is tessellation: there is no pipe_vertex_element "
                            "field for it and no tessellation stage in this port",
             d->buffer_index, vname, sf);
         return false;
      }
      if (stride > 4095) {
         /* The offset's twelve bits are in this tree's own register header; the stride's
          * width is not -- fd6_context.cc writes VFD_FETCH_STRIDE as a whole word with
          * no mask and no assert in Mesa 25.0.7, and Gallium's own src_stride is
          * "technically only uint16_t". So 4095 here is the hardware field docs/air.md
          * 3.1 read, taken conservatively: a refusal can be lifted by a measurement on
          * the phone, and a stride truncated in the ringbuffer cannot be seen at all.
          */
         say(why, why_size, "buffer index %u of %s's vertex descriptor has a stride of "
                            "%u bytes, and a6xx's vertex fetch stride is twelve bits "
                            "(4095)", d->buffer_index, vname, stride);
         return false;
      }
      if (l->step_function != 0 && stride == 0) {
         say(why, why_size, "buffer index %u of %s's vertex descriptor steps with %s and "
                            "has a stride of 0, so every vertex would read the same "
                            "element: that is what %s is for", d->buffer_index, vname,
             sf, step_function_name(0));
         return false;
      }
      if (stride && (size_t)d->offset + f->bytes > stride) {
         say(why, why_size, "vertex attribute %u of %s is %s at offset %u of a %u-byte "
                            "element, so it reads %u bytes of the next vertex", a->index,
             vname, f->name, d->offset, stride,
             (unsigned)((size_t)d->offset + f->bytes - stride));
         return false;
      }

      out->element[loc] = (struct pipe_vertex_element){
         .src_format = f->pipe,
         .src_offset = (uint16_t)d->offset,
         .src_stride = stride,
         .vertex_buffer_index = d->buffer_index,
         .instance_divisor = divisor,
      };
      out->stream_mask |= 1u << d->buffer_index;
      out->stream_count = MAX2(out->stream_count, d->buffer_index + 1);
      out->stride[d->buffer_index] = stride;
      out->bytes[d->buffer_index] = MAX2(out->bytes[d->buffer_index],
                                         d->offset + f->bytes);
   }

   /* Metal's one buffer table, which both -setVertexBuffer: and a [[buffer(n)]] argument
    * read out of, so an index is a stream or an argument and never both. The frontend
    * knows which indices the module declares; this is where the two sets meet.
    */
   for (unsigned i = 0; i < vinfo->num_resources; i++) {
      const struct air_resource *r = &vinfo->resources[i];
      if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER)
         continue;
      if (r->index < RWA_MAX_VERTEX_BUFFERS && (out->stream_mask & (1u << r->index))) {
         say(why, why_size, "buffer index %u is a vertex stream in this descriptor and "
                            "%s reads it as [[buffer(%u)]] (%s): Metal has one table "
                            "behind -setVertexBuffer:offset:atIndex: and an index can be "
                            "a stream or an argument, not both", r->index, vname,
             r->index, r->name ? r->name : "unnamed");
         return false;
      }
   }

   /* The placeholder's buffer index, chosen after the real ones are known: one past
    * Metal's highest, so it can never collide with a stream or an argument, and the draw
    * binds this library's quad there.
    */
   if (out->count) {
      bool placeholder = false;
      for (unsigned i = 0; i < out->count; i++)
         placeholder = placeholder ||
                       out->element[i].vertex_buffer_index == RWA_MAX_VERTEX_BUFFERS;
      if (placeholder)
         out->stream_count = MAX2(out->stream_count, RWA_MAX_VERTEX_BUFFERS + 1);
   }
   return true;
}

/* A compiled pipeline for drawing. The shaders are kept pristine under mem_ctx and a
 * clone is handed to the driver, for the reason compute_state_for gives: a Gallium
 * driver steals the NIR it is given, so what this library frees at destroy must be its
 * own copy and not the driver's.
 */
struct rwa_render {
   struct rwa_device *device;
   void *mem_ctx;

   struct air_shader_info finfo;
   struct air_shader_info vinfo;      /* valid only when a vertex module was supplied */
   bool generated_vertex;

   struct rwa_render_shape shape;

   /* the fragment and (real) vertex texture slots and the samplers each stage declares
    * for itself, kept with the pipeline like rwa_compute's
    */
   struct rwa_slot frag_slots[RWA_MAX_TEXTURES];
   struct rwa_slot vert_slots[RWA_MAX_TEXTURES];
   void *frag_own_sampler[RWA_MAX_TEXTURES];
   void *vert_own_sampler[RWA_MAX_TEXTURES];
   bool frag_compare_sampler[RWA_MAX_TEXTURES];
   bool vert_compare_sampler[RWA_MAX_TEXTURES];

   /* the compiled state, made once: a render pipeline has a fixed shape, so unlike a
    * compute state there is no threadgroup size to recompile for
    */
   void *vs_cso, *fs_cso;
   void *blend, *dsa, *rast, *rast_scissor, *velem;   /* (rast_scissor: the same with the scissor test on) */
   struct pipe_resource *vbuf;        /* the full-screen quad, for the generated stage */

   /* What the vertex descriptor was turned into, empty for a stage that fetches no
    * attributes -- which is the generated quad and every vertex module this library
    * drew before the layout existed. `velem` is built from it when it is not empty, and
    * the draw binds the streams it names instead of the quad.
    */
   struct rwa_vertex_state vstate;
};

/* Clone, finalise and compile one stage, handing the driver a copy it may steal. The
 * clone is not freed on failure, for compute_state_for's reason: a driver that refused
 * it may or may not have taken it, and neither driver in this build reaches this (both
 * return a state object and report a bad shader later, at the draw).
 */
/* How far into each constant buffer the final shader reads, onto info's resources: a
 * buffer bound at an offset this GPU cannot fetch from is uploaded per draw (bind_stage),
 * and without air.buffer_size that was the whole rest of the buffer -- 175 KB a draw of
 * CoreAnimation's uniforms, read out of write-combined memory, 1.6 ms of every draw. A
 * buffer read at a run-time offset, or through a run-time index, stays unknown (0). */
static void
note_constant_reads(nir_shader *nir, struct air_shader_info *info)
{
   uint32_t upto[PIPE_MAX_CONSTANT_BUFFERS] = { 0 };
   bool unknown[PIPE_MAX_CONSTANT_BUFFERS] = { false };
   bool any_unknown_index = false;
   bool ubo_used[PIPE_MAX_CONSTANT_BUFFERS] = { false };
   bool ssbo_used[PIPE_MAX_SHADER_BUFFERS] = { false };
   bool any_ssbo_unknown = false;
   bool tex_used[PIPE_MAX_SHADER_SAMPLER_VIEWS] = { false }, sampler_used[PIPE_MAX_SAMPLERS] = { false };
   bool image_used[PIPE_MAX_SHADER_IMAGES] = { false };
   bool any_tex_unknown = false, any_image_unknown = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               /* which textures and samplers it samples at all (`unused`) */
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               if (nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) >= 0 ||
                   nir_tex_instr_src_index(tex, nir_tex_src_texture_deref) >= 0 ||
                   nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset) >= 0 ||
                   nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref) >= 0)
                  any_tex_unknown = true;
               if (tex->texture_index < PIPE_MAX_SHADER_SAMPLER_VIEWS)
                  tex_used[tex->texture_index] = true;
               if (tex->sampler_index < PIPE_MAX_SAMPLERS)
                  sampler_used[tex->sampler_index] = true;
               continue;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (strstr(nir_intrinsic_infos[in->intrinsic].name, "image") != NULL) {
               if (in->num_components == 0 && nir_intrinsic_infos[in->intrinsic].num_srcs == 0)
                  continue;
               if (strstr(nir_intrinsic_infos[in->intrinsic].name, "deref") != NULL ||
                   !nir_src_is_const(in->src[0]))
                  any_image_unknown = true;
               else if (nir_src_as_uint(in->src[0]) < PIPE_MAX_SHADER_IMAGES)
                  image_used[nir_src_as_uint(in->src[0])] = true;
               continue;
            }
            /* which buffers it reads or writes at all (`unused`) */
            int ssbo_src = -1;
            switch (in->intrinsic) {
            case nir_intrinsic_load_ssbo:
            case nir_intrinsic_ssbo_atomic:
            case nir_intrinsic_ssbo_atomic_swap:
            case nir_intrinsic_get_ssbo_size:
               ssbo_src = 0;
               break;
            case nir_intrinsic_store_ssbo:
               ssbo_src = 1;
               break;
            default:
               break;
            }
            if (ssbo_src >= 0) {
               if (!nir_src_is_const(in->src[ssbo_src]))
                  any_ssbo_unknown = true;
               else if (nir_src_as_uint(in->src[ssbo_src]) < PIPE_MAX_SHADER_BUFFERS)
                  ssbo_used[nir_src_as_uint(in->src[ssbo_src])] = true;
               continue;
            }
            if (in->intrinsic != nir_intrinsic_load_ubo)
               continue;
            if (!nir_src_is_const(in->src[0])) {
               any_unknown_index = true;
               continue;
            }
            unsigned ubo = nir_src_as_uint(in->src[0]);
            if (ubo >= PIPE_MAX_CONSTANT_BUFFERS)
               continue;
            ubo_used[ubo] = true;
            if (!nir_src_is_const(in->src[1])) {
               unknown[ubo] = true;
               continue;
            }
            uint32_t end = nir_src_as_uint(in->src[1]) +
                           in->def.num_components * in->def.bit_size / 8;
            upto[ubo] = MAX2(upto[ubo], end);
         }
      }
   }
   for (unsigned i = 0; i < info->num_resources; i++) {
      struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->argbuf_set == ~0u &&
          r->index < PIPE_MAX_SHADER_BUFFERS)
         r->unused = !any_ssbo_unknown && !ssbo_used[r->index];
      /* a texture: sampled (a sampler view) or, written, an image */
      if (r->kind == AIR_RESOURCE_TEXTURE && r->argbuf_set == ~0u && r->index < PIPE_MAX_SHADER_IMAGES &&
          r->index < PIPE_MAX_SHADER_SAMPLER_VIEWS)
         r->unused = !any_tex_unknown && !any_image_unknown && !tex_used[r->index] && !image_used[r->index];
      if (r->kind == AIR_RESOURCE_SAMPLER && r->argbuf_set == ~0u && r->index < PIPE_MAX_SAMPLERS &&
          !r->inline_sampler)
         r->unused = !any_tex_unknown && !sampler_used[r->index];
      if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER || r->argbuf_set != ~0u ||
          r->index >= PIPE_MAX_CONSTANT_BUFFERS)
         continue;
      r->unused = !any_unknown_index && !ubo_used[r->index];
      r->reads = any_unknown_index || unknown[r->index] ? 0 : MAX2(upto[r->index], 4);
   }
}

static void *
make_stage_cso(struct rwa_device *device, nir_shader *nir, struct air_shader_info *info,
               char *why, size_t why_size)
{
   nir_shader *clone = nir_shader_clone(NULL, nir);
   if (!clone) {
      say(why, why_size, "out of memory compiling a shader stage");
      return NULL;
   }
   finalize_for_gallium(clone, device->screen);
   if (info)
      note_constant_reads(clone, info);

   struct pipe_shader_state templ = { .type = PIPE_SHADER_IR_NIR, .ir.nir = clone };
   struct pipe_context *pipe = device->pipe;
   void *cso = nir->info.stage == MESA_SHADER_VERTEX
                  ? pipe->create_vs_state(pipe, &templ)
                  : pipe->create_fs_state(pipe, &templ);
   if (!cso)
      say(why, why_size, "%s would not compile the %s stage",
          device->screen->get_name(device->screen),
          nir->info.stage == MESA_SHADER_VERTEX ? "vertex" : "fragment");
   return cso;
}

/* Whether a vertex shader reads vertex attributes, which used to be the one thing the
 * contract could not feed it -- rwa_render_shape carried no formats, strides or offsets,
 * and a [[stage_in]] shader was refused by name rather than drawn over garbage. The
 * layout is in the shape now (docs/metal.md 10.44), so this is no longer a refusal: it
 * is the NIR half of a cross-check against air_shader_info's table, because the elements
 * are built from that table and a disagreement between the two would mean an element
 * array that does not describe the fetches. Apple's own three vertex shaders in the GL
 * renderer read their vertices out of a buffer indexed by the vertex id and so have no
 * such input at all.
 */
/*
 * Whether the fragment shader stores to [[depth]], which decides what a depth
 * attachment ends up holding: the interpolated z, or whatever the shader computed.
 *
 * A declaration is not enough and that is the whole point of walking the stores. Both
 * of Apple's depth shaders declare a colour output and never store to it
 * (texturedFragment2DDepth returns {undef, depth}), so a shader's variables say what it
 * might write and only its stores say what it does -- air_run makes the same walk for
 * the colour side and refuses to read back an attachment nothing stored to.
 */
static bool
fragment_writes_depth(nir_shader *fs)
{
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(in->src[0]);
            if (d && d->deref_type == nir_deref_type_var &&
                d->var->data.location == FRAG_RESULT_DEPTH)
               return true;
         }
      }
   }
   return false;
}

static bool
vertex_reads_attributes(nir_shader *vs)
{
   nir_foreach_variable_with_modes(var, vs, nir_var_shader_in)
      if (var->data.location >= VERT_ATTRIB_GENERIC0 &&
          var->data.location < VERT_ATTRIB_MAX)
         return true;
   return false;
}

/*
 * ===================== METAL'S BLEND STATE, MAPPED BY TABLE =====================
 *
 * Two enumerations and a bitfield, and not one of the three may be cast.
 *
 * MTLBlendFactor against pipe_blendfactor. Gallium's enumeration is built out of an
 * INVERT BIT: PIPE_BLENDFACTOR_INVERT_BIT is 0x10 and PIPE_BLENDFACTOR_ZERO is literally
 * `INVERT_BIT | ONE` (src/util/blend.h), so the plain factors run 1..10 and their
 * one-minus forms run 0x11..0x1a with two holes. Metal's run 0..18 with every factor next
 * to its own one-minus form. The two therefore agree on EXACTLY TWO of nineteen values:
 *
 *   Metal                      value   a cast lands on              is that right?
 *   MTLBlendFactorZero             0    (no enumerator at all)      undefined
 *   ...One                         1    PIPE_BLENDFACTOR_ONE        YES, by luck
 *   ...SourceColor                 2    PIPE_BLENDFACTOR_SRC_COLOR  YES, by luck
 *   ...OneMinusSourceColor         3    SRC_ALPHA                   no
 *   ...SourceAlpha                 4    DST_ALPHA                   no
 *   ...OneMinusSourceAlpha         5    DST_COLOR                   no  <-- the compositor's
 *   ...DestinationColor            6    SRC_ALPHA_SATURATE          no
 *   ...OneMinusDestinationColor    7    CONST_COLOR                 no
 *   ...DestinationAlpha            8    CONST_ALPHA                 no
 *   ...OneMinusDestinationAlpha    9    SRC1_COLOR                  no
 *   ...SourceAlphaSaturated       10    SRC1_ALPHA                  no
 *   ...BlendColor                 11    (no enumerator)             undefined
 *   ...OneMinusBlendColor         12    (no enumerator)             undefined
 *   ...BlendAlpha                 13    (no enumerator)             undefined
 *   ...OneMinusBlendAlpha         14    (no enumerator)             undefined
 *   ...Source1Color               15    (no enumerator)             undefined
 *   ...OneMinusSource1Color       16    (no enumerator)             undefined
 *   ...Source1Alpha               17    PIPE_BLENDFACTOR_ZERO       no
 *   ...OneMinusSource1Alpha       18    INV_SRC_COLOR               no
 *
 * Two right, ten silently wrong, seven landing on no enumerator. And the one that
 * matters most is the fifth: CoreAnimation's destination factor is OneMinusSourceAlpha,
 * and a cast turns it into DST_COLOR -- the destination multiplied by ITSELF. On the home
 * screen that is a dark wash that deepens wherever two layers overlap, which is a picture
 * and not a failure.
 *
 * MTLBlendOperation against pipe_blend_func: Add 0, Subtract 1, ReverseSubtract 2, Min 3,
 * Max 4 on both sides, so this one IS the identity -- and it is written out for exactly
 * the reason compare_function_from_metal is. A cast would be a bet that two enumerations
 * nobody here controls stay aligned; a table of five lines is a statement that they are,
 * checkable by reading it.
 *
 * MTLColorWriteMask against Gallium's colormask, and this is the one a test is most
 * likely to pass with backwards. Metal's bits are Red = 1 << 3, Green = 1 << 2,
 * Blue = 1 << 1, Alpha = 1 << 0; Gallium's are PIPE_MASK_R 0x1, G 0x2, B 0x4, A 0x8
 * (src/util/format/u_formats.h). The nibble is REVERSED. Four of the sixteen masks are
 * palindromes and survive a cast -- None 0x0, All 0xf, green|blue 0x6, red|alpha 0x9 --
 * and the other twelve exchange red with alpha and green with blue. A pipeline asking to
 * write colour and not alpha (R|G|B, Metal's 0xe) would, cast, write alpha and not red.
 *
 * WHY EVERY REFUSAL BELOW IS WRITTEN HERE AND NOT LEFT TO THE DRIVER UNDERNEATH, which
 * is the same argument the vertex layout's refusals are made of and it was read in
 * freedreno's own source: fd_blend_factor (freedreno_util.c) ends
 *
 *     default:
 *        DBG("invalid blend factor: %x", factor);
 *        return 0;
 *
 * and 0 is FACTOR_ZERO. DBG is gated on a debug flag, so in an ordinary build a factor
 * freedreno does not recognise produces NO diagnostic and blends with zero -- the term
 * vanishes. fd6_blend.cc's blend_func does the same and its 0 is BLEND_DST_PLUS_SRC, so
 * an unrecognised EQUATION silently becomes Add. softpipe's four factor switches end in
 * a bare `assert(0 && "invalid rgb src factor")`, which is nothing at all in a release
 * build. Neither back end will ever tell anyone; this library has to.
 *
 * `dither` is deliberately left at 0, and that is not a default being accepted: GL's
 * GL_DITHER is on by default and Mesa's state tracker sets the bit, a6xx honours it
 * (A6XX_RB_DITHER_CNTL, DITHER_ALWAYS, in fd6_blend.cc) and softpipe ignores it
 * entirely. Metal has no dither control, so there is nothing to carry -- and a bit set
 * here would make the phone's bytes differ from this machine's for no reason anyone
 * asked for.
 *
 * WHERE THESE NUMBERS COME FROM, and it is two sources that do not share a copy:
 *
 *   1. Apple's own MTLRenderPipeline.h for this release, which is on the build machine
 *      and not in this tree. userland/src/redwoodmetal.m compiles against it and
 *      _Static_asserts every value this table assumes, so a disagreement is a BUILD
 *      FAILURE of the driver rather than a wrong picture. (The same declarations are
 *      byte for byte identical in every iOS SDK from 10.3 to 16.5 and in macOS 13.3's,
 *      which is six copies agreeing.)
 *   2. Metal.framework's own code on the DEVICE, which carries the numbering as data:
 *      MTLBlendFactorString (0x181b08730) bounds its argument at 18 and indexes a table
 *      of nineteen CFStrings at 0x1d59dbe18, in the order this table has them;
 *      MTLBlendOperation's is five entries at 0x1d59dbeb0; and
 *      MTLColorWriteMaskString (0x181b08754) does not use a table at all -- it tests
 *      bit 3 for "R", bit 2 for "G", bit 1 for "B" and bit 0 for "A", which IS the
 *      reversal, read out of the framework that defines it.
 *
 * Neither source is this library's reading of the other, and the second one is what the
 * phone actually runs. blend_tables_are_shaped_right below is a third kind of check and
 * not a third source: it asserts the shape of each table -- the length, the one-minus
 * pairing, the bit reversal -- at every pipeline creation, so a row edited by hand
 * cannot quietly go missing.
 */

/*
 * THE SHAPE OF THE TWO STRUCTS THE METAL DRIVER TYPES OUT BY HAND, stated here as
 * numbers so that an edit to one side alone fails to build on that side.
 *
 * The same four numbers are asserted in userland/src/redwoodmetal.m against ITS
 * definitions. Neither file can see the other's structs, so this is not a comparison --
 * it is two independent statements of one agreement, which is the most a hand-copied
 * struct admits of. This port has paid twice for getting it wrong: a primitive type that
 * arrived as 1871292816, and struct air_resource growing by eight bytes so that every
 * resource after the first was read eight bytes early.
 *
 * The OFFSETS are asserted as well as the sizes, and that is the point: the discipline
 * is that a new field goes on the END, and a field inserted in the middle keeps the size
 * while moving everything after it -- which is the failure that leaves no trace. The
 * blend state took rwa_render_shape from 792 bytes to 1052 (260 bytes at offset 792,
 * nothing above it moved) and rwa_render_info from 108 to 116 (8 bytes at offset 108).
 */
static_assert(sizeof(struct rwa_color_blend) == 32 &&
                 sizeof(struct rwa_render_shape) == 1052 &&
                 offsetof(struct rwa_render_shape, vertex_attribute) == 48 &&
                 offsetof(struct rwa_render_shape, vertex_layout) == 420 &&
                 offsetof(struct rwa_render_shape, color_blend) == 792 &&
                 offsetof(struct rwa_render_shape, color_blend_count) == 1048,
              "struct rwa_render_shape is not the shape redwoodmetal.m asserts: a field "
              "was added in the middle, or only one of the two copies was rebuilt");
static_assert(sizeof(struct rwa_render_info) == 116 &&
                 offsetof(struct rwa_render_info, vertex_argbuf_mask) == 100 &&
                 offsetof(struct rwa_render_info, blend_enabled_mask) == 108,
              "struct rwa_render_info is not the shape redwoodmetal.m asserts, and this "
              "one the LIBRARY writes into: a driver with a shorter one is overrun");

/* MTLBlendFactor, spelled out so a refusal names what the caller passed rather than
 * printing a number at them. */
static const char *
blend_factor_name(unsigned f)
{
   static const char *const names[] = {
      "MTLBlendFactorZero", "MTLBlendFactorOne",
      "MTLBlendFactorSourceColor", "MTLBlendFactorOneMinusSourceColor",
      "MTLBlendFactorSourceAlpha", "MTLBlendFactorOneMinusSourceAlpha",
      "MTLBlendFactorDestinationColor", "MTLBlendFactorOneMinusDestinationColor",
      "MTLBlendFactorDestinationAlpha", "MTLBlendFactorOneMinusDestinationAlpha",
      "MTLBlendFactorSourceAlphaSaturated",
      "MTLBlendFactorBlendColor", "MTLBlendFactorOneMinusBlendColor",
      "MTLBlendFactorBlendAlpha", "MTLBlendFactorOneMinusBlendAlpha",
      "MTLBlendFactorSource1Color", "MTLBlendFactorOneMinusSource1Color",
      "MTLBlendFactorSource1Alpha", "MTLBlendFactorOneMinusSource1Alpha",
   };
   return f < ARRAY_SIZE(names) ? names[f] : NULL;
}

static const char *
blend_operation_name(unsigned op)
{
   static const char *const names[] = {
      "MTLBlendOperationAdd", "MTLBlendOperationSubtract",
      "MTLBlendOperationReverseSubtract", "MTLBlendOperationMin",
      "MTLBlendOperationMax",
   };
   return op < ARRAY_SIZE(names) ? names[op] : NULL;
}

/* The factors this library cannot serve, and WHY each one -- the reason differs, and
 * "unsupported" would hide which of the two it is. Returns NULL for a factor that is
 * served. */
static const char *
blend_factor_refusal(unsigned f)
{
   if (f >= 11 && f <= 14)
      return "one of Metal's BLEND COLOUR factors, whose colour is ENCODER state "
             "(-setBlendColorRed:green:blue:alpha:) and not pipeline state: this "
             "contract has nowhere to carry it and the driver's own setter drops it, so "
             "the blend would be against the zero a Gallium context starts with -- the "
             "layer would vanish rather than fail. Serving it is one float[4] in "
             "rwa_draw and one set_blend_color, on the day something asks";
   if (f >= 15 && f <= 18)
      /* Deliberately short: `why` is 512 bytes here and in the driver, and the prefix
       * naming the field takes some of it, so a longer sentence would arrive truncated
       * -- which is how a refusal stops being evidence. The argument in full is in
       * air_metal.h's refusal list.
       */
      return "one of Metal's SOURCE 1 factors: dual-source blending reads a SECOND "
             "fragment output at [[color(0), index(1)]], and neither this contract nor "
             "this frontend carries one -- air_shader_info has one colour per target and "
             "names no index. The gap is this library's, not a6xx's: freedreno reports "
             "one dual-source target";
   return NULL;
}

/*
 * MTLBlendFactor as pipe_blendfactor. The table above says what a cast would have done
 * with each row.
 */
static bool
blend_factor_from_metal(unsigned f, bool alpha_channel, unsigned *out, const char *what,
                        char *why, size_t why_size)
{
   static const unsigned factor[] = {
      PIPE_BLENDFACTOR_ZERO,               /* MTLBlendFactorZero                     0 */
      PIPE_BLENDFACTOR_ONE,                /* ...One                                 1 */
      PIPE_BLENDFACTOR_SRC_COLOR,          /* ...SourceColor                         2 */
      PIPE_BLENDFACTOR_INV_SRC_COLOR,      /* ...OneMinusSourceColor                 3 */
      PIPE_BLENDFACTOR_SRC_ALPHA,          /* ...SourceAlpha                         4 */
      PIPE_BLENDFACTOR_INV_SRC_ALPHA,      /* ...OneMinusSourceAlpha                 5 */
      PIPE_BLENDFACTOR_DST_COLOR,          /* ...DestinationColor                    6 */
      PIPE_BLENDFACTOR_INV_DST_COLOR,      /* ...OneMinusDestinationColor            7 */
      PIPE_BLENDFACTOR_DST_ALPHA,          /* ...DestinationAlpha                    8 */
      PIPE_BLENDFACTOR_INV_DST_ALPHA,      /* ...OneMinusDestinationAlpha            9 */
      PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, /* ...SourceAlphaSaturated               10 */
      PIPE_BLENDFACTOR_CONST_COLOR,        /* ...BlendColor            11  REFUSED, above */
      PIPE_BLENDFACTOR_INV_CONST_COLOR,    /* ...OneMinusBlendColor    12  REFUSED */
      PIPE_BLENDFACTOR_CONST_ALPHA,        /* ...BlendAlpha            13  REFUSED */
      PIPE_BLENDFACTOR_INV_CONST_ALPHA,    /* ...OneMinusBlendAlpha    14  REFUSED */
      PIPE_BLENDFACTOR_SRC1_COLOR,         /* ...Source1Color          15  REFUSED */
      PIPE_BLENDFACTOR_INV_SRC1_COLOR,     /* ...OneMinusSource1Color  16  REFUSED */
      PIPE_BLENDFACTOR_SRC1_ALPHA,         /* ...Source1Alpha          17  REFUSED */
      PIPE_BLENDFACTOR_INV_SRC1_ALPHA,     /* ...OneMinusSource1Alpha  18  REFUSED */
   };
   if (f >= ARRAY_SIZE(factor)) {
      say(why, why_size, "%s is %u, which is not one of MTLBlendFactor's nineteen values",
          what, f);
      return false;
   }
   const char *no = blend_factor_refusal(f);
   if (no) {
      say(why, why_size, "%s is %s, %s", what, blend_factor_name(f), no);
      return false;
   }
   /*
    * MTLBlendFactorSourceAlphaSaturated AS AN ALPHA FACTOR, and this is a narrow refusal
    * over a narrow disagreement: for the three colour channels the factor is
    * min(As, 1 - Ad) in Metal and in GL alike, and for the ALPHA channel GL defines it
    * as 1. Both back ends implement GL's: softpipe's alpha cases are "multiply alpha by
    * 1.0" and "NO-OP, leave blend_dest as-is" where its RGB cases compute the minimum
    * (sp_quad_blend.c), and freedreno's own a2xx driver says it in a comment --
    * "hardware doesn't support SRC_ALPHA_SATURATE for alpha, but it is equivalent to
    * ONE" (fd2_blend.c), which is the GL reading stated as a property of Adreno
    * hardware; a6xx passes the factor straight through with no fix-up at all
    * (fd_blend_factor in freedreno_util.c, no special case in fd6_blend.cc).
    *
    * Apple's own enumeration carries no such exception and Metal writes one equation for
    * all four channels. So the colour half is served -- the two readings agree on it --
    * and the alpha half is refused by the field's name, because serving it would hand a
    * Metal application GL's answer for a number the NEXT composite reads.
    */
   if (alpha_channel && f == 10 /* MTLBlendFactorSourceAlphaSaturated */) {
      say(why, why_size, "%s is MTLBlendFactorSourceAlphaSaturated, and the two "
                         "specifications disagree about what that means for the ALPHA "
                         "channel: GL (and softpipe, sp_quad_blend.c) define the alpha "
                         "factor as 1 where Metal's own equation gives min(As, 1 - Ad) "
                         "for all four channels. It is served for the RGB factors, where "
                         "they agree. Nothing on this device says which the Adreno's "
                         "blender does", what);
      return false;
   }
   *out = factor[f];
   return true;
}

/*
 * MTLBlendOperation as pipe_blend_func. The identity, written out rather than cast.
 */
static bool
blend_operation_from_metal(unsigned op, unsigned *out, const char *what, char *why,
                           size_t why_size)
{
   static const unsigned func[] = {
      PIPE_BLEND_ADD,              /* MTLBlendOperationAdd             0 */
      PIPE_BLEND_SUBTRACT,         /* ...Subtract                      1 */
      PIPE_BLEND_REVERSE_SUBTRACT, /* ...ReverseSubtract               2 */
      PIPE_BLEND_MIN,              /* ...Min                           3 */
      PIPE_BLEND_MAX,              /* ...Max                           4 */
   };
   if (op >= ARRAY_SIZE(func)) {
      say(why, why_size, "%s is %u, which is not one of MTLBlendOperation's five values",
          what, op);
      return false;
   }
   *out = func[op];
   return true;
}

/*
 * MTLColorWriteMask as Gallium's colormask: FOUR BITS IN THE OTHER ORDER.
 *
 * Written as a table over all sixteen masks and not as four shifts, so that what it does
 * can be read rather than worked out -- and so that the four palindromes (0x0, 0x6, 0x9,
 * 0xf), which are the masks a cast gets right and therefore the masks a careless test
 * would be built out of, are visible as the exceptions they are.
 */
static bool
color_write_mask_from_metal(unsigned m, unsigned *out, const char *what, char *why,
                            size_t why_size)
{
   static const unsigned mask[16] = {
      /* Metal 0x0 None        */ 0,
      /* Metal 0x1 A           */ PIPE_MASK_A,
      /* Metal 0x2 B           */ PIPE_MASK_B,
      /* Metal 0x3 B|A         */ PIPE_MASK_B | PIPE_MASK_A,
      /* Metal 0x4 G           */ PIPE_MASK_G,
      /* Metal 0x5 G|A         */ PIPE_MASK_G | PIPE_MASK_A,
      /* Metal 0x6 G|B         */ PIPE_MASK_G | PIPE_MASK_B,
      /* Metal 0x7 G|B|A       */ PIPE_MASK_G | PIPE_MASK_B | PIPE_MASK_A,
      /* Metal 0x8 R           */ PIPE_MASK_R,
      /* Metal 0x9 R|A         */ PIPE_MASK_R | PIPE_MASK_A,
      /* Metal 0xa R|B         */ PIPE_MASK_R | PIPE_MASK_B,
      /* Metal 0xb R|B|A       */ PIPE_MASK_R | PIPE_MASK_B | PIPE_MASK_A,
      /* Metal 0xc R|G         */ PIPE_MASK_R | PIPE_MASK_G,
      /* Metal 0xd R|G|A       */ PIPE_MASK_R | PIPE_MASK_G | PIPE_MASK_A,
      /* Metal 0xe R|G|B       */ PIPE_MASK_R | PIPE_MASK_G | PIPE_MASK_B,
      /* Metal 0xf All         */ PIPE_MASK_RGBA,
   };
   if (m > 0xf) {
      say(why, why_size, "%s is 0x%x, and MTLColorWriteMask is four bits "
                         "(MTLColorWriteMaskAll is 0xf)", what, m);
      return false;
   }
   *out = mask[m];
   return true;
}

/*
 * The three tables' own shape, asserted at run time rather than trusted.
 *
 * Every claim here is about THIS file and is decidable here, which is what makes it
 * worth the cycles: the factor table has one row per Metal value, each one-minus row is
 * its plain row with Gallium's invert bit set, the operation table is the identity, and
 * the mask table is the bit-reversal of its index. A row deleted, duplicated or
 * transposed by hand breaks one of those, and the library then refuses to open a device
 * at all instead of drawing with a table nobody checked.
 *
 * It does NOT check that Apple's numbering is what the left column says. Nothing on this
 * machine can: see the comment at the top of this section for how that half is
 * corroborated.
 */
static bool
blend_tables_are_shaped_right(char *why, size_t why_size)
{
   unsigned plain, inverted, got;
   char ignored[8];

   /* nineteen factors, and the one-minus of each plain factor is the plain one with
    * Gallium's invert bit set. Metal pairs them adjacently from One onwards: 1/0 is
    * One/Zero (Zero IS the one-minus of One in Gallium's construction), then 2/3, 4/5,
    * 6/7, 8/9.
    */
   static const unsigned pair[][2] = { { 1, 0 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 8, 9 } };
   for (unsigned i = 0; i < ARRAY_SIZE(pair); i++) {
      if (!blend_factor_from_metal(pair[i][0], false, &plain, "", ignored,
                                   sizeof(ignored)) ||
          !blend_factor_from_metal(pair[i][1], false, &inverted, "", ignored,
                                   sizeof(ignored)) ||
          (plain | PIPE_BLENDFACTOR_INVERT_BIT) != inverted ||
          util_blendfactor_is_inverted((enum pipe_blendfactor)plain) ||
          !util_blendfactor_is_inverted((enum pipe_blendfactor)inverted)) {
         say(why, why_size, "the blend factor table is wrong: MTLBlendFactor %u and %u "
                            "are not a factor and its one-minus form", pair[i][0],
             pair[i][1]);
         return false;
      }
   }
   if (blend_factor_from_metal(19, false, &plain, "", ignored, sizeof(ignored)) ||
       !blend_factor_name(18) || blend_factor_name(19)) {
      say(why, why_size, "the blend factor table is not nineteen values wide");
      return false;
   }
   /* the five operations, the identity */
   for (unsigned op = 0; op < 5; op++) {
      if (!blend_operation_from_metal(op, &got, "", ignored, sizeof(ignored)) ||
          got != op || !blend_operation_name(op)) {
         say(why, why_size, "the blend operation table is wrong at MTLBlendOperation %u",
             op);
         return false;
      }
   }
   if (blend_operation_from_metal(5, &got, "", ignored, sizeof(ignored)) ||
       blend_operation_name(5)) {
      say(why, why_size, "the blend operation table is not five values wide");
      return false;
   }
   /* and the write mask: the reversal of the nibble, every one of the sixteen */
   for (unsigned m = 0; m <= 0xf; m++) {
      unsigned want = ((m & 0x8) ? PIPE_MASK_R : 0) | ((m & 0x4) ? PIPE_MASK_G : 0) |
                      ((m & 0x2) ? PIPE_MASK_B : 0) | ((m & 0x1) ? PIPE_MASK_A : 0);
      if (!color_write_mask_from_metal(m, &got, "", ignored, sizeof(ignored)) ||
          got != want) {
         say(why, why_size, "the colour write mask table is wrong at MTLColorWriteMask "
                            "0x%x: it gives 0x%x where the reversal of the nibble is "
                            "0x%x", m, got, want);
         return false;
      }
   }
   if (color_write_mask_from_metal(0x10, &got, "", ignored, sizeof(ignored))) {
      say(why, why_size, "the colour write mask table accepts a fifth bit");
      return false;
   }
   return true;
}

/* Whether two mapped attachments ask for the same thing, which is what decides whether
 * independent blending has to be turned on (and therefore whether a screen without it
 * can serve this pipeline at all). */
static bool
rt_blend_equal(const struct pipe_rt_blend_state *a, const struct pipe_rt_blend_state *b)
{
   return a->blend_enable == b->blend_enable && a->rgb_func == b->rgb_func &&
          a->rgb_src_factor == b->rgb_src_factor &&
          a->rgb_dst_factor == b->rgb_dst_factor && a->alpha_func == b->alpha_func &&
          a->alpha_src_factor == b->alpha_src_factor &&
          a->alpha_dst_factor == b->alpha_dst_factor && a->colormask == b->colormask;
}

/*
 * THE WHOLE BLEND STATE, from the shape's `color_blend[]` to one pipe_blend_state.
 *
 * `color_blend_count` of 0 is a shape that carries no blend state, and it produces
 * EXACTLY what this library made before these fields existed:
 * `{ .rt[0].colormask = PIPE_MASK_RGBA }`. Every draw of the corpus, every pixel-compared
 * fragment shader and every one of air_metal_test's own designated initialisers is that
 * case, so the proven path is not merely preserved, it is the same struct.
 */
/*
 * With independent blending off Gallium says rt[0] governs every target, and freedreno and
 * softpipe read only rt[0] then; panfrost reads rt[c] for every target c regardless
 * (pan_blend.c), so rt[1..] left zero were write mask 0 and blending "Off": on the Mali
 * nothing but target 0 was ever written. Mesa's own GL state tracker copies rt[0] to every
 * target and sets max_rt, and so does this, which every driver reads the same way.
 */
static void
blend_copy_rt0(struct pipe_blend_state *b)
{
   if (!b->independent_blend_enable)
      for (unsigned i = 1; i < PIPE_MAX_COLOR_BUFS; i++)
         b->rt[i] = b->rt[0];
   /* and max_rt, which panfrost takes as how many targets the state describes
    * (rt_count = max_rt + 1): left 0, every target past the first was "Off" */
   b->max_rt = PIPE_MAX_COLOR_BUFS - 1;
}

static bool
blend_state_from_metal_inner(const struct rwa_render_shape *shape, struct pipe_screen *screen,
                             struct pipe_blend_state *out, unsigned *enabled_mask,
                             unsigned *write_masks, char *why, size_t why_size);

static bool
blend_state_from_metal(const struct rwa_render_shape *shape, struct pipe_screen *screen,
                       struct pipe_blend_state *out, unsigned *enabled_mask,
                       unsigned *write_masks, char *why, size_t why_size)
{
   bool ok = blend_state_from_metal_inner(shape, screen, out, enabled_mask, write_masks,
                                          why, why_size);
   if (ok)
      blend_copy_rt0(out);
   return ok;
}

static bool
blend_state_from_metal_inner(const struct rwa_render_shape *shape, struct pipe_screen *screen,
                       struct pipe_blend_state *out, unsigned *enabled_mask,
                       unsigned *write_masks, char *why, size_t why_size)
{
   *out = (struct pipe_blend_state){ .rt[0].colormask = PIPE_MASK_RGBA };
   *enabled_mask = 0;
   *write_masks = 0;

   if (shape->color_blend_count == 0) {
      /* The state every draw before now used, and the mask reported for it is the
       * PIPE_MASK_RGBA actually programmed on render target 0 -- which governs them all
       * with independent blending off.
       */
      for (unsigned i = 0; i < shape->color_count && i < 8; i++)
         *write_masks |= PIPE_MASK_RGBA << (4 * i);
      return true;
   }
   if (shape->color_blend_count < shape->color_count) {
      say(why, why_size, "this pipeline has %u colour attachment%s and blend state for "
                         "only %u: the attachment%s after that would silently draw with "
                         "blending off and all four channels written, which is what a "
                         "struct filled in only as far as the caller cared about looks "
                         "like", shape->color_count, shape->color_count == 1 ? "" : "s",
          shape->color_blend_count, shape->color_count - shape->color_blend_count == 1
                                       ? "" : "s");
      return false;
   }
   if (shape->color_blend_count > 8) {
      say(why, why_size, "blend state for %u colour attachments, and Metal's descriptor "
                         "has eight", shape->color_blend_count);
      return false;
   }

   out->max_rt = shape->color_count ? shape->color_count - 1 : 0;

   for (unsigned i = 0; i < shape->color_count; i++) {
      const struct rwa_color_blend *b = &shape->color_blend[i];
      struct pipe_rt_blend_state *rt = &out->rt[i];
      char what[160];
      unsigned v;

      snprintf(what, sizeof(what), "colour attachment %u's writeMask", i);
      if (!color_write_mask_from_metal(b->write_mask, &v, what, why, why_size))
         return false;
      rt->colormask = v;
      *write_masks |= v << (4 * i);

      if (!b->blending_enabled) {
         /*
          * AND NOTHING ELSE OF THIS ENTRY IS READ, which is Metal's own rule -- with
          * blending disabled the source replaces the destination and the factors and
          * operations are not consulted -- and on this device it is also the difference
          * between a correct home screen and a doubly blended one.
          *
          * 504 of the 1079 pipelines in this device's own CoreAnimation archive blend
          * INSIDE THE FRAGMENT SHADER and leave this descriptor untouched, at Apple's
          * defaults: source factor One, write mask 0xf, both operations Add. Every one of
          * those fields is non-zero, so any rule of the form "some field is set,
          * therefore this descriptor means to blend" would enable the blender for all
          * 504 and blend them twice. `blendingEnabled` is the only field that says so,
          * and a refusal over a factor such a pipeline was never going to use would
          * refuse a perfectly legal one.
          */
         rt->blend_enable = 0;
         continue;
      }

      /*
       * Blending on an integer attachment has no meaning, and NOTHING BELOW THIS LINE
       * would say so. Metal forbids it, GL and Vulkan forbid it, and in Mesa the refusal
       * lives in the GL state tracker (st_atom_blend.c skips blend_enable for a draw
       * buffer in _IntegerDrawBuffers) -- which this library does not go through.
       * freedreno excludes pure-integer formats from PIPE_BIND_BLENDABLE as a
       * CAPABILITY (fd6_screen.cc) and then programs the blender with whatever it is
       * given; softpipe has no integer guard in sp_quad_blend.c at all and would
       * reinterpret the integer bit patterns as floats. So the sentence has to be
       * written here, with the format named.
       */
      const struct rwa_pixel_format *pf = pixel_format(shape->color_format[i]);
      if (pf && util_format_is_pure_integer(pf->pipe)) {
         say(why, why_size, "colour attachment %u is %s and this pipeline enables "
                            "blending on it: a blend equation over integers has no "
                            "defined meaning in Metal, in GL or in Vulkan", i, pf->name);
         return false;
      }

      rt->blend_enable = 1;
      *enabled_mask |= 1u << i;

      snprintf(what, sizeof(what), "colour attachment %u's rgbBlendOperation", i);
      if (!blend_operation_from_metal(b->rgb_blend_operation, &v, what, why, why_size))
         return false;
      rt->rgb_func = v;
      snprintf(what, sizeof(what), "colour attachment %u's alphaBlendOperation", i);
      if (!blend_operation_from_metal(b->alpha_blend_operation, &v, what, why, why_size))
         return false;
      rt->alpha_func = v;

      snprintf(what, sizeof(what), "colour attachment %u's sourceRGBBlendFactor", i);
      if (!blend_factor_from_metal(b->source_rgb_blend_factor, false, &v, what, why,
                                   why_size))
         return false;
      rt->rgb_src_factor = v;
      snprintf(what, sizeof(what), "colour attachment %u's destinationRGBBlendFactor", i);
      if (!blend_factor_from_metal(b->destination_rgb_blend_factor, false, &v, what, why,
                                   why_size))
         return false;
      rt->rgb_dst_factor = v;
      snprintf(what, sizeof(what), "colour attachment %u's sourceAlphaBlendFactor", i);
      if (!blend_factor_from_metal(b->source_alpha_blend_factor, true, &v, what, why,
                                   why_size))
         return false;
      rt->alpha_src_factor = v;
      snprintf(what, sizeof(what), "colour attachment %u's destinationAlphaBlendFactor",
               i);
      if (!blend_factor_from_metal(b->destination_alpha_blend_factor, true, &v, what, why,
                                   why_size))
         return false;
      rt->alpha_dst_factor = v;

      /*
       * MIN AND MAX WITH A FACTOR OTHER THAN ONE, refused by name, and this is the one
       * refusal here that is about a disagreement between the two SPECIFICATIONS rather
       * than about something missing.
       *
       * Metal writes one equation for every operation -- source times its factor OP
       * destination times its factor -- so under Metal's own reading
       * MTLBlendOperationMin with a source factor of SourceAlpha takes the minimum of
       * the SCALED terms. GL does not: it defines MIN and MAX as min(Cs, Cd) and
       * max(Cs, Cd) with the factors IGNORED.
       *
       * Gallium's own convention is GL's, and it is enforced NOWHERE BELOW THE STATE
       * TRACKER: st_atom_blend.c rewrites both factors to PIPE_BLENDFACTOR_ONE when the
       * equation is GL_MIN or GL_MAX ("Min/max are special"), and that is the only place
       * it happens. Below it the two back ends behind this library do different things
       * with a factor that survives -- softpipe multiplies into source[] and
       * blend_dest[] and THEN takes the minimum (sp_quad_blend.c), while fd6_blend.cc
       * writes the factors into A6XX_RB_MRT_BLEND_CONTROL beside the MIN opcode without
       * inspecting them, so what the Adreno's blender does with a scaled minimum is not
       * readable from here at all.
       *
       * One is the identity under both readings, so every Min or Max that asks for One
       * and One is served and is the only form anything real uses (it is how a
       * maximum-blend is written). Anything else is refused with the field named,
       * because the alternative is a picture that is right on one back end and wrong on
       * the other, with no way to tell from the picture which.
       */
      for (unsigned half = 0; half < 2; half++) {
         unsigned op = half ? rt->alpha_func : rt->rgb_func;
         unsigned sf = half ? rt->alpha_src_factor : rt->rgb_src_factor;
         unsigned df = half ? rt->alpha_dst_factor : rt->rgb_dst_factor;
         unsigned msf = half ? b->source_alpha_blend_factor : b->source_rgb_blend_factor;
         unsigned mdf = half ? b->destination_alpha_blend_factor
                             : b->destination_rgb_blend_factor;

         if ((op != PIPE_BLEND_MIN && op != PIPE_BLEND_MAX) ||
             (sf == PIPE_BLENDFACTOR_ONE && df == PIPE_BLENDFACTOR_ONE))
            continue;
         say(why, why_size, "colour attachment %u's %sBlendOperation is %s and its "
                            "factors are %s and %s: Metal's equation multiplies by the "
                            "factors and then takes the minimum or maximum, GL ignores "
                            "the factors for these two operations entirely, and the two "
                            "readings differ unless both factors are "
                            "MTLBlendFactorOne -- which is served, and is the only form "
                            "a maximum-blend is written in", i,
             half ? "alpha" : "rgb",
             blend_operation_name(half ? b->alpha_blend_operation
                                       : b->rgb_blend_operation),
             blend_factor_name(msf), blend_factor_name(mdf));
         return false;
      }

      /*
       * A format with no alpha channel, and this is a FIX-UP rather than a refusal
       * because both APIs agree on what it means: an attachment that stores no alpha
       * reads as alpha = 1 on the destination side. Gallium has the conversion by name
       * (util_blend_dst_alpha_to_one, src/util/blend.h: DST_ALPHA becomes ONE and
       * INV_DST_ALPHA becomes ZERO) and it is applied here, where the format is known,
       * rather than left to a driver that may or may not do it -- on a surface with no
       * alpha the undisturbed factor reads whatever the hardware keeps in those bits,
       * which on an RGBX surface is not 1 and not reliably 0 either.
       */
      if (pf && !util_format_has_alpha(pf->pipe)) {
         rt->rgb_dst_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->rgb_dst_factor);
         rt->rgb_src_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->rgb_src_factor);
         rt->alpha_dst_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->alpha_dst_factor);
         rt->alpha_src_factor =
            util_blend_dst_alpha_to_one((enum pipe_blendfactor)rt->alpha_src_factor);
      }
   }

   /*
    * Independent blending, which Gallium does not merely allow but REQUIRES to be
    * declared: with independent_blend_enable clear, rt[0] governs every attachment and
    * rt[1..] are not read at all. So a pipeline whose attachments differ needs the bit,
    * and a screen without the capability has to be told no by name -- the alternative is
    * the second attachment silently taking the first one's equation.
    */
   bool differ = false;
   for (unsigned i = 1; i < shape->color_count; i++)
      differ = differ || !rt_blend_equal(&out->rt[i], &out->rt[0]);
   if (differ) {
      bool enable = screen->caps.indep_blend_enable;
      bool func = screen->caps.indep_blend_func;
      if (!enable || !func) {
         say(why, why_size, "this pipeline's %u colour attachments do not all ask for "
                            "the same blend state, and %s reports %s: with independent "
                            "blending off Gallium reads render target 0's state for "
                            "every attachment, so the others would silently blend with "
                            "the first one's equation", shape->color_count,
             screen->get_name(screen),
             !enable ? "no independent blend enable at all"
                     : "independent enables but a single blend equation");
         return false;
      }
      out->independent_blend_enable = 1;
   }
   return true;
}

/* rwa_render_constants: what the next rwa_render_create on this thread takes */
#define RWA_MAX_CONSTANTS 128
static _Thread_local struct rwa_constant next_vertex_constants[RWA_MAX_CONSTANTS],
   next_fragment_constants[RWA_MAX_CONSTANTS];
static _Thread_local unsigned next_vertex_count, next_fragment_count;

void
rwa_render_constants(const struct rwa_constant *vertex, unsigned vertex_count,
                     const struct rwa_constant *fragment, unsigned fragment_count)
{
   RWA_GUARD();
   next_vertex_count = MIN2(vertex_count, RWA_MAX_CONSTANTS);
   next_fragment_count = MIN2(fragment_count, RWA_MAX_CONSTANTS);
   if (vertex)
      memcpy(next_vertex_constants, vertex, next_vertex_count * sizeof(*vertex));
   if (fragment)
      memcpy(next_fragment_constants, fragment, next_fragment_count * sizeof(*fragment));
}

/* the values a runtime handed over, laid on the zeros air_module_function_constants made */
static void
take_constants(struct air_constant_value *values, unsigned count,
               const struct rwa_constant *given, unsigned given_count)
{
   for (unsigned i = 0; i < count; i++)
      for (unsigned k = 0; k < given_count; k++)
         if (given[k].index == values[i].index)
            values[i].bits = given[k].bits;
}

struct rwa_render *
rwa_render_create(struct rwa_device *device,
                  const void *vertex_bitcode, size_t vertex_size,
                  const char *vertex_entry,
                  const void *fragment_bitcode, size_t fragment_size,
                  const char *fragment_entry,
                  const struct rwa_render_shape *shape,
                  struct rwa_render_info *info, char *why, size_t why_size)
{
   RWA_GUARD();
   int64_t made_began = os_time_get_nano();
   if (!shape) {
      say(why, why_size, "a render pipeline with no attachment description");
      return NULL;
   }
   if (!fragment_bitcode || fragment_size < 4) {
      say(why, why_size, "%zu bytes is not a fragment shader module", fragment_size);
      return NULL;
   }
   /* No colour attachment is allowed only with a depth one, which is Metal's own rule
    * (a depth-only pass is a shadow map) and is the one case where a draw that writes
    * no colour still produces something a caller can read. Without either there is
    * nothing for the fragment shader to write at all, and the draw would "succeed"
    * having left no mark.
    */
   if (shape->color_count == 0 && shape->depth_format == 0 && shape->stencil_format == 0) {
      say(why, why_size, "a render pipeline with no colour attachments and no depth "
                         "attachment: there would be nothing for the fragment shader to "
                         "write");
      return NULL;
   }
   if (shape->color_count > PIPE_MAX_COLOR_BUFS) {
      say(why, why_size, "%u colour attachments, more than the %u this GPU can bind",
          shape->color_count, (unsigned)PIPE_MAX_COLOR_BUFS);
      return NULL;
   }
   /*
    * The pass's sample count. Metal's own MTLRenderPipelineDescriptor.sampleCount
    * defaults to 1, and 0 here is a caller's struct filled in only as far as it cared
    * about, so both mean one sample per pixel.
    *
    * Above one, every attachment of the pass is a multisample texture of exactly this
    * count. That is Metal's rule (the pipeline's sampleCount must be the attachments')
    * and Gallium's too, in a way worth knowing: a framebuffer's sample count is read
    * back off the attachment's own resource and NOT from pipe_framebuffer_state.samples,
    * which util_framebuffer_get_num_samples looks at only when there is no attachment
    * at all. So the agreement is checked here and at the draw rather than being carried
    * in the framebuffer state.
    *
    * What a multisample pass is FOR here is the only thing that fills a multisample
    * texture, in Metal or on this GPU: rendering into one. Nothing writes one from the
    * processor (rwa_texture_write says why at length), so a pass like this is how the
    * six multisample fragment shaders of AppleMetalGLRenderer get something defined to
    * read.
    */
   unsigned samples = shape->sample_count ? shape->sample_count : 1;
   /*
    * The depth/stencil attachment's format. Metal's pipeline descriptor has two
    * properties and one texture behind them (air_metal.h), so the two have to name the
    * same format: equal, or one of them zero. Two different formats is a request for a
    * separate depth surface and stencil surface, which neither Metal's own render pass
    * descriptor nor a Gallium framebuffer can hold, and honouring one of the two would
    * silently drop the other.
    */
   unsigned zs_format = shape->depth_format ? shape->depth_format : shape->stencil_format;
   if (shape->depth_format && shape->stencil_format &&
       shape->depth_format != shape->stencil_format) {
      say(why, why_size, "a depth attachment of pixel format %u and a stencil attachment "
                         "of %u: one texture carries both aspects, so this library renders "
                         "into one attachment and cannot be given two",
          shape->depth_format, shape->stencil_format);
      return NULL;
   }
   if (zs_format) {
      const struct rwa_pixel_format *fmt = pixel_format(zs_format);
      if (!fmt) {
         say(why, why_size, "the depth/stencil attachment: pixel format %u is %s", zs_format,
             pixel_format_refusal(zs_format));
         return NULL;
      }
      if (!format_is_depth_stencil(fmt->pipe)) {
         say(why, why_size, "%s as the depth/stencil attachment: it is a colour format, and "
                            "a pass tests depth against a depth format", fmt->name);
         return NULL;
      }
      /* Which aspect the format actually has, against which the pipeline asked for.
       * Metal lets an application set stencilAttachmentPixelFormat to a depth-only
       * format and get no stencil; here that is said rather than discovered at the
       * draw, because a stencil test against a buffer with no stencil aspect passes
       * every fragment and looks like a stencil test that works.
       */
      if (shape->stencil_format && !format_has_stencil(fmt->pipe)) {
         say(why, why_size, "%s as the stencil attachment: that format has a depth aspect "
                            "and no stencil one, so there would be nothing to test",
             fmt->name);
         return NULL;
      }
      if (shape->depth_format && !format_has_depth(fmt->pipe)) {
         say(why, why_size, "%s as the depth attachment: that format has a stencil aspect "
                            "and no depth one, so there would be nothing to test",
             fmt->name);
         return NULL;
      }
      unsigned q = samples > 1 ? samples : 0;
      if (!device->screen->is_format_supported(device->screen, fmt->pipe, PIPE_TEXTURE_2D,
                                               q, q, PIPE_BIND_DEPTH_STENCIL)) {
         char ms[48] = "";
         if (q)
            snprintf(ms, sizeof(ms), " of %u samples per pixel", q);
         say(why, why_size, "%s cannot use %s%s as a depth/stencil attachment",
             rwa_device_name(device), fmt->name, ms);
         return NULL;
      }
   }

   /* Each colour format known and renderable, checked here so a bad one is a sentence
    * naming it rather than a failure deep in resource or surface creation. The sample
    * count asked is zero and not one for a single-sample pass, for the reason
    * rwa_texture_create spells out: a non-multisample attachment is zero samples to
    * Gallium and freedreno refuses some bindings asked with one. A multisample pass
    * asks with its own count, and that is where softpipe says no -- which is the whole
    * of why the measurement below this is the Adreno's and not this machine's.
    */
   for (unsigned i = 0; i < shape->color_count; i++) {
      const struct rwa_pixel_format *fmt = pixel_format(shape->color_format[i]);
      if (!fmt) {
         say(why, why_size, "colour attachment %u: pixel format %u is %s", i,
             shape->color_format[i], pixel_format_refusal(shape->color_format[i]));
         return NULL;
      }
      /* Named here rather than left to is_format_supported, which would decline it with
       * a sentence about the GPU: a depth format in a colour slot is a descriptor that
       * put the attachment in the wrong place, and that is what the caller needs told.
       */
      if (format_is_depth_stencil(fmt->pipe)) {
         say(why, why_size, "colour attachment %u is %s, a depth or stencil format: it "
                            "belongs in the depth attachment of the shape and not among "
                            "the colours", i, fmt->name);
         return NULL;
      }
      unsigned q = samples > 1 ? samples : 0;
      if (!device->screen->is_format_supported(device->screen, fmt->pipe,
                                               PIPE_TEXTURE_2D, q, q,
                                               PIPE_BIND_RENDER_TARGET)) {
         char ms[48] = "";
         if (q)
            snprintf(ms, sizeof(ms), " of %u samples per pixel", q);
         say(why, why_size, "%s cannot render into %s%s", rwa_device_name(device),
             fmt->name, ms);
         return NULL;
      }
   }

   struct rwa_render *render = calloc(1, sizeof(*render));
   if (!render) {
      say(why, why_size, "out of memory making a render pipeline");
      return NULL;
   }
   render->device = device;
   render->mem_ctx = ralloc_context(NULL);
   render->shape = *shape;

   struct rwa_render_info out = { 0 };

   /*
    * The translation without the lock (26 October): AIR -> NIR asks nothing of the context (the
    * function constants handed over are this thread's, the translator keeps no state), and it was
    * some 40% of a pipeline's 100 to 250 ms -- time the worker's draws waited, which is the hitch
    * of an effect seen for the first time (run 202, "rwm pipelines"). Taken again before the
    * driver compiles (make_stage_cso) and on the way out.
    */
   bool unlocked = pthread_mutex_unlock(&rwa_lock) == 0;

   /* The fragment shader first. Its AIR -> NIR errors surface here, at creation, which
    * is the half of docs/metal.md 10.18 a driver must put in the right NSError: a link
    * error between the stages cannot surface until the draw, because ir3 compiles and
    * links at draw time (ir3_cache_lookup), so the two belong to two different calls.
    */
   struct air_constant_value *ffc;
   unsigned num_ffc =
      air_module_function_constants(render->mem_ctx, fragment_bitcode, fragment_size, &ffc);
   take_constants(ffc, num_ffc, next_fragment_constants, next_fragment_count);
   next_fragment_count = 0;
   const struct air_to_nir_options fopts = { .constants = ffc, .num_constants = num_ffc };
   const char *error;
   nir_shader *fs = air_to_nir(render->mem_ctx, fragment_bitcode, fragment_size,
                               fragment_entry, &fopts, device->nir_options, &render->finfo,
                               &error);
   if (!fs) {
      say(why, why_size, "%s", error ? error : "the fragment module could not be translated");
      goto fail;
   }
   if (render->finfo.stage != MESA_SHADER_FRAGMENT) {
      say(why, why_size, "%s is a %s shader, and this makes render pipelines with a "
                         "fragment stage",
          render->finfo.entry_point ? render->finfo.entry_point : "the function",
          render->finfo.stage >= MESA_SHADER_VERTEX &&
          render->finfo.stage < MESA_SHADER_STAGES
             ? _mesa_shader_stage_to_string(render->finfo.stage) : "non-fragment");
      goto fail;
   }
   slot_shapes(fs, render->frag_slots, render->frag_compare_sampler);
   count_resources(&render->finfo, render->frag_slots, &out.fragment_buffers,
                   &out.fragment_textures, &out.fragment_samplers);
   out.fragment_writes_depth = fragment_writes_depth(fs) ? 1 : 0;

   /* A shader that stores to [[depth]] with nowhere to store it is refused here, by name.
    *
    * This is not tidiness. texturedFragment2DDepth is one of Apple's own, it stores a
    * depth and no colour, and the draw sweep built it a pipeline with three colour
    * attachments and no depth one: the dispatch never came back and took the whole boot
    * with it (logs/082415-run.log, where the sweep's own "about to draw" line is the last
    * thing in it). Metal's rule is the same one -- a pipeline whose fragment function
    * writes depth needs a depthAttachmentPixelFormat -- so refusing is both what the API
    * says and what keeps a GPU answering.
    */
   if (out.fragment_writes_depth && !shape->depth_format) {
      say(why, why_size, "%s stores to [[depth]] and this pipeline has no depth attachment, "
                         "so there is nowhere for that store to go",
          render->finfo.entry_point ? render->finfo.entry_point : "the fragment function");
      goto fail;
   }

   /* The vertex stage: translated when a module was supplied, generated otherwise. The
    * generated stage is the road with evidence behind it -- 255 fragment shaders drawn
    * over a hand-built quad (docs/air.md) -- and the only one this library has run end
    * to end. A supplied module is translated and bound, but a vertex shader that reads
    * vertex attributes is refused, because the contract carries no layout to feed them.
    */
   nir_shader *vs;
   if (vertex_bitcode) {
      if (vertex_size < 4) {
         say(why, why_size, "%zu bytes is not a vertex shader module", vertex_size);
         goto fail;
      }
      struct air_constant_value *vfc;
      unsigned num_vfc =
         air_module_function_constants(render->mem_ctx, vertex_bitcode, vertex_size, &vfc);
      take_constants(vfc, num_vfc, next_vertex_constants, next_vertex_count);
      const struct air_to_nir_options vopts = { .constants = vfc, .num_constants = num_vfc };
      vs = air_to_nir(render->mem_ctx, vertex_bitcode, vertex_size, vertex_entry, &vopts,
                      device->nir_options, &render->vinfo, &error);
      if (!vs) {
         say(why, why_size, "%s", error ? error : "the vertex module could not be translated");
         goto fail;
      }
      if (render->vinfo.stage != MESA_SHADER_VERTEX) {
         say(why, why_size, "%s is a %s shader, and the vertex stage of a render pipeline "
                            "takes a vertex function",
             render->vinfo.entry_point ? render->vinfo.entry_point : "the function",
             render->vinfo.stage >= MESA_SHADER_VERTEX &&
             render->vinfo.stage < MESA_SHADER_STAGES
                ? _mesa_shader_stage_to_string(render->vinfo.stage) : "non-vertex");
         goto fail;
      }
      /*
       * The vertex attributes, which used to be the end of this road: a shader with a
       * [[stage_in]] input was refused by name because the contract carried no layout,
       * and that refusal is what the phone's window server ran into 120 times
       * (docs/metal.md 10.44). Now the layout is in the shape and the elements are built
       * from the module's own interface.
       *
       * The two readings of "does this stage fetch anything" are compared rather than
       * one of them trusted: NIR's inputs are what ir3 will emit fetches for, and
       * air_shader_info's table is what the elements are built from, so a disagreement
       * between them would mean an element array that does not describe the fetches --
       * and it is cheaper to say so here than to find it in a picture.
       */
      const char *vn = render->vinfo.entry_point ? render->vinfo.entry_point
                                                 : "the vertex shader";
      unsigned fetched = 0;
      for (unsigned i = 0; i < render->vinfo.num_vertex_attributes; i++)
         fetched += render->vinfo.vertex_attributes[i].used ? 1 : 0;
      if (vertex_reads_attributes(vs) != (fetched != 0)) {
         say(why, why_size, "%s: the translated shader %s vertex attribute inputs and its "
                            "interface lists %u fetched attribute%s, and the elements are "
                            "built from the interface", vn,
             vertex_reads_attributes(vs) ? "has" : "has no", fetched,
             fetched == 1 ? "" : "s");
         goto fail;
      }
      if (fetched &&
          !build_vertex_state(&render->vinfo, shape, vn, device->screen, &render->vstate,
                              why, why_size))
         goto fail;
      /* A descriptor configuring attributes this module does not read is ignored, as
       * Metal ignores it: the module's interface decides what is fetched, and an
       * attribute nothing reads has no fetch to configure.
       */
      slot_shapes(vs, render->vert_slots, render->vert_compare_sampler);
      unsigned vtex = 0, vsamp = 0;
      count_resources(&render->vinfo, render->vert_slots, &out.vertex_buffers, &vtex, &vsamp);
      render->generated_vertex = false;
      out.generated_vertex_stage = 0;
   } else {
      /* A vertex descriptor with no vertex function is refused rather than ignored. The
       * generated stage draws this library's own quad out of this library's own buffer
       * -- that is the whole of what it is for -- so a layout for it would describe
       * memory the caller does not own, and honouring it would replace the one vertex
       * path that has 255 pixel-compared draws behind it with an untested one. In Metal
       * a pipeline with a vertexDescriptor and no vertexFunction is not a pipeline at
       * all; the generated stage is this library's own extension and keeps its own
       * vertices.
       */
      for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
         if (shape->vertex_attribute[i].format == 0)
            continue;
         say(why, why_size, "a vertex descriptor configures attribute %u and this "
                            "descriptor names no vertex function: the stage this library "
                            "would generate draws its own full-screen quad out of its own "
                            "buffer, and has no attribute to lay out", i);
         goto fail;
      }
      /* num_varyings of the fragment shader: one coordinate out of the generated stage
       * for each interpolated input the fragment shader reads, so the two stages link.
       */
      vs = quad_vertex_shader(device->nir_options, render->finfo.num_varyings);
      if (!vs) {
         say(why, why_size, "out of memory building the vertex stage");
         goto fail;
      }
      render->generated_vertex = true;
      out.vertex_buffers = 0;
      out.generated_vertex_stage = 1;
   }

   /* Compile both stages now. A failure here is a creation failure and says so; the
    * stages are linked later, at the draw.
    */
   int64_t made_translated = os_time_get_nano();
   if (unlocked) {
      pthread_mutex_lock(&rwa_lock);
      unlocked = false;
   }
   render->fs_cso = make_stage_cso(device, fs, &render->finfo, why, why_size);
   if (!render->fs_cso)
      goto fail;
   render->vs_cso = make_stage_cso(device, vs, render->generated_vertex ? NULL : &render->vinfo,
                                   why, why_size);
   /* the generated stage is standalone (not under mem_ctx); the driver has a clone, so
    * the source is this library's to free either way
    */
   if (render->generated_vertex)
      ralloc_free(vs);
   if (!render->vs_cso)
      goto fail;
   rwa_pipelines_made++;
   rwa_pipelines_translate_ns += made_translated - made_began;
   rwa_pipelines_compile_ns += os_time_get_nano() - made_translated;

   /*
    * THE BLEND STATE, which until now was the fixed one air_run draws with -- colormask
    * RGBA on render target 0 governing them all, blending off, every draw opaque. That
    * is still exactly what a shape with `color_blend_count` 0 produces, which is every
    * draw of the corpus and every one of air_metal_test's own initialisers; what is new
    * is that a shape which CARRIES Metal's blend state gets Metal's blend state, mapped
    * by the three tables above.
    *
    * It is worth saying what the old state did on the phone, because it was not a
    * refusal: CoreAnimation composites with premultiplied alpha, so a label layer whose
    * background is transparent black was drawn as OPAQUE BLACK -- every icon label in a
    * black box and the Search pill a black rectangle (docs/air.md section 5 has the
    * measurement and userland/rc.jobs.blend the greps, and the photograph is the test).
    *
    * The tables' own shape is checked first, here rather than at a draw, so that a row
    * edited by hand cannot reach a picture: the one-minus pairing, the identity of the
    * operations and the reversal of the mask nibble are all decidable in this file.
    */
   if (!blend_tables_are_shaped_right(why, why_size))
      goto fail;
   struct pipe_blend_state blend;
   if (!blend_state_from_metal(shape, device->screen, &blend, &out.blend_enabled_mask,
                               &out.color_write_masks, why, why_size))
      goto fail;
   render->blend = device->pipe->create_blend_state(device->pipe, &blend);
   /* The pipeline's own depth/stencil state, and it is the ZEROED one on purpose: it is
    * what a draw gets when the caller passes no rwa_depth_stencil, which is Metal's
    * state before an application has set one. depth_enabled 0 is "no test", not
    * "PIPE_FUNC_NEVER", and the difference matters -- the zeroed struct's depth_func is
    * NEVER, so a driver reading it with the test enabled would discard every fragment.
    * Every draw this library has ever made used this state, there having been no depth
    * attachment to test against.
    */
   render->dsa = device->pipe->create_depth_stencil_alpha_state(device->pipe,
      &(struct pipe_depth_stencil_alpha_state){ 0 });
   /*
    * clip_halfz, because Metal's NDC z runs from 0 at the near plane to 1 at the far
    * one where Gallium's default is OpenGL's [-1, 1]. It is what decides what a window
    * depth IS: under halfz the viewport above puts clip z = 0 at znear, which is what
    * rwa_draw's depth_range promises, and the generated vertex stage's quad has clip
    * z = 0.
    *
    * It is set only for a pipeline that HAS a depth attachment, and that is a deliberate
    * containment rather than a conviction. Metal's convention is Metal's whether a pass
    * tests depth or not, and a pipeline without a depth attachment is observably
    * identical either way -- the viewport transform is the same code, so a fragment's
    * [[position]].z is the same number, and the only thing halfz changes is which
    * primitives the clipper keeps. But that one thing is a risk I cannot measure from
    * here: 253 of Apple's fragment shaders are compared pixel for pixel on the Adreno
    * through this exact rasterizer state, and their quad sits at clip z = 0, which under
    * halfz is exactly ON the near plane rather than in the middle of the range. If a6xx
    * clipped a primitive on that boundary, every one of those draws would stop drawing.
    * So the proven path keeps the state it was proven with, and the new one gets the
    * convention it needs; if the depth draw comes back having drawn nothing at all, this
    * boundary is the first thing to look at (docs/metal.md 10.30).
    */
   /*
    * `multisample` is set from the pass's own sample count, and what it actually
    * decides on this GPU is worth saying because it is almost nothing: freedreno reads
    * it in one place, fd6_rasterizer.cc, to pick RECTANGULAR over BRESENHAM line
    * rasterisation. Sample coverage itself comes from the framebuffer's sample count,
    * which freedreno computes off the attachments (fd_set_framebuffer_state ->
    * util_framebuffer_get_num_samples) and emits with emit_msaa. So this bit is the
    * honest state for a multisample pass and not the thing that turns multisampling on.
    */
   render->rast = device->pipe->create_rasterizer_state(device->pipe,
      &(struct pipe_rasterizer_state){
         .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
         .depth_clip_near = 1, .depth_clip_far = 1, .clip_halfz = zs_format != 0,
         .multisample = samples > 1 });
   /* the same with the scissor test on, for a draw under -setScissorRect: */
   render->rast_scissor = device->pipe->create_rasterizer_state(device->pipe,
      &(struct pipe_rasterizer_state){
         .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
         .depth_clip_near = 1, .depth_clip_far = 1, .clip_halfz = zs_format != 0,
         .multisample = samples > 1, .scissor = 1 });
   if (!render->blend || !render->dsa || !render->rast) {
      say(why, why_size, "%s would not make the fixed render state", rwa_device_name(device));
      goto fail;
   }

   /*
    * The quad and its one vec4 attribute, built for BOTH paths -- and the "both" is the
    * whole point, paid for on the device.
    *
    * A supplied vertex shader reads no attributes (refused above if it did), so it has
    * no use for a vertex buffer, and binding none looks like the tidy thing to do. It is
    * not: freedreno dereferences the bound vertex-element state with no NULL check at
    * all. fd6_emit_3d_state's FD6_GROUP_VTXSTATE case is
    *
    *     state = fd6_vertex_stateobj(ctx->vtx.vtx)->stateobj;
    *
    * and `stateobj` sits at offset 0x208 of that struct -- pipe[32] is 0x180,
    * strides[32] takes it to 0x200, num_elements to 0x204 and the pointer aligns to
    * 0x208. Binding NULL therefore faults at exactly 0x208, which is where every draw
    * with a supplied vertex shader died on the Adreno while the generated stage drew
    * correctly. softpipe tolerates it, so no test on this machine could have caught it.
    *
    * So both paths now take the same vertex state, the one the generated stage has
    * already drawn with on the device: the shader module is then the only thing that
    * differs between a proven draw and a new one. An attribute the vertex shader never
    * declares is simply never fetched into a register -- ir3 emits fetches for the
    * inputs the shader has -- which is the ordinary case of a GL program with more
    * arrays enabled than it reads.
    */
   /*
    * ... and a stage that FETCHES ATTRIBUTES gets the elements the descriptor describes
    * instead, in driver_location order, which is what build_vertex_state above is for.
    * The quad is still made: a function-constant-gated attribute the descriptor leaves
    * unconfigured is fetched from it at stride 0 rather than from an empty VFD slot,
    * and the generated stage and every attribute-less vertex module still draw with it.
    */
   render->velem = render->vstate.count
      ? device->pipe->create_vertex_elements_state(device->pipe, render->vstate.count,
                                                   render->vstate.element)
      : device->pipe->create_vertex_elements_state(device->pipe, 1,
           &(struct pipe_vertex_element){
              .src_offset = 0, .src_stride = 16, .vertex_buffer_index = 0,
              .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT });
   render->vbuf = pipe_buffer_create(device->screen, PIPE_BIND_VERTEX_BUFFER,
                                     PIPE_USAGE_DEFAULT, sizeof(rwa_quad_verts));
   if (!render->velem || !render->vbuf) {
      say(why, why_size, "%s would not make the quad a draw covers the attachment with",
          rwa_device_name(device));
      goto fail;
   }
   pipe_buffer_write(device->pipe, render->vbuf, 0, sizeof(rwa_quad_verts),
                     rwa_quad_verts);

   /* What the layout became, for a driver that has to fill the stream slots and for a
    * caller that wants to compare the mapping with the shader's own interface rather
    * than trust it (air_metal.h).
    */
   out.vertex_attribute_count = render->vstate.count;
   for (unsigned i = 0; i < render->vstate.count &&
                        i < ARRAY_SIZE(out.vertex_attribute_index); i++)
      out.vertex_attribute_index[i] = render->vstate.attribute[i];
   out.vertex_streams = render->vstate.stream_count;
   out.vertex_stream_mask = render->vstate.stream_mask;

   /* And which buffer indices of each stage are ARGUMENT BUFFERS. Metal's one table per
    * stage now holds three things -- a stream, an argument and a set -- and a driver
    * that could not tell them apart would hand a set's buffer to a shader expecting a
    * constant block. A set at an index the vertex descriptor also fetches from is
    * refused below for the same reason an argument at one is.
    */
   const char *fn = render->finfo.entry_point ? render->finfo.entry_point
                                             : "the fragment function";
   const char *vn = render->vinfo.entry_point ? render->vinfo.entry_point
                                              : "the vertex function";
   if (argbuf_mask_of(&render->finfo, fn, &out.fragment_argbuf_mask, why, why_size) != 0)
      goto fail;
   if (!render->generated_vertex &&
       argbuf_mask_of(&render->vinfo, vn, &out.vertex_argbuf_mask, why, why_size) != 0)
      goto fail;
   if (out.vertex_argbuf_mask & out.vertex_stream_mask) {
      unsigned both = out.vertex_argbuf_mask & out.vertex_stream_mask;

      say(why, why_size, "the vertex descriptor fetches attributes from buffer index %u "
                         "and %s declares an argument buffer there: Metal has one table "
                         "behind -setVertexBuffer:offset:atIndex: and an index is either "
                         "a stream or a set", ffs(both) - 1, vn);
      goto fail;
   }

   if (info)
      *info = out;
   return render;

fail:
   if (unlocked)
      pthread_mutex_lock(&rwa_lock);   /* (the guard lets go of it on the way out) */
   rwa_render_destroy(render);
   return NULL;
}

void
rwa_render_destroy(struct rwa_render *render)
{
   RWA_GUARD();
   if (!render)
      return;
   struct pipe_context *pipe = render->device->pipe;

   /* Unbound before deleted, each of them, for the reason delete_compute_state and
    * rwa_sampler_destroy give: a Gallium context holds a pointer to the state bound in
    * it, and softpipe's delete is a plain FREE, so a state deleted while bound leaves
    * the context holding freed memory until something is bound over it.
    */
   if (render->vs_cso) {
      pipe->bind_vs_state(pipe, NULL);
      pipe->delete_vs_state(pipe, render->vs_cso);
   }
   if (render->fs_cso) {
      pipe->bind_fs_state(pipe, NULL);
      pipe->delete_fs_state(pipe, render->fs_cso);
   }
   if (render->blend) {
      pipe->bind_blend_state(pipe, NULL);
      pipe->delete_blend_state(pipe, render->blend);
   }
   if (render->dsa) {
      pipe->bind_depth_stencil_alpha_state(pipe, NULL);
      pipe->delete_depth_stencil_alpha_state(pipe, render->dsa);
   }
   if (render->rast) {
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->delete_rasterizer_state(pipe, render->rast);
      if (render->rast_scissor)
         pipe->delete_rasterizer_state(pipe, render->rast_scissor);
   }
   if (render->velem) {
      /* Unbound before deleted, and the vertex buffer unbound with it: the context holds
       * a reference of its own to whatever is bound there (set_vertex_buffers took it),
       * and a draw does not clear it afterwards, so this is where it goes.
       */
      pipe->bind_vertex_elements_state(pipe, NULL);
      pipe->set_vertex_buffers(pipe, 0, NULL);
      pipe->delete_vertex_elements_state(pipe, render->velem);
   }
   if (render->vbuf)
      pipe_resource_reference(&render->vbuf, NULL);

   for (unsigned i = 0; i < RWA_MAX_TEXTURES; i++) {
      if (render->frag_own_sampler[i])
         pipe->delete_sampler_state(pipe, render->frag_own_sampler[i]);
      if (render->vert_own_sampler[i])
         pipe->delete_sampler_state(pipe, render->vert_own_sampler[i]);
   }

   ralloc_free(render->mem_ctx);
   free(render);
}

/*
 * Whether one attachment of a pass has the shape the pipeline was compiled for, in the
 * two ways that are not its pixel format.
 *
 * Metal spells "more than one sample" twice -- in the texture's TYPE
 * (MTLTextureType2DMultisample) and in its sampleCount -- and requires the pipeline's
 * sampleCount to be the attachment's. Both are checked, in both directions: a
 * multisample texture in a one-sample pass and a plain 2D texture in a multisample pass
 * are each a descriptor that disagrees with itself, and neither can be honoured by
 * halves. Gallium gives no second opinion to fall back on here, because a framebuffer's
 * sample count IS the attachment's resource's (util_framebuffer_get_num_samples): a
 * mismatch would silently become whatever the first attachment happened to be.
 */
static bool
attachment_shape_fits(const struct rwa_texture *t, unsigned samples, const char *what,
                      char *why, size_t why_size)
{
   unsigned want_type = samples > 1 ? RWA_TYPE_2D_MULTISAMPLE : RWA_TYPE_2D;
   if (t->shape.type != want_type) {
      say(why, why_size, "%s is a %s and a pass of %u sample%s per pixel renders into a %s",
          what, texture_type_name(t->shape.type), samples, samples == 1 ? "" : "s",
          texture_type_name(want_type));
      return false;
   }
   unsigned have = t->res->nr_samples > 1 ? t->res->nr_samples : 1;
   if (have != samples) {
      say(why, why_size, "%s has %u sample%s per pixel and the pipeline was made for %u",
          what, have, have == 1 ? "" : "s", samples);
      return false;
   }
   return true;
}

int
rwa_render_draw(struct rwa_render *render, const struct rwa_draw *draw,
                char *why, size_t why_size)
{
   RWA_GUARD();
   uint64_t t0 = os_time_get_nano();
   struct rwa_device *device = render->device;
   struct pipe_context *pipe = device->pipe;
   const char *fname = render->finfo.entry_point ? render->finfo.entry_point
                                                 : "the fragment shader";

   if (!draw) {
      say(why, why_size, "a draw with no description");
      return -1;
   }
   /* The attachments of the pass must be the attachments the pipeline was compiled for:
    * Metal requires the render pass descriptor's colour formats to match the pipeline
    * state's, and a fragment shader writing a colour a different format would read back
    * as the right bits the wrong way.
    */
   if (draw->color_count != render->shape.color_count) {
      say(why, why_size, "drawn into %u colour attachments and the pipeline was made for "
                         "%u", draw->color_count, render->shape.color_count);
      return -1;
   }
   /* And the depth attachment either is there or is not, on both sides. Metal requires
    * the pass's formats to be the pipeline's; a pass with a depth texture the pipeline
    * knows nothing of would be tested against state the pipeline was not compiled with,
    * and a pipeline made for depth drawn without one would quietly do no test -- which
    * is the failure that reads as "depth testing does not work here".
    */
   unsigned zs_format = render->shape.depth_format ? render->shape.depth_format
                                                   : render->shape.stencil_format;
   if (!!draw->depth != !!zs_format) {
      say(why, why_size, "%s and %s", draw->depth ? "the pass has a depth attachment"
                                                  : "the pass has no depth attachment",
          zs_format ? "the pipeline was made with one" : "the pipeline was made without one");
      return -1;
   }

   enum mesa_prim prim;
   if (!mesa_prim_from_metal(draw->primitive, &prim, why, why_size))
      return -1;
   if (draw->vertex_count == 0) {
      /* A draw of no vertices is the quad trap from the other side: it rasterises
       * nothing and leaves every attachment as it was, which reads like a shader that
       * stored nothing rather than like a draw that never ran.
       */
      say(why, why_size, "a draw of no vertices, which would leave every attachment as it "
                         "is");
      return -1;
   }

   /*
    * Indexed or not, and everything the index buffer has to satisfy before a draw is
    * made of it. rwa_draw's comment carries the reasons; this is the order they are
    * checked in, which is cheapest-and-most-wrong first: the type decides the size, the
    * size decides whether the offset converts to Gallium's element start at all, and
    * only then is it worth reading the indices.
    *
    * `index_size` 0 below means "not indexed" exactly as it does in pipe_draw_info, so
    * the draw at the bottom reads one field and not a flag plus a field.
    */
   unsigned index_size = 0, index_start = 0;
   unsigned min_index = draw->vertex_start;
   unsigned max_index = draw->vertex_start + draw->vertex_count - 1;
   if (!draw->index_buffer) {
      /* Metal's non-indexed forms have no baseVertex and no index type or offset to
       * give. A caller that filled one in asked for something -drawPrimitives: cannot
       * express -- or is a driver whose copy of this struct is a different shape, which
       * is the same sentence and the more likely one.
       */
      if (draw->base_vertex != 0 || draw->index_type != 0 ||
          draw->index_buffer_offset != 0) {
         say(why, why_size, "a draw with no index buffer, and a base vertex of %d, index "
                            "type %u and index buffer offset %zu: -drawPrimitives: has "
                            "none of the three", draw->base_vertex, draw->index_type,
             draw->index_buffer_offset);
         return -1;
      }
      if (render->generated_vertex && draw->vertex_start + draw->vertex_count > 4) {
         say(why, why_size, "vertices %u..%u of the generated full-screen quad, which has "
                            "four (0..3)", draw->vertex_start,
             draw->vertex_start + draw->vertex_count - 1);
         return -1;
      }
   } else {
      if (draw->vertex_start != 0) {
         say(why, why_size, "an indexed draw starting at vertex %u: Metal's indexed draws "
                            "have no vertexStart, and where the indices start is the index "
                            "buffer offset", draw->vertex_start);
         return -1;
      }
      if (!index_size_from_metal(draw->index_type, &index_size, why, why_size))
         return -1;
      /* Apple's own rule, in Apple's own words in MTLRenderCommandEncoder.h:
       * "indexBufferOffset must be a multiple of the index size". Gallium counts the
       * start in elements, so this division is the whole of the conversion and it has to
       * be exact.
       */
      if (draw->index_buffer_offset % index_size != 0) {
         say(why, why_size, "an index buffer offset of %zu bytes with %s, whose indices "
                            "are %u bytes each: Metal requires the offset to be a "
                            "multiple of the index size", draw->index_buffer_offset,
             index_type_name(draw->index_type), index_size);
         return -1;
      }
      size_t need = draw->index_buffer_offset + (size_t)draw->vertex_count * index_size;
      size_t have = rwa_buffer_size(draw->index_buffer);
      if (need > have) {
         /* Not a formality: past the end, softpipe reads index 0 instead ("if the index
          * buffer would overflow we return index 0", draw_private.h's DRAW_GET_IDX) and
          * a6xx clamps the draw against max_indices computed from the resource's width.
          * Both of those are a picture, and neither is the one that was asked for.
          */
         say(why, why_size, "%u indices of %u bytes at offset %zu want %zu bytes of an "
                            "index buffer that holds %zu", draw->vertex_count, index_size,
             draw->index_buffer_offset, need, have);
         return -1;
      }
      index_start = (unsigned)(draw->index_buffer_offset / index_size);
      index_range((const uint8_t *)rwa_buffer_map(draw->index_buffer) +
                  draw->index_buffer_offset, draw->vertex_count, index_size, &min_index,
                  &max_index);
      /* The generated vertex stage's own quad has four corners and this library owns the
       * buffer they are in, so an index past it is not the application's mistake to make
       * -- it is a fetch outside a resource of ours, and both back ends answer one with
       * a vertex rather than an error. This is the indexed form of the vertex_start
       * check above, with base_vertex where Metal puts it.
       */
      if (render->generated_vertex) {
         int64_t low = (int64_t)min_index + draw->base_vertex;
         int64_t high = (int64_t)max_index + draw->base_vertex;
         if (low < 0 || high > 3) {
            say(why, why_size, "indices %u..%u and a base vertex of %d, which is vertices "
                               "%lld..%lld of the generated full-screen quad, and it has "
                               "four (0..3)", min_index, max_index, draw->base_vertex,
                (long long)low, (long long)high);
            return -1;
         }
      }
   }
   /* Metal's instanceCount, with zero read as one: see rwa_draw. An instance count of
    * one is what every draw this library made before this field existed asked for, and
    * what Metal's non-instanced forms mean.
    */
   unsigned instances = draw->instance_count ? draw->instance_count : 1;

   /*
    * THE VERTEX STREAMS, checked here -- before a surface or a binding is made, so a
    * refusal costs nothing to undo -- and bound further down with the elements.
    *
    * This runs only for a pipeline whose vertex stage fetches attributes. Every other
    * draw binds the library's own quad at slot 0, exactly as it did before the layout
    * existed, and passes nothing here.
    */
   const struct rwa_vertex_state *vst = &render->vstate;
   int64_t low_vertex = (int64_t)min_index + (index_size ? draw->base_vertex : 0);
   int64_t high_vertex = (int64_t)max_index + (index_size ? draw->base_vertex : 0);
   for (unsigned k = 0; vst->count && k < RWA_MAX_VERTEX_BUFFERS; k++) {
      if (!(vst->stream_mask & (1u << k)))
         continue;
      const struct rwa_vertex_stream *s =
         k < draw->vertex_stream_count && draw->vertex_streams ? &draw->vertex_streams[k]
                                                               : NULL;
      if (!s || !s->buffer) {
         say(why, why_size, "the vertex descriptor fetches from buffer index %u and "
                            "nothing is bound there (the draw covers %u stream slot%s): "
                            "the fetch would read whatever that slot held", k,
             draw->vertex_stream_count, draw->vertex_stream_count == 1 ? "" : "s");
         return -1;
      }
      size_t have = rwa_buffer_size(s->buffer);
      if (s->offset > have) {
         say(why, why_size, "vertex stream %u is bound at offset %zu of a buffer that "
                            "holds %zu bytes", k, s->offset, have);
         return -1;
      }
      /* The element this draw's last vertex reads, against what is left of the buffer.
       * Only a PER-VERTEX stream is bounded this way: which element an instanced stream
       * reads is the hardware's arithmetic over the base instance and the divisor, and a
       * bound computed from the wrong formula would refuse legal draws (air_metal.h).
       */
      if (!(vst->per_vertex & (1u << k)))
         continue;
      int64_t first = (int64_t)s->offset + (int64_t)vst->stride[k] * low_vertex;
      if (first < 0) {
         say(why, why_size, "vertex %lld of stream %u is %lld bytes before the start of "
                            "its buffer: a base vertex of %d at a stride of %u",
             (long long)low_vertex, k, (long long)-first, draw->base_vertex,
             vst->stride[k]);
         return -1;
      }
      size_t need = (size_t)((int64_t)s->offset +
                             (int64_t)vst->stride[k] * high_vertex) + vst->bytes[k];
      if (need > have) {
         say(why, why_size, "vertex %lld of stream %u ends %zu bytes into a buffer that "
                            "holds %zu: %u attribute byte%s at a stride of %u from offset "
                            "%zu", (long long)high_vertex, k, need, have, vst->bytes[k],
             vst->bytes[k] == 1 ? "" : "s", vst->stride[k], s->offset);
         return -1;
      }
   }

   /* Each colour attachment: a 2D texture, made as a render target, of the format the
    * pipeline was compiled for, and all the same size -- one render pass has one size.
    */
   unsigned samples = render->shape.sample_count ? render->shape.sample_count : 1;
   unsigned w = 0, h = 0;
   for (unsigned i = 0; i < draw->color_count; i++) {
      struct rwa_texture *t = draw->color ? draw->color[i] : NULL;
      char what[48];
      if (!t) {
         say(why, why_size, "colour attachment %u is not a texture", i);
         return -1;
      }
      snprintf(what, sizeof(what), "colour attachment %u", i);
      if (!attachment_shape_fits(t, samples, what, why, why_size))
         return -1;
      if (!(t->res->bind & PIPE_BIND_RENDER_TARGET)) {
         say(why, why_size, "colour attachment %u (%s) was made without "
                            "MTLTextureUsageRenderTarget", i, t->format_name);
         return -1;
      }
      if (t->shape.pixel_format != render->shape.color_format[i]) {
         say(why, why_size, "colour attachment %u is pixel format %u and the pipeline's "
                            "attachment %u is %u", i, t->shape.pixel_format, i,
             render->shape.color_format[i]);
         return -1;
      }
      unsigned tw = t->shape.width;
      unsigned th = t->shape.height ? t->shape.height : 1;
      if (i == 0) {
         w = tw;
         h = th;
      } else if (tw != w || th != h) {
         say(why, why_size, "colour attachment %u is %ux%u and attachment 0 is %ux%u", i,
             tw, th, w, h);
         return -1;
      }
   }

   /* The depth/stencil attachment, by the same checks and one more: it is the only
    * attachment whose format decides which Gallium binding it needed, so a texture made
    * without MTLTextureUsageRenderTarget has no PIPE_BIND_DEPTH_STENCIL and could not be
    * a zsbuf at all.
    */
   if (draw->depth) {
      struct rwa_texture *t = draw->depth;
      if (!attachment_shape_fits(t, samples, "the depth attachment", why, why_size))
         return -1;
      if (!format_is_depth_stencil(t->res->format)) {
         say(why, why_size, "the depth attachment is %s, which is a colour format",
             t->format_name);
         return -1;
      }
      if (t->shape.pixel_format != zs_format) {
         say(why, why_size, "the depth attachment is pixel format %u and the pipeline's is "
                            "%u", t->shape.pixel_format, zs_format);
         return -1;
      }
      if (!(t->res->bind & PIPE_BIND_DEPTH_STENCIL)) {
         say(why, why_size, "the depth attachment (%s) was made without "
                            "MTLTextureUsageRenderTarget, so the GPU was never told it "
                            "could be drawn into", t->format_name);
         return -1;
      }
      unsigned tw = t->shape.width;
      unsigned th = t->shape.height ? t->shape.height : 1;
      if (draw->color_count == 0) {
         w = tw;        /* a depth-only pass: the depth attachment is the pass's size */
         h = th;
      } else if (tw != w || th != h) {
         say(why, why_size, "the depth attachment is %ux%u and colour attachment 0 is %ux%u",
             tw, th, w, h);
         return -1;
      }
   }


   uint64_t t1 = os_time_get_nano();
   rwa_draw_ns[0] += t1 - t0;
   /* Everything each stage declares has to be bound, the same check a dispatch makes. */
   if (check_bindings(&render->finfo, render->frag_slots, &draw->fragment,
                      draw->fragment_buffer_table, draw->fragment_buffer_table_count, fname, why,
                      why_size) != 0)
      return -1;
   const char *vname = NULL;
   if (!render->generated_vertex) {
      vname = render->vinfo.entry_point ? render->vinfo.entry_point : "the vertex shader";
      if (check_bindings(&render->vinfo, render->vert_slots, &draw->vertex,
                         draw->vertex_buffer_table, draw->vertex_buffer_table_count, vname, why,
                         why_size) != 0)
         return -1;
   }

   uint64_t t2 = os_time_get_nano();
   rwa_draw_ns[1] += t2 - t1;
   /* The framebuffer, from the caller's own textures. create_surface wants a context,
    * so it is a draw-time cost and not a pipeline one, and it is released again below
    * before the caller may destroy the textures under it.
    */
   struct pipe_surface *surf[PIPE_MAX_COLOR_BUFS] = { 0 };
   struct pipe_surface *zsurf = NULL;
   /* pipe_framebuffer_state.samples is filled in although nothing here reads it: both
    * drivers take the pass's sample count off the attachments instead
    * (util_framebuffer_get_num_samples ignores this field whenever there is one), and a
    * field left at zero that happens to mean "one sample" is a thing to be found out by
    * the next driver rather than by this comment.
    */
   struct pipe_framebuffer_state fb = { .width = w, .height = h,
                                        .nr_cbufs = draw->color_count,
                                        .samples = samples };
   for (unsigned i = 0; i < draw->color_count; i++) {
      struct rwa_texture *t = (struct rwa_texture *)draw->color[i];
      if (!t->rt)
         t->rt = pipe->create_surface(pipe, t->res,
            &(struct pipe_surface){ .format = t->res->format });
      surf[i] = t->rt;
      if (!surf[i]) {
         say(why, why_size, "%s would not make a render surface of colour attachment %u",
             rwa_device_name(device), i);
         return -1;
      }
      fb.cbufs[i] = surf[i];
   }
   if (draw->depth) {
      struct rwa_texture *t = (struct rwa_texture *)draw->depth;
      if (!t->rt)
         t->rt = pipe->create_surface(pipe, t->res,
            &(struct pipe_surface){ .format = t->res->format });
      zsurf = t->rt;
      if (!zsurf) {
         say(why, why_size, "%s would not make a render surface of the depth attachment",
             rwa_device_name(device));
         return -1;
      }
      fb.zsbuf = zsurf;
   }
   /*
    * (Was off unless RWA_FLUSH_ON_SWITCH was set -- now on, see below: the crash it was put in against was panfrost writing
    * past a pool allocation, answered with slack in pan_mempool.c, and sending passes off early
    * split a pass that CoreAnimation returns to into two, each with its own preload.)
    * One pass in flight at a time: a draw into other attachments than the last one's sends
    * the passes so far off first. Left to itself panfrost keeps a batch per framebuffer and,
    * when one samples what another draws, submits that other one from inside this draw --
    * and that path ended the window server in pan_preload_fb_part, writing a descriptor past
    * the end of a pool's mapping (24 October, twice). The passes are sent in the order
    * CoreAnimation made them either way.
    */
   static int flush_on_switch = -1;
   if (flush_on_switch < 0) {
      /* on unless RWA_FLUSH_ON_SWITCH=0, since 26 October: a frame's passes kept until its end left
       * the GPU idle while the processor recorded them and the processor idle while the GPU drew,
       * 8 + 11 ms one after the other -- 28 frames a second (run 148) */
      const char *flag = getenv("RWA_FLUSH_ON_SWITCH");
      flush_on_switch = flag == NULL || strcmp(flag, "0") != 0;
   }
   if (flush_on_switch) {
      static struct pipe_surface *last_cbuf, *last_zs;
      if ((fb.nr_cbufs ? fb.cbufs[0] : NULL) != last_cbuf || fb.zsbuf != last_zs) {
         if (last_cbuf || last_zs)
            pipe->flush(pipe, NULL, 0);
         last_cbuf = fb.nr_cbufs ? fb.cbufs[0] : NULL;
         last_zs = fb.zsbuf;
      }
   }
   pipe->set_framebuffer_state(pipe, &fb);

   /* The viewport, and now its z as well. Gallium's window depth is
    * clip_z/w * scale[2] + translate[2], and with clip_halfz (the rasterizer state
    * above) the depth range [znear, zfar] is scale = zfar - znear, translate = znear --
    * which is what util_viewport_zmin_zmax reads back out of it (util/u_viewport.h, and
    * freedreno hands exactly that to GRAS_CL_Z_CLAMP). The default is { 0, 1 }, the
    * identity, which is the 1.0/0.0 every draw before depth existed used.
    */
   float znear = draw->depth_range ? draw->depth_range[0] : 0.0f;
   float zfar = draw->depth_range ? draw->depth_range[1] : 1.0f;
   /* -setViewport:'s rectangle where one was given (Metal's: origin top left, y down) */
   float vx = draw->viewport ? draw->viewport[0] : 0.0f, vy = draw->viewport ? draw->viewport[1] : 0.0f;
   float vw = draw->viewport ? draw->viewport[2] : (float)w, vh = draw->viewport ? draw->viewport[3] : (float)h;
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
      .scale = { vw / 2.0f, -(vh / 2.0f), zfar - znear },
      .translate = { vx + vw / 2.0f, vy + vh / 2.0f, znear } });
   if (draw->scissor) {
      unsigned x0 = MIN2(draw->scissor[0], w), y0 = MIN2(draw->scissor[1], h);
      unsigned x1 = MIN2(draw->scissor[0] + draw->scissor[2], w), y1 = MIN2(draw->scissor[1] + draw->scissor[3], h);

      pipe->set_scissor_states(pipe, 0, 1, &(struct pipe_scissor_state){
         .minx = x0, .miny = y0, .maxx = x1, .maxy = y1 });
   }

   pipe->bind_blend_state(pipe, render->blend);
   /* The caller's depth/stencil state, or the pipeline's zeroed one, which is Metal's
    * behaviour with no MTLDepthStencilState set: no test and no write.
    */
   pipe->bind_depth_stencil_alpha_state(pipe, draw->depth_stencil ? draw->depth_stencil->cso
                                                                  : render->dsa);
   /* Metal keeps the reference value on the encoder and not in the state object, so it
    * is set per draw. Gallium's is one byte per face, which is the stencil buffer's
    * width; Metal's is a uint32 and only those bits can match anything.
    */
   pipe->set_stencil_ref(pipe, (struct pipe_stencil_ref){
      .ref_value = { (uint8_t)(draw->stencil_reference[0] & 0xff),
                     (uint8_t)(draw->stencil_reference[1] & 0xff) } });
   pipe->bind_rasterizer_state(pipe, draw->scissor && render->rast_scissor ? render->rast_scissor : render->rast);
   pipe->bind_vs_state(pipe, render->vs_cso);
   pipe->bind_fs_state(pipe, render->fs_cso);

   /*
    * The vertex state, bound on BOTH paths and never NULL.
    *
    * It used to be bound only for the generated stage, on the reasoning that a supplied
    * vertex shader reads no attributes and so needs no vertex buffer. That reasoning is
    * sound about the shader and wrong about the driver: freedreno reads the bound
    * vertex-element state with no NULL check at all. fd6_emit_3d_state's
    * FD6_GROUP_VTXSTATE case is
    *
    *     state = fd6_vertex_stateobj(ctx->vtx.vtx)->stateobj;
    *
    * and `stateobj` is at offset 0x208 of that struct (pipe[32] is 0x180, strides[32]
    * takes it to 0x200, num_elements to 0x204, and the pointer aligns to 0x208). So
    * clearing it faulted at exactly 0x208 on the Adreno, for every draw with a supplied
    * vertex shader, while the generated stage drew correctly -- and softpipe tolerates a
    * NULL there, so nothing on this machine could have caught it.
    *
    * Binding the same state on both paths also means the shader module is the only thing
    * that differs between a draw already proven on the device and a new one. An
    * attribute the vertex shader never declares is never fetched into a register -- ir3
    * emits fetches for the inputs the shader has -- which is the ordinary case of a GL
    * program with more arrays enabled than it reads.
    *
    * A reference is taken for the driver, not handed to it out of ours.
    *
    * set_vertex_buffers TAKES OWNERSHIP of the reference in the pipe_vertex_buffer:
    * both drivers here pass take_ownership=true to util_set_vertex_buffers_* (softpipe
    * in sp_state_vertex.c, freedreno in freedreno_state.c), so a caller that passes
    * its own only reference has given it away and must not release it again. This did
    * release it again -- the pipeline's destroy unreferenced render->vbuf -- which
    * freed the quad out from under a context still holding it, and softpipe's own
    * teardown then decremented a dead reference. It aborted on
    * `pipe_reference_described: count != -1` in 53 of the corpus's 255 drawn fragment
    * shaders and not in the other 202, which is what a use-after-free looks like when
    * the assert only fires if the freed slab happens to hold a zero at that offset.
    */
   /*
    * The streams, when the stage fetches attributes: one pipe_vertex_buffer per slot
    * from 0 to the highest index any element names, because set_vertex_buffers takes a
    * COUNT from slot 0 where Metal's -setVertexBuffer:atIndex: takes an index. A slot no
    * element names is left empty on purpose -- a descriptor that uses index 5 alone is
    * an array of six with five empty entries -- and the slot one past Metal's highest is
    * where the placeholder for a gated, unconfigured attribute reads this library's own
    * quad.
    */
   struct pipe_vertex_buffer vbs[PIPE_MAX_ATTRIBS] = { 0 };
   unsigned nvb = 1;
   if (vst->count) {
      nvb = MIN2(vst->stream_count, (unsigned)PIPE_MAX_ATTRIBS);
      for (unsigned k = 0; k < nvb; k++) {
         if (k == RWA_MAX_VERTEX_BUFFERS) {
            pipe_resource_reference(&vbs[k].buffer.resource, render->vbuf);
            continue;
         }
         if (!(vst->stream_mask & (1u << k)))
            continue;
         const struct rwa_vertex_stream *s = &draw->vertex_streams[k];
         vbs[k].buffer_offset = (unsigned)s->offset;
         pipe_resource_reference(&vbs[k].buffer.resource, s->buffer->res);
      }
   } else {
      pipe_resource_reference(&vbs[0].buffer.resource, render->vbuf);
   }
   /* Elements first, buffers second, which is the order p_context.h requires of a
    * frontend: "Frontends MUST call set_vertex_buffers after bind_vertex_elements_state
    * and before the next draw", because a driver is allowed to merge the two in
    * set_vertex_buffers rather than in draw_vbo. Neither driver here does, so the
    * other order worked; it worked by permission and not by contract.
    */
   pipe->bind_vertex_elements_state(pipe, render->velem);
   pipe->set_vertex_buffers(pipe, nvb, vbs);

   struct rwa_bound fbound = { 0 }, vbound = { 0 };
   int rc = -1;
   uint64_t t3 = os_time_get_nano(), t4 = t3, t5 = t3;
   rwa_draw_ns[2] += t3 - t2;
   if (bind_stage(device, PIPE_SHADER_FRAGMENT, &render->finfo, render->frag_slots,
                  render->frag_own_sampler,
                  render->frag_compare_sampler,
                  &draw->fragment, draw->fragment_buffer_table, draw->fragment_buffer_table_count,
                  fname, &fbound, why, why_size) != 0)
      goto unbind;
   if (!render->generated_vertex &&
       bind_stage(device, PIPE_SHADER_VERTEX, &render->vinfo, render->vert_slots,
                  render->vert_own_sampler,
                  render->vert_compare_sampler,
                  &draw->vertex, draw->vertex_buffer_table, draw->vertex_buffer_table_count, vname,
                  &vbound, why, why_size) != 0)
      goto unbind;

   /* clear_color NULL is MTLLoadActionLoad: keep what the attachment holds. A pointer
    * to four floats is MTLLoadActionClear, and the union is filled from them -- for an
    * integer attachment the bits of the float land in the right place when the colour
    * is zero, which is the one air_run uses.
    *
    * Depth and stencil are each their own pointer and so their own load action, because
    * a pass that clears the depth and keeps the stencil is ordinary and one action for
    * both would be a lie about one of them. Gallium takes all three in one call with a
    * mask, and a mask of nothing means no clear at all -- which is three load actions
    * of Load and the case where this does nothing.
    */
   {
      union pipe_color_union clear;
      unsigned mask = 0;
      for (unsigned c = 0; c < 4; c++)
         clear.f[c] = draw->clear_color ? draw->clear_color[c] : 0.0f;
      if (draw->clear_color)
         mask |= PIPE_CLEAR_COLOR;
      /* An aspect is cleared only if the attachment has it: clearing the stencil of a
       * Depth32Float buffer is a caller's mistake, but Gallium's clear would either
       * ignore it or touch a plane that is not there, and neither is worth finding out
       * at a draw. The format decides, and the format was checked at creation.
       */
      if (draw->clear_depth && draw->depth && format_has_depth(draw->depth->res->format))
         mask |= PIPE_CLEAR_DEPTH;
      if (draw->clear_stencil && draw->depth &&
          format_has_stencil(draw->depth->res->format))
         mask |= PIPE_CLEAR_STENCIL;
      if (mask)
         pipe->clear(pipe, mask, NULL, &clear,
                     draw->clear_depth ? (double)*draw->clear_depth : 0.0,
                     draw->clear_stencil ? *draw->clear_stencil & 0xff : 0);
   }

   {
      /*
       * The one call both forms go through, which is Gallium's shape and not two of
       * ours: `index_size` 0 is a non-indexed draw there as it is here, `start` is in
       * elements of whatever is being walked (vertices, or indices from the index
       * buffer's base), and `index_bias` is the base vertex. Nothing below is
       * conditional except what the index buffer itself provides.
       *
       * primitive_restart stays off, and that is a decision: Metal has no restart index
       * anywhere in MTLRenderCommandEncoder, so 0xFFFF is an ordinary index and a strip
       * drawn with one would break into two primitives for nobody's reason.
       *
       * take_index_buffer_ownership stays off as well: the reference belongs to the
       * caller's rwa_buffer, which outlives this draw and may be drawn with again.
       */
      struct pipe_draw_info info = {
         .mode = prim, .index_bounds_valid = true,
         .index_size = (uint16_t)index_size,
         .min_index = min_index, .max_index = max_index,
         .instance_count = instances,
         .start_instance = draw->base_instance,
      };
      if (index_size)
         info.index.resource = draw->index_buffer->res;
      struct pipe_draw_start_count_bias sc = {
         .start = index_size ? index_start : draw->vertex_start,
         .count = draw->vertex_count,
         .index_bias = index_size ? draw->base_vertex : 0,
      };
      t4 = os_time_get_nano();
      pipe->draw_vbo(pipe, &info, 0, NULL, &sc, 1);
      t5 = os_time_get_nano();
      rwa_draw_ns[3] += t4 - t3;
      rwa_draw_ns[4] += t5 - t4;
   }

   /* Waited for here, on the same grounds as a dispatch: a picture that was never drawn
    * must not read as a result, and this is the call that can say which draw did not
    * come back (docs/air.md section 5). A link error between the stages, which ir3 only
    * finds now, surfaces as this not finishing.
    */
   /* ...was. Waiting for every draw kept the GPU and the CPU taking turns, and a frame of
    * CoreAnimation is dozens of draws, so the window server ran at a few frames a second.
    * The draw is sent off and not waited for; the command buffer waits once, at its end
    * (rwa_finish from redwoodCommitWork), before anything shows the picture. Not even
    * flushed: the next draw into the same attachments joins this one's pass. */
   rc = 0;

unbind:
   unbind_stage(device, PIPE_SHADER_FRAGMENT, &fbound);
   if (!render->generated_vertex)
      unbind_stage(device, PIPE_SHADER_VERTEX, &vbound);
   /* The framebuffer is released before the caller's next call, which may be
    * rwa_texture_destroy on one of these attachments: a context still holding a surface
    * of a freed resource is the same trap as a bound sampler state (rwa_sampler_destroy).
    * The surfaces themselves are the textures' own (rwa_texture.rt) and stay.
    */
   pipe->set_framebuffer_state(pipe, &(struct pipe_framebuffer_state){ 0 });
   rwa_draw_ns[5] += os_time_get_nano() - t5;
   rwa_draw_ns[6]++;
   return rc;
}
