/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * One AIR module -> one nir_shader. The mapping from AIR's named metadata to
 * NIR's variables, bindings and system values is the table in docs/air.md
 * section 3.1; this file is that table in code, and nothing here decides
 * anything the table does not state.
 */

#include "air_private.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "util/hash_table.h"
#include "util/u_math.h"

bool
air_error(struct air_ctx *c, const char *fmt, ...)
{
   if (!c->error[0]) {
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(c->error, sizeof(c->error), fmt, ap);
      va_end(ap);
   }
   return false;
}

void
air_note(struct air_ctx *c, const char *fmt, ...)
{
   struct air_shader_info *info = c->info;
   va_list ap;
   va_start(ap, fmt);
   char *text = ralloc_vasprintf(c->mem_ctx, fmt, ap);
   va_end(ap);
   /* the same note twice says nothing twice: two resources in one group, or a
    * callee translated for two call sites, would each add one */
   for (unsigned i = 0; i < info->num_notes; i++)
      if (!strcmp(info->notes[i], text))
         return;
   info->notes = reralloc(c->mem_ctx, info->notes, const char *, info->num_notes + 1);
   info->notes[info->num_notes++] = text;
}

/* -- values ------------------------------------------------------------- */

struct air_val *
air_val_ssa(struct air_ctx *c, nir_def *def)
{
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_SSA;
   v->def = def;
   return v;
}

struct air_val *
air_val_ptr(struct air_ctx *c, struct air_ptr ptr)
{
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_PTR;
   v->ptr = ptr;
   return v;
}

static struct air_val *
air_val_ptr_select(struct air_ctx *c, nir_def *cond,
                   struct air_val *a, struct air_val *b)
{
   /* A condition that is a constant -- a function constant's, once the pipeline has given
    * it a value, which is how iOS 26's QuartzCore chooses between its inline sampler and the
    * one it was given (`select i1 %fc, @__air_sampler_state, %sampler`) -- chooses now. */
   if (nir_src_is_const(nir_src_for_ssa(cond)))
      return nir_src_as_bool(nir_src_for_ssa(cond)) ? a : b;
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_PTR_SELECT;
   v->def = cond;
   v->num_elems = 2;
   v->elems = rzalloc_array(c->mem_ctx, struct air_val *, 2);
   v->elems[0] = a;
   v->elems[1] = b;
   return v;
}

/* The slot key of a private-memory pointer at a constant byte offset (ptr_slots), or false. */
static bool
ptr_slot_of(const struct air_ptr *p, uint64_t *slot)
{
   if (p->offset && !nir_src_is_const(nir_src_for_ssa(p->offset)))
      return false;
   *slot = (p->offset ? nir_src_as_uint(nir_src_for_ssa(p->offset)) : 0) + 1;
   return true;
}

/* Whether a pointer argument's byte offset goes to the callee as a parameter: one that is not a
 * constant. A constant offset is built again inside the specialised callee (translate_callee). */
static bool
ptr_offset_is_param(const struct air_ctx *c, const struct air_val *v)
{
   /* A constant offset is kept only where a pointer has been stored into private memory, which
    * ptr_slots needs to find again; otherwise it goes in as a parameter, so that call sites
    * differing only in where they point share one translation (spec_fns: QuartzCore's
    * apply_color_opcode, called 12 times per specialisation at 12 offsets of one local array) */
   if (!v || v->kind != AIR_VAL_PTR || !v->ptr.offset)
      return false;
   if (!nir_src_is_const(nir_src_for_ssa(v->ptr.offset)))
      return true;
   return !(v->ptr.class == AIR_PTR_SCRATCH && c->ptr_slots &&
            _mesa_hash_table_u64_num_entries(c->ptr_slots));
}

/* Is this value a pointer in any of the three senses? */
static bool
val_is_pointerish(const struct air_val *v)
{
   return v && (v->kind == AIR_VAL_PTR || v->kind == AIR_VAL_PTR_SELECT);
}

/* Two resolutions that are the same resolution.
 *
 * This is what lets a `phi` or a `select` between pointers be answered without
 * choosing anything at run time, and it is needed rather than merely tidy:
 * LLVM duplicates a `getelementptr` onto two paths and then phis the copies
 * back together, so 207 of the pointer phis in Resident Evil 4's library have
 * two operands that name ONE member of ONE argument buffer. A pointer whose
 * byte offset is not a constant is compared by the def itself -- the same def
 * is the same offset, and two different defs cannot be proved equal here.
 */
static bool
same_ptr(const struct air_ptr *a, const struct air_ptr *b)
{
   if (a->class != b->class || a->binding != b->binding || a->is_image != b->is_image ||
       a->var != b->var)
      return false;
   if (a->offset == b->offset)
      return true;
   if (!a->offset || !b->offset)
      return false;
   if (!nir_src_is_const(nir_src_for_ssa(a->offset)) ||
       !nir_src_is_const(nir_src_for_ssa(b->offset)))
      return false;
   return nir_src_as_uint(nir_src_for_ssa(a->offset)) ==
          nir_src_as_uint(nir_src_for_ssa(b->offset));
}

static struct air_val *
air_val_agg(struct air_ctx *c, unsigned n)
{
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_AGG;
   v->num_elems = n;
   v->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(n, 1));
   return v;
}

void
air_set(struct air_ctx *c, const struct air_value *v, struct air_val *val)
{
   _mesa_hash_table_insert(c->values, v, val);
}

unsigned
air_nir_bit_size(struct air_ctx *c, const struct air_type *t)
{
   unsigned bits = air_type_bit_size(air_type_scalar(t));
   /* NIR has no i1 in SSA: booleans are 1-bit defs, which it does have.
    *
    * It has no 2- or 4-bit integer at all, though, and that is not a
    * hypothetical: an HLSL bit field reaches AIR as `i2` or `i4` through
    * SPIRV-Cross, and every NIR builder handed one ASSERTS instead of
    * refusing -- "Invalid Boolean size", "Invalid bit size" and "Invalid nir
    * alu bit size" between them aborted 495 of Resident Evil 4's modules the
    * first time the argument-buffer interface let them get as far as their
    * bodies. An abort is not a refusal: it says nothing a report can count and
    * it takes the run with it. So the width is named here, once, and 32 is
    * returned so that the instruction under construction stays well formed
    * while the error just set discards the shader.
    */
   if (bits != 1 && bits != 8 && bits != 16 && bits != 32 && bits != 64) {
      /* Zero is a different fact wearing the same symptom, and saying so in one
       * message made 79 modules read as carrying a 0-bit integer when what they
       * carry is a clip-distance ARRAY as a shader output: air_type_bit_size has
       * no answer for an aggregate, and this used to return it unexamined, so
       * `[4 x float]` became a one-component float output and four clip planes
       * became one with nothing in any log. A width NIR cannot hold and a type
       * that has no single width are both refusals, and they are not the same
       * refusal.
       */
      if (!bits)
         air_error(c, "instruction: %s where a number belongs, which has no single "
                      "NIR width",
                   t->kind == AIR_TYPE_ARRAY ? "an array" :
                   t->kind == AIR_TYPE_STRUCT ? "a struct" :
                   t->kind == AIR_TYPE_POINTER ? "a pointer" : "a value of no size");
      else
         air_error(c, "instruction: a %u-bit integer, which is a width NIR does not have",
                   bits);
      return 32;
   }
   return bits;
}

unsigned
air_nir_components(const struct air_type *t)
{
   return t->kind == AIR_TYPE_VECTOR ? t->count : 1;
}

/* The glsl type a NIR function parameter needs to carry a value of this AIR
 * type, matching what air_nir_bit_size and air_nir_components say the def will
 * be. NIR tracks a value's real type by use, so the only thing that has to be
 * right here is the shape.
 */
static const struct glsl_type *
param_glsl_type(struct air_ctx *c, const struct air_type *t)
{
   /* of the value's own width: a 32-bit local holding an i1 the callee stores is a phi of two
    * widths to nir_lower_vars_to_ssa (RenderBox's uber_fragment returns bools from its helpers) */
   unsigned bits = air_nir_bit_size(c, t);
   return glsl_vector_type(bits == 1 ? GLSL_TYPE_BOOL : bits == 8 ? GLSL_TYPE_UINT8 :
                           bits == 16 ? GLSL_TYPE_FLOAT16 :
                           bits == 64 ? GLSL_TYPE_UINT64 : GLSL_TYPE_UINT,
                           air_nir_components(t));
}

/* Whether a struct member is a number this frontend can hand through a
 * function parameter -- a scalar or a vector of one. Everything else takes no
 * parameter: an array, a nested struct, a pointer. In practice the one that
 * occurs is the tail padding Metal's compiler puts on a vertex-output struct
 * (`[8 x i8]`), which the shader never writes and the caller never reads.
 */
static bool
ret_member_is_number(const struct air_type *m)
{
   if (m->kind != AIR_TYPE_VECTOR && m->kind != AIR_TYPE_INT && !air_type_is_float(m))
      return false;
   const struct air_type *sc = air_type_scalar(m);
   return sc->kind == AIR_TYPE_INT || air_type_is_float(sc);
}

/* The numbers of a returned struct, depth first: a member that is a number, and the numbers of a
 * member that is a struct. One out-parameter each. Nested structs were skipped like padding
 * until MercuryPoster's wallpaper (Space.metallib, Flocon::getInputs) returned one inside
 * FloconInputs and read it back with extractvalue: "a struct where a number belongs". */
static unsigned
ret_leaves(const struct air_type *t, const struct air_type **out, unsigned n)
{
   if (ret_member_is_number(t)) {
      if (out)
         out[n] = t;
      return n + 1;
   }
   if (t->kind == AIR_TYPE_STRUCT)
      for (unsigned i = 0; i < t->num_members; i++)
         n = ret_leaves(t->members[i], out, n);
   return n;
}

static struct air_val *air_constant_val(struct air_ctx *c, const struct air_constant *k);

/* the callee's side: each number of the returned value into its out-parameter (a member the
 * callee never inserted is undef, and its parameter is left alone) */
static bool
store_ret_leaves(struct air_ctx *c, const struct air_type *t, const struct air_val *v, unsigned *at)
{
   if (ret_member_is_number(t)) {
      nir_deref_instr *d = c->ret_fields[(*at)++];
      if (!v)
         return true;
      if (v->kind != AIR_VAL_SSA)
         return air_error(c, "instruction: a member of a returned struct is not a number, "
                             "from a function called in the module");
      nir_store_deref(&c->b, d, v->def, nir_component_mask(v->def->num_components));
      return true;
   }
   if (t->kind != AIR_TYPE_STRUCT)
      return true;
   if (v && (v->kind != AIR_VAL_AGG || v->num_elems != t->num_members))
      return air_error(c, "instruction: a returned struct that was never built, "
                          "from a function called in the module");
   for (unsigned i = 0; i < t->num_members; i++)
      if (!store_ret_leaves(c, t->members[i], v ? v->elems[i] : NULL, at))
         return false;
   return true;
}

/* and the caller's: the value rebuilt from the out-parameters, as the tree of the struct's type
 * (what is not a number -- padding arrays, pointers -- is an undef element, as before) */
static struct air_val *
load_ret_leaves(struct air_ctx *c, const struct air_type *t, nir_variable **fields, unsigned *at)
{
   if (ret_member_is_number(t))
      return air_val_ssa(c, nir_load_var(&c->b, fields[(*at)++]));
   if (t->kind != AIR_TYPE_STRUCT)
      return NULL;
   struct air_val *agg = air_val_agg(c, t->num_members);
   for (unsigned i = 0; i < t->num_members; i++)
      agg->elems[i] = load_ret_leaves(c, t->members[i], fields, at);
   return agg;
}

struct air_val *
air_get(struct air_ctx *c, const struct air_value *v)
{
   struct hash_entry *e = _mesa_hash_table_search(c->values, v);
   if (e)
      return e->data;
   const struct air_constant *k = air_value_as_constant(v);
   if (k) {
      /* A constant is built once and then reused from wherever it is asked
       * for, so it must be somewhere that dominates all of those places. A
       * constant has no operands, so the top of the entry block always will.
       *
       * Built at the cursor of its first use instead, a constant that LLVM
       * shares between a conditional block and a block after it is a use that
       * its def does not dominate -- and nothing complains, because
       * nir_lower_goto_ifs runs nir_repair_ssa, which repairs such a use into
       * a phi whose other edge is undef. That is how `phi i16 [ %30, %27 ],
       * [ -1, %22 ]` in MetalFaceMask_AddSeedPoint became `phi b3: %92,
       * b4: %0 (undefined)`: the -1 was the same constant as the one the
       * compare after the branch uses, so the store on the entry edge pointed
       * at a def inside the branch (docs/air.md section 5).
       */
      bool outer = !c->in_const;
      nir_cursor use = c->b.cursor;
      if (outer) {
         c->in_const = true;
         c->b.cursor = c->const_cursor;
      }
      struct air_val *val = air_constant_val(c, k);
      if (outer) {
         c->const_cursor = c->b.cursor;
         c->b.cursor = use;
         c->in_const = false;
      }
      if (val)
         air_set(c, v, val);
      return val;
   }
   air_error(c, "instruction: a %s value used before it is defined%s%s",
             v->kind == AIR_VALUE_GLOBAL ? "global" :
             v->kind == AIR_VALUE_FUNCTION ? "function" :
             v->kind == AIR_VALUE_ARGUMENT ? "argument" :
             v->kind == AIR_VALUE_INSTRUCTION ? "computed" : "metadata",
             v->name ? " " : "", v->name ? v->name : "");
   return NULL;
}

nir_def *
air_get_ssa(struct air_ctx *c, const struct air_value *v)
{
   struct air_val *val = air_get(c, v);
   if (!val)
      return NULL;
   if (val->kind != AIR_VAL_SSA) {
      air_error(c, "instruction: %s where a number belongs",
                val->kind == AIR_VAL_PTR ? "a pointer" :
                val->kind == AIR_VAL_PTR_SELECT ? "a chosen pointer" : "an aggregate");
      return NULL;
   }
   return val->def;
}

/* -- constants ---------------------------------------------------------- */

static nir_def *
scalar_constant(struct air_ctx *c, const struct air_type *t, uint64_t bits)
{
   unsigned n = air_type_bit_size(t);
   if (t->kind == AIR_TYPE_INT && t->bits == 1)
      return nir_imm_bool(&c->b, bits & 1);
   if (air_type_is_float(t)) {
      switch (n) {
      case 16: return nir_imm_intN_t(&c->b, bits, 16);
      case 32: return nir_imm_int(&c->b, (uint32_t)bits);
      case 64: return nir_imm_int64(&c->b, bits);
      default: break;
      }
   }
   return nir_imm_intN_t(&c->b, bits, MAX2(n, 1));
}

/* The float bits of a constant have to come through unchanged: a float
 * constant is built as an integer of the same bits and reinterpreted, so
 * that no NaN payload or denormal is rewritten on the way.
 */
static nir_def *
reinterpret_float(struct air_ctx *c, const struct air_type *t, nir_def *bits)
{
   if (!air_type_is_float(t))
      return bits;
   return nir_mov(&c->b, bits); /* same bits, and NIR tracks the type by use */
}

static struct air_val *
air_constant_val(struct air_ctx *c, const struct air_constant *k)
{
   const struct air_type *t = k->base.type;

   switch (k->ckind) {
   case AIR_CONST_INT:
   case AIR_CONST_FLOAT:
      return air_val_ssa(c, reinterpret_float(c, t, scalar_constant(c, air_type_scalar(t), k->u)));

   case AIR_CONST_NULL:
      if (t->kind == AIR_TYPE_POINTER) {
         struct air_ptr p = { .class = AIR_PTR_NONE, .pointee = t->elem };
         return air_val_ptr(c, p);
      }
      if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
         unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : t->count;
         struct air_val *v = air_val_agg(c, n);
         for (unsigned i = 0; i < n; i++) {
            const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
            struct air_constant z = { .base = { .kind = AIR_VALUE_CONSTANT, .type = et },
                                      .ckind = AIR_CONST_NULL };
            if (!(v->elems[i] = air_constant_val(c, &z)))
               return NULL;
         }
         return v;
      }
      return air_val_ssa(c, nir_imm_zero(&c->b, air_nir_components(t), air_nir_bit_size(c, t)));

   case AIR_CONST_UNDEF:
   case AIR_CONST_POISON:
      if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
         unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : t->count;
         struct air_val *v = air_val_agg(c, n);
         /* Scalar fields stay NONE until insertvalue puts something there; a field that is
          * itself a struct or an array gets its own skeleton, so that a nested insertvalue
          * into a poison value -- `insertvalue [2 x %Image] poison, i8 %x, 0, 0` in iOS 26's
          * QuartzCore -- has somewhere to go. */
         for (unsigned i = 0; i < n; i++) {
            const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
            if (et->kind == AIR_TYPE_STRUCT || et->kind == AIR_TYPE_ARRAY) {
               struct air_constant u = { .base = { .kind = AIR_VALUE_CONSTANT, .type = et },
                                         .ckind = k->ckind };
               if (!(v->elems[i] = air_constant_val(c, &u)))
                  return NULL;
            }
         }
         return v;
      }
      if (t->kind == AIR_TYPE_POINTER) {
         struct air_ptr p = { .class = AIR_PTR_NONE, .pointee = t->elem };
         return air_val_ptr(c, p);
      }
      return air_val_ssa(c, nir_undef(&c->b, air_nir_components(t), air_nir_bit_size(c, t)));

   case AIR_CONST_AGGREGATE: {
      if (t->kind == AIR_TYPE_VECTOR) {
         nir_def *comps[NIR_MAX_VEC_COMPONENTS];
         if (k->num_elems > NIR_MAX_VEC_COMPONENTS)
            return air_error(c, "instruction: a vector constant of %u elements", k->num_elems), NULL;
         for (unsigned i = 0; i < k->num_elems; i++)
            if (!(comps[i] = air_get_ssa(c, k->elems[i])))
               return NULL;
         return air_val_ssa(c, nir_vec(&c->b, comps, k->num_elems));
      }
      struct air_val *v = air_val_agg(c, k->num_elems);
      for (unsigned i = 0; i < k->num_elems; i++)
         if (!(v->elems[i] = air_get(c, k->elems[i])))
            return NULL;
      return v;
   }

   case AIR_CONST_DATA: {
      const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? NULL : t->elem;
      if (!et)
         return air_error(c, "instruction: a data constant of a struct type"), NULL;
      if (t->kind == AIR_TYPE_VECTOR) {
         nir_def *comps[NIR_MAX_VEC_COMPONENTS];
         if (k->num_elems > NIR_MAX_VEC_COMPONENTS)
            return air_error(c, "instruction: a vector constant of %u elements", k->num_elems), NULL;
         for (unsigned i = 0; i < k->num_elems; i++)
            comps[i] = reinterpret_float(c, et, scalar_constant(c, et, k->data[i]));
         return air_val_ssa(c, nir_vec(&c->b, comps, k->num_elems));
      }
      struct air_val *v = air_val_agg(c, k->num_elems);
      for (unsigned i = 0; i < k->num_elems; i++)
         v->elems[i] = air_val_ssa(c, reinterpret_float(c, et, scalar_constant(c, et, k->data[i])));
      return v;
   }

   case AIR_CONST_EXPR:
      /* with opaque pointers the only constant expression left in the corpus
       * is a pointer bitcast, which is the pointer itself
       */
      if (k->expr_op == AIR_OP_CAST && k->expr_sub == AIR_CAST_BITCAST &&
          k->base.type->kind == AIR_TYPE_POINTER)
         return air_get(c, k->operands[0]);
      /* ... and a constant getelementptr, which the GL-on-Metal vertex shaders
       * use to name the first element of a module array
       * (`getelementptr inbounds ([2 x <4 x float>], ptr @g, i64 0, i64 0)`).
       * It is the instruction form with every index a constant, so the whole
       * walk is done here in integers and the result is the base pointer with
       * a constant byte offset and a narrowed pointee.
       */
      if (k->expr_op == AIR_OP_GEP) {
         if (k->num_operands < 2 || !k->src_type)
            return air_error(c, "instruction: a constant getelementptr with no base "
                                "or no source type"), NULL;
         struct air_val *base = air_get(c, k->operands[0]);
         if (!base)
            return NULL;
         if (base->kind != AIR_VAL_PTR)
            return air_error(c, "instruction: a constant getelementptr on something "
                                "that is not a pointer"), NULL;
         struct air_ptr p = base->ptr;
         const struct air_type *t = k->src_type;
         uint64_t off = 0, idx = 0;
         if (!air_const_uint(k->operands[1], &idx))
            return air_error(c, "instruction: a constant getelementptr whose first "
                                "index is not a constant"), NULL;
         off += idx * air_type_alloc_size(c->m, t);
         for (unsigned i = 2; i < k->num_operands; i++) {
            if (!air_const_uint(k->operands[i], &idx))
               return air_error(c, "instruction: a constant getelementptr with an index "
                                   "that is not a constant"), NULL;
            if (t->kind == AIR_TYPE_STRUCT) {
               if (idx >= t->num_members)
                  return air_error(c, "instruction: a constant getelementptr to struct "
                                      "field %u of %u", (unsigned)idx,
                                   t->num_members), NULL;
               off += air_struct_member_offset(c->m, t, idx);
               t = t->members[idx];
            } else if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_VECTOR) {
               off += idx * (t->kind == AIR_TYPE_ARRAY
                                ? air_type_alloc_size(c->m, t->elem)
                                : MAX2(air_type_bit_size(t->elem) / 8, 1));
               t = t->elem;
            } else {
               return air_error(c, "instruction: a constant getelementptr into a "
                                   "scalar"), NULL;
            }
         }
         p.pointee = t;
         if (off)
            p = air_ptr_add(c, p, nir_imm_int(&c->b, off));
         return air_val_ptr(c, p);
      }
      return air_error(c, "instruction: constant expression %u", k->expr_op), NULL;
   }
   return air_error(c, "instruction: constant kind %u", k->ckind), NULL;
}

/* -- memory ------------------------------------------------------------- */

struct air_ptr
air_ptr_add(struct air_ctx *c, struct air_ptr ptr, nir_def *bytes)
{
   if (!bytes)
      return ptr;
   if (!ptr.offset) {
      ptr.offset = bytes;
      return ptr;
   }
   /* Two constant offsets become one constant, and that is a correctness fix
    * rather than a tidiness.
    *
    * A cached constant is built in the const region at the top of the entry
    * block (air_get), while a module-scope variable's byte offset is built
    * where materialise_globals runs, which is BELOW that region. A constant
    * `getelementptr` into a threadgroup array -- `load i32, ptr addrspace(3)
    * getelementptr([16 x i32], ptr @tbl, i64 0, i64 9)`, which is how
    * SPIRV-Cross reads a fixed slot of one -- is a constant whose value was
    * therefore `iadd %globaloffset, 36` sitting above the def of
    * %globaloffset: a use its def does not dominate, which nir_validate
    * rejects with "src->_parent & SRC_TAG_SEEN". 115 of Resident Evil 4's
    * modules are that shape, and every one of them ABORTED the translator the
    * first time the argument-buffer interface let their bodies be reached.
    *
    * Folding the pair removes the reference: a global's offset is always a
    * constant here, so the whole chain is one immediate built in the const
    * region with no operand at all. The latent ordering hazard it stands on is
    * written down in docs/air.md rather than left to be rediscovered.
    */
   if (nir_src_is_const(nir_src_for_ssa(ptr.offset)) &&
       nir_src_is_const(nir_src_for_ssa(bytes)) &&
       ptr.offset->bit_size == 32 && bytes->bit_size == 32 &&
       ptr.offset->num_components == 1 && bytes->num_components == 1) {
      uint32_t sum = (uint32_t)nir_src_as_uint(nir_src_for_ssa(ptr.offset)) +
                     (uint32_t)nir_src_as_uint(nir_src_for_ssa(bytes));
      ptr.offset = nir_imm_int(&c->b, sum);
      return ptr;
   }
   ptr.offset = nir_iadd(&c->b, ptr.offset, bytes);
   return ptr;
}

static nir_def *
ptr_offset(struct air_ctx *c, struct air_ptr ptr)
{
   return ptr.offset ? ptr.offset : nir_imm_int(&c->b, 0);
}

static nir_def *air_static_value(struct air_ctx *c, const struct air_value *g, const struct air_type *type);
static nir_def *air_static_field(struct air_ctx *c, nir_def *offset, const struct air_type *type);
static const struct air_value *air_global_of_variable(struct air_ctx *c, nir_variable *var);
static const struct air_value *air_global_at_scratch(struct air_ctx *c, nir_def *offset, const struct air_type *type);

nir_def *
air_load_ptr(struct air_ctx *c, struct air_ptr ptr, const struct air_type *type, unsigned align)
{
   unsigned comps = air_nir_components(type);
   unsigned bits = air_nir_bit_size(c, type);
   if (type->kind == AIR_TYPE_POINTER)
      return air_error(c, "instruction: a pointer loaded from memory (argument buffers)"), NULL;
   if (!align)
      align = MAX2(bits / 8, 1);

   /* a 1-bit load is a byte in memory */
   bool as_bool = bits == 1;
   if (as_bool)
      bits = 8;

   nir_def *off = ptr_offset(c, ptr);
   nir_def *res;
   switch (ptr.class) {
   case AIR_PTR_UBO:
      res = nir_load_ubo(&c->b, comps, bits, nir_imm_int(&c->b, ptr.binding), off,
                         .align_mul = align, .align_offset = 0, .range_base = 0, .range = ~0);
      break;
   case AIR_PTR_SSBO:
      res = nir_load_ssbo(&c->b, comps, bits, nir_imm_int(&c->b, ptr.binding), off,
                          .align_mul = align, .align_offset = 0);
      break;
   case AIR_PTR_SHARED:
      res = nir_load_shared(&c->b, comps, bits, off, .base = 0, .align_mul = align);
      break;
   case AIR_PTR_SCRATCH:
      /* (a program-scope variable the static initializers alone decide: its value, see air_static_value) */
      if ((res = air_static_value(c, air_global_at_scratch(c, off, type), type)) != NULL)
         break;
      if ((res = air_static_field(c, off, type)) != NULL)
         break;
      res = nir_load_scratch(&c->b, comps, bits, off, .align_mul = align);
      break;
   case AIR_PTR_VARIABLE:
      if (ptr.offset && !(nir_src_is_const(nir_src_for_ssa(ptr.offset)) &&
                          nir_src_as_uint(nir_src_for_ssa(ptr.offset)) == 0))
         return air_error(c, "instruction: a load inside a program-scope variable"), NULL;
      if ((res = air_static_value(c, air_global_of_variable(c, ptr.var), type)) != NULL)
         break;
      res = nir_load_deref(&c->b, nir_build_deref_var(&c->b, ptr.var));
      if (res->num_components != comps || res->bit_size != bits)
         return air_error(c, "instruction: a load of a program-scope variable as another "
                             "type"), NULL;
      break;
   case AIR_PTR_CONSTANT_DATA:
      res = nir_load_constant(&c->b, comps, bits, off, .base = 0, .range = c->nir->constant_data_size,
                              .align_mul = align);
      break;
   case AIR_PTR_FUNCTION_CONSTANT: {
      /* its value is known at pipeline creation, so the load is that value */
      const struct air_constant_value *v = NULL;
      for (unsigned i = 0; c->options && i < c->options->num_constants; i++)
         if (c->options->constants[i].index == ptr.binding)
            v = &c->options->constants[i];
      if (!v)
         return air_error(c, "interface: function constant %u has no value", ptr.binding), NULL;
      /* An undefined constant reads as zero. Metal leaves such a read to the shader to
       * guard with is_function_constant_defined(), which then answers false; Apple's
       * libraries load the value unconditionally and select on that answer, so the load
       * has to translate whatever it reads. */
      if (comps == 1) {
         res = nir_imm_intN_t(&c->b, v->undefined ? 0 : v->bits, as_bool ? 8 : bits);
         break;
      }
      /* A VECTOR function constant. Metal allows one and the GL-on-Metal
       * fragment shaders use two: `uchar3` opcode triples for the texture
       * combiners (`fc_color_opcodes0/1`, declared `<3 x i8>`).
       *
       * air_constant_value carries one 64-bit word per constant, so the
       * components have to be fields of it, and the packing is this frontend's
       * own interface with the runtime rather than something read out of
       * Apple's: component i sits at bit i*width, which is the word a memcpy
       * of the value would produce on this little-endian target. NOTHING HERE
       * MEASURES THAT -- every harness in tools/air supplies zero for every
       * constant, so all components are zero whatever the packing, and the
       * first thing that would tell a wrong packing from a right one is a
       * pipeline that sets one of these two constants.
       */
      if (comps * bits > 64 * (1 + ARRAY_SIZE(v->more)))
         return air_error(c, "interface: a function constant of %u components of %u bits, "
                             "which does not fit the four words air_constant_value carries",
                          comps, bits), NULL;
      /* iOS 26's RenderBox declares uint4 and float4 constants (128 bits), which is why
       * the value is four words now and no longer one */
      nir_def *fields[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < comps; i++) {
         uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
         unsigned at = i * bits;
         uint64_t word = at / 64 ? v->more[at / 64 - 1] : v->bits;
         fields[i] = nir_imm_intN_t(&c->b, v->undefined ? 0 : (word >> (at % 64)) & mask,
                                    as_bool ? 8 : bits);
      }
      res = nir_vec(&c->b, fields, comps);
      break;
   }
   case AIR_PTR_ARGBUF:
      /* argbuf_load answers a load through an argument buffer before it gets
       * here, so this is a load that resolution could not make: through a
       * pointer CHOSEN at run time between two sets, which load_through_choice
       * would read from both of. There is nothing to read either way -- the
       * bytes in an argument buffer are the runtime's 8-byte handles.
       */
      return air_error(c, "instruction: a load of bytes out of an argument buffer"), NULL;
   case AIR_PTR_NONE:
      /* Through null. iOS 26's QuartzCore passes null for a buffer its function constants
       * say is absent (`select i1 %fc, ptr null, ptr %weights`) to a helper whose only call
       * of it is behind the same constant -- dead code that is translated all the same.
       * Metal leaves the read undefined; zero is what it reads here, and nothing reads it. */
      res = nir_imm_zero(&c->b, comps, bits);
      break;
   default:
      return air_error(c, "instruction: a load through a %s pointer",
                       ptr.class == AIR_PTR_NONE ? "null" : "texture or sampler"), NULL;
   }
   return as_bool ? nir_ine_imm(&c->b, res, 0) : res;
}

bool
air_store_ptr(struct air_ctx *c, struct air_ptr ptr, nir_def *value, unsigned align)
{
   unsigned comps = value->num_components;
   if (value->bit_size == 1) {
      value = nir_b2iN(&c->b, value, 8);
   }
   if (!align)
      align = MAX2(value->bit_size / 8, 1);

   nir_def *off = ptr_offset(c, ptr);
   switch (ptr.class) {
   case AIR_PTR_SSBO:
      nir_store_ssbo(&c->b, value, nir_imm_int(&c->b, ptr.binding), off,
                     .write_mask = BITFIELD_MASK(comps), .align_mul = align);
      return true;
   case AIR_PTR_SHARED:
      nir_store_shared(&c->b, value, off, .base = 0, .write_mask = BITFIELD_MASK(comps),
                       .align_mul = align);
      return true;
   case AIR_PTR_SCRATCH:
      nir_store_scratch(&c->b, value, off, .align_mul = align,
                        .write_mask = BITFIELD_MASK(comps));
      return true;
   case AIR_PTR_VARIABLE:
      if (ptr.offset && !(nir_src_is_const(nir_src_for_ssa(ptr.offset)) &&
                          nir_src_as_uint(nir_src_for_ssa(ptr.offset)) == 0))
         return air_error(c, "instruction: a store inside a program-scope variable");
      nir_store_deref(&c->b, nir_build_deref_var(&c->b, ptr.var), value,
                      BITFIELD_MASK(value->num_components));
      return true;
   case AIR_PTR_UBO:
   case AIR_PTR_CONSTANT_DATA:
      return air_error(c, "instruction: a store into constant memory");
   case AIR_PTR_ARGBUF:
      /* Writing a descriptor from inside the shader. The handle it would write
       * is the runtime's, and this frontend has resolved the set's members to
       * bindings rather than to addresses, so there is nothing to write.
       */
      return air_error(c, "instruction: a store into an argument buffer");
   case AIR_PTR_NONE:
      return true;      /* through null: dead code of the same kind as the load above */
   default:
      return air_error(c, "instruction: a store through a %s pointer",
                       ptr.class == AIR_PTR_NONE ? "null" : "texture or sampler");
   }
}

/* -- metadata helpers --------------------------------------------------- */

static const char *
md_str(const struct air_md *node, unsigned i)
{
   if (!node || i >= node->num_ops || !node->ops[i] || node->ops[i]->kind != AIR_MD_STRING)
      return NULL;
   return air_md_string(node->ops[i]);
}

static bool
md_int(const struct air_md *node, unsigned i, uint64_t *out)
{
   if (!node || i >= node->num_ops || !node->ops[i] || node->ops[i]->kind != AIR_MD_VALUE)
      return false;
   /* zero is written as a null constant, as everywhere else in the bitcode */
   return air_const_uint(node->ops[i]->value, out);
}

/* the index of the operand after the key, or -1 */
static int
md_key(const struct air_md *node, const char *key)
{
   for (unsigned i = 0; i < (node ? node->num_ops : 0); i++) {
      const char *s = md_str(node, i);
      if (s && !strcmp(s, key))
         return i;
   }
   return -1;
}

static bool
md_key_int(const struct air_md *node, const char *key, uint64_t *out)
{
   int i = md_key(node, key);
   return i >= 0 && md_int(node, i + 1, out);
}

/* the operand after the key, when it is a nested node: the shape Apple uses for
 * a key whose value is itself a list, as `air.function_constant` is
 * (`!{ptr @__metal_implicit_fc_pred_6, !"bool", !"vtx_attr_has_texcoords"}`)
 */
static const struct air_md *
md_key_node(const struct air_md *node, const char *key)
{
   int i = md_key(node, key);
   if (i < 0 || (unsigned)(i + 1) >= node->num_ops || !node->ops[i + 1] ||
       node->ops[i + 1]->kind != AIR_MD_NODE)
      return NULL;
   return node->ops[i + 1];
}

static const char *
md_key_str(const struct air_md *node, const char *key)
{
   int i = md_key(node, key);
   return i >= 0 ? md_str(node, i + 1) : NULL;
}

static const struct air_named_md *
named_md(const struct air_module *m, const char *name)
{
   for (unsigned i = 0; i < m->num_named_md; i++)
      if (!strcmp(m->named_md[i].name, name))
         return &m->named_md[i];
   return NULL;
}

/* -- stage -------------------------------------------------------------- */

struct stage_md {
   mesa_shader_stage stage;
   const char *md_name;
};

static const struct stage_md stages[] = {
   { MESA_SHADER_VERTEX, "air.vertex" },
   { MESA_SHADER_FRAGMENT, "air.fragment" },
   { MESA_SHADER_COMPUTE, "air.kernel" },
   { MESA_SHADER_NONE, "air.visible" },
};

static int
module_stage(const struct air_module *m, const struct air_md **entry)
{
   for (unsigned i = 0; i < ARRAY_SIZE(stages); i++) {
      const struct air_named_md *n = named_md(m, stages[i].md_name);
      if (n && n->num_ops >= 1 && n->ops[0] && n->ops[0]->kind == AIR_MD_NODE) {
         if (entry)
            *entry = n->ops[0];
         return stages[i].stage;
      }
   }
   return -2; /* no entry point at all */
}

