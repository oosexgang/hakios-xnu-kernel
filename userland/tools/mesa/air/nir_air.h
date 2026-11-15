/*
 * Copyright © 2026 HakiOS contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * AIR -> NIR: Metal shaders (the LLVM bitcode inside a .metallib) into NIR.
 * See userland/docs/air.md for the design and the mapping
 * tables this follows.
 */

#ifndef NIR_AIR_H
#define NIR_AIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler/shader_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nir_shader nir_shader;
typedef struct nir_shader_compiler_options nir_shader_compiler_options;

/* -- the .metallib container ---------------------------------------------- */

struct air_metallib_function {
   const char *name;
   int type;               /* the container's TYPE tag, see air_metallib_type_name() */
   const void *bitcode;    /* the module, still in its 0x0B17C0DE wrapper */
   size_t bitcode_size;
};

struct air_metallib {
   unsigned version_major, version_minor;
   unsigned num_functions;
   struct air_metallib_function *functions;
};

bool air_metallib_parse(void *mem_ctx, const void *data, size_t size,
                        struct air_metallib *lib, const char **error);
const char *air_metallib_type_name(int type);

/* -- what the translated shader expects of the runtime ---------------------- */

enum air_resource_kind {
   AIR_RESOURCE_CONSTANT_BUFFER,    /* UBO at .index */
   AIR_RESOURCE_DEVICE_BUFFER,      /* SSBO at .index */
   AIR_RESOURCE_THREADGROUP_BUFFER, /* sized at dispatch */
   AIR_RESOURCE_TEXTURE,            /* sampler view at .index */
   AIR_RESOURCE_IMAGE,              /* storage image at .index */
   AIR_RESOURCE_SAMPLER,            /* sampler state at .index */
};

/* A `constexpr sampler` of the shader's own, its 64-bit state word decoded
 * (air_decode_sampler_state). The layout and the three sources it was read from
 * are in userland/docs/air.md (the sampler word's table): address s, t, r at
 * bits 0-8, mag and min filter 9-12, mip filter 13-14, pixel coordinates 15,
 * compare function 16-19 (AIR's numbering: 0 none, 1 less ... 7 always, 8 never,
 * the default), max anisotropy - 1 at 20-23, lod min and max as halves at 24-55,
 * border colour 56-57, reduction 58-59.
 */
enum air_sampler_address {
   AIR_SAMPLER_CLAMP_TO_ZERO = 0,    /* transparent black outside: not a border mode */
   AIR_SAMPLER_CLAMP_TO_EDGE = 1,
   AIR_SAMPLER_REPEAT = 2,
   AIR_SAMPLER_MIRRORED_REPEAT = 3,
   AIR_SAMPLER_CLAMP_TO_BORDER = 4,
};

enum air_sampler_filter {
   AIR_SAMPLER_NEAREST = 0,
   AIR_SAMPLER_LINEAR = 1,
   AIR_SAMPLER_BICUBIC = 2,
};

enum air_sampler_mip_filter {
   AIR_SAMPLER_MIP_NONE = 0,
   AIR_SAMPLER_MIP_NEAREST = 1,
   AIR_SAMPLER_MIP_LINEAR = 2,
};

struct air_sampler_state {
   unsigned address[3];
   unsigned mag_filter, min_filter, mip_filter;
   bool pixel_coord;            /* coordinates in texels, not normalized */
   unsigned compare_func;       /* AIR's numbering, see above */
   unsigned max_anisotropy;     /* 1..16 */
   unsigned lod_min, lod_max;   /* the halves' bits */
   unsigned border_color;
   unsigned reduction;
};

struct air_sampler_state air_decode_sampler_state(uint64_t word);

struct air_resource {
   enum air_resource_kind kind;
   unsigned index;   /* Metal's [[buffer(n)]], [[texture(n)]], [[sampler(n)]] */
   unsigned size;    /* air.buffer_size when given, else 0 */
   bool read, write;
   unsigned param;   /* the entry point's parameter; ~0u for an inline sampler */
   const char *name; /* air.arg_name */
   const char *type_name;

   /* A sampler the shader declares itself (Metal's constexpr sampler) rather
    * than one the API binds: the runtime has to create it from this state
    * word and bind it at .index, which the frontend assigns after the API's
    * samplers. The word's layout is not decoded yet -- see docs/air.md.
    */
   bool inline_sampler;
   uint64_t sampler_state;
   struct air_sampler_state sampler;  /* sampler_state decoded, for an inline sampler */

   int raster_order_group;   /* Metal's [[raster_order_group(n)]], -1 for none */

   /* A member of an argument buffer: the Metal buffer index of the buffer that
    * holds it (set) and its [[id(n)]] in it; ~0u for both when the resource is
    * a direct argument, which Metal names by .index. */
   unsigned argbuf_set, argbuf_id;

   /* nothing in the shader, once specialized, reads it: a runtime need not bind it */
   bool unused;

   /* An image the shader writes at one mip level other than the base (Metal's
    * texture.write(value, coord, lod)): the level itself, or -- level_from_buffer -- the
    * level_bytes-byte unsigned word at byte level_offset of the constant buffer bound at
    * Metal index level_buffer, which the runtime reads when it binds the image and binds
    * the view at. The shader writes level 0 of that view. All zero: the base level. */
   unsigned level;
   bool level_from_buffer;
   unsigned level_buffer, level_offset, level_bytes;
   bool level_float;     /* the word is a 32-bit float and the level the whole number below it */
   bool level_written;   /* (the translator's: a write has decided the level) */
};

