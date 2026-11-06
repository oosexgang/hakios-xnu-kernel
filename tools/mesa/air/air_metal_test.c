/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air_metal_test: the library of air_metal.h, driven the way the Metal driver will
 * drive it, on this machine and without the phone.
 *
 * It exists to answer one question that air_run cannot: does a caller holding nothing
 * but air_metal.h and a function's bitcode get a pipeline and a dispatch out of it?
 * air_run is linked against the whole frontend and reaches into air_shader_info for
 * every binding it makes; this one knows only air_metal.h and calls every one of its
 * functions, so anything the contract fails to provide shows up here as code that
 * cannot be written.
 *
 * The textures were where that bit hardest, and the gap was worth naming: the contract
 * reported how many textures a kernel binds and not what shape or format each one wants,
 * because a driver never needs to ask -- the application's MTLTextureDescriptor says.
 * So this program picks, per slot, out of the third argument (a 4x4 2D or 4x4x4 3D
 * texture of one of the three 32-bit RGBA formats), and the library's refusal names
 * what the shader wanted when the pick is wrong: "reads texture 0 as a texture2d<uint>
 * and MTLPixelFormatRGBA32Float is a floating-point or normalised format" is this test
 * being told what to pass next, and every spelling in the lines below was found that
 * way.
 *
 * The gap is now closed from the other side as well: rwa_module_textures answers what a
 * module declares, because a test standing in for an application has no descriptor to
 * read and Metal's own pipeline reflection has to report the same thing. --textures
 * prints that answer for a whole library, so a caller no longer has to find the spelling
 * by being refused.
 *
 * The .metallib is taken apart here rather than by the library, because that is how it
 * goes on the device too: Metal has already parsed the container and -[_MTLFunction
 * bitcodeData] hands the driver one module (docs/metal.md 10.12, where those bytes were
 * checked against this frontend's own reader over all eleven functions of AV1SW --
 * same lengths, same FNV-1a, 11 of 11). The container's layout is docs/metal.md section
 * 9, and tools/metallib.py reads the same fields from Python.
 *
 * The screen is softpipe, through RWA_SOFTWARE=1, which this program sets itself unless
 * the environment already says: there is no Adreno on this machine and the point is the
 * path, not the hardware. RWA_SOFTWARE=0 asks for the GPU and so prints what a driver
 * on a phone booted without gpu=1 is told.
 *
 *   air_metal_test FILE.metallib [FUNCTION [MTLPixelFormat[,...] [FILL]]]
 *   air_metal_test --textures FILE.metallib [FUNCTION]
 *   air_metal_test --draw FILE.metallib FUNCTION
 *                  [MTLPixelFormat[,...] [MTLPixelFormat[:MTLTextureType][,...]]]
 *   air_metal_test --draw-depth FILE.metallib FUNCTION
 *                  [MTLPixelFormat[,...]|none [DEPTHFORMAT]]
 *   air_metal_test --draw-ms FILE.metallib FUNCTION [SAMPLES [FILLFUNCTION]]
 *   air_metal_test --draw-index FILE.metallib FUNCTION [VERTEXFUNCTION]
 *   air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION FUNCTION
 *                  [MTLPixelFormat[,...] [SLOT:COMPONENTS|scale:S[,...]
 *                  [MTLPixelFormat[:MTLTextureType][,...]]]]
 *   air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION FRAGMENTLIB FRAGMENTFUNCTION
 *                  [GATEDVERTEXFUNCTION]
 *   air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB FRAGMENTFUNCTION
 *   air_metal_test --argbufs FILE[.metallib|.bc] [FUNCTION]
 *   air_metal_test --draw-argbuf SETMODULE DIRECTMODULE
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* --attrib-sweep builds one pipeline per child process, because softpipe ASSERTS on a
 * 16-bit opcode rather than declining it (nir_to_tgsi.c's "Unknown NIR opcode") and an
 * abort in the middle of a sweep of 141 modules would report nothing about the other
 * 140. Nothing else in this program forks.
 */
#include <sys/wait.h>
#include <unistd.h>

#include "air_metal.h"

#define WHY 512
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* The texture is 4x4 and so is the grid, which is air_run's TEX_SIZE and GRID: the
 * same texel pattern read by the same number of threads, so a number out of this
 * program and a number out of that one are the same measurement.
 */
#define TEX 4
#define TEXELS (TEX * TEX * TEX * 4)

/* The texture types and pixel formats this program can fill. Only three formats,
 * because the texel values below (100 + x, 200 + y, 300 + x + y + z, 400) have to be
 * exact in whatever format holds them -- in RGBA8Unorm they would all clamp to 1.0 and
 * a kernel reading the wrong texel would read the right answer. The library translates
 * forty-two formats and all ten texture types it can allocate (air_metal.c); this
 * program writes known texels into three formats and two types, which is what Apple's
 * compute kernels bind.
 */
enum {
   MTL_TEXTURE_TYPE_2D = 2, MTL_TEXTURE_TYPE_2D_MULTISAMPLE = 4, MTL_TEXTURE_TYPE_3D = 7,
   MTL_USAGE_SHADER_READ = 1, MTL_USAGE_SHADER_WRITE = 2, MTL_USAGE_RENDER_TARGET = 4,
   MTL_RGBA32_UINT = 123, MTL_RGBA32_SINT = 124, MTL_RGBA32_FLOAT = 125,
   /* and the depth formats --draw-depth makes an attachment of. Depth32Float is the
    * default there because it is one plane on both back ends and stores 0.25, 0.5 and
    * 0.75 exactly, so an expected depth and a measured one are comparable with == and
    * not with a tolerance that would hide a real difference.
    */
   MTL_DEPTH16_UNORM = 250, MTL_DEPTH32_FLOAT = 252, MTL_STENCIL8 = 253,
   MTL_DEPTH32_FLOAT_STENCIL8 = 260,
};

/* MTLVertexFormat and MTLVertexStepFunction, the few --draw-attrib configures and the
 * few it expects to be refused. The numbers are Apple's own, out of the iOS 16.5 SDK's
 * MTLVertexDescriptor.h -- the same header userland/src/redwoodmetal.m asserts the
 * library's table against -- and MTLVertexFormatFloatRGB9E5 (55) is in that list
 * BECAUSE it is not in that header: iOS 17 added it, so nothing on the device can ask
 * for it, and what this program checks about it is that the sentence names it rather
 * than printing a number.
 */
enum {
   MTL_VF_UCHAR4_NORMALIZED = 9, MTL_VF_SHORT4_NORMALIZED = 24, MTL_VF_HALF2 = 25,
   MTL_VF_UCHAR4_NORMALIZED_BGRA = 42,
   MTL_VF_FLOAT2 = 29, MTL_VF_FLOAT4 = 31, MTL_VF_INT4 = 35,
   MTL_VF_UINT1010102_NORMALIZED = 41, MTL_VF_FLOAT_RGB9E5 = 55,
   MTL_STEP_CONSTANT = 0, MTL_STEP_PER_VERTEX = 1, MTL_STEP_PER_INSTANCE = 2,
   MTL_STEP_PER_PATCH = 3,
};

/* air_run's texel: (100 + x + 1000n, 200 + y, 300 + x + y + z, 400) for slot n. Every
 * component differs from every other, and from the same component of another slot and
 * another texel, so a kernel reading the wrong texel or the wrong slot cannot come
 * back with the right number -- and neither can a readback with the wrong stride.
 */
static void
texel_value(unsigned slot, unsigned x, unsigned y, unsigned z, float out[4])
{
   out[0] = 100.0f + (float)x + 1000.0f * (float)slot;
   out[1] = 200.0f + (float)y;
   out[2] = 300.0f + (float)x + (float)y + (float)z;
   out[3] = 400.0f;
}

/* where texel (x, y, z) sits in a tightly packed level of TEX x TEX x TEX */
static unsigned
texel_index(unsigned x, unsigned y, unsigned z)
{
   return ((z * TEX + y) * TEX + x) * 4;
}

/*
 * The packed 16-bit probe, and the one pixel a draw read back.
 *
 * Metal's four packed 16-bit colour formats are five, six and one bit wide per channel,
 * so this program's texel pattern -- 100 + x and up -- cannot be written into one at
 * all, and a texture filled with the nearest representable thing would measure nothing.
 * Instead every texel gets ONE 16-BIT WORD WITH ONE FIELD SET, and the question the
 * draw answers is which CHANNEL that field became. A coordinate cannot change that
 * answer, which is why every texel is the same.
 *
 * A file-scope variable and not an argument because it reaches the middle of run_draw,
 * which already takes six: --pixel-formats sets it for each probe, and a texspec naming
 * a packed format outside that mode is refused rather than filled with something.
 */
static unsigned probe_fill16;
static bool probe_fill16_given;
static float last_pixel[4];
static unsigned last_word;

/* One texel, in the format's own type: a uint texture read as floats prints 1.4e-43
 * and looks like a failure of the kernel rather than of whoever printed it.
 */
static void
print_texel(const char *what, unsigned slot, unsigned x, unsigned y, unsigned z,
            const void *texels, bool is_float)
{
   unsigned k = texel_index(x, y, z);
   if (is_float) {
      const float *f = texels;
      printf("air_metal_test: %s %u texel (%u,%u,%u) = %.9g %.9g %.9g %.9g\n", what, slot, x,
             y, z, (double)f[k], (double)f[k + 1], (double)f[k + 2], (double)f[k + 3]);
   } else {
      const int32_t *i = texels;
      printf("air_metal_test: %s %u texel (%u,%u,%u) = %d %d %d %d\n", what, slot, x, y, z,
             i[k], i[k + 1], i[k + 2], i[k + 3]);
   }
}

/* One function's module out of an MTLB container. The offsets are the header's: the
 * function list at 0x18, the bitcode region at 0x48, and inside a function's record a
 * NAME tag, an OFFT triple whose third word is where that function's module starts
 * inside the region, and MDSZ, how long it is. Everything is little endian and nothing
 * is aligned.
 */
struct metallib {
   uint8_t *bytes;
   size_t size;
};

static uint64_t
u64_at(const struct metallib *lib, size_t off)
{
   uint64_t v = 0;
   if (off + 8 <= lib->size)
      memcpy(&v, lib->bytes + off, 8);
   return v;
}

static uint32_t
u32_at(const struct metallib *lib, size_t off)
{
   uint32_t v = 0;
   if (off + 4 <= lib->size)
      memcpy(&v, lib->bytes + off, 4);
   return v;
}

static uint16_t
u16_at(const struct metallib *lib, size_t off)
{
   uint16_t v = 0;
   if (off + 2 <= lib->size)
      memcpy(&v, lib->bytes + off, 2);
   return v;
}

/* Every function of the container, with each one's module: exactly the bytes
 * tools/metallib.py hands to llvm-dis, the blob MDSZ sizes, wrapper and all. One record
 * loop, because find_function below is one pick out of this table and --textures is the
 * whole of it -- and a second loop over the same unaligned tags would be a second reader
 * of a format that has only ever been read once correctly.
 *
 * A record whose module falls outside the file keeps its name and gets no bytes, so the
 * sentence about it names the function rather than the file.
 */
struct metallib_table {
   unsigned count;
   char **names;
   const uint8_t **bitcode;
   size_t *size;
};

static void
free_functions(struct metallib_table *t)
{
   for (unsigned i = 0; i < t->count; i++)
      free(t->names[i]);
   free(t->names);
   free(t->bitcode);
   free(t->size);
   memset(t, 0, sizeof(*t));
}

static bool
parse_functions(const struct metallib *lib, struct metallib_table *t)
{
   memset(t, 0, sizeof(*t));
   if (lib->size < 0x50 || memcmp(lib->bytes, "MTLB", 4) != 0) {
      fprintf(stderr, "air_metal_test: not a .metallib\n");
      return false;
   }
   size_t list = u64_at(lib, 0x18);
   uint64_t bc_off = u64_at(lib, 0x48), bc_len = u64_at(lib, 0x50);
   unsigned count = u32_at(lib, list);

   /* The count comes out of the file, so it is bounded by the file: the records live
    * between the list's start and the end offset at 0x20, and the smallest one is a
    * size word and an ENDT.
    */
   uint64_t list_end = u64_at(lib, 0x20);
   if (list_end > lib->size || list_end < list) {
      fprintf(stderr, "air_metal_test: the function list runs to %llu and the file is "
                      "%zu bytes: truncated\n", (unsigned long long)list_end, lib->size);
      return false;
   }
   if (count > (list_end - list) / 8) {
      fprintf(stderr, "air_metal_test: a function list of %u records in %llu bytes\n",
              count, (unsigned long long)(list_end - list));
      return false;
   }

   t->names = calloc(count ? count : 1, sizeof(*t->names));
   t->bitcode = calloc(count ? count : 1, sizeof(*t->bitcode));
   t->size = calloc(count ? count : 1, sizeof(*t->size));
   if (!t->names || !t->bitcode || !t->size) {
      fprintf(stderr, "air_metal_test: out of memory for %u functions\n", count);
      free_functions(t);
      return false;
   }

   size_t p = list + 4;
   for (unsigned i = 0; i < count; i++) {
      uint64_t off = 0, len = 0;
      uint32_t size = u32_at(lib, p);   /* counts from this field itself */
      size_t q = p + 4, end = p + size;
      while (q + 6 <= end && q + 6 <= lib->size) {
         const char *tag = (const char *)lib->bytes + q;
         if (!memcmp(tag, "ENDT", 4))
            break;
         uint16_t tlen = u16_at(lib, q + 4);
         if (!memcmp(tag, "NAME", 4))
            t->names[i] = strndup((const char *)lib->bytes + q + 6, tlen);
         else if (!memcmp(tag, "OFFT", 4) && tlen >= 24)
            off = u64_at(lib, q + 6 + 16);
         else if (!memcmp(tag, "MDSZ", 4) && tlen >= 8)
            len = u64_at(lib, q + 6);
         q += 6 + tlen;
      }
      uint64_t start = bc_off + off, stop = bc_off + off + len;
      if (start < lib->size && stop <= lib->size && stop > start &&
          stop <= bc_off + bc_len) {
         t->bitcode[i] = lib->bytes + start;
         t->size[i] = stop - start;
      }
      p = end;
      t->count++;
   }
   return true;
}

/* Finds `want` (or the first function when it is NULL) and returns its module. */
static bool
find_function(const struct metallib *lib, const char *want, char *name, size_t name_size,
              const uint8_t **bitcode, size_t *bitcode_size)
{
   /* A file that is not a container is one module, as air_run reads one. It is here so a
    * hand-written probe can go through THIS library and not only through air_run: the
    * corpus has no fragment shader that reads its own position (all seventeen that
    * declare [[position]] mark it air.arg_unused), so the only way to exercise the
    * driver's own position path is a module written for it -- tools/air/fragpos.ll.
    */
   if (lib->size >= 4 && memcmp(lib->bytes, "MTLB", 4) != 0) {
      snprintf(name, name_size, "%s", want ? want : "(the only module)");
      *bitcode = lib->bytes;
      *bitcode_size = lib->size;
      return true;
   }

   struct metallib_table t;
   if (!parse_functions(lib, &t))
      return false;

   unsigned pick = t.count;
   for (unsigned i = 0; i < t.count; i++)
      if (t.names[i] && (!want || !strcmp(want, t.names[i]))) {
         pick = i;
         break;
      }
   bool ok = pick < t.count;
   if (!ok) {
      fprintf(stderr, "air_metal_test: no function %s in a library of %u\n",
              want ? want : "(any)", t.count);
   } else if (!t.bitcode[pick]) {
      fprintf(stderr, "air_metal_test: %s is outside the file\n", t.names[pick]);
      ok = false;
   } else {
      snprintf(name, name_size, "%s", t.names[pick]);
      *bitcode = t.bitcode[pick];
      *bitcode_size = t.size[pick];
   }

   free_functions(&t);
   return ok;
}

/* The whole file into memory, the way Metal has the container in hand before it hands a
 * driver one function's bitcode. Returns false and says why on stderr.
 */
static bool
load_metallib(const char *path, struct metallib *lib)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      perror(path);
      return false;
   }
   fseek(f, 0, SEEK_END);
   lib->size = ftell(f);
   fseek(f, 0, SEEK_SET);
   lib->bytes = malloc(lib->size);
   if (fread(lib->bytes, 1, lib->size, f) != lib->size) {
      fprintf(stderr, "air_metal_test: short read of %s\n", path);
      fclose(f);
      return false;
   }
   fclose(f);
   return true;
}

/* air_run's buffer pattern, (i * 7 + 13) & 0xff: a byte that varies with its offset, so
 * a shader reading the wrong offset reads a wrong value and not a plausible one, and a
 * number out of this draw and a number out of air_run are the same measurement because
 * the bytes under them are the same.
 */
static uint8_t
pattern_byte(unsigned i)
{
   return (i * 7 + 13) & 0xff;
}

/* The library's own full-screen quad, in clip space -- the same four corners as
 * rwa_quad_verts inside air_metal.c -- and the texture coordinate of each corner.
 *
 * These are what a SUPPLIED vertex shader is fed. Apple's three vertex shaders all read
 * their vertices out of a buffer indexed by the vertex id, so handing them this quad
 * makes them emit the geometry the library's generated stage draws: the two roads then
 * cover the same pixels, and a difference in the picture is a difference in the shader
 * rather than in where the triangles went. The third texture coordinate is 0.125, the
 * centre of slice 0 of a 4-deep 3D texture, so a float3 coordinate lands in slice 0.
 */
static const float draw_quad_pos[4][4] = {
   { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
};
static const float draw_quad_uv[4][3] = {
   { 0, 0, 0.125f }, { 1, 0, 0.125f }, { 0, 1, 0.125f }, { 1, 1, 0.125f },
};

/* What one buffer of a supplied vertex stage holds: `comps` is 0 for the quad's
 * clip-space positions and 2 or 3 for texture coordinates of that many components.
 *
 * The element stride is the caller's to know, and that is the contract working as
 * designed rather than a gap in it: rwa_render_info reports how many buffers a stage
 * binds and not what one element of one of them is, exactly as it reports how many
 * textures and not their formats, because a driver never has to ask -- the application
 * filled the MTLBuffer and knows what it put there. air64's own strides are at work:
 * texturedVertex2D walks a <2 x float> every 8 bytes while texturedVertex3D walks a
 * <3 x float> every 16, because this target aligns a 96-bit vector to 128 bits, and a
 * test that guessed one stride for both would read the second vertex of one of them out
 * of the middle of the first.
 */
static void
fill_vertex_buffer(void *mem, unsigned comps, float scale)
{
   float *f = mem;
   if (comps == 0) {
      for (unsigned v = 0; v < 4; v++) {
         /* x and y only: scaling z and w would move the quad out of the near plane or
          * divide by something other than one, and what this knob is for is a smaller
          * quad in the same plane
          */
         f[v * 4 + 0] = draw_quad_pos[v][0] * scale;
         f[v * 4 + 1] = draw_quad_pos[v][1] * scale;
         f[v * 4 + 2] = draw_quad_pos[v][2];
         f[v * 4 + 3] = draw_quad_pos[v][3];
      }
      return;
   }
   unsigned stride = comps == 3 ? 4 : comps;
   for (unsigned v = 0; v < 4; v++)
      for (unsigned c = 0; c < stride; c++)
         f[v * stride + c] = c < comps ? draw_quad_uv[v][c] : 0.0f;
}

/* What a supplied vertex stage's buffers hold: a comma-separated list of
 *
 *   SLOT:COMPONENTS   that slot holds texture coordinates of that many components,
 *                     as in "1:2" for texturedVertex2D, whose buffer 1 is one float2
 *                     per vertex. Every slot not named holds the quad's positions, for
 *                     the reason the fragment buffers are all filled alike -- the
 *                     contract does not say which index the shader reads, so every
 *                     index holds something the shader could read.
 *   scale:S           the quad's corners are S times as far from the origin.
 *
 * The scale is the canary, and it is here because the first run of this mode agreed with
 * the generated stage to the bit, which is exactly what a mode that quietly drew the
 * generated quad anyway would also have printed. "scale:0.5" covers only the middle of a
 * 4x4 attachment, so the pixel at (0, 0) must come back as the clear colour; if it still
 * came back as the shader's value, the supplied vertex shader is not what put the
 * triangles on the screen and nothing else here is evidence.
 */
static void
parse_vertex_spec(const char *spec, unsigned *comps, unsigned n, float *scale)
{
   while (spec && *spec) {
      const char *colon = strchr(spec, ':');
      const char *comma = strchr(spec, ',');
      if (!strncmp(spec, "scale:", 6)) {
         *scale = strtof(spec + 6, NULL);
      } else if (colon && (!comma || colon < comma)) {
         unsigned slot = (unsigned)strtoul(spec, NULL, 0);
         if (slot < n)
            comps[slot] = (unsigned)strtoul(colon + 1, NULL, 0);
      }
      spec = comma ? comma + 1 : NULL;
   }
}

/* MTLTextureType, in the SDK's own spelling. The numbers are air_metal.c's
 * texture_type_name table and nothing else: a type printed as a number here would be
 * read against a header by hand every time the table is read.
 */
static const char *
texture_type_name(unsigned type)
{
   static const char *const names[] = {
      "MTLTextureType1D", "MTLTextureType1DArray", "MTLTextureType2D",
      "MTLTextureType2DArray", "MTLTextureType2DMultisample", "MTLTextureTypeCube",
      "MTLTextureTypeCubeArray", "MTLTextureType3D", "MTLTextureType2DMultisampleArray",
      "MTLTextureTypeTextureBuffer",
   };
   return type < COUNT_OF(names) ? names[type] : "an MTLTextureType with no name here";
}

static const char *
element_name(unsigned element)
{
   switch (element) {
   case RWA_ELEMENT_FLOAT: return "float";
   case RWA_ELEMENT_INT:   return "int";
   case RWA_ELEMENT_UINT:  return "uint";
   default:                return "-";
   }
}

/*
 * --textures: what every function of a .metallib declares it wants bound, read here.
 *
 * This is the host half of the sweep in userland/src/metal_drv_test.m. That sweep binds
 * one texture kind and one format for a whole library, and so refused 13 of Apple's 271
 * fragment shaders by name -- they declare a texture3d, a texture2d<int> or a multisample
 * texture -- which measured the harness and not the shader. rwa_module_textures was added
 * so a caller can bind what each shader asks for, and this prints that same answer for a
 * whole library, so the expected binding of every function can be read without a boot and
 * a device log can be checked against it afterwards.
 *
 * It is the library's answer and not a second reading of Apple's metadata, which is the
 * only way the table and the device's behaviour can be compared at all: the numbers here
 * come from the same two helpers that refuse a wrong binding there.
 *
 * The entry point is left to the module (NULL): a .metallib holds one LLVM module per
 * function, so there is exactly one to find, and naming it would turn a container whose
 * record name differs from the function's into a refusal about nothing.
 *
 *   air_metal_test --textures FILE.metallib [FUNCTION]
 */
static int
run_textures(const char *path, const char *only)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   struct metallib_table t;
   if (!parse_functions(&lib, &t))
      return 1;

   /* softpipe: the library translates with the screen's own NIR options and this machine
    * has no Adreno. Which screen it is cannot change what a shader declares -- the
    * declaration is Apple's metadata and the shader's own instructions -- but the call
    * takes a device because the pipeline path does, and one translation is the point.
    */
   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      free_functions(&t);
      return 1;
   }

   unsigned functions = 0, with_textures = 0, declared = 0, refused = 0, plain = 0;
   for (unsigned i = 0; i < t.count; i++) {
      if (!t.names[i] || (only && strcmp(only, t.names[i]) != 0))
         continue;
      functions++;

      if (!t.bitcode[i]) {
         refused++;
         printf("textures %-40s outside the file\n", t.names[i]);
         continue;
      }

      struct rwa_texture_decl decl[16];
      unsigned n = 0;
      if (rwa_module_textures(device, t.bitcode[i], t.size[i], NULL, decl, COUNT_OF(decl),
                              &n, why, sizeof(why)) != 0) {
         /* The frontend's own sentence: a module refused here is a module that would be
          * refused at pipeline creation, so this is a row of the table and not a failure
          * of the table.
          */
         refused++;
         printf("textures %-40s no answer: %s\n", t.names[i], why);
         continue;
      }
      if (n == 0) {
         printf("textures %-40s none\n", t.names[i]);
         continue;
      }
      with_textures++;
      for (unsigned k = 0; k < n; k++) {
         declared++;
         /* "a plain texture2d<float>" is the one declaration the sweep could bind
          * before this existed, so it is worth counting apart: everything else is a
          * shader that went unmeasured for want of this call.
          */
         bool is_plain = decl[k].type_known && decl[k].type == MTL_TEXTURE_TYPE_2D &&
                         decl[k].element == RWA_ELEMENT_FLOAT && decl[k].element_bits == 32;
         if (is_plain)
            plain++;
         printf("textures %-40s slot %u  %-32s %s%u%s  usage %u  %s\n", t.names[i],
                decl[k].index,
                decl[k].type_known ? texture_type_name(decl[k].type)
                                   : "(no kind: a computed binding)",
                element_name(decl[k].element),
                decl[k].element_bits,
                decl[k].element_bits ? "" : " (width not declared)", decl[k].usage,
                decl[k].metal_type[0] ? decl[k].metal_type : "(no air.arg_type_name)");
      }
   }

   printf("textures: %s: %u function%s, %u %s %u texture%s in all, %u of those "
          "declarations are a plain 32-bit texture2d<float>, %u modules have no answer\n",
          path, functions, functions == 1 ? "" : "s", with_textures,
          with_textures == 1 ? "declares" : "declare", declared,
          declared == 1 ? "" : "s", plain, refused);

   rwa_device_close(device);
   free_functions(&t);
   return 0;
}

/*
 * The other half of the library, driven the way the Metal driver will drive a draw: a
 * fragment shader out of the container, a vertex stage over it, a few plain textures as
 * colour attachments, and the pixel read back. This is air_run's draw in terms of
 * air_metal.h alone.
 *
 * `vfunc` is NULL for the library's own generated vertex stage, which is the road with
 * 255 drawn and pixel-compared fragment shaders behind it, or the name of a vertex
 * function in the same container -- Apple's own clearVertex, texturedVertex2D or
 * texturedVertex3D -- which is the frontend's vertex path run end to end. The two are
 * one function on purpose: the same fragment shader, the same attachments, the same
 * buffer bytes, so the only thing that changed between two runs is where the vertices
 * came from, and the pixels can be put side by side.
 *
 * The attachments' count and formats are the caller's, as a render pipeline's are in
 * Metal (MTLRenderPipelineDescriptor's colour formats): the contract reports how many
 * buffers, textures and samplers the fragment stage binds, but not how many colours it
 * writes, because a driver has the pipeline descriptor for that. So they are chosen
 * here, defaulting to what clearFragment20 wants -- three MTLPixelFormatRGBA32Uint.
 *
 *   air_metal_test --draw FILE.metallib FUNCTION [MTLPixelFormat[,...]]
 *   air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION FUNCTION
 *                  [MTLPixelFormat[,...] [VERTEXBUFFERS [MTLPixelFormat[:TYPE][,...]]]]
 */