int
air_module_stage(void *mem_ctx, const void *bitcode, size_t size, const char **calls,
                 const char **error)
{
   void *ctx = ralloc_context(mem_ctx);
   struct air_module *m = air_module_read(ctx, bitcode, size, error);
   if (!m) {
      ralloc_free(ctx);
      return -1;
   }

   if (calls) {
      size_t len = 1;
      for (unsigned i = 0; i < m->num_functions; i++) {
         const char *n = m->functions[i]->base.name;
         if (m->functions[i]->is_declaration && n && !strncmp(n, "air.", 4))
            len += strlen(n) + 1;
      }
      char *buf = rzalloc_size(mem_ctx, len), *p = buf;
      for (unsigned i = 0; i < m->num_functions; i++) {
         const char *n = m->functions[i]->base.name;
         if (m->functions[i]->is_declaration && n && !strncmp(n, "air.", 4)) {
            memcpy(p, n, strlen(n) + 1);
            p += strlen(n) + 1;
         }
      }
      *calls = buf;
   }

   int stage = module_stage(m, NULL);
   ralloc_free(ctx);
   return stage;
}

unsigned
air_module_function_constants(void *mem_ctx, const void *bitcode, size_t size,
                              struct air_constant_value **out)
{
   void *ctx = ralloc_context(mem_ctx);
   const char *err;
   struct air_module *m = air_module_read(ctx, bitcode, size, &err);
   *out = NULL;
   if (!m) {
      ralloc_free(ctx);
      return 0;
   }
   const struct air_named_md *n = named_md(m, "air.function_constants");
   unsigned num = 0;
   if (n && n->num_ops) {
      *out = rzalloc_array(mem_ctx, struct air_constant_value, n->num_ops);
      for (unsigned i = 0; i < n->num_ops; i++) {
         const struct air_md *node = n->ops[i];
         for (unsigned k = 1; k < (node ? node->num_ops : 0); k++) {
            uint64_t idx;
            if (md_int(node, k, &idx)) {
               (*out)[num++].index = idx;
               break;
            }
         }
      }
   }
   ralloc_free(ctx);
   return num;
}

/* -- the interface ------------------------------------------------------ */

/* AIR's names for what NIR calls system values */
enum air_sysval {
   AIR_SV_VERTEX_ID, AIR_SV_INSTANCE_ID, AIR_SV_BASE_VERTEX, AIR_SV_BASE_INSTANCE,
   AIR_SV_FRONT_FACING, AIR_SV_SAMPLE_ID, AIR_SV_SAMPLE_MASK_IN, AIR_SV_POINT_COORD,
   AIR_SV_THREAD_POS_GRID, AIR_SV_THREAD_POS_TG, AIR_SV_TG_POS_GRID,
   AIR_SV_THREADS_PER_TG, AIR_SV_TGS_PER_GRID, AIR_SV_THREAD_INDEX_TG,
   AIR_SV_THREADS_PER_GRID, AIR_SV_SIMD_LANE, AIR_SV_SIMD_INDEX, AIR_SV_SIMD_WIDTH,
   AIR_SV_SIMDS_PER_TG,
};

static const struct { const char *key; enum air_sysval sv; } sysvals[] = {
   { "air.vertex_id", AIR_SV_VERTEX_ID },
   { "air.instance_id", AIR_SV_INSTANCE_ID },
   { "air.base_vertex", AIR_SV_BASE_VERTEX },
   { "air.base_instance", AIR_SV_BASE_INSTANCE },
   { "air.front_facing", AIR_SV_FRONT_FACING },
   { "air.sample_id", AIR_SV_SAMPLE_ID },
   { "air.sample_mask_in", AIR_SV_SAMPLE_MASK_IN },
   { "air.point_coord", AIR_SV_POINT_COORD },
   { "air.thread_position_in_grid", AIR_SV_THREAD_POS_GRID },
   { "air.thread_position_in_threadgroup", AIR_SV_THREAD_POS_TG },
   { "air.threadgroup_position_in_grid", AIR_SV_TG_POS_GRID },
   { "air.threads_per_threadgroup", AIR_SV_THREADS_PER_TG },
   { "air.threadgroups_per_grid", AIR_SV_TGS_PER_GRID },
   { "air.thread_index_in_threadgroup", AIR_SV_THREAD_INDEX_TG },
   { "air.threads_per_grid", AIR_SV_THREADS_PER_GRID },
   { "air.thread_index_in_simdgroup", AIR_SV_SIMD_LANE },
   { "air.simdgroup_index_in_threadgroup", AIR_SV_SIMD_INDEX },
   { "air.threads_per_simdgroup", AIR_SV_SIMD_WIDTH },
   { "air.simdgroups_per_threadgroup", AIR_SV_SIMDS_PER_TG },
};

static nir_def *
load_sysval(struct air_ctx *c, enum air_sysval sv)
{
   switch (sv) {
   case AIR_SV_VERTEX_ID: return nir_load_vertex_id(&c->b);
   /* Metal's instance id counts from the base instance, GL's does not */
   case AIR_SV_INSTANCE_ID:
      return nir_iadd(&c->b, nir_load_instance_id(&c->b), nir_load_base_instance(&c->b));
   case AIR_SV_BASE_VERTEX: return nir_load_first_vertex(&c->b);
   case AIR_SV_BASE_INSTANCE: return nir_load_base_instance(&c->b);
   case AIR_SV_FRONT_FACING: return nir_load_front_face(&c->b, 1);
   case AIR_SV_SAMPLE_ID: return nir_load_sample_id(&c->b);
   case AIR_SV_SAMPLE_MASK_IN: return nir_load_sample_mask_in(&c->b);
   /* an input at VARYING_SLOT_PNTC rather than the system value: ir3 has no
    * load_point_coord ("Unhandled intrinsic type", CoreAnimation's particle
    * shaders), and gallium's freedreno feeds that slot the sprite coordinate
    * (rasterizer sprite_coord_enable) the way a GL state tracker's point
    * sprites have it */
   case AIR_SV_POINT_COORD: {
      nir_variable *var = nir_find_variable_with_location(c->nir, nir_var_shader_in,
                                                          VARYING_SLOT_PNTC);
      if (!var) {
         var = nir_variable_create(c->nir, nir_var_shader_in, glsl_vec_type(2),
                                   "gl_PointCoord");
         var->data.location = VARYING_SLOT_PNTC;
         var->data.driver_location = c->nir->num_inputs++;
      }
      return nir_load_var(&c->b, var);
   }
   case AIR_SV_THREAD_POS_GRID: return nir_load_global_invocation_id(&c->b, 32);
   case AIR_SV_THREAD_POS_TG: return nir_load_local_invocation_id(&c->b);
   case AIR_SV_TG_POS_GRID: return nir_load_workgroup_id(&c->b);
   case AIR_SV_THREADS_PER_TG: return nir_load_workgroup_size(&c->b);
   case AIR_SV_TGS_PER_GRID: return nir_load_num_workgroups(&c->b);
   case AIR_SV_THREAD_INDEX_TG: return nir_load_local_invocation_index(&c->b);
   case AIR_SV_THREADS_PER_GRID:
      return nir_imul(&c->b, nir_load_num_workgroups(&c->b), nir_load_workgroup_size(&c->b));
   case AIR_SV_SIMD_LANE: return nir_load_subgroup_invocation(&c->b);
   case AIR_SV_SIMD_INDEX: return nir_load_subgroup_id(&c->b);
   case AIR_SV_SIMD_WIDTH: return nir_load_subgroup_size(&c->b);
   /* ir3 has this one and computes it rather than reading a register:
    * ir3_nir_lower_subgroup_id_cs turns load_num_subgroups into
    * 1 + ((threads_per_threadgroup - 1) >> the SIMD-width shift), which folds
    * to a constant whenever the workgroup size is one (ir3_nir.c).
    */
   case AIR_SV_SIMDS_PER_TG: return nir_load_num_subgroups(&c->b);
   }
   return NULL;
}

/* An index or a shift count as 32 bits. LLVM indexes with an i1 as readily as with an i64
 * (`extractelement <2 x half> %v, i1 %c`, a two-element GEP), and a 1-bit value is a boolean to
 * NIR: u2u32 of one is invalid, and ir3 refuses it ("invalid src bit size: 1", RenderBox's
 * uber_fragment). */
static nir_def *
air_u32(nir_builder *b, nir_def *v)
{
   return v->bit_size == 1 ? nir_b2i32(b, v) : nir_u2u32(b, v);
}

/* a system value cut or widened to the type the shader declared */
static nir_def *
fit(struct air_ctx *c, nir_def *def, const struct air_type *t)
{
   unsigned want = air_nir_components(t);
   unsigned bits = air_nir_bit_size(c, t);
   if (def->num_components > want)
      def = nir_trim_vector(&c->b, def, want);
   else if (def->num_components < want)
      def = nir_pad_vector(&c->b, def, want);
   if (def->bit_size != bits)
      def = air_type_is_float(air_type_scalar(t))
               ? nir_u2fN(&c->b, def, bits)
               : nir_u2uN(&c->b, def, bits);
   return def;
}

/* Returns the resource it added, so that a caller with more to say about it --
 * an argument-buffer member, which carries the Metal (set, id) it came from --
 * can fill the rest in; NULL when the node was refused.
 */
static struct air_resource *
add_resource(struct air_ctx *c, enum air_resource_kind kind, unsigned index, unsigned size,
             bool read, bool write, unsigned param, const struct air_md *node)
{
   struct air_shader_info *info = c->info;
   info->resources = reralloc(c->mem_ctx, info->resources, struct air_resource,
                              info->num_resources + 1);
   /* Metal's `[[raster_order_group(n)]]`. Until this was read it was DROPPED IN
    * SILENCE, and that is worth naming rather than just fixing: the branches of
    * translate_input match on the resource's kind and return, so the loop that
    * refuses an `air.*` key nobody handled never runs over a node whose kind is
    * recognised. Every key in such a node is therefore invisible unless
    * something looks for it by name -- which is how a shader could ask for
    * ordering between overlapping fragments and get none, with nothing in any
    * log saying so. `inplace_copy_{lpf,lph}` of QuartzCore's library ask for it
    * on the texture they write.
    */
   const char *name = md_key_str(node, "air.arg_name");
   uint64_t group = 0;
   bool has_group = md_key_int(node, "air.raster_order_group", &group);
   if (has_group) {
      if (group >= 32) {
         /* the bitmask has 32 bits; a group number past them would be recorded
          * as a different group, which is worse than not being recorded */
         air_error(c, "interface: raster order group %u", (unsigned)group);
         return NULL;
      }
      info->raster_order_groups |= 1u << group;
      /* What the hardware can do is not what was asked. The ask is per
       * resource; a6xx's GRAS_SC_CNTL.SINGLE_PRIM_MODE is per draw, and
       * freedreno drives it from pipe_blend_state.blend_coherent
       * (fd6_emit.cc, build_prim_mode). Setting that bit orders the whole draw,
       * which is MORE than the shader asked for -- safe, and slower. There is
       * no way to give exactly the ask and no way to give less, so the shader
       * is translated and the difference is said out loud here instead of being
       * left to a document.
       */
      air_note(c, "raster order group %u on %s %u (%s): a6xx orders a whole draw or "
                  "nothing (GRAS_SC_CNTL.SINGLE_PRIM_MODE), never one resource, so the "
                  "runtime must set pipe_blend_state.blend_coherent and the ordering "
                  "given is coarser than the shader asked for",
               (unsigned)group,
               kind == AIR_RESOURCE_IMAGE ? "image" :
               kind == AIR_RESOURCE_TEXTURE ? "texture" :
               kind == AIR_RESOURCE_SAMPLER ? "sampler" : "buffer",
               index, name ? name : "unnamed");
   }

   info->resources[info->num_resources] = (struct air_resource) {
      .kind = kind, .index = index, .size = size, .read = read, .write = write,
      .param = param,
      .name = name,
      .type_name = md_key_str(node, "air.arg_type_name"),
      .raster_order_group = has_group ? (int)group : -1,
      /* a direct argument: Metal names it by .index and there is no set */
      .argbuf_set = ~0u, .argbuf_id = ~0u,
   };
   return &info->resources[info->num_resources++];
}

/* a value stored to an output, at the output's own width (make_io_var: a varying is
 * 32 bits whatever the shader computed it in) */
static nir_def *
air_output_bits(struct air_ctx *c, nir_variable *var, nir_def *v)
{
   unsigned bits = glsl_get_bit_size(glsl_without_array(var->type));
   if (!v || bits == v->bit_size)
      return v;
   if (v->bit_size == 1)
      return nir_b2iN(&c->b, v, bits);           /* a bool output travels as an int */
   return glsl_type_is_float_16_32_64(glsl_without_array(var->type)) ? nir_f2fN(&c->b, v, bits)
                                                                      : nir_i2iN(&c->b, v, bits);
}

static nir_variable *
make_io_var(struct air_ctx *c, nir_variable_mode mode, const struct air_type *t,
            unsigned location, const char *name)
{
   const struct glsl_type *gt;
   unsigned comps = air_nir_components(t);
   unsigned bits = air_nir_bit_size(c, t);
   /* A VARYING is 32 bits a component whatever the shader's type, converted where it is
    * read and written: a half4 colour passed from vertex to fragment shader as 16-bit
    * IO came out of ir3 as other colours altogether (the lock screen's battery outline
    * and the ends of its home bar, purple and green). Only a fragment shader's colour
    * outputs keep their 16 bits, which is what a render target of halves wants.
    */
   bool varying = (mode == nir_var_shader_in && c->nir->info.stage == MESA_SHADER_FRAGMENT) ||
                  (mode == nir_var_shader_out && c->nir->info.stage == MESA_SHADER_VERTEX);
   if (varying)
      bits = 32;
   if (air_type_is_float(air_type_scalar(t)))
      gt = bits == 16 ? glsl_f16vec_type(comps) : glsl_vec_type(comps);
   else
      gt = glsl_vector_type(bits == 16 ? GLSL_TYPE_INT16 : GLSL_TYPE_INT, comps);

   nir_variable *var = nir_variable_create(c->nir, mode, gt, name ? name : "io");
   var->data.location = location;
   return var;
}

/* fragment inputs and vertex outputs take the slots in the order they appear;
 * the runtime links the two sides by name (docs/air.md section 7.5)
 */
static void
add_varying(struct air_ctx *c, const char *name, unsigned slot, unsigned comps,
            const struct air_md *node)
{
   struct air_shader_info *info = c->info;
   info->varyings = reralloc(c->mem_ctx, info->varyings, struct air_varying,
                             info->num_varyings + 1);
   info->varyings[info->num_varyings++] = (struct air_varying) {
      .name = name, .slot = slot, .components = comps,
      .flat = md_key(node, "air.flat") >= 0,
      .centroid = md_key(node, "air.centroid") >= 0,
      .sample = md_key(node, "air.sample") >= 0,
      .no_perspective = md_key(node, "air.no_perspective") >= 0,
   };
}

/* Metal's `[[stage_in]]` / `[[attribute(n)]]`: a vertex attribute, read out of
 * a vertex buffer by the fetch hardware rather than computed by the shader.
 *
 * AIR HAS NO STAGE-IN STRUCT. Apple's compiler flattens it: each member is an
 * ordinary parameter of the entry point carrying its own node, and the struct
 * type does not appear in the module at all. QuartzCore's std_vert0_lpf is the
 * shape, read out of the real library and not inferred:
 *
 *   define <{ <4 x float>, <4 x float>, <2 x float> }> @std_vert0_lpf(
 *       <4 x float> %0, <2 x float> %1, <4 x float> %2, ptr addrspace(2) %3, ...)
 *   !19 = !{!20, !21, !22, !23, !24}
 *   !20 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1,
 *           !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
 *   !21 = !{i32 1, !"air.vertex_input", !"air.location_index", i32 1, i32 1,
 *           !"air.arg_type_name", !"float2", !"air.arg_name", !"texcoord0",
 *           !"air.arg_unused"}
 *   !22 = !{i32 2, !"air.vertex_input", !"air.location_index", i32 3, i32 1,
 *           !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
 *
 * Three things in that are worth saying out loud, because each one would be
 * guessed wrong:
 *
 *   - the index is Metal's `[[attribute(n)]]` and NOT a position in the list.
 *     These three are 0, 1 and 3, so a translator that numbered them in order
 *     would feed `color` from attribute 2 -- a wrong vertex, drawn, with
 *     nothing to say so.
 *   - `air.arg_unused` occurs ON an attribute, and eight of QuartzCore's
 *     fourteen such shaders have one. Nothing reads it, so no NIR input is made:
 *     an input costs a VFD fetch slot and makes the pipeline demand a binding
 *     that the application's MTLVertexDescriptor may legally not have for an
 *     attribute the shader does not read.
 *   - the format, offset, stride and buffer of the attribute are NOWHERE in
 *     the module. They are the MTLVertexDescriptor's, which is API state. What
 *     this builds is the shader's half of the match, recorded in
 *     air_shader_info.vertex_attributes for the runtime to check against it.
 *
 * The NIR input is always 32 bits wide and the value narrowed afterwards, which
 * is a decision with a measurement behind it rather than a convention copied
 * from GL: ir3 is perfectly willing to put a 16-bit vertex input in the half
 * register file -- a `half4` attribute compiles to `@in(hr0.x)` and a
 * `cov.f16f32` -- but `ir3_shader_variant.inputs[]` has no half flag (only
 * `outputs[]` does), prints that input as `r0.x`, and
 * `fd6_program.cc:emit_vfd_dest` programs VFD_DEST_CNTL_INSTR_REGID straight
 * out of that field. The fetch would write the full register while the shader
 * read the half one. 32 bits in and a narrowing instruction keeps this in the
 * regime both freedreno drivers use, and the VFD converts the format either way.
 */
static void
add_vertex_attribute(struct air_ctx *c, const struct air_md *node, unsigned index,
                     unsigned comps, unsigned bits, bool is_float, bool used,
                     unsigned driver_location)
{
   struct air_shader_info *info = c->info;
   const char *tn = md_key_str(node, "air.arg_type_name");

   /* `int4` and `uint4` are the same `<4 x i32>`, so the sign is in Apple's
    * reflection string and in nothing the translator can see. `packed_uchar4`
    * is the same question with a prefix.
    */
   const char *base = tn;
   if (base && !strncmp(base, "packed_", 7))
      base += 7;
   enum air_attribute_base_type bt =
      is_float ? AIR_ATTRIB_FLOAT :
      (base && base[0] == 'u') ? AIR_ATTRIB_UINT : AIR_ATTRIB_INT;

   const struct air_md *fc = md_key_node(node, "air.function_constant");

   info->vertex_attributes = reralloc(c->mem_ctx, info->vertex_attributes,
                                      struct air_vertex_attribute,
                                      info->num_vertex_attributes + 1);
   info->vertex_attributes[info->num_vertex_attributes++] =
      (struct air_vertex_attribute) {
         .index = index,
         .location = VERT_ATTRIB_GENERIC0 + index,
         .driver_location = driver_location,
         .components = comps,
         .bit_size = bits,
         .base_type = bt,
         .name = md_key_str(node, "air.arg_name"),
         .type_name = tn,
         .used = used,
         /* operand 2 of the predicate node is its name; 0 is the global and 1
          * its type (`!"bool"`) */
         .function_constant = md_str(fc, 2),
      };
   if (used)
      info->vertex_attributes_read |= 1u << index;
}

static bool
translate_vertex_attribute(struct air_ctx *c, const struct air_md *node, unsigned param,
                           const struct air_type *t)
{
   const char *tn = md_key_str(node, "air.arg_type_name");
   const char *name = md_key_str(node, "air.arg_name");

   int li = md_key(node, "air.location_index");
   uint64_t index = 0, span = 1;
   if (li < 0 || !md_int(node, li + 1, &index))
      return air_error(c, "interface: a vertex input (%s) with no air.location_index",
                       name ? name : "unnamed");

   /* The second integer after the key. What it MEANS is inferred -- Apple's own
    * form for an array of resources, where it is the number of consecutive
    * indices the argument takes -- and nothing in reach can exercise another
    * value: all 974 air.location_index nodes of the five system libraries and
    * all 4 764 on Resident Evil 4's vertex inputs carry 1. An attribute
    * spanning several indices (a matrix member of a stage-in struct is the case
    * that would) is therefore refused by name rather than silently read from
    * the first of them.
    */
   if (md_int(node, li + 2, &span) && span != 1)
      return air_error(c, "interface: a vertex input (%s) spanning %u attribute indices",
                       name ? name : "unnamed", (unsigned)span);

   /* Metal has 31 attribute slots; NIR's gl_vert_attrib has 16 generic ones,
    * and VERT_ATTRIB_GENERIC0 + 16 is VERT_ATTRIB_EDGEFLAG. Unchecked, index
    * 16 translated onto the edge flag and index 30 past the end of the enum --
    * both compiled through ir3 without a word, which is the failure this
    * refusal exists to replace.
    */
   if (index >= MAX_VERTEX_GENERIC_ATTRIBS)
      return air_error(c, "interface: vertex attribute %u (%s), and NIR has %u generic "
                          "vertex attributes",
                       (unsigned)index, name ? name : "unnamed",
                       MAX_VERTEX_GENERIC_ATTRIBS);

   /* Two members at one index would make two NIR inputs at one location, two
    * VFD fetches into the same register, and a pipeline whose element array is
    * longer than the attribute set it describes. Metal forbids it; refusing is
    * what keeps a malformed module from being drawn.
    */
   for (unsigned i = 0; i < c->info->num_vertex_attributes; i++)
      if (c->info->vertex_attributes[i].index == index)
         return air_error(c, "interface: two vertex inputs at attribute %u (%s and %s)",
                          (unsigned)index,
                          c->info->vertex_attributes[i].name
                             ? c->info->vertex_attributes[i].name : "unnamed",
                          name ? name : "unnamed");

   if (!ret_member_is_number(t))
      return air_error(c, "interface: a vertex input (%s) of type %s, which is neither a "
                          "scalar nor a vector",
                       name ? name : "unnamed", tn ? tn : "?");

   unsigned comps = air_nir_components(t);
   unsigned bits = air_nir_bit_size(c, t);
   bool is_float = air_type_is_float(air_type_scalar(t));
   /* A vertex attribute is one fetch of at most four components; `double` has
    * no place in MSL at all, and an `i1` or an `i64` attribute has no
    * MTLVertexFormat to come from.
    */
   if (comps > 4 || (is_float ? (bits != 16 && bits != 32)
                              : (bits != 8 && bits != 16 && bits != 32)))
      return air_error(c, "interface: a vertex input (%s) of type %s",
                       name ? name : "unnamed", tn ? tn : "?");

   if (md_key(node, "air.arg_unused") >= 0) {
      add_vertex_attribute(c, node, index, comps, bits, is_float, false, ~0u);
      c->params[param] = air_val_ssa(c, nir_undef(&c->b, comps, bits));
      return true;
   }

   unsigned location = VERT_ATTRIB_GENERIC0 + index;
   const struct glsl_type *gt = is_float ? glsl_vec_type(comps)
                                         : glsl_vector_type(GLSL_TYPE_INT, comps);
   nir_variable *var = nir_variable_create(c->nir, nir_var_shader_in, gt,
                                           name ? name : "attr");
   var->data.location = location;
   unsigned driver_location = c->nir->num_inputs++;
   var->data.driver_location = driver_location;
   /* An interpolation mode is a fragment-stage word and INTERP_MODE_SMOOTH on a
    * vertex attribute would be a claim about something that does not happen.
    */
   var->data.interpolation = INTERP_MODE_NONE;

   /* nir_shader_gather_info does not run on this path (ir3 calls it only for a
    * binning variant), so what it would have set is set here, as for the
    * fragment position above. A runtime matching an MTLVertexDescriptor can
    * read either this or air_shader_info.vertex_attributes_read.
    */
   c->nir->info.inputs_read |= BITFIELD64_BIT(location);

   nir_def *v = nir_load_var(&c->b, var);
   if (bits != 32) {
      /* The fetch delivered a full register; the shader asked for a narrower
       * type. For an integer this is a truncation and the same instruction for
       * either sign -- which of the two the HARDWARE must do when it widens an
       * 8- or 16-bit component to 32 bits is the format's business, and the
       * format is the MTLVertexDescriptor's.
       */
      v = is_float ? nir_f2f16(&c->b, v) : nir_u2uN(&c->b, v, bits);
   }
   add_vertex_attribute(c, node, index, comps, bits, is_float, true, driver_location);

   /* An attribute the module reads only when a function-constant predicate is
    * true is still a fetch here, and that is a caveat rather than an error: the
    * parameter exists whatever the predicate says, so the input is built and
    * vertex_attributes_read gets its bit. A runtime that required a binding for
    * every bit of that mask would refuse a perfectly legal pipeline -- one
    * whose function constants say the attribute is absent and whose
    * MTLVertexDescriptor therefore configures nothing at that index. Which it
    * is cannot be decided here, because the value of the predicate is the
    * pipeline's: these globals are written by the module's air.static_init
    * constructor from the constants the API supplies. So the condition goes in
    * the log at translation time rather than only in a document.
    */
   const struct air_md *fc = md_key_node(node, "air.function_constant");
   if (fc) {
      const char *pred = md_str(fc, 2);
      air_note(c, "vertex attribute %u (%s) is gated by the function-constant predicate "
                  "%s: the parameter exists either way, so the attribute is fetched and "
                  "its bit is in vertex_attributes_read, and a pipeline whose constants "
                  "make the predicate false may legally configure nothing at that index "
                  "-- the runtime must read air_vertex_attribute.function_constant before "
                  "demanding a binding",
               (unsigned)index, name ? name : "unnamed", pred ? pred : "(unnamed)");
   }

   c->params[param] = air_val_ssa(c, v);
   return true;
}

/* -- argument buffers ---------------------------------------------------- */

/* The module's own declaration of a named struct type, by that name.
 *
 * This is the SECOND statement of what an argument buffer holds. Apple's
 * metadata is the first; it names every member's kind, index and access but
 * not its address space -- not one of the 82 048 members of Resident Evil 4's
 * 7 857 argument buffers carries `air.address_space` -- and a constant buffer
 * bound as a device buffer is a shader reading the wrong descriptor with
 * nothing to say so. The struct type says it, member by member, and the two
 * descriptions are checked against each other before anything is bound: the
 * member count, every member's byte offset and every member's size. They
 * agree on all 7 857 sets of that library, which is what makes using it a
 * reading rather than a guess.
 */
static const struct air_type *
named_struct(const struct air_module *m, const char *name)
{
   for (unsigned i = 0; i < m->num_types; i++)
      if (m->types[i].kind == AIR_TYPE_STRUCT && !m->types[i].opaque &&
          m->types[i].name && !strcmp(m->types[i].name, name))
         return &m->types[i];
   return NULL;
}

/* The `air.*` key of a descriptor this frontend does not bind, for the refusal
 * to name -- `air.command_buffer` and `air.render_pipeline_state` are the two
 * in reach, both in the one kernel of Resident Evil 4 that encodes draws on
 * the GPU. The housekeeping keys every descriptor carries are skipped so that
 * the message names the kind and not the first word in the node.
 */
static const char *
descriptor_kind_key(const struct air_md *desc)
{
   static const char *const plumbing[] = {
      "air.location_index", "air.arg_name", "air.arg_type_name", "air.arg_type_size",
      "air.arg_type_align_size", "air.struct_type_info", "air.address_space",
      "air.buffer_size", "air.read", "air.write", "air.read_write", "air.sample",
      "air.arg_unused", "air.raster_order_group", "air.indirect_argument",
   };
   for (unsigned i = 0; i < (desc ? desc->num_ops : 0); i++) {
      const char *s = md_str(desc, i);
      if (!s || strncmp(s, "air.", 4))
         continue;
      bool known = false;
      for (unsigned k = 0; k < ARRAY_SIZE(plumbing); k++)
         known |= !strcmp(s, plumbing[k]);
      if (!known)
         return s;
   }
   return NULL;
}

/* `air.struct_type_info` of an argument buffer, into a member table.
 *
 * The shape, read off Apple's own output: five fixed fields -- byte offset,
 * size, a flag, the type name, the member name -- and then the key
 * `air.indirect_argument` and a node, which is an ORDINARY resource
 * descriptor. It differs from a direct `air.buffer` / `air.texture` /
 * `air.sampler` argument in one operand, the leading integer, which is the
 * member's number in the set where a direct argument's is its parameter
 * number. So everything needed to bind it is in the vocabulary
 * translate_input has read since the beginning.
 *
 * A member whose key comes BEFORE the five fields is how Apple writes one
 * whose own type is a struct (`!"air.struct_type_info", !N, i32 off, ...`),
 * which in an argument buffer is inline constant data rather than a resource;
 * that falls into the first refusal below, as it must -- the runtime's half of
 * this feature records descriptors against (buffer, offset) and has no bytes
 * to record for a member that is not one.
 */
static bool
argbuf_members(struct air_ctx *c, const struct air_md *sti, const struct air_type *st,
               struct air_argbuf *ab)
{
   unsigned n = 0;
   for (unsigned i = 0; i < sti->num_ops; n++) {
      uint64_t off = 0, msize = 0, flag = 0;
      const char *mtype = md_str(sti, i + 3), *mname = md_str(sti, i + 4);
      if (!md_int(sti, i, &off) || !md_int(sti, i + 1, &msize) ||
          !md_int(sti, i + 2, &flag) || !mtype || !mname) {
         const char *key = md_str(sti, i);
         return air_error(c, "interface: member %u of the argument buffer %s is "
                             "described by %s and not by five fields and a descriptor",
                          n, ab->name ? ab->name : "unnamed",
                          key ? key : "something this translator cannot read");
      }

      const char *key = md_str(sti, i + 5);
      const struct air_md *desc = NULL;
      if (key && !strcmp(key, "air.indirect_argument") && i + 6 < sti->num_ops &&
          sti->ops[i + 6] && sti->ops[i + 6]->kind == AIR_MD_NODE)
         desc = sti->ops[i + 6];
      if (!desc)
         return air_error(c, "interface: member %s of the argument buffer %s is followed "
                             "by %s and not by an air.indirect_argument descriptor",
                          mname, ab->name ? ab->name : "unnamed", key ? key : "nothing");
      i += 7;

      /* A set inside a set. The member's descriptor would have a member table
       * of its own, reached through a pointer LOADED out of this one and then
       * walked -- which is the general pointer model and not this one.
       */
      if (md_key(desc, "air.indirect_buffer") >= 0)
         return air_error(c, "interface: member %s of the argument buffer %s is itself an "
                             "argument buffer", mname, ab->name ? ab->name : "unnamed");

      bool is_buffer = md_key(desc, "air.buffer") >= 0;
      bool is_texture = md_key(desc, "air.texture") >= 0;
      bool is_sampler = md_key(desc, "air.sampler") >= 0;
      if (is_buffer + is_texture + is_sampler != 1) {
         const char *what = descriptor_kind_key(desc);
         return air_error(c, "interface: member %s of the argument buffer %s is %s, and "
                             "this frontend binds a buffer, a texture or a sampler",
                          mname, ab->name ? ab->name : "unnamed",
                          is_buffer + is_texture + is_sampler > 1
                             ? "more than one kind at once"
                             : what ? what : "a kind this translator does not know");
      }

      int mli = md_key(desc, "air.location_index");
      uint64_t mid = 0, mspan = 1;
      if (mli < 0 || !md_int(desc, mli + 1, &mid))
         return air_error(c, "interface: member %s of the argument buffer %s has no "
                             "air.location_index", mname,
                          ab->name ? ab->name : "unnamed");
      /* An ARRAY of descriptors in one member: the shader could then pick one
       * at run time, which no resolution can do. Every one of Resident Evil
       * 4's 82 048 members spans exactly one index, so this is a guard on a
       * shape not yet seen rather than a gap with modules behind it.
       */
      if (md_int(desc, mli + 2, &mspan) && mspan != 1)
         return air_error(c, "interface: member %s of the argument buffer %s spans %u "
                             "descriptor indices", mname,
                          ab->name ? ab->name : "unnamed", (unsigned)mspan);

      /* against the module's own struct type */
      if (n >= st->num_members)
         return air_error(c, "interface: the argument buffer %s declares more members "
                             "than the %u its struct type has",
                          ab->name ? ab->name : "unnamed", st->num_members);
      const struct air_type *mt = st->members[n];
      uint64_t tyoff = air_struct_member_offset(c->m, st, n);
      uint64_t tysize = air_type_alloc_size(c->m, mt);
      if (tyoff != off || tysize != msize)
         return air_error(c, "interface: member %s of the argument buffer %s is %u bytes "
                             "at byte %u, and the module's struct type puts %u bytes "
                             "there at %u", mname, ab->name ? ab->name : "unnamed",
                          (unsigned)msize, (unsigned)off, (unsigned)tysize,
                          (unsigned)tyoff);

      enum air_resource_kind rk;
      unsigned addrspace;
      bool read = md_key(desc, "air.read") >= 0 || md_key(desc, "air.read_write") >= 0 ||
                  md_key(desc, "air.sample") >= 0;
      bool write = md_key(desc, "air.write") >= 0 || md_key(desc, "air.read_write") >= 0;

      if (is_buffer) {
         /* The member IS the pointer, and its address space is the only thing
          * that says whether the descriptor is a constant buffer or a device
          * one. A frontend that reused the direct-argument path unchanged
          * would read air.address_space, find none, and refuse at "a buffer in
          * address space 0" -- which is the better of the two ways to be
          * wrong.
          */
         if (mt->kind != AIR_TYPE_POINTER || (mt->addrspace != 1 && mt->addrspace != 2))
            return air_error(c, "interface: the buffer member %s of the argument buffer "
                                "%s is not a device or constant pointer in the module's "
                                "struct type", mname, ab->name ? ab->name : "unnamed");
         addrspace = mt->addrspace;
         rk = mt->addrspace == 1 ? AIR_RESOURCE_DEVICE_BUFFER : AIR_RESOURCE_CONSTANT_BUFFER;
      } else {
         /* A texture or a sampler arrives as the one-field `metal::texture2d`
          * / `metal::sampler` wrapper the CoreImage stitched functions take
          * (section 7.8), here as a struct member instead of a parameter and
          * with the handle LOADED rather than passed. Keyed on Apple's own
          * name, as the library wrapper is: "a struct with one pointer in it"
          * would quietly accept anything shaped alike.
          */
         if (mt->kind != AIR_TYPE_STRUCT || !mt->name ||
             strncmp(mt->name, "struct.metal::", 14) || mt->num_members != 1 ||
             mt->members[0]->kind != AIR_TYPE_POINTER)
            return air_error(c, "interface: the %s member %s of the argument buffer %s is "
                                "not one of Apple's one-field metal:: wrappers in the "
                                "module's struct type", is_sampler ? "sampler" : "texture",
                             mname, ab->name ? ab->name : "unnamed");
         addrspace = mt->members[0]->addrspace;
         rk = is_sampler ? AIR_RESOURCE_SAMPLER
                         : write ? AIR_RESOURCE_IMAGE : AIR_RESOURCE_TEXTURE;
      }

      ab->members = reralloc(c->mem_ctx, ab->members, struct air_argbuf_member, n + 1);
      ab->members[n] = (struct air_argbuf_member) {
         .offset = off, .kind = rk, .metal_id = (unsigned)mid, .index = ~0u,
         .addrspace = addrspace, .read = read, .write = write,
         .name = mname, .type_name = mtype, .node = desc,
      };
      ab->num_members = n + 1;
   }

   if (n != st->num_members)
      return air_error(c, "interface: the argument buffer %s declares %u members and its "
                          "struct type has %u", ab->name ? ab->name : "unnamed", n,
                       st->num_members);
   return true;
}

