/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * AIR -> NIR: Metal shaders (the LLVM bitcode inside a .metallib) into NIR.
 * See docs/air.md of Darwin on redwood for the design and the mapping
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

/* Metal's `constexpr sampler`, as the 64-bit word a shader carries encodes it.
 *
 * The layout is not guessed and not read off one binary: the names, the enum
 * values and the defaults are in Apple's own metal_stdlib header on the device
 * (`GPUCompiler.framework/.../include/metal/metal_texture`, struct
 * _sampler_state and _get_sampler_state), the field positions come from the
 * encoder that writes the word (llvm::air::AIRSamplerStateEncoder::encode in
 * libGPUCompilerImpl) and the decoder that reads it back
 * (AIRConstSampler::constructFlatReply in AGXCompilerCore, with the flatbuffers
 * schema beside it). Three sources, which is what it took: libGPUCompilerImpl
 * also carries its own AIRAddressMode printer in which zero prints as
 * clamp_to_border, and the string clamp_to_zero does not occur in that binary
 * at all -- read alone it gives a plausible and wrong value for the commonest
 * field.
 *
 * Decoding it against the corpus is the check that it is right: the three words
 * the 456 modules carry come out as sampler(coord::pixel),
 * sampler(coord::pixel, filter::linear) and
 * sampler(coord::pixel, filter::linear, address::clamp_to_zero), with every
 * other field equal to the header's default -- including a lod range of
 * [0, 65504], which is two separate 16-bit halves that a wrong layout would
 * not land on.
 */
enum air_sampler_address {
   AIR_SAMPLER_CLAMP_TO_ZERO = 0,   /* transparent black, and not a border colour */
   AIR_SAMPLER_CLAMP_TO_EDGE = 1,   /* the default */
   AIR_SAMPLER_REPEAT = 2,
   AIR_SAMPLER_MIRRORED_REPEAT = 3,
   AIR_SAMPLER_CLAMP_TO_BORDER = 4,
};

enum air_sampler_filter {
   AIR_SAMPLER_NEAREST = 0,         /* the default */
   AIR_SAMPLER_LINEAR = 1,
   AIR_SAMPLER_BICUBIC = 2,
};

enum air_sampler_mip_filter {
   AIR_SAMPLER_MIP_NONE = 0,        /* the default */
   AIR_SAMPLER_MIP_NEAREST = 1,
   AIR_SAMPLER_MIP_LINEAR = 2,
};

struct air_sampler_state {
   enum air_sampler_address address[3];   /* s, t, r */
   enum air_sampler_filter mag_filter, min_filter;
   enum air_sampler_mip_filter mip_filter;
   bool pixel_coord;              /* coord::pixel; the default is normalized */
   unsigned compare_func;
   unsigned max_anisotropy;       /* 1 to 16 */
   uint16_t lod_min, lod_max;     /* halves, as their bits */
   unsigned border_color;
   unsigned reduction;            /* AGX does not read this field */
};

struct air_resource {
   enum air_resource_kind kind;
   unsigned index;   /* Metal's [[buffer(n)]], [[texture(n)]], [[sampler(n)]] --
                      * EXCEPT for an argument-buffer member, where it is the
                      * index this frontend allocated: see argbuf_set below */
   unsigned size;    /* air.buffer_size when given, else 0 */
   bool unused;      /* a buffer the compiled shader never reads after all: specialized
                      * away (note_constant_reads), so nothing has to be bound there */
   unsigned reads;   /* the most bytes the compiled shader reads from it (a constant buffer,
                      * set by the runtime from the final NIR), else 0: not known */
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
   uint64_t sampler_state;         /* the word itself, for a runtime that wants it raw */
   struct air_sampler_state sampler;  /* ... and decoded */

   /* Metal's `[[raster_order_group(n)]]` on this resource: accesses to it from
    * overlapping fragments happen in rasterization order. -1 when the shader
    * asked for none. a6xx has no per-resource ordering at all -- see
    * air_shader_info.raster_order_groups for what the runtime has to do, and
    * for the note the frontend leaves when a shader asks for this.
    */
   int raster_order_group;