static int
run_draw(const char *path, const char *vfunc, const char *func, const char *colorspec,
         const char *vtxspec, const char *texspec)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;
   printf("air_metal_test: --draw %s, %zu bytes of bitcode, %s\n", name, bitcode_size,
          bitcode[0] == 0xde ? "in its 0x0B17C0DE wrapper" : "a bare module");

   /* The vertex module, out of the same container and by the same reader: on the device
    * a driver gets both stages' bytes from two -[_MTLFunction bitcodeData] calls on two
    * functions of one MTLLibrary, which is these two find_function calls.
    */
   char vname[256];
   const uint8_t *vbitcode = NULL;
   size_t vbitcode_size = 0;
   if (vfunc) {
      if (!find_function(&lib, vfunc, vname, sizeof(vname), &vbitcode, &vbitcode_size))
         return 1;
      printf("air_metal_test: vertex stage %s, %zu bytes of bitcode, %s\n", vname,
             vbitcode_size,
             vbitcode[0] == 0xde ? "in its 0x0B17C0DE wrapper" : "a bare module");
   }

   /* The colour attachments. Comma-separated MTLPixelFormats, one per attachment; the
    * count is how many are given. The default is three RGBA32Uint, which is
    * clearFragment20: it writes render target 2, so targets 0 and 1 have to exist for
    * target 2 to be bound at all. Only the three 32-bit RGBA formats, for the readback
    * reason the texture path has: a uint printed as a float reads as 1.4e-43.
    */
   unsigned format[8];
   bool fmt_is_float[COUNT_OF(format)];
   unsigned color_count = 0;
   const char *arg = colorspec;
   if (!arg || !*arg)
      arg = "123,123,123";    /* empty too, so a later argument can be given alone */
   while (arg && *arg && color_count < COUNT_OF(format)) {
      unsigned v = (unsigned)strtoul(arg, NULL, 0);
      /* ... and one of Metal's packed 16-bit colour formats, which --pixel-formats
       * renders INTO: the pixel then comes back as a 16-bit WORD rather than as four
       * numbers, which is the whole point of that direction -- four floats would have
       * gone back out through the same unpacking the write used and said nothing.
       */
      if (!(v >= 40 && v <= 43) && v != MTL_RGBA32_FLOAT && v != MTL_RGBA32_UINT &&
          v != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: attachment pixel format %u: this program reads "
                         "back MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u), "
                         "RGBA32Sint (%u) or one of the packed 16-bit four (40-43)\n",
                 v, MTL_RGBA32_FLOAT, MTL_RGBA32_UINT, MTL_RGBA32_SINT);
         return 2;
      }
      format[color_count] = v;
      fmt_is_float[color_count] = v == MTL_RGBA32_FLOAT;
      color_count++;
      const char *comma = strchr(arg, ',');
      arg = comma ? comma + 1 : NULL;
   }

   /* softpipe, for the reason the compute path sets it: this machine has no Adreno and
    * the library does not fall back by itself, so RWA_SOFTWARE=0 in the environment
    * still shows what a driver on a phone without gpu=1 is told.
    */
   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: device is \"%s\"\n", rwa_device_name(device));

   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = format[i];

   struct rwa_render_info info = { 0 };
   struct rwa_render *render =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, bitcode, bitcode_size,
                        func, &shape, &info, why, sizeof(why));
   if (!render) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   printf("air_metal_test: pipeline: %u vertex buffers, %u fragment buffers, %u fragment "
          "textures, %u fragment samplers, vertex stage %s\n", info.vertex_buffers,
          info.fragment_buffers, info.fragment_textures, info.fragment_samplers,
          info.generated_vertex_stage ? "generated" : "supplied");

   /* One colour attachment per format: a 4x4 2D texture, made a render target and also
    * readable so the pixel can be fetched back. 4x4 is air_run's WIDTH x HEIGHT, so the
    * same grid of fragments.
    */
   const unsigned origin[3] = { 0, 0, 0 };
   struct rwa_texture *color[COUNT_OF(format)] = { 0 };
   for (unsigned i = 0; i < color_count; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = format[i], .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | 4 /* MTLTextureUsageRenderTarget */,
      };
      color[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }
   printf("air_metal_test: %u colour attachment%s of 4x4\n", color_count,
          color_count == 1 ? "" : "s");

   /* One constant buffer per binding the pipeline says the fragment stage reads, each 48
    * bytes and filled with (i * 7 + 13) & 0xff -- the size and pattern air_run gives
    * clearFragment20's buffer at index 14 (air_run.c: "constant buffer at index 14, 48
    * bytes"). The pattern is position-based, so every index holds the same bytes and the
    * one the shader actually reads, 14, holds what air_run read.
    */
   struct rwa_buffer *buffers[16] = { 0 };
   unsigned nbuf = info.fragment_buffers < COUNT_OF(buffers) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < nbuf; i++) {
      buffers[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create (buffer %u): %s\n", i, why);
         return 1;
      }
      uint8_t *bits = rwa_buffer_map(buffers[i]);
      for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
         bits[k] = pattern_byte(k);
   }
   printf("air_metal_test: %u constant buffer%s of 48 bytes of (i * 7 + 13) & 0xff\n", nbuf,
          nbuf == 1 ? "" : "s");

   /* And one per binding the VERTEX stage reads, which is nothing at all for the
    * generated stage (it reads an attribute, which the library feeds itself) and the
    * quad for a supplied one.
    *
    * The count is the highest index plus one, as Metal counts bindings: clearVertex
    * reads [[buffer(14)]] and so asks for fifteen. All fifteen are filled, and with the
    * same bytes, on the fragment buffers' argument -- the contract does not say which
    * index the shader reads, so whichever one it reads holds the quad. 64 bytes is four
    * float4s, the largest of the three element strides below.
    */
   struct rwa_buffer *vbuffers[16] = { 0 };
   unsigned vcomps[COUNT_OF(vbuffers)] = { 0 };
   float vscale = 1.0f;
   unsigned nvbuf = info.vertex_buffers < COUNT_OF(vbuffers)
                       ? info.vertex_buffers : (unsigned)COUNT_OF(vbuffers);
   parse_vertex_spec(vtxspec, vcomps, COUNT_OF(vbuffers), &vscale);
   for (unsigned i = 0; i < nvbuf; i++) {
      vbuffers[i] = rwa_buffer_create(device, sizeof(draw_quad_pos), why, sizeof(why));
      if (!vbuffers[i]) {
         printf("air_metal_test: rwa_buffer_create (vertex buffer %u): %s\n", i, why);
         return 1;
      }
      memset(rwa_buffer_map(vbuffers[i]), 0, rwa_buffer_size(vbuffers[i]));
      fill_vertex_buffer(rwa_buffer_map(vbuffers[i]), vcomps[i], vscale);
   }
   /* One line for all the position slots and one per coordinate slot: clearVertex asks
    * for fifteen buffers and fifteen identical lines said nothing fourteen did not.
    */
   if (nvbuf) {
      unsigned npos = 0;
      for (unsigned i = 0; i < nvbuf; i++)
         npos += vcomps[i] == 0;
      if (npos)
         printf("air_metal_test: %u vertex buffer%s hold%s the four clip-space corners of "
                "the quad, one float4 each, scaled by %g\n", npos, npos == 1 ? "" : "s",
                npos == 1 ? "s" : "", (double)vscale);
      for (unsigned i = 0; i < nvbuf; i++)
         if (vcomps[i])
            printf("air_metal_test: vertex buffer %u holds a float%u texture coordinate "
                   "per corner\n", i, vcomps[i]);
   }

   /* One texture per slot the pipeline says the fragment stage samples, with air_run's
    * texel pattern in it, so the pixel that comes back names the texel it was sampled
    * from: (100+x, 200+y, 300+x+y+z, 400) can be read by eye, and that is the whole
    * point here -- it is how a coordinate interpolated by a real vertex stage is told
    * apart from the one constant coordinate the generated stage hands every fragment.
    *
    * The format and type come from the last argument, defaulting to attachment 0's
    * format as a 2D texture, which is what texturedFragment2DUInt wants. A wrong pick is
    * refused by the library naming what the shader declared, as the compute path's are.
    */
   struct rwa_texture *textures[16] = { 0 };
   unsigned tex_format[COUNT_OF(textures)], tex_type[COUNT_OF(textures)];
   bool tex_is_float[COUNT_OF(textures)];
   unsigned ntex = info.fragment_textures < COUNT_OF(textures) ? info.fragment_textures
                                                               : (unsigned)COUNT_OF(textures);
   arg = texspec;
   for (unsigned i = 0; i < ntex; i++) {
      const char *colon = arg ? strchr(arg, ':') : NULL;
      const char *comma = arg ? strchr(arg, ',') : NULL;
      tex_type[i] = colon && (!comma || colon < comma)
                       ? (unsigned)strtoul(colon + 1, NULL, 0) : MTL_TEXTURE_TYPE_2D;
      tex_format[i] = arg ? (unsigned)strtoul(arg, NULL, 0) : format[0];
      /* The packed 16-bit formats are not written with the texel pattern -- their
       * channels are five and six bits wide and 100 does not fit in one -- so they are
       * accepted only with a probe word, which fills every texel and is the whole point
       * of asking for one: see probe_fill16.
       */
      bool packed16 = tex_format[i] >= 40 && tex_format[i] <= 43;
      if (packed16 && !probe_fill16_given) {
         fprintf(stderr, "air_metal_test: texture pixel format %u is one of Metal's packed "
                         "16-bit formats, whose channels are too narrow for this program's "
                         "texel pattern -- 100 does not fit in five bits. "
                         "--pixel-formats is the mode that fills them, one 16-bit word "
                         "per texel\n", tex_format[i]);
         return 2;
      }
      if (!packed16 && tex_format[i] != MTL_RGBA32_FLOAT &&
          tex_format[i] != MTL_RGBA32_UINT && tex_format[i] != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: texture pixel format %u: this program writes "
                         "known texels in MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u) "
                         "or RGBA32Sint (%u)\n", tex_format[i], MTL_RGBA32_FLOAT,
                 MTL_RGBA32_UINT, MTL_RGBA32_SINT);
         return 2;
      }
      if (tex_type[i] != MTL_TEXTURE_TYPE_2D && tex_type[i] != MTL_TEXTURE_TYPE_3D) {
         fprintf(stderr, "air_metal_test: texture type %u: this program makes "
                         "MTLTextureType2D (%u) and MTLTextureType3D (%u)\n", tex_type[i],
                 MTL_TEXTURE_TYPE_2D, MTL_TEXTURE_TYPE_3D);
         return 2;
      }
      tex_is_float[i] = tex_format[i] == MTL_RGBA32_FLOAT;
      if (comma)
         arg = comma + 1;      /* otherwise the last one stands for the rest */

      unsigned depth = tex_type[i] == MTL_TEXTURE_TYPE_3D ? TEX : 1;
      const struct rwa_texture_shape ts = {
         .pixel_format = tex_format[i], .type = tex_type[i],
         .width = TEX, .height = TEX, .depth = depth,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         /* read only: a fragment shader that sampled and stored would want
          * MTLTextureUsageShaderWrite as well, and none of Apple's does
          */
         .usage = MTL_USAGE_SHADER_READ,
      };
      textures[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!textures[i]) {
         printf("air_metal_test: rwa_texture_create (texture %u): %s\n", i, why);
         return 1;
      }
      static union { float f[TEXELS]; int32_t i[TEXELS]; uint16_t h[TEXELS * 2]; } written;
      memset(&written, 0, sizeof(written));
      const unsigned extent[3] = { TEX, TEX, depth };
      if (packed16) {
         /* One 16-bit word in every texel, the caller's. Every texel the same on
          * purpose: this measurement is about which FIELD of the word became which
          * channel, so a coordinate must not be able to change the answer.
          */
         for (unsigned k = 0; k < TEX * TEX * depth; k++)
            written.h[k] = (uint16_t)probe_fill16;
         if (rwa_texture_write(textures[i], 0, 0, origin, extent, &written, 0, why,
                               sizeof(why)) != 0) {
            printf("air_metal_test: rwa_texture_write (texture %u): %s\n", i, why);
            return 1;
         }
         printf("air_metal_test: texture %u is %ux%u of pixel format %u, every texel the "
                "16-bit word 0x%04x\n", i, TEX, TEX, tex_format[i],
                (unsigned)(probe_fill16 & 0xffff));
      } else {
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               float v[4];
               texel_value(i, x, y, z, v);
               for (unsigned c = 0; c < 4; c++) {
                  unsigned k = texel_index(x, y, z) + c;
                  if (tex_is_float[i])
                     written.f[k] = v[c];
                  else
                     written.i[k] = (int32_t)v[c];
               }
            }
      if (rwa_texture_write(textures[i], 0, 0, origin, extent, &written, 0, why,
                            sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write (texture %u): %s\n", i, why);
         return 1;
      }
      printf("air_metal_test: texture %u is %ux%ux%u of pixel format %u, texel (x,y,z) = "
             "(100+x+%u, 200+y, 300+x+y+z, 400)\n", i, TEX, TEX, depth, tex_format[i],
             1000 * i);
      }
   }

   /* And a sampler per slot the API is expected to fill. Nearest and clamped as in the
    * compute path, but in NORMALISED coordinates and not pixels, which is the one place
    * the two paths differ and it is not arbitrary: a kernel computes its own integer
    * texel, while these fragment shaders are handed a coordinate by the vertex stage,
    * and the coordinate AppleMetalGLRenderer's vertex stage carries is a GL texture
    * coordinate in [0, 1]. Nearest, so the texel the coordinate falls in is the texel
    * that comes back with no weights to round.
    */
   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.fragment_samplers < COUNT_OF(samplers) ? info.fragment_samplers
                                                                : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 },           /* MTLSamplerAddressModeClampToEdge */
         .min_filter = 0, .mag_filter = 0, /* MTLSamplerMinMagFilterNearest */
         .mip_filter = 0,                  /* MTLSamplerMipFilterNotMipmapped */
         .compare_function = 0,            /* MTLCompareFunctionNever, nothing reads it */
         .max_anisotropy = 1,
         .normalized_coordinates = 1,
         .lod_min = 0.0f, .lod_max = 65504.0f,
      };
      samplers[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }
   if (nsamp)
      printf("air_metal_test: %u sampler%s from the API: nearest, clamp to edge, "
             "normalised\n", nsamp, nsamp == 1 ? "" : "s");

   const struct rwa_draw draw = {
      .color = color, .color_count = color_count,
      .clear_color = (const float[]){ 0, 0, 0, 0 },
      .vertex = { .buffers = vbuffers, .buffer_count = nvbuf },
      .fragment = { .buffers = buffers, .buffer_count = nbuf,
                    .textures = textures, .texture_count = ntex,
                    .samplers = samplers, .sampler_count = nsamp },
      .primitive = 4,                       /* MTLPrimitiveTypeTriangleStrip */
      .vertex_start = 0, .vertex_count = 4, /* the four corners, generated or supplied */
   };
   if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_render_draw: %s\n", why);
      rwa_render_destroy(render);
      for (unsigned i = 0; i < nbuf; i++)
         rwa_buffer_destroy(buffers[i]);
      for (unsigned i = 0; i < nvbuf; i++)
         rwa_buffer_destroy(vbuffers[i]);
      for (unsigned i = 0; i < ntex; i++)
         rwa_texture_destroy(textures[i]);
      for (unsigned i = 0; i < nsamp; i++)
         rwa_sampler_destroy(samplers[i]);
      for (unsigned i = 0; i < color_count; i++)
         rwa_texture_destroy(color[i]);
      rwa_device_close(device);
      return 1;
   }
   printf("air_metal_test: drew a %s of %u vertices into %u attachment%s\n",
          "MTLPrimitiveTypeTriangleStrip", draw.vertex_count, color_count,
          color_count == 1 ? "" : "s");

   /* The pixel at (0, 0) of the last attachment, which for clearFragment20 is render
    * target 2 -- the one it writes. Read as a single texel of its own, the way the
    * contract reads a texture back.
    *
    * And then the opposite corner, which is not decoration. A fragment shader that
    * samples what the vertex stage interpolated came back at (0, 0) with texel (0, 0) --
    * and so would a shader whose varying had been dropped and read as zero, since a
    * coordinate of (0, 0) lands in texel (0, 0) too. The far corner tells those apart:
    * an interpolated coordinate reaches texel (3, 3) there and a dropped one is still
    * (0, 0). The second number is the one that says the varying carries anything.
    */
   unsigned read_rt = color_count - 1;
   const unsigned one[3] = { 1, 1, 1 };
   const unsigned corner[2][3] = { { 0, 0, 0 }, { TEX - 1, TEX - 1, 0 } };
   for (unsigned k = 0; k < 2; k++) {
      union { uint32_t u[4]; float f[4]; } pixel;
      memset(&pixel, 0, sizeof(pixel));
      if (rwa_texture_read(color[read_rt], 0, 0, corner[k], one, &pixel, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read (attachment %u): %s\n", read_rt, why);
         return 1;
      }
      printf("air_metal_test: the pixel at (%u, %u) of attachment %u came back as:\n",
             corner[k][0], corner[k][1], read_rt);
      for (unsigned c = 0; c < 4; c++) {
         if (fmt_is_float[read_rt])
            printf("  %.9g\n", (double)pixel.f[c]);
         else
            printf("  %u\n", pixel.u[c]);
      }
      /* and kept, so a caller in this process can judge it rather than parse its own
       * output: --pixel-formats runs this function sixteen times and compares
       */
      if (k == 0 && fmt_is_float[read_rt])
         for (unsigned c = 0; c < 4; c++)
            last_pixel[c] = pixel.f[c];
      /* A packed 16-bit attachment comes back as its WORD: rwa_texture_read wrote two
       * bytes into the union above, and those two bytes ARE the texel.
       */
      if (k == 0 && format[read_rt] >= 40 && format[read_rt] <= 43) {
         last_word = pixel.u[0] & 0xffff;
         printf("air_metal_test: ... which as a packed 16-bit word is 0x%04x\n",
                last_word);
      }
   }

   rwa_render_destroy(render);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < nvbuf; i++)
      rwa_buffer_destroy(vbuffers[i]);
   for (unsigned i = 0; i < ntex; i++)
      rwa_texture_destroy(textures[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_device_close(device);
   return 0;
}

/*
 * --draw-depth: whether a depth test actually discards anything, measured.
 *
 * Nothing in this port has ever drawn with a depth attachment, so "the depth path
 * works" needs a number that could have come out otherwise. Two things are measured
 * here and they are different measurements:
 *
 *   - a fragment shader that writes COLOUR, drawn three times at three depths with
 *     MTLCompareFunctionLess and depth writes on, and then once more with
 *     MTLCompareFunctionAlways. The quad is the same quad every time; what changes is
 *     the depth it is drawn at (rwa_draw.depth_range, which the generated vertex stage
 *     turns into the quad's window depth exactly) and the bytes in its constant buffer,
 *     so each draw that is NOT discarded leaves a different colour. The colour after
 *     the draw at the far depth is the whole test: unchanged means the test discarded
 *     it, and the Always control at the same depth afterwards proves the compare
 *     function is what discarded it and not the draw.
 *   - a fragment shader that writes DEPTH -- Apple's texturedFragment2DDepth and
 *     texturedFragment3DDepth, the two the corpus has -- where the depth buffer must
 *     come back holding what the shader computed and not the interpolated z. Those two
 *     sample a texture and store channel x to [[depth]], so the sampled texels are
 *     seeded in [0, 1] (depth_texel_value below, NOT air_run's pattern, whose 100.0
 *     would clamp to 1.0 and be indistinguishable from the clear value) and the answer
 *     is the one texel the generated stage's constant coordinate selects.
 *
 * Which of the two it runs is the pipeline's own answer, rwa_render_info's
 * fragment_writes_depth, and not a flag here: the test cannot be told to measure the
 * wrong thing about a shader.
 *
 *   air_metal_test --draw-depth FILE.metallib FUNCTION
 *                  [MTLPixelFormat[,...]|none [DEPTHFORMAT]]
 *
 * "none" in place of the colour formats is a depth-only pass, which Metal allows and this
 * library allows only with a depth attachment; it needs a shader whose output is the depth,
 * since there is then no colour attachment for a discard to show in.
 */

/* Depth texels, for a shader that stores what it samples into [[depth]]: 64 distinct
 * values in (0, 1), each exactly representable as a float, so the one that comes back
 * names the texel it came from -- and none of them is 0 or 1, the two values a cleared
 * or untouched buffer could hold.
 */
static float
depth_texel_value(unsigned x, unsigned y, unsigned z)
{
   return ((float)(x + 4 * y + 16 * z) + 0.5f) / 64.0f;
}

/* The two aspects an MTLPixelFormat has, which decides what goes in rwa_render_shape:
 * Metal's pipeline descriptor has a property per aspect and one texture behind them, so
 * a combined format fills both and a depth-only format fills one.
 */
static bool
depth_format_aspects(unsigned f, bool *has_depth, bool *has_stencil)
{
   switch (f) {
   case 250: *has_depth = true;  *has_stencil = false; return true;  /* Depth16Unorm */
   case 252: *has_depth = true;  *has_stencil = false; return true;  /* Depth32Float */
   case 253: *has_depth = false; *has_stencil = true;  return true;  /* Stencil8 */
   case 260: *has_depth = true;  *has_stencil = true;  return true;  /* ..._Stencil8 */
   default:  return false;
   }
}

/*
 * Whether a depth that came back is the depth that was asked for, and the tolerance is
 * the format's and not a fudge: Depth32Float holds 0.25, 0.5 and 0.75 exactly, so there
 * the tolerance is zero and the comparison is ==. Depth16Unorm holds none of them --
 * 0.5 is 32767.5 of 65535 and has to round -- so one step of that format is allowed,
 * and the line that prints the comparison says which it was. A tolerance wide enough
 * for every format would be a test that cannot fail.
 */
static float
depth_tolerance(unsigned zformat)
{
   return zformat == MTL_DEPTH16_UNORM ? 1.0f / 65535.0f : 0.0f;
}

static bool
depth_is(float got, float want, float tolerance)
{
   float d = got - want;
   return (d < 0.0f ? -d : d) <= tolerance;
}

/* The whole depth buffer, and whether it is one value everywhere. The quad covers every
 * pixel, so it must be -- and a buffer that is not says the draw covered part of the
 * attachment, which no single pixel would have shown.
 */
static bool
read_depth(struct rwa_texture *zt, float *out, float *first, const char *what)
{
   char why[WHY] = "";
   const unsigned origin[3] = { 0, 0, 0 };
   const unsigned extent[3] = { TEX, TEX, 1 };

   if (rwa_depth_read(zt, 0, 0, origin, extent, out, NULL, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_depth_read (%s): %s\n", what, why);
      return false;
   }
   bool uniform = true;
   for (unsigned i = 1; i < TEX * TEX; i++)
      if (out[i] != out[0])
         uniform = false;
   *first = out[0];
   printf("air_metal_test: %s: depth at (0,0) = %.9g, and the 16 texels are %s\n", what,
          (double)out[0], uniform ? "all that value" : "NOT all the same");
   if (!uniform) {
      for (unsigned y = 0; y < TEX; y++) {
         printf("air_metal_test:   row %u:", y);
         for (unsigned x = 0; x < TEX; x++)
            printf(" %.9g", (double)out[y * TEX + x]);
         printf("\n");
      }
   }
   return uniform;
}

static int
run_depth(const char *path, const char *func, const char *colorspec, const char *depthspec)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;

   /* The colour attachments, as --draw takes them: the default is clearFragment20's
    * three RGBA32Uint, since it writes render target 2 and 0 and 1 have to exist for 2
    * to be bound at all.
    */
   unsigned format[8];
   bool fmt_is_float[COUNT_OF(format)];
   unsigned color_count = 0;
   /* "none" is a DEPTH-ONLY pass, which Metal allows and this library allows only with a
    * depth attachment (a shadow map is exactly this). It is spelled out rather than left
    * to an empty argument, because an empty argument is what a caller passes to reach the
    * one after it. */
   const char *arg = colorspec && !strcmp(colorspec, "none") ? NULL
                     : (colorspec && *colorspec) ? colorspec : "123,123,123";
   while (arg && *arg && color_count < COUNT_OF(format)) {
      unsigned v = (unsigned)strtoul(arg, NULL, 0);
      if (v != MTL_RGBA32_FLOAT && v != MTL_RGBA32_UINT && v != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: attachment pixel format %u: this program reads "
                         "back MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u) or "
                         "RGBA32Sint (%u)\n", v, MTL_RGBA32_FLOAT, MTL_RGBA32_UINT,
                 MTL_RGBA32_SINT);
         return 2;
      }
      format[color_count] = v;
      fmt_is_float[color_count] = v == MTL_RGBA32_FLOAT;
      color_count++;
      const char *comma = strchr(arg, ',');
      arg = comma ? comma + 1 : NULL;
   }

   unsigned zformat = depthspec && *depthspec ? (unsigned)strtoul(depthspec, NULL, 0)
                                              : MTL_DEPTH32_FLOAT;
   bool zs_depth = false, zs_stencil = false;
   if (!depth_format_aspects(zformat, &zs_depth, &zs_stencil)) {
      fprintf(stderr, "air_metal_test: depth pixel format %u: iOS has Depth16Unorm (250), "
                      "Depth32Float (252), Stencil8 (253) and Depth32Float_Stencil8 "
                      "(260)\n", zformat);
      return 2;
   }
   if (!zs_depth) {
      fprintf(stderr, "air_metal_test: pixel format %u has no depth aspect, and this mode "
                      "measures a depth test\n", zformat);
      return 2;
   }

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-depth %s on \"%s\", depth attachment pixel format %u\n",
          name, rwa_device_name(device), zformat);

   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1,
                                     .depth_format = zs_depth ? zformat : 0,
                                     .stencil_format = zs_stencil ? zformat : 0 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = format[i];

   struct rwa_render_info info = { 0 };
   struct rwa_render *render =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape, &info,
                        why, sizeof(why));
   if (!render) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      return 1;
   }
   printf("air_metal_test: pipeline: %u fragment buffers, %u fragment textures, %u "
          "samplers, vertex stage %s, and the fragment shader %s to [[depth]]\n",
          info.fragment_buffers, info.fragment_textures, info.fragment_samplers,
          info.generated_vertex_stage ? "generated" : "supplied",
          info.fragment_writes_depth ? "STORES" : "stores nothing");

   /* The attachments. The depth one is MTLTextureUsageRenderTarget and nothing else:
    * its texels come back through rwa_depth_read, which maps the resource rather than
    * sampling it, so it needs no shader-read binding to be measured.
    */
   const unsigned origin[3] = { 0, 0, 0 };
   struct rwa_texture *color[COUNT_OF(format)] = { 0 };
   for (unsigned i = 0; i < color_count; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = format[i], .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | 4 /* MTLTextureUsageRenderTarget */,
      };
      color[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }
   const struct rwa_texture_shape zts = {
      .pixel_format = zformat, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = 4 /* MTLTextureUsageRenderTarget */,
   };
   struct rwa_texture *zt = rwa_texture_create(device, &zts, why, sizeof(why));
   if (!zt) {
      printf("air_metal_test: rwa_texture_create (the depth attachment): %s\n", why);
      return 1;
   }

   /* The depth texture is refused by rwa_texture_read by name, and that refusal is part
    * of the contract rather than an accident, so it is shown here: a caller that asks
    * for a depth texel the colour way is told which call to use instead.
    */
   {
      uint8_t ignored[TEX * TEX * 8];
      const unsigned extent[3] = { TEX, TEX, 1 };
      char refusal[WHY] = "";
      int rc = rwa_texture_read(zt, 0, 0, origin, extent, ignored, 0, refusal,
                                sizeof(refusal));
      printf("air_metal_test: rwa_texture_read of the depth attachment %s: %s\n",
             rc == 0 ? "RETURNED 0, which it must not" : "is refused", refusal);
      if (rc == 0)
         return 1;
   }

   /* The fragment stage's buffers, filled per step below, and its textures, made the
    * shape each slot declares -- which is rwa_module_textures, the same query the
    * driver's sweep uses, so a textured depth shader gets the texture3d it asks for
    * without an argument here saying so.
    */
   struct rwa_buffer *buffers[16] = { 0 };
   unsigned nbuf = info.fragment_buffers < COUNT_OF(buffers) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < nbuf; i++) {
      buffers[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create (buffer %u): %s\n", i, why);
         return 1;
      }
   }

   struct rwa_texture_decl decl[16];
   unsigned ndecl = 0;
   if (info.fragment_textures &&
       rwa_module_textures(device, bitcode, bitcode_size, func, decl, COUNT_OF(decl),
                           &ndecl, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_module_textures: %s\n", why);
      return 1;
   }
   struct rwa_texture *textures[16] = { 0 };
   unsigned ntex = info.fragment_textures < COUNT_OF(textures) ? info.fragment_textures
                                                               : (unsigned)COUNT_OF(textures);
   for (unsigned i = 0; i < ntex; i++) {
      unsigned type = MTL_TEXTURE_TYPE_2D, fmt = MTL_RGBA32_FLOAT, bits = 32;
      for (unsigned k = 0; k < ndecl; k++) {
         if (decl[k].index != i)
            continue;
         if (decl[k].type_known)
            type = decl[k].type;
         fmt = decl[k].element == RWA_ELEMENT_UINT ? MTL_RGBA32_UINT
             : decl[k].element == RWA_ELEMENT_INT  ? MTL_RGBA32_SINT : MTL_RGBA32_FLOAT;
         if (decl[k].element_bits)
            bits = decl[k].element_bits;
      }
      if (type != MTL_TEXTURE_TYPE_2D && type != MTL_TEXTURE_TYPE_3D) {
         printf("air_metal_test: texture %u is declared %s, which this mode does not "
                "make\n", i, texture_type_name(type));
         return 1;
      }
      /* The WIDTH of the declared element and not only its class, refused rather than
       * widened. A half declaration bound a 32-bit texture is a descriptor that
       * disagrees with the instruction the GPU will run, not a conversion: air_run's
       * image_shapes() was picking a 32-bit format for a 16-bit typed store, and that
       * one took a whole boot with it. This mode writes 32-bit texels (the union
       * below), so a 16-bit declaration is said out loud -- the two shaders this mode
       * exists for declare depth2d<float>, which is 32.
       */
      if (bits != 32) {
         printf("air_metal_test: texture %u is declared with a %u-bit element and this "
                "mode writes 32-bit texels; binding a 32-bit format to a %u-bit "
                "declaration is a descriptor that disagrees with the shader, so it is "
                "refused instead\n", i, bits, bits);
         return 1;
      }
      unsigned depth = type == MTL_TEXTURE_TYPE_3D ? TEX : 1;
      const struct rwa_texture_shape ts = {
         .pixel_format = fmt, .type = type, .width = TEX, .height = TEX, .depth = depth,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ,
      };
      textures[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!textures[i]) {
         printf("air_metal_test: rwa_texture_create (texture %u): %s\n", i, why);
         return 1;
      }
      static union { float f[TEXELS]; int32_t i[TEXELS]; } written;
      memset(&written, 0, sizeof(written));
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               float v[4];
               if (info.fragment_writes_depth) {
                  /* a depth in [0, 1] in every channel, because the shader stores what
                   * it samples: air_run's 100.0 would clamp to the far plane and read
                   * as the clear value
                   */
                  for (unsigned c = 0; c < 4; c++)
                     v[c] = depth_texel_value(x, y, z);
               } else {
                  texel_value(i, x, y, z, v);
               }
               for (unsigned c = 0; c < 4; c++) {
                  unsigned k = texel_index(x, y, z) + c;
                  if (fmt == MTL_RGBA32_FLOAT)
                     written.f[k] = v[c];
                  else
                     written.i[k] = (int32_t)v[c];
               }
            }
      const unsigned extent[3] = { TEX, TEX, depth };
      if (rwa_texture_write(textures[i], 0, 0, origin, extent, &written, 0, why,
                            sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write (texture %u): %s\n", i, why);
         return 1;
      }
      printf("air_metal_test: texture %u is %ux%ux%u of pixel format %u, texel (x,y,z) = "
             "%s\n", i, TEX, TEX, depth, fmt,
             info.fragment_writes_depth ? "(x + 4y + 16z + 0.5) / 64 in every channel"
                                        : "(100+x, 200+y, 300+x+y+z, 400)");
   }

   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.fragment_samplers < COUNT_OF(samplers) ? info.fragment_samplers
                                                                : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state st = {
         .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
         .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 1,
         .lod_min = 0.0f, .lod_max = 65504.0f,
      };
      samplers[i] = rwa_sampler_create(device, &st, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }

   /* Two depth/stencil states and nothing else: the one under test and the control.
    * Both write depth, so that what the buffer holds is always the depth of the last
    * draw that passed.
    */
   const struct rwa_depth_stencil_state less_state = {
      .depth_compare_function = 1 /* MTLCompareFunctionLess */, .depth_write_enabled = 1,
      .front = { .compare_function = 7, .read_mask = 0xffffffff, .write_mask = 0xffffffff },
      .back  = { .compare_function = 7, .read_mask = 0xffffffff, .write_mask = 0xffffffff },
   };
   struct rwa_depth_stencil_state always_state = less_state;
   always_state.depth_compare_function = 7;   /* MTLCompareFunctionAlways */

   struct rwa_depth_stencil *less = rwa_depth_stencil_create(device, &less_state, why,
                                                             sizeof(why));
   if (!less) {
      printf("air_metal_test: rwa_depth_stencil_create (Less): %s\n", why);
      return 1;
   }
   struct rwa_depth_stencil *always = rwa_depth_stencil_create(device, &always_state, why,
                                                               sizeof(why));
   if (!always) {
      printf("air_metal_test: rwa_depth_stencil_create (Always): %s\n", why);
      return 1;
   }

   unsigned read_rt = color_count ? color_count - 1 : 0;
   float depth_map[TEX * TEX];
   float tol = depth_tolerance(zformat);
   bool ok = true;

   /* The four-draw experiment reads its answer out of a colour attachment, so a
    * depth-only pass can only be run with a shader whose output is the depth. Said
    * rather than run with a pixel nobody can read. */
   if (color_count == 0 && !info.fragment_writes_depth) {
      printf("air_metal_test: a depth-only pass needs a shader that stores to [[depth]]; "
             "%s writes colour, and the discard is measured in a colour attachment\n", name);
      return 1;
   }

   if (tol != 0.0f)
      printf("air_metal_test: depths are compared to within %.9g, one step of this "
             "format, because it stores none of 0.25, 0.5 and 0.75 exactly\n",
             (double)tol);

   if (info.fragment_writes_depth) {
      /* One draw, with the test off (Always) and depth writes on: what the buffer holds
       * afterwards is the shader's own [[depth]] and nothing else could have put it
       * there.
       *
       * The expected value is the texel the generated vertex stage's one constant
       * coordinate selects. That stage writes (c, d, c, 1) into every varying with
       * c = d = (RWA_TEX_X + 0.5) / 4 = 0.375 (air_metal.c, quad_vertex_shader), so a
       * 2D sample lands in texel (1, 1) and a 3D one in (1, 1, 1) -- the third
       * component repeats the first, and 0.375 of four slices is slice 1. air_run reads
       * the same texel of a 3D texture for the same reason (its tex_z is TEX_Y), so the
       * two harnesses are looking at the same texel and not at two nearly-right ones.
       */
      unsigned want_z = ntex && textures[0] && decl[0].type_known &&
                        decl[0].type == MTL_TEXTURE_TYPE_3D ? 1 : 0;
      float want = depth_texel_value(1, 1, want_z);
      const float clear_depth = 1.0f;
      const float range[2] = { 0.5f, 1.0f };
      const struct rwa_draw draw = {
         .color = color, .color_count = color_count,
         .clear_color = color_count ? (const float[]){ 0, 0, 0, 0 } : NULL,
         .depth = zt, .clear_depth = &clear_depth, .depth_stencil = always,
         .depth_range = range,
         .fragment = { .buffers = buffers, .buffer_count = nbuf,
                       .textures = textures, .texture_count = ntex,
                       .samplers = samplers, .sampler_count = nsamp },
         .primitive = 4, .vertex_start = 0, .vertex_count = 4,
      };
      if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
         printf("air_metal_test: rwa_render_draw: %s\n", why);
         return 1;
      }
      float got = 0.0f;
      ok = read_depth(zt, depth_map, &got, "after the draw") && ok;
      printf("air_metal_test: the shader sampled texel (1, 1, %u) and stored %.9g to "
             "[[depth]]; the buffer holds %.9g\n", want_z, (double)want, (double)got);
      printf("air_metal_test: the quad's own depth was %.9g and the clear was %.9g, so "
             "neither could be mistaken for it\n", (double)range[0], (double)clear_depth);
      if (!depth_is(got, want, tol)) {
         printf("air_metal_test: DIFFERENT -- the depth buffer does not hold what the "
                "shader wrote\n");
         ok = false;
      } else {
         printf("air_metal_test: THE SAME -- a Metal fragment shader's [[depth]] reached "
                "the depth buffer\n");
      }
   } else {
      /* Four draws of one quad: three with MTLCompareFunctionLess at 0.5, 0.75 and
       * 0.25, then the control with Always at 0.75. Each draw's constant buffer holds a
       * different fill, so a draw that was not discarded leaves a colour the previous
       * one did not.
       */
      static const struct { const char *what; float z; bool pattern; uint8_t fill;
                            bool clear; bool control; bool expect_draw; } step[] = {
         { "the near draw at depth 0.5",             0.5f,  true,  0x00, true,  false, true },
         { "the far draw at depth 0.75",             0.75f, false, 0xa5, false, false, false },
         { "the nearer draw at depth 0.25",          0.25f, false, 0x5a, false, false, true },
         { "the control at depth 0.75 with Always",  0.75f, false, 0x3c, false, true,  true },
      };
      unsigned pixel[COUNT_OF(step)][4];
      float depth_at[COUNT_OF(step)];

      memset(pixel, 0, sizeof(pixel));
      for (unsigned s = 0; s < COUNT_OF(step); s++) {
         for (unsigned i = 0; i < nbuf; i++) {
            uint8_t *bits = rwa_buffer_map(buffers[i]);
            for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
               bits[k] = step[s].pattern ? pattern_byte(k) : step[s].fill;
         }
         const float clear_depth = 1.0f;
         const float range[2] = { step[s].z, 1.0f };
         const struct rwa_draw draw = {
            .color = color, .color_count = color_count,
            .clear_color = step[s].clear ? (const float[]){ 0, 0, 0, 0 } : NULL,
            .depth = zt,
            .clear_depth = step[s].clear ? &clear_depth : NULL,
            .depth_stencil = step[s].control ? always : less,
            .depth_range = range,
            .fragment = { .buffers = buffers, .buffer_count = nbuf,
                          .textures = textures, .texture_count = ntex,
                          .samplers = samplers, .sampler_count = nsamp },
            .primitive = 4, .vertex_start = 0, .vertex_count = 4,
         };
         if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
            printf("air_metal_test: rwa_render_draw (%s): %s\n", step[s].what, why);
            return 1;
         }
         const unsigned one[3] = { 1, 1, 1 };
         if (rwa_texture_read(color[read_rt], 0, 0, origin, one, pixel[s], 0, why,
                              sizeof(why)) != 0) {
            printf("air_metal_test: rwa_texture_read (attachment %u): %s\n", read_rt, why);
            return 1;
         }
         printf("air_metal_test: %s: the pixel at (0, 0) of attachment %u is", step[s].what,
                read_rt);
         for (unsigned c = 0; c < 4; c++) {
            if (fmt_is_float[read_rt])
               printf(" %.9g", (double)((const float *)pixel[s])[c]);
            else
               printf(" %u", pixel[s][c]);
         }
         printf("\n");
         ok = read_depth(zt, depth_map, &depth_at[s], step[s].what) && ok;
      }

      /* The verdicts, each naming what would have to be true for it to read otherwise. */
      bool far_discarded = memcmp(pixel[1], pixel[0], sizeof(pixel[0])) == 0;
      bool nearer_drawn = memcmp(pixel[2], pixel[0], sizeof(pixel[0])) != 0;
      bool control_drawn = memcmp(pixel[3], pixel[2], sizeof(pixel[0])) != 0;

      printf("air_metal_test: the far draw was %s, and its buffer held a different fill, "
             "so an undiscarded one would have changed the pixel\n",
             far_discarded ? "DISCARDED (the pixel did not change)"
                           : "NOT discarded -- the pixel changed");
      printf("air_metal_test: the nearer draw %s\n",
             nearer_drawn ? "PASSED (the pixel changed)"
                          : "did NOT pass, and it is in front of what was there");
      printf("air_metal_test: the Always control at the same far depth %s\n",
             control_drawn ? "DREW (so the compare function discarded the other one, not "
                             "the draw)"
                           : "did not draw either, so the fault is not the depth test");
      printf("air_metal_test: depth after each step: %.9g %.9g %.9g %.9g (0.5, 0.5 kept, "
             "0.25, 0.75)\n", (double)depth_at[0], (double)depth_at[1], (double)depth_at[2],
             (double)depth_at[3]);
      if (!depth_is(depth_at[0], 0.5f, tol) || !depth_is(depth_at[1], 0.5f, tol) ||
          !depth_is(depth_at[2], 0.25f, tol) || !depth_is(depth_at[3], 0.75f, tol))
         ok = false;
      if (!far_discarded || !nearer_drawn || !control_drawn)
         ok = false;
      printf("air_metal_test: %s\n",
             ok ? "DEPTH TESTING DISCARDS -- the compare function, the depth write and the "
                  "buffer all agree"
                : "SOMETHING ABOVE DISAGREES, and a depth test that is nearly right is "
                  "worse than none");

      /*
       * And the stencil, when the attachment has one, which is three more draws of the
       * same quad with the depth test out of the way (Always, no depth write) so that
       * only the stencil can discard anything:
       *
       *   - Equal against a reference of 1 over a buffer cleared to 0, which must
       *     discard;
       *   - Always with depthStencilPassOperation Replace and a reference of 7, which
       *     must draw AND leave 7 in the buffer -- the operation table is where Metal
       *     and Gallium disagree in order (MTLStencilOperationInvert is 5 in one and 7
       *     in the other), so what lands in the buffer is the check on that table;
       *   - Equal against 7, which must now draw, because the 7 the previous step wrote
       *     is the 7 this one reads. That pair is what tells a stencil write from a
       *     stencil read: either alone could be a no-op and look right.
       */
      static const struct { const char *what; unsigned compare, pass_op, ref; uint8_t fill;
                            bool clear; bool expect_draw; unsigned expect_stencil; } sstep[] = {
         { "stencil Equal 1 over a buffer cleared to 0", 2, 0, 1, 0x11, true,  false, 0 },
         { "stencil Always, Replace, reference 7",       7, 2, 7, 0x22, false, true,  7 },
         { "stencil Equal 7 over the 7 just written",    2, 0, 7, 0x33, false, true,  7 },
      };
      if (zs_stencil) {
         unsigned before[4];
         uint8_t stencil_map[TEX * TEX];
         bool sok = true;

         memcpy(before, pixel[COUNT_OF(step) - 1], sizeof(before));
         for (unsigned s = 0; s < COUNT_OF(sstep); s++) {
            struct rwa_depth_stencil_state st = {
               .depth_compare_function = 7,   /* Always: only the stencil may discard */
               .depth_write_enabled = 0,
               .front = { .compare_function = sstep[s].compare,
                          .depth_stencil_pass_operation = sstep[s].pass_op,
                          .read_mask = 0xffffffff, .write_mask = 0xffffffff },
            };
            st.back = st.front;
            struct rwa_depth_stencil *ds = rwa_depth_stencil_create(device, &st, why,
                                                                    sizeof(why));
            if (!ds) {
               printf("air_metal_test: rwa_depth_stencil_create (%s): %s\n", sstep[s].what,
                      why);
               return 1;
            }
            for (unsigned i = 0; i < nbuf; i++) {
               uint8_t *bits = rwa_buffer_map(buffers[i]);
               for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
                  bits[k] = sstep[s].fill;
            }
            const unsigned clear_stencil = 0;
            const struct rwa_draw draw = {
               .color = color, .color_count = color_count,
               .depth = zt,
               .clear_stencil = sstep[s].clear ? &clear_stencil : NULL,
               .depth_stencil = ds,
               .stencil_reference = { sstep[s].ref, sstep[s].ref },
               .fragment = { .buffers = buffers, .buffer_count = nbuf,
                             .textures = textures, .texture_count = ntex,
                             .samplers = samplers, .sampler_count = nsamp },
               .primitive = 4, .vertex_start = 0, .vertex_count = 4,
            };
            int rc = rwa_render_draw(render, &draw, why, sizeof(why));
            rwa_depth_stencil_destroy(ds);
            if (rc != 0) {
               printf("air_metal_test: rwa_render_draw (%s): %s\n", sstep[s].what, why);
               return 1;
            }
            unsigned now[4] = { 0 };
            const unsigned one[3] = { 1, 1, 1 };
            const unsigned extent[3] = { TEX, TEX, 1 };
            if (rwa_texture_read(color[read_rt], 0, 0, origin, one, now, 0, why,
                                 sizeof(why)) != 0 ||
                rwa_depth_read(zt, 0, 0, origin, extent, NULL, stencil_map, why,
                               sizeof(why)) != 0) {
               printf("air_metal_test: reading back after %s: %s\n", sstep[s].what, why);
               return 1;
            }
            bool drew = memcmp(now, before, sizeof(before)) != 0;
            bool stencil_right = true;
            for (unsigned i = 0; i < TEX * TEX; i++)
               if (stencil_map[i] != sstep[s].expect_stencil)
                  stencil_right = false;
            printf("air_metal_test: %s: the draw %s (expected %s), and the stencil buffer "
                   "holds %u everywhere expecting %u\n", sstep[s].what,
                   drew ? "DREW" : "was DISCARDED",
                   sstep[s].expect_draw ? "to draw" : "to be discarded", stencil_map[0],
                   sstep[s].expect_stencil);
            if (drew != sstep[s].expect_draw || !stencil_right)
               sok = false;
            memcpy(before, now, sizeof(before));
         }
         printf("air_metal_test: %s\n",
                sok ? "THE STENCIL TEST DISCARDS AND THE STENCIL WRITE LANDS"
                    : "THE STENCIL DISAGREES somewhere above");
         ok = ok && sok;
      } else {
         printf("air_metal_test: this attachment has no stencil aspect, so nothing here "
                "measures the stencil test; pixel format %u does\n",
                MTL_DEPTH32_FLOAT_STENCIL8);
      }
   }

   rwa_depth_stencil_destroy(less);
   rwa_depth_stencil_destroy(always);
   rwa_render_destroy(render);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < ntex; i++)
      rwa_texture_destroy(textures[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_texture_destroy(zt);
   rwa_device_close(device);
   return ok ? 0 : 1;
}

/*
 * --draw-ms: a multisample texture, filled the only way anything can fill one, and read
 * by the shader that declared it.
 *
 * Six of Apple's 271 fragment shaders declare one -- texture2d_ms<float, read> and its
 * int, uint, depth and stencil siblings -- and none of them had ever been drawn, because
 * nothing in this path had ever bound a multisample texture. What that cost was not a
 * binding: it was that THERE IS NO WAY TO PUT A KNOWN TEXEL IN ONE from the processor.
 * Metal forbids it twice (a multisample texture cannot be MTLStorageModeShared on this
 * device, and -replaceRegion: writes only what the processor can see) and so does
 * freedreno, which lays the samples of a texel out in an order of its own. So the fill
 * path is the only one either side has: a render pass whose attachment it is.
 *
 * That makes this mode two passes, which is the whole of its shape:
 *
 *   pass 1   the fill. A quad of Apple's clearFragment20 drawn into the multisample
 *            texture as colour attachment 2 (that shader writes render target 2, so
 *            targets 0 and 1 have to exist for 2 to be bound), after a clear to zero.
 *   pass 2   the measurement. An ORDINARY one-sample pass whose fragment shader is the
 *            one under test, with that multisample texture bound at texture 0. Only the
 *            TEXTURE is multisample; a shader reading one does not need a multisample
 *            pass to read it in.
 *
 * And it runs that twice, because the two configurations measure different things:
 *
 *   uniform  a 4x4 texture and the whole quad, so every sample of every texel holds the
 *            fill colour. Apple's four averaging shaders then have to print exactly that
 *            colour -- they loop i = 0 .. get_num_samples()-1, sum read(coord, i) and
 *            divide by the count -- so the number says the texture was filled, bound and
 *            read, and (for the integer ones, where the sum wraps) that all N reads
 *            happened. It does NOT say the sample index selected anything: four equal
 *            samples average to the same value however the index is treated.
 *   split    a 1x1 texture and the first triangle of the strip (vertexCount 3), whose
 *            hypotenuse cuts that single pixel corner to corner. Some of its samples are
 *            covered and some are not, so the texel holds two different values and the
 *            average is neither of them. THAT is the sample index measured: a shader that
 *            read one sample N times, or read sample 0 N times, prints a value that is
 *            one of the two and not the mean.
 *
 * Nothing of the second half of either configuration runs on this machine, and the
 * reason is not this library: softpipe has no multisample textures at all. Its
 * is_format_supported refuses every sample count above one for every format and every
 * binding (softpipe/sp_screen.c), and it says so about itself -- caps->fake_sw_msaa is
 * true, which asks a frontend to pretend with one sample. freedreno's a6xx takes 2 and 4
 * (a6xx/fd6_screen.cc, valid_sample_count). So on this machine the mode measures
 * everything up to the texture and then prints the back end's own refusal, which is the
 * measurement it can make; on the Adreno it measures the rest.
 *
 * What it does measure here, and what makes the phone's number isolable:
 *
 *   - the declaration, from the same query the refusal judges by (rwa_module_textures);
 *   - the six descriptors that MUST be refused, each for its own reason, with the
 *     sentence each gives -- including a sampleCount of 1 on a multisample type, which
 *     is Metal's own rule and not a limit here;
 *   - pass 1 at ONE sample into an ordinary 2D texture, read back: that is the fill
 *     colour the multisample run has to end up printing, and the coverage map of the
 *     triangle, measured rather than assumed;
 *   - the whole chain at one sample for a shader that declares a plain texture2d, which
 *     is this mode run with SAMPLES = 1 on (say) texturedFragment2DUInt: render into a
 *     texture in one pass and sample it in the next. That is the control for the phone --
 *     if the multisample run comes back wrong, this says whether render-then-sample was
 *     ever right.
 *
 *   air_metal_test --draw-ms FILE.metallib FUNCTION [SAMPLES [FILLFUNCTION]]
 */

/* The pixel format for a declared element class: the 32-bit RGBA format of that class,
 * which is what the three test programs of this port bind and read back, so a texel
 * written here and a texel read on the phone are the same number. */
static unsigned
ms_format_for_element(unsigned element)
{
   switch (element) {
   case RWA_ELEMENT_INT:  return MTL_RGBA32_SINT;
   case RWA_ELEMENT_UINT: return MTL_RGBA32_UINT;
   default:               return MTL_RGBA32_FLOAT;
   }
}

static void
print_components(unsigned element, const uint32_t v[4])
{
   for (unsigned c = 0; c < 4; c++) {
      if (element == RWA_ELEMENT_INT) {
         printf(" %d", (int32_t)v[c]);
      } else if (element == RWA_ELEMENT_UINT) {
         printf(" %u", v[c]);
      } else {
         float f;
         memcpy(&f, &v[c], sizeof(f));
         printf(" %.9g", (double)f);
      }
   }
}

/* A descriptor that must not be made, and the sentence it is refused with. The sentence
 * is the point: a refusal nobody can read is a refusal that will be mistaken for a
 * limitation of the hardware. */
static bool
ms_must_refuse(struct rwa_device *device, const struct rwa_texture_shape *ts,
               const char *what)
{
   char why[WHY] = "";
   struct rwa_texture *t = rwa_texture_create(device, ts, why, sizeof(why));

   if (t) {
      printf("air_metal_test: %s was ACCEPTED and it must not be\n", what);
      rwa_texture_destroy(t);
      return false;
   }
   printf("air_metal_test: %s is refused: %s\n", what, why);
   return true;
}

static int
run_ms(const char *path, const char *func, const char *samplespec, const char *fillfunc)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;

   /* The fill shader, out of the same container: on the device both stages' bytes come
    * from two -[_MTLFunction bitcodeData] calls on one MTLLibrary, which is these two
    * find_function calls. clearFragment20 by default because its colour is already
    * attested -- it is the shader of docs/metal.md 10.22, whose pixel is the four words
    * 2458616957 2930221209 3401825461 3873429713 in a RGBA32Uint attachment.
    */
   char fname[256];
   const uint8_t *fbitcode = NULL;
   size_t fbitcode_size = 0;
   if (!fillfunc || !*fillfunc)
      fillfunc = "clearFragment20";
   if (!find_function(&lib, fillfunc, fname, sizeof(fname), &fbitcode, &fbitcode_size))
      return 1;

   unsigned samples = samplespec && *samplespec
                         ? (unsigned)strtoul(samplespec, NULL, 0) : 4;
   if (samples == 0)
      samples = 1;

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-ms %s on \"%s\", %u sample%s per pixel, filled by %s\n",
          name, rwa_device_name(device), samples, samples == 1 ? "" : "s", fname);

   /*
    * What the shader says it wants bound, asked of the same query the binding check
    * judges by. The texture this mode makes is that declaration plus the sample count
    * asked for on the command line, and nothing else: a type invented here would make
    * the refusal below measure this program.
    */
   struct rwa_texture_decl decl[16];
   unsigned ndecl = 0;
   if (rwa_module_textures(device, bitcode, bitcode_size, NULL, decl, COUNT_OF(decl),
                           &ndecl, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_module_textures: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   if (ndecl != 1 || decl[0].index != 0) {
      printf("air_metal_test: %s declares %u texture slots, and this mode fills one at "
             "index 0\n", name, ndecl);
      rwa_device_close(device);
      return 1;
   }
   unsigned type = decl[0].type_known ? decl[0].type : MTL_TEXTURE_TYPE_2D;
   unsigned element = decl[0].element;
   unsigned format = ms_format_for_element(element);
   printf("air_metal_test: it declares texture 0 as %s: %s, element %s%s, usage 0x%x; "
          "bound as %s of pixel format %u\n", decl[0].metal_type[0] ? decl[0].metal_type
                                                                    : "(no type name)",
          texture_type_name(type), element_name(element),
          decl[0].element_bits ? "" : " (from the instructions, width unstated)",
          decl[0].usage, texture_type_name(type), format);

   /*
    * The descriptors that must be refused, each for its own reason. Five of the six are
    * Metal's own rules or the shader language's, and only the last is a back end's --
    * which is what makes the last one the interesting line: it is the one whose answer
    * differs between this machine and the phone.
    */
   /* Four samples whatever the command line asked for: these five lines are about
    * Metal's rules and the back end's list of counts, not about the count under test,
    * and with SAMPLES = 1 (the one-sample control run) a battery built from it would
    * measure nothing -- every line would be refused by the first rule.
    */
   const struct rwa_texture_shape base = {
      .pixel_format = format, .type = MTL_TEXTURE_TYPE_2D_MULTISAMPLE,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 4,
      .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
   };
   unsigned refused = 0, must = 0;
   struct rwa_texture_shape ts;

   ts = base; ts.sample_count = 1;
   must++; refused += ms_must_refuse(device, &ts,
      "a MTLTextureType2DMultisample with a sampleCount of 1") ? 1 : 0;
   ts = base; ts.mip_levels = 2;
   must++; refused += ms_must_refuse(device, &ts,
      "a four-sample texture with two mip levels") ? 1 : 0;
   ts = base; ts.usage |= MTL_USAGE_SHADER_WRITE;
   must++; refused += ms_must_refuse(device, &ts,
      "a four-sample texture with MTLTextureUsageShaderWrite") ? 1 : 0;
   ts = base; ts.type = MTL_TEXTURE_TYPE_2D;
   must++; refused += ms_must_refuse(device, &ts,
      "a MTLTextureType2D with a sampleCount of 4") ? 1 : 0;
   ts = base; ts.sample_count = 3;
   must++; refused += ms_must_refuse(device, &ts,
      "a multisample texture of three samples, which no back end has") ? 1 : 0;
   printf("air_metal_test: %u of %u descriptors that must be refused were\n", refused,
          must);

   /*
    * One of the six cannot be compiled on this machine at all, and the reason is neither
    * Metal's nor this library's: texturedFragment2DStencilMultisample reads 16-bit
    * texels (texture2d_ms<ushort, read>, and its read returns <4 x i16>), and the path
    * softpipe takes NIR through has no 16-bit texture return type --
    * nir_to_tgsi.c's ntt_emit_texture switches on nir_type_float32/int32/uint32 and
    * calls unreachable("unknown texture type") on anything else, which ABORTS THE
    * PROCESS. Its non-multisample twin texturedFragment2DStencil aborts the same way and
    * always has, which is part of why 263 of Apple's fragment shaders draw on the Adreno
    * and only 256 are compared with a host reference.
    *
    * So it is said and skipped rather than run into. This is the one place this program
    * looks at which back end it got, because the difference is the back end's: ir3
    * compiles 16-bit texture reads, and a crash here would read as a fault of the
    * library under test.
    */
   if (decl[0].element_bits == 16 && !strcmp(rwa_device_name(device), "softpipe")) {
      printf("air_metal_test: %s reads %u-bit texels, and nir_to_tgsi -- the path softpipe "
             "takes NIR through -- has no 16-bit texture return type: ntt_emit_texture "
             "calls unreachable(\"unknown texture type\") and the process dies. Not "
             "attempted here; on the Adreno ir3 compiles it\n", name, decl[0].element_bits);
      rwa_device_close(device);
      return 0;
   }

   /*
    * Pass 1 at ONE sample, into an ordinary 2D texture, read back. Two things come out
    * of this and both are needed to read the multisample run:
    *
    *   - the fill colour. Whatever clearFragment20 writes, the averaging shaders have to
    *     print exactly that when every sample holds it, and this is the only place the
    *     number can be measured rather than asserted (a multisample texture cannot be
    *     read back at all).
    *   - the coverage map of the triangle, which says which pixels of the quad's first
    *     triangle are inside it at one sample. The pixel the split configuration hangs
    *     on is a pixel this map shows as covered, drawn again with the same triangle at
    *     N samples: its samples are then the ones the hypotenuse cuts.
    */
   struct rwa_render_shape fill_shape = { .color_count = 3, .sample_count = 1 };
   for (unsigned i = 0; i < 3; i++)
      fill_shape.color_format[i] = format;
   struct rwa_render_info finfo = { 0 };
   struct rwa_render *fill =
      rwa_render_create(device, NULL, 0, NULL, fbitcode, fbitcode_size, fillfunc,
                        &fill_shape, &finfo, why, sizeof(why));
   if (!fill) {
      printf("air_metal_test: rwa_render_create (the fill pass at one sample): %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   struct rwa_buffer *fbuf[16] = { 0 };
   unsigned nfbuf = finfo.fragment_buffers < COUNT_OF(fbuf) ? finfo.fragment_buffers
                                                            : (unsigned)COUNT_OF(fbuf);
   for (unsigned i = 0; i < nfbuf; i++) {
      fbuf[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!fbuf[i]) {
         printf("air_metal_test: rwa_buffer_create (fill buffer %u): %s\n", i, why);
         return 1;
      }
      uint8_t *bits = rwa_buffer_map(fbuf[i]);
      for (unsigned k = 0; k < rwa_buffer_size(fbuf[i]); k++)
         bits[k] = pattern_byte(k);
   }

   uint32_t filled[4] = { 0 };
   {
      struct rwa_texture *one[3] = { 0 };
      for (unsigned i = 0; i < 3; i++) {
         const struct rwa_texture_shape ots = {
            .pixel_format = format, .type = MTL_TEXTURE_TYPE_2D,
            .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
            .sample_count = 1,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         one[i] = rwa_texture_create(device, &ots, why, sizeof(why));
         if (!one[i]) {
            printf("air_metal_test: rwa_texture_create (one-sample attachment %u): %s\n",
                   i, why);
            return 1;
         }
      }
      for (unsigned tri = 0; tri < 2; tri++) {
         const struct rwa_draw d = {
            .color = one, .color_count = 3,
            .clear_color = (const float[]){ 0, 0, 0, 0 },
            .fragment = { .buffers = fbuf, .buffer_count = nfbuf },
            .primitive = 4,                    /* MTLPrimitiveTypeTriangleStrip */
            .vertex_start = 0, .vertex_count = tri ? 3 : 4,
         };
         if (rwa_render_draw(fill, &d, why, sizeof(why)) != 0) {
            printf("air_metal_test: rwa_render_draw (the fill pass, %u vertices): %s\n",
                   d.vertex_count, why);
            return 1;
         }
         const unsigned one_texel[3] = { 1, 1, 1 };
         printf("air_metal_test: the fill pass at one sample, %u vertices: attachment 2 "
                "holds\n", d.vertex_count);
         for (unsigned y = 0; y < TEX; y++) {
            printf("air_metal_test:  ");
            for (unsigned x = 0; x < TEX; x++) {
               const unsigned at[3] = { x, y, 0 };
               uint32_t px[4] = { 0 };
               if (rwa_texture_read(one[2], 0, 0, at, one_texel, px, 0, why,
                                    sizeof(why)) != 0) {
                  printf("air_metal_test: rwa_texture_read: %s\n", why);
                  return 1;
               }
               printf(" %s", px[0] || px[1] || px[2] || px[3] ? "drawn" : "clear");
               if (!tri && x == 0 && y == 0)
                  memcpy(filled, px, sizeof(filled));
            }
            printf("\n");
         }
      }
      printf("air_metal_test: so the fill colour is");
      print_components(element, filled);
      printf(", and that is what a mean over samples that all hold it must come to\n");

      /* And the attachment check itself, measured rather than described. The helper that
       * judges an attachment now reads its sample count as well as its type, and a
       * wrong TYPE is the half of it this machine can exercise: a 3D texture is
       * makeable here and a multisample one is not.
       */
      {
         const struct rwa_texture_shape vol = {
            .pixel_format = format, .type = MTL_TEXTURE_TYPE_3D,
            .width = TEX, .height = TEX, .depth = TEX, .array_length = 1,
            .mip_levels = 1, .sample_count = 1,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         struct rwa_texture *t3 = rwa_texture_create(device, &vol, why, sizeof(why));
         if (t3) {
            struct rwa_texture *wrong[3] = { t3, one[1], one[2] };
            const struct rwa_draw d = {
               .color = wrong, .color_count = 3,
               .fragment = { .buffers = fbuf, .buffer_count = nfbuf },
               .primitive = 4, .vertex_start = 0, .vertex_count = 4,
            };
            if (rwa_render_draw(fill, &d, why, sizeof(why)) == 0)
               printf("air_metal_test: a 3D texture as colour attachment 0 was ACCEPTED "
                      "and it must not be\n");
            else
               printf("air_metal_test: a 3D texture as colour attachment 0 is refused: "
                      "%s\n", why);
            rwa_texture_destroy(t3);
         }
      }
      for (unsigned i = 0; i < 3; i++)
         rwa_texture_destroy(one[i]);
   }
   rwa_render_destroy(fill);

   /*
    * The measuring pipeline: an ordinary one-sample pass with the shader under test.
    * Four colour attachments because the four averaging shaders write four render
    * targets; a shader that writes one leaves the other three holding the clear.
    *
    * A depth attachment only if the shader stores to [[depth]], which is asked by
    * trying without one: texturedFragment2DDepthMultisample writes depth and nothing to
    * colour, and this library refuses such a pipeline by name when there is nowhere for
    * the store to go (docs/metal.md 10.30). Refusing first and retrying is how the
    * question gets asked without a table in this program saying which shaders they are.
    */
   struct rwa_render_shape shape = { .color_count = 4, .sample_count = 1 };
   for (unsigned i = 0; i < 4; i++)
      shape.color_format[i] = format;
   struct rwa_render_info info = { 0 };
   struct rwa_render *render =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape, &info,
                        why, sizeof(why));
   if (!render) {
      printf("air_metal_test: rwa_render_create with no depth attachment: %s\n", why);
      shape.depth_format = MTL_DEPTH32_FLOAT;
      render = rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func,
                                 &shape, &info, why, sizeof(why));
      if (!render) {
         printf("air_metal_test: rwa_render_create with one: %s\n", why);
         rwa_device_close(device);
         return 1;
      }
   }
   printf("air_metal_test: pipeline: %u fragment buffers, %u fragment textures, %u "
          "samplers, vertex stage %s, and the fragment shader stores %s to [[depth]], "
          "depth attachment %s\n", info.fragment_buffers, info.fragment_textures,
          info.fragment_samplers, info.generated_vertex_stage ? "generated" : "supplied",
          info.fragment_writes_depth ? "a value" : "nothing",
          shape.depth_format ? "there" : "none");

   struct rwa_buffer *buffers[16] = { 0 };
   unsigned nbuf = info.fragment_buffers < COUNT_OF(buffers) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < nbuf; i++) {
      buffers[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create (buffer %u): %s\n", i, why);
         return 1;
      }
      uint8_t *bits = rwa_buffer_map(buffers[i]);
      for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
         bits[k] = pattern_byte(k);
   }
   /* One sampler per slot the API is expected to fill, even though every one of these
    * shaders reads with read(coord, sample) and so ignores it: Apple declares the
    * sampler argument and marks it air.arg_unused, and the contract still counts a slot
    * the interface has. In pixels and nearest, which is what a read() would want if it
    * looked. */
   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.fragment_samplers < COUNT_OF(samplers)
                       ? info.fragment_samplers : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
         .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 0,
         .lod_min = 0.0f, .lod_max = 0.0f,
      };
      samplers[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }

   /* Pass 2's own attachments: four colour, one sample, and a depth one if the shader
    * writes depth -- in which case the depth buffer IS the result and the colour is
    * not, as in docs/metal.md 10.30. */
   struct rwa_texture *color[4] = { 0 };
   for (unsigned i = 0; i < 4; i++) {
      const struct rwa_texture_shape cts = {
         .pixel_format = format, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
      };
      color[i] = rwa_texture_create(device, &cts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }
   struct rwa_texture *zt = NULL;
   if (shape.depth_format) {
      const struct rwa_texture_shape zts = {
         .pixel_format = shape.depth_format, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1, .usage = MTL_USAGE_RENDER_TARGET,
      };
      zt = rwa_texture_create(device, &zts, why, sizeof(why));
      if (!zt) {
         printf("air_metal_test: rwa_texture_create (the depth attachment): %s\n", why);
         return 1;
      }
   }

   /*
    * And the two configurations. Each makes the texture the shader declared -- with the
    * sample count asked for -- renders the fill into it, and then draws the shader under
    * test with it bound.
    */
   static const struct { const char *what; unsigned size, verts; } config[] = {
      { "uniform: every sample of every texel covered", TEX, 4 },
      { "split: one pixel, cut corner to corner by the triangle's hypotenuse", 1, 3 },
   };
   int rc = 0;
   for (unsigned c = 0; c < COUNT_OF(config); c++) {
      printf("air_metal_test: --- %s\n", config[c].what);

      struct rwa_texture *ms[3] = { 0 };
      bool made = true;
      for (unsigned i = 0; i < 3 && made; i++) {
         const struct rwa_texture_shape mts = {
            .pixel_format = format, .type = type,
            .width = config[c].size, .height = config[c].size, .depth = 1,
            .array_length = 1, .mip_levels = 1,
            .sample_count = samples,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         ms[i] = rwa_texture_create(device, &mts, why, sizeof(why));
         if (!ms[i]) {
            printf("air_metal_test: the %ux%u %s of %u samples could not be made: %s\n",
                   config[c].size, config[c].size, texture_type_name(type), samples, why);
            made = false;
            rc = 1;
         }
      }
      if (made) {
         /* Neither the processor's way in nor its way out exists for a multisample
          * texture, and both refusals are measured here rather than described: a caller
          * that believed either worked would be reading or writing bytes in no defined
          * sample. */
         if (samples > 1) {
            uint32_t one_texel[4] = { 1, 2, 3, 4 };
            const unsigned at[3] = { 0, 0, 0 }, sz[3] = { 1, 1, 1 };
            if (rwa_texture_write(ms[0], 0, 0, at, sz, one_texel, 0, why,
                                  sizeof(why)) == 0)
               printf("air_metal_test: rwa_texture_write into a multisample texture was "
                      "ACCEPTED and it must not be\n");
            else
               printf("air_metal_test: writing texels into it is refused: %s\n", why);
            if (rwa_texture_read(ms[0], 0, 0, at, sz, one_texel, 0, why, sizeof(why)) == 0)
               printf("air_metal_test: rwa_texture_read of a multisample texture was "
                      "ACCEPTED and it must not be\n");
            else
               printf("air_metal_test: reading its texels back is refused: %s\n", why);
         }

         /* The fill pass of this configuration's own sample count. On softpipe this is
          * where a multisample run stops if the texture did not already stop it. */
         struct rwa_render_shape ms_shape = { .color_count = 3, .sample_count = samples };
         for (unsigned i = 0; i < 3; i++)
            ms_shape.color_format[i] = format;
         struct rwa_render_info ms_info = { 0 };
         struct rwa_render *ms_fill =
            rwa_render_create(device, NULL, 0, NULL, fbitcode, fbitcode_size, fillfunc,
                              &ms_shape, &ms_info, why, sizeof(why));
         if (!ms_fill) {
            printf("air_metal_test: the fill pipeline of %u samples: %s\n", samples, why);
            rc = 1;
         } else {
            const struct rwa_draw fd = {
               .color = ms, .color_count = 3,
               .clear_color = (const float[]){ 0, 0, 0, 0 },
               .fragment = { .buffers = fbuf, .buffer_count = nfbuf },
               .primitive = 4,
               .vertex_start = 0, .vertex_count = config[c].verts,
            };
            if (rwa_render_draw(ms_fill, &fd, why, sizeof(why)) != 0) {
               printf("air_metal_test: the fill draw of %u vertices into %u samples: %s\n",
                      config[c].verts, samples, why);
               rc = 1;
            } else {
               printf("air_metal_test: filled a %ux%u %s of %u sample%s with %u "
                      "vertices\n", config[c].size, config[c].size,
                      texture_type_name(type), samples, samples == 1 ? "" : "s",
                      config[c].verts);

               struct rwa_texture *bound[1] = { ms[2] };
               const struct rwa_draw d = {
                  .color = color, .color_count = 4,
                  .clear_color = (const float[]){ 0, 0, 0, 0 },
                  .depth = zt,
                  .clear_depth = zt ? (const float[]){ 1.0f } : NULL,
                  .fragment = { .buffers = buffers, .buffer_count = nbuf,
                                .textures = bound, .texture_count = 1,
                                .samplers = samplers, .sampler_count = nsamp },
                  .primitive = 4, .vertex_start = 0, .vertex_count = 4,
               };
               if (rwa_render_draw(render, &d, why, sizeof(why)) != 0) {
                  printf("air_metal_test: the measuring draw: %s\n", why);
                  rc = 1;
               } else if (info.fragment_writes_depth) {
                  float z[TEX * TEX];
                  const unsigned at[3] = { 0, 0, 0 }, sz[3] = { TEX, TEX, 1 };
                  if (rwa_depth_read(zt, 0, 0, at, sz, z, NULL, why, sizeof(why)) != 0) {
                     printf("air_metal_test: rwa_depth_read: %s\n", why);
                     rc = 1;
                  } else {
                     bool same = true;
                     for (unsigned i = 1; i < TEX * TEX; i++)
                        same = same && z[i] == z[0];
                     printf("air_metal_test: %s stored %.9g to [[depth]]%s\n", name,
                            (double)z[0], same ? ", in all 16 pixels" : " at (0, 0)");
                  }
               } else {
                  const unsigned at[3] = { 0, 0, 0 }, sz[3] = { 1, 1, 1 };
                  uint32_t px[4] = { 0 };
                  if (rwa_texture_read(color[0], 0, 0, at, sz, px, 0, why,
                                       sizeof(why)) != 0) {
                     printf("air_metal_test: rwa_texture_read (attachment 0): %s\n", why);
                     rc = 1;
                  } else {
                     printf("air_metal_test: %s printed", name);
                     print_components(element, px);
                     printf("\n");
                  }
               }
            }
            rwa_render_destroy(ms_fill);
         }
      }
      for (unsigned i = 0; i < 3; i++)
         rwa_texture_destroy(ms[i]);
   }

   rwa_render_destroy(render);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < nfbuf; i++)
      rwa_buffer_destroy(fbuf[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   for (unsigned i = 0; i < 4; i++)
      rwa_texture_destroy(color[i]);
   rwa_texture_destroy(zt);
   rwa_device_close(device);
   return rc;
}

/*
 * --draw-index: whether the index buffer is read, and whether it is read as Metal says.
 *
 * Every drawing call in a real application is -drawIndexedPrimitives:, and until this
 * mode nothing in this library had drawn one. The hard part of measuring it is not
 * getting a picture -- it is getting a picture that could not have come out of a driver
 * that ignored the index buffer altogether. A draw of indices 0,1,2,3 over the generated
 * quad is exactly the picture the non-indexed draw of vertices 0..3 gives, so on its own
 * it proves nothing: it is the control, not the measurement.
 *
 * So the measurement is a PERMUTATION. One index buffer holds
 *
 *     0 1 2 3   1 2 3
 *
 * and three indices out of it make one triangle of the quad:
 *
 *   - 0,1,2 (at byte 0) is (-1,-1), (1,-1), (-1,1): the half of the attachment below the
 *     diagonal, and the pixel at (0, 0) is its right angle;
 *   - 1,2,3 (at byte 8) is (1,-1), (-1,1), (1,1): the other half, and (3, 3) is its.
 *
 * Those two draws differ in nothing but WHERE IN THE INDEX BUFFER they start -- same
 * count, same primitive, same base vertex, same buffer -- so a driver that ignored the
 * index buffer or the offset would draw the first triangle twice and (3, 3) would come
 * back as the clear colour both times. The two corners are what the verdict is made of
 * because their pixel centres are strictly inside one triangle each: the four pixels on
 * the diagonal (x + y = 3) have their centres exactly ON the shared edge, and which
 * triangle gets them is the rasteriser's fill rule, so the total covered count is
 * printed and nothing is concluded from it.
 *
 * The same indices are then drawn as MTLIndexTypeUInt32, which must give the same two
 * pictures: an implementation that read 32-bit indices two bytes at a time would see
 * 0, 0, 1 -- a triangle of zero area -- and leave the attachment at its clear colour.
 *
 * baseVertex is measured against the permutation rather than on its own: indices 0,1,2
 * with baseVertex 1 are the vertex sequence 1,2,3, which is the very sequence the second
 * triangle above is drawn from. So that draw must give the SECOND picture exactly -- not
 * merely something different from the first -- and a driver that dropped baseVertex
 * would give the first.
 *
 * instanceCount cannot be seen in a colour attachment at all: every instance of this
 * geometry covers the same pixels with the same colour, so one instance and three are the
 * same picture. It is measured in the STENCIL instead, with the operation that counts:
 * compare Always and depthStencilPassOperation IncrementClamp over a buffer cleared to
 * zero leaves the number of fragments that covered each pixel, so the buffer holds the
 * instance count exactly. Three instances must leave 3; a draw that dropped the count
 * leaves 1, which is a different number and not a different-looking picture.
 *
 *   air_metal_test --draw-index FILE.metallib FUNCTION [VERTEXFUNCTION]
 *
 * With a VERTEXFUNCTION the two halves are drawn a second time over a SUPPLIED vertex
 * stage -- Apple's own clearVertex is `position = vtx[vertex_id]` -- and that is the path
 * a real application is on: there an index is not a row of a vertex attribute but the
 * [[vertex_id]] the shader indexes its own buffer with, so the permutation has to move
 * the triangle the same way through the frontend's air.vertex_id.
 */

/* The index buffer every picture step is drawn from: the strip of four, then the second
 * triangle, so that no two steps need two buffers. One list, widened for UInt32, so the
 * two index types cannot quietly be given different geometry.
 */
static const uint16_t index_values[7] = { 0, 1, 2, 3, 1, 2, 3 };

/* One picture: the whole 4x4 attachment, and how many of its pixels the draw covered.
 * The clear colour is zero and clearFragment20's first word is 2458616957, which is
 * checked before anything is counted -- for a shader whose first word were zero,
 * "covered" would be counting the clear.
 */
struct index_picture {
   uint32_t word[TEX * TEX][4];
   unsigned covered;
};

static bool
index_covered(const struct index_picture *p, unsigned x, unsigned y)
{
   return p->word[y * TEX + x][0] != 0;
}

/* How many of the 64 words of two pictures differ. Zero is "the same picture", and it is
 * the whole attachment and not one pixel: two triangles that differ only along the
 * diagonal differ in four pixels, which a corner-and-corner comparison would miss.
 */
static unsigned
index_picture_diff(const struct index_picture *a, const struct index_picture *b)
{
   unsigned n = 0;
   for (unsigned i = 0; i < TEX * TEX; i++)
      for (unsigned c = 0; c < 4; c++)
         if (a->word[i][c] != b->word[i][c])
            n++;
   return n;
}

/* One draw, into attachments cleared to zero, and the picture that came of it. The clear
 * is forced here rather than left to the caller so that every step starts from the same
 * attachment, and a step that drew nothing is distinguishable from a step that was not
 * run.
 */
static bool
index_draw(struct rwa_render *render, struct rwa_draw *draw,
           struct rwa_texture *const *color, unsigned color_count,
           struct index_picture *out, const char *what)
{
   char why[WHY] = "";
   const unsigned origin[3] = { 0, 0, 0 };
   const unsigned extent[3] = { TEX, TEX, 1 };
   static const float black[4] = { 0, 0, 0, 0 };

   draw->clear_color = black;
   memset(out, 0, sizeof(*out));
   if (rwa_render_draw(render, draw, why, sizeof(why)) != 0) {
      printf("air_metal_test: %s: rwa_render_draw: %s\n", what, why);
      return false;
   }
   if (rwa_texture_read(color[color_count - 1], 0, 0, origin, extent, out->word, 0, why,
                        sizeof(why)) != 0) {
      printf("air_metal_test: %s: rwa_texture_read: %s\n", what, why);
      return false;
   }
   for (unsigned i = 0; i < TEX * TEX; i++)
      out->covered += out->word[i][0] != 0;
   return true;
}

/* The picture, as four rows of four, and the two corners the verdicts are made of. */
static void
index_print(const struct index_picture *p)
{
   printf("air_metal_test:   coverage ");
   for (unsigned y = 0; y < TEX; y++) {
      for (unsigned x = 0; x < TEX; x++)
         printf("%c", index_covered(p, x, y) ? '#' : '.');
      printf("%s", y + 1 < TEX ? "/" : "");
   }
   printf("  %u of %u pixels, (0,0) %s, (3,3) %s\n", p->covered, TEX * TEX,
          index_covered(p, 0, 0) ? "drawn" : "clear",
          index_covered(p, TEX - 1, TEX - 1) ? "drawn" : "clear");
}

static int
run_index(const char *path, const char *func, const char *vfunc)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;
   char vname[256];
   const uint8_t *vbitcode = NULL;
   size_t vbitcode_size = 0;
   if (vfunc && !find_function(&lib, vfunc, vname, sizeof(vname), &vbitcode,
                               &vbitcode_size))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-index %s on \"%s\"\n", name, rwa_device_name(device));

   /* clearFragment20's three RGBA32Uint attachments, as --draw's default: it writes
    * render target 2, so 0 and 1 have to exist for 2 to be bound at all, and 2 is the one
    * read back. 4x4 is air_run's attachment, so a pixel here and a pixel there are the
    * same measurement.
    */
   const unsigned color_count = 3;
   struct rwa_texture *color[3] = { 0 };
   for (unsigned i = 0; i < color_count; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = MTL_RGBA32_UINT, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
      };
      color[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }

   /*
    * TWO pipelines of the same fragment module, and what each is for:
    *
    *   - `plain` has no depth attachment, so the picture steps go through exactly the
    *     state the 256 pixel-compared fragment shaders went through (10.30: clip_halfz
    *     is set only for a pipeline that has a depth attachment). An indexed picture and
    *     a non-indexed one are then comparable, which is the whole of step 2.
    *   - `counted` has Depth32Float_Stencil8, because the instance count can only be
    *     measured in a stencil buffer and Metal requires a pass's formats to be the
    *     pipeline's -- so it cannot be the same pipeline.
    */
   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = MTL_RGBA32_UINT;
   struct rwa_render_shape zshape = shape;
   zshape.depth_format = MTL_DEPTH32_FLOAT_STENCIL8;
   zshape.stencil_format = MTL_DEPTH32_FLOAT_STENCIL8;

   struct rwa_render_info info = { 0 }, zinfo = { 0 };
   struct rwa_render *plain =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape, &info,
                        why, sizeof(why));
   if (!plain) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      return 1;
   }
   struct rwa_render *counted =
      rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &zshape,
                        &zinfo, why, sizeof(why));
   if (!counted) {
      printf("air_metal_test: rwa_render_create (with a stencil attachment): %s\n", why);
      return 1;
   }
   printf("air_metal_test: pipeline: %u fragment buffers, %u fragment textures, %u "
          "samplers, vertex stage %s\n", info.fragment_buffers, info.fragment_textures,
          info.fragment_samplers, info.generated_vertex_stage ? "generated" : "supplied");
   if (info.fragment_textures || info.fragment_samplers) {
      printf("air_metal_test: this mode binds no textures and this shader samples %u, so "
             "it would be refused for the binding and not measured for the indices: pick "
             "one that samples nothing, as clearFragment20 does\n",
             info.fragment_textures);
      return 2;
   }

   /* The fragment stage's constant buffers, air_run's pattern, the same in every step:
    * what is compared here is coverage, so the colour must NOT change between draws
    * (run_depth varies it on purpose, for the opposite reason -- there a draw that was
    * not discarded has to leave a colour that was not there).
    */
   struct rwa_buffer *buffers[16] = { 0 };
   unsigned nbuf = info.fragment_buffers < COUNT_OF(buffers) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < nbuf; i++) {
      buffers[i] = rwa_buffer_create(device, 48, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create (buffer %u): %s\n", i, why);
         return 1;
      }
      uint8_t *bits = rwa_buffer_map(buffers[i]);
      for (unsigned k = 0; k < rwa_buffer_size(buffers[i]); k++)
         bits[k] = pattern_byte(k);
   }

   /* The two index buffers, one list of indices in two widths, and one more holding an
    * index past the quad for the refusal that needs it. An MTLBuffer is what Metal's
    * index buffer is, so these are rwa_buffers like any other -- nothing was added to
    * the contract to make a buffer that could be one.
    */
   struct rwa_buffer *i16 = rwa_buffer_create(device, sizeof(index_values), why,
                                              sizeof(why));
   struct rwa_buffer *i32 = rwa_buffer_create(device, sizeof(index_values) * 2, why,
                                              sizeof(why));
   static const uint16_t past_values[3] = { 0, 1, 4 };
   struct rwa_buffer *past = rwa_buffer_create(device, sizeof(past_values), why,
                                               sizeof(why));
   if (!i16 || !i32 || !past) {
      printf("air_metal_test: rwa_buffer_create (an index buffer): %s\n", why);
      return 1;
   }
   memcpy(rwa_buffer_map(i16), index_values, sizeof(index_values));
   for (unsigned i = 0; i < COUNT_OF(index_values); i++)
      ((uint32_t *)rwa_buffer_map(i32))[i] = index_values[i];
   memcpy(rwa_buffer_map(past), past_values, sizeof(past_values));
   printf("air_metal_test: the index buffer holds");
   for (unsigned i = 0; i < COUNT_OF(index_values); i++)
      printf(" %u", index_values[i]);
   printf(", as %zu bytes of MTLIndexTypeUInt16 and %zu of UInt32\n",
          rwa_buffer_size(i16), rwa_buffer_size(i32));

   /* What never changes between steps: the attachments, the fragment bindings and the
    * primitive. MTLPrimitiveTypeTriangleStrip is what all 256 compared shaders are drawn
    * with, and a strip of three is one triangle of it -- which is how 10.34's split
    * configuration got a partly covered pixel without adding anything to the contract.
    */
   const struct rwa_draw base = {
      .color = color, .color_count = color_count,
      .fragment = { .buffers = buffers, .buffer_count = nbuf },
      .primitive = 4,
   };

   bool ok = true;
   struct index_picture full, idx_full, lo, hi, bias, lo32, hi32;
   struct rwa_draw d;

   /* 1. The reference: -drawPrimitives:vertexStart:0 vertexCount:4, the only form this
    *    library had. Its four words are the fill colour every step below is counted by,
    *    and they have to be 10.22's or nothing here is comparable with anything.
    */
   d = base;
   d.vertex_count = 4;
   if (!index_draw(plain, &d, color, color_count, &full, "the non-indexed strip of 4"))
      return 1;
   printf("air_metal_test: 1. -drawPrimitives: vertexStart 0 vertexCount 4\n");
   printf("air_metal_test:   the fill colour is %u %u %u %u\n", full.word[0][0],
          full.word[0][1], full.word[0][2], full.word[0][3]);
   index_print(&full);
   if (full.word[0][0] == 0) {
      printf("air_metal_test: this shader's first word is zero, so a covered pixel cannot "
             "be told from the clear colour and nothing below would mean anything\n");
      return 2;
   }
   if (full.covered != TEX * TEX)
      ok = false;

   /* 2. The same four vertices through the index buffer. This is the CONTROL and says so:
    *    it must be the same picture, and a driver ignoring the index buffer passes it too.
    */
   d = base;
   d.vertex_count = 4;
   d.index_buffer = i16;
   if (!index_draw(plain, &d, color, color_count, &idx_full, "the indexed strip of 4"))
      return 1;
   printf("air_metal_test: 2. -drawIndexedPrimitives: 4 indices [0 1 2 3], UInt16, "
          "offset 0\n");
   index_print(&idx_full);
   {
      unsigned diff = index_picture_diff(&full, &idx_full);
      printf("air_metal_test:   %s step 1: %u of %u words differ%s\n",
             diff ? "DIFFERS from" : "the same as", diff, TEX * TEX * 4,
             diff ? "" : " -- the control: a draw that ignored the index buffer passes "
                         "this one as well");
      if (diff)
         ok = false;
   }

   /* 3 and 4. The measurement: the same three-index draw at two places in one buffer. */
   static const struct { const char *what; size_t at16, at32; } tri[2] = {
      { "indices [0 1 2]: the half below the diagonal", 0,     0 },
      { "indices [1 2 3]: the half above it",           4 * 2, 4 * 4 },
   };
   struct index_picture *half[2] = { &lo, &hi };
   for (unsigned t = 0; t < 2; t++) {
      d = base;
      d.vertex_count = 3;
      d.index_buffer = i16;
      d.index_buffer_offset = tri[t].at16;
      if (!index_draw(plain, &d, color, color_count, half[t], tri[t].what))
         return 1;
      printf("air_metal_test: %u. %s (UInt16 at byte %zu)\n", 3 + t, tri[t].what,
             tri[t].at16);
      index_print(half[t]);
   }
   {
      /*
       * EACH TRIANGLE'S OWN RIGHT ANGLE, in the corner Metal's orientation puts it in --
       * and the corners are not the ones this check was written with, which is worth the
       * paragraph because the test has been failing on it for a day.
       *
       * The quad is a strip of (-1,-1), (1,-1), (-1,1), (1,1), so indices [0 1 2] are a
       * triangle whose right angle is at clip (-1,-1) and [1 2 3] one whose right angle
       * is at (1,1). Where those land in memory is the viewport's business, and the
       * viewport changed after this check was written: `air_metal_test --draw-index`
       * came in on 21 October at 11:37 and the NEGATIVE y scale -- clip y = +1 in memory
       * row 0, which is Metal's rule and not OpenGL's (docs/metal.md 10.28) -- came in at
       * 13:49 the same day. So clip (-1,-1) is the BOTTOM-left pixel now where it used to
       * be the top-left one, and a check that asks for the first triangle's right angle
       * at (0, 0) is asking for the picture OpenGL would have drawn.
       *
       * After the flip: [0 1 2] holds (0, TEX-1) and not (TEX-1, 0), and [1 2 3] the
       * other way about. The two pixels on the shared diagonal, (0,0) and (TEX-1,TEX-1),
       * are deliberately not asked about: a pixel centre on an edge belongs to whichever
       * triangle the fill rule gives it, which is a rasteriser's business and not an
       * index buffer's.
       */
      unsigned diff = index_picture_diff(&lo, &hi);
      bool read_them = index_covered(&lo, 0, TEX - 1) &&
                       !index_covered(&lo, TEX - 1, 0) &&
                       index_covered(&hi, TEX - 1, 0) &&
                       !index_covered(&hi, 0, TEX - 1);
      printf("air_metal_test:   the two halves differ in %u of %u words, and %s\n", diff,
             TEX * TEX * 4,
             read_them ? "each right angle is drawn in its own triangle only -- THE INDEX "
                         "BUFFER AND THE OFFSET ARE READ"
                       : "a right angle came back on the wrong side, which is what an "
                         "ignored index buffer or offset gives: the first triangle twice");
      if (!read_them || diff == 0)
         ok = false;
   }

   /* 5. baseVertex, against the permutation: 0,1,2 plus one IS 1,2,3. */
   d = base;
   d.vertex_count = 3;
   d.index_buffer = i16;
   d.base_vertex = 1;
   if (!index_draw(plain, &d, color, color_count, &bias,
                   "indices [0 1 2] with baseVertex 1"))
      return 1;
   printf("air_metal_test: 5. indices [0 1 2] with baseVertex 1, which is the vertex "
          "sequence 1,2,3\n");
   index_print(&bias);
   {
      unsigned from_hi = index_picture_diff(&bias, &hi);
      unsigned from_lo = index_picture_diff(&bias, &lo);
      printf("air_metal_test:   %u of %u words differ from step 4 and %u from step 3: "
             "%s\n", from_hi, TEX * TEX * 4, from_lo,
             from_hi == 0 ? "baseVertex IS added to every index"
             : from_lo == 0 ? "baseVertex was DROPPED -- this is the unbiased picture"
                            : "neither picture, which is a third answer and wants reading");
      if (from_hi != 0)
         ok = false;
   }

   /* 6 and 7. The same indices as UInt32. A reader taking 32-bit indices two bytes at a
    *          time would see 0, 0, 1 and draw a triangle of no area.
    */
   struct index_picture *half32[2] = { &lo32, &hi32 };
   for (unsigned t = 0; t < 2; t++) {
      d = base;
      d.vertex_count = 3;
      d.index_buffer = i32;
      d.index_type = 1;             /* MTLIndexTypeUInt32 */
      d.index_buffer_offset = tri[t].at32;
      if (!index_draw(plain, &d, color, color_count, half32[t], tri[t].what))
         return 1;
      unsigned diff = index_picture_diff(half32[t], half[t]);
      printf("air_metal_test: %u. the same %s as UInt32 at byte %zu: %u of %u words "
             "differ from the UInt16 picture, and it covered %u pixels%s\n", 6 + t,
             t ? "[1 2 3]" : "[0 1 2]", tri[t].at32, diff, TEX * TEX * 4,
             half32[t]->covered,
             half32[t]->covered == 0 ? " -- nothing at all, which is what reading them as "
                                       "UInt16 gives" : "");
      if (diff)
         ok = false;
   }

   /*
    * The refusals. Each is a draw this library must decline by name, and the sentence is
    * printed rather than counted so the reason can be read. They matter beyond tidiness:
    * a driver whose copy of struct rwa_draw is a different shape sends garbage in exactly
    * these fields, and a garbage index type, an offset no index size divides and a count
    * no buffer holds are three of the sentences below -- which is the difference between
    * a mismatched header and a picture of the wrong geometry.
    */
   enum { NO_BUFFER, BUF16, BUF32, BUF_PAST };
   static const struct { const char *what; unsigned count, type, vertex_start, buffer;
                         size_t offset; int base_vertex; } bad[] = {
      { "an index type of 2, which MTLIndexType has not",       3, 2, 0, BUF16,    0, 0 },
      { "UInt16 at an odd offset",                              3, 0, 0, BUF16,    1, 0 },
      { "UInt32 at an offset of 2, which 4 does not divide",    3, 1, 0, BUF32,    2, 0 },
      { "8 indices of a buffer that holds 7",                   8, 0, 0, BUF16,    0, 0 },
      { "an index of 4 over a quad of four corners",            3, 0, 0, BUF_PAST, 0, 0 },
      { "a baseVertex of -1, below the first corner",           3, 0, 0, BUF16,    0, -1 },
      { "a baseVertex with no index buffer at all",             4, 0, 0, NO_BUFFER, 0, 1 },
      { "an indexed draw with a vertexStart, which Metal's "
        "indexed forms have not",                               3, 0, 1, BUF16,    0, 0 },
      { "an indexed draw of no indices",                        0, 0, 0, BUF16,    0, 0 },
   };
   unsigned refused = 0;
   printf("air_metal_test: and the draws that must be refused, each by name:\n");
   for (unsigned i = 0; i < COUNT_OF(bad); i++) {
      static const float black[4] = { 0, 0, 0, 0 };
      struct rwa_draw r = base;

      r.clear_color = black;
      r.vertex_count = bad[i].count;
      r.vertex_start = bad[i].vertex_start;
      r.index_type = bad[i].type;
      r.index_buffer_offset = bad[i].offset;
      r.base_vertex = bad[i].base_vertex;
      r.index_buffer = bad[i].buffer == BUF16 ? i16
                       : bad[i].buffer == BUF32 ? i32
                       : bad[i].buffer == BUF_PAST ? past : NULL;
      why[0] = '\0';
      if (rwa_render_draw(plain, &r, why, sizeof(why)) == 0) {
         printf("air_metal_test:   %s: DREW, and it should not have\n", bad[i].what);
         ok = false;
      } else {
         printf("air_metal_test:   %s: refused -- %s\n", bad[i].what, why);
         refused++;
      }
   }
   printf("air_metal_test: %u of %u refused by name\n", refused, (unsigned)COUNT_OF(bad));
   if (refused != COUNT_OF(bad))
      ok = false;

   /*
    * The instance count, in the stencil buffer, because no colour attachment can hold
    * it: three instances of this geometry are the same pixels in the same colour as one.
    * The depth test is Always with no depth write, so nothing but the instances can add
    * anything to what the stencil counts.
    */
   const struct rwa_texture_shape zts = {
      .pixel_format = MTL_DEPTH32_FLOAT_STENCIL8, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_RENDER_TARGET,
   };
   struct rwa_texture *zt = rwa_texture_create(device, &zts, why, sizeof(why));
   if (!zt) {
      printf("air_metal_test: rwa_texture_create (the stencil attachment): %s\n", why);
      return 1;
   }
   struct rwa_depth_stencil_state count_state = {
      .depth_compare_function = 7,              /* MTLCompareFunctionAlways */
      .depth_write_enabled = 0,
      .front = { .compare_function = 7,
                 .depth_stencil_pass_operation = 3,  /* MTLStencilOperationIncrementClamp */
                 .read_mask = 0xffffffff, .write_mask = 0xffffffff },
   };
   count_state.back = count_state.front;
   struct rwa_depth_stencil *counter = rwa_depth_stencil_create(device, &count_state, why,
                                                                sizeof(why));
   if (!counter) {
      printf("air_metal_test: rwa_depth_stencil_create (the counter): %s\n", why);
      return 1;
   }
   static const struct { const char *what; bool indexed; unsigned instances, base; } runs[] = {
      { "-drawPrimitives: with 1 instance",                        false, 1, 0 },
      { "-drawPrimitives: with 3 instances",                       false, 3, 0 },
      { "-drawIndexedPrimitives: with 1 instance",                 true,  1, 0 },
      { "-drawIndexedPrimitives: with 3 instances",                true,  3, 0 },
      { "-drawIndexedPrimitives: with 3 and a baseInstance of 5",  true,  3, 5 },
   };
   printf("air_metal_test: the instance count, counted in the stencil (Always + "
          "IncrementClamp over a clear of 0):\n");
   for (unsigned i = 0; i < COUNT_OF(runs); i++) {
      const unsigned clear_stencil = 0;
      uint8_t stencil[TEX * TEX];
      const unsigned origin[3] = { 0, 0, 0 };
      const unsigned extent[3] = { TEX, TEX, 1 };
      struct rwa_draw id = base;

      id.clear_color = (const float[]){ 0, 0, 0, 0 };
      id.depth = zt;
      id.clear_stencil = &clear_stencil;
      id.depth_stencil = counter;
      id.vertex_count = 4;
      id.instance_count = runs[i].instances;
      id.base_instance = runs[i].base;
      if (runs[i].indexed)
         id.index_buffer = i16;
      if (rwa_render_draw(counted, &id, why, sizeof(why)) != 0) {
         printf("air_metal_test:   %s: rwa_render_draw: %s\n", runs[i].what, why);
         ok = false;
         continue;
      }
      if (rwa_depth_read(zt, 0, 0, origin, extent, NULL, stencil, why, sizeof(why)) != 0) {
         printf("air_metal_test:   %s: rwa_depth_read: %s\n", runs[i].what, why);
         return 1;
      }
      bool uniform = true;
      for (unsigned k = 1; k < TEX * TEX; k++)
         uniform = uniform && stencil[k] == stencil[0];
      printf("air_metal_test:   %s: the stencil holds %u%s, expecting %u\n", runs[i].what,
             stencil[0], uniform ? " in all 16 pixels" : " at (0,0) and NOT everywhere",
             runs[i].instances);
      if (!uniform || stencil[0] != runs[i].instances)
         ok = false;
   }
   printf("air_metal_test: a baseInstance cannot be measured here and is not claimed: the "
          "generated vertex stage reads no [[instance_id]], and none of the corpus's three "
          "vertex functions declares one either\n");

   /*
    * And the same permutation over a SUPPLIED vertex stage, which is the path an
    * application is on: clearVertex is `position = vtx[vertex_id]`, so here an index is
    * not a row of a vertex attribute but the [[vertex_id]] the shader indexes its own
    * buffer with. Nothing about the geometry changes, and that is the point -- the two
    * halves must come back exactly as the generated stage drew them.
    */
   if (vfunc) {
      struct rwa_render_info vinfo = { 0 };
      struct rwa_render *supplied =
         rwa_render_create(device, vbitcode, vbitcode_size, vfunc, bitcode, bitcode_size,
                           func, &shape, &vinfo, why, sizeof(why));
      if (!supplied) {
         printf("air_metal_test: rwa_render_create (%s over %s): %s\n", vfunc, func, why);
         ok = false;
      } else {
         struct rwa_buffer *vbuffers[16] = { 0 };
         unsigned nvbuf = vinfo.vertex_buffers < COUNT_OF(vbuffers)
                             ? vinfo.vertex_buffers : (unsigned)COUNT_OF(vbuffers);
         struct index_picture vhalves[2], vbias;
         struct index_picture *vhalf[2] = { &vhalves[0], &vhalves[1] };

         for (unsigned i = 0; i < nvbuf; i++) {
            vbuffers[i] = rwa_buffer_create(device, sizeof(draw_quad_pos), why,
                                            sizeof(why));
            if (!vbuffers[i]) {
               printf("air_metal_test: rwa_buffer_create (vertex buffer %u): %s\n", i,
                      why);
               return 1;
            }
            fill_vertex_buffer(rwa_buffer_map(vbuffers[i]), 0, 1.0f);
         }
         printf("air_metal_test: and over the supplied vertex stage %s, whose %u buffer%s "
                "hold the four corners:\n", vname, nvbuf, nvbuf == 1 ? "" : "s");
         for (unsigned t = 0; t < 2; t++) {
            struct rwa_draw vd = base;

            vd.vertex.buffers = vbuffers;
            vd.vertex.buffer_count = nvbuf;
            vd.vertex_count = 3;
            vd.index_buffer = i16;
            vd.index_buffer_offset = tri[t].at16;
            if (!index_draw(supplied, &vd, color, color_count, vhalf[t], tri[t].what))
               return 1;
            unsigned diff = index_picture_diff(vhalf[t], half[t]);
            printf("air_metal_test:   %s: %u of %u words differ from the generated "
                   "stage's\n", tri[t].what, diff, TEX * TEX * 4);
            index_print(vhalf[t]);
            if (diff)
               ok = false;
         }
         struct rwa_draw vd = base;

         vd.vertex.buffers = vbuffers;
         vd.vertex.buffer_count = nvbuf;
         vd.vertex_count = 3;
         vd.index_buffer = i16;
         vd.base_vertex = 1;
         if (!index_draw(supplied, &vd, color, color_count, &vbias,
                         "indices [0 1 2] with baseVertex 1")) {
            return 1;
         } else {
            unsigned diff = index_picture_diff(&vbias, vhalf[1]);
            printf("air_metal_test:   baseVertex 1 over [0 1 2]: %u of %u words differ "
                   "from [1 2 3], so [[vertex_id]] %s\n", diff, TEX * TEX * 4,
                   diff ? "did NOT take the base vertex"
                        : "is the index plus the base vertex");
            if (diff)
               ok = false;
         }

         /*
          * And the canary, which this mode needs for the reason --draw-vertex needed
          * one: the three draws above agreed with the generated stage TO THE BIT, and
          * that is also exactly what a run which quietly drew the generated quad anyway
          * would have printed. So the supplied stage's buffers are refilled with the
          * same corners at half the distance from the origin and [0 1 2] drawn once
          * more: a quad of half the size covers only the middle of a 4x4 attachment, so
          * the pixel at (0, 0) must now be CLEAR. If it still came back drawn, nothing
          * above is evidence about the supplied stage -- and the index buffer would have
          * been measured through the generated quad twice.
          */
         for (unsigned i = 0; i < nvbuf; i++)
            fill_vertex_buffer(rwa_buffer_map(vbuffers[i]), 0, 0.5f);
         struct index_picture small;
         struct rwa_draw sd = base;

         sd.vertex.buffers = vbuffers;
         sd.vertex.buffer_count = nvbuf;
         sd.vertex_count = 3;
         sd.index_buffer = i16;
         if (!index_draw(supplied, &sd, color, color_count, &small,
                         "the canary: [0 1 2] over corners at half the distance"))
            return 1;
         printf("air_metal_test:   the canary, the same indices over corners at half the "
                "distance: %u of %u words differ, and (0,0) is %s\n",
                index_picture_diff(&small, vhalf[0]), TEX * TEX * 4,
                index_covered(&small, 0, 0)
                   ? "STILL DRAWN, so the supplied vertex stage is not what put the "
                     "triangle there and nothing above is evidence about it"
                   : "clear, so the vertices came from the shader's own buffer");
         index_print(&small);
         if (index_covered(&small, 0, 0) || index_picture_diff(&small, vhalf[0]) == 0)
            ok = false;

         for (unsigned i = 0; i < nvbuf; i++)
            rwa_buffer_destroy(vbuffers[i]);
         rwa_render_destroy(supplied);
      }
   }

   printf("air_metal_test: %s\n",
          ok ? "INDEXED DRAWING READS THE INDICES -- the permutation, the offset, the "
               "index type, the base vertex and the instance count each changed what came "
               "back"
             : "SOMETHING ABOVE DISAGREES, and an indexed draw that is nearly right draws "
               "a picture of the wrong geometry");

   rwa_depth_stencil_destroy(counter);
   rwa_texture_destroy(zt);
   rwa_buffer_destroy(i16);
   rwa_buffer_destroy(i32);
   rwa_buffer_destroy(past);
   for (unsigned i = 0; i < nbuf; i++)
      rwa_buffer_destroy(buffers[i]);
   rwa_render_destroy(plain);
   rwa_render_destroy(counted);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_device_close(device);
   return ok ? 0 : 1;
}