/* Metal's argument buffer: `air.indirect_buffer`, one struct of descriptors
 * bound at one buffer index, which is how a Vulkan descriptor set reaches
 * Metal through SPIRV-Cross and how an MSL `[[id(n)]]` struct reaches it from
 * Metal itself. 7 620 of Resident Evil 4's 7 879 modules have one and 0 of
 * iOS 16.5's 671 do, so this is the one interface word that is invisible from
 * Apple's own libraries and is 97% of a game (docs/air.md section 8.2).
 *
 * The parameter does not become a buffer to read: it becomes a pointer of its
 * own class, and the member table built here is what a load through it
 * resolves against (argbuf_load). No address, no descriptor indirection, and
 * nothing of the `32bit_index_offset` model the rest of the frontend is built
 * on changes.
 */
static bool
translate_argument_buffer(struct air_ctx *c, const struct air_md *node, unsigned param)
{
   const char *name = md_key_str(node, "air.arg_name");
   const char *tn = md_key_str(node, "air.arg_type_name");

   int li = md_key(node, "air.location_index");
   uint64_t set_index = 0, span = 1, size = 0;
   if (li < 0 || !md_int(node, li + 1, &set_index))
      return air_error(c, "interface: an argument buffer (%s) with no air.location_index",
                       name ? name : "unnamed");
   if (md_int(node, li + 2, &span) && span != 1)
      return air_error(c, "interface: an argument buffer (%s) spanning %u buffer indices",
                       name ? name : "unnamed", (unsigned)span);
   md_key_int(node, "air.buffer_size", &size);

   const struct air_md *sti = md_key_node(node, "air.struct_type_info");
   if (!sti)
      return air_error(c, "interface: an argument buffer (%s) with no "
                          "air.struct_type_info, so nothing says what is in it",
                       name ? name : "unnamed");

   const struct air_type *st = NULL;
   if (tn) {
      char *sname = ralloc_asprintf(c->mem_ctx, "struct.%s", tn);
      st = named_struct(c->m, sname);
   }
   if (!st)
      return air_error(c, "interface: an argument buffer (%s) whose struct type %s the "
                          "module does not declare, so its members' address spaces "
                          "cannot be read", name ? name : "unnamed", tn ? tn : "(unnamed)");

   /* Two sets at one Metal buffer index would have one record at draw time and
    * two sets of members wanting it. */
   for (unsigned i = 0; i < c->num_argbufs; i++)
      if (c->argbufs[i].metal_index == set_index)
         return air_error(c, "interface: two argument buffers at buffer index %u (%s and "
                             "%s)", (unsigned)set_index,
                          c->argbufs[i].name ? c->argbufs[i].name : "unnamed",
                          name ? name : "unnamed");

   c->argbufs = reralloc(c->mem_ctx, c->argbufs, struct air_argbuf, c->num_argbufs + 1);
   struct air_argbuf *ab = &c->argbufs[c->num_argbufs];
   *ab = (struct air_argbuf) {
      .metal_index = (unsigned)set_index, .size = (unsigned)size, .param = param,
      .name = name, .type_name = tn,
   };
   if (!argbuf_members(c, sti, st, ab))
      return false;
   /* eight bytes per member is what MTLArgumentEncoder writes and what the
    * runtime's encodedLength has to answer; a set whose declared size is not
    * that is a layout this frontend has not seen */
   if (size != 8ull * ab->num_members)
      return air_error(c, "interface: the argument buffer %s is %u bytes with %u members, "
                          "and this frontend knows the eight-bytes-per-member layout "
                          "only", name ? name : "unnamed", (unsigned)size,
                       ab->num_members);

   struct air_ptr p = { .class = AIR_PTR_ARGBUF, .binding = c->num_argbufs++ };
   c->params[param] = air_val_ptr(c, p);
   return true;
}

/* The flat per-kind index of every member of every argument buffer, and one
 * air_resource each.
 *
 * Why this is a decision and not a transcription: a member's own
 * `air.location_index` is Metal's `[[id(n)]]`, which is per set, so set 0's
 * member 0 and set 1's member 0 both say 0 and 237 of Resident Evil 4's
 * modules have two sets. One counter per kind in (set, member) order gives
 * each member a binding of its own, and `argbuf_set` / `argbuf_id` on the
 * resource keep the Metal name the application uses -- the runtime binds by
 * the first and the application names by the second.
 *
 * The counters start above the highest index a DIRECT argument of that kind
 * already took, so a member and an argument cannot land in one Gallium slot.
 * None of Resident Evil 4's 7 620 modules mixes the two -- those shaders have
 * no direct resources at all -- so this is the part of the allocation nothing
 * in reach exercises, and it is here because the collision it prevents would
 * be a draw over the wrong resource rather than an error.
 *
 * Called after every input is translated, so that the direct arguments are all
 * known, and before plan_globals, so that a shader's own `constexpr sampler`
 * still lands after the samplers the API binds.
 */
static void
allocate_argbuf_indices(struct air_ctx *c)
{
   unsigned next_buffer = 0, next_texture = 0, next_sampler = 0;
   for (unsigned i = 0; i < c->info->num_resources; i++) {
      const struct air_resource *r = &c->info->resources[i];
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
      case AIR_RESOURCE_DEVICE_BUFFER:
         next_buffer = MAX2(next_buffer, r->index + 1);
         break;
      case AIR_RESOURCE_TEXTURE:
      case AIR_RESOURCE_IMAGE:
         next_texture = MAX2(next_texture, r->index + 1);
         break;
      case AIR_RESOURCE_SAMPLER:
         next_sampler = MAX2(next_sampler, r->index + 1);
         break;
      default:
         break;
      }
   }

   for (unsigned s = 0; s < c->num_argbufs; s++) {
      struct air_argbuf *ab = &c->argbufs[s];
      for (unsigned k = 0; k < ab->num_members; k++) {
         struct air_argbuf_member *m = &ab->members[k];
         switch (m->kind) {
         case AIR_RESOURCE_CONSTANT_BUFFER:
         case AIR_RESOURCE_DEVICE_BUFFER:
            m->index = next_buffer++;
            break;
         case AIR_RESOURCE_TEXTURE:
         case AIR_RESOURCE_IMAGE:
            m->index = next_texture++;
            break;
         default:
            m->index = next_sampler++;
            break;
         }
         struct air_resource *r = add_resource(c, m->kind, m->index, 0, m->read, m->write,
                                               ab->param, m->node);
         if (!r)
            return;   /* add_resource has said why */
         r->argbuf_set = ab->metal_index;
         r->argbuf_id = m->metal_id;
      }
   }
}

/* A load through an argument-buffer pointer RESOLVES instead of loading: the
 * byte offset names a member, and the member's descriptor names a binding.
 *
 * Keying on the byte offset rather than on the member number is what makes the
 * three shapes SPIRV-Cross emits one case. A buffer member is the pointer
 * itself; a texture or sampler member is a one-field wrapper, and the
 * `getelementptr` into that wrapper adds zero bytes -- so the texture shape
 * resolves by the same lookup as the buffer shape, with nothing special about
 * it and no second path to keep right.
 */
static struct air_val *
argbuf_load(struct air_ctx *c, struct air_ptr ptr, const struct air_type *type)
{
   const struct air_argbuf *ab = &c->argbufs[ptr.binding];
   const char *abname = ab->name ? ab->name : "unnamed";

   uint64_t off = 0;
   if (ptr.offset) {
      if (!nir_src_is_const(nir_src_for_ssa(ptr.offset))) {
         air_error(c, "instruction: a load through the argument buffer %s at an offset "
                      "that is not constant, which is a descriptor chosen at run time",
                   abname);
         return NULL;
      }
      off = nir_src_as_uint(nir_src_for_ssa(ptr.offset));
   }

   const struct air_argbuf_member *m = NULL;
   for (unsigned i = 0; i < ab->num_members; i++)
      if (ab->members[i].offset == off)
         m = &ab->members[i];
   if (!m) {
      air_error(c, "instruction: a load from byte %u of the argument buffer %s, which "
                   "declares no member there", (unsigned)off, abname);
      return NULL;
   }

   /* A number read out of the set rather than a descriptor: Metal's
    * `constantDataAtIndex:`. Every member of this set is a resource -- that is
    * what argbuf_members checked -- so this is a load of a descriptor's bytes,
    * which are the runtime's 8-byte handle and mean nothing to a driver that
    * binds by index.
    */
   if (type->kind != AIR_TYPE_POINTER) {
      air_error(c, "instruction: a load of a value out of the argument buffer %s, whose "
                   "member %s at byte %u is a resource and has no bytes to read",
                abname, m->name ? m->name : "unnamed", (unsigned)off);
      return NULL;
   }

   /* The address space of the pointer the shader LOADS, against the one the
    * module's struct type declares for that member. The two come from
    * different records -- the load instruction's own result type and the
    * module's type table -- and either one alone would bind a constant buffer
    * as a device buffer without a word if it were wrong.
    */
   if (type->addrspace != m->addrspace) {
      air_error(c, "instruction: a pointer in address space %u loaded from member %s of "
                   "the argument buffer %s, which the module's struct type puts in "
                   "address space %u", type->addrspace, m->name ? m->name : "unnamed",
                abname, m->addrspace);
      return NULL;
   }

   struct air_ptr p = { .binding = m->index, .pointee = type->elem };
   switch (m->kind) {
   case AIR_RESOURCE_CONSTANT_BUFFER: p.class = AIR_PTR_UBO; break;
   case AIR_RESOURCE_DEVICE_BUFFER:   p.class = AIR_PTR_SSBO; break;
   case AIR_RESOURCE_SAMPLER:         p.class = AIR_PTR_SAMPLER; break;
   case AIR_RESOURCE_IMAGE:
      p.class = AIR_PTR_TEXTURE;
      p.is_image = true;
      break;
   default:
      p.class = AIR_PTR_TEXTURE;
      break;
   }
   return air_val_ptr(c, p);
}

/* -- numbers the pipeline decides --------------------------------------- */

/* iOS 26's RenderBox names its colour attachments by EXPRESSION: `[[color(e)]]` where e is
 * computed from a function constant, and the metadata carries a pointer where the target
 * number used to be:
 *
 *   !{!"air.render_target", ptr addrspace(2) @__metal_implicit_attr_int_expr_0.76, i32 0, ...}
 *
 * The global is written by the module's air.static_init constructor --
 * `store (lshr (extractelement (load @..._shader_state.MTL_FC_INIT_0), 0), 16) & 2` --
 * so its value is known once the constants are, and an interface slot has to be a number
 * while the interface is being built. So the constructor is RUN here, on the constants
 * this translation was given: integers only, which is all an attribute expression is,
 * following its branches (filter_distance_fragment's has a switch and a phi), and what it
 * stores to each global is the global's value.
 */
#define AIR_EVAL_LANES 16
struct air_eval {
   unsigned n;       /* lanes: 1 for a scalar */
   unsigned bits;    /* of each lane */
   uint64_t lane[AIR_EVAL_LANES];
};

static uint64_t
eval_mask(unsigned bits)
{
   return bits >= 64 ? ~0ull : (1ull << bits) - 1;
}

static int64_t
eval_signed(uint64_t v, unsigned bits)
{
   return bits >= 64 ? (int64_t)v : (int64_t)(v << (64 - bits)) >> (64 - bits);
}

static bool
eval_shape(const struct air_type *t, struct air_eval *out)
{
   unsigned n = 1;
   if (t->kind == AIR_TYPE_VECTOR) {
      n = t->count;
      t = t->elem;
   }
   if (t->kind != AIR_TYPE_INT || n > AIR_EVAL_LANES)
      return false;
   out->n = n;
   out->bits = t->bits;
   return true;
}

/* the function constant a global is, as its index; -1 when it is not one */
static int
eval_fc_index(struct air_ctx *c, const struct air_value *g)
{
   const struct air_named_md *n = named_md(c->m, "air.function_constants");
   for (unsigned i = 0; n && i < n->num_ops; i++) {
      const struct air_md *node = n->ops[i];
      if (!node || !node->num_ops || !node->ops[0] || node->ops[0]->kind != AIR_MD_VALUE ||
          node->ops[0]->value != g)
         continue;
      /* the same reading as air_module_function_constants: the first integer after it */
      for (unsigned k = 1; k < node->num_ops; k++) {
         uint64_t idx;
         if (md_int(node, k, &idx))
            return (int)idx;
      }
   }
   return -1;
}

/* one run of the constructors: the instructions' values, what they stored, and the block
 * control came from (for a phi) */
struct air_eval_env {
   void *mem;
   struct hash_table *vals;     /* air_inst -> struct air_eval */
   struct hash_table *stores;   /* global -> struct air_eval, or NULL: stored, value unknown */
   const struct air_block *prev;
};

static bool air_eval(struct air_ctx *c, struct air_eval_env *env, const struct air_value *v,
                     struct air_eval *out, unsigned depth);

static void
eval_run_function(struct air_ctx *c, struct air_eval_env *env, const struct air_function *f)
{
   const struct air_block *blk = f->num_blocks ? &f->blocks[0] : NULL;
   env->prev = NULL;
   for (unsigned steps = 0; blk && steps < 256; steps++) {
      /* the phis first, all of them on the edge just taken */
      unsigned k = 0;
      for (; k < blk->num_insts && blk->insts[k]->op == AIR_OP_PHI; k++) {
         const struct air_inst *in = blk->insts[k];
         struct air_eval *e = ralloc(env->mem, struct air_eval);
         if (air_eval(c, env, &in->base, e, 0))
            _mesa_hash_table_insert(env->vals, in, e);
      }
      const struct air_block *next = NULL;
      for (; k < blk->num_insts; k++) {
         const struct air_inst *in = blk->insts[k];
         struct air_eval cond, *e;
         switch (in->op) {
         case AIR_OP_STORE:
            e = ralloc(env->mem, struct air_eval);
            _mesa_hash_table_insert(env->stores, in->operands[1],
                                    air_eval(c, env, in->operands[0], e, 0) ? e : NULL);
            break;
         case AIR_OP_RET:
            return;
         case AIR_OP_BR:
            if (in->num_targets == 1)
               next = in->targets[0];
            else if (air_eval(c, env, in->operands[0], &cond, 0))
               next = in->targets[cond.lane[0] ? 0 : 1];
            else
               return;   /* a branch on something unknown: what follows is unknown too */
            break;
         case AIR_OP_SWITCH:
            if (!air_eval(c, env, in->operands[0], &cond, 0))
               return;
            next = in->targets[0];
            for (unsigned i = 1; i < in->num_targets; i++) {
               uint64_t u;
               if (air_const_uint(in->operands[i], &u) &&
                   (u & eval_mask(cond.bits)) == cond.lane[0]) {
                  next = in->targets[i];
                  break;
               }
            }
            break;
         default:
            e = ralloc(env->mem, struct air_eval);
            if (in->base.type->kind != AIR_TYPE_VOID && air_eval(c, env, &in->base, e, 0))
               _mesa_hash_table_insert(env->vals, in, e);
            break;
         }
      }
      env->prev = blk;
      blk = next;
   }
}

/* what air.static_init stores to `g`, or a function constant's own value */
static bool
eval_global(struct air_ctx *c, struct air_eval_env *env, const struct air_value *g,
            struct air_eval *out, unsigned depth)
{
   int fc = eval_fc_index(c, g);
   if (fc >= 0) {
      const struct air_type *t = ((const struct air_global *)g)->value_type;
      if (!eval_shape(t, out))
         return false;
      const struct air_constant_value *v = NULL;
      for (unsigned i = 0; c->options && i < c->options->num_constants; i++)
         if (c->options->constants[i].index == (unsigned)fc)
            v = &c->options->constants[i];
      for (unsigned i = 0; i < out->n; i++) {
         unsigned at = i * out->bits;
         if (!v || v->undefined || at >= 256) {
            out->lane[i] = 0;
            continue;
         }
         uint64_t word = at / 64 ? v->more[at / 64 - 1] : v->bits;
         out->lane[i] = (word >> (at % 64)) & eval_mask(out->bits);
      }
      return true;
   }
   if (env) {
      /* inside a run: what the constructors have stored so far */
      struct hash_entry *he = _mesa_hash_table_search(env->stores, g);
      if (!he || !he->data)
         return false;
      *out = *(struct air_eval *)he->data;
      return true;
   }
   /* a run of every constructor, in module order, as @llvm.global_ctors runs them */
   struct air_eval_env run = { .mem = ralloc_context(NULL) };
   run.vals = _mesa_pointer_hash_table_create(run.mem);
   run.stores = _mesa_pointer_hash_table_create(run.mem);
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const struct air_function *f = c->m->functions[i];
      if (!f->is_declaration && f->section && !strcmp(f->section, "air.static_init"))
         eval_run_function(c, &run, f);
   }
   struct hash_entry *he = _mesa_hash_table_search(run.stores, g);
   bool ok = he && he->data;
   if (ok)
      *out = *(struct air_eval *)he->data;
   ralloc_free(run.mem);
   (void)depth;
   return ok;
}

/*
 * A program-scope variable's value, when the static initializers alone decide it: iOS 26's
 * QuartzCore keeps every function constant of its uber shader in such a variable
 * (`@_ZL19fc_has_attachment_0 = internal addrspace(2) global i8 undef`, stored in
 * _GLOBAL__sub_I_ogl_metal_uber.metal), and every branch on one was a load until inlining and
 * vars_to_ssa made it a constant -- so the whole shader was translated and pruned only
 * afterwards. Known here, the load is the number, the branch on it is decided while it is
 * translated (translate_body), and what it does not take is never translated at all. Integers
 * only (what air_eval computes), and only a global nothing but a static initializer stores to.
 */
static bool
air_stored_outside_static_init(struct air_ctx *c, const struct air_value *g)
{
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const struct air_function *f = c->m->functions[i];
      if (f->is_declaration || (f->section && !strcmp(f->section, "air.static_init")))
         continue;
      for (unsigned bi = 0; bi < f->num_blocks; bi++) {
         const struct air_block *blk = &f->blocks[bi];
         for (unsigned k = 0; k < blk->num_insts; k++) {
            const struct air_inst *in = blk->insts[k];
            if (in->op == AIR_OP_STORE && in->num_operands > 1 && in->operands[1] == g)
               return true;
         }
      }
   }
   return false;
}

static const struct air_value *
air_global_of_variable(struct air_ctx *c, nir_variable *var)
{
   for (unsigned i = 0; i < c->m->num_globals; i++)
      if (c->global_plan[i].class == AIR_PTR_VARIABLE && c->global_plan[i].var == var)
         return &c->m->globals[i]->base;
   return NULL;
}

/* the program-scope variable laid out in scratch at `offset`, when the load is the whole of it
 * (QuartzCore's function-constant copies are `global i8 undef`, an initializer, so scratch) */
static const struct air_value *
air_global_at_scratch(struct air_ctx *c, nir_def *offset, const struct air_type *type)
{
   if (!nir_src_is_const(nir_src_for_ssa(offset)))
      return NULL;
   uint64_t at = nir_src_as_uint(nir_src_for_ssa(offset));
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      const struct air_global_plan *pl = &c->global_plan[i];
      const struct air_global *g = c->m->globals[i];
      if (pl->skip || pl->class != AIR_PTR_SCRATCH || !pl->has_offset || pl->offset != at)
         continue;
      if (air_type_alloc_size(c->m, g->value_type) != air_type_alloc_size(c->m, type))
         return NULL;
      return &g->base;
   }
   return NULL;
}

static void
air_static_run(struct air_ctx *c)
{
   if (c->static_stores)
      return;
   /* every constructor once, in module order, as @llvm.global_ctors runs them */
   struct air_eval_env run = { .mem = ralloc_context(c->mem_ctx) };
   run.vals = _mesa_pointer_hash_table_create(run.mem);
   run.stores = _mesa_pointer_hash_table_create(run.mem);
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const struct air_function *f = c->m->functions[i];
      if (!f->is_declaration && f->section && !strcmp(f->section, "air.static_init"))
         eval_run_function(c, &run, f);
   }
   c->static_stores = run.stores;
   c->static_mem = run.mem;
}

/* A store's pointer as (global, constant byte offset into it): the global itself, a bitcast of
 * one, or a constant getelementptr with constant indices on one (the walk of air_constant_val). */
static bool
air_const_global_offset(struct air_ctx *c, const struct air_value *v, const struct air_value **g,
                        uint64_t *off, const struct air_type **at)
{
   if (!v)
      return false;
   if (v->kind == AIR_VALUE_GLOBAL) {
      *g = v;
      *off = 0;
      *at = NULL;      /* the whole global */
      return true;
   }
   const struct air_constant *k = air_value_as_constant(v);
   if (!k || k->ckind != AIR_CONST_EXPR)
      return false;
   if (k->expr_op == AIR_OP_CAST && k->expr_sub == AIR_CAST_BITCAST)
      return air_const_global_offset(c, k->operands[0], g, off, at);
   if (k->expr_op != AIR_OP_GEP || k->num_operands < 2 || !k->src_type)
      return false;
   uint64_t base, idx;
   const struct air_type *bt;
   if (!air_const_global_offset(c, k->operands[0], g, &base, &bt) || !air_const_uint(k->operands[1], &idx))
      return false;
   const struct air_type *t = k->src_type;
   uint64_t o = base + idx * air_type_alloc_size(c->m, t);
   for (unsigned i = 2; i < k->num_operands; i++) {
      if (!air_const_uint(k->operands[i], &idx))
         return false;
      if (t->kind == AIR_TYPE_STRUCT) {
         if (idx >= t->num_members)
            return false;
         o += air_struct_member_offset(c->m, t, idx);
         t = t->members[idx];
      } else if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_VECTOR) {
         o += idx * (t->kind == AIR_TYPE_ARRAY ? air_type_alloc_size(c->m, t->elem)
                                               : MAX2(air_type_bit_size(t->elem) / 8, 1));
         t = t->elem;
      } else {
         return false;
      }
   }
   *off = o;
   *at = t;
   return true;
}

/* A FIELD of a program-scope struct the static initializers fill one field at a time: iOS 26
 * QuartzCore's fc_fragment_shader_config, which fixed_frag reads through `select has_config,
 * @config, %buffer` -- its color opcodes decide whether the twelve apply_color_opcode calls
 * run at all, and unknown they were all translated and folded away again afterwards (42 000
 * instructions into the opt loops for a 26-instruction specialisation; 0.5 s a pipeline on the
 * phone, run glass20). The value is the one store of exactly this field and type; any other
 * store that overlaps it, or any store outside the initializers, leaves it unknown. */
static nir_def *
air_static_field(struct air_ctx *c, nir_def *offset, const struct air_type *type)
{
   if (!nir_src_is_const(nir_src_for_ssa(offset)) || type->kind == AIR_TYPE_POINTER ||
       air_type_is_float(air_type_scalar(type)))
      return NULL;
   uint64_t at = nir_src_as_uint(nir_src_for_ssa(offset));
   const struct air_value *g = NULL;
   uint64_t field = 0;
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      const struct air_global_plan *pl = &c->global_plan[i];
      const struct air_global *gl = c->m->globals[i];
      if (pl->skip || pl->class != AIR_PTR_SCRATCH || !pl->has_offset)
         continue;
      uint64_t size = air_type_alloc_size(c->m, gl->value_type);
      if (at >= pl->offset && at < pl->offset + size) {
         g = &gl->base;
         field = at - pl->offset;
         break;
      }
   }
   if (getenv("AIR_FIELD_DEBUG"))
      fprintf(stderr, "air field: at %llu -> %s +%llu\n", (unsigned long long)at,
              g && g->name ? g->name : "(none)", (unsigned long long)field);
   if (!g)
      return NULL;
   air_static_run(c);
   unsigned comps = air_nir_components(type), bits = air_nir_bit_size(c, type);
   uint64_t want = comps * MAX2(bits / 8, 1);
   const struct air_eval *found = NULL;
   hash_table_foreach(c->static_stores, he) {
      const struct air_value *sg;
      const struct air_type *st;
      uint64_t so;
      if (!air_const_global_offset(c, he->key, &sg, &so, &st) || sg != g)
         continue;
      const struct air_eval *e = he->data;
      /* how much it wrote: what was stored, or the field the pointer names when that is unknown */
      uint64_t ssize = e ? e->n * MAX2(e->bits / 8, 1)
                         : st ? air_type_alloc_size(c->m, st) : ~0ull;
      if (!e) {
         if (so + ssize <= field || field + want <= so)
            continue;            /* something unknown, but in another field */
         if (getenv("AIR_FIELD_DEBUG"))
            fprintf(stderr, "air field:   +%llu: unknown store at +%llu size %llu\n", (unsigned long long)field,
                    (unsigned long long)so, (unsigned long long)ssize);
         return NULL;
      }
      if (so + ssize <= field || field + want <= so)
         continue;               /* another field */
      if (so != field || e->n != comps || e->bits != bits || (found && found != e)) {
         if (getenv("AIR_FIELD_DEBUG"))
            fprintf(stderr, "air field:   +%llu (%ux%u): store at +%llu %ux%u%s\n", (unsigned long long)field,
                    comps, bits, (unsigned long long)so, e->n, e->bits, found ? " (twice)" : "");
         return NULL;            /* overlapping differently, or stored twice */
      }
      found = e;
   }
   if (getenv("AIR_FIELD_DEBUG"))
      fprintf(stderr, "air field:   found %d outside %d\n", found != NULL,
              found ? air_stored_outside_static_init(c, g) : -1);
   if (!found || comps > NIR_MAX_VEC_COMPONENTS || air_stored_outside_static_init(c, g))
      return NULL;
   nir_const_value v[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < comps; i++)
      v[i] = nir_const_value_for_raw_uint(found->lane[i], bits);
   return nir_build_imm(&c->b, comps, bits, v);
}

static nir_def *
air_static_value(struct air_ctx *c, const struct air_value *g, const struct air_type *type)
{
   if (!g || type->kind == AIR_TYPE_POINTER || air_type_is_float(air_type_scalar(type)))
      return NULL;
   air_static_run(c);
   struct hash_entry *he = _mesa_hash_table_search(c->static_stores, g);
   unsigned comps = air_nir_components(type), bits = air_nir_bit_size(c, type);
   if (getenv("AIR_LIVE_DEBUG")) {
      static unsigned said;
      if (said++ < 8)
         fprintf(stderr, "air: static %s: %s (n %u bits %u, load %ux%u)\n", g->name ? g->name : "?",
                 !he ? "never stored" : !he->data ? "stored, unknown" : "known",
                 he && he->data ? ((const struct air_eval *)he->data)->n : 0,
                 he && he->data ? ((const struct air_eval *)he->data)->bits : 0, comps, bits);
   }
   if (!he || !he->data)
      return NULL;
   const struct air_eval *e = he->data;
   if (e->n != comps || e->bits != bits || comps > NIR_MAX_VEC_COMPONENTS)
      return NULL;
   /* (asked once per global: the answer is the same for every load of it) */
   if (air_stored_outside_static_init(c, g)) {
      he->data = NULL;
      return NULL;
   }
   nir_const_value v[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < comps; i++)
      v[i] = nir_const_value_for_raw_uint(e->lane[i], bits);
   return nir_build_imm(&c->b, comps, bits, v);
}

static int function_constant_index(struct air_ctx *c, const struct air_global *g);
static void plan_globals(struct air_ctx *c);

static bool
air_eval(struct air_ctx *c, struct air_eval_env *env, const struct air_value *v,
         struct air_eval *out, unsigned depth)
{
   if (!v || depth > 64)
      return false;
   if (env && v->kind == AIR_VALUE_INSTRUCTION) {
      struct hash_entry *he = _mesa_hash_table_search(env->vals, v);
      if (he) {
         *out = *(struct air_eval *)he->data;
         return true;
      }
   }
   if (v->kind == AIR_VALUE_CONSTANT) {
      const struct air_constant *k = air_value_as_constant(v);
      if (!eval_shape(v->type, out))
         return false;
      switch (k->ckind) {
      case AIR_CONST_INT:
         if (out->n != 1)
            return false;
         out->lane[0] = k->u & eval_mask(out->bits);
         return true;
      case AIR_CONST_NULL:
      case AIR_CONST_UNDEF:
      case AIR_CONST_POISON:
         memset(out->lane, 0, sizeof(out->lane));
         return true;
      case AIR_CONST_DATA:
         if (k->num_elems != out->n)
            return false;
         for (unsigned i = 0; i < out->n; i++)
            out->lane[i] = k->data[i] & eval_mask(out->bits);
         return true;
      case AIR_CONST_AGGREGATE:
         if (k->num_elems != out->n)
            return false;
         for (unsigned i = 0; i < out->n; i++) {
            uint64_t u;
            if (!air_const_uint(k->elems[i], &u))
               return false;
            out->lane[i] = u & eval_mask(out->bits);
         }
         return true;
      default:
         return false;
      }
   }
   const struct air_inst *in = air_value_as_inst(v);
   if (!in)
      return false;
   struct air_eval a, b, s;
   switch (in->op) {
   case AIR_OP_PHI:
      /* the value on the edge control came in by: only inside a run */
      if (!env || !env->prev)
         return false;
      for (unsigned i = 0; i < in->num_targets; i++)
         if (in->targets[i] == env->prev)
            return air_eval(c, env, in->operands[i], out, depth + 1);
      return false;
   case AIR_OP_LOAD:
      /* through a global, which is a function constant or something the constructor wrote */
      if (in->operands[0]->kind != AIR_VALUE_GLOBAL ||
          !eval_global(c, env, in->operands[0], &a, depth))
         return false;
      if (!eval_shape(v->type, out) || out->n != a.n || out->bits != a.bits)
         return false;
      *out = a;
      return true;
   case AIR_OP_FREEZE:
      return air_eval(c, env, in->operands[0], out, depth + 1);
   case AIR_OP_CAST:
      if (!air_eval(c, env, in->operands[0], &a, depth + 1) || !eval_shape(v->type, out) ||
          out->n != a.n)
         return false;
      for (unsigned i = 0; i < out->n; i++) {
         switch (in->sub) {
         case AIR_CAST_TRUNC:
         case AIR_CAST_ZEXT:
            out->lane[i] = a.lane[i] & eval_mask(out->bits);
            break;
         case AIR_CAST_SEXT:
            out->lane[i] = (uint64_t)eval_signed(a.lane[i], a.bits) & eval_mask(out->bits);
            break;
         case AIR_CAST_BITCAST:
            if (out->bits != a.bits)
               return false;
            out->lane[i] = a.lane[i];
            break;
         default:
            return false;
         }
      }
      return true;
   case AIR_OP_BINOP:
      if (!air_eval(c, env, in->operands[0], &a, depth + 1) ||
          !air_eval(c, env, in->operands[1], &b, depth + 1) || a.n != b.n)
         return false;
      *out = a;
      for (unsigned i = 0; i < a.n; i++) {
         uint64_t x = a.lane[i], y = b.lane[i], r;
         int64_t sx = eval_signed(x, a.bits), sy = eval_signed(y, a.bits);
         switch (in->sub) {
         case AIR_BINOP_ADD: r = x + y; break;
         case AIR_BINOP_SUB: r = x - y; break;
         case AIR_BINOP_MUL: r = x * y; break;
         case AIR_BINOP_UDIV: if (!y) return false; r = x / y; break;
         case AIR_BINOP_UREM: if (!y) return false; r = x % y; break;
         case AIR_BINOP_SDIV: if (!sy) return false; r = (uint64_t)(sx / sy); break;
         case AIR_BINOP_SREM: if (!sy) return false; r = (uint64_t)(sx % sy); break;
         case AIR_BINOP_SHL: r = y >= a.bits ? 0 : x << y; break;
         case AIR_BINOP_LSHR: r = y >= a.bits ? 0 : x >> y; break;
         case AIR_BINOP_ASHR: r = (uint64_t)(sx >> MIN2(y, a.bits - 1)); break;
         case AIR_BINOP_AND: r = x & y; break;
         case AIR_BINOP_OR: r = x | y; break;
         case AIR_BINOP_XOR: r = x ^ y; break;
         default: return false;
         }
         out->lane[i] = r & eval_mask(a.bits);
      }
      return true;
   case AIR_OP_CMP:
      if (!air_eval(c, env, in->operands[0], &a, depth + 1) ||
          !air_eval(c, env, in->operands[1], &b, depth + 1) || a.n != b.n)
         return false;
      out->n = a.n;
      out->bits = 1;
      for (unsigned i = 0; i < a.n; i++) {
         uint64_t x = a.lane[i], y = b.lane[i];
         int64_t sx = eval_signed(x, a.bits), sy = eval_signed(y, a.bits);
         bool r;
         switch (in->sub) {
         case AIR_ICMP_EQ: r = x == y; break;
         case AIR_ICMP_NE: r = x != y; break;
         case AIR_ICMP_UGT: r = x > y; break;
         case AIR_ICMP_UGE: r = x >= y; break;
         case AIR_ICMP_ULT: r = x < y; break;
         case AIR_ICMP_ULE: r = x <= y; break;
         case AIR_ICMP_SGT: r = sx > sy; break;
         case AIR_ICMP_SGE: r = sx >= sy; break;
         case AIR_ICMP_SLT: r = sx < sy; break;
         case AIR_ICMP_SLE: r = sx <= sy; break;
         default: return false;
         }
         out->lane[i] = r;
      }
      return true;
   case AIR_OP_SELECT:
      if (!air_eval(c, env, in->operands[0], &s, depth + 1) ||
          !air_eval(c, env, in->operands[1], &a, depth + 1) ||
          !air_eval(c, env, in->operands[2], &b, depth + 1) || a.n != b.n ||
          (s.n != 1 && s.n != a.n))
         return false;
      *out = a;
      for (unsigned i = 0; i < a.n; i++)
         out->lane[i] = s.lane[s.n == 1 ? 0 : i] ? a.lane[i] : b.lane[i];
      return true;
   case AIR_OP_EXTRACTELT: {
      uint64_t idx;
      if (!air_eval(c, env, in->operands[0], &a, depth + 1) ||
          !air_const_uint(in->operands[1], &idx) || idx >= a.n)
         return false;
      out->n = 1;
      out->bits = a.bits;
      out->lane[0] = a.lane[idx];
      return true;
   }
   case AIR_OP_INSERTELT: {
      uint64_t idx;
      if (!air_eval(c, env, in->operands[0], out, depth + 1) ||
          !air_eval(c, env, in->operands[1], &b, depth + 1) ||
          !air_const_uint(in->operands[2], &idx) || idx >= out->n || b.n != 1)
         return false;
      out->lane[idx] = b.lane[0];
      return true;
   }
   case AIR_OP_CALL: {
      const char *callee = in->operands[0]->name;
      /* air.is_function_constant_defined(@x.MTL_FC_INIT_n): whether constant n was given, as
       * fc_defined_op translates it. QuartzCore's static initializer builds its fragment shader
       * config out of `select defined(x), x, 0` per field, and unknown here the config was
       * unknown and the whole color program translated (air_static_field). */
      if (callee && !strcmp(callee, "air.is_function_constant_defined") && in->num_operands == 2) {
         /* the constant's index as plan_globals finds it (function_constant_index): this can run
          * before the globals are planned (gated_off, from the interface) */
         const struct air_value *gv = in->operands[1];
         int index = -1;
         for (unsigned i = 0; gv && i < c->m->num_globals; i++)
            if (&c->m->globals[i]->base == gv && c->m->globals[i]->externally_initialized)
               index = function_constant_index(c, c->m->globals[i]);
         if (index < 0)
            return false;
         bool defined = false;
         for (unsigned k = 0; c->options && k < c->options->num_constants; k++)
            if (c->options->constants[k].index == index)
               defined = !c->options->constants[k].undefined;
         out->n = 1;
         out->bits = 1;
         out->lane[0] = defined;
         return true;
      }
      if (!callee || strncmp(callee, "air.normalize_function_constant_predicate.", 42) ||
          in->num_operands != 2 || !air_eval(c, env, in->operands[1], &a, depth + 1) ||
          !eval_shape(v->type, out) || out->n != a.n)
         return false;
      /* x != 0, as fc_pred_op translates it */
      for (unsigned i = 0; i < a.n; i++)
         out->lane[i] = a.lane[i] != 0;
      return true;
   }
   default:
      return false;
   }
}

