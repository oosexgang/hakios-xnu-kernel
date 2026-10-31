/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/* Shared between air_to_nir.c and air_intrinsics.c. */

#ifndef AIR_PRIVATE_H
#define AIR_PRIVATE_H

#include "air_module.h"
#include "nir_air.h"

#include "nir.h"
#include "nir_builder.h"

/* Where a pointer points. AIR's address spaces say which kind of memory;
 * which buffer, and the byte offset into it, the translator tracks itself
 * (docs/air.md section 3): no pointer in the corpus is ever stored to
 * memory or cast to an integer, so a pointer never has to become a number.
 */
enum air_ptr_class {
   AIR_PTR_NONE,
   AIR_PTR_UBO,           /* a constant buffer: .binding is its index */
   AIR_PTR_SSBO,          /* a device buffer */
   AIR_PTR_SHARED,        /* threadgroup memory */
   AIR_PTR_SCRATCH,       /* an alloca, or a program-scope variable */
   AIR_PTR_CONSTANT_DATA, /* a constant global: nir->constant_data */
   AIR_PTR_FUNCTION_CONSTANT, /* a function constant: .binding is its index */
   AIR_PTR_TEXTURE,       /* a texture handle; offset unused */
   AIR_PTR_SAMPLER,       /* a sampler handle */
   AIR_PTR_VARIABLE,      /* a program-scope scalar or vector kept in a nir_variable (.var) */

   /* An ARGUMENT BUFFER (`air.indirect_buffer`): .binding is the set's number
    * in air_ctx.argbufs and .offset the byte offset into it. Nothing is ever
    * loaded from such a pointer as a value -- a load through it RESOLVES to
    * the member that byte offset names (argbuf_load), which is the whole of
    * why argument buffers need no addresses here.
    */
   AIR_PTR_ARGBUF,
};

/* the binding of iOS 26's null texture (air.get_null_texture_*) and read sampler (air.get_read_sampler) */
#define AIR_NULL_BINDING        0xfffffffeu

struct air_ptr {
   enum air_ptr_class class;
   unsigned binding;
   bool is_image;                    /* a texture the shader writes: an image, not a view */
   nir_def *offset;                  /* bytes, 32-bit; NULL means zero */
   const struct air_type *pointee;   /* for sizes, and for loads without a type */
   struct nir_variable *var;         /* AIR_PTR_VARIABLE */
};

enum air_val_kind {
   AIR_VAL_NONE,
   AIR_VAL_SSA,   /* a scalar or vector */
   AIR_VAL_AGG,   /* a struct or array, as a tree of values */
   AIR_VAL_PTR,

   /* A POINTER as a value rather than a resolution. air_ptr is *(where, byte
    * offset)* decided while translating, which is what lets this frontend emit
    * load_ubo at Metal's own index with no descriptor indirection; a pointer
    * CHOSEN at run time has no such resolution, so the choice itself is kept
    * and made where the pointer is used. A pointer STORED to memory is the
    * other half of the same problem and has no form here at all -- see
    * docs/air.md section 3 for what that costs.
    */

   /* `select i1 %c, ptr %a, ptr %b`: .def is the condition and .elems[0..1]
    * are the two pointers. A LOAD through it loads BOTH and selects the
    * values, which is the only form NIR has; that is why it leaves a note.
    */
   AIR_VAL_PTR_SELECT,
};

struct air_val {
   enum air_val_kind kind;
   nir_def *def;
   struct air_ptr ptr;
   unsigned num_elems;
   struct air_val **elems;
};

/* One member of an argument buffer, as `air.struct_type_info` declares it and
 * the module's own struct type confirms it.
 *
 * Keyed on `offset` and not on the member number, which is what makes the
 * three shapes SPIRV-Cross emits one case: a buffer member is the pointer
 * itself, while a texture or sampler member is a one-field
 * `metal::texture2d` / `metal::sampler` wrapper whose field sits at byte 0 of
 * it -- so the extra `getelementptr` into the wrapper adds nothing to the
 * offset and resolves by the same lookup (docs/air.md section 8.2).
 */