/*
 * --draw-attrib: whether the vertex attribute layout is read, and read as Metal says.
 *
 * This is the half of the contract the phone's window server asked for and did not get:
 * 120 of its render pipelines were refused because rwa_render_shape carried no formats,
 * strides or offsets (docs/metal.md 10.44). What is hard to measure about a layout is
 * not getting a picture -- a quad drawn out of a float4 at offset 0 of a 16-byte element
 * is the same quad the library's own generated stage draws, so on its own it proves
 * nothing. What makes this a measurement is that EVERY NUMBER IN THE LAYOUT IS WRONG FOR
 * THE DATA unless it is honoured:
 *
 *   - the position is NOT at offset 0 and the element is NOT the size of the position, so
 *     a fetch that ignored the offset or the stride reads a texture coordinate as a
 *     clip-space position and draws somewhere else;
 *   - the stream is at BUFFER INDEX 5, so a fetch that bound it at slot 0 reads nothing;
 *   - the second measured attribute is Metal's [[attribute(3)]] while its NIR
 *     driver_location is 1, because the module's attribute 1 is air.arg_unused -- and
 *     this mode CONFIGURES that unused attribute with a layout of its own, pointing at
 *     bytes that hold a different texture coordinate. An element array built by
 *     attribute index, or built over all three attributes in order, feeds attribute 1's
 *     layout to attribute 3's fetch, and then the picture is flat where it should be a
 *     gradient. That is the one thing in docs/air.md 3.1 that cannot be re-derived, and
 *     it is the only wrong answer here that still draws the right geometry;
 *   - the same four vertices are then drawn a second time with the SAME numbers in
 *     different formats -- UChar4Normalized and Short4Normalized where the first draw
 *     had Float4 -- at a different stride and different offsets. Those two pictures must
 *     agree to the bit, which no implementation that ignores a format can manage: 0 and
 *     255 read as floats are 0.0 and 3.57e-43.
 *
 * The pipeline is Apple's own code on both sides and from two libraries, which is a
 * legal Metal pipeline and the shape a real one has: QuartzCore's std_vert0_lpf, whose
 * position is `mvp_matrix * attribute(0)` (so an IDENTITY matrix in its [[buffer(2)]]
 * makes the position the attribute verbatim) and whose [[attribute(3)]] `color` becomes
 * the first varying, over AppleMetalGLRenderer's texturedFragment2DUInt, which samples
 * texture 0 at that varying and writes the texel to four RGBA32Uint targets. So the
 * COVERAGE of the picture measures attribute 0 and every WORD in it measures attribute
 * 3: pixel (x, y) must come back as the texel the interpolated coordinate lands in,
 * (100 + x, 200 + y, 300 + x + y, 400) up to the vertical orientation of 10.32.
 *
 * The texture coordinate is the only thing this mode puts in a buffer that it does not
 * know the meaning of: mvp_matrix is a float4x4 by Apple's own air.arg_type_name and the
 * identity is the identity in either column order, and texmat (its [[buffer(3)]]) is
 * air.arg_unused in this module and bound only because the module declares it.
 *
 *   air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION FRAGMENTLIB FRAGMENTFUNCTION
 *                  [GATEDVERTEXFUNCTION]
 *
 * Both paths may be a .metallib or a bare module, as everywhere else in this program. The
 * fifth argument is a second vertex function of the same library whose attributes are
 * gated by function constants (QuartzCore's fixed_vert_lph_spc), for the step that
 * measures the gate; without it that step says it was not run.
 */