/* operand i of `node` as a number: an integer, as it always was, or (iOS 26) a pointer to
 * a global the constructor computes from function constants */
static bool
md_int_or_eval(struct air_ctx *c, const struct air_md *node, unsigned i, uint64_t *out)
{
   if (md_int(node, i, out))
      return true;
   if (!node || i >= node->num_ops || !node->ops[i] || node->ops[i]->kind != AIR_MD_VALUE ||
       !node->ops[i]->value || node->ops[i]->value->kind != AIR_VALUE_GLOBAL)
      return false;
   struct air_eval e;
   if (!eval_global(c, NULL, node->ops[i]->value, &e, 0) || e.n != 1)
      return false;
   *out = e.lane[0];
   return true;
}

/* Whether `node` -- an entry point's argument or result -- is one its function-constant
 * predicate leaves out: `[[function_constant(p)]]`, metadata
 * `!"air.function_constant", !{ptr @pred, !"bool", !"name"}`, with p false for the constants
 * this translation was given. Metal's rule is that such an argument DOES NOT EXIST: the
 * application binds nothing there (CoreAnimation's fixed_frag_* config_buffer, when its
 * configuration is a constant instead; RenderBox's custom_fn table), and every use of it is
 * behind the same predicate. A predicate this cannot evaluate counts as true -- the argument
 * is then translated as it always was.
 */
static bool
gated_off(struct air_ctx *c, const struct air_md *node)
{
   const struct air_md *gate = md_key_node(node, "air.function_constant");
   struct air_eval e;

   return gate && gate->num_ops && gate->ops[0] && gate->ops[0]->kind == AIR_MD_VALUE &&
          gate->ops[0]->value && gate->ops[0]->value->kind == AIR_VALUE_GLOBAL &&
          eval_global(c, NULL, gate->ops[0]->value, &e, 0) && e.n == 1 && !e.lane[0];
}

static bool
translate_input(struct air_ctx *c, const struct air_md *node, unsigned *next_varying)
{
   uint64_t param;
   if (!md_int(node, 0, &param))
      return air_error(c, "interface: an input without a parameter number");
   if (param >= c->fn->num_args)
      return air_error(c, "interface: input for parameter %u of %u", (unsigned)param,
                       c->fn->num_args);
   const struct air_type *t = c->fn->args[param].type;

   /* An argument its predicate leaves out (gated_off) is not declared at all -- no resource
    * for the runtime to demand a binding for, no varying, no framebuffer fetch -- and is the
    * null of its kind: a null buffer reads zero (AIR_PTR_NONE), the null texture is the one
    * get_null_texture gives, a value is undef.
    */
   if (gated_off(c, node)) {
      if (t->kind != AIR_TYPE_POINTER)
         c->params[param] = air_val_ssa(c, nir_undef(&c->b, air_nir_components(t),
                                                     air_nir_bit_size(c, t)));
      else if (md_key(node, "air.texture") >= 0)
         c->params[param] = air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_TEXTURE,
                                                             .binding = AIR_NULL_BINDING });
      else if (md_key(node, "air.sampler") >= 0)
         c->params[param] = air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_SAMPLER,
                                                             .binding = AIR_NULL_BINDING });
      else
         c->params[param] = air_val_ptr(c, (struct air_ptr){ .class = AIR_PTR_NONE,
                                                             .pointee = t->elem });
      return true;
   }

   /* resources */
   if (md_key(node, "air.buffer") >= 0) {
      uint64_t as = 0, index = 0, size = 0;
      md_key_int(node, "air.address_space", &as);
      md_key_int(node, "air.location_index", &index);
      md_key_int(node, "air.buffer_size", &size);
      bool read = md_key(node, "air.read") >= 0 || md_key(node, "air.read_write") >= 0;
      bool write = md_key(node, "air.write") >= 0 || md_key(node, "air.read_write") >= 0;

      struct air_ptr p = { .binding = index, .pointee = t->elem };
      enum air_resource_kind rk;
      switch (as) {
      case 1: p.class = AIR_PTR_SSBO; rk = AIR_RESOURCE_DEVICE_BUFFER; break;
      case 2: p.class = AIR_PTR_UBO; rk = AIR_RESOURCE_CONSTANT_BUFFER; break;
      case 3:
         p.class = AIR_PTR_SHARED;
         rk = AIR_RESOURCE_THREADGROUP_BUFFER;
         /* a threadgroup argument is sized at dispatch; its base is where the
          * previous ones end, which only the runtime knows. One is all the
          * corpus uses, so its base is zero and anything else is refused.
          */
         if (index != 0)
            return air_error(c, "interface: a second threadgroup buffer (index %u)",
                             (unsigned)index);
         break;
      default:
         return air_error(c, "interface: a buffer in address space %u", (unsigned)as);
      }
      add_resource(c, rk, index, size, read, write, param, node);
      c->params[param] = air_val_ptr(c, p);
      return true;
   }

   if (md_key(node, "air.indirect_buffer") >= 0)
      return translate_argument_buffer(c, node, param);

   if (md_key(node, "air.texture") >= 0) {
      uint64_t index = 0;
      md_key_int(node, "air.location_index", &index);
      bool sample = md_key(node, "air.sample") >= 0;
      bool read = md_key(node, "air.read") >= 0;
      bool write = md_key(node, "air.write") >= 0 || md_key(node, "air.read_write") >= 0;
      /* A texture the shader writes is an image to Gallium, with its own
       * binding space; one it only samples or reads is a sampler view.
       */
      struct air_ptr p = {
         .class = AIR_PTR_TEXTURE,
         .binding = index,
         .is_image = write,
      };
      add_resource(c, write ? AIR_RESOURCE_IMAGE : AIR_RESOURCE_TEXTURE, index, 0,
                   sample || read, write, param, node);
      c->params[param] = air_val_ptr(c, p);
      return true;
   }

   if (md_key(node, "air.sampler") >= 0) {
      uint64_t index = 0;
      md_key_int(node, "air.location_index", &index);
      struct air_ptr p = { .class = AIR_PTR_SAMPLER, .binding = index };
      add_resource(c, AIR_RESOURCE_SAMPLER, index, 0, true, false, param, node);
      c->params[param] = air_val_ptr(c, p);
      return true;
   }

   /* system values */
   for (unsigned i = 0; i < ARRAY_SIZE(sysvals); i++) {
      if (md_key(node, sysvals[i].key) < 0)
         continue;
      nir_def *def = load_sysval(c, sysvals[i].sv);
      if (!def)
         return air_error(c, "interface: %s", sysvals[i].key);
      c->params[param] = air_val_ssa(c, fit(c, def, t));
      return true;
   }

   /* interpolated inputs of a fragment shader, and vertex attributes */
   if (md_key(node, "air.position") >= 0) {
      if (c->nir->info.stage == MESA_SHADER_FRAGMENT) {
         /* A position the shader does not read gets no input at all, and the order of
          * these two tests is the whole reason the test is here: every one of Apple's
          * seventeen texturedFragment* declares `float4 pos [[position]]` and marks it
          * air.arg_unused, so putting the position machinery before this check would
          * give all seventeen a dead varying slot and a dead FS_COORD property -- a
          * change to 17 shaders already compared pixel by pixel, bought for nothing.
          */
         if (md_key(node, "air.arg_unused") >= 0) {
            c->params[param] = air_val_ssa(c, nir_undef(&c->b, air_nir_components(t),
                                                        air_nir_bit_size(c, t)));
            return true;
         }
         /*
          * Metal's [[position]] in a fragment function is the window coordinate with
          * the origin at the TOP left, y down, and the pixel centre at a half-integer
          * -- which is what origin_upper_left plus !pixel_center_integer says in NIR.
          * NIR's default is OpenGL's bottom-left origin, so leaving these unset would
          * give every position-dependent Metal shader a y counted from the wrong edge.
          *
          * And the value is a SYSTEM-VALUE VARIABLE, not a bare nir_load_frag_coord,
          * because the two Gallium drivers this frontend feeds want opposite forms and
          * only one direction has a pass:
          *
          *   - freedreno sets pipe_caps.fs_position_is_sysval, and ir3 compile_asserts
          *     `slot != VARYING_SLOT_POS` in its fragment-input setup
          *     (ir3_compiler_nir.c): it will only take the intrinsic, which is what
          *     nir_lower_system_values turns this variable into;
          *   - softpipe does NOT set that cap, and nir_to_tgsi turns the intrinsic into
          *     ureg_DECL_system_value(TGSI_SEMANTIC_POSITION), which softpipe fills only
          *     for compute (sp_compute.c is the one writer of machine->SystemValue). A
          *     fragment shader therefore read (0,0,0,0) -- measured, not reasoned: a
          *     hand-written module returning its own position printed four zeros while
          *     the same module returning a constant printed the constant. Softpipe wants
          *     the position as an input varying, which only
          *     nir_lower_sysvals_to_varyings produces, and only from a variable.
          *
          * So the frontend emits the form spirv_to_nir emits, for the same reason, and
          * the consumer lowers it per its screen's cap (finalize_for_gallium).
          */
         nir_variable *var = nir_variable_create(c->nir, nir_var_system_value,
                                                 glsl_vec4_type(), "position");
         var->data.location = SYSTEM_VALUE_FRAG_COORD;
         var->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
         nir_def *pos = nir_load_var(&c->b, var);
         c->params[param] = air_val_ssa(c, fit(c, pos, t));
         c->nir->info.fs.origin_upper_left = true;
         c->nir->info.fs.pixel_center_integer = false;
         /* nothing in this path calls nir_shader_gather_info, and nir_to_tgsi emits the
          * FS_COORD_ORIGIN property only for a shader whose info says it reads the
          * position -- without this the property is absent and softpipe falls back on
          * TGSI_FS_COORD_ORIGIN_UPPER_LEFT (0), which is the right answer by accident.
          */
         BITSET_SET(c->nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
         c->nir->info.inputs_read |= VARYING_BIT_POS;
         return true;
      }
      return air_error(c, "interface: air.position as an input of a %s shader",
                       mesa_shader_stage_name(c->nir->info.stage));
   }

   /* Metal's `[[color(n)]]` argument to a fragment function: the value render target n
    * holds NOW, read inside the shader. That is framebuffer fetch, and Metal's name for
    * it is programmable blending.
    *
    * The modules say which feature it is rather than this being inferred from the name:
    * every one of the twenty in QuartzCore's library carries
    * `!"air.compile.framebuffer_fetch_enable"` among its `air.compile_options`, the
    * option Apple's compiler sets exactly when a fragment argument is `[[color(n)]]`.
    *
    * The shape is ONE integer after the key -- the target -- where the output form has
    * two, the second being the dual-source index:
    *
    *   output  !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", ...}
    *   input   !{i32 1, !"air.render_target", i32 2, !"air.arg_type_name", !"float", ...}
    *
    * so the two are read here and in translate_output at their own offsets, not through
    * one shared helper that would read a dual-source index out of an argument name.
    *
    * NIR's form of a framebuffer fetch is a READ OF THE OUTPUT: load_output carrying
    * io_semantics.fb_fetch_output, which is what nir_lower_io makes of a GLSL `inout`
    * colour and what nir_lower_blend emits for a blend it has to lower in software. The
    * intrinsic is built here directly instead of as a second nir_var_shader_out variable
    * at the same location, because both consumers of this frontend lower IO before the
    * backend sees it and two output variables at one location would take two driver
    * locations and two entries in ir3's output table for one attachment.
    *
    * It is read as FOUR 32-BIT FLOATS whatever the argument's type, and narrowed after:
    * nir_lower_fb_read -- the pass ir3 runs in ir3_nir_post_finalize -- rewrites
    * load_output into a txf_ms_fb whose destination is fixed at 4 x f32 and rewrites the
    * intrinsic's uses with it. So `float` (path_blit_frag's coverage mask) loses three
    * channels and `half4` (every _lph build) loses precision here, where the conversion
    * is visible, rather than silently inside the lowering.
    */
   int rt_in = md_key(node, "air.render_target");
   if (rt_in >= 0) {
      if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
         return air_error(c, "interface: air.render_target as an input of a %s shader",
                          mesa_shader_stage_name(c->nir->info.stage));
      /* An argument nothing reads must not turn the shader into a framebuffer-fetch
       * shader: info.fs.uses_fbfetch_output alone makes a6xx flush the pipeline per
       * overlapping primitive (fd6_emit.cc, build_prim_mode), and an unused argument
       * needs no value at all. None of the corpus's twenty is marked this way; the
       * order is here so that one would cost nothing rather than a flush.
       */
      if (md_key(node, "air.arg_unused") >= 0) {
         c->params[param] = air_val_ssa(c, nir_undef(&c->b, air_nir_components(t),
                                                     air_nir_bit_size(c, t)));
         return true;
      }
      uint64_t n = 0;
      if (!md_int_or_eval(c, node, rt_in + 1, &n))
         return air_error(c, "interface: an air.render_target input without a target");
      if (n >= MAX_DRAW_BUFFERS)
         return air_error(c, "interface: air.render_target input %u", (unsigned)n);
      /* Only a float target. The lowering hands back floats converted from the
       * attachment's format, so an int4 or uint4 `[[color(n)]]` would arrive as the
       * wrong value rather than the wrong type -- and nothing would say so. The corpus
       * declares float4, half4 and float and nothing else, so the refusal is a guard on
       * a case not yet seen rather than a gap with modules behind it.
       */
      unsigned bits = air_nir_bit_size(c, t);
      if (!air_type_is_float(air_type_scalar(t)) || (bits != 16 && bits != 32)) {
         const char *tn = md_key_str(node, "air.arg_type_name");
         return air_error(c, "interface: an air.render_target input of type %s",
                          tn ? tn : "?");
      }

      nir_io_semantics sem = {
         .location = FRAG_RESULT_DATA0 + n,
         .num_slots = 1,
         .fb_fetch_output = true,
         /* Coherent, which is Metal's guarantee and not a safe default picked here: a
          * `[[color(n)]]` read sees what the primitives already drawn left behind, which
          * is the whole of how CoreAnimation blends a layer in place. On a6xx the flag
          * is what makes the GMEM path flush per overlap instead of not at all
          * (fd6_emit.cc); the sysmem path flushes either way.
          */
         .fb_fetch_output_coherent = true,
      };
      nir_def *fetched = nir_load_output(&c->b, 4, 32, nir_imm_int(&c->b, 0),
                                         .dest_type = nir_type_float32,
                                         .io_semantics = sem);
      unsigned want = air_nir_components(t);
      if (want < 4)
         fetched = nir_trim_vector(&c->b, fetched, want);
      if (bits == 16)
         fetched = nir_f2f16(&c->b, fetched);

      /* nir_shader_gather_info does not run on this path -- ir3 calls it only for a
       * binning variant -- so what it would have set from the intrinsic
       * (nir_gather_info.c) is set here. Without uses_fbfetch_output the draw gets no
       * flush at all and the read returns whatever the tile buffer happened to hold.
       */
      c->nir->info.outputs_read |= BITFIELD64_BIT(sem.location);
      c->nir->info.fs.uses_fbfetch_output = true;
      c->nir->info.fs.fbfetch_coherent = true;
      c->info->fbfetch_targets |= 1u << n;

      /* The layer the fetch reads is not this frontend's to choose, and it is
       * not safe by default, so it goes in the log rather than only in a
       * document. nir_lower_fb_read builds the coordinate as (x, y, layer)
       * with the layer from nir_load_layer_id, which ir3_nir_lower_layer_id
       * turns into a flat varying read of VARYING_SLOT_LAYER. If the pipeline's
       * vertex stage writes air.render_target_array_index, ir3_link_shaders
       * finds it by slot and the fetch reads the right layer; if nothing wrote
       * it, ir3_link_add skips the entry (regid(63, 0)) and the varying holds
       * whatever the VPC location held. Which of the two it is cannot be known
       * from this module -- the fragment shader is identical either way -- so
       * the condition is stated for whoever builds the pipeline.
       */
      air_note(c, "the framebuffer fetch reads its layer from VARYING_SLOT_LAYER, which "
                  "nir_lower_fb_read asks for and this module cannot write: for a LAYERED "
                  "render target the pipeline's vertex stage must write "
                  "air.render_target_array_index, or the fetch reads an undefined layer. "
                  "A single-layer target is unaffected -- there is no second layer in the "
                  "descriptor to reach");

      c->params[param] = air_val_ssa(c, fetched);
      return true;
   }

   if (md_key(node, "air.fragment_input") >= 0 || md_key(node, "air.vertex_input") >= 0 ||
       md_key(node, "air.stage_in") >= 0) {
      /* A vertex attribute and an interpolated varying are two different things
       * that AIR spells with keys in the same family, and only the one below is
       * a varying: it is linked to the stage before it, interpolated, and takes
       * its slot from the order it appears in. An attribute is fetched from a
       * vertex buffer at Metal's own index and has nothing to interpolate.
       */
      if (c->nir->info.stage == MESA_SHADER_VERTEX)
         return translate_vertex_attribute(c, node, param, t);
      unsigned slot = VARYING_SLOT_VAR0 + (*next_varying)++;
      const char *name = md_key_str(node, "air.arg_name");
      nir_variable *var = make_io_var(c, nir_var_shader_in, t, slot, name);
      var->data.driver_location = c->nir->num_inputs++;
      if (md_key(node, "air.flat") >= 0)
         var->data.interpolation = INTERP_MODE_FLAT;
      else if (md_key(node, "air.no_perspective") >= 0)
         var->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
      else
         var->data.interpolation = INTERP_MODE_SMOOTH;
      var->data.centroid = md_key(node, "air.centroid") >= 0;
      var->data.sample = md_key(node, "air.sample") >= 0;
      if (c->nir->info.stage == MESA_SHADER_FRAGMENT)
         add_varying(c, name, slot, air_nir_components(t), node);
      nir_def *in_v = nir_load_var(&c->b, var);
      unsigned want = air_nir_bit_size(c, t);
      if (want == 1)
         in_v = nir_ine_imm(&c->b, in_v, 0);     /* a bool varying travels as an int */
      else if (want != in_v->bit_size)
         in_v = air_type_is_float(air_type_scalar(t)) ? nir_f2fN(&c->b, in_v, want)
                                                      : nir_i2iN(&c->b, in_v, want);
      c->params[param] = air_val_ssa(c, in_v);
      return true;
   }

   /* the one thing left that is not an error: an argument nothing reads */
   if (md_key(node, "air.arg_unused") >= 0) {
      c->params[param] = air_val_ssa(c, nir_undef(&c->b, air_nir_components(t),
                                                  air_nir_bit_size(c, t)));
      return true;
   }

   for (unsigned i = 0; i < node->num_ops; i++) {
      const char *s = md_str(node, i);
      if (s && !strncmp(s, "air.", 4) && strcmp(s, "air.arg_name") &&
          strcmp(s, "air.arg_type_name"))
         return air_error(c, "interface: input %s", s);
   }
   return air_error(c, "interface: an input with no key this translator knows");
}

static bool
translate_output(struct air_ctx *c, const struct air_md *node, unsigned field,
                 const struct air_type *t, unsigned *next_varying)
{
   unsigned location = ~0u, index = 0;
   const char *name = md_key_str(node, "air.arg_name");

   /* a result its predicate leaves out is not stored anywhere (emit_return skips a field
    * with no variable): the attachment it names may not even be there */
   if (gated_off(c, node)) {
      c->outputs[field].gated_off = true;
      return true;
   }

   int rt = md_key(node, "air.render_target");
   if (rt >= 0) {
      uint64_t n = 0, dual = 0;
      /* An expression this cannot evaluate is refused rather than read as zero: a store
       * to the wrong attachment looks exactly like a shader that draws the wrong thing. */
      if (!md_int_or_eval(c, node, rt + 1, &n))
         return air_error(c, "interface: an air.render_target output whose target is "
                             "neither a number nor an expression of function constants "
                             "this can evaluate");
      md_int(node, rt + 2, &dual);
      if (n >= MAX_DRAW_BUFFERS)
         return air_error(c, "interface: air.render_target output %u", (unsigned)n);
      location = FRAG_RESULT_DATA0 + n;
      index = dual;
      c->info->num_render_targets = MAX2(c->info->num_render_targets, n + 1);
   } else if (md_key(node, "air.depth") >= 0) {
      location = FRAG_RESULT_DEPTH;
      const char *q = md_key_str(node, "air.depth_qualifier");
      c->nir->info.fs.depth_layout =
         !q || !strcmp(q, "air.any") ? FRAG_DEPTH_LAYOUT_ANY :
         !strcmp(q, "air.greater") ? FRAG_DEPTH_LAYOUT_GREATER :
         !strcmp(q, "air.less") ? FRAG_DEPTH_LAYOUT_LESS : FRAG_DEPTH_LAYOUT_UNCHANGED;
   } else if (md_key(node, "air.sample_mask") >= 0) {
      location = FRAG_RESULT_SAMPLE_MASK;
   } else if (md_key(node, "air.stencil") >= 0) {
      location = FRAG_RESULT_STENCIL;
   } else if (md_key(node, "air.position") >= 0) {
      location = VARYING_SLOT_POS;
   } else if (md_key(node, "air.point_size") >= 0) {
      location = VARYING_SLOT_PSIZ;
   } else if (md_key(node, "air.clip_distance") >= 0) {
      location = VARYING_SLOT_CLIP_DIST0;
   } else if (md_key(node, "air.render_target_array_index") >= 0) {
      location = VARYING_SLOT_LAYER;
   } else if (md_key(node, "air.viewport_array_index") >= 0) {
      location = VARYING_SLOT_VIEWPORT;
   } else if (md_key(node, "air.vertex_output") >= 0) {
      location = VARYING_SLOT_VAR0 + (*next_varying)++;
      add_varying(c, md_str(node, md_key(node, "air.vertex_output") + 1), location,
                  air_nir_components(t), node);
   } else {
      for (unsigned i = 0; i < node->num_ops; i++) {
         const char *s = md_str(node, i);
         if (s && !strncmp(s, "air.", 4) && strcmp(s, "air.arg_name") &&
             strcmp(s, "air.arg_type_name"))
            return air_error(c, "interface: output %s", s);
      }
      return air_error(c, "interface: an output with no key this translator knows");
   }

   /* An output whose type is not a number has no varying here, and until this
    * was checked it got one anyway: an `air.clip_distance` output arrives as
    * an ARRAY of float, and air_type_scalar has no answer for an array, so the
    * varying was declared with whatever air_nir_bit_size made of that. NIR's
    * own form is an output of several slots, which this frontend does not
    * build, so it is refused by name instead.
    *
    * The length is in the message because it decides how much work closing
    * this is, and the measured length is not the one the general case
    * suggests: over all 86 of Resident Evil 4's modules that stop here --
    * every one a vertex shader -- the member is `[1 x float]`, an array of
    * exactly ONE, and Metal's own `air.arg_type_name` on the node says
    * `float`. So there is no module in reach that declares two clip planes,
    * and the shape that is actually in the way is an array of one, which is a
    * scalar wearing a bracket. Closing that is two things and not one: this
    * declaration would take the element type, and emit_return would have to
    * store elems[0] of a one-element aggregate instead of skipping a field
    * that is not an AIR_VAL_SSA -- which is what silently drops the plane
    * today. It is left refused rather than half built, because a clip distance
    * that is written to nothing is a shader that draws without the clipping it
    * asked for, and nothing in this tree can check a vertex clip distance
    * numerically.
    */
   if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_STRUCT) {
      const char *tn = md_key_str(node, "air.arg_type_name");
      if (t->kind == AIR_TYPE_ARRAY)
         return air_error(c, "interface: an output (%s), declared %s, that arrives as an "
                             "array of %u, which this frontend has no varying for",
                          name ? name : "unnamed", tn ? tn : "an aggregate",
                          (unsigned)t->count);
      return air_error(c, "interface: an output (%s), declared %s, that arrives as a "
                          "struct of %u members, which this frontend has no varying for",
                       name ? name : "unnamed", tn ? tn : "an aggregate", t->num_members);
   }

   nir_variable *var = make_io_var(c, nir_var_shader_out, t, location, name);
   var->data.index = index;
   var->data.driver_location = c->nir->num_outputs++;
   c->outputs[field].location = location;
   c->outputs[field].index = index;
   c->outputs[field].var = var;
   return true;
}

/* -- instructions ------------------------------------------------------- */

/* A load through a pointer this frontend could not resolve: load through BOTH
 * sides of the choice and select the values. That is the only form NIR has --
 * there is no select of pointers in the 32bit_index_offset model -- and it is
 * not free of consequence, so it is said out loud: both sides are read, so a
 * side that is not a valid place to read from is read anyway. In the shaders
 * that need this one side is a module constant array and the other a buffer
 * the pipeline binds, so both are readable; a pipeline that bound neither
 * would read a descriptor it did not set up.
 */
static nir_def *
load_through_choice(struct air_ctx *c, struct air_val *v, const struct air_type *t,
                    unsigned align)
{
   if (v->kind == AIR_VAL_PTR)
      return air_load_ptr(c, v->ptr, t, align);
   if (v->kind != AIR_VAL_PTR_SELECT) {
      air_error(c, "instruction: a load through something that is not a pointer");
      return NULL;
   }
   nir_def *a = load_through_choice(c, v->elems[0], t, align);
   nir_def *b = load_through_choice(c, v->elems[1], t, align);
   if (!a || !b)
      return NULL;
   air_note(c, "a pointer chosen at run time (`select` between two buffers): both sides "
               "are LOADED and the values selected, because NIR has no select of "
               "pointers -- so a side the pipeline left unbound is read anyway, and its "
               "value discarded");
   return nir_bcsel(&c->b, v->def, a, b);
}

static bool
gep_into(struct air_ctx *c, struct air_val *base, const struct air_inst *in,
         struct air_val **out)
{
   /* A getelementptr on a pointer CHOSEN at run time: the index goes into both
    * sides and the choice stays. Both sides have the same pointee type -- the
    * select that made the choice required it -- so this is one walk per side
    * and not a different offset on each.
    */
   if (base->kind == AIR_VAL_PTR_SELECT) {
      struct air_val *a = NULL, *b = NULL;
      if (!gep_into(c, base->elems[0], in, &a) ||
          !gep_into(c, base->elems[1], in, &b))
         return false;
      *out = air_val_ptr_select(c, base->def, a, b);
      return true;
   }
   if (base->kind != AIR_VAL_PTR)
      return air_error(c, "instruction: getelementptr on something that is not a "
                          "pointer");

   struct air_ptr p = base->ptr;
   const struct air_type *t = in->aux_type;

   /* index 0 steps over objects of the source type */
   uint64_t stride = air_type_alloc_size(c->m, t);
   uint64_t first_const;
   if (!air_const_uint(in->operands[1], &first_const) || first_const != 0) {
      nir_def *first = air_get_ssa(c, in->operands[1]);
      if (!first)
         return false;
      p = air_ptr_add(c, p, nir_imul_imm(&c->b, air_u32(&c->b, first), stride));
   }

   for (unsigned i = 2; i < in->num_operands; i++) {
      nir_def *idx = air_get_ssa(c, in->operands[i]);
      if (!idx)
         return false;
      if (t->kind == AIR_TYPE_STRUCT) {
         uint64_t field;
         if (!air_const_uint(in->operands[i], &field))
            return air_error(c, "instruction: a struct field chosen at run time");
         unsigned n = field;
         if (n >= t->num_members)
            return air_error(c, "instruction: struct field %u of %u", n, t->num_members);
         uint64_t off = air_struct_member_offset(c->m, t, n);
         if (off)
            p = air_ptr_add(c, p, nir_imm_int(&c->b, off));
         t = t->members[n];
      } else if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_VECTOR) {
         uint64_t es = t->kind == AIR_TYPE_ARRAY ? air_type_alloc_size(c->m, t->elem)
                                                 : MAX2(air_type_bit_size(t->elem) / 8, 1);
         p = air_ptr_add(c, p, nir_imul_imm(&c->b, air_u32(&c->b, idx), es));
         t = t->elem;
      } else {
         return air_error(c, "instruction: getelementptr into a scalar");
      }
   }
   p.pointee = t;
   *out = air_val_ptr(c, p);
   return true;
}

static bool
translate_gep(struct air_ctx *c, const struct air_inst *in)
{
   struct air_val *base = air_get(c, in->operands[0]);
   if (!base)
      return false;
   struct air_val *res = NULL;
   if (!gep_into(c, base, in, &res))
      return false;
   air_set(c, &in->base, res);
   return true;
}

static bool
translate_binop(struct air_ctx *c, const struct air_inst *in)
{
   nir_def *x = air_get_ssa(c, in->operands[0]);
   nir_def *y = air_get_ssa(c, in->operands[1]);
   if (!x || !y)
      return false;
   bool is_float = air_type_is_float(air_type_scalar(in->base.type));
   nir_def *res;

   /* Metal compiles with fast math on, and the modules say so per
    * instruction. Without the flags the arithmetic must not be reassociated,
    * so the builder is told to keep it exact.
    */
   uint32_t was_exact = c->b.fp_math_ctrl;
   if (is_float)
      c->b.fp_math_ctrl = (in->flags & (AIR_FMF_UNSAFE_ALGEBRA | AIR_FMF_REASSOC)) ? 0 : nir_fp_exact;

   switch (in->sub) {
   case AIR_BINOP_ADD: res = is_float ? nir_fadd(&c->b, x, y) : nir_iadd(&c->b, x, y); break;
   case AIR_BINOP_SUB: res = is_float ? nir_fsub(&c->b, x, y) : nir_isub(&c->b, x, y); break;
   case AIR_BINOP_MUL: res = is_float ? nir_fmul(&c->b, x, y) : nir_imul(&c->b, x, y); break;
   case AIR_BINOP_UDIV: res = is_float ? nir_fdiv(&c->b, x, y) : nir_udiv(&c->b, x, y); break;
   case AIR_BINOP_SDIV: res = is_float ? nir_fdiv(&c->b, x, y) : nir_idiv(&c->b, x, y); break;
   case AIR_BINOP_UREM: res = is_float ? nir_frem(&c->b, x, y) : nir_umod(&c->b, x, y); break;
   case AIR_BINOP_SREM: res = is_float ? nir_frem(&c->b, x, y) : nir_irem(&c->b, x, y); break;
   case AIR_BINOP_SHL: res = nir_ishl(&c->b, x, air_u32(&c->b, y)); break;
   case AIR_BINOP_LSHR: res = nir_ushr(&c->b, x, air_u32(&c->b, y)); break;
   case AIR_BINOP_ASHR: res = nir_ishr(&c->b, x, air_u32(&c->b, y)); break;
   case AIR_BINOP_AND: res = nir_iand(&c->b, x, y); break;
   case AIR_BINOP_OR: res = nir_ior(&c->b, x, y); break;
   case AIR_BINOP_XOR: res = nir_ixor(&c->b, x, y); break;
   default:
      c->b.fp_math_ctrl = was_exact;
      return air_error(c, "instruction: binary operator %u", in->sub);
   }
   c->b.fp_math_ctrl = was_exact;
   air_set(c, &in->base, air_val_ssa(c, res));
   return true;
}