struct air_argbuf_member {
   uint64_t offset;        /* bytes from the start of the set: the KEY */
   enum air_resource_kind kind;
   unsigned metal_id;      /* the member's own air.location_index: Metal's [[id(n)]] */
   unsigned index;         /* the flat per-kind index this frontend allocated */

   /* A buffer member: 1 device, 2 constant. The metadata does not say -- not
    * one of the 82 048 members of Resident Evil 4's 7 857 argument buffers
    * carries `air.address_space` -- so this comes from the module's own struct
    * type, and every load through the member checks the pointer it loads
    * against it (argbuf_load). 0 for a texture or a sampler.
    */
   unsigned addrspace;
   bool read, write;
   const char *name, *type_name;
   const struct air_md *node;   /* the member's own air.indirect_argument descriptor */
};

struct air_argbuf {
   unsigned metal_index;   /* the SET's own Metal [[buffer(n)]] */
   unsigned size;          /* air.buffer_size: eight bytes per member */
   unsigned param;         /* the entry point's parameter the set arrives as */
   const char *name, *type_name;
   unsigned num_members;
   struct air_argbuf_member *members;
};

struct air_ctx {
   void *mem_ctx;
   const struct air_module *m;
   const struct air_function *fn;
   nir_shader *nir;
   nir_builder b;

   const struct air_to_nir_options *options;
   struct air_shader_info *info;

   /* translated values, keyed on const struct air_value * */
   struct hash_table *values;
   /* air_block * -> nir_block * */
   struct hash_table *blocks;
   /* a cursor in each block, before its terminator, where phi stores go */
   struct hash_table *phi_cursors;
   /* phi -> nir register decl */
   struct hash_table *phi_regs;
   /* Where this function's constants are built: the top of its entry block,
    * which dominates every use of them. air_get caches a constant the first
    * time it is asked for, so the def has to be somewhere every later block
    * can see; built at the cursor of the first use, a constant shared by a
    * branch and the block after it is a non-dominating use, and nir_repair_ssa
    * turns that into undef on the path that misses the branch.
    */
   nir_cursor const_cursor;
   /* set while a constant is being built there, so that the elements of an
    * aggregate constant -- which come through air_get too -- keep appending
    * after it instead of each restoring the cursor to where the aggregate
    * itself will go, which put a vec before the components it reads */
   bool in_const;

   /* A call of another function in the module becomes a nir_call, and
    * nir_inline_functions folds it in afterwards. These two carry the state
    * that needs: the functions already translated, so a callee shared by two
    * call sites is translated once, and -- while a callee is being
    * translated -- the deref its `ret` has to store through, since a NIR call
    * returns nothing and the convention is an out-parameter at index 0.
    * ret_deref is NULL while the entry point is being translated, which is how
    * emit_return tells the two apart.
    */
   struct hash_table *fns;
   /* specialised callees by what they were specialised on (spec_key): one translation per
    * distinct resolution of the arguments rather than one per call site */
   struct hash_table *spec_fns;
   nir_deref_instr *ret_deref;

   /* ... and, for a callee whose return type is a STRUCT, one out-parameter per
    * member instead of one for the whole value: a NIR function parameter
    * carries a number, and a struct is a tree of values in this translator and
    * never a NIR value at all. ret_fields is indexed by the AIR member number
    * and is NULL for a member that is not a number -- the `[8 x i8]` tail
    * padding that Metal's own vertex-output structs carry, which takes no
    * parameter because nothing may read or write it. num_ret_fields is
    * non-zero only while such a callee is being translated, and that is how
    * emit_return tells this case from the scalar one.
    */
   nir_deref_instr **ret_fields;
   unsigned num_ret_fields;
   const struct air_type *ret_type;   /* the struct the leaves above are the numbers of */

   /* What each module global resolves to, decided once (plan_globals) and
    * turned into a value in whichever function needs it (materialise_globals).
    * The two are separate because the deciding half has side effects that must
    * happen once -- it appends inline samplers to info->resources and lays out
    * shared and scratch memory -- while the making half emits an nir_def for
    * the byte offset, and a def belongs to the impl that created it. Copying
    * the entry point's values into a callee instead left the callee using the
    * caller's defs, which is a cross-function def/use: nir_repair_ssa then
    * asks the wrong function for its dominance metadata and asserts.
    */
   struct air_global_plan {
      enum air_ptr_class class;
      unsigned binding;
      unsigned offset;      /* bytes, when has_offset */
      bool has_offset;
      bool skip;            /* llvm.* lists, and anything that failed */
      struct nir_variable *var;   /* AIR_PTR_VARIABLE */
   } *global_plan;