/* The eight elements of the stream: the four corners of the full-screen quad with a
 * texture coordinate that runs the whole [0, 1] square, and then four more at half the
 * distance from the origin whose coordinate is the far corner. Elements 4..7 exist for
 * one step only -- the one that draws the same data at twice the stride, where the
 * fetch then walks 0, 2, 4, 6 and must produce a picture nothing else here produces.
 */
static const float attrib_pos[8][4] = {
   { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, 1, 0, 1 },
   { -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 },
   { -0.5f, 0.5f, 0, 1 }, { 0.5f, 0.5f, 0, 1 },
};
static const float attrib_uv[8][2] = {
   { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 },
   { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 },
};

/* The DECOY: what the module's unused [[attribute(1)]] points at, in every element. A
 * coordinate at the centre of texel (0, 0), so an implementation that fed this layout to
 * the fetch of attribute 3 comes back with (100, 200, 300, 400) in all sixteen pixels --
 * one number, nameable in advance, and not a plausible gradient.
 */
static const float attrib_decoy_uv[2] = { 0.125f, 0.125f };

/* One attribute's place in the element, as MTLVertexAttributeDescriptor has it. */
struct attrib_slot {
   unsigned format;   /* MTLVertexFormat */
   unsigned offset;
   unsigned buffer;
};

/* One shape of the stream: where each of the three attributes sits and how far apart two
 * vertices are. The two that matter are `wide` and `narrow` -- the same numbers in two
 * encodings at two strides with three different offsets -- and every other step is one
 * of those two with one field changed.
 */
struct attrib_layout {
   const char *what;
   unsigned stride;
   struct attrib_slot pos, uv, decoy;
};

static int16_t
attrib_snorm16(float v)
{
   /* Metal's Short4Normalized: value / 32767, so 1.0 is 32767 exactly and the quad's
    * corners are exact. The halves are not, and nothing here compares them.
    */
   float s = v * 32767.0f;
   return (int16_t)(s < 0 ? s - 0.5f : s + 0.5f);
}

static uint8_t
attrib_unorm8(float v)
{
   return (uint8_t)(v * 255.0f + 0.5f);   /* UChar4Normalized: 255 is 1.0 exactly */
}

/* The stream, written in whatever encoding the layout names. The buffer is always eight
 * elements of the layout's own stride, so a step that reads it at twice the stride has
 * something defined to read rather than running off the end.
 */
static void
attrib_fill(uint8_t *mem, size_t size, const struct attrib_layout *l)
{
   memset(mem, 0, size);
   for (unsigned i = 0; i < 8; i++) {
      uint8_t *e = mem + (size_t)i * l->stride;

      if (l->pos.format == MTL_VF_FLOAT4) {
         float v[4] = { attrib_pos[i][0], attrib_pos[i][1], attrib_pos[i][2],
                        attrib_pos[i][3] };
         memcpy(e + l->pos.offset, v, sizeof(v));
      } else {
         int16_t v[4] = { attrib_snorm16(attrib_pos[i][0]),
                          attrib_snorm16(attrib_pos[i][1]),
                          attrib_snorm16(attrib_pos[i][2]),
                          attrib_snorm16(attrib_pos[i][3]) };
         memcpy(e + l->pos.offset, v, sizeof(v));
      }
      if (l->uv.format == MTL_VF_FLOAT4) {
         float v[4] = { attrib_uv[i][0], attrib_uv[i][1], 0.0f, 1.0f };
         memcpy(e + l->uv.offset, v, sizeof(v));
      } else if (l->uv.format == MTL_VF_UCHAR4_NORMALIZED_BGRA) {
         /* The one place here where the bytes are NOT in the order the components are:
          * MTLVertexFormatUChar4Normalized_BGRA delivers (x, y, z, w) as
          * (byte2, byte1, byte0, byte3), so the coordinate goes in bytes 2 and 1 and byte 0
          * is ZERO -- read in memory order the shader's x would be that zero, and every
          * pixel would sample column 0 of the texture.
          */
         uint8_t v[4] = { 0, attrib_unorm8(attrib_uv[i][1]),
                          attrib_unorm8(attrib_uv[i][0]), 255 };
         memcpy(e + l->uv.offset, v, sizeof(v));
      } else {
         uint8_t v[4] = { attrib_unorm8(attrib_uv[i][0]), attrib_unorm8(attrib_uv[i][1]),
                          0, 255 };
         memcpy(e + l->uv.offset, v, sizeof(v));
      }
      if (l->decoy.format == MTL_VF_FLOAT2) {
         float v[2] = { attrib_decoy_uv[0], attrib_decoy_uv[1] };
         memcpy(e + l->decoy.offset, v, sizeof(v));
      } else {
         /* 0.125 as a half, written out because this program includes air_metal.h and
          * nothing of Mesa: 2^-3 is exponent field 15 - 3 = 12 with a zero mantissa,
          * which is 0x3000.
          */
         uint16_t v[2] = { 0x3000, 0x3000 };
         memcpy(e + l->decoy.offset, v, sizeof(v));
      }
   }
}

/* The per-instance stream of the instancing steps: four texture coordinates, one per
 * element, in the two corners and the two opposite ones. A stream that steps per
 * instance hands ALL FOUR VERTICES of one instance the same element, so the varying is
 * constant over the quad and every pixel samples one texel; a stream that stepped per
 * vertex instead would hand the four vertices four different coordinates and the picture
 * would be a gradient. One flat number against a gradient is the whole measurement.
 */
static const float attrib_instance_uv[4][2] = {
   { 0.125f, 0.125f }, { 0.875f, 0.875f }, { 0.125f, 0.875f }, { 0.875f, 0.125f },
};

/* How the shape's vertex descriptor is filled in from a layout: Metal's own two arrays,
 * indexed by Metal's own indices. Attribute 1 is configured although the module marks it
 * air.arg_unused, which is legal and is this mode's decoy.
 */
static void
attrib_configure(struct rwa_render_shape *shape, const struct attrib_layout *l,
                 unsigned step_function, unsigned step_rate, unsigned instance_buffer)
{
   memset(shape->vertex_attribute, 0, sizeof(shape->vertex_attribute));
   memset(shape->vertex_layout, 0, sizeof(shape->vertex_layout));

   shape->vertex_attribute[0] = (struct rwa_vertex_attribute){
      .format = l->pos.format, .offset = l->pos.offset, .buffer_index = l->pos.buffer };
   shape->vertex_attribute[1] = (struct rwa_vertex_attribute){
      .format = l->decoy.format, .offset = l->decoy.offset,
      .buffer_index = l->decoy.buffer };
   shape->vertex_attribute[3] = (struct rwa_vertex_attribute){
      .format = l->uv.format, .offset = l->uv.offset, .buffer_index = l->uv.buffer };

   shape->vertex_layout[l->pos.buffer] = (struct rwa_vertex_layout){
      .stride = l->stride, .step_function = MTL_STEP_PER_VERTEX, .step_rate = 1 };
   if (instance_buffer != ~0u) {
      shape->vertex_layout[instance_buffer] = (struct rwa_vertex_layout){
         .stride = 8, .step_function = step_function, .step_rate = step_rate };
   }
}

/* The four words of one pixel, and what the texel the coordinate should have landed in
 * holds. 10.32 established that the picture comes back mirrored vertically against a
 * naive reading of the coordinate, so BOTH orientations are counted and the one that
 * matched is printed rather than one of them being assumed: what this mode is about is
 * which attribute fed the fetch, and that question has the same answer either way.
 */
struct attrib_verdict {
   unsigned x_gradient;    /* pixels whose first word is 100 + x */
   unsigned mirrored;      /* ... and whose second is 200 + (3 - y) */
   unsigned upright;       /* ... or 200 + y */
   unsigned decoy_texel;   /* pixels holding texel (0, 0) exactly: the decoy's answer */
   unsigned column0;       /* pixels holding texel (0, y): what a dropped BGRA swap gives */
   unsigned uniform;       /* pixels equal to the pixel at (0, 0) */
};

static struct attrib_verdict
attrib_judge(const struct index_picture *p)
{
   struct attrib_verdict v = { 0 };
   float t[4];

   texel_value(0, 0, 0, 0, t);
   for (unsigned y = 0; y < TEX; y++)
      for (unsigned x = 0; x < TEX; x++) {
         const uint32_t *w = p->word[y * TEX + x];

         v.x_gradient += w[0] == 100 + x;
         v.mirrored += w[0] == 100 + x && w[1] == 200 + (TEX - 1 - y);
         v.upright += w[0] == 100 + x && w[1] == 200 + y;
         v.decoy_texel += w[0] == (uint32_t)t[0] && w[1] == (uint32_t)t[1] &&
                          w[2] == (uint32_t)t[2] && w[3] == (uint32_t)t[3];
         v.column0 += w[0] == 100;
         v.uniform += w[0] == p->word[0][0] && w[1] == p->word[0][1] &&
                      w[2] == p->word[0][2] && w[3] == p->word[0][3];
      }
   return v;
}

static void
attrib_print(const struct index_picture *p, const struct attrib_verdict *v)
{
   index_print(p);
   printf("air_metal_test:   the pixel at (0,0) is %u %u %u %u and at (3,3) %u %u %u %u; "
          "%u of 16 pixels hold texel x, %u hold (x, 3-y), %u hold (x, y), %u hold texel "
          "(0,0), %u hold column 0, %u are the same as (0,0)\n",
          p->word[0][0], p->word[0][1], p->word[0][2], p->word[0][3],
          p->word[TEX * TEX - 1][0], p->word[TEX * TEX - 1][1],
          p->word[TEX * TEX - 1][2], p->word[TEX * TEX - 1][3],
          v->x_gradient, v->mirrored, v->upright, v->decoy_texel, v->column0,
          v->uniform);
}

