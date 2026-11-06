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
#include "nir_constant_expressions.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
   struct air_val *v = rzalloc(c->mem_ctx, struct air_val);
   v->kind = AIR_VAL_PTR_SELECT;
   v->def = cond;
   v->num_elems = 2;
   v->elems = rzalloc_array(c->mem_ctx, struct air_val *, 2);
   v->elems[0] = a;
   v->elems[1] = b;
   return v;
}

/* Is this value a pointer in any of the three senses? */
static bool
val_is_pointerish(const struct air_val *v)
{
   return v && (v->kind == AIR_VAL_PTR || v->kind == AIR_VAL_PTR_SELECT);
}

/* The local a pointer is stored into or loaded from, by its byte offset in
 * scratch, or -1: a local at a place known now, and nothing else. */
/* A byte offset known while translating, through the arithmetic a getelementptr
 * leaves on it (an add of two constants is an iadd here, not a constant). */
static bool
offset_known(nir_def *d, uint64_t *out, unsigned depth)
{
   if (depth > 16 || d->num_components != 1)
      return false;
   if (nir_src_is_const(nir_src_for_ssa(d))) {
      *out = nir_src_as_uint(nir_src_for_ssa(d));
      return true;
   }
   nir_instr *ins = d->parent_instr;
   if (ins->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(ins);
   uint64_t a = 0, b = 0;
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      if (alu->src[i].swizzle[0] != 0 && alu->src[i].src.ssa->num_components == 1)
         return false;
      if (!offset_known(alu->src[i].src.ssa, i == 0 ? &a : &b, depth + 1))
         return false;
   }
   uint64_t mask = d->bit_size >= 64 ? ~0ull : (1ull << d->bit_size) - 1;
   switch (alu->op) {
   case nir_op_iadd: *out = (a + b) & mask; return true;
   case nir_op_imul: *out = (a * b) & mask; return true;
   case nir_op_ishl: *out = (a << (b & 63)) & mask; return true;
   case nir_op_mov:
   case nir_op_u2u32: case nir_op_u2u64: case nir_op_i2i32: case nir_op_i2i64:
      *out = a & mask; return true;
   default:
      return false;
   }
}

/*
 * A scalar known while translating: a constant, or an ALU instruction over scalars that are
 * (nir_builder does not fold). What decides a branch of QuartzCore's uber shader is a function
 * constant, compared and combined: known here, only the side taken is translated into the CFG,
 * and the shader Mesa optimizes is the specialized one rather than all of it -- a pipeline took
 * one to four seconds to build, the window server waiting (24 October).
 */
static bool scalar_known_component(nir_def *d, unsigned component, nir_const_value *out, unsigned depth);