   /* A member of an ARGUMENT BUFFER (`air.indirect_buffer`, Metal's
    * `[[id(n)]]` struct) rather than an argument of its own, and the one place
    * in this structure where `.index` is not Metal's number: ~0u when this
    * resource is a direct argument, otherwise the Metal `[[buffer(n)]]` the
    * SET is bound at and the member's own `[[id(n)]]` inside it.
    *
    * Both halves are needed because the two ends name the member differently
    * and neither name can be derived from the other. The application names it
    * by (set, id): that is what it passes to MTLArgumentEncoder. A member's own
    * `air.location_index` is per set -- set 0's member 0 and set 1's member 0
    * both say 0, and 237 of Resident Evil 4's modules have two sets -- so it is
    * not a binding. The runtime binds by `.index`, which this frontend
    * allocates: one flat counter per kind (buffer, texture, sampler), in
    * (set, member) order, continuing above the highest index any DIRECT
    * argument of that kind already took, so that a member and an argument
    * cannot land in one Gallium slot.
    *
    * The allocation is over every member the set declares, used or not, so
    * that it depends on the metadata alone: the runtime's encoder has to answer
    * `encodedLength` for a set whichever of its members this shader reads, and
    * a numbering that skipped the unread ones would differ between two shaders
    * sharing one set. See docs/air.md section 8.2.
    */
   unsigned argbuf_set;
   unsigned argbuf_id;
};

struct air_varying {
   const char *name;  /* the AIR name that links vertex to fragment */
   unsigned slot;     /* gl_varying_slot assigned by the translator */
   unsigned components;
   bool flat, centroid, sample, no_perspective;
};

/* What the shader wants of each component of a vertex attribute. The LLVM type
 * cannot say whether an integer is signed -- `int4` and `uint4` are both
 * `<4 x i32>` -- so the sign comes from `air.arg_type_name`, Apple's own
 * reflection string, and from nowhere else. A runtime that checks an
 * MTLVertexFormat against this is checking the shader's declared type against
 * the format's converted type, which is what Metal's own pipeline validation
 * checks.
 */
enum air_attribute_base_type {
   AIR_ATTRIB_FLOAT,   /* `float`, `half` */
   AIR_ATTRIB_INT,     /* `char`, `short`, `int` -- and the fallback with no type name */
   AIR_ATTRIB_UINT,    /* `uchar`, `ushort`, `uint` */
};

/* One member of a vertex function's `[[stage_in]]` struct: Metal's
 * `[[attribute(n)]]`.
 *
 * AIR does not present the struct at all. Apple's compiler FLATTENS it: every
 * member is an ordinary parameter of the entry point with its own
 * `air.vertex_input` node, and the struct type is nowhere in the module. So
 * there is no layout here and none can be inferred -- an attribute's format,
 * offset, stride and buffer are the MTLVertexDescriptor's, which lives in the
 * API and not in the shader. This table is the shader's HALF of that match:
 * which indices it reads and what it wants each to be.
 */
struct air_vertex_attribute {
   unsigned index;        /* Metal's `[[attribute(n)]]`, `air.location_index` */
   unsigned location;     /* VERT_ATTRIB_GENERIC0 + index, the NIR input's location */
   unsigned components;   /* 1 to 4 */

   /* The NIR input's driver_location, and the one field here a runtime cannot
    * do without: Gallium matches vertex elements to shader inputs BY POSITION,
    * not by location. ir3's setup_input indexes `so->inputs[]` by
    * nir_intrinsic_base -- the driver_location nir_lower_io wrote -- and
    * fd6_vertex_state_create emits VFD_DECODE[i] from `elements[i]` while
    * emit_vfd_dest emits VFD_DEST_CNTL_INSTR[i] from `vs->inputs[i]`. So
    * pipe_vertex_element[i] feeds the input whose driver_location is i, and
    * these are assigned in the order AIR lists the attributes with the UNUSED
    * ones skipped -- for std_vert0_lpf, 0 is attribute 0 and 1 is attribute 3.
    * ~0u when `used` is false.
    */
   unsigned driver_location;

   /* The width the SHADER reads, 8, 16 or 32. The NIR input is 32 bits wide
    * whatever this says, and the narrowing is an instruction in the shader --
    * see docs/air.md: ir3 puts a 16-bit vertex input in the HALF register file
    * and then reports it in `ir3_shader_variant.inputs[]`, which has no half
    * flag, as a full register, which is the number fd6_program.cc programs
    * VFD_DEST_CNTL_INSTR_REGID from. A 32-bit input keeps the hardware fetch
    * in the only regime either freedreno driver uses.
    */
   unsigned bit_size;
   enum air_attribute_base_type base_type;

   const char *name;      /* air.arg_name */
   const char *type_name; /* air.arg_type_name: "float4", "half4", "uchar4" ... */