static int
run_attrib(const char *vpath, const char *vfunc, const char *fpath, const char *ffunc,
           const char *gated)
{
   struct metallib vlib = { 0 }, flib = { 0 };
   char vname[256], fname[256];
   const uint8_t *vbitcode = NULL, *fbitcode = NULL;
   size_t vbitcode_size = 0, fbitcode_size = 0;

   if (!load_metallib(vpath, &vlib) || !load_metallib(fpath, &flib))
      return 1;
   if (!find_function(&vlib, vfunc, vname, sizeof(vname), &vbitcode, &vbitcode_size))
      return 1;
   if (!find_function(&flib, ffunc, fname, sizeof(fname), &fbitcode, &fbitcode_size))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --draw-attrib %s over %s on \"%s\"\n", vname, fname,
          rwa_device_name(device));

   /* Four RGBA32Uint attachments, because texturedFragment2DUInt writes four, and 4x4
    * as everywhere else in this program: a pixel here and a pixel in air_run are the
    * same measurement.
    */
   const unsigned color_count = 4;
   struct rwa_texture *color[4] = { 0 };
   for (unsigned i = 0; i < color_count; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = MTL_RGBA32_UINT, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1,
         .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
      };
      color[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!color[i]) {
         printf("air_metal_test: rwa_texture_create (attachment %u): %s\n", i, why);
         return 1;
      }
   }

   /* The two shapes of the stream. Everything about them differs except the numbers they
    * encode: the stride, all three offsets and all three formats.
    */
   static const struct attrib_layout wide = {
      "Float4 at offset 16 of a 48-byte element",
      48,
      { MTL_VF_FLOAT4, 16, 5 },   /* the position */
      { MTL_VF_FLOAT4, 0, 5 },    /* attribute 3, the texture coordinate */
      { MTL_VF_FLOAT2, 32, 5 },   /* attribute 1, unused by the module: the decoy */
   };
   static const struct attrib_layout narrow = {
      "Short4Normalized at offset 4 of a 16-byte element",
      16,
      { MTL_VF_SHORT4_NORMALIZED, 4, 5 },
      { MTL_VF_UCHAR4_NORMALIZED, 0, 5 },
      { MTL_VF_HALF2, 12, 5 },
   };

   /* One buffer per shape, eight elements each, and one more for the instanced steps. */
   struct rwa_buffer *wide_stream = rwa_buffer_create(device, 8 * wide.stride, why,
                                                     sizeof(why));
   struct rwa_buffer *narrow_stream = rwa_buffer_create(device, 8 * narrow.stride, why,
                                                        sizeof(why));
   struct rwa_buffer *instance_stream = rwa_buffer_create(device,
                                                          sizeof(attrib_instance_uv), why,
                                                          sizeof(why));
   if (!wide_stream || !narrow_stream || !instance_stream) {
      printf("air_metal_test: rwa_buffer_create (a vertex stream): %s\n", why);
      return 1;
   }
   attrib_fill(rwa_buffer_map(wide_stream), rwa_buffer_size(wide_stream), &wide);
   attrib_fill(rwa_buffer_map(narrow_stream), rwa_buffer_size(narrow_stream), &narrow);
   memcpy(rwa_buffer_map(instance_stream), attrib_instance_uv,
          sizeof(attrib_instance_uv));
   printf("air_metal_test: the stream is 8 elements: four corners of the quad with the "
          "texture coordinate running the whole [0,1] square, then four at half the "
          "distance with the coordinate (1,1). The unused attribute 1 points at "
          "(%.3f, %.3f), the centre of texel (0,0)\n",
          (double)attrib_decoy_uv[0], (double)attrib_decoy_uv[1]);

   /* The index buffer of the indexed steps: the strip of four, then its last three, as
    * run_index's own permutation -- the difference being that here an index selects the
    * ELEMENT THE FETCH READS and not a vertex id the shader indexes a buffer with.
    */
   static const uint16_t attrib_indices[7] = { 0, 1, 2, 3, 1, 2, 3 };
   struct rwa_buffer *indices = rwa_buffer_create(device, sizeof(attrib_indices), why,
                                                  sizeof(why));
   if (!indices) {
      printf("air_metal_test: rwa_buffer_create (the index buffer): %s\n", why);
      return 1;
   }
   memcpy(rwa_buffer_map(indices), attrib_indices, sizeof(attrib_indices));

   struct rwa_render_shape shape = { .color_count = color_count, .sample_count = 1 };
   for (unsigned i = 0; i < color_count; i++)
      shape.color_format[i] = MTL_RGBA32_UINT;

   /* The first pipeline, and the first thing it says is what the layout became. */
   struct rwa_render_shape wide_shape = shape;
   attrib_configure(&wide_shape, &wide, MTL_STEP_PER_VERTEX, 1, ~0u);

   struct rwa_render_info info = { 0 };
   struct rwa_render *wide_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &wide_shape, &info, why, sizeof(why));
   if (!wide_pipe) {
      printf("air_metal_test: rwa_render_create: %s\n", why);
      return 1;
   }
   printf("air_metal_test: pipeline: %u vertex buffers, %u fragment textures, %u "
          "samplers; %u vertex elements, %u stream slots, mask 0x%x; elements are",
          info.vertex_buffers, info.fragment_textures, info.fragment_samplers,
          info.vertex_attribute_count, info.vertex_streams, info.vertex_stream_mask);
   for (unsigned i = 0; i < info.vertex_attribute_count; i++)
      printf(" [%u]=attribute(%u)", i, info.vertex_attribute_index[i]);
   printf("\n");

   /* This mode's arithmetic is this module's: two fetched attributes, Metal's 0 and 3,
    * and a third configured attribute the module does not read. A module of another
    * shape is refused here rather than measured against the wrong expectation.
    */
   if (info.vertex_attribute_count != 2 || info.vertex_attribute_index[0] != 0 ||
       info.vertex_attribute_index[1] != 3) {
      printf("air_metal_test: this mode needs a vertex function of std_vert0_lpf's shape "
             "-- [[attribute(0)]] into the position, an unused [[attribute(1)]], and "
             "[[attribute(3)]] into the first varying -- and %s fetches %u attribute(s). "
             "Nothing below would be comparable with the expectations above\n", vname,
             info.vertex_attribute_count);
      return 2;
   }
   if (info.vertex_streams != 6 || info.vertex_stream_mask != (1u << 5)) {
      printf("air_metal_test: the layout puts both fetched attributes in buffer index 5, "
             "so six slots with only the sixth in the mask is the answer; this says %u "
             "and 0x%x\n", info.vertex_streams, info.vertex_stream_mask);
      return 2;
   }
   if (info.fragment_textures != 1 || info.fragment_samplers != 1) {
      printf("air_metal_test: this mode needs a fragment function that samples one "
             "texture with the first varying, as texturedFragment2DUInt does, and %s "
             "samples %u\n", fname, info.fragment_textures);
      return 2;
   }

   /* The vertex stage's own buffers: mvp_matrix at [[buffer(2)]] gets the IDENTITY, which
    * is what makes the position the attribute verbatim and the picture computable;
    * texmat at [[buffer(3)]] is air.arg_unused in this module and is bound because the
    * module declares it. Both are 64 bytes, the size Apple's metadata gives the matrix.
    */
   struct rwa_buffer *vbuffers[16] = { 0 };
   unsigned nvbuf = info.vertex_buffers < COUNT_OF(vbuffers) ? info.vertex_buffers
                                                             : (unsigned)COUNT_OF(vbuffers);
   for (unsigned i = 0; i < nvbuf; i++) {
      vbuffers[i] = rwa_buffer_create(device, 64, why, sizeof(why));
      if (!vbuffers[i]) {
         printf("air_metal_test: rwa_buffer_create (vertex buffer %u): %s\n", i, why);
         return 1;
      }
      float *f = rwa_buffer_map(vbuffers[i]);
      memset(f, 0, 64);
      for (unsigned k = 0; k < 4; k++)
         f[k * 4 + k] = 1.0f;         /* the identity, in either column order */
   }

   /* The texture and the sampler, air_run's texels and air_run's sampler state: nearest,
    * so the texel the coordinate falls in is the texel that comes back and there are no
    * weights to round.
    */
   struct rwa_texture *tex = NULL;
   {
      const struct rwa_texture_shape ts = {
         .pixel_format = MTL_RGBA32_UINT, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1, .usage = MTL_USAGE_SHADER_READ,
      };
      tex = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!tex) {
         printf("air_metal_test: rwa_texture_create (the sampled texture): %s\n", why);
         return 1;
      }
      static int32_t texels[TEXELS];
      memset(texels, 0, sizeof(texels));
      for (unsigned y = 0; y < TEX; y++)
         for (unsigned x = 0; x < TEX; x++) {
            float v[4];
            texel_value(0, x, y, 0, v);
            for (unsigned c = 0; c < 4; c++)
               texels[texel_index(x, y, 0) + c] = (int32_t)v[c];
         }
      const unsigned origin[3] = { 0, 0, 0 }, extent[3] = { TEX, TEX, 1 };
      if (rwa_texture_write(tex, 0, 0, origin, extent, texels, 0, why, sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write: %s\n", why);
         return 1;
      }
   }
   const struct rwa_sampler_state sstate = {
      .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
      .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 1,
      .lod_min = 0.0f, .lod_max = 65504.0f,
   };
   struct rwa_sampler *sampler = rwa_sampler_create(device, &sstate, why, sizeof(why));
   if (!sampler) {
      printf("air_metal_test: rwa_sampler_create: %s\n", why);
      return 1;
   }

   struct rwa_texture *textures[1] = { tex };
   struct rwa_sampler *samplers[1] = { sampler };
   struct rwa_vertex_stream streams[8] = { 0 };
   const struct rwa_draw base = {
      .color = color, .color_count = color_count,
      .vertex = { .buffers = vbuffers, .buffer_count = nvbuf },
      .fragment = { .textures = textures, .texture_count = 1,
                    .samplers = samplers, .sampler_count = 1 },
      .primitive = 4,                /* MTLPrimitiveTypeTriangleStrip */
      .vertex_streams = streams, .vertex_stream_count = COUNT_OF(streams),
   };

   bool ok = true;
   struct index_picture pw, pn, pstride, pindex7, pfull, phalf, pinst, prate;
   struct rwa_draw d;

   /* 1. The wide layout. If the offsets, the stride and the buffer index are honoured,
    *    the quad covers the attachment and the coordinate runs the square: every pixel
    *    must hold the texel under it. If attribute 1's layout had been fed to attribute
    *    3's fetch instead -- which is what an element array built by attribute index
    *    gives -- all sixteen pixels would hold texel (0,0), (100, 200, 300, 400).
    */
   streams[5] = (struct rwa_vertex_stream){ .buffer = wide_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   if (!index_draw(wide_pipe, &d, color, color_count, &pw, wide.what))
      return 1;
   struct attrib_verdict vw = attrib_judge(&pw);
   printf("air_metal_test: 1. %s, stream at buffer index 5\n", wide.what);
   attrib_print(&pw, &vw);
   if (pw.covered != TEX * TEX || vw.x_gradient != TEX * TEX ||
       (vw.mirrored != TEX * TEX && vw.upright != TEX * TEX)) {
      printf("air_metal_test:   NOT the picture the layout describes%s\n",
             vw.decoy_texel == TEX * TEX
                ? ": every pixel holds texel (0,0), which is where the UNUSED "
                  "attribute 1 points -- the elements were not built in "
                  "driver_location order"
                : "");
      ok = false;
   } else {
      printf("air_metal_test:   the coordinate of [[attribute(3)]] reached every pixel "
             "and the quad of [[attribute(0)]] covered all 16: the offsets, the stride "
             "and buffer index 5 are READ, and element 1 is attribute 3 and not the "
             "unused attribute 1 (%s orientation, 10.32)\n",
             vw.mirrored == TEX * TEX ? "mirrored" : "upright");
   }

   /* 2. The same numbers in other formats at another stride and other offsets. Equal to
    *    the bit, or the formats are not what decoded the bytes: 0 and 255 read as a
    *    float are 0.0 and 3.57e-43, and a Short4Normalized position read as Float4 is
    *    four denormals -- a quad collapsed into the origin, covering nothing.
    */
   struct rwa_render_shape narrow_shape = shape;
   attrib_configure(&narrow_shape, &narrow, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render_info ninfo = { 0 };
   struct rwa_render *narrow_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &narrow_shape, &ninfo, why, sizeof(why));
   if (!narrow_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", narrow.what, why);
      return 1;
   }
   streams[5] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   if (!index_draw(narrow_pipe, &d, color, color_count, &pn, narrow.what))
      return 1;
   printf("air_metal_test: 2. %s, the same four vertices\n", narrow.what);
   {
      struct attrib_verdict v = attrib_judge(&pn);
      attrib_print(&pn, &v);
      unsigned diff = index_picture_diff(&pw, &pn);
      printf("air_metal_test:   %u of %u words differ from step 1: %s\n", diff,
             TEX * TEX * 4,
             diff == 0 ? "two encodings, two strides, three different offsets, ONE "
                         "PICTURE -- the formats decoded the bytes"
                       : "the same numbers in another format came back as another "
                         "picture");
      if (diff)
         ok = false;
   }

   /*
    * 2a. THE BGRA ORDER, which is a translation here and a hardware field on the phone:
    *     MTLVertexFormatUChar4Normalized_BGRA becomes PIPE_FORMAT_B8G8R8A8_UNORM, and
    *     nothing of the byte order is in the fetch's format -- freedreno puts it in
    *     VFD_DECODE_INSTR.swap, which fd6_vertex_swap() reads off the pipe_format, while
    *     softpipe's own fetch reads the format description. So what this step settles is
    *     that the TRANSLATION is right, and it is the reference the same step on the device
    *     is compared with (metal_drv_test --draw-attrib, step 3).
    *
    *     The coordinate is stored with its first byte ZERO and the two components in bytes
    *     2 and 1, which the BGRA order delivers as x and y. Read in memory order instead,
    *     the shader's x is that zero and every pixel samples column 0 of the texture.
    */
   struct attrib_layout bgra = narrow;
   bgra.what = "the coordinate as UChar4Normalized_BGRA";
   bgra.uv.format = MTL_VF_UCHAR4_NORMALIZED_BGRA;
   struct rwa_buffer *bgra_stream = rwa_buffer_create(device, 8 * bgra.stride, why,
                                                      sizeof(why));
   if (!bgra_stream) {
      printf("air_metal_test: rwa_buffer_create (the BGRA stream): %s\n", why);
      return 1;
   }
   attrib_fill(rwa_buffer_map(bgra_stream), rwa_buffer_size(bgra_stream), &bgra);
   struct rwa_render_shape bgra_shape = shape;
   attrib_configure(&bgra_shape, &bgra, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render *bgra_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &bgra_shape, NULL, why, sizeof(why));
   if (!bgra_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", bgra.what, why);
      return 1;
   }
   {
      struct index_picture pbgra;

      streams[5] = (struct rwa_vertex_stream){ .buffer = bgra_stream, .offset = 0 };
      d = base;
      d.vertex_count = 4;
      if (!index_draw(bgra_pipe, &d, color, color_count, &pbgra, bgra.what))
         return 1;
      struct attrib_verdict v = attrib_judge(&pbgra);
      printf("air_metal_test: 2a. %s, whose first byte is zero\n", bgra.what);
      attrib_print(&pbgra, &v);
      unsigned diff = index_picture_diff(&pn, &pbgra);
      printf("air_metal_test:   %u of %u words differ from step 2: %s\n", diff,
             TEX * TEX * 4,
             diff == 0 ? "the BGRA order is honoured -- bytes 2 and 1 arrived as x and y"
             : v.column0 == TEX * TEX
                  ? "every pixel holds column 0, which is what reading the bytes in memory "
                    "order gives: the order was NOT applied"
                  : "neither picture, which is a third answer and wants reading");
      if (diff)
         ok = false;
      streams[5] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };
   }

   /* 3. The stride, with a picture that nothing else here produces: the narrow layout
    *    read at 32 bytes instead of 16, over a buffer that holds eight elements. The
    *    fetch then walks elements 0, 2, 4 and 6 -- two corners of the full quad and two
    *    of the half-size one -- so the coverage must differ from step 2 and the pixels
    *    it does cover must hold the far texel the half-size elements carry.
    */
   struct attrib_layout stride_layout = narrow;
   stride_layout.what = "the narrow layout at twice its stride";
   stride_layout.stride = 32;
   struct rwa_render_shape stride_shape = shape;
   attrib_configure(&stride_shape, &stride_layout, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render *stride_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &stride_shape, NULL, why, sizeof(why));
   if (!stride_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", stride_layout.what, why);
      return 1;
   }
   d = base;
   d.vertex_count = 4;
   if (!index_draw(stride_pipe, &d, color, color_count, &pstride, stride_layout.what))
      return 1;
   printf("air_metal_test: 3. %s: the fetch walks elements 0, 2, 4, 6\n",
          stride_layout.what);
   {
      struct attrib_verdict v = attrib_judge(&pstride);
      attrib_print(&pstride, &v);
      unsigned diff = index_picture_diff(&pn, &pstride);
      printf("air_metal_test:   %u of %u words differ from step 2: %s\n", diff,
             TEX * TEX * 4,
             diff ? "the stride decides which element a vertex reads"
                  : "A STRIDE OF 32 DREW WHAT A STRIDE OF 16 DREW, so the stride is not "
                    "read");
      if (!diff)
         ok = false;
   }

   /* 4. The buffer index, from the other side: the same narrow layout with both
    *    attributes fetched from index 7, and the stream bound at 7 and nowhere else. An
    *    implementation that bound the stream at slot 0 and ignored Metal's index would
    *    fetch from an empty slot here and draw nothing.
    */
   struct attrib_layout seven = narrow;
   seven.what = "the narrow layout fetched from buffer index 7";
   seven.pos.buffer = seven.uv.buffer = seven.decoy.buffer = 7;
   struct rwa_render_shape seven_shape = shape;
   attrib_configure(&seven_shape, &seven, MTL_STEP_PER_VERTEX, 1, ~0u);
   struct rwa_render_info sinfo = { 0 };
   struct rwa_render *seven_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &seven_shape, &sinfo, why, sizeof(why));
   if (!seven_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", seven.what, why);
      return 1;
   }
   streams[5] = (struct rwa_vertex_stream){ 0 };
   streams[7] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   if (!index_draw(seven_pipe, &d, color, color_count, &pindex7, seven.what))
      return 1;
   printf("air_metal_test: 4. %s (%u slots, mask 0x%x)\n", seven.what,
          sinfo.vertex_streams, sinfo.vertex_stream_mask);
   {
      unsigned diff = index_picture_diff(&pn, &pindex7);
      printf("air_metal_test:   %u of %u words differ from step 2, and it covered %u "
             "pixels: %s\n", diff, TEX * TEX * 4, pindex7.covered,
             diff == 0 ? "the same picture out of another slot -- Metal's buffer index "
                         "is carried all the way down"
                       : "the index moved the picture, which it must not");
      if (diff)
         ok = false;
   }
   streams[7] = (struct rwa_vertex_stream){ 0 };
   streams[5] = (struct rwa_vertex_stream){ .buffer = narrow_stream, .offset = 0 };

   /* 5 and 6. The indexed forms over a real layout, which is what every draw in a real
    *          application is: here an index is the ELEMENT THE FETCH READS, where in
    *          10.35 it was the [[vertex_id]] a shader indexed its own buffer with. Four
    *          indices must give step 2's picture and the last three must give the half
    *          of it above the diagonal.
    */
   d = base;
   d.vertex_count = 4;
   d.index_buffer = indices;
   if (!index_draw(narrow_pipe, &d, color, color_count, &pfull, "indices [0 1 2 3]"))
      return 1;
   printf("air_metal_test: 5. -drawIndexedPrimitives: 4 indices [0 1 2 3] over the "
          "narrow layout\n");
   {
      unsigned diff = index_picture_diff(&pn, &pfull);
      printf("air_metal_test:   %u of %u words differ from step 2: %s\n", diff,
             TEX * TEX * 4,
             diff ? "an index fed the fetch the wrong element"
                  : "the index buffer selects the element the fetch reads");
      if (diff)
         ok = false;
   }
   d = base;
   d.vertex_count = 3;
   d.index_buffer = indices;
   d.index_buffer_offset = 4 * 2;
   if (!index_draw(narrow_pipe, &d, color, color_count, &phalf, "indices [1 2 3]"))
      return 1;
   printf("air_metal_test: 6. the same buffer's last three indices [1 2 3] (UInt16 at "
          "byte 8)\n");
   {
      struct attrib_verdict v = attrib_judge(&phalf);
      attrib_print(&phalf, &v);
      printf("air_metal_test:   %u of %u words differ from step 5, covering %u pixels of "
             "16: %s\n", index_picture_diff(&pfull, &phalf), TEX * TEX * 4, phalf.covered,
             phalf.covered < TEX * TEX && phalf.covered > 0
                ? "one triangle of the quad out of three of its four elements"
                : "NOT one triangle, which is what an offset that was not applied or an "
                  "index that was not read would give");
      if (phalf.covered == 0 || phalf.covered == TEX * TEX)
         ok = false;
   }

   /* 7. MTLVertexStepFunctionPerInstance, which cannot be seen in a colour attachment
    *    the way 10.35 saw the instance COUNT (there, in a stencil buffer): two instances
    *    of this geometry cover the same pixels. So the coordinate comes out of a SECOND
    *    stream that steps per instance, holding four coordinates -- and the picture says
    *    which way it stepped. Per instance: all four vertices of one instance share one
    *    coordinate, the varying is constant, every pixel holds ONE texel, and the second
    *    instance is drawn over the first, so that texel is element 1's: (0.875, 0.875)
    *    is texel (3, 3), which is 103 203 306 400. Per vertex: the four vertices get
    *    four different coordinates and the picture is a gradient of four texels.
    */
   struct attrib_layout split = narrow;
   split.what = "the coordinate from a second stream that steps per instance";
   split.uv.format = MTL_VF_FLOAT2;
   split.uv.offset = 0;
   split.uv.buffer = 6;
   struct rwa_render_shape inst_shape = shape;
   attrib_configure(&inst_shape, &split, MTL_STEP_PER_INSTANCE, 1, 6);
   struct rwa_render_info iinfo = { 0 };
   struct rwa_render *inst_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &inst_shape, &iinfo, why, sizeof(why));
   if (!inst_pipe) {
      printf("air_metal_test: rwa_render_create (%s): %s\n", split.what, why);
      return 1;
   }
   streams[6] = (struct rwa_vertex_stream){ .buffer = instance_stream, .offset = 0 };
   d = base;
   d.vertex_count = 4;
   d.instance_count = 2;
   if (!index_draw(inst_pipe, &d, color, color_count, &pinst, split.what))
      return 1;
   printf("air_metal_test: 7. %s, 2 instances (%u slots, mask 0x%x)\n", split.what,
          iinfo.vertex_streams, iinfo.vertex_stream_mask);
   {
      struct attrib_verdict v = attrib_judge(&pinst);
      float t[4];
      texel_value(0, TEX - 1, TEX - 1, 0, t);
      attrib_print(&pinst, &v);
      bool flat_far = v.uniform == TEX * TEX && pinst.word[0][0] == (uint32_t)t[0] &&
                      pinst.word[0][1] == (uint32_t)t[1];
      printf("air_metal_test:   expecting %u %u %u %u in all 16 pixels (texel (3,3), the "
             "second instance's element): %s\n", (unsigned)t[0], (unsigned)t[1],
             (unsigned)t[2], (unsigned)t[3],
             flat_far ? "the stream stepped PER INSTANCE"
                      : v.uniform != TEX * TEX
                           ? "the picture is a gradient, so that stream stepped per "
                             "VERTEX"
                           : "flat, but not that texel");
      if (!flat_far)
         ok = false;
   }

   /* 8. The step RATE, which is Gallium's instance_divisor: two instances at a rate of 2
    *    share element 0, so the picture must be the NEAR texel instead -- 100 200 300
    *    400 -- and a rate that was dropped would leave step 7's far texel.
    */
   struct rwa_render_shape rate_shape = shape;
   attrib_configure(&rate_shape, &split, MTL_STEP_PER_INSTANCE, 2, 6);
   struct rwa_render *rate_pipe =
      rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode, fbitcode_size,
                        ffunc, &rate_shape, NULL, why, sizeof(why));
   if (!rate_pipe) {
      printf("air_metal_test: rwa_render_create (a step rate of 2): %s\n", why);
      return 1;
   }
   d = base;
   d.vertex_count = 4;
   d.instance_count = 2;
   if (!index_draw(rate_pipe, &d, color, color_count, &prate, "a step rate of 2"))
      return 1;
   printf("air_metal_test: 8. the same, at a step rate of 2: both instances share "
          "element 0\n");
   {
      struct attrib_verdict v = attrib_judge(&prate);
      float t[4];
      texel_value(0, 0, 0, 0, t);
      attrib_print(&prate, &v);
      bool flat_near = v.uniform == TEX * TEX && prate.word[0][0] == (uint32_t)t[0] &&
                       prate.word[0][1] == (uint32_t)t[1];
      printf("air_metal_test:   expecting %u %u %u %u in all 16 pixels (texel (0,0)), "
             "and step 7's picture would mean the rate was dropped: %s\n",
             (unsigned)t[0], (unsigned)t[1], (unsigned)t[2], (unsigned)t[3],
             flat_near ? "the step rate IS the instance divisor"
                       : index_picture_diff(&pinst, &prate) == 0
                            ? "step 7's picture exactly, so the rate was dropped"
                            : "neither, which is a third answer and wants reading");
      if (!flat_near)
         ok = false;
   }

   /*
    * 9. THE FUNCTION-CONSTANT GATE, which nothing above touches and which is the one rule in
    *    docs/air.md 3.1 a runtime cannot derive from the module alone: an attribute the
    *    module reads only when an implicit predicate is true is FETCHED either way -- the
    *    parameter exists whatever the predicate says -- so its bit is in
    *    vertex_attributes_read, and a pipeline whose function constants make the predicate
    *    false may legally configure nothing at that index. QuartzCore's fixed_vert_lph_spc is
    *    the shape of it: five attributes, listed 0, 4, 1, 2, 3, with 4 (point_size), 1 and 2
    *    (texcoord0, texcoord1) gated and 0 (position) and 3 (color) not.
    *
    *    Two configurations, differing in one field, say whether the gate is honoured:
    *
    *      - position AND color configured, the three gated ones left out: the layout must be
    *        ACCEPTED. The three get a placeholder element each -- this library's own quad
    *        buffer at a stride of 0 -- because skipping them would shift every later
    *        driver_location by one.
    *      - position alone: the layout must be REFUSED, and the sentence must name COLOR. That
    *        is the whole measurement: color is the fifth attribute in AIR's own list and the
    *        first UNGATED one that is missing, so a runtime that demanded a binding for every
    *        bit of vertex_attributes_read would have stopped at point_size, the second.
    *
    *    "Accepted" cannot be read off a returned pipeline on this machine, and that is
    *    softpipe's limit rather than a gap in the measurement: every module of this shape
    *    dies inside softpipe -- the `_lph` ones on the 16-bit narrowing of a `half4`
    *    attribute (nir_to_tgsi ASSERTS on a 16-bit opcode instead of declining it), the
    *    `_lpf` ones on `store_scratch` -- and in both cases the assertion's own text is in
    *    the log above the verdict. So the attempt is made in a child process and the outcome
    *    read off its exit: a child that printed a refusal was stopped by the layout, and a
    *    child that DIED got past the layout and was stopped by the back end. The two are
    *    distinguishable, which is what makes this a comparison and not a hope.
    */
   if (gated) {
      struct metallib glib = { 0 };
      char gname[256];
      const uint8_t *gbitcode = NULL;
      size_t gbitcode_size = 0;
      bool gated_ok = true;

      if (!load_metallib(vpath, &glib) ||
          !find_function(&glib, gated, gname, sizeof(gname), &gbitcode, &gbitcode_size))
         return 1;
      printf("air_metal_test: 9. the function-constant gate, over %s\n", gname);

      static const struct { const char *what; bool color; } gcase[2] = {
         { "position and color configured, the gated three left out", true },
         { "position alone configured", false },
      };
      for (unsigned c = 0; c < 2; c++) {
         struct rwa_render_shape gs = shape;

         memset(gs.vertex_attribute, 0, sizeof(gs.vertex_attribute));
         memset(gs.vertex_layout, 0, sizeof(gs.vertex_layout));
         gs.vertex_attribute[0] = (struct rwa_vertex_attribute){
            .format = MTL_VF_FLOAT4, .offset = 0, .buffer_index = 0 };
         if (gcase[c].color)
            gs.vertex_attribute[3] = (struct rwa_vertex_attribute){
               .format = MTL_VF_UCHAR4_NORMALIZED, .offset = 16, .buffer_index = 0 };
         gs.vertex_layout[0] = (struct rwa_vertex_layout){
            .stride = 32, .step_function = MTL_STEP_PER_VERTEX, .step_rate = 1 };

         fflush(stdout);
         pid_t pid = fork();
         if (pid < 0) {
            printf("air_metal_test:   fork: %s\n", strerror(errno));
            return 1;
         }
         if (pid == 0) {
            struct rwa_render_info gi = { 0 };

            why[0] = '\0';
            struct rwa_render *r =
               rwa_render_create(device, gbitcode, gbitcode_size, gated, fbitcode,
                                 fbitcode_size, ffunc, &gs, &gi, why, sizeof(why));
            if (r) {
               printf("air_metal_test:     built: %u elements, attributes", 
                      gi.vertex_attribute_count);
               for (unsigned k = 0; k < gi.vertex_attribute_count; k++)
                  printf(" %u", gi.vertex_attribute_index[k]);
               printf("\n");
            } else {
               printf("air_metal_test:     refused -- %s\n", why);
            }
            fflush(stdout);
            _exit(r ? 0 : 1);
         }
         int wstatus = 0;
         if (waitpid(pid, &wstatus, 0) < 0) {
            printf("air_metal_test:   waitpid: %s\n", strerror(errno));
            return 1;
         }
         bool refused_by_layout = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 1;
         bool got_past = !WIFEXITED(wstatus) ||
                         (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

         printf("air_metal_test:   %s: %s\n", gcase[c].what,
                refused_by_layout ? "the LAYOUT refused it"
                : WIFEXITED(wstatus) ? "built"
                                     : "got past the layout and died in the back end, which "
                                       "on this machine is softpipe's own assertion -- its "
                                       "text is in the lines above -- and not a refusal of "
                                       "the layout");
         if (gcase[c].color && !got_past) {
            printf("air_metal_test:     and it must not have been: the three unconfigured "
                   "attributes are gated by function constants, so a pipeline is allowed to "
                   "configure nothing at those indices\n");
            gated_ok = false;
         }
         if (!gcase[c].color && !refused_by_layout) {
            printf("air_metal_test:     and it must have been: color is read, ungated and "
                   "unconfigured\n");
            gated_ok = false;
         }
      }
      printf("air_metal_test:   %s\n",
             gated_ok ? "the gate is honoured: a gated attribute needs no configuration and "
                        "an ungated one does, and the refusal named the ungated one -- which "
                        "is the fifth attribute in AIR's list, not the second"
                      : "THE GATE IS NOT HONOURED as docs/air.md 3.1 requires");
      if (!gated_ok)
         ok = false;
   } else {
      printf("air_metal_test: 9. the function-constant gate was NOT measured: give a fifth "
             "argument naming a module whose attributes are gated (QuartzCore's "
             "fixed_vert_lph_spc) to measure it\n");
   }

   /*
    * And the layouts that must be REFUSED, each by name. Every one of them is a
    * descriptor a real application can build -- Metal accepts all but the last two -- and
    * what makes them worth a sentence rather than a silent clamp is in air_metal.h: the
    * offset's twelve bits are set with a masking setter, so 4096 would fetch 0, and
    * fd6_vertex_state_create asserts on a format it cannot fetch instead of refusing it.
    */
   struct {
      const char *what;
      struct attrib_layout layout;
      unsigned step_function, step_rate, instance_buffer;
      bool generated;
   } bad[] = {
      { "attribute 0 configured with no format at all", narrow, MTL_STEP_PER_VERTEX, 1,
        ~0u, false },
      { "an integer format for a float4 attribute", narrow, MTL_STEP_PER_VERTEX, 1, ~0u,
        false },
      { "MTLVertexFormatFloatRGB9E5, which the VFD cannot fetch", narrow,
        MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "MTLVertexFormatUInt1010102Normalized, a packed word", narrow,
        MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "a format of 99, which Metal has not", narrow, MTL_STEP_PER_VERTEX, 1, ~0u,
        false },
      { "an attribute at offset 4096", narrow, MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "a stride of 4096", narrow, MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "an attribute that reads into the next vertex", narrow, MTL_STEP_PER_VERTEX, 1,
        ~0u, false },
      { "MTLVertexStepFunctionPerPatch", split, MTL_STEP_PER_PATCH, 1, 6, false },
      { "per-vertex at a step rate of 3", narrow, MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "a stream at the buffer index the module reads mvp_matrix from", narrow,
        MTL_STEP_PER_VERTEX, 1, ~0u, false },
      { "MTLVertexStepFunctionPerVertex with a stride of 0", narrow, MTL_STEP_PER_VERTEX,
        1, ~0u, false },
      { "a vertex descriptor with no vertex function at all", narrow, MTL_STEP_PER_VERTEX,
        1, ~0u, true },
   };
   unsigned refused = 0;
   printf("air_metal_test: and the vertex layouts that must be refused, each by name:\n");
   for (unsigned i = 0; i < COUNT_OF(bad); i++) {
      struct rwa_render_shape s = shape;

      attrib_configure(&s, &bad[i].layout, bad[i].step_function, bad[i].step_rate,
                       bad[i].instance_buffer);
      switch (i) {
      case 0: s.vertex_attribute[0].format = 0; break;
      case 1: s.vertex_attribute[0].format = MTL_VF_INT4; break;
      case 2: s.vertex_attribute[0].format = MTL_VF_FLOAT_RGB9E5; break;
      case 3: s.vertex_attribute[0].format = MTL_VF_UINT1010102_NORMALIZED; break;
      case 4: s.vertex_attribute[0].format = 99; break;
      case 5: s.vertex_attribute[0].offset = 4096; break;
      case 6: s.vertex_layout[5].stride = 4096; break;
      case 7: s.vertex_attribute[0].offset = 12; break;   /* 12 + 8 > a 16-byte element */
      case 8: break;                                      /* the step function is above */
      case 9: s.vertex_layout[5].step_rate = 3; break;
      case 10:
         s.vertex_attribute[0].buffer_index = 2;          /* mvp_matrix's own index */
         s.vertex_layout[2] = s.vertex_layout[5];
         break;
      case 11: s.vertex_layout[5].stride = 0; break;
      case 12: break;                                     /* no vertex module, below */
      }
      why[0] = '\0';
      struct rwa_render *r = bad[i].generated
         ? rwa_render_create(device, NULL, 0, NULL, fbitcode, fbitcode_size, ffunc, &s,
                             NULL, why, sizeof(why))
         : rwa_render_create(device, vbitcode, vbitcode_size, vfunc, fbitcode,
                             fbitcode_size, ffunc, &s, NULL, why, sizeof(why));
      if (r) {
         printf("air_metal_test:   %s: BUILT, and it should not have\n", bad[i].what);
         rwa_render_destroy(r);
         ok = false;
      } else {
         printf("air_metal_test:   %s: refused -- %s\n", bad[i].what, why);
         refused++;
      }
   }
   printf("air_metal_test: %u of %u refused by name\n", refused, (unsigned)COUNT_OF(bad));
   if (refused != COUNT_OF(bad))
      ok = false;

   /* And the draws that must be refused although the pipeline is sound: the stream the
    * layout fetches from, missing, and a buffer too small for the vertices asked for.
    * Both are the application's mistake and both are bounded rather than wild on these
    * back ends -- freedreno programs VFD_FETCH_SIZE off the resource and softpipe clamps
    * in the draw module -- so what they would otherwise produce is a picture made partly
    * of zeros.
    */
   printf("air_metal_test: and the draws that must be refused:\n");
   {
      static const struct { const char *what; unsigned slot, count; size_t offset; } dbad[] = {
         { "nothing bound at the slot the layout fetches from", 9, 4, 0 },
         { "four vertices of a stream bound 16 bytes from its end", 5, 4, 112 },
         { "sixteen vertices of a stream that holds eight", 5, 16, 0 },
      };
      unsigned draw_refused = 0;
      for (unsigned i = 0; i < COUNT_OF(dbad); i++) {
         struct rwa_draw r = base;
         static const float black[4] = { 0, 0, 0, 0 };

         memset(streams, 0, sizeof(streams));
         if (dbad[i].slot < COUNT_OF(streams))
            streams[dbad[i].slot] = (struct rwa_vertex_stream){
               .buffer = narrow_stream, .offset = dbad[i].offset };
         r.clear_color = black;
         r.vertex_count = dbad[i].count;
         why[0] = '\0';
         if (rwa_render_draw(narrow_pipe, &r, why, sizeof(why)) == 0) {
            printf("air_metal_test:   %s: DREW, and it should not have\n", dbad[i].what);
            ok = false;
         } else {
            printf("air_metal_test:   %s: refused -- %s\n", dbad[i].what, why);
            draw_refused++;
         }
      }
      if (draw_refused != COUNT_OF(dbad))
         ok = false;
   }

   printf("air_metal_test: %s\n",
          ok ? "THE VERTEX ATTRIBUTE LAYOUT IS READ -- the offsets, the stride, the "
               "formats, the buffer index, the step function and its rate each changed "
               "what came back, and the element order is the shader's driver_location "
               "order and not Metal's attribute order"
             : "SOMETHING ABOVE DISAGREES, and a layout that is nearly right draws a "
               "picture of the wrong vertices");

   rwa_render_destroy(wide_pipe);
   rwa_render_destroy(narrow_pipe);
   rwa_render_destroy(stride_pipe);
   rwa_render_destroy(seven_pipe);
   rwa_render_destroy(inst_pipe);
   rwa_render_destroy(rate_pipe);
   rwa_sampler_destroy(sampler);
   rwa_texture_destroy(tex);
   rwa_render_destroy(bgra_pipe);
   rwa_buffer_destroy(bgra_stream);
   rwa_buffer_destroy(wide_stream);
   rwa_buffer_destroy(narrow_stream);
   rwa_buffer_destroy(instance_stream);
   rwa_buffer_destroy(indices);
   for (unsigned i = 0; i < nvbuf; i++)
      rwa_buffer_destroy(vbuffers[i]);
   for (unsigned i = 0; i < color_count; i++)
      rwa_texture_destroy(color[i]);
   rwa_device_close(device);
   return ok ? 0 : 1;
}

/*
 * --attrib-sweep: how many of a library's vertex functions get a pipeline out of this
 * contract now, which is the one number on this machine that predicts what the phone's
 * window server will do with the layout.
 *
 * It asks exactly what -newRenderPipelineStateWithDescriptor: asks and nothing more: a
 * vertex module, a fragment module and a descriptor, in and a pipeline out. No draw --
 * there is nothing to check in a picture here, because the descriptor below is not any
 * application's and the buffers are not filled.
 *
 * The descriptor is the same for every module and is deliberately the most permissive
 * one that is still a real layout: all 31 attributes configured as MTLVertexFormatFloat4
 * at offset 0 of a 64-byte element stepping per vertex out of buffer index 0. So an
 * attribute index any module reads is configured, and what the sweep measures is
 * everything else -- the base types, the element order, the buffer index space, the step
 * function, and whether ir3 compiles a vertex stage that was never reached while the
 * contract refused it.
 *
 * What it is NOT: a measurement of the descriptors QuartzCore actually builds. Float4 is
 * the right base type for every attribute in that library (`float4`, `float2`, `float`
 * and `half4` are all floating-point members) and the wrong one for a quarter of
 * Resident Evil 4's, which are integers -- so a module refused here for a base type is
 * being told about this sweep's descriptor and not about its own.
 *
 *   air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB FRAGMENTFUNCTION
 */
static int
run_attrib_sweep(const char *vpath, const char *fpath, const char *ffunc)
{
   struct metallib vlib = { 0 }, flib = { 0 };
   char fname[256];
   const uint8_t *fbitcode = NULL;
   size_t fbitcode_size = 0;

   if (!load_metallib(vpath, &vlib) || !load_metallib(fpath, &flib))
      return 1;
   if (!find_function(&flib, ffunc, fname, sizeof(fname), &fbitcode, &fbitcode_size))
      return 1;

   struct metallib_table t;
   if (!parse_functions(&vlib, &t))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: --attrib-sweep %s over %s on \"%s\": %u functions\n", vpath,
          fname, rwa_device_name(device), t.count);

   struct rwa_render_shape shape = { .color_count = 4, .sample_count = 1 };
   for (unsigned i = 0; i < shape.color_count; i++)
      shape.color_format[i] = MTL_RGBA32_UINT;
   for (unsigned i = 0; i < RWA_MAX_VERTEX_ATTRIBUTES; i++)
      shape.vertex_attribute[i] = (struct rwa_vertex_attribute){
         .format = MTL_VF_FLOAT4, .offset = 0, .buffer_index = 0 };
   shape.vertex_layout[0] = (struct rwa_vertex_layout){
      .stride = 64, .step_function = MTL_STEP_PER_VERTEX, .step_rate = 1 };

   /*
    * One child per module, and the exit status is the outcome: 0 built with attributes,
    * 3 built without any, 1 refused by name (the child printed the sentence), 2 not a
    * vertex function at all. A child that DIES says something no status could: softpipe
    * asserts on a 16-bit NIR opcode instead of declining it, and a `half` attribute's
    * narrowing is exactly that opcode -- so those modules are measurable on the Adreno
    * and not here, which is the same boundary docs/air.md draws around softpipe.
    */
   enum { SWEPT_ATTRIBS = 0, SWEPT_REFUSED = 1, SWEPT_NOT_VERTEX = 2, SWEPT_PLAIN = 3 };
   unsigned built = 0, fetching = 0, refused = 0, not_vertex = 0, died = 0;
   for (unsigned i = 0; i < t.count; i++) {
      if (!t.bitcode[i] || !t.names[i])
         continue;
      fflush(stdout);
      pid_t pid = fork();
      if (pid < 0) {
         printf("air_metal_test: fork: %s\n", strerror(errno));
         return 1;
      }
      if (pid == 0) {
         struct rwa_render_info info = { 0 };
         int status = SWEPT_PLAIN;

         why[0] = '\0';
         struct rwa_render *r =
            rwa_render_create(device, t.bitcode[i], t.size[i], t.names[i], fbitcode,
                              fbitcode_size, ffunc, &shape, &info, why, sizeof(why));
         if (r && info.vertex_attribute_count) {
            printf("air_metal_test:   %-34s %u element%s, %u stream slot%s, mask 0x%x, "
                   "attributes", t.names[i], info.vertex_attribute_count,
                   info.vertex_attribute_count == 1 ? "" : "s", info.vertex_streams,
                   info.vertex_streams == 1 ? "" : "s", info.vertex_stream_mask);
            for (unsigned k = 0; k < info.vertex_attribute_count; k++)
               printf(" %u", info.vertex_attribute_index[k]);
            printf("\n");
            status = SWEPT_ATTRIBS;
         } else if (!r) {
            status = strstr(why, "takes a vertex function") ? SWEPT_NOT_VERTEX
                                                            : SWEPT_REFUSED;
            if (status == SWEPT_REFUSED)
               printf("air_metal_test:   %-34s REFUSED -- %s\n", t.names[i], why);
         }
         /* _exit and not exit: the pipeline and the device are the parent's as well, and
          * running this child's atexit handlers over them would free the parent's work
          * twice in two processes that share nothing.
          */
         fflush(stdout);
         _exit(status);
      }
      int wstatus = 0;
      if (waitpid(pid, &wstatus, 0) < 0) {
         printf("air_metal_test: waitpid: %s\n", strerror(errno));
         return 1;
      }
      if (!WIFEXITED(wstatus)) {
         died++;
         printf("air_metal_test:   %-34s DIED (signal %d), which on this machine means "
                "softpipe's own assertion and not a refusal of ours\n", t.names[i],
                WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0);
         continue;
      }
      switch (WEXITSTATUS(wstatus)) {
      case SWEPT_ATTRIBS:    built++; fetching++; break;
      case SWEPT_PLAIN:      built++; break;
      case SWEPT_REFUSED:    refused++; break;
      case SWEPT_NOT_VERTEX: not_vertex++; break;
      default:               refused++; break;
      }
   }
   printf("air_metal_test: %u vertex pipelines built, %u of them fetching attributes, %u "
          "refused by name, %u died inside the back end, %u functions are not vertex "
          "functions\n", built, fetching, refused, died, not_vertex);

   rwa_device_close(device);
   free_functions(&t);
   return refused ? 1 : 0;
}