   /* What the static initializers store to each global, run once on this translation's
    * function constants (air_static_value): global -> struct air_eval, NULL when unknown */
   struct hash_table *static_stores;
   /* pointers stored into private memory at a constant offset (AIR_PTR_SCRATCH): byte offset + 1
    * -> the struct air_val stored there; a load of a pointer from the same place is that value */
   struct hash_table_u64 *ptr_slots;
   void *static_mem;

   /* what the entry point's parameters are */
   unsigned num_params;
   struct air_val **params;

   /* the argument buffers the entry point declares, in the order its input
    * metadata lists them, which is the order their members are numbered in */
   unsigned num_argbufs;
   struct air_argbuf *argbufs;

   /* outputs, in the order of the returned struct's fields */
   unsigned num_outputs;
   struct {
      unsigned location;      /* gl_varying_slot or gl_frag_result */
      unsigned index;         /* dual-source index */
      nir_variable *var;
      bool gated_off;         /* its function-constant predicate is false: stored nowhere */
   } *outputs;

   unsigned scratch_size;
   unsigned shared_size;

   /* module-scope variables, by their air_global */
   struct hash_table *globals;

   char error[256];
};

bool air_error(struct air_ctx *c, const char *fmt, ...) PRINTFLIKE(2, 3);

/* value bookkeeping */
struct air_val *air_val_ssa(struct air_ctx *c, nir_def *def);
struct air_val *air_val_ptr(struct air_ctx *c, struct air_ptr ptr);
struct air_val *air_get(struct air_ctx *c, const struct air_value *v);

/* a two-way split inside one AIR block (air_to_nir.c): the impl is unstructured */
struct air_split {
   struct nir_block *then_block, *else_block, *merge_block;
};
void air_split_begin(struct air_ctx *c, nir_def *cond, struct air_split *sp);
void air_split_else(struct air_ctx *c, struct air_split *sp);
void air_split_end(struct air_ctx *c, struct air_split *sp);
struct air_val *air_split_keep(struct air_ctx *c, struct air_val *v, struct air_val *regs);
struct air_val *air_split_load(struct air_ctx *c, struct air_val *regs);
void air_set(struct air_ctx *c, const struct air_value *v, struct air_val *val);

/* an SSA value, with aggregates and pointers refused */
nir_def *air_get_ssa(struct air_ctx *c, const struct air_value *v);

/* the NIR type of an AIR type: bit size and components */
unsigned air_nir_bit_size(struct air_ctx *c, const struct air_type *t);
unsigned air_nir_components(const struct air_type *t);

/* Records that the shader was translated with a caveat: what it asked for and
 * what was done instead (air_shader_info.notes). Not an error -- translation
 * continues -- and not a warning about our own code: it is for a thing the
 * hardware or this frontend cannot do exactly, where doing something else is
 * still correct. Refusing is air_error; passing over it in silence is the one
 * option this is here to remove.
 */
void air_note(struct air_ctx *c, const char *fmt, ...) PRINTFLIKE(2, 3);

/* memory, by the pointer's class */
nir_def *air_load_ptr(struct air_ctx *c, struct air_ptr ptr, const struct air_type *type,
                      unsigned align);
bool air_store_ptr(struct air_ctx *c, struct air_ptr ptr, nir_def *value, unsigned align);
struct air_ptr air_ptr_add(struct air_ctx *c, struct air_ptr ptr, nir_def *bytes);

/* air.* and llvm.* calls; the result is set on the instruction by the callee */
bool air_translate_call(struct air_ctx *c, const struct air_inst *in, const char *name);
/* a call of a function defined in this module, folded in by nir_inline_functions */
bool air_call_in_module(struct air_ctx *c, const struct air_inst *in, const char *name);

#endif /* AIR_PRIVATE_H */