static bool
translate_cast(struct air_ctx *c, const struct air_inst *in)
{
   const struct air_type *dt = in->base.type;
   const struct air_type *st = in->operands[0]->type;

   if (in->sub == AIR_CAST_BITCAST && dt->kind == AIR_TYPE_POINTER) {
      struct air_val *v = air_get(c, in->operands[0]);
      if (!v || v->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a bitcast of something that is not a pointer");
      struct air_ptr p = v->ptr;
      p.pointee = dt->elem;
      air_set(c, &in->base, air_val_ptr(c, p));
      return true;
   }

   nir_def *x = air_get_ssa(c, in->operands[0]);
   if (!x)
      return false;
   unsigned bits = air_nir_bit_size(c, dt);
   nir_def *res;
   switch (in->sub) {
   case AIR_CAST_TRUNC:
      res = bits == 1 ? nir_i2b(&c->b, nir_iand_imm(&c->b, x, 1)) : nir_u2uN(&c->b, x, bits);
      break;
   case AIR_CAST_ZEXT:
      res = x->bit_size == 1 ? nir_b2iN(&c->b, x, bits) : nir_u2uN(&c->b, x, bits);
      break;
   case AIR_CAST_SEXT:
      res = x->bit_size == 1 ? nir_ineg(&c->b, nir_b2iN(&c->b, x, bits))
                             : nir_i2iN(&c->b, x, bits);
      break;
   case AIR_CAST_FPTOUI: res = nir_f2uN(&c->b, x, bits); break;
   case AIR_CAST_FPTOSI: res = nir_f2iN(&c->b, x, bits); break;
   case AIR_CAST_UITOFP:
      res = x->bit_size == 1 ? nir_b2fN(&c->b, x, bits) : nir_u2fN(&c->b, x, bits);
      break;
   case AIR_CAST_SITOFP:
      res = x->bit_size == 1 ? nir_fneg(&c->b, nir_b2fN(&c->b, x, bits))
                             : nir_i2fN(&c->b, x, bits);
      break;
   case AIR_CAST_FPTRUNC: res = nir_f2fN(&c->b, x, bits); break;
   case AIR_CAST_FPEXT: res = nir_f2fN(&c->b, x, bits); break;
   /* A bitcast moves no bits: it renames them. When the two types have the
    * same shape -- the same component count and the same component width --
    * the rename is a NIR `mov`, because a NIR def carries a width and a
    * component count and no notion of float or integer, so `i32 -> float` and
    * `<4 x i32> -> <4 x float>` are already the same def.
    *
    * When the shape differs the bits have to be redistributed between
    * components, and NIR has an operation for exactly one redistribution of
    * exactly one width: pack_32_2x16 and unpack_32_2x16. That is the whole of
    * what this corpus asks for, and the number is the reason this is two cases
    * and not a general repacker -- `tools/air/typecensus.py` over all 7 879 of
    * Resident Evil 4's modules, reading the types out of llvm-dis rather than
    * out of our own reader, finds 111 657 bitcasts of 14 distinct type pairs,
    * and the ONLY two that change shape are:
    *
    *     i32        -> <2 x half>    44 136 of them, in 2 927 modules
    *     <2 x half> -> i32            4 333 of them, in   688 modules
    *
    * with 0 shape-changing bitcasts in the whole 456-module system corpus.
    * There is no third shape to be found behind these, so the pair below is
    * not a sample of a family: it is the family. Anything else keeps a refusal
    * -- and one that names the two shapes, so that a third, if some other
    * library has one, arrives as an arithmetic fact instead of a mystery.
    *
    * The half/half-pair of an `i32` is how an HLSL `min16float2` reaches here
    * through SPIRV-Cross: the pair travels through the shader packed in one
    * 32-bit register and is unpacked where it is used.
    *
    * Keyed on the SHAPE and not on the AIR type names, deliberately. NIR's
    * SSA has no float at 32 bits that is distinct from an integer at 32 bits,
    * so `float -> <2 x half>` -- which this corpus does not contain -- is the
    * same bit movement as `i32 -> <2 x half>` and is right for the same
    * reason. What a case keyed on the names would buy is a refusal for a
    * module whose bits are being moved correctly.
    *
    * Which half goes where is little-endian, which is not a choice: LLVM lays
    * element 0 of a vector at the lowest address, so element 0 of the
    * `<2 x half>` is bits 0..15 of the `i32`, and `unpack_32_2x16` is defined
    * `dst.x = src0.x; dst.y = src0.x >> 16` -- the same halves in the same
    * order. Getting that backwards would not crash and would not look wrong:
    * both halves are still finite halves, so the shader would draw with every
    * pair of 16-bit values transposed. tools/air/loweringcheck.py gives the
    * two halves values that cannot be confused for each other (13 and 113, 1
    * and -2) and checks them twice -- against LLVM running the same module on
    * this processor, and against the four numbers written out in the case
    * list, which come from Python's own binary16 codec and so are not LLVM's
    * endianness restated. It also says what the transposed answer would have
    * been. The drawn reference is NOT available for these: softpipe's
    * nir_to_tgsi has no 16-bit arithmetic at all, with or without a bitcast.
    */
   case AIR_CAST_BITCAST: {
      unsigned dn = air_nir_components(dt), sn = air_nir_components(st);
      unsigned db = bits, sb = air_nir_bit_size(c, st);
      if (dn == sn && db == sb)
         res = nir_mov(&c->b, x);
      else if (sn == 1 && sb == 32 && dn == 2 && db == 16)
         res = nir_unpack_32_2x16(&c->b, x);
      else if (sn == 2 && sb == 16 && dn == 1 && db == 32)
         res = nir_pack_32_2x16(&c->b, x);
      else if (sn * sb == dn * db && sb >= 8 && db >= 8)
         /* any other repacking of the same bits, little-endian as above (`<2 x i32>` to
          * `<4 x half>`: 77 of MPSImage's kernels) */
         res = nir_bitcast_vector(&c->b, x, db);
      else
         return air_error(c, "instruction: a bitcast from %u x %u bits to %u x %u bits, "
                             "which is a repacking this frontend does not do",
                          sn, sb, dn, db);
      break;
   }
   default:
      return air_error(c, "instruction: cast %u", in->sub);
   }
   air_set(c, &in->base, air_val_ssa(c, res));
   return true;
}

static bool
translate_cmp(struct air_ctx *c, const struct air_inst *in)
{
   nir_def *x = air_get_ssa(c, in->operands[0]);
   nir_def *y = air_get_ssa(c, in->operands[1]);
   if (!x || !y)
      return false;
   nir_def *res;
   switch (in->sub) {
   /* NaN never appears in these shaders (nnan is set on every one), so the
    * ordered and unordered forms of a comparison are the same comparison;
    * what differs is only the answer when an operand is NaN.
    */
   case AIR_FCMP_OEQ: case AIR_FCMP_UEQ: res = nir_feq(&c->b, x, y); break;
   case AIR_FCMP_ONE: case AIR_FCMP_UNE: res = nir_fneu(&c->b, x, y); break;
   case AIR_FCMP_OGT: case AIR_FCMP_UGT: res = nir_flt(&c->b, y, x); break;
   case AIR_FCMP_OGE: case AIR_FCMP_UGE: res = nir_fge(&c->b, x, y); break;
   case AIR_FCMP_OLT: case AIR_FCMP_ULT: res = nir_flt(&c->b, x, y); break;
   case AIR_FCMP_OLE: case AIR_FCMP_ULE: res = nir_fge(&c->b, y, x); break;
   case AIR_FCMP_ORD: res = nir_iand(&c->b, nir_feq(&c->b, x, x), nir_feq(&c->b, y, y)); break;
   case AIR_FCMP_UNO: res = nir_ior(&c->b, nir_fneu(&c->b, x, x), nir_fneu(&c->b, y, y)); break;
   case AIR_FCMP_FALSE: res = nir_imm_false(&c->b); break;
   case AIR_FCMP_TRUE: res = nir_imm_true(&c->b); break;
   case AIR_ICMP_EQ: res = nir_ieq(&c->b, x, y); break;
   case AIR_ICMP_NE: res = nir_ine(&c->b, x, y); break;
   case AIR_ICMP_UGT: res = nir_ult(&c->b, y, x); break;
   case AIR_ICMP_UGE: res = nir_uge(&c->b, x, y); break;
   case AIR_ICMP_ULT: res = nir_ult(&c->b, x, y); break;
   case AIR_ICMP_ULE: res = nir_uge(&c->b, y, x); break;
   case AIR_ICMP_SGT: res = nir_ilt(&c->b, y, x); break;
   case AIR_ICMP_SGE: res = nir_ige(&c->b, x, y); break;
   case AIR_ICMP_SLT: res = nir_ilt(&c->b, x, y); break;
   case AIR_ICMP_SLE: res = nir_ige(&c->b, y, x); break;
   default:
      return air_error(c, "instruction: comparison %u", in->sub);
   }
   air_set(c, &in->base, air_val_ssa(c, res));
   return true;
}

/* an aggregate value's field, by the index path of extract/insertvalue */
static struct air_val **
agg_slot(struct air_ctx *c, struct air_val *agg, const uint32_t *idx, unsigned n)
{
   /* extractvalue and insertvalue always carry at least one index. Without this
    * the loop below never runs and the function returns the address of its own
    * parameter -- which gcc points out (-Wreturn-local-addr) and which would be
    * a write through a dead stack slot if anything ever took the path.
    */
   if (!n) {
      air_error(c, "instruction: an aggregate access with no index");
      return NULL;
   }
   struct air_val **slot = &agg;
   for (unsigned i = 0; i < n; i++) {
      if (!*slot || (*slot)->kind != AIR_VAL_AGG || idx[i] >= (*slot)->num_elems) {
         air_error(c, "instruction: an aggregate index out of its value");
         return NULL;
      }
      slot = &(*slot)->elems[idx[i]];
   }
   return slot;
}

static struct air_val *
clone_val(struct air_ctx *c, const struct air_val *v)
{
   if (!v)
      return NULL;
   struct air_val *n = rzalloc(c->mem_ctx, struct air_val);
   *n = *v;
   if (v->kind == AIR_VAL_AGG) {
      n->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(v->num_elems, 1));
      for (unsigned i = 0; i < v->num_elems; i++)
         n->elems[i] = clone_val(c, v->elems[i]);
   }
   return n;
}

/* What a value turned out to be, for a message that has to be read without the
 * module in hand. A refusal that only says "not constant" sends the reader to
 * llvm-dis to find out which of six things it was, and the three kernels that
 * stopped on a shuffle mask cost exactly that detour.
 */
static const char *
air_value_kind_name(const struct air_value *v)
{
   if (!v)
      return "nothing";
   const struct air_constant *c = air_value_as_constant(v);
   if (!c) {
      switch (v->kind) {
      case AIR_VALUE_GLOBAL:      return "a global";
      case AIR_VALUE_FUNCTION:    return "a function";
      case AIR_VALUE_ARGUMENT:    return "a function argument";
      case AIR_VALUE_INSTRUCTION: return "an instruction";
      case AIR_VALUE_METADATA:    return "metadata";
      default:                    return "a value of an unknown kind";
      }
   }
   switch (c->ckind) {
   case AIR_CONST_INT:       return "an integer";
   case AIR_CONST_FLOAT:     return "a float";
   case AIR_CONST_NULL:      return "zeroinitializer";
   case AIR_CONST_UNDEF:     return "undef";
   case AIR_CONST_POISON:    return "poison";
   case AIR_CONST_AGGREGATE: return "an aggregate of constants";
   case AIR_CONST_DATA:      return "a vector of plain integers";
   case AIR_CONST_EXPR:      return "a constant expression";
   }
   return "a constant of an unknown kind";
}

static bool
translate_instruction(struct air_ctx *c, const struct air_inst *in)
{
   switch (in->op) {
   case AIR_OP_BINOP:
      return translate_binop(c, in);
   case AIR_OP_UNOP: {
      nir_def *x = air_get_ssa(c, in->operands[0]);
      if (!x)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_fneg(&c->b, x)));
      return true;
   }
   case AIR_OP_CAST:
      return translate_cast(c, in);
   case AIR_OP_CMP:
      return translate_cmp(c, in);
   case AIR_OP_GEP:
      return translate_gep(c, in);
   case AIR_OP_SELECT: {
      /* A select between two POINTERS, which the GL-on-Metal uber shaders use
       * to fall back on a module constant array when the pipeline bound no
       * buffer. It is not a number, so it cannot be a bcsel here; the choice
       * is kept and made where the pointer is loaded through.
       */
      struct air_val *pa = air_get(c, in->operands[1]);
      struct air_val *pb = air_get(c, in->operands[2]);
      if (!pa || !pb)
         return false;
      if (val_is_pointerish(pa) || val_is_pointerish(pb)) {
         if (!val_is_pointerish(pa) || !val_is_pointerish(pb))
            return air_error(c, "instruction: a select between a pointer and "
                                "something that is not one");
         /* A choice between two members of an argument buffer cannot be kept
          * the way a choice between two buffers can: AIR_VAL_PTR_SELECT loads
          * both sides and selects the values, and a texture or a sampler is
          * not a value to load. Two names for ONE member is no choice at all
          * and is answered; anything else is refused rather than drawn over
          * whichever of the two the code happened to take.
          */
         if ((pa->kind == AIR_VAL_PTR && pa->ptr.class == AIR_PTR_ARGBUF) ||
             (pb->kind == AIR_VAL_PTR && pb->ptr.class == AIR_PTR_ARGBUF)) {
            if (pa->kind == AIR_VAL_PTR && pb->kind == AIR_VAL_PTR &&
                same_ptr(&pa->ptr, &pb->ptr)) {
               air_set(c, &in->base, pa);
               return true;
            }
            return air_error(c, "instruction: a select between two different places in "
                                "an argument buffer");
         }
         nir_def *pc = air_get_ssa(c, in->operands[0]);
         if (!pc)
            return false;
         if (pc->num_components != 1)
            return air_error(c, "instruction: a vector select between pointers");
         air_set(c, &in->base, air_val_ptr_select(c, pc, pa, pb));
         return true;
      }
      nir_def *cond = air_get_ssa(c, in->operands[0]);
      nir_def *x = air_get_ssa(c, in->operands[1]);
      nir_def *y = air_get_ssa(c, in->operands[2]);
      if (!cond || !x || !y)
         return false;
      if (cond->num_components > 1 && cond->num_components != x->num_components)
         return air_error(c, "instruction: a select whose condition does not match its values");
      air_set(c, &in->base, air_val_ssa(c, nir_bcsel(&c->b, cond, x, y)));
      return true;
   }
   case AIR_OP_EXTRACTELT: {
      nir_def *v = air_get_ssa(c, in->operands[0]);
      nir_def *i = air_get_ssa(c, in->operands[1]);
      if (!v || !i)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_vector_extract(&c->b, v, air_u32(&c->b, i))));
      return true;
   }
   case AIR_OP_INSERTELT: {
      nir_def *v = air_get_ssa(c, in->operands[0]);
      nir_def *x = air_get_ssa(c, in->operands[1]);
      nir_def *i = air_get_ssa(c, in->operands[2]);
      if (!v || !x || !i)
         return false;
      if (!nir_src_is_const(nir_src_for_ssa(i))) {
         /* a run-time index (iOS 26's QuartzCore uber shader): NIR's bcsel per component */
         air_set(c, &in->base, air_val_ssa(c, nir_vector_insert(&c->b, v, x, air_u32(&c->b, i))));
         return true;
      }
      air_set(c, &in->base,
              air_val_ssa(c, nir_vector_insert_imm(&c->b, v, x,
                                                   nir_src_as_uint(nir_src_for_ssa(i)))));
      return true;
   }
   case AIR_OP_SHUFFLEVEC: {
      nir_def *x = air_get_ssa(c, in->operands[0]);
      nir_def *y = air_get_ssa(c, in->operands[1]);
      struct air_val *mv = air_get(c, in->operands[2]);
      if (!x || !y || !mv)
         return false;
      unsigned n = air_nir_components(in->base.type);
      nir_def *comps[NIR_MAX_VEC_COMPONENTS];
      if (n > NIR_MAX_VEC_COMPONENTS)
         return air_error(c, "instruction: a shuffle of %u components", n);
      /* the mask is a constant vector, or zeroinitializer, or undef */
      const struct air_constant *mk = air_value_as_constant(in->operands[2]);
      for (unsigned i = 0; i < n; i++) {
         int64_t sel = 0;
         bool undef = false;
         if (mk && mk->ckind == AIR_CONST_DATA && i < mk->num_elems) {
            sel = (int32_t)mk->data[i];
         } else if (mk && mk->ckind == AIR_CONST_AGGREGATE && i < mk->num_elems) {
            const struct air_constant *e = air_value_as_constant(mk->elems[i]);
            /* The mask is an aggregate rather than plain data whenever a lane
             * is poison, so <i32 0, i32 poison, ...> -- what a Metal
             * half2-to-half4 widening compiles to -- mixes LLVM's two
             * spellings of a constant integer. air_const_uint reads both; this
             * code used to test ckind == AIR_CONST_INT by hand instead, which
             * is the whole bug.
             */
            uint64_t lane = 0;
            bool have = air_const_uint(mk->elems[i], &lane);
            if (!e || (!have && e->ckind != AIR_CONST_UNDEF &&
                       e->ckind != AIR_CONST_POISON))
               return air_error(c, "instruction: element %u of a shuffle mask is %s",
                                i, air_value_kind_name(mk->elems[i]));
            undef = !have;
            sel = have ? (int32_t)lane : 0;
         } else if (mk && (mk->ckind == AIR_CONST_NULL)) {
            sel = 0;
         } else if (mk && (mk->ckind == AIR_CONST_UNDEF || mk->ckind == AIR_CONST_POISON)) {
            undef = true;
         } else {
            return air_error(c, "instruction: a shuffle mask that is %s, with %u of the "
                                "%u elements it needs",
                             air_value_kind_name(in->operands[2]),
                             mk ? mk->num_elems : 0, n);
         }
         if (undef) {
            comps[i] = nir_undef(&c->b, 1, x->bit_size);
         } else if (sel < x->num_components) {
            comps[i] = nir_channel(&c->b, x, sel);
         } else if (sel - x->num_components < y->num_components) {
            comps[i] = nir_channel(&c->b, y, sel - x->num_components);
         } else {
            return air_error(c, "instruction: a shuffle index past its inputs");
         }
      }
      air_set(c, &in->base, air_val_ssa(c, nir_vec(&c->b, comps, n)));
      return true;
   }
   case AIR_OP_EXTRACTVAL: {
      struct air_val *agg = air_get(c, in->operands[0]);
      if (!agg)
         return false;
      struct air_val **slot = agg_slot(c, agg, in->indices, in->num_indices);
      if (!slot)
         return false;
      if (!*slot) {
         /* a field of an undef aggregate -- itself an aggregate of undefs when it is a struct or
          * an array: Flocon's fragment shaders take the padding of FloconInputs ([8 x i8], which
          * nothing ever wrote) and hand it on, field by field, to getReflexion */
         const struct air_type *t = in->base.type;
         if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
            unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : (unsigned)t->count;
            air_set(c, &in->base, air_val_agg(c, n));
            return true;
         }
         air_set(c, &in->base,
                 air_val_ssa(c, nir_undef(&c->b, air_nir_components(in->base.type),
                                          air_nir_bit_size(c, in->base.type))));
         return true;
      }
      air_set(c, &in->base, *slot);
      return true;
   }
   case AIR_OP_INSERTVAL: {
      struct air_val *agg = air_get(c, in->operands[0]);
      struct air_val *val = air_get(c, in->operands[1]);
      if (!agg || !val)
         return false;
      struct air_val *copy = clone_val(c, agg);
      struct air_val **slot = agg_slot(c, copy, in->indices, in->num_indices);
      if (!slot)
         return false;
      *slot = val;
      air_set(c, &in->base, copy);
      return true;
   }
   case AIR_OP_FREEZE:
      air_set(c, &in->base, air_get(c, in->operands[0]));
      return c->error[0] == 0;
   case AIR_OP_ALLOCA: {
      uint64_t count;
      if (!air_const_uint(in->operands[0], &count))
         return air_error(c, "instruction: an alloca of a run-time size");
      /* A local holding a POINTER. Scratch memory cannot hold one -- a pointer
       * here is a resolution and not bytes -- so it becomes a translation-time
       * slot instead, when and only when that is sound: see AIR_VAL_SLOT. The
       * two GL-on-Metal fragment shaders store their last two buffer arguments
       * into such a local and never read them back, which LLVM left behind and
       * which used to stop the module at "a pointer where a number belongs".
       */
      uint64_t size = air_type_alloc_size(c->m, in->aux_type) * count;
      unsigned align = MAX2(in->align, air_type_abi_align(c->m, in->aux_type));
      c->scratch_size = ALIGN_POT(c->scratch_size, MAX2(align, 1));
      struct air_ptr p = {
         .class = AIR_PTR_SCRATCH,
         .offset = nir_imm_int(&c->b, c->scratch_size),
         .pointee = in->aux_type,
      };
      c->scratch_size += size;
      air_set(c, &in->base, air_val_ptr(c, p));
      return true;
   }
   case AIR_OP_LOAD: {
      struct air_val *p = air_get(c, in->operands[0]);
      if (!p)
         return false;
      if (p->kind == AIR_VAL_PTR_SELECT) {
         nir_def *res = load_through_choice(c, p, in->base.type, in->align);
         if (!res)
            return false;
         air_set(c, &in->base, air_val_ssa(c, res));
         return true;
      }
      if (p->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a load from something that is not a pointer");
      /* a pointer read back from the local it was stored into (the store above) */
      if (in->base.type->kind == AIR_TYPE_POINTER && p->ptr.class == AIR_PTR_SCRATCH) {
         uint64_t slot;
         struct air_val *had = c->ptr_slots && ptr_slot_of(&p->ptr, &slot)
                                  ? _mesa_hash_table_u64_search(c->ptr_slots, slot) : NULL;
         if (!had)
            return air_error(c, "instruction: a load of a pointer from a local no pointer "
                                "was stored into");
         air_set(c, &in->base, had);
         return true;
      }
      /* An argument buffer resolves before it loads: the byte offset names a
       * member and the member's descriptor names a binding, so this load
       * becomes a pointer and not an instruction.
       */
      if (p->ptr.class == AIR_PTR_ARGBUF) {
         struct air_val *res = argbuf_load(c, p->ptr, in->base.type);
         if (!res)
            return false;
         air_set(c, &in->base, res);
         return true;
      }
      nir_def *res = air_load_ptr(c, p->ptr, in->base.type, in->align);
      if (!res)
         return false;
      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }
   case AIR_OP_STORE: {
      struct air_val *p = air_get(c, in->operands[1]);
      if (!p)
         return false;
      /* A POINTER stored into memory. There is nothing to write: a pointer in
       * this frontend is a resolution -- a binding and a byte offset decided
       * while translating -- and not an address, so it has no bytes. Naming
       * that here rather than letting air_get_ssa say "a pointer where a
       * number belongs" is the difference between a wall with a reason and a
       * wall with a symptom; the reason is in docs/air.md section 3.
       */
      struct air_val *sv = air_get(c, in->operands[0]);
      if (!sv)
         return false;
      if (val_is_pointerish(sv)) {
         /* A pointer kept in a local -- MPS's kernels put their output texture in a
          * `TextureWriter` struct on the stack and read it back (D8F18H, the Gaussian blur
          * SpringBoard runs: MPS asserts and SpringBoard aborts when the pipeline is refused).
          * There are no bytes to write, but where it went is known while translating: a load of
          * a pointer from the same constant place is this value. Another pointer stored to the
          * same place is still refused -- which one a load sees would depend on the path. */
         uint64_t slot;
         if (p->kind == AIR_VAL_PTR && p->ptr.class == AIR_PTR_SCRATCH && ptr_slot_of(&p->ptr, &slot)) {
            if (!c->ptr_slots)
               c->ptr_slots = _mesa_hash_table_u64_create(c->mem_ctx);
            struct air_val *had = _mesa_hash_table_u64_search(c->ptr_slots, slot);
            if (had && had != sv)
               return air_error(c, "instruction: two different pointers stored into one "
                                   "local, which a load would have to choose between at run time");
            _mesa_hash_table_u64_insert(c->ptr_slots, slot, sv);
            return true;
         }
         return air_error(c, "instruction: a store of a pointer into memory, which this "
                             "frontend has no address to write");
      }
      if (p->kind == AIR_VAL_PTR_SELECT)
         return air_error(c, "instruction: a store through a chosen pointer, which would "
                             "have to write both of them");
      nir_def *v = air_get_ssa(c, in->operands[0]);
      if (!v)
         return false;
      if (p->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a store to something that is not a pointer");
      return air_store_ptr(c, p->ptr, v, in->align);
   }
   case AIR_OP_CALL: {
      const struct air_value *callee = in->operands[0];
      if (callee->kind == AIR_VALUE_INSTRUCTION || callee->kind == AIR_VALUE_ARGUMENT) {
         /* through a function pointer, here or handed down as an argument (RenderBox's
          * custom_effect_color): only the null one a visible function table the pipeline
          * leaves out hands back (air_intrinsics.c), whose call is dead code under the
          * same function constant -- undef is its whole meaning */
         struct air_val *fp = air_get(c, callee);
         if (!fp)
            return false;
         if (fp->kind != AIR_VAL_PTR || fp->ptr.class != AIR_PTR_NONE)
            return air_error(c, "instruction: a call through a pointer");
         const struct air_type *rt = in->base.type;
         if (rt->kind != AIR_TYPE_VOID) {
            if (!ret_member_is_number(rt))
               return air_error(c, "instruction: a call through the null function "
                                   "returning an aggregate");
            air_set(c, &in->base, air_val_ssa(c, nir_undef(&c->b, air_nir_components(rt),
                                                            air_nir_bit_size(c, rt))));
         }
         return true;
      }
      if (callee->kind != AIR_VALUE_FUNCTION || !callee->name)
         return air_error(c, "instruction: a call through a pointer");
      return air_translate_call(c, in, callee->name);
   }
   case AIR_OP_RET:
   case AIR_OP_BR:
   case AIR_OP_SWITCH:
   case AIR_OP_UNREACHABLE:
   case AIR_OP_PHI:
      return true; /* handled by the CFG walk */
   default:
      return air_error(c, "instruction: opcode %u", in->op);
   }
}

/* -- a two-way split inside one AIR block ----------------------------------
 *
 * This translator builds an UNSTRUCTURED impl (nir_goto, nir_goto_if; nir_lower_goto_ifs makes it
 * structured afterwards), so nir_push_if -- which needs a structured one, and asserts or, in a
 * release build, never terminates -- cannot make the two-sided calls and texture operations a
 * pointer CHOSEN at run time needs (emit_module_call, texture_op_chosen). These make the same if
 * out of blocks: a then and an else, each going to a merge where translation carries on; what a side
 * produces reaches the merge in a register (air_split_keep), as the phis of this translator do.
 */
static nir_block *
air_new_block(struct air_ctx *c)
{
   nir_block *n = nir_block_create(c->b.impl);
   exec_list_push_tail(&c->b.impl->body, &n->cf_node.node);
   n->cf_node.parent = &c->b.impl->cf_node;
   return n;
}

void
air_split_begin(struct air_ctx *c, nir_def *cond, struct air_split *sp)
{
   sp->then_block = air_new_block(c);
   sp->else_block = air_new_block(c);
   sp->merge_block = air_new_block(c);
   if (cond->bit_size != 1)
      cond = nir_ine_imm(&c->b, cond, 0);
   nir_goto_if(&c->b, sp->then_block, cond, sp->else_block);
   c->b.cursor = nir_after_block(sp->then_block);
}

void
air_split_else(struct air_ctx *c, struct air_split *sp)
{
   nir_goto(&c->b, sp->merge_block);
   c->b.cursor = nir_after_block(sp->else_block);
}

void
air_split_end(struct air_ctx *c, struct air_split *sp)
{
   nir_goto(&c->b, sp->merge_block);
   c->b.cursor = nir_after_block(sp->merge_block);
}

/* What one side made, kept for the merge: a register per SSA leaf, declared at the impl's start on
 * the first side (*regs NULL) and stored to on both. Returns the value made of the registers,
 * to be loaded at the merge with air_split_load. */
struct air_val *
air_split_keep(struct air_ctx *c, struct air_val *v, struct air_val *regs)
{
   if (!v)
      return regs;
   if (v->kind == AIR_VAL_SSA) {
      if (!regs) {
         nir_builder at = nir_builder_at(nir_before_impl(c->b.impl));
         regs = air_val_ssa(c, nir_decl_reg(&at, v->def->num_components, v->def->bit_size, 0));
      }
      nir_store_reg(&c->b, v->def, regs->def);
      return regs;
   }
   if (v->kind == AIR_VAL_AGG) {
      if (!regs) {
         regs = air_val_agg(c, v->num_elems);
      }
      for (unsigned i = 0; i < v->num_elems && i < regs->num_elems; i++)
         regs->elems[i] = air_split_keep(c, v->elems[i], regs->elems[i]);
      return regs;
   }
   return regs;
}

struct air_val *
air_split_load(struct air_ctx *c, struct air_val *regs)
{
   if (!regs)
      return NULL;
   if (regs->kind == AIR_VAL_SSA)
      return air_val_ssa(c, nir_load_reg(&c->b, regs->def));
   struct air_val *v = air_val_agg(c, regs->num_elems);
   for (unsigned i = 0; i < regs->num_elems; i++)
      v->elems[i] = air_split_load(c, regs->elems[i]);
   return v;
}

/* -- the CFG ------------------------------------------------------------ */

static nir_block *
nir_block_of(struct air_ctx *c, const struct air_block *b)
{
   struct hash_entry *e = _mesa_hash_table_search(c->blocks, b);
   if (e)
      return e->data;
   nir_block *n = nir_block_create(c->b.impl);
   exec_list_push_tail(&c->b.impl->body, &n->cf_node.node);
   n->cf_node.parent = &c->b.impl->cf_node;
   _mesa_hash_table_insert(c->blocks, b, n);
   return n;
}

static bool
emit_return(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands == 0)
      return true;
   struct air_val *v = air_get(c, in->operands[0]);
   if (!v)
      return false;

   /* Inside a callee the return value goes through the out-parameter the
    * caller passed, because a NIR call has no destination. The entry point
    * has no such parameter and its return is its render targets, below.
    */
   if (c->num_ret_fields) {
      /* A callee returning a struct: one out-parameter per member, because a
       * NIR parameter carries a number and a struct here is a tree of values.
       * The members with no parameter are the ones nothing can read (see
       * ret_fields in air_private.h), and a member the callee left out of its
       * insertvalue chain is undef -- Metal's own vertex-output structs leave
       * their padding field exactly that way, so this is the normal case and
       * not a defect.
       */
      if (v->kind != AIR_VAL_AGG)
         return air_error(c, "instruction: a returned struct that was never built, "
                             "from a function called in the module");
      unsigned at = 0;
      return store_ret_leaves(c, c->ret_type, v, &at);
   }
   if (c->ret_deref) {
      nir_def *d = air_get_ssa(c, in->operands[0]);
      if (!d)
         return air_error(c, "instruction: a return of a value this cannot put in a "
                             "register, from a function called in the module");
      nir_store_deref(&c->b, c->ret_deref, d, nir_component_mask(d->num_components));
      return true;
   }

   const struct air_type *rt = in->operands[0]->type;
   if (rt->kind == AIR_TYPE_STRUCT) {
      if (v->kind != AIR_VAL_AGG || v->num_elems != rt->num_members)
         return air_error(c, "instruction: a returned struct that was never built");
      for (unsigned i = 0; i < v->num_elems; i++) {
         if (c->outputs[i].gated_off)
            continue;
         if (!c->outputs[i].var)
            return air_error(c, "interface: field %u of the result has no output", i);
         if (!v->elems[i] || v->elems[i]->kind != AIR_VAL_SSA)
            continue; /* a field left undef is a target nothing writes */
         nir_store_var(&c->b, c->outputs[i].var, air_output_bits(c, c->outputs[i].var,
                                                                 v->elems[i]->def),
                       BITFIELD_MASK(v->elems[i]->def->num_components));
      }
      return true;
   }
   if (v->kind != AIR_VAL_SSA)
      return air_error(c, "instruction: a returned value that is not a number");
   if (c->num_outputs && c->outputs[0].gated_off)
      return true;
   if (!c->num_outputs || !c->outputs[0].var)
      return air_error(c, "interface: a returned value with no output");
   nir_store_var(&c->b, c->outputs[0].var, air_output_bits(c, c->outputs[0].var, v->def),
                 BITFIELD_MASK(v->def->num_components));
   return true;
}

/* The blocks in reverse post-order, so that every value is translated before
 * it is used: LLVM lays blocks out in its own order, and a block whose only
 * predecessor comes after it in that order is ordinary (a loop's exit, say).
 * Reverse post-order respects dominance, which is exactly the guarantee the
 * translation needs. Blocks the entry cannot reach are left out, and the phi
 * pass below ignores edges from them.
 */
static unsigned
reverse_post_order(struct air_ctx *c, const struct air_function *f, unsigned *order)
{
   unsigned n = f->num_blocks;
   uint8_t *state = rzalloc_array(c->mem_ctx, uint8_t, MAX2(n, 1)); /* 0 new, 1 open, 2 done */
   unsigned *stack = rzalloc_array(c->mem_ctx, unsigned, MAX2(n, 1));
   unsigned *next_succ = rzalloc_array(c->mem_ctx, unsigned, MAX2(n, 1));
   unsigned sp = 0, num_post = 0;

   stack[sp++] = 0;
   state[0] = 1;
   while (sp) {
      unsigned bi = stack[sp - 1];
      const struct air_block *blk = &f->blocks[bi];
      const struct air_inst *term = blk->insts[blk->num_insts - 1];
      if (next_succ[bi] < term->num_targets) {
         unsigned to = term->targets[next_succ[bi]++]->index;
         if (state[to] == 0) {
            state[to] = 1;
            next_succ[to] = 0;
            stack[sp++] = to;
         }
         continue;
      }
      state[bi] = 2;
      order[num_post++] = bi; /* post-order */
      sp--;
   }
   /* reverse it in place */
   for (unsigned i = 0; i < num_post / 2; i++) {
      unsigned t = order[i];
      order[i] = order[num_post - 1 - i];
      order[num_post - 1 - i] = t;
   }
   return num_post;
}

static bool translate_body(struct air_ctx *c);
static void materialise_globals(struct air_ctx *c);

/* Does this value hold a pointer anywhere inside it? */
static bool
val_holds_ptr(const struct air_val *v)
{
   if (!v)
      return false;
   if (val_is_pointerish(v))
      return true;
   if (v->kind != AIR_VAL_AGG)
      return false;
   for (unsigned i = 0; i < v->num_elems; i++)
      if (val_holds_ptr(v->elems[i]))
         return true;
   return false;
}

/* An aggregate passed BY VALUE ([2 x %Image], [3 x i8] in iOS 26's QuartzCore): NIR's parameters
 * are scalars and vectors, so it travels as its leaves, depth first, one parameter each. */
static bool
type_is_value_aggregate(const struct air_type *t)
{
   return t && (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY);
}

static void
agg_param_leaves(struct air_ctx *c, const struct air_type *t, nir_parameter *params, unsigned *idx)
{
   if (type_is_value_aggregate(t)) {
      unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : (unsigned)t->count;
      for (unsigned i = 0; i < n; i++)
         agg_param_leaves(c, t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem, params, idx);
      return;
   }
   if (params)
      params[*idx] = (nir_parameter){ .num_components = (uint8_t)air_nir_components(t),
                                      .bit_size = (uint8_t)air_nir_bit_size(c, t) };
   (*idx)++;
}

static struct air_val *
agg_from_params(struct air_ctx *c, const struct air_type *t, unsigned *idx)
{
   if (type_is_value_aggregate(t)) {
      unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : (unsigned)t->count;
      struct air_val *v = air_val_agg(c, n);
      for (unsigned i = 0; i < n; i++)
         v->elems[i] = agg_from_params(c, t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem, idx);
      return v;
   }
   return air_val_ssa(c, nir_load_param(&c->b, (*idx)++));
}

static void
agg_to_call(struct air_ctx *c, const struct air_type *t, const struct air_val *v,
            nir_call_instr *call, unsigned *idx)
{
   if (type_is_value_aggregate(t)) {
      unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : (unsigned)t->count;
      for (unsigned i = 0; i < n; i++)
         agg_to_call(c, t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem,
                     v && v->kind == AIR_VAL_AGG && i < v->num_elems ? v->elems[i] : NULL,
                     call, idx);
      return;
   }
   nir_def *d = v && v->kind == AIR_VAL_SSA ? v->def : NULL;
   unsigned comps = air_nir_components(t), bits = air_nir_bit_size(c, t);
   if (!d || d->num_components != comps || d->bit_size != bits)
      d = nir_undef(&c->b, comps, bits);       /* a field nothing put there (poison, undef) */
   call->params[(*idx)++] = nir_src_for_ssa(d);
}

/* A function of the module, translated as a nir_function so that
 * nir_inline_functions can fold it into its callers afterwards.
 *
 * Translating it in place at the call site was the other option and is worse:
 * the callees here are not straight-line code -- FilmGrainLuma has 17 blocks
 * and 24 branches -- so inlining by hand would mean reproducing a CFG inside
 * another one, with its own block map and phis. NIR already has that pass.
 *
 * The return goes through an out-parameter at index 0, with is_return set,
 * because a NIR call has no destination; this is the convention spirv_to_nir
 * uses (vtn_cfg.c) and the one nir_inline_functions expects.
 *
 * The per-function tables are swapped rather than shared: values, blocks and
 * phis are all keyed on this function's own objects, and leaving the caller's
 * entries in place would let a callee resolve a value it cannot see.
 */
/* What a specialised callee was specialised on, as a string, or false where an argument has no
 * description that two call sites could share (spec_fns). QuartzCore's uber shader calls
 * apply_color_opcode 12 times per specialisation with the same textures and buffers: each was a
 * translation of all its 82 blocks, and the opt loops then spent most of a pipeline's 70 ms on
 * this Mac (0.5 s on the phone, run glass20) folding the copies back. */
static bool
spec_key_val(const struct air_ctx *c, char **key, const struct air_val *v, bool nested)
{
   if (!v)
      return false;
   switch (v->kind) {
   case AIR_VAL_PTR:
      ralloc_asprintf_append(key, "P%u.%u.%u.%p.%p.", v->ptr.class, v->ptr.binding, v->ptr.is_image,
                             (void *)v->ptr.var, (const void *)v->ptr.pointee);
      if (!v->ptr.offset)
         ralloc_strcat(key, "0;");
      else if (nested ? nir_src_is_const(nir_src_for_ssa(v->ptr.offset)) : !ptr_offset_is_param(c, v))
         ralloc_asprintf_append(key, "%llu;",
                                (unsigned long long)nir_src_as_uint(nir_src_for_ssa(v->ptr.offset)));
      else if (!nested)
         ralloc_strcat(key, "o;");     /* the offset goes in as a parameter */
      else
         return false;                 /* a pointer inside a struct carries the caller's def */
      return true;
   case AIR_VAL_AGG:
      ralloc_asprintf_append(key, "A%u(", v->num_elems);
      for (unsigned i = 0; i < v->num_elems; i++) {
         if (!v->elems[i])
            ralloc_strcat(key, "-,");
         else if (v->elems[i]->kind == AIR_VAL_SSA) {
            /* a number beside the pointer is the caller's own def, which the callee reads as
             * it is: shared only when it is a constant, and then with its value in the key */
            nir_def *d = v->elems[i]->def;
            if (!d || !nir_src_is_const(nir_src_for_ssa(d)))
               return false;
            nir_load_const_instr *lc = nir_instr_as_load_const(nir_def_instr(d));
            ralloc_asprintf_append(key, "k%u.%u", lc->def.num_components, lc->def.bit_size);
            for (unsigned k = 0; k < lc->def.num_components; k++)
               ralloc_asprintf_append(key, ".%llx", (unsigned long long)lc->value[k].u64);
            ralloc_strcat(key, ",");
         } else if (!spec_key_val(c, key, v->elems[i], true))
            return false;
      }
      ralloc_strcat(key, ")");
      return true;
   default:
      return false;
   }
}