/*
 * --surface: rwa_texture_from_surface, and the refusals it owes.
 *
 * This is the drawable path of the Metal driver, from the library's side (docs/metal.md
 * 10.46): CoreAnimation makes an IOSurface and asks the device for a texture over it, and
 * the one call that can answer is this one. The test exists because the answer is a NULL
 * and a sentence, and a sentence is only worth anything if the right one comes out of the
 * right wrong input.
 *
 * So it sweeps six shapes against one surface ID, and every one but the first is a case
 * this call is supposed to decline BY NAME:
 *
 *   1  a plain 2D texture of the format and geometry a drawable has -- the only one that
 *      can succeed, and on a machine with no GPU it still cannot: the software screen has
 *      no resource_from_handle, and saying so is the refusal under test
 *   2  a mip chain over it           (the import is told one pitch and nothing else)
 *   3  an array of slices over it    (one plane of one image)
 *   4  a multisample type over it    (a sample count the layout would have to agree about)
 *   5  a cube over it                (the same, with six faces)
 *   6  a pitch of zero              (the import takes the pitch from the caller)
 *
 * The surface ID is the caller's, because this machine has no IOSurface at all: on the
 * phone it comes from a surface the caller made, and here any number reaches the refusal
 * that does not depend on there being a surface. An ID of 0 is its own refusal and is
 * checked too, which is why the sweep is run twice when no ID is given.
 */
static int
run_surface(int surface_id, unsigned width, unsigned height, unsigned row_bytes)
{
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: surface sweep on %s: IOSurface %d, %ux%u, %u bytes a row\n",
          rwa_device_name(device), surface_id, width, height, row_bytes);

   const struct rwa_texture_shape base = {
      .pixel_format = 80 /* MTLPixelFormatBGRA8Unorm */, .type = 2 /* MTLTextureType2D */,
      .width = width, .height = height, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_RENDER_TARGET,
   };
   struct {
      const char *what;
      struct rwa_texture_shape shape;
      unsigned row_bytes;
      bool must_refuse;
   } cases[] = {
      { "a drawable's own shape",       base, row_bytes, false },
      { "four mip levels",             base, row_bytes, true },
      { "two array slices",            base, row_bytes, true },
      { "four samples a texel",        base, row_bytes, true },
      { "a cube's six faces",          base, row_bytes, true },
      { "no bytes a row",              base, 0,         true },
   };
   cases[1].shape.mip_levels = 4;
   cases[2].shape.type = 3 /* MTLTextureType2DArray */;
   cases[2].shape.array_length = 2;
   cases[3].shape.type = 4 /* MTLTextureType2DMultisample */;
   cases[3].shape.sample_count = 4;
   cases[4].shape.type = 5 /* MTLTextureTypeCube */;
   cases[4].shape.height = width;

   unsigned wrong = 0;
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      why[0] = 0;
      struct rwa_texture *texture =
         rwa_texture_from_surface(device, &cases[i].shape, surface_id, 0,
                                  cases[i].row_bytes, why, sizeof(why));
      if (texture) {
         printf("  %-24s MADE a texture over the surface\n", cases[i].what);
         if (cases[i].must_refuse) {
            printf("  ^^ WRONG: that shape cannot be laid over an imported surface and "
                   "should have been refused by name\n");
            wrong++;
         }
         rwa_texture_destroy(texture);
         continue;
      }
      printf("  %-24s refused: %s\n", cases[i].what, why);
      if (!why[0]) {
         printf("  ^^ WRONG: refused without a sentence, which is the one thing a caller "
                "cannot act on\n");
         wrong++;
      }
   }

   /* An ID of zero has a refusal of its own, and it must not be the pitch one: a caller
    * that passes a surface nobody made should be told that, not told about alignment. */
   why[0] = 0;
   if (rwa_texture_from_surface(device, &base, 0, 0, row_bytes, why, sizeof(why))) {
      printf("  %-24s MADE a texture, which is wrong: there is no surface 0\n",
             "surface ID zero");
      wrong++;
   } else {
      printf("  %-24s refused: %s\n", "surface ID zero", why);
      if (!strstr(why, "not a surface")) {
         printf("  ^^ WRONG: that is not the sentence an ID of zero owes\n");
         wrong++;
      }
   }

   rwa_device_close(device);
   printf("air_metal_test: surface sweep: %u answer%s wrong\n", wrong,
          wrong == 1 ? "" : "s");
   return wrong ? 1 : 0;
}

/*
 * --argbufs: what argument buffers a module declares, and the two numbers an
 * application allocates its buffer from.
 *
 * This is the reflection half, and it is printed rather than asserted because the
 * numbers have to be Apple's: -[MTLArgumentEncoder encodedLength] is what the
 * application passes to -newBufferWithLength:, so a row here can be put beside
 * `air.arg_type_size` in the module's own metadata (tools/air/argbuflen.py counts that
 * over a whole library) and the two readings compared.
 *
 *   air_metal_test --argbufs FILE[.metallib|.bc] [FUNCTION]
 */
static int
run_argbufs(const char *path, const char *only)
{
   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }

   /* One module, or every function of a container, by the same reader the draw modes
    * use: on the device these bytes come from -[_MTLFunction bitcodeData].
    */
   struct metallib_table t = { 0 };
   bool container = lib.size >= 4 && memcmp(lib.bytes, "MTLB", 4) == 0;
   if (container && !parse_functions(&lib, &t)) {
      rwa_device_close(device);
      return 1;
   }
   unsigned count = container ? t.count : 1;
   int rc = 0;

   for (unsigned i = 0; i < count; i++) {
      const char *name = container ? t.names[i] : "(the only module)";
      const uint8_t *bitcode = container ? t.bitcode[i] : lib.bytes;
      size_t size = container ? t.size[i] : lib.size;

      if (!bitcode || (only && name && strcmp(only, name)))
         continue;

      struct rwa_argbuf_layout sets[4];
      unsigned nsets = 0;
      if (rwa_module_argbufs(device, bitcode, size, container ? name : NULL, sets,
                             COUNT_OF(sets), &nsets, why, sizeof(why)) != 0) {
         printf("argbufs %s  refused: %s\n", name, why);
         rc = 1;
         continue;
      }
      if (nsets == 0) {
         printf("argbufs %s  none\n", name);
         continue;
      }
      for (unsigned k = 0; k < nsets; k++) {
         const struct rwa_argbuf_layout *l = &sets[k];

         printf("argbufs %s  set at [[buffer(%u)]]  encodedLength %u  alignment %u  "
                "%u members\n", name, l->metal_index, l->encoded_length, l->alignment,
                l->member_count);
         for (unsigned m = 0; m < l->member_count; m++) {
            static const char *const kind[] = { "buffer", "texture", "sampler" };

            printf("argbufs %s    id %u  %s  index %u%s  %s\n", name, l->members[m].id,
                   l->members[m].kind < 3 ? kind[l->members[m].kind] : "?",
                   l->members[m].index, l->members[m].write ? "  written" : "",
                   l->members[m].name);
         }
      }
   }

   if (container)
      free_functions(&t);
   rwa_device_close(device);
   return rc;
}

/*
 * --draw-argbuf: the same picture through an argument buffer as through direct
 * arguments, and a NAMED DIFFERENT picture for every record that is deliberately wrong.
 *
 * What this measures and why it is shaped this way:
 *
 *   - THE DIFFERENTIAL. tools/air/argbuf_set.ll and tools/air/argbuf_direct.ll compute
 *     the same four floats out of the same five resources; the only difference is that
 *     one reaches them through `air.indirect_buffer` members and the other through
 *     [[buffer(0)]], [[sampler(0)]], [[texture(0)]], [[texture(1)]], [[sampler(1)]].
 *     So "the encoder works" needs no oracle: the two draws must produce the same
 *     pixel, and the direct draw is a path this library has drawn 255 shaders through
 *     (docs/air.md section 5).
 *   - THE WRONG RECORDS. A differential that only ever passes says nothing about what
 *     would have failed, so every member is also encoded WRONG once -- the two samplers
 *     exchanged, the two textures exchanged, the buffer member at another offset -- and
 *     each has a picture of its own written out below. Those are the two gaps the
 *     frontend's own check could not close: air_run binds one sampler state to every
 *     slot, so a sampler member's index was invisible there, and softpipe's COMPUTE path
 *     answers every texture from the view bound at slot 0, so a texture member's index
 *     was invisible as well (argbufcheck.py says both in as many words). A fragment
 *     shader on the same softpipe distinguishes them, and R and A of one pixel are the
 *     same texture through two samplers, which is what makes a swap visible at all.
 *   - THE REFUSALS. An argument buffer nobody encoded, a member nobody set, a member set
 *     with the wrong call, no buffer at the set's index at all, a resource destroyed
 *     after it was encoded, and an offset this GPU cannot fetch from. Each must come
 *     back with the sentence this feature was written for and not with a picture.
 *
 * One buffer holds FOUR encodings at four offsets here, and they are drawn in turn: that
 * is Metal's own object model (an application allocates one argument buffer and encodes
 * many sets into it at multiples of encodedLength) and the reason the records are keyed
 * by offset rather than kept on the encoder.
 *
 *   air_metal_test --draw-argbuf SETMODULE DIRECTMODULE
 */

/* The four floats of one pixel, and what they mean (argbuf_set.ll):
 *   [0] texture member 2 sampled through sampler member 1 (nearest)
 *   [1] texture member 3 sampled through sampler member 4 (linear)
 *   [2] the float at the start of buffer member 0, wherever it was encoded from
 *   [3] texture member 2 FETCHED at texel (2, 3), with no sampler at all
 *
 * With air_run's texel pattern -- channel 0 of texel (x, y) of the texture at slot s is
 * 100 + x + 1000 * s -- the reference picture is therefore
 *
 *   101    nearest at (0.3125, 0.375) of slot 0 is texel (1, 1)
 *   1100.75  linear there is a quarter of texel (0, 1) and three quarters of (1, 1),
 *            of slot 1
 *   7.5    the buffer
 *   102    texel (2, 3) of slot 0
 *
 * and three of those four are exact whatever the filter weights are, which is why they
 * are checked against the arithmetic below and the linear one is only bounded.
 */
struct argbuf_pixel {
   float c[4];
};

static bool
argbuf_pixel_eq(const struct argbuf_pixel *a, const struct argbuf_pixel *b)
{
   for (unsigned i = 0; i < 4; i++)
      if (a->c[i] != b->c[i])
         return false;
   return true;
}

static void
argbuf_print_pixel(const char *what, const struct argbuf_pixel *p)
{
   printf("draw-argbuf: %-34s %.9g %.9g %.9g %.9g\n", what, (double)p->c[0],
          (double)p->c[1], (double)p->c[2], (double)p->c[3]);
}

/* One draw and the pixel it left, read at two corners: the shader writes the same value
 * at every fragment (it takes no varying at all), so two corners that disagree would be
 * a rasterisation or readback fault rather than anything about a binding, and this says
 * so instead of reading one pixel and trusting it.
 */
static bool
argbuf_draw_pixel(struct rwa_render *render, const struct rwa_draw *draw,
                  struct rwa_texture *attachment, struct argbuf_pixel *out, char *why,
                  size_t why_size)
{
   if (rwa_render_draw(render, draw, why, why_size) != 0)
      return false;

   const unsigned one[3] = { 1, 1, 1 };
   const unsigned corner[2][3] = { { 0, 0, 0 }, { TEX - 1, TEX - 1, 0 } };
   struct argbuf_pixel got[2] = { { { 0 } } };

   for (unsigned k = 0; k < 2; k++) {
      if (rwa_texture_read(attachment, 0, 0, corner[k], one, got[k].c, 0, why,
                           why_size) != 0)
         return false;
   }
   if (!argbuf_pixel_eq(&got[0], &got[1])) {
      snprintf(why, why_size, "the two corners of the attachment came back different "
                              "(%.9g... and %.9g...)", (double)got[0].c[0],
               (double)got[1].c[0]);
      return false;
   }
   *out = got[0];
   return true;
}