   /* False when Apple's metadata marks the parameter `air.arg_unused`: the
    * attribute is in the shader's interface and nothing reads it, so there is
    * NO NIR input for it and the runtime need not bind anything. Eight of
    * QuartzCore's fourteen `[[stage_in]]` vertex shaders have one, and a fetch
    * set up for a value nothing reads costs a VFD slot and a binding the
    * application's descriptor may legally not have configured at all.
    */
   bool used;

   /* The implicit function-constant predicate that gates this attribute, as
    * Apple's metadata names it (`vtx_attr_has_texcoords`), or NULL. The
    * parameter EXISTS either way -- the entry point's signature is fixed --
    * so the attribute is translated either way; what the predicate decides is
    * whether the shader reads it, and the value of that predicate comes from
    * the function constants the pipeline supplies. There is no index to give:
    * these globals are not in `!air.function_constants`, they are computed by
    * the module's `air.static_init` constructor from the constants that are.
    */
   const char *function_constant;
};

struct air_shader_info {
   gl_shader_stage stage;     /* MESA_SHADER_NONE for an air.visible function */
   const char *entry_point;
   unsigned num_resources;
   struct air_resource *resources;
   unsigned num_varyings;     /* vertex outputs or fragment inputs */
   struct air_varying *varyings;

   /* The `[[stage_in]]` members of a vertex function, in the order AIR lists
    * them (which is the order of the entry point's parameters and NOT the
    * order of the attribute indices: QuartzCore's std_vert0_lpf declares
    * 0, 1, 3). Empty for every other stage, and empty for a vertex shader that
    * reads its vertices out of a buffer indexed by air.vertex_id -- Apple's
    * three in the GL renderer and twenty-five of QuartzCore's thirty-nine.
    */
   unsigned num_vertex_attributes;
   struct air_vertex_attribute *vertex_attributes;

   /* One bit per Metal attribute index that the shader actually READS: the set
    * the pipeline's MTLVertexDescriptor must configure. An attribute marked
    * air.arg_unused is in vertex_attributes and not here. One caveat, which the
    * translation also leaves as a note: an attribute gated by a function
    * constant has its bit set whatever the constant says, because the parameter
    * exists either way -- see air_vertex_attribute.function_constant before
    * demanding a binding for a bit of this mask.
    */
   unsigned vertex_attributes_read;

   unsigned num_render_targets;
   /* The render targets a fragment shader READS -- Metal's `[[color(n)]]` argument,
    * framebuffer fetch -- one bit per target. It is separate from num_render_targets,
    * which is what the stage writes, because the two need not overlap: QuartzCore's
    * `inplace_copy` reads target 0 and declares no colour output at all, and its
    * `path_blit_frag` reads target 2 while writing 0, 1 and 2.
    */
   unsigned fbfetch_targets;

   /* The raster order groups the shader declares on its resources, one bit
    * each; 0 when it declares none.
    *
    * a6xx cannot give what this asks for. Metal's guarantee is per RESOURCE --
    * two fragments that overlap are ordered against each other for the
    * resources in a group and not for anything else -- and the only knob the
    * hardware has is per DRAW: GRAS_SC_CNTL.SINGLE_PRIM_MODE, which
    * freedreno drives from pipe_blend_state.blend_coherent (fd6_emit.cc,
    * build_prim_mode). So a runtime that sets blend_coherent for any pipeline
    * whose shader has a non-zero raster_order_groups gives the shader MORE
    * synchronisation than it asked for, which is safe and slower; there is no
    * way to give exactly what it asked for, and no way to give less.
    *
    * The frontend does not refuse such a shader -- the coarser guarantee is
    * still a correct one -- but it does not pass over it in silence either:
    * every resource with a group leaves a note in `notes`, which
    * `air2nir --report` prints and tools/air/corpus.py counts.
    */
   unsigned raster_order_groups;

   /* What was translated with a caveat: a shader this frontend accepted while
    * giving something other than exactly what the module asked for. Nothing
    * here is an error -- an error is a refusal and air_to_nir returns NULL for
    * it -- and nothing here is advice. Each note names a thing the module asked
    * for and what was done instead, so that it is in the log at translation
    * time rather than only in a document.
    */
   unsigned num_notes;
   const char **notes;

   unsigned threadgroup_bytes; /* static threadgroup memory */
};

/* The word, field by field. A runtime that holds only `sampler_state` can call
 * this; `air_resource.sampler` is what it returns.
 */
struct air_sampler_state air_decode_sampler_state(uint64_t word);

/* -- the translator --------------------------------------------------------- */

struct air_constant_value {
   unsigned index;     /* the function constant's index */
   uint64_t bits;      /* its value, as the bits of its type */
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