static bool
scalar_known(nir_def *d, nir_const_value *out, unsigned depth)
{
   if (depth > 24 || d->num_components != 1)
      return false;
   if (d->parent_instr->type == nir_instr_type_load_const) {
      *out = nir_instr_as_load_const(d->parent_instr)->value[0];
      return true;
   }
   if (d->parent_instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(d->parent_instr);
   const nir_op_info *info = &nir_op_infos[alu->op];
   if (info->output_size != 0 || info->num_inputs > 4)
      return false;
   nir_const_value values[4][NIR_MAX_VEC_COMPONENTS];
   nir_const_value *srcs[4];
   for (unsigned i = 0; i < info->num_inputs; i++) {
      nir_def *s = alu->src[i].src.ssa;
      nir_const_value v;
      if (info->input_sizes[i] != 0 || !scalar_known_component(s, alu->src[i].swizzle[0], &v, depth + 1))
         return false;
      for (unsigned k = 0; k < NIR_MAX_VEC_COMPONENTS; k++)
         values[i][k] = v;
      srcs[i] = values[i];
   }
   nir_const_value dest[NIR_MAX_VEC_COMPONENTS];
   /* the size an unsized type has here, as nir_opt_constant_folding takes it: the result's,
    * or for a comparison (a sized result) its first unsized source's */
   unsigned bit_size = 0;
   if (!nir_alu_type_get_type_size(info->output_type))
      bit_size = d->bit_size;
   for (unsigned i = 0; !bit_size && i < info->num_inputs; i++)
      if (!nir_alu_type_get_type_size(info->input_types[i]))
         bit_size = alu->src[i].src.ssa->bit_size;
   if (!bit_size)
      bit_size = 32;
   nir_eval_const_opcode(alu->op, dest, 1, bit_size, srcs, 0);
   *out = dest[0];
   return true;
}

/* one component of a def, known while translating */
static bool
scalar_known_component(nir_def *d, unsigned component, nir_const_value *out, unsigned depth)
{
   if (d->num_components == 1)
      return component == 0 && scalar_known(d, out, depth);
   if (d->parent_instr->type == nir_instr_type_load_const && component < d->num_components) {
      *out = nir_instr_as_load_const(d->parent_instr)->value[component];
      return true;
   }
   return false;
}

static int
ptr_slot_offset(const struct air_val *p)
{
   if (!p || p->kind != AIR_VAL_PTR || p->ptr.class != AIR_PTR_SCRATCH)
      return -1;
   if (p->ptr.offset == NULL)
      return 0;
   uint64_t at;
   if (!offset_known(p->ptr.offset, &at, 0) || at > INT32_MAX)
      return -1;
   return (int)at;
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
   if (a->class != b->class || a->binding != b->binding || a->is_image != b->is_image)
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
   unsigned bits = air_nir_bit_size(c, t);
   return glsl_vector_type(bits == 16 ? GLSL_TYPE_FLOAT16 :
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

static struct air_val *air_constant_val(struct air_ctx *c, const struct air_constant *k);

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
         /* Scalar fields stay NONE until insertvalue puts something there; an aggregate
          * field is an aggregate of its own, so that a nested index path (insertvalue
          * [2 x struct] poison, x, 0, 0 -- QuartzCore's uber shader builds its image
          * configs that way) has somewhere to go. */
         for (unsigned i = 0; i < n; i++) {
            const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
            if (et->kind == AIR_TYPE_STRUCT || et->kind == AIR_TYPE_ARRAY) {
               struct air_constant u = { .base = { .kind = AIR_VALUE_CONSTANT, .type = et },
                                         .ckind = k->ckind };
               v->elems[i] = air_constant_val(c, &u);
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

static int scratch_global_at(struct air_ctx *c, nir_def *offset);
static int scratch_global_containing(struct air_ctx *c, uint64_t off);

static nir_def *
ptr_offset(struct air_ctx *c, struct air_ptr ptr)
{
   return ptr.offset ? ptr.offset : nir_imm_int(&c->b, 0);
}

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
   nir_def *res = NULL;
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
   case AIR_PTR_SCRATCH: {
      int g = scratch_global_at(c, ptr.offset);
      nir_def *k = g >= 0 && !c->global_plan[g].written_outside_init ? c->global_plan[g].init_value : NULL;

      if (k && k->num_components == comps && k->bit_size == bits) {
         res = nir_build_imm(&c->b, comps, bits, nir_instr_as_load_const(k->parent_instr)->value);
         break;
      }
      /* a field of such a global, stored once with a constant (init_stores) */
      uint64_t at;
      if (ptr.offset && offset_known(ptr.offset, &at, 0)) {
         int gc = scratch_global_containing(c, at);
         for (unsigned i = 0; gc >= 0 && !c->global_plan[gc].written_outside_init && i < c->num_init_stores; i++) {
            nir_def *v = c->init_stores[i].value;
            if (c->init_stores[i].offset == at && !c->init_stores[i].bad && v &&
                v->num_components == comps && v->bit_size == bits) {
               res = nir_build_imm(&c->b, comps, bits, nir_instr_as_load_const(v->parent_instr)->value);
               break;
            }
         }
         if (res)
            break;
      }
      res = nir_load_scratch(&c->b, comps, bits, off, .align_mul = align);
      break;
   }
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
      if (comps == 1) {
         res = nir_imm_intN_t(&c->b, v->bits, as_bool ? 8 : bits);
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
      if (comps * bits > 64)
         return air_error(c, "interface: a function constant of %u components of %u bits, "
                             "which does not fit the one word air_constant_value carries",
                          comps, bits), NULL;
      nir_def *fields[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < comps; i++) {
         uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
         fields[i] = nir_imm_intN_t(&c->b, (v->bits >> (i * bits)) & mask,
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
      /* Through null: what the uber shaders' `fc_has_x ? buffer : nullptr` leaves in a branch
       * the pipeline's function constants never take (this frontend translates every block,
       * taken or not). Zero, as reading it never happens; a branch that IS taken with a null
       * buffer is a bug of the caller's either way. */
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
   case AIR_PTR_SCRATCH: {
      if (c->in_static_init && value->num_components == 1 &&
          !nir_src_is_const(nir_src_for_ssa(value))) {
         /* a function constant passed through air.normalize_function_constant_predicate and the
          * like is still a constant: stored as one, the copy folds (scalar_known) */
         nir_const_value known;
         bool ok = scalar_known(value, &known, 0);
         if (ok)
            value = nir_build_imm(&c->b, 1, value->bit_size, &known);
      }
      uint64_t at;
      if (c->in_static_init && ptr.offset && offset_known(ptr.offset, &at, 0) &&
          scratch_global_containing(c, at) >= 0) {
         unsigned i;
         for (i = 0; i < c->num_init_stores; i++)
            if (c->init_stores[i].offset == at)
               break;
         bool is_const = nir_src_is_const(nir_src_for_ssa(value));
         if (i == c->num_init_stores && c->num_init_stores < ARRAY_SIZE(c->init_stores)) {
            c->init_stores[i].offset = at;
            c->init_stores[i].value = is_const ? value : NULL;
            c->init_stores[i].bad = !is_const;
            c->num_init_stores++;
         } else if (i < c->num_init_stores) {
            nir_def *old = c->init_stores[i].value;
            if (!is_const || !old || old->bit_size != value->bit_size ||
                old->num_components != value->num_components ||
                memcmp(nir_instr_as_load_const(old->parent_instr)->value,
                       nir_instr_as_load_const(value->parent_instr)->value,
                       sizeof(nir_const_value) * value->num_components))
               c->init_stores[i].bad = true;
         }
      }
      if (c->in_static_init) {
         int g = scratch_global_at(c, ptr.offset);
         if (g >= 0) {
            struct air_global_plan *pl = &c->global_plan[g];
            nir_def *old = pl->init_value;
            bool same = old && old->num_components == value->num_components &&
                        old->bit_size == value->bit_size &&
                        nir_src_is_const(nir_src_for_ssa(value)) &&
                        !memcmp(nir_instr_as_load_const(old->parent_instr)->value,
                                nir_instr_as_load_const(value->parent_instr)->value,
                                sizeof(nir_const_value) * value->num_components);
            /* one constant, however many times stored, is foldable; a second value (an
             * initializer with branches stores different ones on different paths) or a
             * value that is not a constant is not */
            if (!nir_src_is_const(nir_src_for_ssa(value)) || (old && !same))
               pl->written_outside_init = true;
            else
               pl->init_value = value;
         }
      }
      nir_store_scratch(&c->b, value, off, .align_mul = align,
                        .write_mask = BITFIELD_MASK(comps));
      return true;
   }
   case AIR_PTR_UBO:
   case AIR_PTR_CONSTANT_DATA:
      return air_error(c, "instruction: a store into constant memory");
   case AIR_PTR_ARGBUF:
      /* Writing a descriptor from inside the shader. The handle it would write
       * is the runtime's, and this frontend has resolved the set's members to
       * bindings rather than to addresses, so there is nothing to write.
       */
      return air_error(c, "instruction: a store into an argument buffer");
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
   gl_shader_stage stage;
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
   case AIR_SV_POINT_COORD: return nir_load_point_coord(&c->b);
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

static nir_variable *
make_io_var(struct air_ctx *c, nir_variable_mode mode, const struct air_type *t,
            unsigned location, const char *name)
{
   const struct glsl_type *gt;
   unsigned comps = air_nir_components(t);
   unsigned bits = air_nir_bit_size(c, t);
   if (air_type_is_float(air_type_scalar(t)))
      gt = bits == 16 ? glsl_f16vec_type(comps) : glsl_vec_type(comps);
   else
      gt = glsl_vector_type(bits == 16 ? GLSL_TYPE_INT16 : GLSL_TYPE_INT, comps);

   /*
    * A VARYING is 32 bits a component whatever the shader's type, on both sides: the vertex
    * stage widens what it writes and the fragment stage narrows what it reads (as a vertex
    * attribute is fetched, add_vertex_attribute). Panfrost on the Bifrost Mali laid a `half4`
    * varying out as four halves on the side that wrote it and read four floats on the other:
    * a black fill came out (0, 1/128, garbage, garbage) -- every CoreAnimation layer whose
    * colour is a vertex's, the widgets' platters blue (24 October).
    */
   if (bits == 16 && location >= VARYING_SLOT_VAR0 &&
       ((mode == nir_var_shader_out && c->nir->info.stage == MESA_SHADER_VERTEX) ||
        (mode == nir_var_shader_in && c->nir->info.stage == MESA_SHADER_FRAGMENT)))
      gt = air_type_is_float(air_type_scalar(t)) ? glsl_vec_type(comps)
                                                  : glsl_vector_type(GLSL_TYPE_INT, comps);
   nir_variable *var = nir_variable_create(c->nir, mode, gt, name ? name : "io");
   var->data.location = location;
   return var;
}

/* A value into an output whose variable may be wider than the value (a widened varying). */
static void
store_output(struct air_ctx *c, nir_variable *var, nir_def *v)
{
   unsigned want = glsl_get_bit_size(glsl_without_array(var->type));
   if (v->bit_size != want) {
      enum glsl_base_type base = glsl_get_base_type(glsl_without_array(var->type));
      bool is_float = base == GLSL_TYPE_FLOAT || base == GLSL_TYPE_FLOAT16;
      v = is_float ? nir_f2fN(&c->b, v, want) : nir_i2iN(&c->b, v, want);
   }
   nir_store_var(&c->b, var, v, BITFIELD_MASK(v->num_components));
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
                       gl_shader_stage_name(c->nir->info.stage));
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
                          gl_shader_stage_name(c->nir->info.stage));
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
      if (!md_int(node, rt_in + 1, &n))
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
      unsigned in_bits = air_nir_bit_size(c, t);
      if (in_v->bit_size != in_bits)
         in_v = air_type_is_float(air_type_scalar(t)) ? nir_f2fN(&c->b, in_v, in_bits)
                                                      : nir_u2uN(&c->b, in_v, in_bits);
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

   int rt = md_key(node, "air.render_target");
   if (rt >= 0) {
      uint64_t n = 0, dual = 0;
      md_int(node, rt + 1, &n);
      md_int(node, rt + 2, &dual);
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
/* Is this pointer a sampler the shader declares itself (__air_sampler_state)? */
static bool
is_inline_sampler(struct air_ctx *c, const struct air_val *v)
{
   if (!v || v->kind != AIR_VAL_PTR || v->ptr.class != AIR_PTR_SAMPLER)
      return false;
   for (unsigned i = 0; i < c->info->num_resources; i++) {
      const struct air_resource *r = &c->info->resources[i];
      if (r->kind == AIR_RESOURCE_SAMPLER && r->index == v->ptr.binding)
         return r->inline_sampler;
   }
   return false;
}

/*
 * The POINTER a load from `p` gives, known while translating, or NULL (and no
 * error) when it is not: one stored into a local before (AIR_OP_STORE), or one a
 * constant global was made with -- QuartzCore's uber shader falls back on
 * `constant metal::sampler unused_sampler`, a struct whose one member points at
 * the shader's own __air_sampler_state. Through a choice made at run time, both
 * sides; and a choice between a sampler argument and that fallback is the
 * argument: the fallback is taken when there is no texture to sample, where
 * which sampler samples nothing does not matter (QuartzCore's materials, the
 * widgets' platters, 24 October).
 */
static struct air_val *
pointer_loaded(struct air_ctx *c, struct air_val *p)
{
   if (p->kind == AIR_VAL_PTR_SELECT) {
      struct air_val *a = pointer_loaded(c, p->elems[0]);
      struct air_val *b = a ? pointer_loaded(c, p->elems[1]) : NULL;
      if (!a || !b)
         return NULL;
      if (a == b || (a->kind == AIR_VAL_PTR && b->kind == AIR_VAL_PTR && same_ptr(&a->ptr, &b->ptr)))
         return a;
      if (is_inline_sampler(c, a) && !is_inline_sampler(c, b))
         return b;
      if (is_inline_sampler(c, b) && !is_inline_sampler(c, a))
         return a;
      return air_val_ptr_select(c, p->def, a, b);
   }
   int at = ptr_slot_offset(p);
   if (at < 0)
      return NULL;
   for (unsigned i = 0; i < c->num_ptr_slots; i++)
      if (c->ptr_slots[i].offset == (unsigned)at)
         return c->ptr_slots[i].val;
   int g = scratch_global_at(c, nir_imm_int(&c->b, at));
   if (g < 0 || c->global_plan[g].written_outside_init)
      return NULL;
   const struct air_value *init = c->m->globals[g]->init;
   const struct air_constant *k = init ? air_value_as_constant(init) : NULL;
   if (k && k->ckind == AIR_CONST_AGGREGATE && k->num_elems > 0)
      init = k->elems[0];
   /* through the casts a constant may wear (`ptr addrspace(2) @__air_sampler_state` is
    * written as a cast of the global in QuartzCore's modules) */
   for (unsigned depth = 0; init && init->kind == AIR_VALUE_CONSTANT && depth < 4; depth++) {
      const struct air_constant *e = air_value_as_constant(init);
      if (!e || e->ckind != AIR_CONST_EXPR || e->num_operands < 1)
         break;
      if (e->expr_op == AIR_OP_GEP) {
         /* a getelementptr whose indices are all zero is its base */
         bool zero = true;
         for (unsigned o = 1; o < e->num_operands; o++) {
            uint64_t v;
            if (!air_const_uint(e->operands[o], &v) || v != 0)
               zero = false;
         }
         if (!zero)
            break;
      } else if (e->expr_op != AIR_OP_CAST) {
         break;
      }
      init = e->operands[0];
   }
   if (!init || init->kind != AIR_VALUE_GLOBAL)
      return NULL;
   return air_get(c, init);
}

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
      p = air_ptr_add(c, p, nir_imul_imm(&c->b, nir_u2u32(&c->b, first), stride));
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
         p = air_ptr_add(c, p, nir_imul_imm(&c->b, nir_u2u32(&c->b, idx), es));
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
   bool was_exact = c->b.exact;
   if (is_float)
      c->b.exact = !(in->flags & (AIR_FMF_UNSAFE_ALGEBRA | AIR_FMF_REASSOC));

   switch (in->sub) {
   case AIR_BINOP_ADD: res = is_float ? nir_fadd(&c->b, x, y) : nir_iadd(&c->b, x, y); break;
   case AIR_BINOP_SUB: res = is_float ? nir_fsub(&c->b, x, y) : nir_isub(&c->b, x, y); break;
   case AIR_BINOP_MUL: res = is_float ? nir_fmul(&c->b, x, y) : nir_imul(&c->b, x, y); break;
   case AIR_BINOP_UDIV: res = is_float ? nir_fdiv(&c->b, x, y) : nir_udiv(&c->b, x, y); break;
   case AIR_BINOP_SDIV: res = is_float ? nir_fdiv(&c->b, x, y) : nir_idiv(&c->b, x, y); break;
   case AIR_BINOP_UREM: res = is_float ? nir_frem(&c->b, x, y) : nir_umod(&c->b, x, y); break;
   case AIR_BINOP_SREM: res = is_float ? nir_frem(&c->b, x, y) : nir_irem(&c->b, x, y); break;
   case AIR_BINOP_SHL: res = nir_ishl(&c->b, x, nir_u2u32(&c->b, y)); break;
   case AIR_BINOP_LSHR: res = nir_ushr(&c->b, x, nir_u2u32(&c->b, y)); break;
   case AIR_BINOP_ASHR: res = nir_ishr(&c->b, x, nir_u2u32(&c->b, y)); break;
   case AIR_BINOP_AND: res = nir_iand(&c->b, x, y); break;
   case AIR_BINOP_OR: res = nir_ior(&c->b, x, y); break;
   case AIR_BINOP_XOR: res = nir_ixor(&c->b, x, y); break;
   default:
      c->b.exact = was_exact;
      return air_error(c, "instruction: binary operator %u", in->sub);
   }
   c->b.exact = was_exact;
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
   /* Two integer constants -- a function constant against a number, most of all -- are
    * compared now, so that what depends on the answer (a select between a texture and
    * texture2d<T>(), or between two samplers) can be chosen while translating. */
   if (in->sub >= AIR_ICMP_EQ && x->num_components == 1 && y->num_components == 1 &&
       nir_src_is_const(nir_src_for_ssa(x)) && nir_src_is_const(nir_src_for_ssa(y))) {
      uint64_t ux = nir_src_as_uint(nir_src_for_ssa(x)), uy = nir_src_as_uint(nir_src_for_ssa(y));
      int64_t sx = nir_src_as_int(nir_src_for_ssa(x)), sy = nir_src_as_int(nir_src_for_ssa(y));
      bool r;
      switch (in->sub) {
      case AIR_ICMP_EQ: r = ux == uy; break;
      case AIR_ICMP_NE: r = ux != uy; break;
      case AIR_ICMP_UGT: r = ux > uy; break;
      case AIR_ICMP_UGE: r = ux >= uy; break;
      case AIR_ICMP_ULT: r = ux < uy; break;
      case AIR_ICMP_ULE: r = ux <= uy; break;
      case AIR_ICMP_SGT: r = sx > sy; break;
      case AIR_ICMP_SGE: r = sx >= sy; break;
      case AIR_ICMP_SLT: r = sx < sy; break;
      case AIR_ICMP_SLE: r = sx <= sy; break;
      default: goto not_folded;
      }
      air_set(c, &in->base, air_val_ssa(c, nir_imm_bool(&c->b, r)));
      return true;
   }
not_folded:;
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
         /* a condition known now -- a function constant -- chooses now: a texture or a
          * sampler cannot be chosen at run time, and the uber shaders choose between the
          * real texture and texture2d<T>() exactly this way */
         if (nir_src_is_const(nir_src_for_ssa(pc))) {
            air_set(c, &in->base, nir_src_as_bool(nir_src_for_ssa(pc)) ? pa : pb);
            return true;
         }
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
      air_set(c, &in->base, air_val_ssa(c, nir_vector_extract(&c->b, v, nir_u2u32(&c->b, i))));
      return true;
   }
   case AIR_OP_INSERTELT: {
      nir_def *v = air_get_ssa(c, in->operands[0]);
      nir_def *x = air_get_ssa(c, in->operands[1]);
      nir_def *i = air_get_ssa(c, in->operands[2]);
      if (!v || !x || !i)
         return false;
      /* a run-time index is a select per component (nir_vector_insert);
       * QuartzCore's ...Tc3b... draw shaders insert that way */
      if (!nir_src_is_const(nir_src_for_ssa(i))) {
         air_set(c, &in->base,
                 air_val_ssa(c, nir_vector_insert(&c->b, v, x, nir_u2u32(&c->b, i))));
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
         /* a field of an undef aggregate */
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
      if (p->kind == AIR_VAL_PTR_SELECT && in->base.type->kind == AIR_TYPE_POINTER) {
         struct air_val *v = pointer_loaded(c, p);
         if (!v)
            return c->error[0] ? false : air_error(c, "instruction: a pointer loaded from "
                                                      "memory chosen at run time");
         air_set(c, &in->base, v);
         return true;
      }
      if (p->kind == AIR_VAL_PTR_SELECT) {
         nir_def *res = load_through_choice(c, p, in->base.type, in->align);
         if (!res)
            return false;
         air_set(c, &in->base, air_val_ssa(c, res));
         return true;
      }
      if (p->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a load from something that is not a pointer");
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
      if (in->base.type->kind == AIR_TYPE_POINTER) {
         struct air_val *v = pointer_loaded(c, p);
         if (v) {
            air_set(c, &in->base, v);
            return true;
         }
         if (c->error[0])
            return false;
         /* what was not known, for the refusal to say */
         int at = ptr_slot_offset(p), g = at >= 0 ? scratch_global_at(c, nir_imm_int(&c->b, at)) : -1;
         const struct air_value *init = g >= 0 ? c->m->globals[g]->init : NULL;
         const struct air_constant *k = init ? air_value_as_constant(init) : NULL;
         return air_error(c, "ptr load: cl %u at %d slots %u g %d %.24s w%d vk %d/%d e%d",
                          p->ptr.class, at, c->num_ptr_slots, g,
                          g >= 0 && c->m->globals[g]->base.name ? c->m->globals[g]->base.name : "-",
                          g >= 0 ? c->global_plan[g].written_outside_init : -1,
                          init ? (int)init->kind : -1, k ? (int)k->ckind : -1,
                          k && k->num_elems > 0 && k->elems[0] ? (int)k->elems[0]->kind : -1);
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
      /* A POINTER stored into a local: QuartzCore's uber shader keeps each
       * sampler argument in a metal::sampler of its own (an alloca holding the
       * argument) and loads it back where it samples. The local is a slot
       * known while translating, and a load of it answers the pointer stored --
       * sound while one pointer only is ever stored there, which is checked. */
      if (val_is_pointerish(sv)) {
         int at = ptr_slot_offset(p);
         if (at < 0)
            return air_error(c, "instruction: a store of a pointer into memory, which this "
                                "frontend has no address to write");
         for (unsigned i = 0; i < c->num_ptr_slots; i++) {
            if (c->ptr_slots[i].offset == (unsigned)at) {
               if (c->ptr_slots[i].val == sv ||
                   (sv->kind == AIR_VAL_PTR && c->ptr_slots[i].val->kind == AIR_VAL_PTR &&
                    same_ptr(&sv->ptr, &c->ptr_slots[i].val->ptr)))
                  return true;
               return air_error(c, "instruction: two different pointers stored into one local");
            }
         }
         if (c->num_ptr_slots == ARRAY_SIZE(c->ptr_slots))
            return air_error(c, "instruction: more pointers stored into locals than %u",
                             (unsigned)ARRAY_SIZE(c->ptr_slots));
         c->ptr_slots[c->num_ptr_slots].offset = at;
         c->ptr_slots[c->num_ptr_slots].val = sv;
         c->num_ptr_slots++;
         return true;
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

/* -- the CFG ------------------------------------------------------------ */

static nir_block *
nir_block_of(struct air_ctx *c, const struct air_block *b)
{
   struct hash_entry *e = _mesa_hash_table_search(c->blocks, b);
   if (e)
      return e->data;
   nir_block *n = nir_block_create(c->nir);
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
      if (v->kind != AIR_VAL_AGG || v->num_elems != c->num_ret_fields)
         return air_error(c, "instruction: a returned struct that was never built, "
                             "from a function called in the module");
      for (unsigned i = 0; i < v->num_elems; i++) {
         if (!c->ret_fields[i] || !v->elems[i])
            continue;
         if (v->elems[i]->kind != AIR_VAL_SSA)
            return air_error(c, "instruction: field %u of a returned struct is not a "
                                "number, from a function called in the module", i);
         nir_def *d = v->elems[i]->def;
         nir_store_deref(&c->b, c->ret_fields[i], d,
                         nir_component_mask(d->num_components));
      }
      return true;
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
         if (!c->outputs[i].var)
            return air_error(c, "interface: field %u of the result has no output", i);
         if (!v->elems[i] || v->elems[i]->kind != AIR_VAL_SSA)
            continue; /* a field left undef is a target nothing writes */
         store_output(c, c->outputs[i].var, v->elems[i]->def);
      }
      return true;
   }
   if (v->kind != AIR_VAL_SSA)
      return air_error(c, "instruction: a returned value that is not a number");
   if (!c->num_outputs || !c->outputs[0].var)
      return air_error(c, "interface: a returned value with no output");
   store_output(c, c->outputs[0].var, v->def);
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

/* A pointer argument whose byte offset is computed at run time: the offset is
 * a def of the CALLER's impl, so it travels as one more parameter, after the
 * ordinary ones. A constant offset needs none -- the callee builds the same
 * immediate itself, which also keeps an argument buffer's member resolvable.
 */
static bool
ptr_offset_is_param(const struct air_val *v)
{
   return v && v->kind == AIR_VAL_PTR && v->ptr.offset &&
          v->ptr.offset->parent_instr->type != nir_instr_type_load_const;
}

/* The extra parameters ptr_offset_is_param asks for, appended to a call. */
static void
call_add_ptr_offsets(nir_call_instr *call, unsigned *idx, unsigned n,
                     struct air_val **argv, const bool *is_param)
{
   for (unsigned i = 0; i < n; i++)
      if (!is_param[i] && ptr_offset_is_param(argv[i]))
         call->params[(*idx)++] = nir_src_for_ssa(argv[i]->ptr.offset);
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
static nir_function *
translate_callee(struct air_ctx *c, const struct air_function *f,
                 struct air_val **argv, bool *is_param)
{
   /* A texture, sampler or buffer argument is not a value in this frontend: it
    * is resolved at translation time into a binding, and tex_handle wants an
    * AIR_VAL_PTR rather than an SSA def. Such an argument is bound from the
    * caller's own resolution and takes no NIR parameter, which specialises the
    * callee to its call site -- so it is not cached. Each of the four modules
    * that needs this has exactly one call site, counted beforehand.
    *
    * A pointer carrying a byte offset computed at run time passes that offset
    * as a parameter of its own (ptr_offset_is_param): using the caller's def
    * here would be the same cross-function def/use that the globals used to
    * cause. QuartzCore's widget and blur shaders (…Tmua…, …Tmap…) are that
    * shape, and refusing them took backboardd down with them.
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
      /* The same reason, for the two pointers that are values: a chosen
       * pointer's condition is a def of the CALLER's impl, and a slot's
       * soundness rests on every use being in one block of one function.
       */
      if (argv && argv[i] && argv[i]->kind == AIR_VAL_PTR_SELECT) {
         air_error(c, "intrinsic: a call passing a pointer chosen at run time as "
                      "argument %u", i);
         return NULL;
      }
      is_param[i] = !ptr;
      specialised |= ptr;
   }

   struct hash_entry *e = specialised ? NULL : _mesa_hash_table_search(c->fns, f);
   if (e)
      return e->data;

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
   if (ret_struct) {
      for (unsigned i = 0; i < rt->num_members; i++)
         nret += ret_member_is_number(rt->members[i]);
   } else if (has_ret) {
      nret = 1;
   }
   unsigned noffsets = 0;
   for (unsigned i = 0; i < f->num_args; i++)
      noffsets += argv && !is_param[i] && ptr_offset_is_param(argv[i]);
   unsigned nparams = f->num_args + nret + noffsets;
   nir_function *fn = nir_function_create(c->nir, f->base.name ? f->base.name : "callee");
   fn->num_params = nparams;
   fn->params = rzalloc_array(c->nir, nir_parameter, MAX2(nparams, 1));
   fn->should_inline = true;

   unsigned idx = 0;
   const struct glsl_type *ret_glsl = NULL;
   if (ret_struct) {
      for (unsigned i = 0; i < rt->num_members; i++) {
         if (!ret_member_is_number(rt->members[i]))
            continue;
         fn->params[idx++] = (nir_parameter){
            .num_components = 1, .bit_size = 32,
            .is_return = true,
            .mode = nir_var_function_temp,
            .type = param_glsl_type(c, rt->members[i]),
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
      fn->params[idx++] = (nir_parameter){
         .num_components = (uint8_t)air_nir_components(at),
         .bit_size = (uint8_t)air_nir_bit_size(c, at),
      };
   }
   for (unsigned i = 0; i < noffsets; i++)
      fn->params[idx++] = (nir_parameter){ .num_components = 1, .bit_size = 32 };
   fn->num_params = idx;   /* the pointers took no parameter, only their offsets */

   /* cached before the body is translated, so that a callee which calls itself
    * meets its own declaration rather than being translated for ever */
   if (!specialised)
      _mesa_hash_table_insert(c->fns, f, fn);
   else
      _mesa_hash_table_insert(c->fns, fn, fn);

   nir_builder saved_b = c->b;
   const struct air_function *saved_fn = c->fn;
   struct hash_table *sv = c->values, *sb = c->blocks,
                     *sp = c->phi_cursors, *sr = c->phi_regs;
   nir_deref_instr *saved_ret = c->ret_deref;
   nir_deref_instr **saved_ret_fields = c->ret_fields;
   unsigned saved_num_ret_fields = c->num_ret_fields;
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
      c->ret_fields = rzalloc_array(c->mem_ctx, nir_deref_instr *,
                                    MAX2(rt->num_members, 1));
      c->num_ret_fields = rt->num_members;
      unsigned p = 0;
      for (unsigned i = 0; i < rt->num_members; i++) {
         if (!ret_member_is_number(rt->members[i]))
            continue;
         c->ret_fields[i] =
            nir_build_deref_cast(&c->b, nir_load_param(&c->b, p++),
                                 nir_var_function_temp,
                                 param_glsl_type(c, rt->members[i]), 0);
      }
   } else if (has_ret) {
      c->ret_deref = nir_build_deref_cast(&c->b, nir_load_param(&c->b, 0),
                                          nir_var_function_temp, ret_glsl, 0);
   }
   unsigned pidx = nret;
   for (unsigned i = 0; i < f->num_args && ok; i++) {
      if (!is_param[i]) {
         air_set(c, &f->args[i], argv[i]);   /* the caller's own resolution */
         continue;
      }
      air_set(c, &f->args[i], air_val_ssa(c, nir_load_param(&c->b, pidx++)));
   }
   /* the pointers with an offset, rebuilt here with this impl's own offset */
   for (unsigned i = 0; i < f->num_args && ok; i++) {
      if (!argv || is_param[i] || !argv[i] || argv[i]->kind != AIR_VAL_PTR ||
          !argv[i]->ptr.offset)
         continue;
      struct air_val *v = ralloc(c->mem_ctx, struct air_val);
      *v = *argv[i];
      if (ptr_offset_is_param(argv[i])) {
         v->ptr.offset = nir_load_param(&c->b, pidx++);
      } else {
         nir_load_const_instr *k = nir_instr_as_load_const(argv[i]->ptr.offset->parent_instr);
         v->ptr.offset = nir_imm_int(&c->b, (int)k->value[0].u32);
      }
      air_set(c, &f->args[i], v);
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
   bool *is_param = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_args, 1));
   for (unsigned i = 0; i < f->num_args; i++) {
      argv[i] = air_get(c, in->operands[i + 1]);
      if (!argv[i])
         return false;
   }

   nir_function *fn = translate_callee(c, f, argv, is_param);
   if (!fn)
      return false;   /* translate_callee has said why */

   const struct air_type *rt = f->fn_type->elem;   /* FUNCTION: the return type */
   bool has_ret = rt && rt->kind != AIR_TYPE_VOID;
   bool ret_struct = has_ret && rt->kind == AIR_TYPE_STRUCT;

   nir_variable *tmp = NULL;
   /* One local per member for a struct return, in the member order
    * translate_callee made its parameters from; the ones with no parameter stay
    * NULL and the aggregate's element for them stays NULL too, which is the
    * same thing an extractvalue of an undef field already reads.
    */
   nir_variable **fields = ret_struct ?
      rzalloc_array(c->mem_ctx, nir_variable *, MAX2(rt->num_members, 1)) : NULL;
   nir_call_instr *call = nir_call_instr_create(c->nir, fn);
   unsigned idx = 0;
   if (ret_struct) {
      for (unsigned i = 0; i < rt->num_members; i++) {
         if (!ret_member_is_number(rt->members[i]))
            continue;
         fields[i] = nir_local_variable_create(c->b.impl,
                                               param_glsl_type(c, rt->members[i]),
                                               "callret");
         call->params[idx++] =
            nir_src_for_ssa(&nir_build_deref_var(&c->b, fields[i])->def);
      }
   } else if (has_ret) {
      tmp = nir_local_variable_create(c->b.impl, param_glsl_type(c, rt), "callret");
      call->params[idx++] = nir_src_for_ssa(&nir_build_deref_var(&c->b, tmp)->def);
   }
   /* operand 0 is the callee; the arguments follow it */
   for (unsigned i = 0; i < f->num_args; i++) {
      if (!is_param[i])
         continue;
      nir_def *a = air_get_ssa(c, in->operands[i + 1]);
      if (!a)
         return air_error(c, "intrinsic: argument %u of %s is not a value this can pass",
                          i, name);
      call->params[idx++] = nir_src_for_ssa(a);
   }
   call_add_ptr_offsets(call, &idx, f->num_args, argv, is_param);
   nir_builder_instr_insert(&c->b, &call->instr);

   if (ret_struct) {
      struct air_val *agg = air_val_agg(c, rt->num_members);
      for (unsigned i = 0; i < rt->num_members; i++)
         if (fields[i])
            agg->elems[i] = air_val_ssa(c, nir_load_var(&c->b, fields[i]));
      air_set(c, &in->base, agg);
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

   nir_function *fn = translate_callee(c, f, argv, is_param);
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
   call_add_ptr_offsets(call, &idx, f->num_args, argv, is_param);
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
static bool
resolve_pointer_phi(struct air_ctx *c, const struct air_inst *in, const bool *reachable)
{
   struct air_val *first = NULL;
   for (unsigned k = 0; k < in->num_operands; k++) {
      if (!in->targets[k] || !reachable[in->targets[k]->index])
         continue;
      struct hash_entry *e = _mesa_hash_table_search(c->values, in->operands[k]);
      if (!e)
         return air_error(c, "instruction: a phi of pointers whose operand comes in over "
                             "a loop edge, so which resource it is depends on the "
                             "iteration");
      struct air_val *v = e->data;
      if (!v || v->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a phi of pointers one of whose operands is "
                             "%s", v && v->kind == AIR_VAL_PTR_SELECT
                                      ? "itself a pointer chosen at run time"
                                      : "not a resolved pointer");
      /* texture2d<T>() on one path and the real texture on another is the uber shaders'
       * "a texture if the pipeline has one": the null one is never sampled (a sample of it
       * is refused where it happens), so the phi is the real texture */
      if (v->ptr.class == AIR_PTR_TEXTURE && v->ptr.binding == AIR_NULL_TEXTURE_BINDING) {
         if (!first)
            first = v;
         continue;
      }
      if (!first || (first->ptr.class == AIR_PTR_TEXTURE &&
                     first->ptr.binding == AIR_NULL_TEXTURE_BINDING))
         first = v;
      else if (!same_ptr(&first->ptr, &v->ptr))
         return air_error(c, "instruction: a phi between pointers that name different "
                             "places, which would have to be chosen at run time");
   }
   if (!first)
      return air_error(c, "instruction: a phi of pointers with no reachable operand");
   air_set(c, &in->base, first);
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
         /* A phi of an AGGREGATE, named here rather than by whatever notices
          * the consequence further down. A struct is a tree of values in this
          * translator and never a NIR value, so there is no register to carry
          * one across an edge; the honest form would be one phi per member,
          * which this does not build. Until it was named, a register of ZERO
          * bits was declared for it and the module failed later with
          * "a returned struct that was never built" -- true of the module, and
          * not what is wrong with it.
          */
         if (in->base.type->kind == AIR_TYPE_STRUCT ||
             in->base.type->kind == AIR_TYPE_ARRAY)
            return air_error(c, "instruction: a phi of %s, which NIR has no register for",
                             in->base.type->kind == AIR_TYPE_STRUCT ? "a struct"
                                                                    : "an array");
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

   /* LIVE: reached by an edge taken while translating -- a branch decided now (scalar_known)
    * leaves its other side unreached, and that side is not translated at all */
   bool *live = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_blocks, 1));
   live[0] = true;

   for (unsigned oi = 0; oi < num_order; oi++) {
      unsigned bi = order[oi];
      if (!live[bi])
         continue;
      const struct air_block *blk = &f->blocks[bi];
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
      int only = -1;            /* the one target taken, when the branch was decided now */
      switch (term->op) {
      case AIR_OP_RET:
         if (!emit_return(c, term))
            return false;
         nir_goto(&c->b, c->b.impl->end_block);
         break;
      case AIR_OP_BR:
         if (term->num_targets == 1) {
            nir_goto(&c->b, nir_block_of(c, term->targets[0]));
         } else {
            nir_def *cond = air_get_ssa(c, term->operands[0]);
            if (!cond)
               return false;
            nir_const_value known;
            if (term->targets[0] == term->targets[1])
               nir_goto(&c->b, nir_block_of(c, term->targets[0]));
            else if (cond->num_components == 1 && scalar_known(cond, &known, 0)) {
               /* decided now: only the side taken (scalar_known) */
               only = nir_const_value_as_bool(known, cond->bit_size) ? 0 : 1;
               nir_goto(&c->b, nir_block_of(c, term->targets[only]));
            }
            else
               nir_goto_if(&c->b, nir_block_of(c, term->targets[0]), cond,
                           nir_block_of(c, term->targets[1]));
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
         nir_block *deflt = nir_block_of(c, term->targets[0]);
         unsigned n = term->num_targets - 1;
         for (unsigned k = 0; k < n; k++) {
            nir_def *val = air_get_ssa(c, term->operands[1 + k]);
            if (!val)
               return false;
            nir_block *dest = nir_block_of(c, term->targets[1 + k]);
            bool last = k + 1 == n;
            nir_block *next = deflt;
            if (!last) {
               next = nir_block_create(c->nir);
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
      for (unsigned k = 0; k < term->num_targets; k++)
         if (term->targets[k] && (only < 0 || (int)k == only))
            live[term->targets[k]->index] = true;
   }

   /* the phi stores, now that every value exists */
   for (unsigned bi = 0; bi < f->num_blocks; bi++) {
      const struct air_block *blk = &f->blocks[bi];
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

         uint64_t word = 0;
         const struct air_constant *agg = air_value_as_constant(g->init);
         /* Newer compilers (AIR 2.8, the macOS QuartzCore's library) write the state as an
          * aggregate whose first member is the same 64-bit word -- a struct of the word and
          * the LOD bias, or an array of words -- so the word is taken from there. */
         if (agg && agg->ckind == AIR_CONST_AGGREGATE && agg->num_elems > 0 &&
             air_const_uint(agg->elems[0], &word)) {
            /* taken */
         } else if (agg && agg->ckind == AIR_CONST_DATA && agg->data) {
            word = agg->data[0];
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
      };
      air_set(c, &g->base, air_val_ptr(c, p));
   }
}

/* Which program-scope globals anything but air.static_init stores into. Newer compilers
 * (AIR 2.8) copy every function constant into such a global in the initializer and read the
 * copy everywhere else; a copy nothing else writes holds the constant for the whole
 * invocation, so a load of it can BE the constant (air_load_ptr), and what branches or selects
 * on it -- a texture against texture2d<T>(), one sampler against another -- is decided while
 * translating, which a texture or a sampler has to be. */
static void
mark_globals_written_outside_init(struct air_ctx *c)
{
   for (unsigned i = 0; i < c->m->num_functions; i++) {
      const struct air_function *f = c->m->functions[i];
      if (f->is_declaration || (f->section && !strcmp(f->section, "air.static_init")))
         continue;
      for (unsigned b = 0; b < f->num_blocks; b++) {
         for (unsigned k = 0; k < f->blocks[b].num_insts; k++) {
            const struct air_inst *in = f->blocks[b].insts[k];
            if (in->op != AIR_OP_STORE && in->op != AIR_OP_CALL)
               continue;
            /* a store's pointer, or any global handed to a call, may be written */
            for (unsigned o = in->op == AIR_OP_STORE ? 1 : 0; o < in->num_operands; o++) {
               const struct air_value *v = in->operands[o];
               if (!v || v->kind != AIR_VALUE_GLOBAL)
                  continue;
               for (unsigned g = 0; g < c->m->num_globals; g++)
                  if (&c->m->globals[g]->base == v)
                     c->global_plan[g].written_outside_init = true;
               if (in->op == AIR_OP_STORE)
                  break;
            }
         }
      }
   }
}

/* the global whose planned scratch slot CONTAINS this byte offset, or -1 */
static int
scratch_global_containing(struct air_ctx *c, uint64_t off)
{
   for (unsigned g = 0; g < c->m->num_globals; g++) {
      const struct air_global_plan *pl = &c->global_plan[g];
      if (pl->skip || pl->class != AIR_PTR_SCRATCH || !pl->has_offset)
         continue;
      uint64_t size = air_type_alloc_size(c->m, c->m->globals[g]->value_type);
      if (off >= pl->offset && off < pl->offset + size)
         return g;
   }
   return -1;
}

/* the global whose planned scratch slot is at this constant offset, or -1 */
static int
scratch_global_at(struct air_ctx *c, nir_def *offset)
{
   uint64_t off;
   /* (through the iadds a getelementptr leaves: offset_known) */
   if (!offset || !offset_known(offset, &off, 0))
      return -1;
   for (unsigned g = 0; g < c->m->num_globals; g++) {
      const struct air_global_plan *pl = &c->global_plan[g];
      if (!pl->skip && pl->class == AIR_PTR_SCRATCH && pl->has_offset && pl->offset == off)
         return g;
   }
   return -1;
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
         c->in_static_init = true;
         nir_function *fn = translate_callee(c, f, NULL, NULL);
         c->in_static_init = false;
         if (!fn)
            return;   /* translate_callee has said why */
         nir_builder_instr_insert(&c->b, &nir_call_instr_create(c->nir, fn)->instr);
         continue;
      }
      /* its instructions, in place, without its ret */
      const struct air_block *blk = &f->blocks[0];
      c->in_static_init = true;
      for (unsigned k = 0; k < blk->num_insts; k++) {
         const struct air_inst *in = blk->insts[k];
         if (in->op == AIR_OP_RET)
            continue;
         if (!translate_instruction(c, in)) {
            c->in_static_init = false;
            return;
         }
      }
      c->in_static_init = false;
   }
}

/* -- the entry point ---------------------------------------------------- */

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
   c.m = air_module_read(ctx, bitcode, size, &read_error);
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

   c.nir = nir_shader_create(mem_ctx, stage, nir_options, NULL);
   c.nir->info.name = ralloc_strdup(c.nir, fn->base.name ? fn->base.name : "air");
   c.nir->info.internal = false;
   c.fns = _mesa_pointer_hash_table_create(ctx);
   nir_function *main = nir_function_create(c.nir, "main");
   main->is_entrypoint = true;
   c.b = nir_builder_at(nir_before_impl(nir_function_impl_create(main)));
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
      mark_globals_written_outside_init(&c);
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

   nir_lower_goto_ifs(c.nir);

   /* Fold in any function of the module the shader called, and then drop the
    * callees. This has to come after nir_lower_goto_ifs and not before: this
    * translator emits an unstructured CFG, and nir_inline_functions walks the
    * control-flow tree, which asserts on one. The pass order inside is the one
    * nir_functions.c prescribes -- inline, then nir_opt_deref to undo the cast
    * a pointer parameter leaves behind, then delete everything but the entry
    * point, which every consumer of this NIR assumes.
    */
   if (_mesa_hash_table_num_entries(c.fns)) {
      NIR_PASS(_, c.nir, nir_inline_functions);
      NIR_PASS(_, c.nir, nir_opt_deref);
      nir_remove_non_entrypoints(c.nir);
      NIR_PASS(_, c.nir, nir_lower_variable_initializers, nir_var_function_temp);
      NIR_PASS(_, c.nir, nir_split_var_copies);
      NIR_PASS(_, c.nir, nir_lower_var_copies);
      NIR_PASS(_, c.nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, c.nir, nir_opt_dce);
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
   }
   ralloc_free(ctx);
   if (error)
      *error = NULL;
   return c.nir;
}