static int
run_draw_argbuf(const char *setpath, const char *directpath)
{
   struct metallib setlib = { 0 }, directlib = { 0 };
   if (!load_metallib(setpath, &setlib) || !load_metallib(directpath, &directlib))
      return 1;

   char setname[256], directname[256];
   const uint8_t *setbits = NULL, *directbits = NULL;
   size_t setsize = 0, directsize = 0;
   if (!find_function(&setlib, NULL, setname, sizeof(setname), &setbits, &setsize) ||
       !find_function(&directlib, NULL, directname, sizeof(directname), &directbits,
                      &directsize))
      return 1;

   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("draw-argbuf: device is \"%s\"\n", rwa_device_name(device));

   int failed = 0, checks = 0;

   /* The set's layout, which is what an application would allocate its buffer from.
    * Checked against the module this test wrote: five members, 8 bytes each, and the
    * flat indices the frontend allocates in (set, member) order per kind.
    */
   struct rwa_argbuf_layout layout[2];
   unsigned nsets = 0;
   if (rwa_module_argbufs(device, setbits, setsize, NULL, layout, COUNT_OF(layout),
                          &nsets, why, sizeof(why)) != 0) {
      printf("draw-argbuf: rwa_module_argbufs: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   static const struct { unsigned id, kind, index; } want_member[] = {
      { 0, RWA_ARGBUF_BUFFER, 0 }, { 1, RWA_ARGBUF_SAMPLER, 0 },
      { 2, RWA_ARGBUF_TEXTURE, 0 }, { 3, RWA_ARGBUF_TEXTURE, 1 },
      { 4, RWA_ARGBUF_SAMPLER, 1 },
   };
   bool layout_ok = nsets == 1 && layout[0].metal_index == 0 &&
                    layout[0].encoded_length == 8 * COUNT_OF(want_member) &&
                    layout[0].alignment == 8 &&
                    layout[0].member_count == COUNT_OF(want_member);
   for (unsigned i = 0; layout_ok && i < COUNT_OF(want_member); i++)
      layout_ok = layout[0].members[i].id == want_member[i].id &&
                  layout[0].members[i].kind == want_member[i].kind &&
                  layout[0].members[i].index == want_member[i].index;
   printf("draw-argbuf: %s: %u set%s, encodedLength %u, alignment %u, %u members -- %s\n",
          layout_ok ? "ok" : "FAIL", nsets, nsets == 1 ? "" : "s",
          nsets ? layout[0].encoded_length : 0, nsets ? layout[0].alignment : 0,
          nsets ? layout[0].member_count : 0,
          layout_ok ? "the five members this module declares, at the flat indices "
                      "(buffer 0; samplers 0, 1; textures 0, 1) the frontend allocates"
                    : "NOT what tools/air/argbuf_set.ll declares");
   failed += !layout_ok;
   checks++;

   /* And the control module declares none, which is the other half of that answer: a
    * module without an `air.indirect_buffer` is 0 sets and not an error.
    */
   unsigned ndirect = 99;
   bool direct_none = rwa_module_argbufs(device, directbits, directsize, NULL, layout,
                                         COUNT_OF(layout), &ndirect, why,
                                         sizeof(why)) == 0 && ndirect == 0;
   printf("draw-argbuf: %s: the direct module declares %u argument buffers\n",
          direct_none ? "ok" : "FAIL", ndirect);
   failed += !direct_none;
   checks++;

   /*
    * The rig, shared by every draw below so that nothing but the binding differs: one
    * RGBA32Float attachment, two textures with air_run's texel pattern (slot 0 and slot
    * 1, which differ by 1000 in channel 0 -- that is what makes a texture swap visible),
    * two samplers that differ in their filter, and one buffer holding two floats 64
    * bytes apart.
    */
   const unsigned origin[3] = { 0, 0, 0 };
   const struct rwa_texture_shape ats = {
      .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
   };
   struct rwa_texture *attachment = rwa_texture_create(device, &ats, why, sizeof(why));
   if (!attachment) {
      printf("draw-argbuf: rwa_texture_create (attachment): %s\n", why);
      rwa_device_close(device);
      return 1;
   }

   struct rwa_texture *tex[2] = { NULL, NULL };
   for (unsigned i = 0; i < 2; i++) {
      const struct rwa_texture_shape ts = {
         .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
         .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
         .sample_count = 1, .usage = MTL_USAGE_SHADER_READ,
      };
      tex[i] = rwa_texture_create(device, &ts, why, sizeof(why));
      if (!tex[i]) {
         printf("draw-argbuf: rwa_texture_create (texture %u): %s\n", i, why);
         return 1;
      }
      static float texels[TEXELS];
      memset(texels, 0, sizeof(texels));
      for (unsigned y = 0; y < TEX; y++)
         for (unsigned x = 0; x < TEX; x++) {
            float v[4];
            texel_value(i, x, y, 0, v);
            for (unsigned c = 0; c < 4; c++)
               texels[texel_index(x, y, 0) + c] = v[c];
         }
      const unsigned extent[3] = { TEX, TEX, 1 };
      if (rwa_texture_write(tex[i], 0, 0, origin, extent, texels, 0, why,
                            sizeof(why)) != 0) {
         printf("draw-argbuf: rwa_texture_write (texture %u): %s\n", i, why);
         return 1;
      }
   }

   /* Nearest and linear, and the coordinate the shader samples at is between two texels
    * on purpose: nearest there is texel (1, 1) and linear is a weighted sum, so the two
    * states cannot be confused for one another. Everything else about them is equal.
    */
   struct rwa_sampler *samp[2] = { NULL, NULL };
   for (unsigned i = 0; i < 2; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 },                 /* ClampToEdge */
         .min_filter = i, .mag_filter = i,       /* 0 Nearest, 1 Linear */
         .mip_filter = 0, .compare_function = 0, .max_anisotropy = 1,
         .normalized_coordinates = 1, .lod_min = 0.0f, .lod_max = 65504.0f,
      };
      samp[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samp[i]) {
         printf("draw-argbuf: rwa_sampler_create (sampler %u): %s\n", i, why);
         return 1;
      }
   }

   /* The buffer the shader's member 0 points at: 7.5 at offset 0 and 9.25 at offset 64.
    * 64 and not 16, so that the same encoding is legal on the Adreno as well -- a6xx
    * fetches a UBO at multiples of 64 and softpipe at multiples of 16, and the point of
    * the offset case is the offset reaching the GPU rather than an alignment refusal.
    */
   struct rwa_buffer *values = rwa_buffer_create(device, 2048, why, sizeof(why));
   /* ... and the argument buffer itself, four encodings of 40 bytes at 64-byte spacing */
   struct rwa_buffer *argbuf = rwa_buffer_create(device, 512, why, sizeof(why));
   struct rwa_buffer *unencoded = rwa_buffer_create(device, 512, why, sizeof(why));
   if (!values || !argbuf || !unencoded) {
      printf("draw-argbuf: rwa_buffer_create: %s\n", why);
      return 1;
   }
   {
      float *f = rwa_buffer_map(values);
      memset(f, 0, rwa_buffer_size(values));
      f[0] = 7.5f;
      f[1] = 3.25f;       /* one float in = byte 4, which no GPU here can fetch from */
      f[16] = 9.25f;      /* 16 floats in = byte 64, which both can */
      /* and byte 1164, which is the offset the window server of this port binds its
       * fragment buffer 2 at, 55 times in one boot: 1164 is a multiple of 4 (Metal's own
       * rule) and of neither 16 nor 64, so it is the uploaded path at its own number */
      f[291] = 5.75f;
      /* The unencoded buffer is not left as zeros: zeros would be a handle of zero,
       * which is what an application that forgot to encode would have, and this test
       * must not pass because the bytes happened to be unreadable. These are handles
       * that LOOK like handles.
       */
      uint64_t *h = rwa_buffer_map(unencoded);
      for (unsigned i = 0; i < rwa_buffer_size(unencoded) / 8; i++)
         h[i] = 0x0000000100000000ull + i * 0x40;
   }

   /* The two pipelines. Neither is given a vertex module, so the library supplies its
    * quad: these shaders read no varying at all, which is what makes every fragment's
    * value the same and a pixel comparable with ==.
    */
   struct rwa_render_shape shape = { .color_count = 1, .sample_count = 1,
                                     .color_format[0] = MTL_RGBA32_FLOAT };
   struct rwa_render_info setinfo = { 0 }, directinfo = { 0 };
   struct rwa_render *setpipe =
      rwa_render_create(device, NULL, 0, NULL, setbits, setsize, NULL, &shape, &setinfo,
                        why, sizeof(why));
   if (!setpipe) {
      printf("draw-argbuf: rwa_render_create (the set module): %s\n", why);
      return 1;
   }
   struct rwa_render *directpipe =
      rwa_render_create(device, NULL, 0, NULL, directbits, directsize, NULL, &shape,
                        &directinfo, why, sizeof(why));
   if (!directpipe) {
      printf("draw-argbuf: rwa_render_create (the direct module): %s\n", why);
      return 1;
   }
   bool mask_ok = setinfo.fragment_argbuf_mask == 1u &&
                  directinfo.fragment_argbuf_mask == 0u;
   printf("draw-argbuf: %s: the set pipeline reports fragment_argbuf_mask %#x and the "
          "direct one %#x\n", mask_ok ? "ok" : "FAIL",
          setinfo.fragment_argbuf_mask, directinfo.fragment_argbuf_mask);
   failed += !mask_ok;
   checks++;

   /* THE DIRECT DRAW, which is the reference every encoded draw is compared against. */
   struct rwa_buffer *const direct_buffers[] = { values };
   struct rwa_texture *const direct_textures[] = { tex[0], tex[1] };
   struct rwa_sampler *const direct_samplers[] = { samp[0], samp[1] };
   const struct rwa_draw direct_draw = {
      .color = (struct rwa_texture *const[]){ attachment }, .color_count = 1,
      .clear_color = (const float[]){ 0, 0, 0, 0 },
      .fragment = { .buffers = direct_buffers, .buffer_count = 1,
                    .textures = direct_textures, .texture_count = 2,
                    .samplers = direct_samplers, .sampler_count = 2 },
      .primitive = 4, .vertex_start = 0, .vertex_count = 4,
   };
   struct argbuf_pixel reference;
   if (!argbuf_draw_pixel(directpipe, &direct_draw, attachment, &reference, why,
                          sizeof(why))) {
      printf("draw-argbuf: the direct draw failed: %s\n", why);
      return 1;
   }
   argbuf_print_pixel("direct, the reference:", &reference);

   /*
    * THE ENCODED DRAW. Nothing is in draw.fragment at all: the five resources reach the
    * shader only through the records, so a lookup that failed would draw nothing rather
    * than draw this.
    */
   struct rwa_buffer_bind at0[1] = { { argbuf, 0 } };
   struct rwa_draw encoded_draw = direct_draw;
   encoded_draw.fragment = (struct rwa_bindings){ 0 };
   encoded_draw.fragment_buffer_table = at0;
   encoded_draw.fragment_buffer_table_count = 1;

   bool enc = rwa_argbuf_encode_buffer(argbuf, 0, 0, values, 0, why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_sampler(argbuf, 0, 1, samp[0], why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_texture(argbuf, 0, 2, tex[0], why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_texture(argbuf, 0, 3, tex[1], why, sizeof(why)) == 0 &&
              rwa_argbuf_encode_sampler(argbuf, 0, 4, samp[1], why, sizeof(why)) == 0;
   if (!enc) {
      printf("draw-argbuf: encoding at offset 0: %s\n", why);
      return 1;
   }
   printf("draw-argbuf: %u members recorded at offset 0 of the argument buffer\n",
          rwa_argbuf_encoded_members(argbuf, 0));

   struct argbuf_pixel through;
   if (!argbuf_draw_pixel(setpipe, &encoded_draw, attachment, &through, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the encoded draw failed: %s\n", why);
      failed += 2;
      checks += 2;
   } else {
      argbuf_print_pixel("through the encoder:", &through);
      bool same = argbuf_pixel_eq(&through, &reference);
      printf("draw-argbuf: %s: the same pixel through an argument buffer as through "
             "direct arguments\n", same ? "ok" : "FAIL");
      failed += !same;
      checks++;

      /* And the reference is what the texel pattern says it should be, so that "the two
       * agree" cannot be two draws agreeing on the same wrong resource. Three channels
       * are exact arithmetic; the linear one is only bounded, because the weights are
       * the rasteriser's and this test is not about them.
       */
      bool arith = through.c[0] == 101.0f && through.c[3] == 102.0f &&
                   through.c[2] == 7.5f &&
                   through.c[1] > 1100.0f && through.c[1] < 1101.0f;
      printf("draw-argbuf: %s: and it is the picture the texel pattern names -- texel "
             "(1,1) of texture 0 sampled, texel (2,3) of texture 0 fetched, 7.5 out of "
             "the buffer, and texture 1 filtered between two of its texels\n",
             arith ? "ok" : "FAIL");
      failed += !arith;
      checks++;

      /* All four different, which is what makes every comparison below worth anything:
       * a picture whose channels agree would read the same with two members exchanged.
       */
      bool distinct = through.c[0] != through.c[1] && through.c[0] != through.c[2] &&
                      through.c[0] != through.c[3] && through.c[1] != through.c[2] &&
                      through.c[1] != through.c[3] && through.c[2] != through.c[3];
      printf("draw-argbuf: %s: the four channels are four different numbers (%s)\n",
             distinct ? "ok" : "FAIL",
             distinct ? "so exchanging any two members has to show"
                      : "two channels agree, so a swap would be invisible");
      failed += !distinct;
      checks++;
   }

   /*
    * THE SAMPLERS EXCHANGED, at offset 64 of the same buffer -- a second encoding in one
    * buffer, which is what the records are keyed by offset for.
    *
    * This is the gap argbufcheck.py could not close: air_run binds one sampler state to
    * every slot, so nothing there could tell sampler member 1 from member 4. Here the
    * two states differ and the picture has both in it, so exchanging the records must
    * exchange R and A and change nothing else.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 64, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 64, 1, samp[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 64, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 64, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 64, 4, samp[0], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 64: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at64[1] = { { argbuf, 64 } };
   struct rwa_draw swapped_samplers = encoded_draw;
   swapped_samplers.fragment_buffer_table = at64;

   struct argbuf_pixel got;
   if (!argbuf_draw_pixel(setpipe, &swapped_samplers, attachment, &got, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the samplers exchanged failed: %s\n", why);
      failed++;
      checks++;
   } else {
      argbuf_print_pixel("samplers exchanged:", &got);
      /* R is texture 0 through what was the linear sampler, so it is the linear number
       * of slot 0, which is G minus its 1000; G is texture 1 through nearest, which is
       * R plus 1000. The FETCH does not move at all, because it names no sampler -- and
       * that is the half of this that says a sampler record cannot disturb a texture
       * binding.
       */
      bool as_named = got.c[0] == through.c[1] - 1000.0f &&
                      got.c[1] == through.c[0] + 1000.0f &&
                      got.c[2] == through.c[2] && got.c[3] == through.c[3];
      printf("draw-argbuf: %s: exchanging sampler members 1 and 4 exchanges the two "
             "FILTERS and leaves the fetch alone -- so a sampler member is bound at ITS "
             "OWN index\n", as_named ? "ok" : "FAIL");
      failed += !as_named;
      checks++;
   }

   /*
    * THE TEXTURES EXCHANGED, at offset 128. The other gap: softpipe's compute path
    * answers every texture from the view bound at slot 0, so a texture member's index
    * was invisible to argbufcheck.py's dispatch. A fragment shader distinguishes them,
    * and the two textures differ by 1000 in channel 0, so the picture says which
    * texture answered which channel.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 128, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 128, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 128, 2, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 128, 3, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 128, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 128: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at128[1] = { { argbuf, 128 } };
   struct rwa_draw swapped_textures = encoded_draw;
   swapped_textures.fragment_buffer_table = at128;

   if (!argbuf_draw_pixel(setpipe, &swapped_textures, attachment, &got, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the textures exchanged failed: %s\n", why);
      failed++;
      checks++;
   } else {
      argbuf_print_pixel("textures exchanged:", &got);
      /* Channel 0 of texel (x, y) of slot s is 100 + x + 1000 * s, so exchanging the two
       * textures moves exactly 1000 through every channel that names one: R and A gain
       * it (they were slot 0 and are now slot 1) and G loses it. The buffer does not
       * move. This picture is also unlike the samplers-exchanged one above in A, so the
       * two mistakes cannot be confused for each other.
       */
      bool as_named = got.c[0] == through.c[0] + 1000.0f &&
                      got.c[1] == through.c[1] - 1000.0f &&
                      got.c[2] == through.c[2] &&
                      got.c[3] == through.c[3] + 1000.0f;
      printf("draw-argbuf: %s: exchanging texture members 2 and 3 moves 1000 through "
             "every channel that names a texture, the sampled and the fetched alike -- "
             "so a texture member is bound at ITS OWN index\n",
             as_named ? "ok" : "FAIL");
      failed += !as_named;
      checks++;
   }

   /*
    * THE BUFFER MEMBER AT ANOTHER OFFSET, at encoding offset 192. Metal's own `offset:`
    * on -setBuffer:offset:atIndex:, which is the one buffer offset this contract takes
    * at all, and the only channel that may move is B.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 192, 0, values, 64, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 192, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 192, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 192, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 192, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 192: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at192[1] = { { argbuf, 192 } };
   struct rwa_draw moved_buffer = encoded_draw;
   moved_buffer.fragment_buffer_table = at192;

   if (!argbuf_draw_pixel(setpipe, &moved_buffer, attachment, &got, why, sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the buffer member at offset 64 failed: "
             "%s\n", why);
      failed++;
      checks++;
   } else {
      argbuf_print_pixel("buffer member at offset 64:", &got);
      bool as_named = got.c[2] == 9.25f && through.c[2] == 7.5f &&
                      got.c[0] == through.c[0] && got.c[1] == through.c[1] &&
                      got.c[3] == through.c[3];
      printf("draw-argbuf: %s: the buffer member's own offset reaches the GPU (B is "
             "9.25 against 7.5) and no other channel moves\n",
             as_named ? "ok" : "FAIL");
      failed += !as_named;
      checks++;
   }

   /* And all four encodings are still there, in one buffer, each with its five members:
    * the draws above each found their own and none of them cleared another.
    */
   bool kept = rwa_argbuf_encoded_members(argbuf, 0) == 5 &&
               rwa_argbuf_encoded_members(argbuf, 64) == 5 &&
               rwa_argbuf_encoded_members(argbuf, 128) == 5 &&
               rwa_argbuf_encoded_members(argbuf, 192) == 5;
   printf("draw-argbuf: %s: one buffer holds four encodings at four offsets, five "
          "members each\n", kept ? "ok" : "FAIL");
   failed += !kept;
   checks++;

   /*
    * AN OFFSET THIS GPU CANNOT FETCH FROM, served and not refused -- 4 bytes, which is
    * legal in Metal and a multiple of neither softpipe's 16 nor a6xx's 64. The library
    * binds it as a `user_buffer`, which the back end uploads itself, and the only channel
    * that may move is B: 3.25 is the float four bytes into the buffer.
    */
   if (rwa_argbuf_encode_buffer(argbuf, 384, 0, values, 4, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 384, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 384, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 384, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 384, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 384: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at384[1] = { { argbuf, 384 } };
   struct rwa_draw unaligned_member = encoded_draw;
   unaligned_member.fragment_buffer_table = at384;

   if (!argbuf_draw_pixel(setpipe, &unaligned_member, attachment, &got, why,
                          sizeof(why))) {
      printf("draw-argbuf: FAIL: the draw with the buffer member at offset 4 failed: "
             "%s\n", why);
      failed++;
   } else {
      argbuf_print_pixel("buffer member at offset 4:", &got);
      bool as_named = got.c[2] == 3.25f && got.c[0] == through.c[0] &&
                      got.c[1] == through.c[1] && got.c[3] == through.c[3];
      printf("draw-argbuf: %s: an offset this GPU cannot fetch from is uploaded instead "
             "of refused (B is 3.25, the float four bytes in)\n",
             as_named ? "ok" : "FAIL");
      failed += !as_named;
   }
   checks++;

   /*
    * AND THE SAME TWO OFFSETS ON A DIRECT BUFFER ARGUMENT, which is the window server's
    * own case rather than an argument buffer's: 55 bindings of one boot arrived as
    * "fragment buffer 2 at offset 1164" and every one of them was a refused binding
    * before this. The direct pipeline is drawn with its one `constant` argument bound at
    * 64 (which this GPU can fetch from) and at 4 (which it cannot), and the answer must
    * be the same two floats the member path gave.
    */
   {
      struct rwa_buffer_bind direct_table[1];
      struct rwa_draw moved_direct = direct_draw;
      static const struct { size_t offset; float want; const char *what; } cases[] = {
         { 64, 9.25f, "a direct buffer argument at offset 64 (aligned: no copy)" },
         { 4, 3.25f, "a direct buffer argument at offset 4 (uploaded)" },
         { 1164, 5.75f, "a direct buffer argument at offset 1164, which is the window "
                        "server's own (uploaded)" },
      };

      for (unsigned i = 0; i < COUNT_OF(cases); i++) {
         direct_table[0] = (struct rwa_buffer_bind){ values, cases[i].offset };
         moved_direct.fragment_buffer_table = direct_table;
         moved_direct.fragment_buffer_table_count = 1;
         if (!argbuf_draw_pixel(directpipe, &moved_direct, attachment, &got, why,
                                sizeof(why))) {
            printf("draw-argbuf: FAIL: %s: %s\n", cases[i].what, why);
            failed++;
         } else {
            bool as_named = got.c[2] == cases[i].want &&
                            got.c[0] == reference.c[0] && got.c[1] == reference.c[1] &&
                            got.c[3] == reference.c[3];
            printf("draw-argbuf: %s: %s reads %.9g and moves nothing else\n",
                   as_named ? "ok" : "FAIL", cases[i].what, (double)got.c[2]);
            failed += !as_named;
         }
         checks++;
      }
   }

   /*
    * THE REFUSALS. Each is a draw that must come back with the sentence, and the test is
    * on the WORDS and not merely on the failure: a refusal for another reason would be a
    * pass for the wrong reason, which this project has paid for before.
    */
   struct {
      const char *what;
      const char *words;
      struct rwa_draw draw;
   } refusal[6];
   unsigned nrefusal = 0;

   /* 1. an argument buffer nobody encoded -- the refusal this feature is mostly made of */
   struct rwa_buffer_bind at_unencoded[1] = { { unencoded, 0 } };
   refusal[nrefusal].what = "an argument buffer nobody encoded";
   refusal[nrefusal].words = "was never filled through an MTLArgumentEncoder";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = at_unencoded;
   nrefusal++;

   /* 2. nothing bound at the set's Metal index at all */
   refusal[nrefusal].what = "no buffer at the set's own index";
   refusal[nrefusal].words = "nothing is bound there";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = NULL;
   refusal[nrefusal].draw.fragment_buffer_table_count = 0;
   nrefusal++;

   /* 3. an encoding with one member missing: id 3 is never set at offset 256 */
   if (rwa_argbuf_encode_buffer(argbuf, 256, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 256, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 256, 2, tex[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 256, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 256: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at256[1] = { { argbuf, 256 } };
   refusal[nrefusal].what = "a member nobody set";
   refusal[nrefusal].words = "never set on the encoder";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = at256;
   nrefusal++;

   /* 4. a member set with the wrong call: a buffer where the shader declares a texture */
   if (rwa_argbuf_encode_buffer(argbuf, 320, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 320, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_buffer(argbuf, 320, 2, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 320, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 320, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 320: %s\n", why);
      return 1;
   }
   struct rwa_buffer_bind at320[1] = { { argbuf, 320 } };
   refusal[nrefusal].what = "a buffer encoded where a texture is declared";
   refusal[nrefusal].words = "and a buffer was encoded there";
   refusal[nrefusal].draw = encoded_draw;
   refusal[nrefusal].draw.fragment_buffer_table = at320;
   nrefusal++;

   for (unsigned i = 0; i < nrefusal; i++) {
      why[0] = '\0';
      int rc = rwa_render_draw(setpipe, &refusal[i].draw, why, sizeof(why));
      bool named = rc != 0 && strstr(why, refusal[i].words) != NULL;

      printf("draw-argbuf: %s: %s -- %s\n", named ? "ok" : "FAIL", refusal[i].what,
             rc == 0 ? "DREW A PICTURE instead of refusing" : why);
      failed += !named;
      checks++;
   }

   /*
    * And the last one, which has to be last because it destroys a resource: a texture
    * encoded and then destroyed. Metal would leave a stale handle for the GPU to read;
    * here the record holds a pointer this library would dereference, so the destroy
    * scrubs it and the draw names the member.
    */
   struct rwa_texture *doomed = rwa_texture_create(device, &(struct rwa_texture_shape){
      .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
      .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
      .sample_count = 1, .usage = MTL_USAGE_SHADER_READ }, why, sizeof(why));
   if (!doomed) {
      printf("draw-argbuf: rwa_texture_create (the doomed texture): %s\n", why);
      return 1;
   }
   if (rwa_argbuf_encode_buffer(argbuf, 448, 0, values, 0, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 448, 1, samp[0], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 448, 2, doomed, why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_texture(argbuf, 448, 3, tex[1], why, sizeof(why)) != 0 ||
       rwa_argbuf_encode_sampler(argbuf, 448, 4, samp[1], why, sizeof(why)) != 0) {
      printf("draw-argbuf: encoding at offset 448: %s\n", why);
      return 1;
   }
   rwa_texture_destroy(doomed);
   struct rwa_buffer_bind at448[1] = { { argbuf, 448 } };
   struct rwa_draw after_destroy = encoded_draw;
   after_destroy.fragment_buffer_table = at448;
   why[0] = '\0';
   int rc = rwa_render_draw(setpipe, &after_destroy, why, sizeof(why));
   bool named = rc != 0 && strstr(why, "has been destroyed since") != NULL;
   printf("draw-argbuf: %s: a texture destroyed after it was encoded -- %s\n",
          named ? "ok" : "FAIL",
          rc == 0 ? "DREW A PICTURE instead of refusing" : why);
   failed += !named;
   checks++;

   /* And the scrub took out exactly one member: the encoding at offset 0 still draws
    * what it drew before, with the texture that was NOT destroyed. A scrub that cleared
    * too much would refuse this, and one that cleared too little would have crashed
    * above.
    */
   if (!argbuf_draw_pixel(setpipe, &encoded_draw, attachment, &got, why, sizeof(why))) {
      printf("draw-argbuf: FAIL: the first encoding no longer draws: %s\n", why);
      failed++;
   } else {
      bool unchanged = argbuf_pixel_eq(&got, &through);
      printf("draw-argbuf: %s: the other encodings are untouched by that destroy\n",
             unchanged ? "ok" : "FAIL");
      failed += !unchanged;
   }
   checks++;

   printf("draw-argbuf: %d of %d checks failed\n", failed, checks);

   rwa_render_destroy(setpipe);
   rwa_render_destroy(directpipe);
   rwa_buffer_destroy(argbuf);
   rwa_buffer_destroy(unencoded);
   rwa_buffer_destroy(values);
   for (unsigned i = 0; i < 2; i++) {
      rwa_texture_destroy(tex[i]);
      rwa_sampler_destroy(samp[i]);
   }
   rwa_texture_destroy(attachment);
   rwa_device_close(device);
   return failed ? 1 : 0;
}

/*
 * Metal's four packed 16-bit colour formats, measured rather than read: which FIELD of
 * the 16-bit word becomes which CHANNEL.
 *
 * The window server asks for one of them -- MTLPixelFormatBGR5A1Unorm, 43 -- twice a
 * boot for the home screen, and the library declined all four until now because the
 * question below had no answer that was not a guess.
 *
 * THE QUESTION IS ONE BIT OF INFORMATION. Gallium's side is mechanical: on a
 * little-endian host the first channel of a packed name sits at the LEAST significant
 * bits, which the generated unpackers in u_format_table.c state in so many words.
 * Metal's side is whether MTLPixelFormat lists a packed format's channels from the
 * least significant end or the most -- and Gallium happens to carry BOTH spellings of
 * each of these four, so the two readings are both expressible and they EXCHANGE RED
 * AND BLUE. air_metal.c's format table says which reading was taken and why.
 *
 * WHAT THIS MEASURES AND WHAT IT CANNOT. It writes a word with exactly one field set,
 * draws a shader that samples it, and reads which channel came back as 1. That decides
 * everything on this side of the contract: whether the pipe_format this library chose
 * really has the fields the reading claims, and whether the driver's texture unit reads
 * them as it says. It CANNOT decide Apple's convention, because nothing on this machine
 * states the bit positions -- Apple's MTLPixelFormat.h carries no comment on them and
 * Metal.framework's binary carries the names and the values and no layout. So a clean
 * run here means "the reading is implemented exactly", and the reading itself is
 * settled by a boot: red and blue exchanged in the window server's own surfaces is a
 * blue clock on an orange ground, not a subtle shift.
 *
 * Every texel of the probe texture holds the same word, so no coordinate, no filter and
 * no addressing can change the answer. A unorm field of all ones is exactly 1.0 and a
 * field of zeros exactly 0.0, so the expected pixel has no rounding in it.
 *
 *   air_metal_test --pixel-formats FILE.metallib FUNCTION
 */
static int
run_pixel_formats(const char *path, const char *func)
{
   struct packed16 {
      unsigned mtl;
      const char *name;
      const char *taken;      /* the pipe_format this library chose */
      const char *other;      /* ... and the one the other reading would have chosen */
      unsigned shift[4];      /* r, g, b, a: where the field starts */
      unsigned width[4];      /* ... and how wide it is; 0 means the format has none */
   };
   static const struct packed16 formats[] = {
      { 40, "MTLPixelFormatB5G6R5Unorm", "PIPE_FORMAT_R5G6B5_UNORM",
        "PIPE_FORMAT_B5G6R5_UNORM", { 0, 5, 11, 0 }, { 5, 6, 5, 0 } },
      { 41, "MTLPixelFormatA1BGR5Unorm", "PIPE_FORMAT_A1R5G5B5_UNORM",
        "PIPE_FORMAT_A1B5G5R5_UNORM", { 1, 6, 11, 0 }, { 5, 5, 5, 1 } },
      { 42, "MTLPixelFormatABGR4Unorm", "PIPE_FORMAT_A4R4G4B4_UNORM",
        "PIPE_FORMAT_A4B4G4R4_UNORM", { 4, 8, 12, 0 }, { 4, 4, 4, 4 } },
      { 43, "MTLPixelFormatBGR5A1Unorm", "PIPE_FORMAT_R5G5B5A1_UNORM",
        "PIPE_FORMAT_B5G5R5A1_UNORM", { 0, 5, 10, 15 }, { 5, 5, 5, 1 } },
   };
   static const char *const channel[4] = { "red", "green", "blue", "alpha" };

   /* The library serves these by default now; RWA_PACKED16=0 takes them out of the
    * picture, and this measurement is about the table and not about that switch, so it
    * asks for them whatever the environment says.
    */
   setenv("RWA_PACKED16", "1", 1);

   unsigned cases = 0, exact = 0, wrong = 0, failed_run = 0;
   for (unsigned f = 0; f < COUNT_OF(formats); f++) {
      const struct packed16 *p = &formats[f];
      for (unsigned c = 0; c < 4; c++) {
         if (!p->width[c])
            continue;
         unsigned word = ((1u << p->width[c]) - 1u) << p->shift[c];
         /* what the reading says that word must sample as: the one channel whose field
          * is set comes back 1, every other channel the format HAS comes back 0, and a
          * channel the format does not have is 1 for alpha and 0 otherwise -- which is
          * util_format's own rule for an absent channel and Metal's for a format with
          * no alpha.
          */
         float want[4];
         for (unsigned k = 0; k < 4; k++)
            want[k] = !p->width[k] ? (k == 3 ? 1.0f : 0.0f) : (k == c ? 1.0f : 0.0f);

         char texspec[32];
         snprintf(texspec, sizeof(texspec), "%u", p->mtl);
         probe_fill16 = word;
         probe_fill16_given = true;
         memset(last_pixel, 0, sizeof(last_pixel));
         printf("air_metal_test: --pixel-formats %s, the %u-bit %s field at bit %u, "
                "word 0x%04x\n", p->name, p->width[c], channel[c], p->shift[c], word);
         int rc = run_draw(path, NULL, func, "125", NULL, texspec);
         cases++;
         if (rc != 0) {
            printf("pixfmt: FAIL  %s %s field: the draw did not happen, so this case "
                   "measured nothing\n", p->name, channel[c]);
            failed_run++;
            continue;
         }
         bool ok = true;
         for (unsigned k = 0; k < 4; k++)
            ok = ok && last_pixel[k] == want[k];
         printf("pixfmt: %s  %s word 0x%04x -> %g %g %g %g (want %g %g %g %g, which is "
                "%s read as %s)\n", ok ? "ok  " : "WRONG", p->name, word,
                (double)last_pixel[0], (double)last_pixel[1], (double)last_pixel[2],
                (double)last_pixel[3], (double)want[0], (double)want[1],
                (double)want[2], (double)want[3], channel[c], p->taken);
         if (ok) {
            exact++;
         } else {
            wrong++;
            printf("pixfmt:        if %s came back 1 instead, the other reading is the "
                   "right one and this format wants %s\n",
                   c == 0 ? "blue" : c == 2 ? "red" : channel[c], p->other);
         }

         /* AND THE OTHER DIRECTION. The run above sampled the word; this one renders
          * the result back INTO a target of the same format and reads the raw 16-bit
          * word out. The two together are the round trip, and the word that comes back
          * must be the word that went in: a write that placed the fields somewhere else
          * than the read takes them from would come back as a different word, which
          * four floats could never show -- they would have gone out through the same
          * unpacking the write used.
          *
          * The alpha needs no special case. A format without one samples as 1 and the
          * target has nowhere to put it; a format with one carries whatever the probe
          * word set, which for a colour probe is 0 and for the alpha probe is 1.
          */
         probe_fill16 = word;
         probe_fill16_given = true;
         last_word = ~0u;
         rc = run_draw(path, NULL, func, texspec, NULL, texspec);
         cases++;
         if (rc != 0 || last_word == ~0u) {
            printf("pixfmt: FAIL  %s %s field: the round trip did not draw into a "
                   "%s target\n", p->name, channel[c], p->name);
            failed_run++;
         } else if (last_word != word) {
            printf("pixfmt: WRONG %s round trip: 0x%04x went in and 0x%04x came out, so "
                   "the write does not put the fields where the read takes them\n",
                   p->name, word, last_word);
            wrong++;
         } else {
            printf("pixfmt: ok    %s round trip: 0x%04x in, 0x%04x out -- the write and "
                   "the read agree on where every field is\n", p->name, word, last_word);
            exact++;
         }
      }
   }
   probe_fill16_given = false;

   printf("\npixfmt: %u probes over %u packed 16-bit formats: %u put the field in the "
          "channel the reading says, %u did not, %u could not be drawn\n", cases,
          (unsigned)COUNT_OF(formats), exact, wrong, failed_run);
   printf("pixfmt: every texel held the same word, so no coordinate, filter or address "
          "mode could have changed which channel answered\n");
   printf("pixfmt: a field of all ones is exactly 1.0 and a field of zeros exactly 0.0, "
          "so there is no rounding in these expectations\n");
   printf("pixfmt: this does NOT settle Apple's convention -- nothing on this machine "
          "states the bit positions -- it settles that the pipe_format chosen for each "
          "name has the fields that reading claims. The reading is settled by a boot: "
          "the other one exchanges red and blue\n");
   return (wrong || failed_run || !exact) ? 1 : 0;
}

/*
 * ===== --draw-blend: colour blending, against the arithmetic and against the ways of
 * ===== being wrong.
 *
 * WHY THIS MODE EXISTS. The contract carried no blend state at all until now, so every
 * pipeline this library made was opaque -- `.rt[0].colormask = PIPE_MASK_RGBA` and
 * nothing else -- and the window server's own compositor draws premultiplied-alpha
 * layers through it. A transparent-black label layer therefore landed as OPAQUE BLACK:
 * the black box behind every icon name on the home screen. That is the failure this
 * mode is built to make impossible to ship again, and it is a failure with a particular
 * shape -- nothing refused, nothing crashed, a picture came out wrong.
 *
 * A PASS HERE IS NOT "IT BLENDED". Blending at all is easy to achieve by accident, and
 * four plausible mistakes all produce a picture that looks blended:
 *
 *   - the source and destination factors exchanged
 *   - premultiplied alpha read as straight alpha (source factor One read as SourceAlpha)
 *   - the alpha half of the equation taken from the RGB half, or the other way round
 *   - the enumerations CAST instead of mapped: MTLBlendFactor and pipe_blendfactor agree
 *     on two of nineteen values, and MTLColorWriteMask's bits run backwards from
 *     Gallium's colormask
 *
 * So every case below computes, besides the correct answer, WHAT EACH OF THOSE MISTAKES
 * WOULD HAVE PRODUCED, prints them side by side, and says which of them the case can
 * tell apart. A case that cannot distinguish a mistake is counted as not distinguishing
 * it, and the run FAILS if any mistake is distinguished by no case at all -- a test that
 * cannot fail for the right reason is the thing this project has been bitten by before
 * (docs/air.md: "ask what else would have given the same answer").
 *
 * THE NUMBERS. Source (0.25, 0.5, 0.125, 0.25) over destination (0.5, 0.25, 0.75, 0.5),
 * all dyadic rationals, so every product, sum and difference in these equations is EXACT
 * in binary32 and the comparison is `==` and not a tolerance. The source alpha is 0.25
 * and not 0.5 deliberately: with alpha 0.5 the source-over equation is symmetrical in
 * the alpha channel and "the factors exchanged" would produce the SAME alpha as the
 * correct state. Nothing in these numbers is round.
 *
 * TWO ATTACHMENT FORMATS, for two different questions:
 *
 *   - MTLPixelFormatRGBA32Float (125) for the arithmetic. softpipe does not clamp a
 *     float attachment (sp_quad_blend.c sets its clamp flag from
 *     channel[0].normalized), so a Subtract that goes negative comes back negative
 *     instead of being hidden at zero -- which is what tells Subtract from
 *     ReverseSubtract.
 *   - MTLPixelFormatBGRA8Unorm (80) for the compositor's own case, because that is the
 *     format the home screen is composited in and an eight-bit round trip is a different
 *     path: softpipe blends in float in its tile cache and converts on write-back with
 *     float_to_ubyte, whose rounding is a bit trick and NOT (int)(f * 255 + 0.5f) --
 *     they disagree at 128 of the 255 tie points. The expectation here replicates that
 *     function rather than rounding its own way, and the destination is opaque WHITE on
 *     purpose: with every destination channel 255, source-over gives
 *     src + (255 - alpha)/255, which is an exact multiple of 1/255 and so an exact byte.
 *
 * WHAT IT CANNOT DO. It runs on softpipe, so it says nothing about the Adreno's blender
 * -- in particular nothing about Min or Max with a factor other than One (refused by
 * name: Metal's equation multiplies by the factors first and GL ignores them, and the
 * two back ends behind this library do different things), nothing about dither (a6xx
 * honours the bit and softpipe ignores it; this library leaves it clear), and nothing
 * about whether Apple's enumerations really hold the values the tables claim. That last
 * one is settled by the SDK header and by Metal.framework's own code, and in the end by
 * a photograph of the home screen.
 *
 *   air_metal_test --draw-blend FILE.metallib FUNCTION
 */

enum {
   /* MTLBlendFactor, MTLBlendOperation and MTLColorWriteMask, Apple's own numbers */
   BF_ZERO = 0, BF_ONE = 1, BF_SRC_COLOR = 2, BF_INV_SRC_COLOR = 3,
   BF_SRC_ALPHA = 4, BF_INV_SRC_ALPHA = 5, BF_DST_COLOR = 6, BF_INV_DST_COLOR = 7,
   BF_DST_ALPHA = 8, BF_INV_DST_ALPHA = 9, BF_SRC_ALPHA_SAT = 10,
   BF_BLEND_COLOR = 11, BF_SRC1_COLOR = 15, BF_PAST_END = 19,
   BO_ADD = 0, BO_SUB = 1, BO_RSUB = 2, BO_MIN = 3, BO_MAX = 4, BO_PAST_END = 5,
   CWM_NONE = 0x0, CWM_A = 0x1, CWM_B = 0x2, CWM_G = 0x4, CWM_R = 0x8,
   CWM_GB = 0x6, CWM_RA = 0x9, CWM_GBA = 0x7, CWM_RGB = 0xe, CWM_ALL = 0xf,
   MTL_BGRA8_UNORM = 80,
};

static float
blend_min(float a, float b)
{
   return a < b ? a : b;
}

/*
 * Metal's blend factor, as Apple's own equation defines it: the number multiplied into
 * one channel of one term. For channel 3 a "Color" factor means that term's alpha, which
 * is what every specification of this equation means by it.
 *
 * MTLBlendFactorSourceAlphaSaturated is written with GL's alpha rule -- 1 -- because
 * that is what BOTH back ends implement (softpipe says "multiply alpha by 1.0";
 * freedreno's a2xx driver says "hardware doesn't support SRC_ALPHA_SATURATE for alpha,
 * but it is equivalent to ONE"), and the library refuses that factor for the two alpha
 * fields for exactly that reason: Metal's one equation says min(As, 1 - Ad) there. The
 * alpha case below is therefore unreachable from a served pipeline and is written out so
 * the two readings can be printed next to each other.
 */
static float
metal_factor(unsigned f, unsigned channel, const float src[4], const float dst[4],
             bool *modelled)
{
   float sc = src[channel], dc = dst[channel];

   switch (f) {
   case BF_ZERO:          return 0.0f;
   case BF_ONE:           return 1.0f;
   case BF_SRC_COLOR:     return sc;
   case BF_INV_SRC_COLOR: return 1.0f - sc;
   case BF_SRC_ALPHA:     return src[3];
   case BF_INV_SRC_ALPHA: return 1.0f - src[3];
   case BF_DST_COLOR:     return dc;
   case BF_INV_DST_COLOR: return 1.0f - dc;
   case BF_DST_ALPHA:     return dst[3];
   case BF_INV_DST_ALPHA: return 1.0f - dst[3];
   case BF_SRC_ALPHA_SAT:
      return channel < 3 ? blend_min(src[3], 1.0f - dst[3]) : 1.0f;
   default:
      *modelled = false;
      return 0.0f;
   }
}

/*
 * The same question asked of GALLIUM's enumeration, which is what a CAST would have
 * produced. Not a hypothetical: these are the values pipe_blendfactor really holds
 * (src/util/blend.h -- the plain factors run 1..10 and their one-minus forms are those
 * numbers with 0x10 set, so ZERO is 0x11), and a Metal number landing in this table is
 * the mistake this library's table exists to prevent.
 *
 * The two constant-colour factors answer 0 and their inverses 1, because that is what a
 * Gallium context holds before anyone calls set_blend_color -- which nothing here does,
 * and which is why the library refuses Metal's BlendColor factors outright.
 */
static float
gallium_factor(unsigned pf, unsigned channel, const float src[4], const float dst[4],
               bool *modelled)
{
   float sc = src[channel], dc = dst[channel];

   switch (pf) {
   case 1:    return 1.0f;                                        /* ONE */
   case 2:    return sc;                                           /* SRC_COLOR */
   case 3:    return src[3];                                       /* SRC_ALPHA */
   case 4:    return dst[3];                                       /* DST_ALPHA */
   case 5:    return dc;                                           /* DST_COLOR */
   case 6:    return channel < 3 ? blend_min(src[3], 1.0f - dst[3]) : 1.0f;
   case 7:    return 0.0f;                                         /* CONST_COLOR */
   case 8:    return 0.0f;                                         /* CONST_ALPHA */
   case 0x11: return 0.0f;                                         /* ZERO */
   case 0x12: return 1.0f - sc;                                    /* INV_SRC_COLOR */
   case 0x13: return 1.0f - src[3];                                /* INV_SRC_ALPHA */
   case 0x14: return 1.0f - dst[3];                                /* INV_DST_ALPHA */
   case 0x15: return 1.0f - dc;                                    /* INV_DST_COLOR */
   case 0x17: return 1.0f;                                         /* INV_CONST_COLOR */
   case 0x18: return 1.0f;                                         /* INV_CONST_ALPHA */
   default:
      /* 0 and 0x0b..0x10 and 0x16 are no enumerator at all, and 9, 10, 0x19, 0x1a are
       * dual-source factors reading a second output that does not exist here. Both are
       * "this program cannot say what would have come out", which is a stronger thing to
       * report than a number.
       */
      *modelled = false;
      return 0.0f;
   }
}

static float
blend_equation(unsigned op, float s, float d, bool *modelled)
{
   switch (op) {
   case BO_ADD:  return s + d;
   case BO_SUB:  return s - d;
   case BO_RSUB: return d - s;
   case BO_MIN:  return blend_min(s, d);
   case BO_MAX:  return s > d ? s : d;
   default:
      *modelled = false;
      return 0.0f;
   }
}

/* The ways of being wrong this mode distinguishes, each one a real mistake someone could
 * make in the fifty lines of translation this is a test of. */
enum blend_mistake {
   MISTAKE_NONE = 0,              /* the correct answer */
   MISTAKE_FACTORS_EXCHANGED,
   MISTAKE_STRAIGHT_ALPHA,
   MISTAKE_ALPHA_FROM_RGB,
   MISTAKE_RGB_FROM_ALPHA,
   MISTAKE_FACTOR_CAST,
   MISTAKE_MASK_CAST,
   MISTAKE_NOT_BLENDED,
   MISTAKE_COUNT,
};

static const char *const mistake_name[MISTAKE_COUNT] = {
   "the mapping as written",
   "the source and destination factors exchanged",
   "premultiplied alpha read as straight alpha (source factor One as SourceAlpha)",
   "the alpha operation and factors taken from the RGB ones",
   "the RGB operation and factors taken from the alpha ones",
   "MTLBlendFactor CAST to pipe_blendfactor instead of mapped",
   "MTLColorWriteMask CAST to a Gallium colormask instead of reversed",
   "not blended at all: the source replacing the destination, which is what this "
      "library did until now",
};

/*
 * One case's answer under one reading. `clamp01` is the attachment's own rule: softpipe
 * clamps a normalised format at every step and does not clamp a float one.
 */
static void
blend_answer(enum blend_mistake how, const struct rwa_color_blend *b, const float src[4],
             const float dst[4], bool clamp01, float out[4], bool *modelled)
{
   struct rwa_color_blend c = *b;

   *modelled = true;

   switch (how) {
   case MISTAKE_FACTORS_EXCHANGED:
      c.source_rgb_blend_factor = b->destination_rgb_blend_factor;
      c.destination_rgb_blend_factor = b->source_rgb_blend_factor;
      c.source_alpha_blend_factor = b->destination_alpha_blend_factor;
      c.destination_alpha_blend_factor = b->source_alpha_blend_factor;
      break;
   case MISTAKE_STRAIGHT_ALPHA:
      if (c.source_rgb_blend_factor == BF_ONE)
         c.source_rgb_blend_factor = BF_SRC_ALPHA;
      if (c.source_alpha_blend_factor == BF_ONE)
         c.source_alpha_blend_factor = BF_SRC_ALPHA;
      break;
   case MISTAKE_ALPHA_FROM_RGB:
      c.alpha_blend_operation = b->rgb_blend_operation;
      c.source_alpha_blend_factor = b->source_rgb_blend_factor;
      c.destination_alpha_blend_factor = b->destination_rgb_blend_factor;
      break;
   case MISTAKE_RGB_FROM_ALPHA:
      c.rgb_blend_operation = b->alpha_blend_operation;
      c.source_rgb_blend_factor = b->source_alpha_blend_factor;
      c.destination_rgb_blend_factor = b->destination_alpha_blend_factor;
      break;
   case MISTAKE_NOT_BLENDED:
      c.blending_enabled = 0;
      c.write_mask = CWM_ALL;
      break;
   default:
      break;
   }

   for (unsigned ch = 0; ch < 4; ch++) {
      unsigned sf = ch < 3 ? c.source_rgb_blend_factor : c.source_alpha_blend_factor;
      unsigned df = ch < 3 ? c.destination_rgb_blend_factor
                           : c.destination_alpha_blend_factor;
      unsigned op = ch < 3 ? c.rgb_blend_operation : c.alpha_blend_operation;
      bool written;
      float v;

      if (!c.blending_enabled) {
         v = src[ch];
      } else if (how == MISTAKE_FACTOR_CAST) {
         v = blend_equation(op, src[ch] * gallium_factor(sf, ch, src, dst, modelled),
                            dst[ch] * gallium_factor(df, ch, src, dst, modelled),
                            modelled);
      } else {
         v = blend_equation(op, src[ch] * metal_factor(sf, ch, src, dst, modelled),
                            dst[ch] * metal_factor(df, ch, src, dst, modelled),
                            modelled);
      }
      if (clamp01)
         v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);

      /* MTLColorWriteMask has Red at 1 << 3 and Alpha at 1 << 0, so channel ch is bit
       * 3 - ch. The cast mistake reads channel ch at bit ch, which is Gallium's order.
       */
      written = how == MISTAKE_MASK_CAST ? (c.write_mask & (1u << ch)) != 0
                                         : (c.write_mask & (1u << (3 - ch))) != 0;
      out[ch] = written ? v : dst[ch];
   }
}

/*
 * softpipe's own float-to-byte conversion, replicated bit for bit (src/util/u_math.h's
 * float_to_ubyte). It is NOT (int)(f * 255 + 0.5f) and it is not lrintf(f * 255): it is
 * a bit trick that rounds half to even, and the two disagree at 128 of the 255 tie
 * points. An expectation computed any other way would be a second rounding rule
 * disagreeing with the first, which is a test failing for a reason that has nothing to
 * do with blending.
 */
static uint8_t
expect_ubyte(float f)
{
   union { float f; uint32_t i; } t;

   if (!(f > 0.0f))
      return 0;
   if (f >= 1.0f)
       return 255;
   t.f = f * (255.0f / 256.0f) + 32768.0f;
   return (uint8_t)t.i;
}

static float
byte_as_float(uint8_t b)
{
   return (float)b * (1.0f / 255.0f);   /* u_math.h's ubyte_to_float */
}

struct blend_case {
   const char *what;
   unsigned attachment;          /* the colour attachment's MTLPixelFormat */
   unsigned attachments;         /* how many attachments the pipeline declares */
   unsigned blend_count;         /* color_blend_count passed; 0 is "no blend state" */
   struct rwa_color_blend b;
   bool must_refuse;             /* this case must be refused, and the sentence printed */
   /* attachment 1's entry, for the one case that has two and wants them to DIFFER.
    * Appended so that every row above can leave it out -- which is also why it is read
    * only when `blend_count` is 2 or more. */
   struct rwa_color_blend b1;
};

/* A served case, spelled out so the table reads as Metal's own descriptor. */
#define BLEND_ON(sr, dr, opr, sa, da, opa, mask)                                        \
   { .blending_enabled = 1, .rgb_blend_operation = (opr),                               \
     .alpha_blend_operation = (opa), .source_rgb_blend_factor = (sr),                   \
     .destination_rgb_blend_factor = (dr), .source_alpha_blend_factor = (sa),           \
     .destination_alpha_blend_factor = (da), .write_mask = (mask) }

static int
run_blend(const char *path, const char *func)
{
   /* The source and the destination: dyadic, asymmetrical, and the source alpha is not
    * 0.5 so that exchanging the factors changes the alpha channel too.
    */
   static const float src[4] = { 0.25f, 0.5f, 0.125f, 0.25f };
   static const float dst[4] = { 0.5f, 0.25f, 0.75f, 0.5f };
   /* and the eight-bit case's pair: an OPAQUE WHITE destination, which makes
    * source-over land on an exact multiple of 1/255 in every channel
    */
   static const uint8_t dst8[4] = { 255, 255, 255, 255 };          /* B, G, R, A */
   static const float src8[4] = { 32.0f / 255.0f, 64.0f / 255.0f, 16.0f / 255.0f,
                                  64.0f / 255.0f };

   static const struct blend_case cases[] = {
      /* The old path, unchanged: no blend state in the shape at all. */
      { "no blend state in the shape (color_blend_count 0)", MTL_RGBA32_FLOAT, 1, 0,
        { 0 }, false },
      { "blending disabled, writeMask All", MTL_RGBA32_FLOAT, 1, 1,
        { .write_mask = CWM_ALL }, false },
      /*
       * APPLE'S OWN UNTOUCHED DESCRIPTOR, which is the single most important row here:
       * 504 of the 1079 pipelines in this device's CoreAnimation archive arrive exactly
       * like this, because they blend inside their fragment shader and never touch the
       * descriptor. The word
       * -[MTLRenderPipelineColorAttachmentDescriptorInternal init] writes is 0x78001080:
       * blendingEnabled NO, both operations Add, source factors One, destination
       * factors Zero, writeMask All.
       *
       * EVERY ONE OF THOSE FIELDS IS NON-ZERO EXCEPT blendingEnabled. So a driver that
       * decided "this descriptor carries blend state because a field is set" would blend
       * these 504 twice, and this row is what catches that: the answer must be the
       * source, unchanged, exactly as the row above it.
       */
      { "Apple's untouched descriptor: One/Zero, Add, writeMask All, blending OFF -- the "
        "504-of-1079 case", MTL_RGBA32_FLOAT, 1, 1,
        { .blending_enabled = 0, .rgb_blend_operation = BO_ADD,
          .alpha_blend_operation = BO_ADD, .source_rgb_blend_factor = BF_ONE,
          .destination_rgb_blend_factor = BF_ZERO, .source_alpha_blend_factor = BF_ONE,
          .destination_alpha_blend_factor = BF_ZERO, .write_mask = CWM_ALL }, false },

      /* The compositor's own state, and the two things it is most often confused with. */
      { "premultiplied source-over (One, OneMinusSourceAlpha, Add) -- CoreAnimation's",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_ALL), false },
      { "straight alpha (SourceAlpha, OneMinusSourceAlpha, Add)", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BO_ADD, BF_SRC_ALPHA, BF_INV_SRC_ALPHA,
                 BO_ADD, CWM_ALL), false },
      { "source-over with the factors the other way round", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_INV_SRC_ALPHA, BF_ONE, BO_ADD, BF_INV_SRC_ALPHA, BF_ONE, BO_ADD,
                 CWM_ALL), false },

      /* The two halves of the equation, made to differ. */
      { "colour blended, alpha accumulated (rgb One/OneMinusSourceAlpha, alpha One/One)",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_ONE, BO_ADD, CWM_ALL),
        false },
      { "rgb Add and alpha Max, both One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_ADD, BF_ONE, BF_ONE, BO_MAX, CWM_ALL), false },

      /* The five operations. Min and Max take One and One, which is the only form this
       * library serves them in and the only one both back ends agree about.
       */
      { "Subtract, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_SUB, BF_ONE, BF_ONE, BO_SUB, CWM_ALL), false },
      { "ReverseSubtract, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_RSUB, BF_ONE, BF_ONE, BO_RSUB, CWM_ALL), false },
      { "Min, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_MIN, BF_ONE, BF_ONE, BO_MIN, CWM_ALL), false },
      { "Max, One/One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ONE, BO_MAX, BF_ONE, BF_ONE, BO_MAX, CWM_ALL), false },

      /* Rows of the factor table a cast gets wrong in a way no source-over case would
       * show: 2 and 3 are the only two a cast gets RIGHT or nearly so, 6 and 7 land on
       * SourceAlphaSaturated and CONST_COLOR, 8 and 9 on CONST_ALPHA and a dual-source
       * factor, and 0 lands on no enumerator at all.
       */
      { "modulate (DestinationColor, Zero)", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_DST_COLOR, BF_ZERO, BO_ADD, BF_DST_COLOR, BF_ZERO, BO_ADD, CWM_ALL),
        false },
      { "OneMinusDestinationColor and SourceColor", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_INV_DST_COLOR, BF_SRC_COLOR, BO_ADD, BF_INV_DST_COLOR, BF_SRC_COLOR,
                 BO_ADD, CWM_ALL), false },
      { "DestinationAlpha and OneMinusDestinationAlpha", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_DST_ALPHA, BF_INV_DST_ALPHA, BO_ADD, BF_DST_ALPHA, BF_INV_DST_ALPHA,
                 BO_ADD, CWM_ALL), false },
      { "OneMinusSourceColor and SourceColor", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_INV_SRC_COLOR, BF_SRC_COLOR, BO_ADD, BF_INV_SRC_COLOR, BF_SRC_COLOR,
                 BO_ADD, CWM_ALL), false },
      { "SourceAlphaSaturated for the colour, One for the alpha", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC_ALPHA_SAT, BF_ZERO, BO_ADD, BF_ONE, BF_ZERO, BO_ADD, CWM_ALL),
        false },

      /* THE WRITE MASKS, and the point of this group is the two that are NOT palindromes
       * under a bit reversal. A suite built out of All and None alone would pass with the
       * nibble backwards.
       */
      { "writeMask Red only (0x8), blending on", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_R), false },
      { "writeMask Alpha only (0x1), blending on", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_A), false },
      { "writeMask Red|Green|Blue (0xe): colour written, alpha kept", MTL_RGBA32_FLOAT, 1,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_RGB), false },
      { "writeMask Green|Blue|Alpha (0x7): red kept", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_GBA), false },
      { "writeMask Green|Blue (0x6), a palindrome a cast gets right", MTL_RGBA32_FLOAT, 1,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_GB), false },
      { "writeMask None (0x0): nothing written at all", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_NONE), false },

      /* The home screen's own format and own equation, in eight bits. */
      { "premultiplied source-over into MTLPixelFormatBGRA8Unorm, over opaque white",
        MTL_BGRA8_UNORM, 1, 1,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_ALL), false },

      /*
       * TWO ATTACHMENTS WHOSE BLEND STATES DIFFER, which must be SERVED and not refused:
       * it is the shape CoreAnimation itself builds. create_pipeline_state's
       * multiple-attachment loop sends `setWriteMask: 0` to every attachment that is not
       * the primary one and sends `setBlendingEnabled:` to none of them, so attachment 0
       * blends with every channel written and the rest write nothing at all. With
       * Gallium's independent_blend_enable clear, rt[0] would govern them all and
       * attachment 1 would be written -- so what this row checks is that the bit went
       * on. Both back ends have the capability (freedreno_screen.c and sp_screen.c each
       * set indep_blend_enable and indep_blend_func).
       */
      { "two attachments, one blending with writeMask All and one with writeMask None "
        "-- CoreAnimation's own multiple-attachment shape", MTL_RGBA32_FLOAT, 2, 2,
        BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                 CWM_ALL), false,
        { .blending_enabled = 0, .write_mask = CWM_NONE } },

      /* AND WHAT MUST BE REFUSED BY NAME. Every one of these would otherwise draw
       * something.
       */
      { "MTLBlendFactorBlendColor, whose colour is encoder state", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_BLEND_COLOR, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA,
                 BO_ADD, CWM_ALL), true },
      { "MTLBlendFactorSource1Color: dual-source, with no second output in the contract",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC1_COLOR, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA,
                 BO_ADD, CWM_ALL), true },
      { "a blend factor of 19, one past Metal's last", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_PAST_END, BF_ZERO, BO_ADD, BF_ONE, BF_ZERO, BO_ADD, CWM_ALL), true },
      { "a blend operation of 5, one past Metal's last", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ZERO, BO_PAST_END, BF_ONE, BF_ZERO, BO_ADD, CWM_ALL), true },
      { "a writeMask of 0x10, a fifth bit MTLColorWriteMask has not got",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ZERO, BO_ADD, BF_ONE, BF_ZERO, BO_ADD, 0x10), true },
      { "MTLBlendFactorSourceAlphaSaturated as the sourceAlphaBlendFactor",
        MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_ONE, BF_ZERO, BO_ADD, BF_SRC_ALPHA_SAT, BF_ZERO, BO_ADD, CWM_ALL),
        true },
      { "Min with a factor other than One", MTL_RGBA32_FLOAT, 1, 1,
        BLEND_ON(BF_SRC_ALPHA, BF_ONE, BO_MIN, BF_ONE, BF_ONE, BO_ADD, CWM_ALL), true },
      { "blending enabled on an MTLPixelFormatRGBA32Uint attachment", MTL_RGBA32_UINT, 1,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_ALL), true },
      { "blend state for one attachment on a pipeline that has two", MTL_RGBA32_FLOAT, 2,
        1, BLEND_ON(BF_ONE, BF_INV_SRC_ALPHA, BO_ADD, BF_ONE, BF_INV_SRC_ALPHA, BO_ADD,
                    CWM_ALL), true },
   };

   struct metallib lib = { 0 };
   if (!load_metallib(path, &lib))
      return 1;

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, func, name, sizeof(name), &bitcode, &bitcode_size))
      return 1;
   printf("air_metal_test: --draw-blend %s, %zu bytes of bitcode\n", name, bitcode_size);

   setenv("RWA_SOFTWARE", "1", 0);
   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: device is \"%s\"\n", rwa_device_name(device));
   printf("blend: source (%g %g %g %g) over destination (%g %g %g %g), and the eight-bit "
          "case is (%u %u %u)/255 alpha %u/255 over opaque white\n",
          (double)src[0], (double)src[1], (double)src[2], (double)src[3], (double)dst[0],
          (double)dst[1], (double)dst[2], (double)dst[3], 16u, 64u, 32u, 64u);

   unsigned served = 0, right = 0, wrong = 0, refused_right = 0, refused_wrong = 0;
   unsigned distinguishes[MISTAKE_COUNT] = { 0 };

   for (unsigned ci = 0; ci < COUNT_OF(cases); ci++) {
      const struct blend_case *c = &cases[ci];
      bool eight = c->attachment == MTL_BGRA8_UNORM;
      const float *s = eight ? src8 : src;
      float d[4];

      for (unsigned ch = 0; ch < 4; ch++)
         d[ch] = eight ? byte_as_float(dst8[ch == 0 ? 2 : ch == 2 ? 0 : ch]) : dst[ch];

      printf("\nblend: %s\n", c->what);

      /* The pipeline, which is where every refusal happens: blend state is compiled into
       * a Metal pipeline and so is checked when one is made, not at a draw.
       */
      struct rwa_render_shape shape = {
         .color_count = c->attachments, .sample_count = 1,
         .color_blend_count = c->blend_count,
      };
      for (unsigned i = 0; i < c->attachments; i++)
         shape.color_format[i] = c->attachment;
      if (c->blend_count)
         shape.color_blend[0] = c->b;
      if (c->blend_count >= 2)
         shape.color_blend[1] = c->b1;

      struct rwa_render_info info = { 0 };
      why[0] = '\0';
      struct rwa_render *render =
         rwa_render_create(device, NULL, 0, NULL, bitcode, bitcode_size, func, &shape,
                           &info, why, sizeof(why));
      if (c->must_refuse) {
         if (render) {
            printf("blend: NOT REFUSED  %s: the pipeline was made, so this would have "
                   "drawn something\n", c->what);
            refused_wrong++;
            rwa_render_destroy(render);
         } else {
            printf("blend: refused     %s\n", why);
            refused_right++;
         }
         continue;
      }
      if (!render) {
         printf("blend: FAIL  rwa_render_create: %s\n", why);
         wrong++;
         continue;
      }
      printf("blend: the library blends attachments 0x%x and writes 0x%08x (four Gallium "
             "bits per attachment, R 0x1 .. A 0x8)\n", info.blend_enabled_mask,
             info.color_write_masks);

      /* The attachment, pre-filled with the destination and NOT cleared by the draw:
       * clear_color NULL is MTLLoadActionLoad, so what the blender reads is what was
       * written here.
       */
      const unsigned origin[3] = { 0, 0, 0 }, extent[3] = { TEX, TEX, 1 };
      /* One per attachment the pipeline declares, all filled with the destination: a
       * draw must cover every attachment the pipeline was made for, which the library
       * refuses a mismatch of by name.
       */
      struct rwa_texture *color[2] = { 0 };
      union { float f[TEX * TEX * 4]; uint8_t b[TEX * TEX * 4]; } fill;
      for (unsigned t = 0; t < TEX * TEX; t++)
         for (unsigned ch = 0; ch < 4; ch++) {
            if (eight)
               fill.b[t * 4 + ch] = dst8[ch];
            else
               fill.f[t * 4 + ch] = dst[ch];
         }
      bool made = true;
      for (unsigned i = 0; i < c->attachments && i < COUNT_OF(color) && made; i++) {
         const struct rwa_texture_shape cts = {
            .pixel_format = c->attachment, .type = MTL_TEXTURE_TYPE_2D,
            .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
            .sample_count = 1,
            .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_RENDER_TARGET,
         };
         color[i] = rwa_texture_create(device, &cts, why, sizeof(why));
         made = color[i] != NULL &&
                rwa_texture_write(color[i], 0, 0, origin, extent, &fill, 0, why,
                                  sizeof(why)) == 0;
      }
      if (!made) {
         printf("blend: FAIL  making attachment %u: %s\n", c->attachments, why);
         for (unsigned i = 0; i < COUNT_OF(color); i++)
            rwa_texture_destroy(color[i]);
         rwa_render_destroy(render);
         wrong++;
         continue;
      }

      /* The source, which is a texture the fragment shader samples: every texel the same
       * value, so no coordinate, filter or address mode can change what the source
       * colour is. It is always RGBA32Float whatever the attachment is -- the source of a
       * blend is the shader's output and has nothing to do with the attachment's format.
       */
      struct rwa_texture *tex[8] = { 0 };
      struct rwa_sampler *samp[8] = { 0 };
      struct rwa_buffer *buf[16] = { 0 };
      unsigned ntex = info.fragment_textures < COUNT_OF(tex) ? info.fragment_textures
                                                             : (unsigned)COUNT_OF(tex);
      unsigned nsamp = info.fragment_samplers < COUNT_OF(samp) ? info.fragment_samplers
                                                                : (unsigned)COUNT_OF(samp);
      unsigned nbuf = info.fragment_buffers < COUNT_OF(buf) ? info.fragment_buffers
                                                             : (unsigned)COUNT_OF(buf);
      bool built = true;
      for (unsigned i = 0; i < ntex && built; i++) {
         const struct rwa_texture_shape ts = {
            .pixel_format = MTL_RGBA32_FLOAT, .type = MTL_TEXTURE_TYPE_2D,
            .width = TEX, .height = TEX, .depth = 1, .array_length = 1, .mip_levels = 1,
            .sample_count = 1, .usage = MTL_USAGE_SHADER_READ,
         };
         tex[i] = rwa_texture_create(device, &ts, why, sizeof(why));
         built = tex[i] != NULL;
         if (built) {
            float texels[TEX * TEX * 4];
            for (unsigned t = 0; t < TEX * TEX; t++)
               for (unsigned ch = 0; ch < 4; ch++)
                  texels[t * 4 + ch] = s[ch];
            built = rwa_texture_write(tex[i], 0, 0, origin, extent, texels, 0, why,
                                      sizeof(why)) == 0;
         }
      }
      for (unsigned i = 0; i < nsamp && built; i++) {
         const struct rwa_sampler_state st = {
            .address = { 0, 0, 0 }, .min_filter = 0, .mag_filter = 0, .mip_filter = 0,
            .compare_function = 0, .max_anisotropy = 1, .normalized_coordinates = 1,
            .lod_min = 0.0f, .lod_max = 65504.0f,
         };
         samp[i] = rwa_sampler_create(device, &st, why, sizeof(why));
         built = samp[i] != NULL;
      }
      for (unsigned i = 0; i < nbuf && built; i++) {
         buf[i] = rwa_buffer_create(device, 48, why, sizeof(why));
         built = buf[i] != NULL;
         if (built) {
            uint8_t *bits = rwa_buffer_map(buf[i]);
            for (unsigned k = 0; k < rwa_buffer_size(buf[i]); k++)
               bits[k] = pattern_byte(k);
         }
      }

      union { float f[TEX * TEX * 4]; uint8_t b[TEX * TEX * 4]; } got;
      memset(&got, 0, sizeof(got));
      if (built) {
         const struct rwa_draw draw = {
            .color = color, .color_count = c->attachments,
            .clear_color = NULL,           /* MTLLoadActionLoad: keep the destination */
            .fragment = { .buffers = buf, .buffer_count = nbuf, .textures = tex,
                          .texture_count = ntex, .samplers = samp,
                          .sampler_count = nsamp },
            .primitive = 4,                /* MTLPrimitiveTypeTriangleStrip */
            .vertex_start = 0, .vertex_count = 4,
         };
         if (rwa_render_draw(render, &draw, why, sizeof(why)) != 0) {
            printf("blend: FAIL  rwa_render_draw: %s\n", why);
            built = false;
         } else if (rwa_texture_read(color[0], 0, 0, origin, extent, &got, 0, why,
                                     sizeof(why)) != 0) {
            printf("blend: FAIL  rwa_texture_read: %s\n", why);
            built = false;
         }
      } else {
         printf("blend: FAIL  setting up the draw: %s\n", why);
      }

      for (unsigned i = 0; i < ntex; i++)
         rwa_texture_destroy(tex[i]);
      for (unsigned i = 0; i < nsamp; i++)
         rwa_sampler_destroy(samp[i]);
      for (unsigned i = 0; i < nbuf; i++)
         rwa_buffer_destroy(buf[i]);
      for (unsigned i = 0; i < COUNT_OF(color); i++)
         rwa_texture_destroy(color[i]);
      rwa_render_destroy(render);

      if (!built) {
         wrong++;
         continue;
      }
      served++;

      /*
       * The verdict, and every candidate answer printed beside it. `clamp01` is the
       * attachment's rule and not a choice made here: softpipe clamps a normalised
       * format at every step and leaves a float one alone.
       */
      struct rwa_color_blend eff = c->blend_count ? c->b : (struct rwa_color_blend){
         .write_mask = CWM_ALL };
      float want[MISTAKE_COUNT][4];
      bool modelled[MISTAKE_COUNT];
      for (unsigned m = 0; m < MISTAKE_COUNT; m++) {
         modelled[m] = true;
         blend_answer((enum blend_mistake)m, &eff, s, d, eight, want[m], &modelled[m]);
      }

      /* every texel, not one: a blend applied to some fragments and not others is a
       * thing this would otherwise miss entirely
       */
      unsigned differing_texels = 0;
      float measured[4];
      for (unsigned ch = 0; ch < 4; ch++)
         measured[ch] = eight ? byte_as_float(got.b[ch == 0 ? 2 : ch == 2 ? 0 : ch])
                              : got.f[ch];
      for (unsigned t = 0; t < TEX * TEX; t++)
         for (unsigned ch = 0; ch < 4; ch++) {
            bool same = eight ? got.b[t * 4 + ch] == got.b[ch]
                              : got.f[t * 4 + ch] == got.f[ch];
            differing_texels += same ? 0 : 1;
         }

      bool ok;
      if (eight) {
         /* in bytes, with softpipe's own rounding replicated for the expectation */
         ok = true;
         for (unsigned ch = 0; ch < 4; ch++) {
            unsigned mem = ch == 0 ? 2 : ch == 2 ? 0 : ch;   /* B G R A in memory */
            ok = ok && got.b[mem] == expect_ubyte(want[MISTAKE_NONE][ch]);
         }
         printf("blend: %s  got bytes B %u G %u R %u A %u, want B %u G %u R %u A %u\n",
                ok ? "ok  " : "WRONG", got.b[0], got.b[1], got.b[2], got.b[3],
                expect_ubyte(want[MISTAKE_NONE][2]),
                expect_ubyte(want[MISTAKE_NONE][1]),
                expect_ubyte(want[MISTAKE_NONE][0]),
                expect_ubyte(want[MISTAKE_NONE][3]));
      } else {
         ok = true;
         for (unsigned ch = 0; ch < 4; ch++)
            ok = ok && got.f[ch] == want[MISTAKE_NONE][ch];
         printf("blend: %s  got %g %g %g %g, want %g %g %g %g\n",
                ok ? "ok  " : "WRONG", (double)got.f[0], (double)got.f[1],
                (double)got.f[2], (double)got.f[3], (double)want[MISTAKE_NONE][0],
                (double)want[MISTAKE_NONE][1], (double)want[MISTAKE_NONE][2],
                (double)want[MISTAKE_NONE][3]);
      }
      if (differing_texels)
         printf("blend: and %u of the %u channels of this 4x4 differ from texel (0,0), "
                "so the blend was not uniform over the quad\n", differing_texels,
                TEX * TEX * 4);

      /* What each mistake would have drawn, and whether this case can tell it from the
       * right answer. A mistake that matches the right answer HERE is not a mistake this
       * case catches, and saying so is the difference between a suite and a number.
       */
      for (unsigned m = MISTAKE_NONE + 1; m < MISTAKE_COUNT; m++) {
         bool same = true;
         for (unsigned ch = 0; ch < 4; ch++)
            same = same && want[m][ch] == want[MISTAKE_NONE][ch];
         if (!modelled[m]) {
            printf("blend:        %-72s would not have drawn a number this program can "
                   "name (a Gallium enumerator that does not exist, or a dual-source "
                   "read)\n", mistake_name[m]);
            distinguishes[m]++;
            continue;
         }
         /* In the attachment's own units, and that matters: an eight-bit case measured
          * in bytes and priced in floats is two scales in one table, and two of these
          * candidates differ by less than 1/255 -- which is a distinction the bytes do
          * not have and the floats would claim.
          */
         if (eight) {
            same = true;
            for (unsigned ch = 0; ch < 4; ch++)
               same = same && expect_ubyte(want[m][ch]) ==
                                 expect_ubyte(want[MISTAKE_NONE][ch]);
            printf("blend:        %-72s %s bytes B %u G %u R %u A %u\n", mistake_name[m],
                   same ? "gives the SAME" : "would give", expect_ubyte(want[m][2]),
                   expect_ubyte(want[m][1]), expect_ubyte(want[m][0]),
                   expect_ubyte(want[m][3]));
         } else {
            printf("blend:        %-72s %s %g %g %g %g\n", mistake_name[m],
                   same ? "gives the SAME" : "would give", (double)want[m][0],
                   (double)want[m][1], (double)want[m][2], (double)want[m][3]);
         }
         if (!same) {
            distinguishes[m]++;
            /* and if the measurement matches a mistake, name it rather than leave the
             * reader to compare two rows of numbers
             */
            bool matches = true;
            for (unsigned ch = 0; ch < 4; ch++) {
               unsigned mem = ch == 0 ? 2 : ch == 2 ? 0 : ch;
               matches = matches && (eight ? got.b[mem] == expect_ubyte(want[m][ch])
                                           : measured[ch] == want[m][ch]);
            }
            if (!ok && matches)
               printf("blend:        ... AND THAT IS WHAT CAME BACK: %s\n",
                      mistake_name[m]);
         }
      }

      if (ok)
         right++;
      else
         wrong++;
   }

   rwa_device_close(device);

   printf("\nblend: %u cases drawn: %u gave the arithmetic exactly, %u did not; %u of %u "
          "cases that must be refused were refused by name, %u were not\n",
          served, right, wrong, refused_right,
          refused_right + refused_wrong, refused_wrong);
   printf("blend: the expectations are Metal's own equation -- source times its factor, "
          "the operation, destination times its factor -- and not this library's reading "
          "of it: every input is a dyadic rational, so the float cases are compared with "
          "== and the eight-bit case against softpipe's own float_to_ubyte\n");
   printf("blend: what each way of being wrong would have drawn, and whether any case "
          "tells it apart:\n");
   unsigned thin = 0;
   for (unsigned m = MISTAKE_NONE + 1; m < MISTAKE_COUNT; m++) {
      printf("blend:   %-72s %u of %u cases distinguish it\n", mistake_name[m],
             distinguishes[m], served);
      if (!distinguishes[m])
         thin++;
   }
   if (thin)
      printf("blend: %u way%s of being wrong that NO case here distinguishes, so a pass "
             "says nothing about %s\n", thin, thin == 1 ? "" : "s",
             thin == 1 ? "it" : "them");
   printf("blend: nothing here is measured on the Adreno. What this cannot reach: Min or "
          "Max with a factor other than One (refused by name -- Metal's equation "
          "multiplies by the factors and GL ignores them, and softpipe and fd6_blend.cc "
          "do different things with what survives), dither (a6xx honours the bit, "
          "softpipe ignores it, and this library leaves it clear), and whether Apple's "
          "enumerations hold the values these tables claim -- which the home screen's "
          "own photograph answers\n");
   return (wrong || refused_wrong || thin || !served) ? 1 : 0;
}