static char *
spec_key(const struct air_ctx *c, const struct air_function *f, struct air_val **argv,
         const bool *is_param)
{
   char *key = ralloc_asprintf(c->mem_ctx, "%p:", (const void *)f);
   for (unsigned i = 0; i < f->num_args; i++) {
      struct air_val *v = argv ? argv[i] : NULL;
      if (is_param[i]) {
         ralloc_strcat(&key, "_;");
      } else if (v && v->kind == AIR_VAL_SSA && v->def && nir_src_is_const(nir_src_for_ssa(v->def))) {
         nir_load_const_instr *lc = nir_instr_as_load_const(nir_def_instr(v->def));
         ralloc_asprintf_append(&key, "K%u.%u", lc->def.num_components, lc->def.bit_size);
         for (unsigned k = 0; k < lc->def.num_components; k++)
            ralloc_asprintf_append(&key, ".%llx", (unsigned long long)lc->value[k].u64);
         ralloc_strcat(&key, ";");
      } else if (!spec_key_val(c, &key, v, false)) {
         return NULL;
      }
   }
   return key;
}

/*
 * A call of NAME.MTL_VISIBLE_FN_REF -- a function the module declares and Metal links in from
 * another library at pipeline time (MTLLinkedFunctions; RenderBox's custom effects call SwiftUI
 * shaders such as MercuryPoster's distanceGradient_v1 this way, as custom_fn). The linked
 * function's module is read, and the context is switched to it for as long as the function is
 * translated: its globals, their plan, its static fields and its function constants are its own
 * module's, while the shader, its resources, the builder and the caller's resolution of each
 * argument go on being the caller's. translate_callee does the rest as for a callee of this
 * module, and nir_inline_functions folds it in like any other.
 */
struct air_link_saved {
   const struct air_module *m;
   struct air_global_plan *global_plan;
   struct hash_table *globals;
   struct hash_table *static_stores;
   void *static_mem;
   const struct air_to_nir_options *options;
};

static const struct air_function *
link_visible(struct air_ctx *c, const struct air_function *f, struct air_link_saved *saved)
{
   static const char suffix[] = ".MTL_VISIBLE_FN_REF";
   const char *name = f->base.name ? f->base.name : "";
   size_t len = strlen(name);
   if (len <= sizeof(suffix) - 1 || strcmp(name + len - (sizeof(suffix) - 1), suffix))
      return NULL;
   len -= sizeof(suffix) - 1;

   const struct air_to_nir_options *o = c->options;
   const struct air_linked_function *l = NULL;
   for (unsigned i = 0; o && i < o->num_linked && !l; i++)
      if (o->linked[i].name && strlen(o->linked[i].name) == len &&
          !strncmp(o->linked[i].name, name, len))
         l = &o->linked[i];
   if (!l && o && o->num_linked == 1)
      l = &o->linked[0];
   if (!l) {
      air_error(c, "intrinsic: a call of the linked function %.*s, and the pipeline links %u "
                   "function%s, none of that name", (int)len, name, o ? o->num_linked : 0,
                o && o->num_linked == 1 ? "" : "s");
      return NULL;
   }

   const char *read_error = NULL;
   const struct air_module *m2 = air_module_read(c->mem_ctx, l->bitcode, l->size, &read_error);
   if (!m2) {
      air_error(c, "reader: the linked function %.*s: %s", (int)len, name, read_error);
      return NULL;
   }
   /* by its name in the module -- and failing that the module's one function with a body: Metal
    * calls a function made with MTLFunctionDescriptor.specializedName by that name (SwiftUI's
    * effects are all "custom_fn"), which is in no module */
   const struct air_function *g = NULL, *only = NULL;
   unsigned bodies = 0;
   for (unsigned i = 0; i < m2->num_functions; i++) {
      const struct air_function *h = m2->functions[i];
      if (h->is_declaration || !h->num_blocks || (h->section && !strcmp(h->section, "air.static_init")))
         continue;
      if (!g && l->entry && h->base.name && !strcmp(h->base.name, l->entry))
         g = h;
      only = h;
      bodies++;
   }
   if (!g && bodies == 1)
      g = only;
   if (!g) {
      air_error(c, "intrinsic: the module linked as %.*s has no function %s", (int)len, name,
                l->entry ? l->entry : "with a body");
      return NULL;
   }
   for (unsigned i = 0; i < m2->num_functions; i++) {
      const struct air_function *h = m2->functions[i];
      if (!h->is_declaration && h->section && !strcmp(h->section, "air.static_init")) {
         air_error(c, "intrinsic: the function linked as %.*s has static initializers, which "
                      "a linked module does not run here", (int)len, name);
         return NULL;
      }
   }
   if (g->num_args != f->num_args) {
      air_error(c, "intrinsic: %s takes %u arguments and the call of %.*s passes %u",
                g->base.name ? g->base.name : "the linked function", g->num_args, (int)len,
                name, f->num_args);
      return NULL;
   }

   *saved = (struct air_link_saved){
      .m = c->m, .global_plan = c->global_plan, .globals = c->globals,
      .static_stores = c->static_stores, .static_mem = c->static_mem, .options = c->options,
   };
   struct air_to_nir_options *opts = ralloc(c->mem_ctx, struct air_to_nir_options);
   *opts = *o;
   opts->constants = l->constants;
   opts->num_constants = l->num_constants;
   c->options = opts;
   c->m = m2;
   c->globals = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->static_stores = NULL;
   c->static_mem = NULL;
   plan_globals(c);
   return g;
}

static void
unlink_visible(struct air_ctx *c, const struct air_link_saved *saved)
{
   c->m = saved->m;
   c->global_plan = saved->global_plan;
   c->globals = saved->globals;
   c->static_stores = saved->static_stores;
   c->static_mem = saved->static_mem;
   c->options = saved->options;
}

static nir_function *
translate_callee(struct air_ctx *c, const struct air_function *f,
                 struct air_val **argv, bool *is_param, bool *off_param)
{
   /* A texture, sampler or buffer argument is not a value in this frontend: it
    * is resolved at translation time into a binding, and tex_handle wants an
    * AIR_VAL_PTR rather than an SSA def. Such an argument is bound from the
    * caller's own resolution and takes no NIR parameter, which specialises the
    * callee to its call site -- so it is not cached. Each of the four modules
    * that needs this has exactly one call site, counted beforehand.
    *
    * A pointer carrying a byte offset is refused instead: that offset is an
    * nir_def of the CALLER's impl, and using it here would be the same
    * cross-function def/use that the globals used to cause.
    */
   bool specialised = false;
   for (unsigned i = 0; i < f->num_args; i++) {
      /* A struct holding a pointer counts as one too: CoreImage passes a
       * texture and a sampler as metal::texture2d and metal::sampler, which
       * are one-field structs wrapping the address-space-1 and -2 pointers.
       * Passing such a struct as a NIR value would hand the callee something
       * it then takes extractvalue of, and extractvalue of a value that is not
       * an aggregate here is exactly the error this used to produce.
       */
      bool ptr = argv && argv[i] && (argv[i]->kind == AIR_VAL_PTR ||
                                     val_holds_ptr(argv[i]));
      /* A pointer at a byte offset the caller computed: the offset is a def of the CALLER's
       * impl, so it goes in as a parameter of its own (after the value parameters, in argument
       * order; callee_offset_args below and emit_module_call agree on it), and the callee's
       * resolution of the argument is the caller's with that parameter for its offset. iOS
       * 26's QuartzCore needs it: its glass, path-stroke and uber fragment shaders hand
       * helpers a `getelementptr` into a constant buffer. */
      /* The same reason, for the two pointers that are values: a chosen
       * pointer's condition is a def of the CALLER's impl, and a slot's
       * soundness rests on every use being in one block of one function.
       */
      if (argv && argv[i] && argv[i]->kind == AIR_VAL_PTR_SELECT) {
         air_error(c, "intrinsic: a call passing a pointer chosen at run time as "
                      "argument %u", i);
         return NULL;
      }
      /* An argument the caller knows -- a function constant, or what one decided: the callee
       * gets it as the same immediate and is translated for this call alone, so that its own
       * branches on it are decided while it is translated (translate_body) and what they do not
       * take is never translated. QuartzCore's uber shader hands its helpers the configuration
       * this way (apply_color_opcode's opcode, ycbcr_to_rgb's matrix choice, ...): before this a
       * specialisation translated all 1 782 blocks of the module and pruned afterwards. */
      bool known = !ptr && argv && argv[i] && argv[i]->kind == AIR_VAL_SSA && argv[i]->def &&
                   !type_is_value_aggregate(f->args[i].type) &&
                   nir_src_is_const(nir_src_for_ssa(argv[i]->def));
      is_param[i] = !ptr && !known;
      specialised |= ptr || known;
   }

   /* which pointer arguments' offsets go in as parameters, decided once here: the call site
    * (emit_module_call) must make the same choice, and the callee's body may change what
    * ptr_offset_is_param would answer by storing a pointer into a local */
   for (unsigned i = 0; i < f->num_args; i++)
      off_param[i] = !is_param[i] && ptr_offset_is_param(c, argv[i]);

   struct hash_entry *e = specialised ? NULL : _mesa_hash_table_search(c->fns, f);
   if (e)
      return e->data;
   char *skey = specialised ? spec_key(c, f, argv, is_param) : NULL;
   if (specialised && getenv("AIR_SPEC_DEBUG"))
      fprintf(stderr, "air spec %s: %s\n", f->base.name ? f->base.name : "?", skey ? skey : "(none)");
   if (skey) {
      if (!c->spec_fns)
         c->spec_fns = _mesa_hash_table_create(c->mem_ctx, _mesa_hash_string, _mesa_key_string_equal);
      struct hash_entry *se = _mesa_hash_table_search(c->spec_fns, skey);
      if (se)
         return se->data;
   }

   if (f->is_declaration) {
      struct air_link_saved saved;
      const struct air_function *g = link_visible(c, f, &saved);
      if (g) {
         nir_function *fn = translate_callee(c, g, argv, is_param, off_param);
         unlink_visible(c, &saved);
         return fn;
      }
      if (c->error[0])
         return NULL;
   }
   if (f->is_declaration || !f->num_blocks) {
      air_error(c, "intrinsic: a call of %s, which the module only declares",
                f->base.name ? f->base.name : "a function");
      return NULL;
   }

   const struct air_type *rt = f->fn_type->elem;   /* FUNCTION: the return type */
   bool has_ret = rt && rt->kind != AIR_TYPE_VOID;
   /* A struct return takes one out-parameter per member rather than one for the
    * whole value. Four of QuartzCore's vertex shaders are nothing but a wrapper
    * around such a function -- `tail call fastcc`, then extractvalue of each
    * field -- and before this they stopped at the call with "an aggregate where
    * a number belongs", which was emit_return inside the callee asking for the
    * returned struct as a register.
    */
   bool ret_struct = has_ret && rt->kind == AIR_TYPE_STRUCT;
   unsigned nret = 0;
   const struct air_type **leaf_types = NULL;
   if (ret_struct) {
      nret = ret_leaves(rt, NULL, 0);
      leaf_types = rzalloc_array(c->mem_ctx, const struct air_type *, MAX2(nret, 1));
      ret_leaves(rt, leaf_types, 0);
   } else if (has_ret) {
      nret = 1;
   }
   unsigned nparams = 2 * f->num_args + nret;     /* (at most: a value or an offset each, see below) */
   for (unsigned i = 0; i < f->num_args; i++) {
      if (type_is_value_aggregate(f->args[i].type)) {
         unsigned leaves = 0;
         agg_param_leaves(c, f->args[i].type, NULL, &leaves);
         nparams += leaves;
      }
   }
   nir_function *fn = nir_function_create(c->nir, f->base.name ? f->base.name : "callee");
   fn->num_params = nparams;
   fn->params = rzalloc_array(c->nir, nir_parameter, MAX2(nparams, 1));
   fn->should_inline = true;

   unsigned idx = 0;
   const struct glsl_type *ret_glsl = NULL;
   if (ret_struct) {
      for (unsigned i = 0; i < nret; i++) {
         fn->params[idx++] = (nir_parameter){
            .num_components = 1, .bit_size = 32,
            .is_return = true,
            .mode = nir_var_function_temp,
            .type = param_glsl_type(c, leaf_types[i]),
         };
      }
   } else if (has_ret) {
      ret_glsl = param_glsl_type(c, rt);
      fn->params[idx++] = (nir_parameter){
         .num_components = 1, .bit_size = 32,
         .is_return = true,
         .mode = nir_var_function_temp,
         .type = ret_glsl,
      };
   }
   for (unsigned i = 0; i < f->num_args; i++) {
      if (!is_param[i])
         continue;
      const struct air_type *at = f->args[i].type;
      if (type_is_value_aggregate(at)) {
         agg_param_leaves(c, at, fn->params, &idx);
         continue;
      }
      fn->params[idx++] = (nir_parameter){
         .num_components = (uint8_t)air_nir_components(at),
         .bit_size = (uint8_t)air_nir_bit_size(c, at),
      };
   }
   for (unsigned i = 0; i < f->num_args; i++) {
      if (off_param[i])
         fn->params[idx++] = (nir_parameter){ .num_components = 1, .bit_size = 32 };
   }
   fn->num_params = idx;   /* the pointers took no parameter, beyond their offsets */

   /* cached before the body is translated, so that a callee which calls itself
    * meets its own declaration rather than being translated for ever */
   if (!specialised)
      _mesa_hash_table_insert(c->fns, f, fn);
   else
      _mesa_hash_table_insert(c->fns, fn, fn);
   if (skey)
      _mesa_hash_table_insert(c->spec_fns, skey, fn);

   nir_builder saved_b = c->b;
   const struct air_function *saved_fn = c->fn;
   struct hash_table *sv = c->values, *sb = c->blocks,
                     *sp = c->phi_cursors, *sr = c->phi_regs;
   nir_deref_instr *saved_ret = c->ret_deref;
   nir_deref_instr **saved_ret_fields = c->ret_fields;
   unsigned saved_num_ret_fields = c->num_ret_fields;
   const struct air_type *saved_ret_type = c->ret_type;
   /* a constant of the caller is a def of the caller's impl, so the place
    * constants are built belongs to the function being translated, like the
    * values table above */
   nir_cursor saved_consts = c->const_cursor;

   c->fn = f;
   c->values = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->blocks = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->phi_cursors = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->phi_regs = _mesa_pointer_hash_table_create(c->mem_ctx);
   c->b = nir_builder_at(nir_before_impl(nir_function_impl_create(fn)));
   c->b.constant_fold_alu = true;     /* function constants fold, and what they decide is known */
   /* as for the entry point: this translator emits an unstructured CFG with
    * nir_goto, and nir_goto_if asserts on a structured impl. A new impl is
    * structured by default, so a callee crashed on its first branch -- which
    * is what the corpus's reader count caught, 456 falling to 454. */
   c->b.impl->structured = false;
   c->const_cursor = nir_before_impl(c->b.impl);
   c->ret_deref = NULL;
   c->ret_fields = NULL;
   c->num_ret_fields = 0;

   /* this function's own globals, built in its own builder */
   materialise_globals(c);

   bool ok = true;
   /* The caller passes each deref's own def and the callee casts it back, which
    * is the convention nir_functions.c documents: load_param is replaced by the
    * call's source at inline time and nir_opt_deref removes the cast afterwards.
    */
   if (ret_struct) {
      c->ret_fields = rzalloc_array(c->mem_ctx, nir_deref_instr *, MAX2(nret, 1));
      c->num_ret_fields = nret;
      c->ret_type = rt;
      for (unsigned i = 0; i < nret; i++) {
         c->ret_fields[i] =
            nir_build_deref_cast(&c->b, nir_load_param(&c->b, i),
                                 nir_var_function_temp,
                                 param_glsl_type(c, leaf_types[i]), 0);
      }
   } else if (has_ret) {
      c->ret_deref = nir_build_deref_cast(&c->b, nir_load_param(&c->b, 0),
                                          nir_var_function_temp, ret_glsl, 0);
   }
   unsigned pidx = nret;
   unsigned oidx = nret;
   for (unsigned i = 0; i < f->num_args; i++) {
      if (!is_param[i])
         continue;
      if (type_is_value_aggregate(f->args[i].type))
         agg_param_leaves(c, f->args[i].type, NULL, &oidx);
      else
         oidx++;
   }
   for (unsigned i = 0; i < f->num_args && ok; i++) {
      if (!is_param[i]) {
         if (off_param[i]) {
            struct air_ptr p = argv[i]->ptr;
            p.offset = nir_load_param(&c->b, oidx++);
            air_set(c, &f->args[i], air_val_ptr(c, p));
            continue;
         }
         if (argv[i] && argv[i]->kind == AIR_VAL_PTR && argv[i]->ptr.offset) {
            /* at a constant offset: the same constant, built again in the callee's own impl, so
             * that a pointer it reads back from a local it was passed is known there (ptr_slots) */
            struct air_ptr p = argv[i]->ptr;
            p.offset = nir_imm_intN_t(&c->b, nir_src_as_uint(nir_src_for_ssa(argv[i]->ptr.offset)),
                                      argv[i]->ptr.offset->bit_size);
            air_set(c, &f->args[i], air_val_ptr(c, p));
            continue;
         }
         if (argv[i] && argv[i]->kind == AIR_VAL_SSA && argv[i]->def) {
            /* a known argument: the caller's immediate, built again in the callee's own impl */
            nir_load_const_instr *lc = nir_instr_as_load_const(nir_def_instr(argv[i]->def));
            nir_def *k = nir_build_imm(&c->b, lc->def.num_components, lc->def.bit_size, lc->value);
            air_set(c, &f->args[i], air_val_ssa(c, k));
            continue;
         }
         air_set(c, &f->args[i], argv[i]);   /* the caller's own resolution */
         continue;
      }
      if (type_is_value_aggregate(f->args[i].type)) {
         air_set(c, &f->args[i], agg_from_params(c, f->args[i].type, &pidx));
         continue;
      }
      air_set(c, &f->args[i], air_val_ssa(c, nir_load_param(&c->b, pidx++)));
   }
   if (ok)
      ok = translate_body(c);

   c->b = saved_b;
   c->fn = saved_fn;
   c->values = sv;
   c->blocks = sb;
   c->phi_cursors = sp;
   c->phi_regs = sr;
   c->const_cursor = saved_consts;
   c->ret_deref = saved_ret;
   c->ret_fields = saved_ret_fields;
   c->num_ret_fields = saved_num_ret_fields;
   c->ret_type = saved_ret_type;
   return ok ? fn : NULL;
}

/* A call of a function this module defines. The callee is translated as a
 * nir_function (above) and the call becomes a nir_call, which
 * nir_inline_functions folds in after the whole shader is translated.
 *
 * This used to be refused outright, with a comment saying no system shader
 * needed it because Apple's own inliner had already flattened every one. That
 * was not true: six entry points of the corpus call a function in their module
 * and five of them are compute kernels, which is how the claim went unnoticed
 * -- nothing dispatched a kernel until there was something to dispatch it with.
 */
/* The call itself, with the arguments resolved in argv, its return written to the locals the
 * caller made (tmp, or fields for a struct). A pointer argument CHOSEN at run time -- iOS 26's
 * QuartzCore passes `select i1 %fc, @__air_sampler_state, %sampler` to its sampling helpers --
 * cannot specialise the callee to one binding, so the call is made twice, under an if on the
 * choice's condition, one specialised to each side; both write the same locals. The condition is a
 * function constant's, so once nir_inline_functions and constant folding have run, one side is
 * gone again.
 */
static bool
emit_module_call(struct air_ctx *c, const struct air_inst *in, const struct air_function *f,
                 const char *name, struct air_val **argv, nir_variable *tmp, nir_variable **fields)
{
   for (unsigned i = 0; i < f->num_args; i++) {
      if (!argv[i] || argv[i]->kind != AIR_VAL_PTR_SELECT)
         continue;
      struct air_val **a = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(f->num_args, 1));
      struct air_val **b = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(f->num_args, 1));
      memcpy(a, argv, f->num_args * sizeof(*a));
      memcpy(b, argv, f->num_args * sizeof(*b));
      a[i] = argv[i]->elems[0];
      b[i] = argv[i]->elems[1];
      struct air_split sp;
      air_split_begin(c, argv[i]->def, &sp);
      if (!emit_module_call(c, in, f, name, a, tmp, fields))
         return false;
      air_split_else(c, &sp);
      if (!emit_module_call(c, in, f, name, b, tmp, fields))
         return false;
      air_split_end(c, &sp);
      return true;
   }

   bool *is_param = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_args, 1));
   bool *off_param = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_args, 1));
   nir_function *fn = translate_callee(c, f, argv, is_param, off_param);
   if (!fn)
      return false;   /* translate_callee has said why */

   const struct air_type *rt = f->fn_type->elem;   /* FUNCTION: the return type */
   bool has_ret = rt && rt->kind != AIR_TYPE_VOID;
   bool ret_struct = has_ret && rt->kind == AIR_TYPE_STRUCT;

   nir_call_instr *call = nir_call_instr_create(c->nir, fn);
   unsigned idx = 0;
   if (ret_struct) {
      unsigned nleaves = ret_leaves(rt, NULL, 0);
      for (unsigned i = 0; i < nleaves; i++)
         call->params[idx++] =
            nir_src_for_ssa(&nir_build_deref_var(&c->b, fields[i])->def);
   } else if (has_ret) {
      call->params[idx++] = nir_src_for_ssa(&nir_build_deref_var(&c->b, tmp)->def);
   }
   /* operand 0 is the callee; the arguments follow it */
   for (unsigned i = 0; i < f->num_args; i++) {
      if (!is_param[i])
         continue;
      if (type_is_value_aggregate(f->args[i].type)) {
         agg_to_call(c, f->args[i].type, argv[i], call, &idx);
         continue;
      }
      nir_def *a = air_get_ssa(c, in->operands[i + 1]);
      if (!a)
         return air_error(c, "intrinsic: argument %u of %s is not a value this can pass",
                          i, name);
      call->params[idx++] = nir_src_for_ssa(a);
   }
   /* and the byte offsets of the pointers passed at one, in argument order (translate_callee) */
   for (unsigned i = 0; i < f->num_args; i++) {
      if (off_param[i])
         call->params[idx++] = nir_src_for_ssa(nir_u2u32(&c->b, argv[i]->ptr.offset));
   }
   nir_builder_instr_insert(&c->b, &call->instr);
   return true;
}

bool
air_call_in_module(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   const struct air_function *f = NULL;
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const char *fname = c->m->functions[i]->base.name;
      if (fname && !strcmp(fname, name)) {
         f = c->m->functions[i];
         break;
      }
   }
   if (!f)
      return air_error(c, "intrinsic: a call of %s, which is not a function of this "
                          "module", name);

   if (in->num_operands - 1 != f->num_args)
      return air_error(c, "intrinsic: a call of %s with %u arguments, wanting %u", name,
                       in->num_operands - 1, f->num_args);

   /* resolved in the CALLER's value map, which is the live one here */
   struct air_val **argv = rzalloc_array(c->mem_ctx, struct air_val *,
                                         MAX2(f->num_args, 1));
   for (unsigned i = 0; i < f->num_args; i++) {
      argv[i] = air_get(c, in->operands[i + 1]);
      if (!argv[i])
         return false;
   }

   const struct air_type *rt = f->fn_type->elem;   /* FUNCTION: the return type */
   bool has_ret = rt && rt->kind != AIR_TYPE_VOID;
   bool ret_struct = has_ret && rt->kind == AIR_TYPE_STRUCT;

   nir_variable *tmp = NULL;
   /* One local per member for a struct return, in the member order
    * translate_callee made its parameters from; the ones with no parameter stay
    * NULL and the aggregate's element for them stays NULL too, which is the
    * same thing an extractvalue of an undef field already reads.
    */
   unsigned nleaves = ret_struct ? ret_leaves(rt, NULL, 0) : 0;
   nir_variable **fields = ret_struct ?
      rzalloc_array(c->mem_ctx, nir_variable *, MAX2(nleaves, 1)) : NULL;
   if (ret_struct) {
      const struct air_type **leaf_types =
         rzalloc_array(c->mem_ctx, const struct air_type *, MAX2(nleaves, 1));
      ret_leaves(rt, leaf_types, 0);
      for (unsigned i = 0; i < nleaves; i++)
         fields[i] = nir_local_variable_create(c->b.impl, param_glsl_type(c, leaf_types[i]),
                                               "callret");
   } else if (has_ret) {
      tmp = nir_local_variable_create(c->b.impl, param_glsl_type(c, rt), "callret");
   }

   if (!emit_module_call(c, in, f, name, argv, tmp, fields))
      return false;

   if (ret_struct) {
      unsigned at = 0;
      air_set(c, &in->base, load_ret_leaves(c, rt, fields, &at));
   } else if (has_ret) {
      air_set(c, &in->base, air_val_ssa(c, nir_load_var(&c->b, tmp)));
   }
   return true;
}

/* The air.visible_input node for argument i, or NULL. The nodes carry a
 * parameter number as their first operand rather than being in order, so they
 * are searched by it.
 */
static const struct air_md *
arg_md(const struct air_md *inputs, unsigned i)
{
   for (unsigned k = 0; k < (inputs ? inputs->num_ops : 0); k++) {
      const struct air_md *n = inputs->ops[k];
      uint64_t which;
      if (n && md_int(n, 0, &which) && which == i)
         return n;
   }
   return NULL;
}

/* An air.visible library function, inside an entry point we generate for it.
 *
 * A library function is not a shader. Metal links one into a caller at
 * pipeline time through the ABI its own metadata describes -- air.stitching_info
 * names the four functions that do it (load_argument, copy_from_buffer,
 * store_return_value, destroy) and gives each argument's size and alignment --
 * and none of that is implemented here: it belongs to the runtime, not to the
 * compiler. So 135 modules of the corpus, a third of it, had nothing that
 * could be dispatched and were counted only as refused.
 *
 * This wraps one instead: argument i is read from device buffer 0 at byte
 * 16*i, the function is called, and the return value is written to device
 * buffer 1 at offset 0, by a single thread. The 16-byte slot is Apple's own
 * size and alignment for a float4 (the air.vector_type node beside each
 * stitching argument says 16 and 16), and a fixed slot makes the layout
 * derivable from the signature alone -- which is what lets a harness on this
 * machine fill the same bytes and compare the result.
 *
 * What that measures is the body: its arithmetic, its control flow, its calls.
 * What it does not touch is the stitching ABI, so a wrapped function is
 * reported in a row of its own and never counted among the corpus's entry
 * points.
 */
static bool
wrap_library_function(struct air_ctx *c, const struct air_md *out_md,
                      const struct air_md *in_md)
{
   const struct air_function *f = c->fn;

   /* The result buffer carries the function's own air.arg_type_name ("float4"
    * and the like, from air.visible_output): a harness that fills the argument
    * buffer has to know how to read the bytes that come back, and Apple's name
    * for the type is a better source for that than our reading of the LLVM
    * type -- the two can then be checked against each other.
    */
   add_resource(c, AIR_RESOURCE_DEVICE_BUFFER, 0, 16 * MAX2(f->num_args, 1),
                true, false, ~0u, NULL);
   add_resource(c, AIR_RESOURCE_DEVICE_BUFFER, 1, 16, false, true, ~0u, out_md);

   struct air_val **argv = rzalloc_array(c->mem_ctx, struct air_val *,
                                         MAX2(f->num_args, 1));
   bool *is_param = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_args, 1));

   /* Eleven of the corpus's library functions are CoreImage's sampling helpers,
    * and they take what CoreImage hands a stitched function: a texture, a
    * sampler and a transform. Apple passes the first two as one-field structs
    * named metal::texture2d and metal::sampler -- the field being exactly the
    * address-space-1 or -2 pointer an entry point's texture or sampler
    * argument is -- and the transform as a bare constant-space pointer. Those
    * three shapes are bound rather than loaded: a texture is not a value in
    * this frontend (section 7.4), so the wrapper resolves each to a binding
    * and the harness binds its own texture, sampler and constant buffer there.
    *
    * Keyed on the struct's name, which is Apple's and is in these modules, and
    * not on "a struct with one pointer in it": the second would quietly accept
    * anything shaped alike.
    */
   unsigned next_tex = 0, next_samp = 0, next_ubo = 0;
   for (unsigned i = 0; i < f->num_args; i++) {
      const struct air_type *t = f->args[i].type;
      const char *sname = t->kind == AIR_TYPE_STRUCT ? t->name : NULL;
      bool is_tex = sname && !strncmp(sname, "struct.metal::texture", 21);
      bool is_samp = sname && !strcmp(sname, "struct.metal::sampler");

      if (is_tex || is_samp) {
         if (t->num_members != 1)
            return air_error(c, "library: %s has %u members, not the one pointer this "
                                "expects", sname, t->num_members);
         struct air_ptr p = {
            .class = is_tex ? AIR_PTR_TEXTURE : AIR_PTR_SAMPLER,
            .binding = is_tex ? next_tex : next_samp,
         };
         add_resource(c, is_tex ? AIR_RESOURCE_TEXTURE : AIR_RESOURCE_SAMPLER,
                      p.binding, 0, true, false, i, arg_md(in_md, i));
         if (is_tex)
            next_tex++;
         else
            next_samp++;
         /* the value is the struct, so the pointer goes inside an aggregate of
          * one: the body reaches it with extractvalue
          */
         struct air_val *agg = air_val_agg(c, 1);
         agg->elems[0] = air_val_ptr(c, p);
         argv[i] = agg;
         is_param[i] = false;
         continue;
      }

      if (t->kind == AIR_TYPE_POINTER && t->addrspace == 2) {
         /* the transform, as a constant buffer the harness fills */
         struct air_ptr p = { .class = AIR_PTR_UBO, .binding = next_ubo,
                              .pointee = t->elem };
         /* with the argument's own metadata, so the resource carries Apple's
          * name for it -- "float4x4" for the transform these functions take,
          * which is what lets the harness put something meaningful there
          * rather than the byte pattern
          */
         add_resource(c, AIR_RESOURCE_CONSTANT_BUFFER, next_ubo++, 256, true, false,
                      i, arg_md(in_md, i));
         argv[i] = air_val_ptr(c, p);
         is_param[i] = false;
         continue;
      }

      const struct air_type *e = t->kind == AIR_TYPE_VECTOR ? t->elem : t;
      bool numeric = e && (e->kind == AIR_TYPE_FLOAT || e->kind == AIR_TYPE_HALF ||
                           e->kind == AIR_TYPE_INT);
      if (!numeric || (t->kind != AIR_TYPE_VECTOR && t->kind != AIR_TYPE_FLOAT &&
                       t->kind != AIR_TYPE_HALF && t->kind != AIR_TYPE_INT))
         return air_error(c, "library: argument %u is none of a scalar, a vector, a "
                             "metal::texture, a metal::sampler or a constant pointer, "
                             "which is all this wrapper can supply", i);
      if (air_nir_components(t) * air_nir_bit_size(c, t) > 16 * 8)
         return air_error(c, "library: argument %u does not fit the wrapper's 16-byte "
                             "slot", i);

      struct air_ptr p = { .class = AIR_PTR_SSBO, .binding = 0, .pointee = t };
      nir_def *v = air_load_ptr(c, air_ptr_add(c, p, nir_imm_int(&c->b, 16 * i)), t, 16);
      if (!v)
         return false;
      argv[i] = air_val_ssa(c, v);
      is_param[i] = true;
   }

   const struct air_type *rt = f->fn_type->elem;
   const struct air_type *re = rt && rt->kind == AIR_TYPE_VECTOR ? rt->elem : rt;
   if (!rt || rt->kind == AIR_TYPE_VOID)
      return air_error(c, "library: the function returns nothing, so a wrapper would "
                          "have nothing to write");
   if (!re || (re->kind != AIR_TYPE_FLOAT && re->kind != AIR_TYPE_HALF &&
               re->kind != AIR_TYPE_INT))
      return air_error(c, "library: the return value is neither a scalar nor a vector");

   bool *off_param = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_args, 1));
   nir_function *fn = translate_callee(c, f, argv, is_param, off_param);
   if (!fn)
      return false;   /* translate_callee has said why */

   unsigned bits = air_nir_bit_size(c, rt);
   unsigned comps = air_nir_components(rt);
   const struct glsl_type *gt =
      glsl_vector_type(bits == 16 ? GLSL_TYPE_FLOAT16 :
                       bits == 64 ? GLSL_TYPE_UINT64 : GLSL_TYPE_UINT, comps);
   nir_variable *tmp = nir_local_variable_create(c->b.impl, gt, "libret");

   nir_call_instr *call = nir_call_instr_create(c->nir, fn);
   unsigned idx = 0;
   call->params[idx++] = nir_src_for_ssa(&nir_build_deref_var(&c->b, tmp)->def);
   for (unsigned i = 0; i < f->num_args; i++) {
      /* only the arguments translate_callee made parameters: a texture, a
       * sampler or a constant pointer was bound into the callee instead, and
       * has no SSA def to pass. Passing one anyway inserts a call whose source
       * has no value, which NIR dereferences while adding uses -- a segfault
       * inside nir_instr_insert rather than a message.
       */
      if (!is_param[i])
         continue;
      nir_def *a = argv[i] ? argv[i]->def : NULL;
      if (!a)
         return air_error(c, "library: argument %u has no value to pass", i);
      call->params[idx++] = nir_src_for_ssa(a);
   }
   nir_builder_instr_insert(&c->b, &call->instr);

   struct air_ptr out = { .class = AIR_PTR_SSBO, .binding = 1, .pointee = rt };
   if (!air_store_ptr(c, out, nir_load_var(&c->b, tmp), 16))
      return false;

   /* the generated block ends the way a translated one does: this impl is
    * unstructured until nir_lower_goto_ifs runs over it
    */
   nir_goto(&c->b, c->b.impl->end_block);
   return true;
}

/* A phi of an AGGREGATE: one register per leaf of the type, in the order
 * agg_param_leaves walks it. A struct here is a tree of values and never a NIR
 * value, so the phi is carried member by member -- what LLVM's own SROA would
 * have made of it. The registers are what phi_regs holds for such a phi.
 */
struct air_agg_reg {
   unsigned num;
   nir_def **regs;
};

static void
agg_decl_regs(struct air_ctx *c, nir_builder *b, const struct air_type *t,
              struct air_agg_reg *r, unsigned *idx)
{
   if (type_is_value_aggregate(t)) {
      unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : (unsigned)t->count;
      for (unsigned i = 0; i < n; i++)
         agg_decl_regs(c, b, t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem, r, idx);
      return;
   }
   r->regs[(*idx)++] = nir_decl_reg(b, air_nir_components(t), air_nir_bit_size(c, t), 0);
}

static struct air_val *
agg_load_regs(struct air_ctx *c, const struct air_type *t, const struct air_agg_reg *r,
              unsigned *idx)
{
   if (type_is_value_aggregate(t)) {
      unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : (unsigned)t->count;
      struct air_val *v = air_val_agg(c, n);
      for (unsigned i = 0; i < n; i++)
         v->elems[i] = agg_load_regs(c, t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem,
                                     r, idx);
      return v;
   }
   return air_val_ssa(c, nir_load_reg(&c->b, r->regs[(*idx)++]));
}