/* the type a vertex attribute is read as: its format converts to this */
enum air_attribute_base_type {
   AIR_ATTRIB_FLOAT,
   AIR_ATTRIB_INT,
   AIR_ATTRIB_UINT,
};

/* a vertex function's [[stage_in]] attribute */
struct air_vertex_attribute {
   unsigned index;            /* air.location_index: Metal's [[attribute(n)]] */
   unsigned location;         /* gl_vert_attrib: VERT_ATTRIB_GENERIC0 + index */
   unsigned driver_location;  /* the shader's input slot, ~0u when it has none */
   unsigned components;
   unsigned bit_size;         /* as declared; the input itself is 32 bits */
   enum air_attribute_base_type base_type;
   const char *name;          /* air.arg_name */
   const char *type_name;     /* air.arg_type_name */
   bool used;                 /* read once the function constants are applied */
   const char *function_constant;  /* the constant that decides whether it exists, or NULL */
};

struct air_varying {
   const char *name;  /* the AIR name that links vertex to fragment */
   unsigned slot;     /* gl_varying_slot assigned by the translator */
   unsigned components;
   bool flat, centroid, sample, no_perspective;
};

struct air_shader_info {
   mesa_shader_stage stage;     /* MESA_SHADER_NONE for an air.visible function */
   const char *entry_point;
   unsigned num_resources;
   struct air_resource *resources;
   unsigned num_varyings;     /* vertex outputs or fragment inputs */
   struct air_varying *varyings;
   unsigned num_render_targets;
   unsigned threadgroup_bytes; /* static threadgroup memory */

   unsigned num_vertex_attributes;
   struct air_vertex_attribute *vertex_attributes;
   uint32_t vertex_attributes_read;  /* a bit per [[attribute(n)]] the shader reads */

   uint32_t raster_order_groups;     /* a bit per raster order group the shader uses */
   uint32_t fbfetch_targets;         /* a bit per colour attachment read back ([[color(n)]] as input) */

   /* what was done other than exactly what the shader asked for, one line each,
    * for a runtime or a harness to say */
   unsigned num_notes;
   const char **notes;
};

/* -- the translator --------------------------------------------------------- */

struct air_constant_value {
   unsigned index;     /* the function constant's index */
   uint64_t bits;      /* its value, as the bits of its type (the first 64) */
   uint64_t more[3];   /* the rest of a vector constant: component i at bit i*width */
   bool undefined;     /* not given: reads as zero, is_function_constant_defined() false */
};

/* A function linked into a pipeline (MTLLinkedFunctions, or a stitched
 * function): a module of its own that the shader calls by name. */
struct air_linked_function {
   const char *name;         /* the name the shader calls it by */
   const void *bitcode;
   size_t size;
   const char *entry;        /* the function in that module, or NULL for its one function with a body */
   const struct air_constant_value *constants;
   unsigned num_constants;
};

struct air_to_nir_options {
   const struct air_constant_value *constants;
   unsigned num_constants;

   /* Translate an air.visible library function by generating an entry point
    * that calls it: argument i read from device buffer 0 at byte 16*i, the
    * return value written to device buffer 1 at offset 0, one thread. Metal's
    * own way of using these functions is to stitch them into a caller at
    * pipeline time (air.stitching_info in their metadata), which this frontend
    * does not implement; the wrapper exists so that a library function's body
    * can be run and compared at all. A shader produced this way is not an
    * entry point of the corpus and is counted separately.
    */
   bool library_wrapper;

   /* the functions the pipeline links, which the shader may call */
   const struct air_linked_function *linked;
   unsigned num_linked;
};

/* Translates one AIR module (one function of a .metallib). entry_point may
 * be NULL: the module's own entry point is taken. On failure returns NULL
 * and sets *error (allocated under mem_ctx) to "<category>: <what>", where
 * the category is one of
 *
 *    reader       the bitcode could not be read
 *    stage        no entry point, or not one this translator knows
 *    interface    an entry-point metadata key not handled yet
 *    instruction  an LLVM instruction or form not handled yet
 *    intrinsic    an air.* (or llvm.*) function not handled yet
 *
 * so the corpus report can count them. *info, when non-NULL, describes what
 * the shader expects to be bound (allocated under mem_ctx).
 */
nir_shader *air_to_nir(void *mem_ctx, const void *bitcode, size_t size,
                       const char *entry_point,
                       const struct air_to_nir_options *options,
                       const nir_shader_compiler_options *nir_options,
                       struct air_shader_info *info, const char **error);

/* The function constants the module declares, with .bits zero: a caller
 * fills in the values it will specialize with. Returns how many there are.
 */
unsigned air_module_function_constants(void *mem_ctx, const void *bitcode, size_t size,
                                       struct air_constant_value **out);

/* What the module is, without translating it: the stage from its metadata
 * (MESA_SHADER_NONE for visible functions, -1 when unknown) and the air.*
 * functions it calls, NUL-separated in *calls.
 */
int air_module_stage(void *mem_ctx, const void *bitcode, size_t size, const char **calls,
                     const char **error);

#ifdef __cplusplus
}
#endif

#endif /* NIR_AIR_H */