int
main(int argc, char **argv)
{
   /* --draw puts a fragment shader through rwa_render_*; without it the program
    * dispatches a kernel through rwa_compute_*, as it did before. The two are separate
    * because a fragment shader handed to rwa_compute_create is refused ("is a fragment
    * shader, and this makes compute pipelines"), which is the right answer and not a
    * test.
    */
   /* --textures asks what a library's functions declare and binds nothing, so it comes
    * first: it is the answer a caller needs BEFORE it can make the textures the two
    * modes below make.
    */
   if (argc >= 2 && !strcmp(argv[1], "--textures")) {
      if (argc < 3) {
         fprintf(stderr, "usage: air_metal_test --textures FILE.metallib [FUNCTION]\n");
         return 2;
      }
      return run_textures(argv[2], argc > 3 ? argv[3] : NULL);
   }

   /* --argbufs is the same kind of question as --textures and comes next to it: what
    * does a module declare that a caller has to allocate before it can bind anything.
    */
   if (argc >= 2 && !strcmp(argv[1], "--argbufs")) {
      if (argc < 3) {
         fprintf(stderr, "usage: air_metal_test --argbufs FILE [FUNCTION]\n");
         return 2;
      }
      return run_argbufs(argv[2], argc > 3 ? argv[3] : NULL);
   }

   /* --draw-argbuf is the runtime half of docs/air.md 8.2: the same picture through an
    * MTLArgumentEncoder's records as through direct arguments, and a named different
    * picture for each record that is deliberately wrong. Two modules, because the
    * measurement IS the comparison between them.
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-argbuf")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-argbuf SETMODULE DIRECTMODULE\n");
         return 2;
      }
      return run_draw_argbuf(argv[2], argv[3]);
   }

   /* --surface takes no .metallib at all: it is about memory, not about a shader. */
   if (argc >= 2 && !strcmp(argv[1], "--surface")) {
      return run_surface(argc > 2 ? atoi(argv[2]) : 1,
                         argc > 3 ? (unsigned) atoi(argv[3]) : 1080,
                         argc > 4 ? (unsigned) atoi(argv[4]) : 2400,
                         argc > 5 ? (unsigned) atoi(argv[5]) : 4352);
   }

   if (argc >= 2 && !strcmp(argv[1], "--draw")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw FILE.metallib FUNCTION "
                         "[MTLPixelFormat[,...] [MTLPixelFormat[:MTLTextureType][,...]]]"
                         "\n");
         return 2;
      }
      /* no vertex-buffer argument: the generated stage reads none. The texture one is
       * here so the generated stage can be the reference for a textured fragment
       * shader -- texturedFragment3DUInt wants a texture3d, and it is the comparison
       * that needs it rather than the stage.
       */
      return run_draw(argv[2], NULL, argv[3], argc > 4 ? argv[4] : NULL, NULL,
                      argc > 5 ? argv[5] : NULL);
   }

   /* --pixel-formats is the draw used as an instrument: the same shader sixteen times
    * over textures of Metal's four packed 16-bit formats, each holding one 16-bit word
    * with one field set, to measure which field became which channel.
    */
   /* --draw-blend: colour blending, with each way of being wrong priced beside the
    * right answer. Two arguments, like --pixel-formats: a container and a fragment
    * function whose output this program controls through the texture it samples.
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-blend")) {
      if (argc < 4) {
         fprintf(stderr, "usage: %s --draw-blend FILE.metallib FUNCTION\n", argv[0]);
         return 2;
      }
      return run_blend(argv[2], argv[3]);
   }

   if (argc >= 2 && !strcmp(argv[1], "--pixel-formats")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --pixel-formats FILE.metallib FUNCTION\n");
         return 2;
      }
      return run_pixel_formats(argv[2], argv[3]);
   }

   /* --draw-depth is the draw with a depth attachment under it, which no pass in this
    * port has ever had. It is its own flag because what it measures is not a pixel but
    * whether a draw was discarded, and that takes four draws of one quad rather than
    * one (run_depth says why each).
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-depth")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-depth FILE.metallib FUNCTION "
                         "[MTLPixelFormat[,...]|none [DEPTHFORMAT]]\n");
         return 2;
      }
      return run_depth(argv[2], argv[3], argc > 4 ? argv[4] : NULL,
                       argc > 5 ? argv[5] : NULL);
   }

   /* --draw-ms is the draw whose TEXTURE is multisample, which takes two passes and not
    * one: nothing puts a known texel in a multisample texture from the processor, so the
    * first pass renders the fill into it and the second is the shader under test reading
    * it (run_ms says why at length, and what each of the two configurations measures).
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-ms")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-ms FILE.metallib FUNCTION "
                         "[SAMPLES [FILLFUNCTION]]\n");
         return 2;
      }
      return run_ms(argv[2], argv[3], argc > 4 ? argv[4] : NULL,
                    argc > 5 ? argv[5] : NULL);
   }

   /* --draw-index is the draw that goes through an index buffer, which every drawing
    * call in a real application does and none in this library did. It is its own flag
    * because what it measures is not a pixel but whether the indices were read at all:
    * the same three indices are drawn from two places in one buffer, and the two
    * pictures have to differ (run_index says what each step would print if it were
    * wrong).
    */
   if (argc >= 2 && !strcmp(argv[1], "--draw-index")) {
      if (argc < 4) {
         fprintf(stderr, "usage: air_metal_test --draw-index FILE.metallib FUNCTION "
                         "[VERTEXFUNCTION]\n");
         return 2;
      }
      return run_index(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
   }

   /* --draw-vertex is the same draw with one thing changed: the vertex stage is a real
    * module out of the container instead of the one this library generates. It is its own
    * flag and not an optional argument of --draw so that the two can be run back to back
    * over the same fragment shader and the pixels compared -- which is the only way the
    * supplied stage can be said to be right about anything, since there is no second
    * implementation of Apple's vertex shaders to check it against.
    */
   /* --draw-attrib is the vertex ATTRIBUTE path: a real MTLVertexDescriptor over a real
    * [[stage_in]] vertex function, which is what the phone's window server asks for and
    * what this contract could not express until now. It takes two libraries because a
    * Metal pipeline may take its two functions from two of them, and the pair this was
    * measured with is QuartzCore's std_vert0_lpf over AppleMetalGLRenderer's
    * texturedFragment2DUInt: the first has the attributes and the second turns the
    * varying they feed into a texel a pixel can be read as.
    */
   /* --attrib-sweep is the same creation call over every function of a library, which is
    * what the window server does one pipeline at a time: no draw, no picture, just how
    * many of them get a pipeline out of this contract and what the rest are refused for.
    */
   if (argc >= 2 && !strcmp(argv[1], "--attrib-sweep")) {
      if (argc < 5) {
         fprintf(stderr, "usage: air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB "
                         "FRAGMENTFUNCTION\n");
         return 2;
      }
      return run_attrib_sweep(argv[2], argv[3], argv[4]);
   }

   if (argc >= 2 && !strcmp(argv[1], "--draw-attrib")) {
      if (argc < 6) {
         fprintf(stderr, "usage: air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION "
                         "FRAGMENTLIB FRAGMENTFUNCTION [GATEDVERTEXFUNCTION]\n");
         return 2;
      }
      return run_attrib(argv[2], argv[3], argv[4], argv[5],
                        argc > 6 ? argv[6] : NULL);
   }

   if (argc >= 2 && !strcmp(argv[1], "--draw-vertex")) {
      if (argc < 5) {
         fprintf(stderr, "usage: air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION "
                         "FUNCTION [MTLPixelFormat[,...] [SLOT:COMPONENTS[,...] "
                         "[MTLPixelFormat[:MTLTextureType][,...]]]]\n");
         return 2;
      }
      return run_draw(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : NULL,
                      argc > 6 ? argv[6] : NULL, argc > 7 ? argv[7] : NULL);
   }

   if (argc < 2) {
      fprintf(stderr, "usage: air_metal_test FILE.metallib [FUNCTION "
                      "[MTLPixelFormat[,...] [FILL]]]\n"
                      "       air_metal_test --textures FILE.metallib [FUNCTION]\n"
                      "       air_metal_test --draw FILE.metallib FUNCTION "
                      "[MTLPixelFormat[,...] "
                      "[MTLPixelFormat[:MTLTextureType][,...]]]\n"
                      "       air_metal_test --draw-depth FILE.metallib FUNCTION "
                      "[MTLPixelFormat[,...]|none [DEPTHFORMAT]]\n"
                      "       air_metal_test --draw-ms FILE.metallib FUNCTION "
                      "[SAMPLES [FILLFUNCTION]]\n"
                      "       air_metal_test --draw-index FILE.metallib FUNCTION "
                      "[VERTEXFUNCTION]\n"
                      "       air_metal_test --draw-vertex FILE.metallib VERTEXFUNCTION "
                      "FUNCTION [MTLPixelFormat[,...] [SLOT:COMPONENTS[,...] "
                      "[MTLPixelFormat[:MTLTextureType][,...]]]]\n"
                      "       air_metal_test --draw-attrib VERTEXLIB VERTEXFUNCTION "
                      "FRAGMENTLIB FRAGMENTFUNCTION [GATEDVERTEXFUNCTION]\n"
                      "       air_metal_test --attrib-sweep VERTEXLIB FRAGMENTLIB "
                      "FRAGMENTFUNCTION\n");
      return 2;
   }

   /* What every buffer is filled with, 0xa5 unless the fourth argument says. This is
    * air_run's --zero and --ones in one byte and for the same reason (docs/air.md
    * section 5): a kernel whose buffers carry counts, bounds or sentinels does nothing
    * at all on a byte pattern, and which fill gets it to the end says what it was
    * reading them as. Zero is the degenerate input that makes a bounds check pass;
    * 0xa5 is the one that makes a dispatch which wrote nothing distinguishable from
    * one that wrote zeros.
    */
   unsigned fill = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) & 0xff : 0xa5;

   /* One format per texture slot, comma separated, each optionally followed by ":" and
    * an MTLTextureType (2 is 2D and the default, 7 is 3D). Per slot and not per kernel
    * because Apple's own kernels mix both: BlitKernel_Unorm reads a texture2d<uint>
    * and writes a texture2d<float>, so "123,125" is that kernel, and
    * MetalFaceMask_PopulateCube reads a texture2d and writes a texture3d, so
    * "125,125:7" is that one. A single format or a single type would be refused for
    * whichever of the two slots it is not -- which is the library naming what the
    * shader declared, and is how these spellings were found.
    */
   unsigned format[16], type[16];
   bool is_float[COUNT_OF(format)];
   const char *arg = argc > 3 ? argv[3] : NULL;
   for (unsigned i = 0; i < COUNT_OF(format); i++) {
      const char *colon = arg ? strchr(arg, ':') : NULL;
      const char *comma = arg ? strchr(arg, ',') : NULL;
      type[i] = colon && (!comma || colon < comma) ? (unsigned)strtoul(colon + 1, NULL, 0)
                                                   : MTL_TEXTURE_TYPE_2D;
      if (type[i] != MTL_TEXTURE_TYPE_2D && type[i] != MTL_TEXTURE_TYPE_3D) {
         fprintf(stderr, "air_metal_test: texture type %u: this program makes "
                         "MTLTextureType2D (%u) and MTLTextureType3D (%u); the library makes "
                         "the other eight too\n", type[i], MTL_TEXTURE_TYPE_2D,
                 MTL_TEXTURE_TYPE_3D);
         return 2;
      }
      format[i] = arg ? (unsigned)strtoul(arg, NULL, 0) : MTL_RGBA32_FLOAT;
      if (format[i] != MTL_RGBA32_FLOAT && format[i] != MTL_RGBA32_UINT &&
          format[i] != MTL_RGBA32_SINT) {
         fprintf(stderr, "air_metal_test: pixel format %u: this program writes known texels "
                         "in MTLPixelFormatRGBA32Float (%u), RGBA32Uint (%u) or RGBA32Sint "
                         "(%u)\n", format[i], MTL_RGBA32_FLOAT, MTL_RGBA32_UINT,
                 MTL_RGBA32_SINT);
         return 2;
      }
      is_float[i] = format[i] == MTL_RGBA32_FLOAT;
      if (comma)
         arg = comma + 1;      /* otherwise the last one stands for the rest */
   }

   struct metallib lib = { 0 };
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   lib.size = ftell(f);
   fseek(f, 0, SEEK_SET);
   lib.bytes = malloc(lib.size);
   if (fread(lib.bytes, 1, lib.size, f) != lib.size) {
      fprintf(stderr, "air_metal_test: short read of %s\n", argv[1]);
      return 1;
   }
   fclose(f);

   char name[256];
   const uint8_t *bitcode = NULL;
   size_t bitcode_size = 0;
   if (!find_function(&lib, argc > 2 ? argv[2] : NULL, name, sizeof(name), &bitcode,
                      &bitcode_size))
      return 1;
   printf("air_metal_test: %s, %zu bytes of bitcode, %s\n", name, bitcode_size,
          bitcode[0] == 0xde ? "in its 0x0B17C0DE wrapper" : "a bare module");

   /* This machine has no Adreno, and rwa_device_open does not fall back to software by
    * itself: on the phone an absent GPU means a boot without gpu=1, and a driver has to
    * report that rather than quietly compute the answer on the processor. Not
    * overwritten, so that RWA_SOFTWARE=0 in the environment shows what a driver would be
    * told there.
    */
   setenv("RWA_SOFTWARE", "1", 0);

   char why[WHY] = "";
   struct rwa_device *device = rwa_device_open(why, sizeof(why));
   if (!device) {
      printf("air_metal_test: rwa_device_open: %s\n", why);
      return 1;
   }
   printf("air_metal_test: device is \"%s\"\n", rwa_device_name(device));

   struct rwa_compute_info info = { 0 };
   struct rwa_compute *compute =
      rwa_compute_create(device, bitcode, bitcode_size, argc > 2 ? argv[2] : NULL, &info,
                         why, sizeof(why));
   if (!compute) {
      printf("air_metal_test: rwa_compute_create: %s\n", why);
      rwa_device_close(device);
      return 1;
   }
   printf("air_metal_test: pipeline: %u threads per group, execution width %u, "
          "%u bytes of threadgroup memory, %u buffers, %u textures, %u samplers\n",
          info.max_threads_per_group, info.thread_execution_width,
          info.static_shared_bytes, info.buffers, info.textures, info.samplers);

   /* One buffer per binding the pipeline says it reads, each filled with a byte the
    * kernel could not have produced itself, so that a dispatch which wrote nothing is
    * distinguishable from one that wrote zeros -- the mistake air_run made first and
    * docs/air.md section 5 records.
    */
   struct rwa_buffer *buffers[16] = { 0 };
   unsigned count = info.buffers < COUNT_OF(buffers) ? info.buffers
                                                      : (unsigned)COUNT_OF(buffers);
   for (unsigned i = 0; i < count; i++) {
      buffers[i] = rwa_buffer_create(device, 4096, why, sizeof(why));
      if (!buffers[i]) {
         printf("air_metal_test: rwa_buffer_create: %s\n", why);
         return 1;
      }
      memset(rwa_buffer_map(buffers[i]), (int)fill, rwa_buffer_size(buffers[i]));
   }
   printf("air_metal_test: %u buffer%s of 4096 bytes, filled with 0x%02x\n", count,
          count == 1 ? "" : "s", fill);

   /* One texture per slot the pipeline says it binds, each with air_run's texel
    * pattern in it. A slot the kernel only writes gets the pattern too, which is the
    * same argument as the buffers' 0xa5: a kernel that writes nothing has to be
    * distinguishable from one that writes zeros, and InitMinMax writing zeros would
    * look identical to a dispatch that never ran (docs/air.md section 5).
    */
   const unsigned origin[3] = { 0, 0, 0 };
   struct rwa_texture *textures[16] = { 0 };
   union { float f[TEXELS]; int32_t i[TEXELS]; } written[16];
   unsigned extent[COUNT_OF(textures)][3];
   unsigned ntex = info.textures < COUNT_OF(textures) ? info.textures
                                                      : (unsigned)COUNT_OF(textures);
   for (unsigned i = 0; i < ntex; i++) {
      unsigned depth = type[i] == MTL_TEXTURE_TYPE_3D ? TEX : 1;
      extent[i][0] = TEX;
      extent[i][1] = TEX;
      extent[i][2] = depth;
      const struct rwa_texture_shape shape = {
         .pixel_format = format[i],
         .type = type[i],
         .width = TEX, .height = TEX, .depth = depth,
         .array_length = 1, .mip_levels = 1, .sample_count = 1,
         /* both, because the contract says how many textures a kernel binds and not
          * which of them it writes -- and MTLTextureUsageShaderRead alone would be
          * refused at the dispatch for exactly the slot the kernel stores to
          */
         .usage = MTL_USAGE_SHADER_READ | MTL_USAGE_SHADER_WRITE,
      };
      textures[i] = rwa_texture_create(device, &shape, why, sizeof(why));
      if (!textures[i]) {
         printf("air_metal_test: rwa_texture_create: %s\n", why);
         return 1;
      }
      memset(&written[i], 0, sizeof(written[i]));
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               float v[4];
               texel_value(i, x, y, z, v);
               for (unsigned c = 0; c < 4; c++) {
                  unsigned k = texel_index(x, y, z) + c;
                  if (is_float[i])
                     written[i].f[k] = v[c];
                  else
                     written[i].i[k] = (int32_t)v[c];
               }
            }
      if (rwa_texture_write(textures[i], 0, 0, origin, extent[i], &written[i], 0, why,
                            sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write: %s\n", why);
         return 1;
      }
   }
   for (unsigned i = 0; i < ntex; i++)
      printf("air_metal_test: texture %u is %ux%ux%u of pixel format %u, texel (x,y,z) = "
             "(100+x+%u, 200+y, 300+x+y+z, 400)\n", i, extent[i][0], extent[i][1],
             extent[i][2], format[i], 1000 * i);

   /* and a sampler per slot the API is expected to fill. Nearest, clamped and in
    * pixels, which is what air_run binds in its compute path: a kernel's own
    * coordinates are integer texels, and nearest filtering at a texel centre has no
    * weights to round. A sampler the shader declares for itself is not counted here
    * and not bound -- the library builds that one out of the shader's word.
    */
   struct rwa_sampler *samplers[16] = { 0 };
   unsigned nsamp = info.samplers < COUNT_OF(samplers) ? info.samplers
                                                        : (unsigned)COUNT_OF(samplers);
   for (unsigned i = 0; i < nsamp; i++) {
      const struct rwa_sampler_state state = {
         .address = { 0, 0, 0 },          /* MTLSamplerAddressModeClampToEdge */
         .min_filter = 0, .mag_filter = 0,/* MTLSamplerMinMagFilterNearest */
         .mip_filter = 0,                 /* MTLSamplerMipFilterNotMipmapped */
         .compare_function = 0,           /* MTLCompareFunctionNever, and nothing reads it */
         .max_anisotropy = 1,
         .normalized_coordinates = 0,
         .lod_min = 0.0f, .lod_max = 65504.0f,   /* the range Apple's own default carries */
      };
      samplers[i] = rwa_sampler_create(device, &state, why, sizeof(why));
      if (!samplers[i]) {
         printf("air_metal_test: rwa_sampler_create: %s\n", why);
         return 1;
      }
   }
   if (nsamp)
      printf("air_metal_test: %u sampler%s from the API: nearest, clamp to edge, in pixels\n",
             nsamp, nsamp == 1 ? "" : "s");

   const struct rwa_bindings bindings = {
      .buffers = buffers, .buffer_count = count,
      .textures = textures, .texture_count = ntex,
      .samplers = samplers, .sampler_count = nsamp,
   };
   const unsigned groups[3] = { 1, 1, 1 };
   const unsigned group_size[3] = { TEX, TEX, 1 };
   if (rwa_dispatch(compute, &bindings, groups, group_size, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_dispatch: %s\n", why);
      rwa_compute_destroy(compute);
      for (unsigned i = 0; i < count; i++)
         rwa_buffer_destroy(buffers[i]);
      for (unsigned i = 0; i < ntex; i++)
         rwa_texture_destroy(textures[i]);
      for (unsigned i = 0; i < nsamp; i++)
         rwa_sampler_destroy(samplers[i]);
      rwa_device_close(device);
      return 1;
   }
   if (rwa_finish(device, why, sizeof(why)) != 0) {
      printf("air_metal_test: rwa_finish: %s\n", why);
      return 1;
   }
   printf("air_metal_test: dispatched %ux%ux%u threadgroups of %ux%ux%u threads\n",
          groups[0], groups[1], groups[2], group_size[0], group_size[1], group_size[2]);

   for (unsigned i = 0; i < count; i++) {
      const uint32_t *words = rwa_buffer_map(buffers[i]);
      uint32_t as_went_in = fill * 0x01010101u;
      bool changed = false;
      for (unsigned w = 0; w < rwa_buffer_size(buffers[i]) / 4 && !changed; w++)
         changed = words[w] != as_went_in;
      printf("air_metal_test: buffer %u %s; first six words %08x %08x %08x %08x %08x %08x\n",
             i, changed ? "was written" : "came back as it went in", words[0], words[1],
             words[2], words[3], words[4], words[5]);
   }

   /* and every texture read back, which is the half of this a buffer cannot stand in
    * for: most of the corpus's texture kernels write a texture and nothing else, so
    * without this the program would report a dispatch it had not looked at.
    */
   for (unsigned i = 0; i < ntex; i++) {
      union { float f[TEXELS]; int32_t i[TEXELS]; } got;
      memset(&got, 0, sizeof(got));
      if (rwa_texture_read(textures[i], 0, 0, origin, extent[i], &got, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read: %s\n", why);
         return 1;
      }
      bool changed = memcmp(&got, &written[i], sizeof(got)) != 0;
      printf("air_metal_test: texture %u %s\n", i,
             changed ? "was written" : "came back as it went in");
      print_texel("texture", i, 0, 0, 0, &got, is_float[i]);
      print_texel("texture", i, 1, 1, extent[i][2] > 1 ? 1 : 0, &got, is_float[i]);

      /* And texel (1,1) again on its own, which is the part a whole-level read cannot
       * check: an origin that is ignored or a row stride taken from the wrong side
       * comes back as the texel at (0,0) or as a neighbour, and every component of
       * every texel here differs from every other, so it cannot come back right by
       * accident.
       */
      unsigned z = extent[i][2] > 1 ? 1 : 0;
      const unsigned one[3] = { 1, 1, z }, single[3] = { 1, 1, 1 };
      union { float f[4]; int32_t i[4]; } texel;
      if (rwa_texture_read(textures[i], 0, 0, one, single, &texel, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read of one texel: %s\n", why);
         return 1;
      }
      printf("air_metal_test: texture %u texel (1,1,%u) read on its own %s what the whole "
             "level gave\n", i, z,
             memcmp(&texel, (const char *)&got + texel_index(1, 1, z) * sizeof(float),
                    sizeof(texel)) == 0 ? "is" : "IS NOT");
   }

   /* And the same question on the write side, which the dispatch cannot ask: one texel
    * written at an origin a whole-level write never visits. Afterwards exactly that
    * texel must differ from the pattern and all fifteen others must not -- an origin
    * dropped, or a row stride measured in texels instead of bytes, lands the value
    * somewhere else and leaves two texels wrong instead of one.
    */
   if (ntex) {
      const unsigned at[3] = { 2, 3, extent[0][2] > 1 ? 2u : 0u }, single[3] = { 1, 1, 1 };
      union { float f[4]; int32_t i[4]; } mark;
      for (unsigned c = 0; c < 4; c++) {
         if (is_float[0])
            mark.f[c] = -1.0f - (float)c;
         else
            mark.i[c] = -1 - (int32_t)c;
      }
      if (rwa_texture_write(textures[0], 0, 0, at, single, &mark, 0, why, sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_write of one texel: %s\n", why);
         return 1;
      }
      union { float f[TEXELS]; int32_t i[TEXELS]; } level;
      memset(&level, 0, sizeof(level));
      if (rwa_texture_read(textures[0], 0, 0, origin, extent[0], &level, 0, why,
                           sizeof(why)) != 0) {
         printf("air_metal_test: rwa_texture_read after it: %s\n", why);
         return 1;
      }
      unsigned moved = 0;
      bool landed = false;
      for (unsigned z = 0; z < extent[0][2]; z++)
         for (unsigned y = 0; y < TEX; y++)
            for (unsigned x = 0; x < TEX; x++) {
               size_t off = texel_index(x, y, z) * sizeof(float);
               const char *there = (const char *)&level + off;
               if (x == at[0] && y == at[1] && z == at[2])
                  landed = memcmp(there, &mark, sizeof(mark)) == 0;
               else if (memcmp(there, (const char *)&written[0] + off, sizeof(mark)) != 0)
                  moved++;
            }
      printf("air_metal_test: one texel written at (%u,%u,%u) %s, and %u other texel%s "
             "changed\n", at[0], at[1], at[2], landed ? "is there" : "IS NOT THERE", moved,
             moved == 1 ? "" : "s");
   }

   rwa_compute_destroy(compute);
   for (unsigned i = 0; i < count; i++)
      rwa_buffer_destroy(buffers[i]);
   for (unsigned i = 0; i < ntex; i++)
      rwa_texture_destroy(textures[i]);
   for (unsigned i = 0; i < nsamp; i++)
      rwa_sampler_destroy(samplers[i]);
   rwa_device_close(device);
   return 0;
}