static void
agg_store_regs(struct air_ctx *c, const struct air_type *t, const struct air_val *v,
               const struct air_agg_reg *r, unsigned *idx)
{
   if (type_is_value_aggregate(t)) {
      unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : (unsigned)t->count;
      for (unsigned i = 0; i < n; i++)
         agg_store_regs(c, t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem,
                        v && v->kind == AIR_VAL_AGG && i < v->num_elems ? v->elems[i] : NULL,
                        r, idx);
      return;
   }
   nir_def *d = v && v->kind == AIR_VAL_SSA ? v->def : NULL;
   unsigned comps = air_nir_components(t), bits = air_nir_bit_size(c, t);
   if (!d || d->num_components != comps || d->bit_size != bits)
      d = nir_undef(&c->b, comps, bits);       /* a field nothing put there (poison, undef) */
   nir_store_reg(&c->b, d, r->regs[(*idx)++]);
}

/* A phi of POINTERS, resolved rather than carried in a register.
 *
 * A pointer here is *(which resource, which byte offset)* decided while
 * translating, so it has no bits to put in a register and a phi of two of them
 * is not a value NIR can hold. It need not be: LLVM duplicates a
 * `getelementptr` onto two paths and phis the copies back, so both operands
 * name the same member -- 207 of the pointer phis in Resident Evil 4's library
 * are exactly that, and not one names two different members.
 *
 * Refused when they differ. This COULD go through the AIR_VAL_PTR_SELECT
 * machinery that already loads both sides and selects the values, but nothing
 * in reach needs it, a texture or a sampler is not a value that path can load,
 * and an untested path is worse than a refusal.
 *
 * `reachable` is why this is sound rather than optimistic: the blocks are
 * walked in reverse post order, so an operand coming in over a BACK edge has
 * not been translated yet. Skipping an operand merely because it is missing
 * would accept a loop-carried pointer by ignoring the edge that changes it, so
 * a missing operand from a reachable block is a refusal and only an edge the
 * entry cannot reach at all is passed over.
 */
/* A pointer phi whose operands all name the same place and differ only in the byte
 * offset -- LLVM's loop that walks a buffer, `phi ptr [ %base, %entry ], [ %next, %loop ]`
 * -- carried as that place with its offset in a register: the offset is a number, and a
 * number has bits. The place is checked again for every operand when the stores are
 * made (translate_body), which is when the one that comes in over the loop's back edge
 * exists at last.
 */
struct air_ptr_phi_reg {
   nir_def *reg;
   struct air_ptr place;
   /* a phi between DIFFERENT places (num_places > 1): which one in idx_reg, the offset in reg,
    * and the value a chain of AIR_VAL_PTR_SELECTs over places[] (resolve_pointer_phi) */
   unsigned num_places;
   struct air_ptr places[4];
   nir_def *idx_reg;
};

static bool
same_place(const struct air_ptr *a, const struct air_ptr *b)
{
   return a->class == b->class && a->binding == b->binding && a->is_image == b->is_image &&
          a->var == b->var;
}

/* The index of `p`'s place among r's, or -1. */
static int
phi_place_index(const struct air_ptr_phi_reg *r, const struct air_ptr *p)
{
   for (unsigned i = 0; i < r->num_places; i++)
      if (same_place(&r->places[i], p))
         return (int)i;
   return -1;
}

/* Every place a pointer -- or a tree of runtime choices of pointers -- can name, into r. */
static bool
phi_collect_places(struct air_ctx *c, struct air_ptr_phi_reg *r, struct air_val *v)
{
   if (v && v->kind == AIR_VAL_PTR_SELECT && v->num_elems == 2)
      return phi_collect_places(c, r, v->elems[0]) && phi_collect_places(c, r, v->elems[1]);
   if (!v || v->kind != AIR_VAL_PTR)
      return air_error(c, "instruction: a phi between pointers that name different "
                          "places, one of them not a resolved pointer");
   if (v->ptr.class == AIR_PTR_ARGBUF)
      return air_error(c, "instruction: a phi between argument buffers that name "
                          "different places");
   if (phi_place_index(r, &v->ptr) >= 0)
      return true;
   if (r->num_places == ARRAY_SIZE(r->places))
      return air_error(c, "instruction: a phi between pointers to more than four places");
   r->places[r->num_places] = v->ptr;
   r->places[r->num_places].offset = NULL;
   r->num_places++;
   return true;
}

/* Which of r's places `v` names, and its offset there, as numbers -- chosen at run time when `v`
 * is a tree of choices. False (and an error said) for a place r does not have. */
static bool
phi_place_of(struct air_ctx *c, const struct air_ptr_phi_reg *r, struct air_val *v,
             nir_def **idx, nir_def **off)
{
   if (v && v->kind == AIR_VAL_PTR_SELECT && v->num_elems == 2) {
      nir_def *ia, *oa, *ib, *ob;
      if (!phi_place_of(c, r, v->elems[0], &ia, &oa) || !phi_place_of(c, r, v->elems[1], &ib, &ob))
         return false;
      *idx = nir_bcsel(&c->b, v->def, ia, ib);
      *off = nir_bcsel(&c->b, v->def, oa, ob);
      return true;
   }
   int at = v && v->kind == AIR_VAL_PTR ? phi_place_index(r, &v->ptr) : -1;
   if (at < 0)
      return air_error(c, "instruction: a phi of pointers whose operand names a place the "
                          "others do not");
   *idx = nir_imm_int(&c->b, at);
   *off = v->ptr.offset ? air_u32(&c->b, v->ptr.offset) : nir_imm_int(&c->b, 0);
   return true;
}

/* A pointer phi whose operands name DIFFERENT places -- `phi ptr [ %materialA, %b1 ], [ %materialB,
 * %b2 ]`, which MercuryPoster's space shaders make of their per-disc material structs (six of
 * Space.metallib's variants refused before this). A place is not a value, but which one is a
 * number: every predecessor stores the index of its place and its offset in two registers (the
 * stores in translate_body), and the phi is a chain of AIR_VAL_PTR_SELECTs over the places, each
 * at the stored offset -- loads go through load_through_choice, GEPs through gep_into, as a
 * `select` of two pointers already does. Memory only: a texture or a sampler is not a value a
 * select can load. Up to four places. */
static bool
resolve_pointer_phi_places(struct air_ctx *c, const struct air_inst *in, const bool *reachable)
{
   struct air_ptr_phi_reg *r = rzalloc(c->mem_ctx, struct air_ptr_phi_reg);
   for (unsigned k = 0; k < in->num_operands; k++) {
      if (!in->targets[k] || !reachable[in->targets[k]->index])
         continue;
      struct hash_entry *e = _mesa_hash_table_search(c->values, in->operands[k]);
      if (!e)
         continue;       /* over a loop edge: checked against these places when it is stored */
      if (!phi_collect_places(c, r, e->data))
         return false;
   }
   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   r->reg = nir_decl_reg(&entry, 1, 32, 0);
   r->idx_reg = nir_decl_reg(&entry, 1, 32, 0);
   /* defined on every path, so the sides not taken read at an offset of zero and not undef */
   nir_store_reg(&entry, nir_imm_int(&entry, 0), r->reg);
   nir_store_reg(&entry, nir_imm_int(&entry, 0), r->idx_reg);
   r->place = r->places[0];
   _mesa_hash_table_insert(c->phi_regs, in, r);

   nir_def *off = nir_load_reg(&c->b, r->reg), *idx = nir_load_reg(&c->b, r->idx_reg);
   struct air_val *v = NULL;
   for (int i = (int)r->num_places - 1; i >= 0; i--) {
      struct air_val *pi = rzalloc(c->mem_ctx, struct air_val);
      pi->kind = AIR_VAL_PTR;
      pi->ptr = r->places[i];
      /* (a texture or a sampler has no offset: texture_op_chosen samples each side) */
      pi->ptr.offset = r->places[i].class == AIR_PTR_TEXTURE ||
                       r->places[i].class == AIR_PTR_SAMPLER ? NULL : off;
      v = v ? air_val_ptr_select(c, nir_ieq_imm(&c->b, idx, i), pi, v) : pi;
   }
   air_set(c, &in->base, v);
   return true;
}

static bool
resolve_pointer_phi(struct air_ctx *c, const struct air_inst *in, const bool *reachable)
{
   struct air_val *first = NULL, *null_texture = NULL;
   bool missing = false, differ = false, many = false;

   for (unsigned k = 0; k < in->num_operands; k++) {
      if (!in->targets[k] || !reachable[in->targets[k]->index])
         continue;
      struct hash_entry *e = _mesa_hash_table_search(c->values, in->operands[k]);
      if (!e) {
         const struct air_constant *kc = air_value_as_constant(in->operands[k]);
         if (!kc) {
            missing = true;     /* over a loop edge: not translated yet */
            continue;
         }
         struct air_val *kv = air_get(c, in->operands[k]);
         if (!kv)
            return false;
         e = _mesa_hash_table_search(c->values, in->operands[k]);
         if (!e)
            return air_error(c, "instruction: a phi of pointers with a constant operand "
                                "that is no pointer");
      }
      struct air_val *v = e->data;
      /* a pointer chosen at run time between two places that are the same one: its
       * offset is chosen, and stored as the others' are (the stores in translate_body) */
      if (v && v->kind == AIR_VAL_PTR_SELECT) {
         if (v->num_elems != 2 || !v->elems[0] || !v->elems[1] ||
             v->elems[0]->kind != AIR_VAL_PTR || v->elems[1]->kind != AIR_VAL_PTR ||
             !same_place(&v->elems[0]->ptr, &v->elems[1]->ptr)) {
            many = true;      /* a choice between places: resolve_pointer_phi_places */
            continue;
         }
         v = v->elems[0];
         differ = true;
      }
      if (!v || v->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a phi of pointers one of whose operands is "
                             "%s", v && v->kind == AIR_VAL_PTR_SELECT
                                      ? "itself a pointer chosen at run time between two places"
                                      : "not a resolved pointer");
      /* iOS 26's null texture beside a real one: the real one. Where the null one is
       * the side taken, the function constant that chose it also guards every use (see
       * air.get_null_texture in air_intrinsics.c), so what is bound there is never read. */
      if (v->ptr.class == AIR_PTR_TEXTURE && v->ptr.binding == AIR_NULL_BINDING) {
         null_texture = v;
         continue;
      }
      if (!first) {
         first = v;
      } else if (!same_place(&first->ptr, &v->ptr)) {
         many = true;
      } else if (!same_ptr(&first->ptr, &v->ptr)) {
         differ = true;
      }
   }
   if (many)
      return resolve_pointer_phi_places(c, in, reachable);
   if (!first)
      first = null_texture;
   if (!first)
      return air_error(c, "instruction: a phi of pointers with no reachable operand");
   if (!missing && !differ) {
      air_set(c, &in->base, first);
      return true;
   }
   if (first->ptr.class == AIR_PTR_TEXTURE || first->ptr.class == AIR_PTR_SAMPLER) {
      if (differ)
         return air_error(c, "instruction: a phi of textures or samplers at different offsets");
      /* a texture or sampler has no offset: whatever comes over the loop edge is
       * checked to be the same one when the stores are made */
   }
   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   struct air_ptr_phi_reg *r = rzalloc(c->mem_ctx, struct air_ptr_phi_reg);
   r->reg = nir_decl_reg(&entry, 1, 32, 0);
   r->place = first->ptr;
   _mesa_hash_table_insert(c->phi_regs, in, r);
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   *v = *first;
   v->ptr.offset = nir_load_reg(&c->b, r->reg);
   air_set(c, &in->base, v);
   return true;
}

static bool
translate_body(struct air_ctx *c)
{
   const struct air_function *f = c->fn;

   /* phis first: each becomes a register, loaded where the phi is and stored
    * at the end of every predecessor (docs/air.md: NIR's own
    * nir_lower_goto_ifs does the same to make an unstructured CFG safe) --
    * except a phi of POINTERS, which has no bits and is resolved where it
    * stands (resolve_pointer_phi). It gets no register, and the store loop at
    * the end skips a phi that has none.
    */
   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   for (unsigned bi = 0; bi < f->num_blocks; bi++) {
      const struct air_block *blk = &f->blocks[bi];
      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op != AIR_OP_PHI || in->base.type->kind == AIR_TYPE_POINTER)
            continue;
         if (type_is_value_aggregate(in->base.type)) {
            struct air_agg_reg *r = rzalloc(c->mem_ctx, struct air_agg_reg);
            agg_param_leaves(c, in->base.type, NULL, &r->num);
            r->regs = rzalloc_array(c->mem_ctx, nir_def *, MAX2(r->num, 1));
            unsigned idx = 0;
            agg_decl_regs(c, &entry, in->base.type, r, &idx);
            _mesa_hash_table_insert(c->phi_regs, in, r);
            continue;
         }
         nir_def *reg = nir_decl_reg(&entry, air_nir_components(in->base.type),
                                     air_nir_bit_size(c, in->base.type), 0);
         _mesa_hash_table_insert(c->phi_regs, in, reg);
      }
   }

   /* NIR's first block is the function's first; the rest are made as they
    * are reached, and nir_lower_goto_ifs puts them in order afterwards
    */
   _mesa_hash_table_insert(c->blocks, &f->blocks[0], nir_start_block(c->b.impl));

   unsigned *order = rzalloc_array(c->mem_ctx, unsigned, MAX2(f->num_blocks, 1));
   unsigned num_order = reverse_post_order(c, f, order);
   bool *reachable = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_blocks, 1));
   for (unsigned oi = 0; oi < num_order; oi++)
      reachable[order[oi]] = true;

   /* Which blocks a branch that is actually taken reaches. A branch on a function constant is
    * a branch on an immediate here (constant_fold_alu), so only its taken side is live, and a
    * block no live branch reaches is never translated: a specialised uber shader is mostly
    * such blocks -- QuartzCore's fixed_frag_lph_cph was translated whole and pruned only
    * afterwards, 400 ms and 300 MB a specialisation on this Mac, which on the phone kept
    * backboardd from drawing until its watchdog killed it (runs glass6, glass7). Blocks are
    * taken in reverse post-order, so every forward edge into a block is decided before it;
    * a loop's back edge only ever makes its header live, and the header is live already. */
   bool *live = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_blocks, 1));
   bool *translated = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_blocks, 1));
   live[0] = true;

   for (unsigned oi = 0; oi < num_order; oi++) {
      unsigned bi = order[oi];
      const struct air_block *blk = &f->blocks[bi];
      if (!live[bi]) {
         reachable[bi] = false;     /* (for the pointer phis of the blocks after it) */
         continue;
      }
      translated[bi] = true;
      nir_block *nb = nir_block_of(c, blk);
      c->b.cursor = nir_after_block(nb);

      /* the phis of this block are its first instructions */
      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op != AIR_OP_PHI)
            continue;
         if (in->base.type->kind == AIR_TYPE_POINTER) {
            if (!resolve_pointer_phi(c, in, reachable))
               return false;
            continue;
         }
         struct hash_entry *e = _mesa_hash_table_search(c->phi_regs, in);
         if (type_is_value_aggregate(in->base.type)) {
            unsigned idx = 0;
            air_set(c, &in->base, agg_load_regs(c, in->base.type, e->data, &idx));
            continue;
         }
         air_set(c, &in->base,
                 air_val_ssa(c, nir_load_reg(&c->b, e->data)));
      }

      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op == AIR_OP_PHI)
            continue;
         /* c->error as well as the return value: a refusal raised by something
          * that cannot report one upwards -- air_nir_bit_size, handed a width
          * NIR does not have -- must stop the walk here rather than let the
          * next instruction be built on top of it.
          */
         if (!translate_instruction(c, in) || c->error[0]) {
            /* say where, so the corpus report points at a line of the module */
            char where[64];
            snprintf(where, sizeof(where), " (block %u, instruction %u, opcode %u)",
                     bi, i, in->op);
            if (strlen(c->error) + strlen(where) < sizeof(c->error))
               strcat(c->error, where);
            return false;
         }
      }

      /* Where the phi stores of this block's successors go. A nop marks the
       * spot: without it, a block whose only instruction is its terminator
       * leaves the cursor at the end of the block, and a store put there
       * lands after the jump -- which NIR asserts on. vtn marks its blocks
       * the same way (vtn_cfg.c's end_nop).
       */
      nir_instr *end = &nir_nop(&c->b)->instr;
      nir_cursor at_end = nir_after_instr(end);
      _mesa_hash_table_insert(c->phi_cursors, blk,
                              ralloc_memdup(c->mem_ctx, &at_end, sizeof(at_end)));

      const struct air_inst *term = blk->insts[blk->num_insts - 1];
      switch (term->op) {
      case AIR_OP_RET:
         if (!emit_return(c, term))
            return false;
         nir_goto(&c->b, c->b.impl->end_block);
         break;
      case AIR_OP_BR:
         if (term->num_targets == 1) {
            live[term->targets[0] - f->blocks] = true;
            nir_goto(&c->b, nir_block_of(c, term->targets[0]));
         } else {
            nir_def *cond = air_get_ssa(c, term->operands[0]);
            if (!cond)
               return false;
            if (getenv("AIR_LIVE_DEBUG")) {
               static unsigned said;
               if (said++ < 4000) {
                  nir_instr *pi = nir_def_instr(cond);
                  fprintf(stderr, "air: br in %s block %u: cond %s (instr type %d%s%s)\n", f->base.name ? f->base.name : "?", bi,
                          nir_src_is_const(nir_src_for_ssa(cond)) ? "CONSTANT" : "not constant", pi->type,
                          pi->type == nir_instr_type_alu ? " op " : "",
                          pi->type == nir_instr_type_alu ? nir_op_infos[nir_instr_as_alu(pi)->op].name : "");
               }
            }
            if (term->targets[0] == term->targets[1]) {
               live[term->targets[0] - f->blocks] = true;
               nir_goto(&c->b, nir_block_of(c, term->targets[0]));
            } else if (nir_src_is_const(nir_src_for_ssa(cond))) {
               /* decided by function constants: the other side is dead */
               const struct air_block *taken = term->targets[nir_src_as_bool(nir_src_for_ssa(cond)) ? 0 : 1];
               live[taken - f->blocks] = true;
               nir_goto(&c->b, nir_block_of(c, taken));
            } else {
               live[term->targets[0] - f->blocks] = true;
               live[term->targets[1] - f->blocks] = true;
               nir_goto_if(&c->b, nir_block_of(c, term->targets[0]), cond,
                           nir_block_of(c, term->targets[1]));
            }
         }
         break;
      case AIR_OP_SWITCH: {
         /* NIR has no switch, so the cases become a chain of nir_goto_if:
          * each test gets its own block, and the last one falls to the
          * default. nir_lower_goto_ifs structures the result like every other
          * branch this translator emits.
          *
          * What makes the chain safe is where the phi stores already are: all
          * of this block's successors' stores sit at phi_cursors[blk], above
          * the chain, and each successor's phis have registers of their own --
          * so storing on an edge the chain does not take writes a register
          * that the block actually entered never reads. That is the same
          * property the conditional br above relies on, and the reason a
          * switch needs no per-edge bookkeeping.
          */
         nir_def *cond = air_get_ssa(c, term->operands[0]);
         if (!cond)
            return false;
         unsigned n = term->num_targets - 1;
         /* a switch on a constant: the one case it takes (or the default), and no chain */
         if (nir_src_is_const(nir_src_for_ssa(cond))) {
            const struct air_block *taken = term->targets[0];
            uint64_t want = nir_src_as_uint(nir_src_for_ssa(cond));
            for (unsigned k = 0; k < n; k++) {
               nir_def *val = air_get_ssa(c, term->operands[1 + k]);
               if (!val)
                  return false;
               if (nir_src_is_const(nir_src_for_ssa(val)) && nir_src_as_uint(nir_src_for_ssa(val)) == want) {
                  taken = term->targets[1 + k];
                  break;
               }
            }
            live[taken - f->blocks] = true;
            nir_goto(&c->b, nir_block_of(c, taken));
            break;
         }
         for (unsigned k = 0; k <= n; k++)
            live[term->targets[k] - f->blocks] = true;
         nir_block *deflt = nir_block_of(c, term->targets[0]);
         for (unsigned k = 0; k < n; k++) {
            nir_def *val = air_get_ssa(c, term->operands[1 + k]);
            if (!val)
               return false;
            nir_block *dest = nir_block_of(c, term->targets[1 + k]);
            bool last = k + 1 == n;
            nir_block *next = deflt;
            if (!last) {
               next = nir_block_create(c->b.impl);
               exec_list_push_tail(&c->b.impl->body, &next->cf_node.node);
               next->cf_node.parent = &c->b.impl->cf_node;
            }
            /* a last case that goes where the default goes is not a branch at
             * all, and nir_goto_if with one block on both sides is what the
             * conditional br above refuses for the same reason
             */
            if (dest == next)
               nir_goto(&c->b, dest);
            else
               nir_goto_if(&c->b, dest, nir_ieq(&c->b, cond, val), next);
            if (!last)
               c->b.cursor = nir_after_block(next);
         }
         if (n == 0)
            nir_goto(&c->b, deflt);
         break;
      }
      case AIR_OP_UNREACHABLE:
         nir_goto(&c->b, c->b.impl->end_block);
         break;
      default:
         return air_error(c, "instruction: block %u ends with opcode %u", bi, term->op);
      }
   }

   if (getenv("AIR_LIVE_DEBUG")) {
      unsigned n = 0;
      for (unsigned bi = 0; bi < f->num_blocks; bi++)
         n += translated[bi];
      fprintf(stderr, "air: %s: %u of %u blocks translated\n", f->base.name ? f->base.name : "?", n, f->num_blocks);
   }

   /* the phi stores, now that every value exists */
   for (unsigned bi = 0; bi < f->num_blocks; bi++) {
      const struct air_block *blk = &f->blocks[bi];
      if (!translated[bi])
         continue;         /* a block no taken branch reaches: its phis are never read */
      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->op != AIR_OP_PHI)
            continue;
         struct hash_entry *re = _mesa_hash_table_search(c->phi_regs, in);
         if (!re)
            continue; /* a phi in an unreachable block, or one of pointers */
         for (unsigned k = 0; k < in->num_operands; k++) {
            struct hash_entry *ce = _mesa_hash_table_search(c->phi_cursors, in->targets[k]);
            if (!ce)
               continue; /* an edge from a block the entry cannot reach */
            c->b.cursor = *(nir_cursor *)ce->data;
            if (in->base.type->kind == AIR_TYPE_POINTER) {
               struct air_ptr_phi_reg *pr = re->data;
               struct air_val *pv = air_get(c, in->operands[k]);
               if (!pv)
                  return false;
               if (pr->num_places > 1) {
                  /* which place, and where in it (resolve_pointer_phi_places) */
                  nir_def *pidx, *poff;
                  if (!phi_place_of(c, pr, pv, &pidx, &poff))
                     return false;
                  nir_store_reg(&c->b, pidx, pr->idx_reg);
                  nir_store_reg(&c->b, poff, pr->reg);
                  *(nir_cursor *)ce->data = c->b.cursor;
                  continue;
               }
               if (pv->kind == AIR_VAL_PTR_SELECT && pv->num_elems == 2 && pv->elems[0] &&
                   pv->elems[1] && pv->elems[0]->kind == AIR_VAL_PTR &&
                   pv->elems[1]->kind == AIR_VAL_PTR &&
                   same_place(&pv->elems[0]->ptr, &pr->place) &&
                   same_place(&pv->elems[1]->ptr, &pr->place)) {
                  nir_def *a = pv->elems[0]->ptr.offset ? nir_u2u32(&c->b, pv->elems[0]->ptr.offset)
                                                        : nir_imm_int(&c->b, 0);
                  nir_def *b = pv->elems[1]->ptr.offset ? nir_u2u32(&c->b, pv->elems[1]->ptr.offset)
                                                        : nir_imm_int(&c->b, 0);
                  nir_store_reg(&c->b, nir_bcsel(&c->b, pv->def, a, b), pr->reg);
                  *(nir_cursor *)ce->data = c->b.cursor;
                  continue;
               }
               if (pv->kind != AIR_VAL_PTR ||
                   (!same_place(&pv->ptr, &pr->place) &&
                    !(pv->ptr.class == AIR_PTR_TEXTURE &&
                      pv->ptr.binding == AIR_NULL_BINDING)))
                  return air_error(c, "instruction: a phi of pointers whose operand over a "
                                      "loop edge names another place than the others, so "
                                      "which resource it is depends on the iteration");
               nir_def *off = pv->ptr.offset ? nir_u2u32(&c->b, pv->ptr.offset)
                                             : nir_imm_int(&c->b, 0);
               nir_store_reg(&c->b, off, pr->reg);
               *(nir_cursor *)ce->data = c->b.cursor;
               continue;
            }
            if (type_is_value_aggregate(in->base.type)) {
               struct air_val *av = air_get(c, in->operands[k]);
               if (!av)
                  return false;
               unsigned idx = 0;
               agg_store_regs(c, in->base.type, av, re->data, &idx);
               *(nir_cursor *)ce->data = c->b.cursor;
               continue;
            }
            nir_def *val = air_get_ssa(c, in->operands[k]);
            if (!val)
               return false;
            nir_store_reg(&c->b, val, re->data);
            /* later stores into the same block go after this one */
            *(nir_cursor *)ce->data = c->b.cursor;
         }
      }
   }
   return true;
}

/* -- module-scope variables --------------------------------------------- */

/* the index !air.function_constants gives this global, or -1 */
static int
function_constant_index(struct air_ctx *c, const struct air_global *g)
{
   const struct air_named_md *n = named_md(c->m, "air.function_constants");
   for (unsigned i = 0; i < (n ? n->num_ops : 0); i++) {
      const struct air_md *node = n->ops[i];
      if (!node || node->kind != AIR_MD_NODE || !node->num_ops)
         continue;
      if (!node->ops[0] || node->ops[0]->kind != AIR_MD_VALUE ||
          node->ops[0]->value != &g->base)
         continue;
      /* !{ptr @g, !"uint", !"name", i32 index, i1 true} */
      for (unsigned k = 1; k < node->num_ops; k++) {
         uint64_t idx;
         if (md_int(node, k, &idx))
            return (int)idx;
      }
      return -1;
   }
   return -1;
}

/* Decide what every global is: once per shader, because this appends inline
 * samplers to info->resources and lays out the shared and scratch memory. It
 * records rather than builds -- making the value needs a builder, and which
 * function's builder matters (see materialise_globals).
 */
/* The 64-bit `constexpr sampler` word, field by field (nir_air.h has where the
 * layout comes from and why it took three sources). Bit 63 marks the one-word
 * form; the two-word form of AIR 2.6 carries a LOD bias in the second word's
 * low 16 bits and no module of this corpus uses it, so it is reported rather
 * than decoded.
 */
struct air_sampler_state
air_decode_sampler_state(uint64_t w)
{
#define FIELD(lo, n) ((unsigned)((w >> (lo)) & ((1ull << (n)) - 1)))
   struct air_sampler_state s = {
      .address = { FIELD(0, 3), FIELD(3, 3), FIELD(6, 3) },
      .mag_filter = FIELD(9, 2),
      .min_filter = FIELD(11, 2),
      .mip_filter = FIELD(13, 2),
      .pixel_coord = FIELD(15, 1) != 0,
      .compare_func = FIELD(16, 4),
      .max_anisotropy = FIELD(20, 4) + 1,
      .lod_min = FIELD(24, 16),
      .lod_max = FIELD(40, 16),
      .border_color = FIELD(56, 2),
      .reduction = FIELD(58, 2),
   };
#undef FIELD
   return s;
}

static void
plan_globals(struct air_ctx *c)
{
   c->global_plan = rzalloc_array(c->mem_ctx, struct air_global_plan,
                                  MAX2(c->m->num_globals, 1));
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      struct air_global_plan *pl = &c->global_plan[i];
      const struct air_global *g = c->m->globals[i];
      const char *name = g->base.name ? g->base.name : "";

      /* llvm.compiler.used and llvm.global_ctors are lists for the linker,
       * not memory the shader reads
       */
      if (!strncmp(name, "llvm.", 5)) {
         pl->skip = true;
         continue;
      }

      if (g->externally_initialized) {
         int idx = function_constant_index(c, g);
         if (idx < 0) {
            air_error(c, "interface: an externally initialized variable that is not a "
                         "function constant");
            return;
         }
         pl->class = AIR_PTR_FUNCTION_CONSTANT;
         pl->binding = idx;
      } else if (!strncmp(name, "__air_sampler_state", 19)) {
         /* A sampler the shader declares itself. It needs a real index, not a
          * sentinel: the Adreno's texture instruction holds the sampler in a
          * four-bit field, and ~0u there asserts in ir3's encoder. The index
          * goes after the API's samplers, and the runtime creates the sampler
          * from the state word (docs/air.md section 7.7).
          */
         unsigned next = 0;
         for (unsigned k = 0; k < c->info->num_resources; k++)
            if (c->info->resources[k].kind == AIR_RESOURCE_SAMPLER)
               next = MAX2(next, c->info->resources[k].index + 1);
         pl->class = AIR_PTR_SAMPLER;
         pl->binding = next;

         /* The state word. iOS 26's compiler writes it as [2 x i64] -- the 64-bit word
          * of before, then a second one (0 in all 62 of QuartzCore's iOS 26 modules that
          * declare one) -- where iOS 16.5's wrote a plain i64. The first element is the word
          * air_decode_sampler_state knows; a second one that is not 0 is something this
          * reading does not cover, and is refused by name rather than dropped.
          */
         uint64_t word = 0;
         const struct air_constant *ini = g->init ? air_value_as_constant(g->init) : NULL;
         if (ini && ini->base.type->kind == AIR_TYPE_ARRAY &&
             (ini->ckind == AIR_CONST_DATA || ini->ckind == AIR_CONST_AGGREGATE ||
              ini->ckind == AIR_CONST_NULL)) {
            uint64_t second = 0;
            if (ini->ckind == AIR_CONST_DATA && ini->num_elems >= 1) {
               word = ini->data[0];
               second = ini->num_elems > 1 ? ini->data[1] : 0;
            } else if (ini->ckind == AIR_CONST_AGGREGATE && ini->num_elems >= 1) {
               if (!air_const_uint(ini->elems[0], &word) ||
                   (ini->num_elems > 1 && !air_const_uint(ini->elems[1], &second))) {
                  air_error(c, "interface: an inline sampler whose state is not a constant");
                  return;
               }
            }
            if (second != 0) {
               air_error(c, "interface: an inline sampler with a second state word %#llx "
                            "(iOS 26's [2 x i64] form; only 0 is understood)",
                         (unsigned long long)second);
               return;
            }
         } else if (g->init && !air_const_uint(g->init, &word)) {
            air_error(c, "interface: an inline sampler whose state is not a constant");
            return;
         }
         struct air_shader_info *info = c->info;
         info->resources = reralloc(c->mem_ctx, info->resources, struct air_resource,
                                    info->num_resources + 1);
         info->resources[info->num_resources++] = (struct air_resource) {
            .kind = AIR_RESOURCE_SAMPLER, .index = next, .read = true,
            .param = ~0u, .name = name,
            .inline_sampler = true, .sampler_state = word,
            .sampler = air_decode_sampler_state(word),
            /* not through add_resource, so the "no group" value has to be
             * written here: a zeroed field would read as group 0, and a zeroed
             * argbuf_set as "member 0 of the set at Metal index 0" */
            .raster_order_group = -1,
            .argbuf_set = ~0u, .argbuf_id = ~0u,
         };
      } else if (g->addrspace == 3) {
         /* a static threadgroup array */
         unsigned align = MAX2(g->align, air_type_abi_align(c->m, g->value_type));
         c->shared_size = ALIGN_POT(c->shared_size, MAX2(align, 1));
         pl->class = AIR_PTR_SHARED;
         pl->offset = c->shared_size;
         pl->has_offset = true;
         c->shared_size += air_type_alloc_size(c->m, g->value_type);
      } else if (g->addrspace == 2 && !g->init &&
                 (g->value_type->kind == AIR_TYPE_INT || g->value_type->kind == AIR_TYPE_FLOAT ||
                  (g->value_type->kind == AIR_TYPE_VECTOR &&
                   g->value_type->elem->kind != AIR_TYPE_POINTER))) {
         /* A program-scope SCALAR or VECTOR in the constant address space that the static
          * initializer writes: iOS 26's QuartzCore keeps each of its 60 function constants in
          * one (`@_ZL15fc_has_image_0A = internal addrspace(2) global i8 undef`, stored from
          * the real constant in _GLOBAL__sub_I_ogl_metal_uber.metal). In scratch memory NIR
          * cannot see through the store to the loads, so not one branch on them folded and
          * the uber shader stayed at 219 000 instructions. A shader_temp variable instead:
          * after inlining, nir_lower_global_vars_to_local and vars_to_ssa make every load the
          * constant that was stored.
          */
         pl->class = AIR_PTR_VARIABLE;
         pl->var = nir_variable_create(c->nir, nir_var_shader_temp,
                                       param_glsl_type(c, g->value_type), name);

      } else {
         /* a program-scope variable: private to the invocation, written by
          * the static initializers below
          */
         unsigned align = MAX2(g->align, air_type_abi_align(c->m, g->value_type));
         c->scratch_size = ALIGN_POT(c->scratch_size, MAX2(align, 1));
         pl->class = AIR_PTR_SCRATCH;
         pl->offset = c->scratch_size;
         pl->has_offset = true;
         c->scratch_size += air_type_alloc_size(c->m, g->value_type);
      }
   }
}

/* Build the globals as values of the function being translated now. Called for
 * the entry point and again for every callee, because the byte offset is an
 * nir_def and a def belongs to one impl: handing a callee the entry point's
 * defs is what crashed nir_repair_ssa. Module-level constants need no such
 * care -- air_get materialises one on demand in whatever builder asks.
 */
static void
materialise_globals(struct air_ctx *c)
{
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      const struct air_global_plan *pl = &c->global_plan[i];
      if (pl->skip)
         continue;
      const struct air_global *g = c->m->globals[i];
      struct air_ptr p = {
         .pointee = g->value_type,
         .class = pl->class,
         .binding = pl->binding,
         .offset = pl->has_offset ? nir_imm_int(&c->b, pl->offset) : NULL,
         .var = pl->var,
      };
      air_set(c, &g->base, air_val_ptr(c, p));
   }
}

/* The functions of @llvm.global_ctors, inlined at the top of the entry point:
 * this is where a function constant turns into the program-scope variable the
 * body reads (AV1SW computes kPixelShift from kBitDepth that way).
 */
static void
run_static_initializers(struct air_ctx *c)
{
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const struct air_function *f = c->m->functions[i];
      if (f->is_declaration || !f->section || strcmp(f->section, "air.static_init"))
         continue;
      if (f->num_args) {
         air_error(c, "interface: a static initializer with arguments");
         return;
      }
      if (f->num_blocks != 1) {
         /* A static initializer with branches cannot be translated in place:
          * the CFG walk owns the impl's start block, so running it a second
          * time in the same impl would map two AIR blocks onto it. It becomes
          * a nir_function instead and the entry point calls it, which is the
          * machinery a call of another function in the module already uses,
          * and nir_inline_functions folds it back in afterwards. The globals
          * it writes are at the offsets plan_globals decided, which is one
          * plan for the whole module, so the callee's stores and the entry
          * point's loads reach the same scratch.
          *
          * These are the GL-on-Metal fragment shaders, whose initializer
          * decides 58 function constants and has a switch in it -- which is
          * also why this could not be reached before the reader learnt to read
          * one.
          */
         nir_function *fn = translate_callee(c, f, NULL, NULL, NULL);
         if (!fn)
            return;   /* translate_callee has said why */
         nir_builder_instr_insert(&c->b, &nir_call_instr_create(c->nir, fn)->instr);
         continue;
      }
      /* its instructions, in place, without its ret */
      const struct air_block *blk = &f->blocks[0];
      for (unsigned k = 0; k < blk->num_insts; k++) {
         const struct air_inst *in = blk->insts[k];
         if (in->op == AIR_OP_RET)
            continue;
         if (!translate_instruction(c, in))
            return;
      }
   }
}

/* -- the entry point ---------------------------------------------------- */

/* Which `device` buffers the shader really writes, from the shader rather than from its
 * metadata: a `device` pointer without `const` is read_write to Metal whether anything is
 * stored through it or not, and the runtime may serve a read-only one at an offset the
 * GPU cannot fetch from by copying it (air_metal.c) -- the window server's VfxXgh reads
 * its vertices through such a pointer at offsets like 2080. A store whose buffer index is
 * not a constant counts as a store to every buffer.
 */
/* Which textures, images and samplers the shader as translated actually reaches (nir_air.h,
 * air_resource.unused). Everything counts as reached when one index is not a constant. */
static void
air_find_unused_resources(nir_shader *nir, struct air_shader_info *info)
{
   uint64_t tex = 0, samp = 0;
   bool dynamic = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *t = nir_instr_as_tex(instr);
               if (nir_tex_instr_src_index(t, nir_tex_src_texture_deref) >= 0 ||
                   nir_tex_instr_src_index(t, nir_tex_src_texture_offset) >= 0 ||
                   nir_tex_instr_src_index(t, nir_tex_src_sampler_deref) >= 0 ||
                   nir_tex_instr_src_index(t, nir_tex_src_sampler_offset) >= 0 ||
                   t->texture_index >= 64 || t->sampler_index >= 64) {
                  dynamic = true;
                  continue;
               }
               tex |= 1ull << t->texture_index;
               if (nir_tex_instr_need_sampler(t))
                  samp |= 1ull << t->sampler_index;
               continue;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            const char *iname = nir_intrinsic_infos[intr->intrinsic].name;
            if (strncmp(iname, "image_", 6) && strncmp(iname, "bindless_image_", 15))
               continue;
            if (!strncmp(iname, "bindless_", 9) || !nir_src_is_const(intr->src[0]) ||
                nir_src_as_uint(intr->src[0]) >= 64)
               dynamic = true;
            else
               tex |= 1ull << nir_src_as_uint(intr->src[0]);
         }
      }
   }
   if (dynamic)
      return;
   for (unsigned i = 0; i < info->num_resources; i++) {
      struct air_resource *r = &info->resources[i];
      if (r->argbuf_set != ~0u || r->index >= 64)
         continue;
      if (r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE)
         r->unused = !(tex & (1ull << r->index));
      else if (r->kind == AIR_RESOURCE_SAMPLER && !r->inline_sampler)
         r->unused = !(samp & (1ull << r->index));
   }

   /* An inline sampler goes after the API's samplers (plan_globals), and RenderBox's uber
    * shaders declare all sixteen of those: its own samplers came out at 16 and up, past the
    * sixteen this GPU has per stage, and every such draw was refused (run glass22: "uber_fragment
    * binds a sampler at index 16"). Most of the sixteen are unused once specialised, so an inline
    * sampler past the end moves to the lowest slot nothing else uses, here and in the shader. */
   uint32_t taken = 0;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_SAMPLER && !r->unused && r->index < 32 &&
          (!r->inline_sampler || r->index < 16))
         taken |= 1u << r->index;
   }
   for (unsigned i = 0; i < info->num_resources; i++) {
      struct air_resource *r = &info->resources[i];
      if (r->kind != AIR_RESOURCE_SAMPLER || !r->inline_sampler || r->index < 16)
         continue;
      if (!(samp & (1ull << r->index))) {
         r->unused = true;
         continue;
      }
      unsigned to = ffs(~taken & 0xffffu);
      if (!to)
         break;            /* no room: left where it is, and refused at the draw as before */
      to--;
      taken |= 1u << to;
      unsigned from = r->index;
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_tex && nir_instr_as_tex(instr)->sampler_index == from)
                  nir_instr_as_tex(instr)->sampler_index = to;
            }
         }
      }
      r->index = to;
   }
}

static void
air_find_written_buffers(nir_shader *nir, struct air_shader_info *info)
{
   uint64_t written = 0;
   bool any = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            nir_src *index;
            switch (intr->intrinsic) {
            case nir_intrinsic_store_ssbo:
               index = &intr->src[1];
               break;
            case nir_intrinsic_ssbo_atomic:
            case nir_intrinsic_ssbo_atomic_swap:
               index = &intr->src[0];
               break;
            default:
               continue;
            }
            if (nir_src_is_const(*index) && nir_src_as_uint(*index) < 64)
               written |= 1ull << nir_src_as_uint(*index);
            else
               any = true;
         }
      }
   }
   for (unsigned i = 0; i < info->num_resources; i++) {
      struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->write && !any &&
          r->index < 64 && !(written & (1ull << r->index)))
         r->write = false;
   }
}

/*
 * Scratch memory as variables: one nir_variable a 32-bit word, when every access to scratch is at
 * an offset known here. The frontend lays each alloca out in scratch (AIR_OP_ALLOCA) and reads and
 * writes it with load_scratch/store_scratch, which no NIR pass looks through -- so a local struct
 * that a specialised uber-shader fills with function-constant values and then branches and loops
 * on (QuartzCore's fixed_frag_lph_cpf: 248 bytes, 508 stores, 3522 loads, every offset constant)
 * kept all of it alive. As words, vars_to_ssa makes those values SSA and folding goes through them.
 * Little-endian bytes, as the memory was: a sub-word or unaligned access is shifts and masks.
 */
static nir_def *
scratch_word_byte(nir_builder *b, nir_variable **words, unsigned o)
{
   return nir_iand_imm(b, nir_ushr_imm(b, nir_load_var(b, words[o / 4]), (o % 4) * 8), 0xff);
}

static nir_def *
scratch_read(nir_builder *b, nir_variable **words, unsigned o, unsigned bytes)
{
   nir_def *v;
   if (bytes == 4 && o % 4 == 0)
      return nir_load_var(b, words[o / 4]);
   if (o % 4 + bytes <= 4) {
      v = nir_ushr_imm(b, nir_load_var(b, words[o / 4]), (o % 4) * 8);
   } else {
      v = nir_imm_int(b, 0);
      for (unsigned i = 0; i < bytes; i++)
         v = nir_ior(b, v, nir_ishl_imm(b, scratch_word_byte(b, words, o + i), 8 * i));
   }
   return bytes == 4 ? v : nir_u2uN(b, v, bytes * 8);
}

static void
scratch_write_in_word(nir_builder *b, nir_variable **words, unsigned o, unsigned bytes, nir_def *v32)
{
   unsigned shift = (o % 4) * 8;
   uint32_t mask = (bytes == 4 ? 0xffffffffu : ((1u << (bytes * 8)) - 1)) << shift;
   if (bytes == 4) {
      nir_store_var(b, words[o / 4], v32, 1);
      return;
   }
   nir_def *old = nir_load_var(b, words[o / 4]);
   nir_def *part = nir_iand_imm(b, nir_ishl_imm(b, v32, shift), mask);
   nir_store_var(b, words[o / 4], nir_ior(b, nir_iand_imm(b, old, ~mask), part), 1);
}

static void
scratch_write(nir_builder *b, nir_variable **words, unsigned o, unsigned bytes, nir_def *v)
{
   nir_def *v32 = v->bit_size == 32 ? v : nir_u2u32(b, v);
   if (o % 4 + bytes <= 4) {
      scratch_write_in_word(b, words, o, bytes, v32);
      return;
   }
   for (unsigned i = 0; i < bytes; i++)
      scratch_write_in_word(b, words, o + i, 1, nir_ushr_imm(b, v32, 8 * i));
}

/*
 * Every loop of a render stage gets an iteration cap. A loop whose trip count comes out of a
 * buffer -- RenderBox's uber_fragment counts its blur taps in a 16-bit int against a count in
 * its constants -- never ends when that count is wrong, and on the Adreno a fragment shader
 * that never ends is the whole GPU stopped: run glass25, "no answer", the CP done with its
 * indirect buffer and waiting for the shader core, every process's draws after it lost. A
 * capped loop is wrong pixels in one draw instead. Compute kernels are left alone (MPS loops
 * over whole rows). AIR_LOOP_CAP=N on the Mac: another cap; 0: none.
 */
static bool
air_cap_loops(nir_shader *nir, unsigned limit)
{
   bool progress = false;

   /* values a loop defines and the code after it uses go through phis after the loop first, which
    * the new exit gives an undef: without them, a use after the loop of something the body defines
    * past the cap's test is a use its definition no longer dominates */
   nir_convert_to_lcssa(nir, false, false);
   nir_foreach_function_impl(impl, nir) {
      nir_loop *loops[512];
      unsigned count = 0;

      nir_foreach_block(block, impl) {
         nir_cf_node *parent = block->cf_node.parent;
         if (parent->type == nir_cf_node_loop && count < ARRAY_SIZE(loops) &&
             nir_loop_first_block(nir_cf_node_as_loop(parent)) == block)
            loops[count++] = nir_cf_node_as_loop(parent);
      }
      if (count == 0)
         continue;
      nir_builder b = nir_builder_create(impl);
      for (unsigned i = 0; i < count; i++) {
         nir_variable *v = nir_local_variable_create(impl, glsl_uint_type(), "air_loop_cap");
         b.cursor = nir_before_cf_node(&loops[i]->cf_node);
         nir_store_var(&b, v, nir_imm_int(&b, 0), 1);
         b.cursor = nir_after_phis(nir_loop_first_block(loops[i]));
         nir_def *n = nir_iadd_imm(&b, nir_load_var(&b, v), 1);
         nir_store_var(&b, v, n, 1);
         nir_if *nif = nir_push_if(&b, nir_uge_imm(&b, n, limit));
         nir_jump(&b, nir_jump_break);
         nir_pop_if(&b, nif);
         /* the builder's jump does not give the phis after the loop a source for the new exit */
         nir_insert_phi_undef(nir_cf_node_as_block(nir_cf_node_next(&loops[i]->cf_node)),
                              nir_if_first_then_block(nif));
      }
      nir_progress(true, impl, nir_metadata_none);
      progress = true;
   }
   return progress;
}

/*
 * Every scratch access made 32 bits wide. ir3 keeps the width of a load_scratch/store_scratch
 * (ldp.u8, stp.u16), and RenderBox's uber_fragment, which copies structs of bytes and halves
 * into private memory, came out with 71 such accesses; its path draws -- the ones that reach
 * them -- stopped the Adreno 830 every time (runs glass25-30: the same draw with a 1x1 scissor
 * finishes). A narrow load is the dword it is in, shifted; a narrow store a read-modify-write
 * of that dword, which nothing else sees: private memory is the invocation's own.
 */
static bool
widen_scratch(nir_shader *nir)
{
   bool progress = false;

   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            bool load = in->intrinsic == nir_intrinsic_load_scratch;
            if (!load && in->intrinsic != nir_intrinsic_store_scratch)
               continue;
            unsigned bits = load ? in->def.bit_size : nir_src_bit_size(in->src[0]);
            unsigned comps = load ? in->def.num_components : nir_src_num_components(in->src[0]);
            if (bits >= 32 || bits == 1)
               continue;

            b.cursor = nir_before_instr(instr);
            nir_def *addr = in->src[load ? 0 : 1].ssa;
            nir_def *parts[NIR_MAX_VEC_COMPONENTS];
            unsigned mask = load ? 0 : nir_intrinsic_write_mask(in);
            for (unsigned i = 0; i < comps; i++) {
               if (!load && !(mask & (1u << i)))
                  continue;
               nir_def *at = nir_iadd_imm(&b, addr, i * (bits / 8));
               nir_def *dword = nir_iand_imm(&b, at, ~3u);
               nir_def *shift = nir_ishl_imm(&b, nir_iand_imm(&b, at, 3), 3);
               nir_def *word = nir_load_scratch(&b, 1, 32, dword, .align_mul = 4);
               if (load) {
                  parts[i] = nir_u2uN(&b, nir_ushr(&b, word, shift), bits);
               } else {
                  nir_def *value = nir_u2u32(&b, nir_channel(&b, in->src[0].ssa, i));
                  nir_def *field = nir_ishl(&b, nir_imm_int(&b, (1u << bits) - 1), shift);
                  word = nir_ior(&b, nir_iand(&b, word, nir_inot(&b, field)),
                                 nir_ishl(&b, value, shift));
                  nir_store_scratch(&b, word, dword, .align_mul = 4);
               }
            }
            if (load)
               nir_def_rewrite_uses(&in->def, nir_vec(&b, parts, comps));
            nir_instr_remove(instr);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   return progress;
}

/*
 * Metal's discard_fragment, carried out at the end. The frontend says demote (air_intrinsics.c,
 * discard_op: the thread lives on as a helper), and the a6xx-family encoder turns ir3's DEMOTE into
 * the hardware's KILL: the lane stops there. RenderBox's uber_fragment discards every fragment of a
 * path fan with no coverage and goes on to take derivatives (dsxpp/dsypp, which read the quad's
 * other lanes) and to fetch the framebuffer -- and in exactly those draws the Adreno 830 stopped,
 * every time, for good (runs glass25-37; the same draws finish when no fragment runs). So the
 * discard only marks the invocation, the invocation runs to the end as the helper Metal says it
 * is, and is terminated there, its outputs with it. AIR_DEMOTE=1 keeps the demote.
 */
static bool
defer_demote(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_variable *flag = NULL;
   nir_builder b = nir_builder_create(impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic ||
             nir_instr_as_intrinsic(instr)->intrinsic != nir_intrinsic_demote)
            continue;
         if (!flag) {
            flag = nir_local_variable_create(impl, glsl_bool_type(), "air_discarded");
            b.cursor = nir_before_impl(impl);
            nir_store_var(&b, flag, nir_imm_false(&b), 1);
         }
         b.cursor = nir_before_instr(instr);
         nir_store_var(&b, flag, nir_imm_true(&b), 1);
         nir_instr_remove(instr);
      }
   }
   if (!flag)
      return false;
   b.cursor = nir_after_impl(impl);
   nir_terminate_if(&b, nir_load_var(&b, flag));
   nir_progress(true, impl, nir_metadata_none);
   return true;
}

static bool
scratch_to_words(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   uint64_t end = nir->scratch_size;
   if (!nir->scratch_size || nir->scratch_size > 4096)
      return false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         nir_src *off;
         unsigned bits, comps;
         if (intr->intrinsic == nir_intrinsic_load_scratch)
            off = &intr->src[0], bits = intr->def.bit_size, comps = intr->def.num_components;
         else if (intr->intrinsic == nir_intrinsic_store_scratch)
            off = &intr->src[1], bits = intr->src[0].ssa->bit_size, comps = intr->src[0].ssa->num_components;
         else
            continue;
         if (!nir_src_is_const(*off) || (bits != 8 && bits != 16 && bits != 32))
            return false;
         /* (a vec3 at the end of a local may be read as a vec4: the words cover every access) */
         end = MAX2(end, nir_src_as_uint(*off) + comps * bits / 8);
      }
   }

   if (end > 4096 + 64)
      return false;
   unsigned count = DIV_ROUND_UP((unsigned)end, 4);
   nir_variable **words = ralloc_array(nir, nir_variable *, count);
   for (unsigned i = 0; i < count; i++)
      words[i] = nir_local_variable_create(impl, glsl_uint_type(), "scratch_word");
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         b.cursor = nir_before_instr(instr);
         if (intr->intrinsic == nir_intrinsic_load_scratch) {
            unsigned o = nir_src_as_uint(intr->src[0]), bytes = intr->def.bit_size / 8;
            nir_def *comps[NIR_MAX_VEC_COMPONENTS];
            for (unsigned k = 0; k < intr->def.num_components; k++)
               comps[k] = scratch_read(&b, words, o + k * bytes, bytes);
            nir_def_replace(&intr->def, nir_vec(&b, comps, intr->def.num_components));
         } else if (intr->intrinsic == nir_intrinsic_store_scratch) {
            unsigned o = nir_src_as_uint(intr->src[1]), bytes = intr->src[0].ssa->bit_size / 8;
            unsigned mask = nir_intrinsic_write_mask(intr);
            for (unsigned k = 0; k < intr->src[0].ssa->num_components; k++)
               if (mask & (1u << k))
                  scratch_write(&b, words, o + k * bytes, bytes, nir_channel(&b, intr->src[0].ssa, k));
            nir_instr_remove(instr);
         }
      }
   }
   nir->scratch_size = 0;
   return nir_progress(true, impl, nir_metadata_none);
}

#include <time.h>
static double
air_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#define AIR_T(label) do { if (air_timing) { double _n = air_ms(); \
   fprintf(stderr, "AIR_TIME %-14s %7.2f ms\n", label, _n - air_t); air_t = air_ms(); } } while (0)

nir_shader *
air_to_nir(void *mem_ctx, const void *bitcode, size_t size, const char *entry_point,
           const struct air_to_nir_options *options,
           const nir_shader_compiler_options *nir_options,
           struct air_shader_info *info, const char **error)
{
   void *ctx = ralloc_context(mem_ctx);
   struct air_shader_info local_info = { 0 };
   struct air_ctx c = {
      .mem_ctx = ctx,
      .options = options,
      .info = info ? info : &local_info,
   };
   memset(c.info, 0, sizeof(*c.info));

   const char *read_error = NULL;
   bool air_timing = getenv("AIR_TIME") != NULL;
   double air_t = air_timing ? air_ms() : 0;
   c.m = air_module_read(ctx, bitcode, size, &read_error);
   AIR_T("read");
   if (!c.m) {
      if (error)
         *error = ralloc_asprintf(mem_ctx, "reader: %s", read_error);
      ralloc_free(ctx);
      return NULL;
   }

   const struct air_md *entry = NULL;
   int stage = module_stage(c.m, &entry);
   if (stage == -2) {
      if (error)
         *error = ralloc_strdup(mem_ctx, "stage: no entry point metadata");
      ralloc_free(ctx);
      return NULL;
   }
   /* air.visible: a library function, linked into its caller at pipeline time
    * (docs/air.md section 7.5). Not an entry point, so not a shader -- unless
    * the caller asks for the generated wrapper, which makes one compute shader
    * whose only job is to call it (wrap_library_function).
    */
   bool wrap = false;
   if (stage == MESA_SHADER_NONE) {
      if (!options || !options->library_wrapper) {
         if (error)
            *error = ralloc_strdup(mem_ctx,
                                   "stage: air.visible, a library function (not yet)");
         ralloc_free(ctx);
         return NULL;
      }
      stage = MESA_SHADER_COMPUTE;
      wrap = true;
   }

   if (!entry->num_ops || !entry->ops[0] || entry->ops[0]->kind != AIR_MD_VALUE ||
       entry->ops[0]->value->kind != AIR_VALUE_FUNCTION) {
      if (error)
         *error = ralloc_strdup(mem_ctx, "stage: entry point metadata without a function");
      ralloc_free(ctx);
      return NULL;
   }
   const struct air_function *fn = (const struct air_function *)entry->ops[0]->value;
   if (entry_point && strcmp(entry_point, fn->base.name ? fn->base.name : "")) {
      if (error)
         *error = ralloc_asprintf(mem_ctx, "stage: this module's entry point is %s",
                                  fn->base.name);
      ralloc_free(ctx);
      return NULL;
   }
   if (fn->is_declaration) {
      if (error)
         *error = ralloc_strdup(mem_ctx, "stage: the entry point has no body");
      ralloc_free(ctx);
      return NULL;
   }
   c.fn = fn;

   c.nir = nir_shader_create(mem_ctx, stage, nir_options);
   c.nir->info.name = ralloc_strdup(c.nir, fn->base.name ? fn->base.name : "air");
   c.nir->info.internal = false;
   c.fns = _mesa_pointer_hash_table_create(ctx);
   nir_function *main = nir_function_create(c.nir, "main");
   main->is_entrypoint = true;
   c.b = nir_builder_at(nir_before_impl(nir_function_impl_create(main)));
   c.b.constant_fold_alu = true;      /* function constants fold, and what they decide is known */
   c.b.impl->structured = false;
   c.const_cursor = nir_before_impl(c.b.impl);

   c.values = _mesa_pointer_hash_table_create(ctx);
   c.globals = _mesa_pointer_hash_table_create(ctx);
   c.blocks = _mesa_pointer_hash_table_create(ctx);
   c.phi_cursors = _mesa_pointer_hash_table_create(ctx);
   c.phi_regs = _mesa_pointer_hash_table_create(ctx);
   c.num_params = fn->num_args;
   c.params = rzalloc_array(ctx, struct air_val *, MAX2(c.num_params, 1));

   c.info->stage = stage;
   c.info->entry_point = fn->base.name;

   /* the outputs: fields of the returned struct, in order. A wrapped library
    * function has none of this: its result is a value the wrapper stores to a
    * buffer, not a shader output, and its own metadata uses air.visible_input
    * and air.visible_output rather than the keys translate_input reads.
    */
   const struct air_md *out_node = (!wrap && entry->num_ops > 1) ? entry->ops[1] : NULL;
   const struct air_type *rt = fn->fn_type->elem;
   unsigned num_fields = wrap ? 0 :
                         rt->kind == AIR_TYPE_STRUCT ? rt->num_members :
                         rt->kind == AIR_TYPE_VOID ? 0 : 1;
   c.num_outputs = num_fields;
   c.outputs = rzalloc_array(ctx, __typeof__(*c.outputs), MAX2(num_fields, 1));

   unsigned next_out_varying = 0, next_in_varying = 0;
   if (out_node && out_node->num_ops != num_fields && num_fields) {
      air_error(&c, "interface: %u outputs for %u result fields", out_node->num_ops,
                num_fields);
   } else {
      for (unsigned i = 0; i < num_fields && !c.error[0]; i++) {
         const struct air_type *ft = rt->kind == AIR_TYPE_STRUCT ? rt->members[i] : rt;
         if (!out_node || i >= out_node->num_ops || !out_node->ops[i]) {
            air_error(&c, "interface: result field %u has no metadata", i);
            break;
         }
         translate_output(&c, out_node->ops[i], i, ft, &next_out_varying);
      }
   }

   /* the inputs */
   const struct air_md *in_node = (!wrap && entry->num_ops > 2) ? entry->ops[2] : NULL;
   for (unsigned i = 0; i < (in_node ? in_node->num_ops : 0) && !c.error[0]; i++)
      translate_input(&c, in_node->ops[i], &next_in_varying);

   /* Now that every direct argument's Metal index is known, the members of the
    * argument buffers can be given bindings that cannot collide with them --
    * and before plan_globals, so that a shader's own `constexpr sampler` still
    * comes after every sampler the API binds.
    */
   if (!c.error[0] && c.num_argbufs)
      allocate_argbuf_indices(&c);

   /* the entry point's parameters, as the body will read them */
   for (unsigned i = 0; i < (wrap ? 0 : c.num_params) && !c.error[0]; i++) {
      if (c.params[i])
         air_set(&c, &fn->args[i], c.params[i]);
      else
         air_error(&c, "interface: parameter %u has no metadata", i);
   }

   if (!c.error[0])
      plan_globals(&c);
   if (!c.error[0])
      materialise_globals(&c);
   if (!c.error[0])
      run_static_initializers(&c);

   if (!c.error[0]) {
      if (wrap) {
         /* air.visible's second operand is the result node, whose one entry
          * carries air.visible_output and the type's name
          */
         const struct air_md *rn = entry->num_ops > 1 ? entry->ops[1] : NULL;
         wrap_library_function(&c, rn && rn->num_ops ? rn->ops[0] : NULL,
                               entry->num_ops > 2 ? entry->ops[2] : NULL);
      }
      else
         translate_body(&c);
   }

   if (c.error[0]) {
      if (error)
         *error = ralloc_strdup(mem_ctx, c.error);
      ralloc_free(c.nir);
      ralloc_free(ctx);
      return NULL;
   }

   /* What the shader expects to be bound, as NIR counts it: Metal's indices
    * are the binding numbers, so a shader using [[buffer(14)]] alone still
    * declares fifteen UBOs. ir3 and NIR's own passes read these.
    */
   for (unsigned i = 0; i < c.info->num_resources; i++) {
      const struct air_resource *r = &c.info->resources[i];
      switch (r->kind) {
      case AIR_RESOURCE_CONSTANT_BUFFER:
         c.nir->info.num_ubos = MAX2(c.nir->info.num_ubos, r->index + 1);
         break;
      case AIR_RESOURCE_DEVICE_BUFFER:
         c.nir->info.num_ssbos = MAX2(c.nir->info.num_ssbos, r->index + 1);
         break;
      case AIR_RESOURCE_TEXTURE:
         c.nir->info.num_textures = MAX2(c.nir->info.num_textures, r->index + 1);
         BITSET_SET(c.nir->info.textures_used, r->index);
         break;
      case AIR_RESOURCE_IMAGE:
         c.nir->info.num_images = MAX2(c.nir->info.num_images, r->index + 1);
         BITSET_SET(c.nir->info.images_used, r->index);
         break;
      default:
         break;
      }
   }

   c.nir->scratch_size = c.scratch_size;
   c.nir->info.shared_size = c.shared_size;
   c.info->threadgroup_bytes = c.shared_size;
   c.nir->info.io_lowered = false;
   if (stage == MESA_SHADER_COMPUTE) {
      /* the threadgroup size is a dispatch argument, not a shader constant */
      c.nir->info.workgroup_size_variable = true;
   }

   AIR_T("translate");
   nir_lower_goto_ifs(c.nir);
   AIR_T("goto_ifs");
   if (air_timing) {
      nir_foreach_function_impl(impl, c.nir) {
         unsigned n = 0;
         nir_foreach_block(block, impl) nir_foreach_instr(instr, block) n++;
         if (n > 300)
            fprintf(stderr, "AIR_TIME fn %6u %s\n", n, impl->function->name ? impl->function->name : "?");
      }
   }

   /* Fold in any function of the module the shader called, and then drop the
    * callees. This has to come after nir_lower_goto_ifs and not before: this
    * translator emits an unstructured CFG, and nir_inline_functions walks the
    * control-flow tree, which asserts on one. The pass order inside is the one
    * nir_functions.c prescribes -- inline, then nir_opt_deref to undo the cast
    * a pointer parameter leaves behind, then delete everything but the entry
    * point, which every consumer of this NIR assumes.
    */
   if (_mesa_hash_table_num_entries(c.fns)) {
      /* A callee that returns from inside a loop or an if comes out of nir_lower_goto_ifs with
       * a return jump there, which nir_inline_function_impl asserts on: RenderBox's uber_fragment,
       * primitive_color_fragment and five more of its library (glass16's MercuryPoster, SwiftUI) */
      NIR_PASS(_, c.nir, nir_lower_returns);
      NIR_PASS(_, c.nir, nir_inline_functions);
      NIR_PASS(_, c.nir, nir_opt_deref);
      nir_remove_non_entrypoints(c.nir);
      /* the program-scope variables (AIR_PTR_VARIABLE) into the one function left, where
       * vars_to_ssa turns each load into what the static initializer stored */
      NIR_PASS(_, c.nir, nir_lower_global_vars_to_local);
      NIR_PASS(_, c.nir, nir_lower_variable_initializers, nir_var_function_temp);
      NIR_PASS(_, c.nir, nir_split_var_copies);
      NIR_PASS(_, c.nir, nir_lower_var_copies);
      NIR_PASS(_, c.nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, c.nir, nir_opt_dce);
   }

   AIR_T("inline+ssa");
   if (air_timing) {
      unsigned n = 0;
      nir_foreach_function_impl(impl, c.nir) nir_foreach_block(block, impl) nir_foreach_instr(instr, block) n++;
      fprintf(stderr, "AIR_TIME instructions before the opt loops: %u\n", n);
   }
   /* (and where nothing was inlined: the static initializer's stores are in the entry point) */
   NIR_PASS(_, c.nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, c.nir, nir_lower_vars_to_ssa);

   /* What the function constants leave dead, gone before any backend sees it. Each constant's
    * load is its value (AIR_PTR_FUNCTION_CONSTANT), so a specialised uber-shader is mostly
    * branches on immediates -- but nothing folded them: QuartzCore's fixed_frag_lph_cpf with the
    * constants macOS's CoreAnimation gives a plain image draw came out of here as 292,000 lines
    * of NIR (12,616 ifs, 1,344 loops), and llvmpipe took 240 s and 8.1 GB to compile it
    * (glassref, 28 October) -- on the phone, backboardd's jetsam with boot-arg metal_fc=1. */
   {
      bool progress;
      unsigned rounds = 0;
      do {
         progress = false;
         NIR_PASS(progress, c.nir, nir_opt_copy_prop);
         NIR_PASS(progress, c.nir, nir_opt_constant_folding);
         NIR_PASS(progress, c.nir, nir_opt_algebraic);
         NIR_PASS(progress, c.nir, nir_opt_dead_cf);
         NIR_PASS(progress, c.nir, nir_opt_remove_phis);
         NIR_PASS(progress, c.nir, nir_opt_dce);
      } while (progress && ++rounds < 32);
      AIR_T("loop1");
      /* and through the locals (scratch_to_words), which wanted the offsets folded first */
      if (scratch_to_words(c.nir)) {
         NIR_PASS(_, c.nir, nir_lower_vars_to_ssa);
         NIR_PASS(_, c.nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
         rounds = 0;
         do {
            progress = false;
            NIR_PASS(progress, c.nir, nir_opt_copy_prop);
            NIR_PASS(progress, c.nir, nir_opt_constant_folding);
            NIR_PASS(progress, c.nir, nir_opt_algebraic);
            NIR_PASS(progress, c.nir, nir_opt_dead_cf);
            NIR_PASS(progress, c.nir, nir_opt_remove_phis);
            NIR_PASS(progress, c.nir, nir_opt_dce);
         } while (progress && ++rounds < 32);
      }
      AIR_T("words+loop2");
      /* ... and what only CSE, if-folding and undef propagation reach. A specialisation of
       * fixed_frag_lph_cph still came out of the loops above as 8,907 instructions; the driver
       * keeps its own copy of the NIR for as long as the pipeline lives (ir3 makes variants from
       * it), 6 MB each, and backboardd made 295 such pipelines before its jetsam in run glass8.
       * Through these: 25 instructions, the same 48 of ir3's, and 0.1 MB (rwa_ir3_cost driver_kept). */
      rounds = 0;
      do {
         progress = false;
         NIR_PASS(progress, c.nir, nir_opt_copy_prop);
         NIR_PASS(progress, c.nir, nir_opt_dce);
         NIR_PASS(progress, c.nir, nir_opt_dead_cf);
         NIR_PASS(progress, c.nir, nir_opt_cse);
         NIR_PASS(progress, c.nir, nir_opt_algebraic);
         NIR_PASS(progress, c.nir, nir_opt_constant_folding);
         NIR_PASS(progress, c.nir, nir_opt_remove_phis);
         NIR_PASS(progress, c.nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
         NIR_PASS(progress, c.nir, nir_opt_undef);
      } while (progress && ++rounds < 32);
   }

   AIR_T("opt loops");
   if (stage == MESA_SHADER_FRAGMENT && !getenv("AIR_DEMOTE") && defer_demote(c.nir)) {
      NIR_PASS(_, c.nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, c.nir, nir_opt_dce);
   }
   if (!getenv("AIR_NARROW_SCRATCH") && widen_scratch(c.nir)) {
      NIR_PASS(_, c.nir, nir_opt_constant_folding);
      NIR_PASS(_, c.nir, nir_opt_cse);
      NIR_PASS(_, c.nir, nir_opt_dce);
   }
   if (stage != MESA_SHADER_COMPUTE) {
      const char *cap = getenv("AIR_LOOP_CAP");
      unsigned limit = cap != NULL ? (unsigned)strtoul(cap, NULL, 0) : 16384;
      if (limit != 0 && air_cap_loops(c.nir, limit)) {
         NIR_PASS(_, c.nir, nir_lower_vars_to_ssa);
         NIR_PASS(_, c.nir, nir_opt_dce);
      }
   }
   nir_index_ssa_defs(nir_shader_get_entrypoint(c.nir));

   if (info) {
      /* The info's arrays live under mem_ctx, not under the scratch context --
       * and so must every STRING in them, which is the part this used to get
       * wrong. `air_md_string` returns a NUL-terminated copy allocated as a
       * child of the metadata node it came from, so every `air.arg_name` and
       * `air.arg_type_name` in here pointed into the module, which is under
       * `ctx`, which is freed three lines below. The caller was handed
       * pointers into freed memory and mostly got away with it, because the
       * bytes usually survive; what it looks like when they do not is
       * `--interface` printing `name=on=1` for an attribute whose name is
       * `texcoord1`, which is how this was found -- vattrcheck.py counted four
       * attributes in a three-attribute shader because two garbled lines each
       * carried a stray tab.
       *
       * vertex_attributes was not copied at all, which was the same bug with
       * no bytes left to get away with.
       */
      if (info->num_resources) {
         info->resources = ralloc_memdup(mem_ctx, info->resources,
                                         info->num_resources * sizeof(*info->resources));
         for (unsigned i = 0; i < info->num_resources; i++) {
            info->resources[i].name = ralloc_strdup(mem_ctx, info->resources[i].name);
            info->resources[i].type_name =
               ralloc_strdup(mem_ctx, info->resources[i].type_name);
         }
      }
      if (info->num_varyings) {
         info->varyings = ralloc_memdup(mem_ctx, info->varyings,
                                        info->num_varyings * sizeof(*info->varyings));
         for (unsigned i = 0; i < info->num_varyings; i++)
            info->varyings[i].name = ralloc_strdup(mem_ctx, info->varyings[i].name);
      }
      if (info->num_vertex_attributes) {
         info->vertex_attributes =
            ralloc_memdup(mem_ctx, info->vertex_attributes,
                          info->num_vertex_attributes * sizeof(*info->vertex_attributes));
         for (unsigned i = 0; i < info->num_vertex_attributes; i++) {
            struct air_vertex_attribute *a = &info->vertex_attributes[i];
            a->name = ralloc_strdup(mem_ctx, a->name);
            a->type_name = ralloc_strdup(mem_ctx, a->type_name);
            a->function_constant = ralloc_strdup(mem_ctx, a->function_constant);
         }
      }
      if (info->num_notes) {
         info->notes = ralloc_memdup(mem_ctx, info->notes,
                                     info->num_notes * sizeof(*info->notes));
         for (unsigned i = 0; i < info->num_notes; i++)
            info->notes[i] = ralloc_strdup(mem_ctx, info->notes[i]);
      }
      info->entry_point = ralloc_strdup(mem_ctx, info->entry_point);
      air_find_written_buffers(c.nir, info);
      air_find_unused_resources(c.nir, info);
   }
   ralloc_free(ctx);
   /* What the passes above removed -- the callees before inlining, the code the function constants
    * left dead -- is still allocated under the shader until a sweep: a specialisation of QuartzCore's
    * uber shader kept 40 MB that way, for as long as its pipeline lived (rwa_render_create keeps the
    * context), and CoreAnimation makes dozens of them (rwa_ir3_cost's `retained`; run glass8). */
   if (c.nir)
      nir_sweep(c.nir);
   AIR_T("info+sweep");
   if (error)
      *error = NULL;
   return c.nir;
}
