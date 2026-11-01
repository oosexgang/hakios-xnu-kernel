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

#include "nir_constant_expressions.h"
#include "util/hash_table.h"
#include "util/u_math.h"

bool
air_error(struct air_ctx *c, const char *fmt, ...)
{
   if (!c->error[0]) {
      va_list ap;
      va_start(ap, fmt);
      int n = vsnprintf(c->error, sizeof(c->error), fmt, ap);
      va_end(ap);

      /* A REFUSAL THAT WAS CUT SAYS SO.
       *
       * This buffer is 256 bytes (air_private.h) and vsnprintf stops there in
       * silence, so a longer sentence used to arrive mid-word with nothing to
       * show for the rest -- which is this project's cardinal defect wearing a
       * disguise: a message that reads as complete and is not. It is not
       * hypothetical. The tile-shading refusal was written long enough to name
       * both of its walls and every report printed it ending at
       * "-featureProfile", so the half that said which wall decides was the
       * half that vanished, and nothing anywhere said a word was missing.
       *
       * The marker carries the length vsnprintf WANTED, because that is the
       * number a reader can act on: it says how much is gone and therefore
       * whether the sentence should be shortened or the buffer grown. Widening
       * the buffer is the other fix and is not this one -- air_ctx is in a
       * header every object of the frontend includes, and a marker costs
       * nothing to anyone who is not already over the limit.
       */
      if (n >= (int)sizeof(c->error)) {
         char *tail = c->error + sizeof(c->error) - 20;
         snprintf(tail, 20, " ...[cut, %d]", n);
      }
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

/* -- constants the builder did not fold ---------------------------------- */

/* Is this def a constant expression, and what is its value?
 *
 * nir_builder does NOT fold: nir_build_alu inserts the instruction whatever
 * its sources are (nir_builder.c), so `icmp eq i8 %c, 0` with %c an immediate
 * is an ALU instruction here and nir_src_is_const says no. That matters
 * because the two decisions this is for -- which edge of a branch exists, and
 * which side of a `select` between two pointers is taken -- are made while
 * translating, long before nir_opt_constant_folding runs, and a branch left in
 * is a phi of pointers left unresolved.
 *
 * THE SEMANTICS ARE NIR'S OWN. Every operation is evaluated by
 * nir_eval_const_opcode with this shader's float controls, which is the table
 * and the mode nir_opt_constant_folding itself uses, so nothing here can
 * disagree with the folder that runs after. What this adds to try_fold_alu is
 * only recursion: that one requires every source to be a load_const, which a
 * chain of an immediate and two operations never is.
 */
static bool
def_const(nir_def *def, nir_const_value out[NIR_MAX_VEC_COMPONENTS],
          unsigned depth, unsigned float_controls)
{
   if (!def || depth == 0 || def->num_components > NIR_MAX_VEC_COMPONENTS)
      return false;

   if (def->parent_instr->type == nir_instr_type_load_const) {
      nir_load_const_instr *lc = nir_instr_as_load_const(def->parent_instr);
      for (unsigned i = 0; i < def->num_components; i++)
         out[i] = lc->value[i];
      return true;
   }
   if (def->parent_instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(def->parent_instr);
   const nir_op_info *info = &nir_op_infos[alu->op];
   nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
   nir_const_value *srcs[NIR_ALU_MAX_INPUTS];

   /* the unsized-type rule, copied from try_fold_alu rather than reasoned
    * about: an op whose output type carries no size takes it from the first
    * source that carries none either
    */
   unsigned bit_size = 0;
   if (!nir_alu_type_get_type_size(info->output_type))
      bit_size = alu->def.bit_size;

   for (unsigned i = 0; i < info->num_inputs; i++) {
      nir_const_value v[NIR_MAX_VEC_COMPONENTS];
      if (!def_const(alu->src[i].src.ssa, v, depth - 1, float_controls))
         return false;
      if (bit_size == 0 &&
          !nir_alu_type_get_type_size(info->input_types[i]))
         bit_size = alu->src[i].src.ssa->bit_size;
      for (unsigned j = 0; j < nir_ssa_alu_instr_src_components(alu, i); j++)
         src[i][j] = v[alu->src[i].swizzle[j]];
      srcs[i] = src[i];
   }
   if (bit_size == 0)
      bit_size = 32;

   memset(out, 0, sizeof(nir_const_value) * NIR_MAX_VEC_COMPONENTS);
   nir_eval_const_opcode(alu->op, out, def->num_components, bit_size, srcs,
                         float_controls);
   return true;
}

/* A one-component condition that is decided already. 16 is the recursion
 * limit and not a judgement about shaders: the chains this is for are three
 * instructions long (a load folded to an immediate, an icmp, the branch), and
 * a limit is here so that a pathological module costs a refusal to fold and
 * not a stack.
 */
/* The same question for an integer, which a switch asks of its selector and of
 * every one of its case values. */
static bool
uint_const(struct air_ctx *c, nir_def *def, uint64_t *val)
{
   nir_const_value v[NIR_MAX_VEC_COMPONENTS];
   if (!def || def->num_components != 1)
      return false;
   if (!def_const(def, v, 16, c->nir->info.float_controls_execution_mode))
      return false;
   *val = nir_const_value_as_uint(v[0], def->bit_size);
   return true;
}

static bool
cond_const(struct air_ctx *c, nir_def *def, bool *val)
{
   nir_const_value v[NIR_MAX_VEC_COMPONENTS];
   if (!def || def->num_components != 1)
      return false;
   if (!def_const(def, v, 16, c->nir->info.float_controls_execution_mode))
      return false;
   *val = nir_const_value_as_bool(v[0], def->bit_size);
   return true;
}

/* Two things the module-scope half of this file decides and the INSTRUCTION half
 * needs: plan_global_folding's verdict for one global, and the name and
 * required-ness of a function constant by index. Both are declared here because
 * air_load_ptr and AIR_OP_{LOAD,STORE} come first in this file and the planner
 * last, and a missing prototype in C is an implicit int that compiles with a
 * warning and returns a pointer truncated to one.
 */
static struct air_global_plan *foldable_global_plan(struct air_ctx *c,
                                                    const struct air_value *v);
static const char *function_constant_decl(struct air_ctx *c, unsigned index,
                                          bool *required);

/* -- translation-time slots ---------------------------------------------- */

/* Does this type hold a pointer anywhere inside it? A slot exists for exactly
 * the values that memory cannot hold, and that is what makes one necessary.
 */
static bool
type_has_pointer(const struct air_type *t, unsigned depth)
{
   if (!t || depth == 0)
      return false;
   switch (t->kind) {
   case AIR_TYPE_POINTER:
      return true;
   case AIR_TYPE_ARRAY:
   case AIR_TYPE_VECTOR:
      return type_has_pointer(t->elem, depth - 1);
   case AIR_TYPE_STRUCT:
      for (unsigned i = 0; i < t->num_members; i++)
         if (type_has_pointer(t->members[i], depth - 1))
            return true;
      return false;
   default:
      return false;
   }
}

static unsigned
new_slot(struct air_ctx *c, const struct air_type *type, const struct air_value *init)
{
   c->slots = reralloc(c->mem_ctx, c->slots, struct air_slot, c->num_slots + 1);
   c->slots[c->num_slots] = (struct air_slot) { .type = type, .init = init };
   return c->num_slots++;
}

/* The value at byte offset 0 of a slot, for a load that wants `want`.
 *
 * Offset 0 of a struct or an array is offset 0 of its first element, and again
 * of that element's first element, so descending is the whole of the
 * arithmetic -- but only while the caller is asking for something that is not
 * itself the aggregate. A load of the aggregate gets the aggregate.
 */
static struct air_val *
slot_at_zero(struct air_val *v, const struct air_type *want)
{
   bool want_agg = want && (want->kind == AIR_TYPE_STRUCT ||
                            want->kind == AIR_TYPE_ARRAY);
   while (!want_agg && v && v->kind == AIR_VAL_AGG && v->num_elems)
      v = v->elems[0];
   return v;
}

/* A pointer that IS a slot's base: class SLOT and offset zero. A slot holds a
 * tree of values and not bytes, so an offset into one is not something to
 * resolve -- `gep_into` may well have produced one (a getelementptr past the
 * first member), and the honest answer to that is the refusal at the load.
 */
static bool
slot_base(struct air_ctx *c, const struct air_val *v, unsigned *index)
{
   if (!v || v->kind != AIR_VAL_PTR || v->ptr.class != AIR_PTR_SLOT)
      return false;
   if (v->ptr.offset &&
       (!nir_src_is_const(nir_src_for_ssa(v->ptr.offset)) ||
        nir_src_as_uint(nir_src_for_ssa(v->ptr.offset)) != 0))
      return false;
   if (v->ptr.binding >= c->num_slots)
      return false;
   *index = v->ptr.binding;
   return true;
}

static struct air_val *air_val_agg(struct air_ctx *c, unsigned n);

/* A pointer into a slot at a byte offset the translator knows: class SLOT, a
 * binding in range, and an offset that is constant or absent.
 */
static bool
slot_at_const(struct air_ctx *c, const struct air_val *v, unsigned *index,
              uint64_t *off)
{
   if (!v || v->kind != AIR_VAL_PTR || v->ptr.class != AIR_PTR_SLOT)
      return false;
   if (v->ptr.binding >= c->num_slots)
      return false;
   if (v->ptr.offset) {
      if (!nir_src_is_const(nir_src_for_ssa(v->ptr.offset)))
         return false;
      *off = nir_src_as_uint(nir_src_for_ssa(v->ptr.offset));
   } else {
      *off = 0;
   }
   *index = v->ptr.binding;
   return true;
}

/* THE CELL OF A SLOT'S TREE THAT A BYTE OFFSET AND A TYPE NAME TOGETHER.
 *
 * A slot holds a whole air_val because a pointer in this frontend is a
 * resolution and not bytes (air_private.h, AIR_PTR_SLOT), and until now only
 * offset zero could reach one: a `getelementptr` past the first member
 * produced a byte offset, and a tree of values has no bytes for it to mean.
 * That is still true of a byte offset ALONE, and it is why this takes a type
 * as well. Offset 4 of `{ {ptr}, {ptr}, ptr, <2 x float> }` is the second
 * member, the second member's first member, and nothing else -- but only
 * because the store says it is storing a pointer. The same offset with
 * `<2 x float>` would land nowhere and this returns NULL.
 *
 * So the rule is: descend while the type at hand is not the type asked for,
 * taking at each level the one member or element whose byte range contains the
 * remaining offset, and refuse unless the descent ends exactly on the asked
 * type at offset zero. Types are interned per module, so `==` is the whole
 * comparison. A partial overlap -- half of a pointer, two members at once --
 * never matches and is refused by name at the caller, which is the behaviour
 * this replaces and not a behaviour it adds.
 *
 * What the four `disparity_refinement_*` of CIPortraitBlurStitchableV3 look
 * like, which is what this exists for: CoreImage hands a stitched function a
 * `coreimage::Sampler` flattened into four arguments, and the function's first
 * act is to `alloca` the struct back and store each argument into its field.
 * Three of those fields are a texture, a sampler and a constant buffer -- the
 * things a slot exists to hold -- and the fourth is a float2, which a slot can
 * hold too and could not reach.
 *
 * Levels that nothing has written yet are made here rather than refused, for
 * the reason agg_slot gives: an aggregate whose every field is NONE *is* the
 * undef of that type, so building one changes no value.
 */
static struct air_val **
slot_cell(struct air_ctx *c, unsigned s, uint64_t off, const struct air_type *want)
{
   struct air_val **cell = &c->slots[s].val;
   const struct air_type *t = c->slots[s].type;

   /* A struct nested 32 deep is not a thing Metal's compiler emits; the bound
    * is here so that a cyclic or malformed type cannot spin.
    */
   for (unsigned depth = 0; depth < 32; depth++) {
      if (t && t == want && off == 0)
         return cell;
      if (!t)
         return NULL;

      unsigned index, count;
      const struct air_type *et;
      if (t->kind == AIR_TYPE_STRUCT) {
         count = t->num_members;
         index = count;
         uint64_t base = 0;
         for (unsigned i = 0; i < count; i++) {
            uint64_t mo = air_struct_member_offset(c->m, t, i);
            uint64_t ms = air_type_alloc_size(c->m, t->members[i]);
            if (off >= mo && off < mo + ms) {
               index = i;
               base = mo;
               break;
            }
         }
         if (index == count)
            return NULL;
         et = t->members[index];
         off -= base;
      } else if (t->kind == AIR_TYPE_ARRAY) {
         uint64_t es = air_type_alloc_size(c->m, t->elem);
         if (!es || off / es >= t->count)
            return NULL;
         count = t->count > UINT_MAX ? UINT_MAX : (unsigned)t->count;
         index = (unsigned)(off / es);
         et = t->elem;
         off -= (uint64_t)index * es;
      } else {
         return NULL;      /* a scalar, a vector or a pointer: no further in */
      }

      if (!*cell)
         *cell = air_val_agg(c, count);
      if (!*cell || (*cell)->kind != AIR_VAL_AGG || index >= (*cell)->num_elems)
         return NULL;
      cell = &(*cell)->elems[index];
      t = et;
   }
   return NULL;
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

/* An integer of a width NIR does not have, and how wide it is.
 *
 * Metal's own compiler makes them. A function constant holding a 3-bit field
 * is narrowed with `trunc i32 %x to i3` and then switched on, which is how
 * four of RenderBox's shaders pick a blend mode; `i2` occurs the same way.
 * NIR has 1, 8, 16, 32 and 64 bits and nothing between, so such a value is
 * carried here in the next width up with the low N bits significant and
 * everything above them ZERO -- canonical form.
 *
 * Canonical form makes exactly five things exact, and that is the whole list:
 * a truncation INTO the type (mask the low N bits), a truncation or a
 * zero-extension OUT of it, an unsigned conversion to float, an unsigned or
 * equality comparison, and a switch -- which is equalities. A signed
 * comparison, a sign extension, an addition, a multiplication and a shift all
 * read or write the bits above N and would be wrong. narrow_int_gate refuses
 * everything that is not on the list, by width and by opcode, because a wrong
 * answer that nothing says is wrong is the one failure this frontend must not
 * have.
 */
static unsigned
narrow_int_bits(const struct air_type *t)
{
   if (!t)
      return 0;
   const struct air_type *s = air_type_scalar(t);
   if (!s || s->kind != AIR_TYPE_INT)
      return 0;
   unsigned b = s->bits;
   if (!b || b > 64 || b == 1 || b == 8 || b == 16 || b == 32 || b == 64)
      return 0;
   return b;
}

/* The NIR width such a value is carried in: 32 bits, or 64 for the widths
 * above it.
 *
 * NOT the next width up, which was the first choice and is worse for two
 * reasons measured rather than guessed. An `i3` carried in eight bits makes
 * `trunc i32 %x to i3` a `u2u8` followed by a mask, and nir_to_tgsi has no
 * u2u8 at all -- softpipe aborts on it ("Unknown NIR opcode: u2u8"), which
 * removes the one reference on this machine that RUNS the shader rather than
 * folding it. At 32 bits the truncation of a 32-bit word is the mask alone,
 * which is fewer instructions as well as portable. Canonical form is exactly
 * as exact in 32 bits as in 8: the low N bits significant, the rest zero.
 */
static unsigned
narrow_promoted_bits(unsigned n)
{
   return n <= 32 ? 32 : 64;
}

static uint64_t
narrow_mask(unsigned n)
{
   return n >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << n) - 1;
}

/* The width a value of this type is CARRIED in, promoting a width NIR has no
 * register for instead of refusing it.
 *
 * Only for the places narrow_int_gate governs the uses of -- a constant, a
 * truncation into the type, a phi's register. air_nir_bit_size stays the
 * default everywhere else and stays a refusal, because a type that needs a
 * register whose upper bits MEAN something has no NIR width and promoting it
 * would be wrong rather than inconvenient.
 */
static unsigned
air_narrow_bits(struct air_ctx *c, const struct air_type *t)
{
   unsigned n = narrow_int_bits(t);
   return n ? narrow_promoted_bits(n) : air_nir_bit_size(c, t);
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
         /* Reaching HERE, and not narrow_int_gate, means the width turned up
          * somewhere that needs a register of its own -- a function parameter,
          * a shader output, a phi, a value stored to memory -- rather than in
          * an instruction whose low N bits are all it reads. Canonical form
          * (narrow_int_bits) does not help there: the bits above N would have
          * to mean something, and NIR has no width in which they do. So the
          * sentence names both halves, because "a width NIR does not have" on
          * its own sent the reader looking for a missing case in the builder
          * when what is missing is a representation.
          */
         air_error(c, "instruction: a %u-bit integer where NIR needs a register of its "
                      "own, a width it has no register for; it is carried in the next "
                      "width up only where nothing reads above bit %u",
                   bits, bits - 1);
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
   unsigned narrow = narrow_int_bits(t);
   if (narrow) {
      /* Canonical form (narrow_int_bits): the low N bits, zero above them.
       *
       * The mask here is the SECOND guard and not the first. The reader
       * already truncates an integer constant to its type's width
       * (truncate_to in air_module.c), so `i3 -4` arrives as 4 and not as a
       * sign-extended 0xfffc; removing this mask and rebuilding changes no
       * number in tools/air/callargs.ll, which was checked rather than
       * assumed. What makes the switch's case live is the mask on the
       * TRUNCATION into the type, in translate_cast, and that one is
       * load-bearing: without it `trunc i32 %w to i3` keeps eight bits, no
       * case matches and the default is taken.
       *
       * It stays because the invariant belongs to both halves: a constant of
       * this type must be canonical however it was built, and AIR_CONST_DATA
       * carries its words from the record without passing through
       * truncate_to.
       */
      return nir_imm_intN_t(&c->b, bits & narrow_mask(narrow),
                            narrow_promoted_bits(narrow));
   }
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
      return air_val_ssa(c, nir_imm_zero(&c->b, air_nir_components(t), air_narrow_bits(c, t)));

   case AIR_CONST_UNDEF:
   case AIR_CONST_POISON:
      if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
         unsigned n = t->kind == AIR_TYPE_STRUCT ? t->num_members : t->count;
         struct air_val *v = air_val_agg(c, n);
         /* fields stay NONE until insertvalue puts something there */
         return v;
      }
      if (t->kind == AIR_TYPE_POINTER) {
         struct air_ptr p = { .class = AIR_PTR_NONE, .pointee = t->elem };
         return air_val_ptr(c, p);
      }
      return air_val_ssa(c, nir_undef(&c->b, air_nir_components(t), air_narrow_bits(c, t)));

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
      res = nir_load_scratch(&c->b, comps, bits, off, .align_mul = align);
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
      /* READ, AND NOBODY SUPPLIED IT. air_module_function_constants registers
       * every constant a module DECLARES, with bits = 0 and supplied = false
       * (nir_air.h), so a load here answers zero whether or not a pipeline ever
       * chose one -- and this frontend now SPECIALISES on that zero
       * (plan_global_folding), which turns it from a number in an expression
       * into which blocks exist.
       *
       * air_metal.c says this on a refusal, with a count and two names; this is
       * the other half, and it is here rather than there because it lands in
       * the corpus report and so becomes countable instead of anecdotal. One
       * note per constant per module (air_note drops a repeat), so the report's
       * row for a name is the number of modules that read it unsupplied.
       *
       * Required-ness is in the sentence because it is what decides whether the
       * zero is a defect or a legal default: a declaration ending `i1 true` is
       * one Metal obliges the pipeline to supply, and `i1 false` is one the
       * module is supposed to ask about with air.is_function_constant_defined
       * first. The second is ordinary; the first means a pipeline left out
       * something it had to give.
       */
      if (!v->supplied) {
         bool required = false;
         const char *fcname = function_constant_decl(c, ptr.binding, &required);
         /* SHORT ON PURPOSE. 70 of these appear over the corpus, one per
          * constant, and the row that matters is the name beside the number of
          * modules -- a census. Repeating what it means 70 times would bury
          * exactly that, so the consequence is said once in docs/air.md 3 and
          * what is here is the fact.
          */
         air_note(c, "the function constant %s (index %u, %s) is read and no pipeline "
                     "supplied a value, so it was taken as zero",
                  fcname ? fcname : "(unnamed)", ptr.binding,
                  required ? "i1 true, which Metal obliges a pipeline to supply"
                           : "i1 false, so absent is legal");
      }
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
   case AIR_PTR_SLOT:
      /* AIR_OP_LOAD answers a whole load of a slot before it gets here, so
       * this is one that could not be answered: through a CHOSEN pointer, one
       * of whose sides is a slot (load_through_choice), or at an offset into
       * one. There is nothing to read either way -- a slot is a tree of values.
       */
      return air_error(c, "instruction: a load of bytes out of a local that holds a "
                          "pointer"), NULL;
   case AIR_PTR_NONE:
      /* A LOAD THROUGH A LITERAL `null`, which is undefined behaviour in the
       * source and therefore has exactly one honest translation: undef. Not a
       * value invented for it -- a declaration that there is no value, which
       * is what the source said.
       *
       * AIR_PTR_NONE arises from nothing else. air_constant_val builds it only
       * for AIR_CONST_NULL of a pointer type, so the module literally wrote
       * `null` here; a binding this frontend failed to resolve is a different
       * class and still refuses.
       *
       * Where it comes from is Metal's own contract rather than a mistake.
       * fixed_frag_lph_cph has `%124 = select i1 %123, ptr addrspace(1) null,
       * ptr addrspace(1) %14` on `fc_has_asg_77_weights`, and passes %124 to
       * its asg77 filter -- so when no weights are supplied the pointer IS
       * null, and the path that dereferences it is reachable only through a
       * switch on a FragmentShaderConfig field that comes from a bound buffer.
       * The application is what must not select that filter without supplying
       * weights; this frontend cannot know that it did not, and refusing the
       * module for a path the application is forbidden to take would refuse
       * the shader QuartzCore asks this driver for more than any other.
       *
       * It leaves a note, because a shader that reads undef is a fact a reader
       * of the report should be able to count.
       */
      air_note(c, "a load through a literal `null` pointer is undef: the module has a "
                  "path that dereferences null, Metal's contract is that the pipeline "
                  "must not take it (in the GL-on-Metal uber shaders it is a filter "
                  "whose weights buffer was not supplied), and undef is what the source "
                  "says rather than a value chosen here");
      return nir_undef(&c->b, comps, bits);
   default:
      return air_error(c, "instruction: a load through a %s pointer",
                       "texture or sampler"), NULL;
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
   case AIR_PTR_UBO:
   case AIR_PTR_CONSTANT_DATA:
      return air_error(c, "instruction: a store into constant memory");
   case AIR_PTR_ARGBUF:
      /* Writing a descriptor from inside the shader. The handle it would write
       * is the runtime's, and this frontend has resolved the set's members to
       * bindings rather than to addresses, so there is nothing to write.
       */
      return air_error(c, "instruction: a store into an argument buffer");
   case AIR_PTR_SLOT:
      /* as in air_load_ptr: a whole store into a slot is answered by
       * AIR_OP_STORE, so this one is at an offset or through a chosen pointer */
      return air_error(c, "instruction: a store of bytes into a local that holds a "
                          "pointer");
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
   /* [[thread_execution_width]]: Metal's older name for the same number, the
    * width of the SIMD-group the kernel runs in (Resident Evil 4's mainCS and
    * CS_MiniClusterCompaction take it as an input) */
   { "air.thread_execution_width", AIR_SV_SIMD_WIDTH },
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

/* A BOOLEAN varying is an integer varying.
 *
 * No hardware interpolates one bit, and make_io_var gives such an output
 * GLSL_TYPE_INT, which is 32 bits wide. Metal spells a `bool2` vertex output
 * as `<2 x i1>` in the returned struct -- RenderBox's stroke shaders pass the
 * two line caps that way, and the fragment stage takes them back as
 * `<2 x i1>` and zero-extends them -- so the store converts to the variable's
 * width and the load converts back.
 *
 * Both halves or neither: storing a 1-bit def into a 32-bit variable is what
 * nir_validate aborts on ("src->ssa->bit_size & bit_sizes"), and reading the
 * integer back as though it were already a boolean worked only because the
 * integer happens to hold 0 or 1 -- an agreement by accident, through a
 * `u2u8` that would have been a `b2i8`.
 */
static bool
io_is_boolean(const struct air_type *t)
{
   const struct air_type *s = air_type_scalar(t);
   return s->kind == AIR_TYPE_INT && s->bits == 1;
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
         r->argbuf_offset = (unsigned)m->offset;
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
      /* AIR before 2.4 (MapKit's Clear::clear_vertex, Metal 2.3 for iOS 14)
       * writes no air.address_space on a buffer: the parameter's own pointer
       * type is then the only statement of it, and it is the same fact -- the
       * key, where present, restates the address space of that pointer.
       */
      if (!md_key_int(node, "air.address_space", &as) && t->kind == AIR_TYPE_POINTER)
         as = t->addrspace;
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
         /* a threadgroup argument is sized at dispatch, so it can only go
          * after everything whose size is known here: the module's static
          * threadgroup arrays, which plan_globals lays out from offset 0. Its
          * base is set once they are (below plan_globals). At base 0 it would
          * share its bytes with the first of them. One is all the corpus uses;
          * a second would start where the first's runtime length ends, which
          * only the runtime knows, and is refused.
          */
         if (index != 0)
            return air_error(c, "interface: a second threadgroup buffer (index %u)",
                             (unsigned)index);
         c->tg_param_plus1 = param + 1;
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

   /* A TILE SHADER's imageblock. Named here rather than left to the catch-all
    * at the end of this function, because that one printed "interface: input
    * air.imageblock" -- the key it met, which reads like a decision and was
    * only a gap. There are two walls behind it and the near one is NOT the
    * Adreno's, so the sentence has to say which is which or the next reader
    * spends a day on the wrong one.
    *
    * THE NEAR WALL: nothing on this device can ask for these modules.
    * QuartzCore reaches its four tile_downsample_* pipelines from exactly two
    * places, and both ask one question first -- the same bit either way, bit 23
    * of the capability word at ctx+0xa3c:
    *
    *   CA::OGL::BlurState::tile_downsample   0x1883d3578  get(property 37), and
    *                                                      returns 0 on a zero
    *   CA::OGL::MetalContext::warmup_shaders 0x188476350  ldrb [ctx, #0xa3e]
    *                                         0x188476354  tbz w8, #7 -> skip
    *
    * `get` (0x1882e8a78) is that bit and a CA default; the bit is set by
    * CA::OGL::new_metal_context (0x18847fbfc) from -[MTLDevice
    * supportsTileShaders]; and Metal answers that as
    * -[_MTLDevice supportsTileShaders] (0x181aa897c) ->
    * -[_MTLDeviceFeatureQueries familySupportsTileShaders] (0x181b216b0), whose
    * whole body is `supportsFamily:` with w2 = 0x3ec -- MTLGPUFamilyApple4.
    * -[_MTLDevice initGPUFamilySupport] (0x181a1be1c) builds that list from
    * -featureProfile through a jump table at 0x181b5bc7c: profile 0 claims
    * Apple1, profile 1 adds Apple2, and Apple4 first appears at profile 5
    * (0x181a1c424). This port answers 0 or 1 (userland/src/redwoodmetal.m,
    * boot-arg metal_profile=), and the phone itself has printed
    * supportsFamily:Apple4 = 0.
    *
    * Apple4 is not a later generation's feature, which is the part worth
    * saying out loud: it is the iPhone X's OWN family, and Apple's A11 driver
    * answers profile 5. These four shaders run on this very phone under iOS.
    * The only reason nothing here can ask for them is that this port claims
    * less than its hardware is.
    *
    * THE FAR WALL is the Adreno's, and it is why raising the profile would buy
    * a refusal rather than a shader: Mesa 25.0.7 has no tile memory anywhere in
    * src/freedreno, and the one instruction that reads the tile buffer is
    * fragment-only by assertion (ir3_compiler_nir.c, nir_texop_txf_ms_fb).
    * docs/air.md section 3 has what the DRIVER would have to do instead -- break
    * the pass, resolve the attachment, dispatch an ordinary kernel -- which is
    * the round trip tile shading exists to avoid, and which belongs to whoever
    * writes the pass splitting and not to this function.
    *
    * Only the near wall is in the message, because air_error's buffer is 256
    * bytes (air_private.h) and vsnprintf cuts the rest off mid-word without
    * saying so: a longer sentence would have ended at "-featureProfile" in
    * every report, and the second wall is exactly the half that would have
    * gone. The message names the one that decides and points here for the one
    * that would decide next.
    */
   if (md_key(node, "air.imageblock") >= 0)
      return air_error(c, "interface: an imageblock input -- a tile shader nothing on "
                          "this device can ask for: CoreAnimation builds these only "
                          "behind -[MTLDevice supportsTileShaders] = "
                          "supportsFamily:Apple4, and this port claims at most Apple2 "
                          "(docs/air.md 3 has the other wall)");

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
      nir_def *loaded = nir_load_var(&c->b, var);
      if (io_is_boolean(t))
         loaded = nir_ine_imm(&c->b, loaded, 0);   /* see io_is_boolean */
      c->params[param] = air_val_ssa(c, loaded);
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

   /* index 0 steps over objects of the source type. A constant one folds, for
    * the reason the element index below gives: `getelementptr inbounds i8, ptr
    * %local, i64 4` is the shape a byte offset into a local arrives in, and a
    * slot can only resolve an offset it can read (slot_cell).
    */
   uint64_t stride = air_type_alloc_size(c->m, t);
   uint64_t first_const;
   if (air_const_uint(in->operands[1], &first_const)) {
      uint32_t off = (uint32_t)first_const * (uint32_t)stride;
      if (off)
         p = air_ptr_add(c, p, nir_imm_int(&c->b, off));
   } else {
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
         /* A CONSTANT INDEX IS A CONSTANT OFFSET, and saying so here is not an
          * optimisation: a slot resolves a byte offset into one of its members
          * only when that offset is decidable while translating (slot_cell), and
          * `getelementptr inbounds i8, ptr %local, i64 4` -- which is how a byte
          * offset into a struct reaches one -- used to arrive as an SSA value
          * because nir_builder does not fold. The arithmetic is the same one
          * the SSA path performs: nir_u2u32 takes the low 32 bits and
          * nir_imul_imm multiplies in 32-bit wrapping, so this folds to the
          * identical number rather than to a more correct one.
          */
         uint64_t ci;
         if (air_const_uint(in->operands[i], &ci)) {
            uint32_t off = (uint32_t)ci * (uint32_t)es;
            if (off)
               p = air_ptr_add(c, p, nir_imm_int(&c->b, off));
         } else {
            p = air_ptr_add(c, p, nir_imul_imm(&c->b, nir_u2u32(&c->b, idx), es));
         }
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
   /* A truncation into a width NIR has no register for is the one way such a
    * value is ever made (narrow_int_bits), so the width is taken from there
    * rather than from air_nir_bit_size, which would refuse it. Anything else
    * that touches the result or the source is refused by narrow_int_gate.
    */
   unsigned narrow = narrow_int_bits(dt);
   unsigned bits = narrow ? narrow_promoted_bits(narrow) : air_nir_bit_size(c, dt);
   nir_def *res;
   switch (in->sub) {
   case AIR_CAST_TRUNC:
      if (narrow) {
         /* into canonical form: the low N bits, zero above them */
         res = nir_iand_imm(&c->b, nir_u2uN(&c->b, x, bits), narrow_mask(narrow));
         break;
      }
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
      /* i32 <-> <4 x i8>: MPS's and CoreMedia's image kernels (mediaserverd's
       * hbf_compute_residual_error_kernel) take a word apart into its bytes; the
       * same little-endian order, element 0 the low byte */
      else if (sn == 1 && sb == 32 && dn == 4 && db == 8)
         res = nir_unpack_32_4x8(&c->b, x);
      else if (sn == 4 && sb == 8 && dn == 1 && db == 32)
         res = nir_pack_32_4x8(&c->b, x);
      /* i64 <-> <2 x i32>, the low word first */
      else if (sn == 1 && sb == 64 && dn == 2 && db == 32)
         res = nir_unpack_64_2x32(&c->b, x);
      else if (sn == 2 && sb == 32 && dn == 1 && db == 64)
         res = nir_pack_64_2x32(&c->b, x);
      /* any other repacking of as many bits (<2 x i32> -> <4 x half> in
       * mediaserverd's CNN kernels): nir_extract_bits moves them in the same
       * little-endian order the cases above spell out */
      else if (sn * sb == dn * db && sb >= 8 && db >= 8 && dn <= NIR_MAX_VEC_COMPONENTS)
         res = nir_extract_bits(&c->b, &x, 1, 0, dn, db);
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
agg_slot(struct air_ctx *c, struct air_val *agg, const struct air_type *type,
         const uint32_t *idx, unsigned n)
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
   const struct air_type *t = type;
   for (unsigned i = 0; i < n; i++) {
      /* The TYPE is walked beside the value because the value alone cannot say
       * how many elements a level has that nothing has written yet.
       */
      unsigned count;
      const struct air_type *et;
      if (t && t->kind == AIR_TYPE_STRUCT) {
         count = t->num_members;
         et = idx[i] < count ? t->members[idx[i]] : NULL;
      } else if (t && t->kind == AIR_TYPE_ARRAY) {
         count = t->count > UINT_MAX ? UINT_MAX : (unsigned)t->count;
         et = idx[i] < count ? t->elem : NULL;
      } else {
         count = 0;
         et = NULL;
      }
      if (!et) {
         air_error(c, "instruction: an aggregate index out of its type");
         return NULL;
      }
      /* A level nothing has written yet, made here rather than refused.
       *
       * `insertvalue [3 x [2 x float]] undef, float %x, 0, 0` reaches the row
       * before the row exists: an undef aggregate is built with its fields
       * NONE on purpose, so a NESTED one has no row for the column to go in.
       * Making it changes no value -- an aggregate whose every field is NONE
       * *is* the undef of that type, which is exactly what air_constant_val
       * builds for the top level -- so this is the same value said in the form
       * the walk can continue through.
       *
       * It is 16 of RenderBox's 47 modules, every one of them building a 3x2
       * transform this way before passing it to a shape function, and the
       * refusal it used to raise ("an aggregate index out of its value") named
       * the symptom at the second index rather than the missing level at the
       * first.
       */
      if (!*slot)
         *slot = air_val_agg(c, count);
      if ((*slot)->kind != AIR_VAL_AGG || idx[i] >= (*slot)->num_elems) {
         air_error(c, "instruction: an aggregate index out of its value");
         return NULL;
      }
      slot = &(*slot)->elems[idx[i]];
      t = et;
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

/* The one place a value of a width NIR has no register for is let through.
 *
 * Such a value is carried in the next width up with the bits above it zero
 * (narrow_int_bits), which is exact for a short list of operations and wrong
 * for the rest -- and wrong in the quiet way: `add i3` in a 8-bit register
 * carries into bit 3 and the shader computes something, so nothing fails. The
 * list is therefore checked here, over every instruction of every module,
 * rather than relied on from the four modules that happen to need it: the gate
 * is what makes the representation safe, and a representation whose safety
 * rests on nobody writing the other case is not safe.
 *
 * The ones that pass are the ones canonical form makes exact: a truncation
 * into the type, a truncation or a zero-extension out of it, an unsigned
 * conversion to float, an equality or an unsigned comparison, and a switch,
 * which the CFG walk emits as equalities against constants of the same width.
 * Everything else is refused by width, opcode and operand number.
 */
static bool
narrow_int_gate(struct air_ctx *c, const struct air_inst *in)
{
   unsigned n = narrow_int_bits(in->base.type);
   if (n && !(in->op == AIR_OP_CAST && in->sub == AIR_CAST_TRUNC))
      return air_error(c, "instruction: a %u-bit integer made by opcode %u, a width NIR "
                          "has no register for; only a truncation into one is exact in "
                          "the next width up", n, in->op);

   for (unsigned i = 0; i < in->num_operands; i++) {
      const struct air_value *v = in->operands[i];
      unsigned m = v ? narrow_int_bits(v->type) : 0;
      if (!m)
         continue;
      if (in->op == AIR_OP_SWITCH)
         continue;
      if (in->op == AIR_OP_CMP &&
          (in->sub == AIR_ICMP_EQ || in->sub == AIR_ICMP_NE ||
           in->sub == AIR_ICMP_ULT || in->sub == AIR_ICMP_ULE ||
           in->sub == AIR_ICMP_UGT || in->sub == AIR_ICMP_UGE))
         continue;
      if (in->op == AIR_OP_CAST &&
          (in->sub == AIR_CAST_TRUNC || in->sub == AIR_CAST_ZEXT ||
           in->sub == AIR_CAST_UITOFP))
         continue;
      return air_error(c, "instruction: a %u-bit integer reaching opcode %u as operand "
                          "%u, a width NIR has no register for; only a switch, an "
                          "equality, an unsigned comparison and a widening that ignores "
                          "the sign are exact in the next width up", m, in->op, i);
   }
   return true;
}

static bool
translate_instruction(struct air_ctx *c, const struct air_inst *in)
{
   if (!narrow_int_gate(c, in))
      return false;

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
         /* A choice that is not a choice. The GL-on-Metal uber shaders pick
          * between the pipeline's sampler and their own inline one on a
          * function-constant predicate, and once that predicate is an
          * immediate there is one pointer here and not two -- which matters
          * because the alternative below keeps the choice and a TEXTURE or a
          * SAMPLER is not a value it can load. Folding first is also what
          * answers the argument-buffer case under it for free when the
          * condition is known, instead of refusing a select that has already
          * been decided.
          */
         nir_def *pcond = air_get_ssa(c, in->operands[0]);
         bool ptaken;
         if (pcond && pcond->num_components == 1 &&
             cond_const(c, pcond, &ptaken)) {
            air_set(c, &in->base, ptaken ? pa : pb);
            return true;
         }
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
      /* A BOOLEAN index. `extractelement <2 x i1> %54, i1 %55` is in
       * RenderBox's stroke shaders -- the line's two caps picked by which end
       * of the segment the pixel is nearer -- and `u2u32` is the wrong
       * conversion for it: a NIR boolean is a 1-bit def, not a 1-bit integer,
       * and widening one with an unsigned conversion is undefined where
       * nir_b2i32 is exactly 0 or 1. ir3 says so outright ("invalid src bit
       * size: 1") and refuses the shader; a backend that did not say so would
       * have read lane ~0u instead of lane 1.
       */
      nir_def *idx = i->bit_size == 1 ? nir_b2i32(&c->b, i) : nir_u2u32(&c->b, i);
      air_set(c, &in->base, air_val_ssa(c, nir_vector_extract(&c->b, v, idx)));
      return true;
   }
   case AIR_OP_INSERTELT: {
      nir_def *v = air_get_ssa(c, in->operands[0]);
      nir_def *x = air_get_ssa(c, in->operands[1]);
      nir_def *i = air_get_ssa(c, in->operands[2]);
      if (!v || !x || !i)
         return false;
      /* AN INDEX DECIDED WHILE THE SHADER RUNS, which NIR has its own lowering
       * for: nir_vector_insert builds `bcsel(i == {0,1,2,3}, scalar, vec)`,
       * one comparison per component, and takes the constant path when the
       * index is one. So this is Mesa's rule rather than one written here, and
       * the constant case goes through exactly the code it went through before
       * (nir_vector_insert calls nir_vector_insert_imm itself).
       *
       * The one place the two languages differ is an index PAST the vector.
       * LLVM says an out-of-range insertelement is poison; the bcsel chain
       * misses every component and yields the vector unchanged. Poison permits
       * any value, so an unchanged vector is a refinement of it and not a
       * disagreement -- which is the only reason this can be done without
       * inventing a semantics.
       *
       * It is the wall fixed_frag_lph_cph reaches once its texture and sampler
       * choices are resolved, under BOTH the all-ones configuration and the
       * compositor's own (docs/air.md section 3), which is what made it worth
       * taking rather than refusing: two configurations that disagree about
       * everything else converge here.
       */
      air_set(c, &in->base, air_val_ssa(c, nir_vector_insert(&c->b, v, x, i)));
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
      struct air_val **slot = agg_slot(c, agg, in->operands[0]->type,
                                       in->indices, in->num_indices);
      if (!slot)
         return false;
      if (!*slot) {
         /* A field of an undef aggregate: the undef of that field's OWN type,
          * built the one way this translator knows how to build an undef of
          * anything -- which matters because the field may itself be an
          * aggregate, and a nir_undef cannot be one.
          */
         struct air_constant u = { .base = { .kind = AIR_VALUE_CONSTANT,
                                             .type = in->base.type },
                                   .ckind = AIR_CONST_UNDEF };
         struct air_val *v = air_constant_val(c, &u);
         if (!v)
            return false;
         air_set(c, &in->base, v);
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
      struct air_val **slot = agg_slot(c, copy, in->operands[0]->type,
                                       in->indices, in->num_indices);
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
      /* An alloca is bytes in scratch -- unless it holds a POINTER, which
       * scratch cannot: a pointer in this frontend is a resolution, a binding
       * and a byte offset decided while translating, and has no bytes to put
       * anywhere. Then it is a translation-time SLOT instead (AIR_PTR_SLOT in
       * air_private.h, and the whole-store and whole-load cases above).
       *
       * Spelled out because what stood here twice described code that was not
       * here. It first promised a slot "see AIR_VAL_SLOT" when no such value
       * kind existed -- that slot had been written and removed -- and the
       * correction then said no slot would help, which was true of the module
       * count and not of the wall: the slot closes the FIRST of four walls in
       * the two GL-on-Metal fragment shaders, and the three behind it are
       * either closed by specialising on the function constants
       * (plan_global_folding) or, in the case of air.get_null_texture_2d, a
       * refusal that now says whose wall it is. tools/air/chosencheck.sh is
       * the record of all four.
       *
       * The same comment also said those two shaders "store their last two
       * buffer arguments into such a local and never read them back".
       * Parameters 34 and 35 of fixed_frag_lph_cph are `air.sampler`
       * (img_samp_0, img_samp_1), not buffers, and the module does read them
       * back: `select` against its own inline sampler, then `load`, into the
       * sampler argument of 27 air.sample_texture_2d calls and 12 calls to its
       * own box-sample functions. That is why the slot has to hold a whole
       * air_val and not a number.
       */
      /* ONE object that holds a POINTER: a translation-time slot, because
       * scratch cannot hold one. `count == 1` and nothing else, so an array of
       * pointers still goes to memory and still refuses at the store -- an
       * array would need an index, and an index into a tree of values is the
       * thing this deliberately does not do.
       */
      if (count == 1 && type_has_pointer(in->aux_type, 8)) {
         struct air_ptr sp = {
            .class = AIR_PTR_SLOT,
            .binding = new_slot(c, in->aux_type, NULL),
            .pointee = in->aux_type,
         };
         air_set(c, &in->base, air_val_ptr(c, sp));
         return true;
      }

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
      /* A program-scope variable whose single static-initialiser store turned
       * out to be a constant: the load IS that constant, rebuilt in whatever
       * builder is asking. Rebuilt and not shared, because a nir_def belongs
       * to one impl and a callee may load the same variable -- which is the
       * same rule materialise_globals follows for the offset.
       */
      struct air_global_plan *gpl = foldable_global_plan(c, in->operands[0]);
      if (gpl && gpl->known) {
         air_set(c, &in->base,
                 air_val_ssa(c, nir_imm_intN_t(&c->b, gpl->bits, gpl->bit_size)));
         return true;
      }
      struct air_val *p = air_get(c, in->operands[0]);
      if (!p)
         return false;
      unsigned slot;
      if (slot_base(c, p, &slot)) {
         struct air_val *v = slot_at_zero(c->slots[slot].val, in->base.type);
         if (!v)
            return air_error(c, "instruction: a load from a local that holds a "
                                "pointer and has not been written");
         air_set(c, &in->base, v);
         return true;
      }
      /* Not the slot's base: a member of it, if the offset and the loaded type
       * name one between them (slot_cell). Tried AFTER slot_base and not
       * instead of it, so that nothing that translated before this existed can
       * take a different path: offset zero with a type slot_at_zero's looser
       * descent accepts still goes the way it went.
       */
      uint64_t soff;
      if (slot_at_const(c, p, &slot, &soff)) {
         struct air_val **cell = slot_cell(c, slot, soff, in->base.type);
         if (cell && *cell) {
            air_set(c, &in->base, *cell);
            return true;
         }
         if (cell)
            return air_error(c, "instruction: a load from a member of a local that "
                                "holds a pointer and has not been written");
      }
      if (p->kind == AIR_VAL_PTR && p->ptr.class == AIR_PTR_SLOT)
         return air_error(c, "instruction: a load at an offset into a local that holds "
                             "a pointer which does not name one of its members, so "
                             "there is no value there to give");
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
      unsigned sslot;
      uint64_t soff;
      if (slot_base(c, p, &sslot) &&
          in->operands[0]->type == c->slots[sslot].type) {
         if (c->slots[sslot].init)
            return air_error(c, "instruction: a store into a read-only global, which is "
                                "where its own initialiser lives");
         c->slots[sslot].val = sv;
         return true;
      }
      /* A store into one MEMBER of a slot. The type stored and the byte offset
       * name the member between them, and neither does on its own
       * (slot_cell). This is what the four `disparity_refinement_*` stop at:
       * CoreImage's stitched functions rebuild a `coreimage::Sampler` in a
       * local out of the four arguments it was flattened into.
       */
      if (slot_at_const(c, p, &sslot, &soff)) {
         if (c->slots[sslot].init)
            return air_error(c, "instruction: a store into a read-only global, which is "
                                "where its own initialiser lives");
         struct air_val **cell = slot_cell(c, sslot, soff,
                                           in->operands[0]->type);
         if (cell) {
            *cell = sv;
            return true;
         }
         return air_error(c, "instruction: a store into a local that holds a pointer at "
                             "byte %u, which names no member of it: a tree of values has "
                             "no bytes for an offset that straddles two of them",
                          (unsigned)soff);
      }
      if (p->kind == AIR_VAL_PTR && p->ptr.class == AIR_PTR_SLOT)
         return air_error(c, "instruction: a store at an offset computed at run time "
                             "into a local that holds a pointer, so which member it "
                             "names is not decidable here");
      if (val_is_pointerish(sv))
         return air_error(c, "instruction: a store of a pointer into memory, which this "
                             "frontend has no address to write");
      if (p->kind == AIR_VAL_PTR_SELECT)
         return air_error(c, "instruction: a store through a chosen pointer, which would "
                             "have to write both of them");
      nir_def *v = air_get_ssa(c, in->operands[0]);
      if (!v)
         return false;
      if (p->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a store to something that is not a pointer");
      /* The one store into a foldable program-scope variable. Its value is
       * recorded when it is a constant, and the memory is written either way:
       * plan_global_folding has proved that nothing else reaches these bytes,
       * so the store is dead when the fold answers -- and leaving it in costs
       * one DCE'd instruction, where leaving it OUT would be a promise about
       * every access shape that scan might have missed.
       */
      struct air_global_plan *gpl = foldable_global_plan(c, in->operands[1]);
      if (gpl) {
         nir_const_value cv[NIR_MAX_VEC_COMPONENTS];
         if (v->num_components == 1 &&
             def_const(v, cv, 16, c->nir->info.float_controls_execution_mode)) {
            gpl->known = true;
            gpl->bits = nir_const_value_as_uint(cv[0], v->bit_size);
            gpl->bit_size = v->bit_size;
         }
      }
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
         nir_def *d = v->elems[i]->def;
         if (io_is_boolean(rt->members[i]) && d->bit_size == 1)
            d = nir_b2iN(&c->b, d, 32);   /* see io_is_boolean */
         nir_store_var(&c->b, c->outputs[i].var, d,
                       BITFIELD_MASK(d->num_components));
      }
      return true;
   }
   if (v->kind != AIR_VAL_SSA)
      return air_error(c, "instruction: a returned value that is not a number");
   if (!c->num_outputs || !c->outputs[0].var)
      return air_error(c, "interface: a returned value with no output");
   nir_store_var(&c->b, c->outputs[0].var, v->def, BITFIELD_MASK(v->def->num_components));
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
/* A module's globals and its static initialisers: per MODULE, and a stitched
 * shader is built from several, so these run once for each of them rather than
 * once for the shader (stitch_graph below).
 */
static void plan_globals(struct air_ctx *c);
static void run_static_initializers(struct air_ctx *c);

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

/* -- an aggregate handed to a call of a module function -------------------
 *
 * A NIR function parameter carries a NUMBER -- a scalar, or a vector of them.
 * A struct or an array is a tree of values in this translator and never a NIR
 * value at all (air_private.h), so a function called with one BY VALUE has to
 * take the numbers inside it, one parameter each, and put the tree back
 * together on the other side.
 *
 * This is what Metal's own compiler asks for here and not an exotic case.
 * RenderBox splits its shader state into one-field `RenderState` structs and
 * hands every shape, blend, filter and resolve function its globals as a
 * struct of matrices -- `{ [3 x [2 x float]], float }`, and for the blur
 * `{ float, [2 x float], [2 x float], [2 x float], [4 x [2 x float]],
 * [4 x half] }`, nineteen numbers in one argument. 26 of that library's 47
 * modules stop at such a call.
 *
 * The order is extractvalue's: member 0's numbers, then member 1's, depth
 * first. The caller and the callee walk the same type with the same function,
 * which is what keeps the sources and the parameters in step -- and every call
 * site checks the count it built against nir_call_instr.num_params as well,
 * because "in step" is a claim and a call whose sources are one short is a
 * segfault inside nir_instr_insert rather than a message.
 */
#define AIR_MAX_ARG_LEAVES 64

/* the numbers inside a type, or why it has one that no parameter can carry */
static bool
type_leaves(struct air_ctx *c, const struct air_type *t,
            const struct air_type **out, unsigned *n)
{
   if (!t)
      return air_error(c, "intrinsic: a call argument of no type");
   if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
      unsigned count = t->kind == AIR_TYPE_STRUCT ? t->num_members
                                                  : (unsigned)MIN2(t->count, UINT_MAX);
      for (unsigned i = 0; i < count; i++) {
         const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
         if (!type_leaves(c, et, out, n))
            return false;
      }
      return true;
   }
   if (t->kind != AIR_TYPE_VECTOR && t->kind != AIR_TYPE_INT && !air_type_is_float(t))
      return air_error(c, "intrinsic: a call passing %s inside a value, which is not a "
                          "number a parameter can carry",
                       t->kind == AIR_TYPE_POINTER ? "a pointer" : "a type of no size");
   if (*n >= AIR_MAX_ARG_LEAVES)
      return air_error(c, "intrinsic: a call passing a value of more than %u numbers",
                       AIR_MAX_ARG_LEAVES);
   if (out)
      out[*n] = t;
   (*n)++;
   return true;
}

/* the caller's side: the defs to pass, in that same order */
static bool
val_leaves(struct air_ctx *c, const struct air_type *t, struct air_val *v,
           nir_def **out, unsigned *n)
{
   if (!t)
      return air_error(c, "intrinsic: a call argument of no type");
   if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
      if (v && v->kind != AIR_VAL_AGG)
         return air_error(c, "intrinsic: a call passing %s where its type says an "
                             "aggregate",
                          v->kind == AIR_VAL_SSA ? "a number" : "a pointer");
      unsigned count = t->kind == AIR_TYPE_STRUCT ? t->num_members
                                                  : (unsigned)MIN2(t->count, UINT_MAX);
      for (unsigned i = 0; i < count; i++) {
         const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
         if (!val_leaves(c, et, v && i < v->num_elems ? v->elems[i] : NULL, out, n))
            return false;
      }
      return true;
   }
   if (*n >= AIR_MAX_ARG_LEAVES)
      return air_error(c, "intrinsic: a call passing a value of more than %u numbers",
                       AIR_MAX_ARG_LEAVES);
   if (!v) {
      /* A number nothing wrote is undef, which is what the aggregate already
       * says: an undef struct is built with its fields NONE (air_constant_val),
       * and Metal's compiler does pass whole `undef` globals -- filter_blur is
       * handed `PlaneGlobals undef` because the blur reads none of it.
       */
      out[(*n)++] = nir_undef(&c->b, air_nir_components(t), air_narrow_bits(c, t));
      return true;
   }
   if (v->kind != AIR_VAL_SSA)
      return air_error(c, "intrinsic: a call passing %s where a number belongs",
                       v->kind == AIR_VAL_AGG ? "an aggregate" : "a pointer");
   out[(*n)++] = v->def;
   return true;
}

/* the callee's side: the tree, back out of its parameters */
static struct air_val *
val_from_leaves(struct air_ctx *c, const struct air_type *t, nir_def **in, unsigned *n)
{
   if (t->kind == AIR_TYPE_STRUCT || t->kind == AIR_TYPE_ARRAY) {
      unsigned count = t->kind == AIR_TYPE_STRUCT ? t->num_members
                                                  : (unsigned)MIN2(t->count, UINT_MAX);
      struct air_val *agg = air_val_agg(c, count);
      for (unsigned i = 0; i < count; i++) {
         const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
         if (!(agg->elems[i] = val_from_leaves(c, et, in, n)))
            return NULL;
      }
      return agg;
   }
   return air_val_ssa(c, in[(*n)++]);
}

/* Whether a resolution inside this value carries an offset computed at run
 * time. Checked before anything is built, so the refusal names the argument
 * rather than arriving from the middle of the callee's body.
 */
static bool
val_offset_is_computed(const struct air_val *v)
{
   if (!v)
      return false;
   if (v->kind == AIR_VAL_AGG) {
      for (unsigned i = 0; i < v->num_elems; i++)
         if (val_offset_is_computed(v->elems[i]))
            return true;
      return false;
   }
   return v->kind == AIR_VAL_PTR && v->ptr.offset &&
          !nir_src_is_const(nir_src_for_ssa(v->ptr.offset));
}

/* The caller's resolution, with every byte offset in it rebuilt in whichever
 * builder is current now.
 *
 * A pointer here is *(where, byte offset)* and the offset is an nir_def, which
 * belongs to the impl that made it; handing the callee the caller's def is the
 * cross-function def/use that the globals used to cause, and that is why an
 * argument carrying one was refused outright.
 *
 * A CONSTANT offset need not be refused, and every one that occurs is
 * constant: what a RenderBox shader hands its shape function is an ALLOCA,
 * whose offset is the scratch cursor at translation time (AIR_OP_ALLOCA builds
 * nir_imm_int of it), and the callee reads the struct back out of scratch with
 * geps of its own. Nothing is shared -- the NUMBER is copied and a new def
 * made here -- so the callee's offset is a def of the callee's own impl while
 * the resolution is the same resolution. The scratch it names is the same
 * scratch because air_ctx.scratch_size is the SHADER's, not the function's,
 * and nir_inline_functions folds the callee into the caller afterwards anyway.
 *
 * FOURTEEN of RenderBox's 47 modules stop here once the aggregate-index barrier
 * above is lifted, chronod's first fault among them, and thirteen of the
 * fourteen need only this: the one that does not is uber_fragment, whose
 * offset is read out of its globals and goes through ptr_offset_to_pass. The
 * number is the frontend's own count of the modules that reported this
 * refusal, not a reading of the IR -- a scan of the bitcode for "an alloca
 * passed to a call" gave 12 and then 14 depending on how the call line was
 * matched, which is a reason to take the count from the thing that counts.
 */
/* The byte offset of a pointer argument that has to be PASSED as a number
 * rather than copied as one.
 *
 * Where a pointer points is a translation-time fact -- which buffer, at which
 * Metal index -- and that half is bound into the callee whatever the offset is.
 * The offset is the half that can be a VALUE: uber_fragment reads an index out
 * of its globals and hands `device void *` at that byte to sample_image
 * (`getelementptr inbounds i8, ptr addrspace(1) %7, i64 %416`), so there is
 * nothing constant to copy and nothing resolved to bind.
 *
 * So the binding is bound and the offset becomes one more NIR parameter, which
 * puts the def on the right side of the function boundary: the caller passes
 * its own def as a call source, and the callee's offset is its own
 * load_param. That is the same thing nir_call does for every other number and
 * it is why this is exact rather than a workaround.
 *
 * Only a pointer that IS the argument, not one inside a struct: a resolution
 * nested in an aggregate with a run-time offset has no case in either corpus,
 * and rebind_here still refuses it by name rather than guessing a layout for
 * the parameters it would take.
 */
static nir_def *
ptr_offset_to_pass(struct air_val *v)
{
   if (!v || v->kind != AIR_VAL_PTR || !v->ptr.offset)
      return NULL;
   return nir_src_is_const(nir_src_for_ssa(v->ptr.offset)) ? NULL : v->ptr.offset;
}

/* A SLOT PASSED TO A CALL, and the parameters it costs.
 *
 * A slot NUMBER means the same thing in a callee as in a caller -- air_ctx.slots
 * is the SHADER's table, not the function's, the same way scratch_size is -- so
 * what cannot cross a function boundary is not the slot but the DEFS inside it.
 * A slot holds a tree of resolutions, and two kinds of thing in that tree belong
 * to the caller's impl: a plain value (`coreimage::Sampler`'s `float2 coord`),
 * and a pointer's byte offset when it is computed rather than constant. Both go
 * the way the run-time offset of a plain pointer argument goes
 * (ptr_offset_to_pass): one more nir_parameter the caller supplies as a call
 * source and the callee reads with load_param, which puts the def on the side of
 * the boundary it belongs to instead of hiding the crossing.
 *
 * It used to be refused outright, as a latent hole nothing asked for. Something
 * asks for it now: the four `disparity_refinement_*` of
 * CIPortraitBlurStitchableV3 are a stitched function whose first act is to
 * rebuild the `coreimage::Sampler` CoreImage flattened into four arguments, and
 * 126 modules of the image do the same thing (docs/air.md 10).
 *
 * THE TWO FUNCTIONS BELOW ARE A PAIR AND THE ORDER IS THE CONTRACT. slot_leaves
 * collects the caller's defs; rebind_slot_contents rebuilds the tree with the
 * callee's load_params in their place. They walk identically -- aggregate
 * elements in index order, then a pointer's offset, then a value -- and the
 * caller checks the two counts against each other rather than trusting them,
 * because a call one source short is a null nir_src that nir_instr_insert
 * dereferences while adding uses: a segfault, not a message. That is the same
 * rule, for the same reason, as the type_leaves/val_leaves pair below.
 */
#define AIR_MAX_SLOT_LEAVES 16

static bool
slot_leaves(struct air_ctx *c, struct air_val *v, nir_def **out, unsigned *n)
{
   if (!v)
      return true;
   switch (v->kind) {
   case AIR_VAL_AGG:
      for (unsigned i = 0; i < v->num_elems; i++)
         if (!slot_leaves(c, v->elems[i], out, n))
            return false;
      return true;
   case AIR_VAL_PTR:
      if (!v->ptr.offset || nir_src_is_const(nir_src_for_ssa(v->ptr.offset)))
         return true;
      break;
   case AIR_VAL_SSA:
      if (!v->def)
         return false;
      break;
   default:
      /* a pointer chosen at run time, or a cell nothing wrote: neither has a
       * form here, and both are named by the caller
       */
      return false;
   }
   if (*n >= AIR_MAX_SLOT_LEAVES)
      return false;
   if (out)
      out[*n] = v->kind == AIR_VAL_SSA ? v->def : v->ptr.offset;
   (*n)++;
   return true;
}

/* The caller's tree, rebuilt for the function being translated: `got` holds one
 * load_param per leaf slot_leaves found, in that order, and `k` walks it.
 */
static struct air_val *
rebind_slot_contents(struct air_ctx *c, struct air_val *v, nir_def **got,
                     unsigned ngot, unsigned *k)
{
   if (!v)
      return v;
   switch (v->kind) {
   case AIR_VAL_AGG: {
      struct air_val *n = air_val_agg(c, v->num_elems);
      for (unsigned i = 0; i < v->num_elems; i++) {
         if (!v->elems[i])
            continue;
         if (!(n->elems[i] = rebind_slot_contents(c, v->elems[i], got, ngot, k)))
            return NULL;
      }
      return n;
   }
   case AIR_VAL_PTR: {
      struct air_ptr p = v->ptr;
      if (!p.offset)
         return v;
      nir_src src = nir_src_for_ssa(p.offset);
      if (nir_src_is_const(src)) {
         /* a constant of the caller is still a def of the caller's impl */
         p.offset = nir_imm_int(&c->b, nir_src_as_uint(src));
         return air_val_ptr(c, p);
      }
      if (*k >= ngot)
         return NULL;
      p.offset = got[(*k)++];
      return air_val_ptr(c, p);
   }
   case AIR_VAL_SSA:
      if (*k >= ngot)
         return NULL;
      return air_val_ssa(c, got[(*k)++]);
   default:
      return NULL;
   }
}

/* The defs a slot argument must pass, or n = 0 for an argument that is not one.
 * One function so that the three places that have to agree -- the parameter
 * count, the parameter declarations and the call's sources -- ask the same
 * question of the same value.
 */
static bool
slot_arg_leaves(struct air_ctx *c, struct air_val *v, nir_def **out, unsigned *n)
{
   *n = 0;
   if (!v || v->kind != AIR_VAL_PTR || v->ptr.class != AIR_PTR_SLOT)
      return true;
   if (v->ptr.binding >= c->num_slots)
      return false;
   if (v->ptr.offset &&
       (!nir_src_is_const(nir_src_for_ssa(v->ptr.offset)) ||
        nir_src_as_uint(nir_src_for_ssa(v->ptr.offset)) != 0))
      return false;      /* a member of a slot, not the whole of it */
   return slot_leaves(c, c->slots[v->ptr.binding].val, out, n);
}

static struct air_val *
rebind_here(struct air_ctx *c, struct air_val *v, unsigned arg)
{
   if (!v)
      return v;
   if (v->kind == AIR_VAL_AGG) {
      struct air_val *n = air_val_agg(c, v->num_elems);
      for (unsigned i = 0; i < v->num_elems; i++) {
         if (!v->elems[i])
            continue;
         if (!(n->elems[i] = rebind_here(c, v->elems[i], arg)))
            return NULL;
      }
      return n;
   }
   if (v->kind != AIR_VAL_PTR || !v->ptr.offset)
      return v;
   nir_src src = nir_src_for_ssa(v->ptr.offset);
   if (!nir_src_is_const(src)) {
      air_error(c, "intrinsic: a call passing a pointer at an offset computed at run "
                   "time as argument %u, whose value belongs to the caller's own "
                   "function", arg);
      return NULL;
   }
   struct air_ptr p = v->ptr;
   p.offset = nir_imm_int(&c->b, nir_src_as_uint(src));
   return air_val_ptr(c, p);
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
    * A pointer carrying a byte offset that is a CONSTANT is bound the same way,
    * with the number copied into a def of this function's own impl
    * (rebind_here); one computed at run time is refused, because using the
    * caller's def here is the cross-function def/use that the globals used to
    * cause.
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
      /* A constant byte offset is rebuilt in the callee's own builder
       * (rebind_here) and a run-time one on the pointer ITSELF is passed as a
       * parameter (ptr_offset_to_pass). What is left -- a run-time offset on a
       * pointer buried inside a struct -- is refused.
       */
      if (ptr && !ptr_offset_to_pass(argv[i]) && val_offset_is_computed(argv[i])) {
         air_error(c, "intrinsic: a call passing a pointer at an offset computed at run "
                      "time as argument %u, whose value belongs to the caller's own "
                      "function", i);
         return NULL;
      }
      /* The same reason, for the two pointers that are values: a chosen
       * pointer's condition is a def of the CALLER's impl, and a slot's
       * soundness rests on every use being in one block of one function.
       */
      if (argv && argv[i] && argv[i]->kind == AIR_VAL_PTR_SELECT) {
         air_error(c, "intrinsic: a call passing a pointer chosen at run time as "
                      "argument %u", i);
         return NULL;
      }
      /* A SLOT used to be refused outright here, as a latent cross-function
       * def/use nothing asked for. Something does ask for it now -- the four
       * `disparity_refinement_*` of CIPortraitBlurStitchableV3 pass a
       * `coreimage::Sampler` they rebuilt in a local, and 126 modules of the
       * image do the same thing -- so it goes through rebind_here, which makes
       * the callee a slot of its own and copies the resolutions into it. What
       * is still refused, and by rebind_slot_contents rather than here, is a
       * slot holding a plain VALUE: that is a def of the caller's impl.
       */
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
   /* one parameter per NUMBER in each argument, not one per argument: see
    * type_leaves. Counted before fn exists, because fn->params is sized from it.
    */
   unsigned nargparams = 0;
   for (unsigned i = 0; i < f->num_args; i++) {
      if (!is_param[i]) {
         if (ptr_offset_to_pass(argv ? argv[i] : NULL)) {
            nargparams++;
            continue;
         }
         /* a slot's value leaves, which are the caller's defs: slot_arg_leaves */
         unsigned ns = 0;
         if (!slot_arg_leaves(c, argv ? argv[i] : NULL, NULL, &ns)) {
            air_error(c, "intrinsic: a call passing a local that holds a pointer as "
                         "argument %u, whose contents this cannot hand across a "
                         "function boundary", i);
            return NULL;
         }
         nargparams += ns;
         continue;
      }
      unsigned n = 0;
      if (!type_leaves(c, f->args[i].type, NULL, &n))
         return NULL;
      nargparams += n;
   }
   unsigned nparams = nargparams + nret;
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
      if (!is_param[i]) {
         /* a bound pointer whose byte offset is a value: see ptr_offset_to_pass */
         if (ptr_offset_to_pass(argv ? argv[i] : NULL)) {
            fn->params[idx++] = (nir_parameter){ .num_components = 1, .bit_size = 32 };
            continue;
         }
         /* a slot's value leaves. Each parameter is declared from its own def's
          * shape and the source is passed unconverted, so the two match by
          * construction rather than by a width this would have to choose.
          */
         nir_def *sv[AIR_MAX_SLOT_LEAVES];
         unsigned ns = 0;
         if (!slot_arg_leaves(c, argv ? argv[i] : NULL, sv, &ns))
            return NULL;   /* counted above, so this cannot fail; not assumed */
         for (unsigned k = 0; k < ns; k++)
            fn->params[idx++] = (nir_parameter){
               .num_components = (uint8_t)sv[k]->num_components,
               .bit_size = (uint8_t)sv[k]->bit_size,
            };
         continue;
      }
      const struct air_type *lv[AIR_MAX_ARG_LEAVES];
      unsigned n = 0;
      if (!type_leaves(c, f->args[i].type, lv, &n))
         return NULL;   /* counted above, so this cannot fail; not assumed */
      for (unsigned k = 0; k < n; k++)
         fn->params[idx++] = (nir_parameter){
            .num_components = (uint8_t)air_nir_components(lv[k]),
            .bit_size = (uint8_t)air_nir_bit_size(c, lv[k]),
         };
   }
   fn->num_params = idx;   /* the pointers took no parameter */

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
         if (ptr_offset_to_pass(argv[i])) {
            /* the binding from the caller, the offset from a parameter */
            struct air_ptr pp = argv[i]->ptr;
            pp.offset = nir_load_param(&c->b, pidx++);
            air_set(c, &f->args[i], air_val_ptr(c, pp));
            continue;
         }
         /* A SLOT: a new slot of the same type for this function, filled with
          * the caller's resolutions rebuilt here and its values read out of the
          * parameters declared above, in the order slot_leaves walked them.
          */
         if (argv[i] && argv[i]->kind == AIR_VAL_PTR &&
             argv[i]->ptr.class == AIR_PTR_SLOT) {
            nir_def *sv[AIR_MAX_SLOT_LEAVES];
            unsigned ns = 0;
            if (!slot_arg_leaves(c, argv[i], sv, &ns)) {
               ok = false;
               break;
            }
            for (unsigned k = 0; k < ns; k++)
               sv[k] = nir_load_param(&c->b, pidx++);
            unsigned used = 0;
            const struct air_type *st = c->slots[argv[i]->ptr.binding].type;
            struct air_val *inner =
               rebind_slot_contents(c, c->slots[argv[i]->ptr.binding].val,
                                    sv, ns, &used);
            if (used != ns || (!inner && c->slots[argv[i]->ptr.binding].val)) {
               air_error(c, "intrinsic: a call passing a local that holds a pointer as "
                            "argument %u, rebuilt with %u of its %u parameters",
                         i, used, ns);
               ok = false;
               break;
            }
            struct air_ptr sp = {
               .class = AIR_PTR_SLOT,
               .binding = new_slot(c, st, NULL),
               .pointee = argv[i]->ptr.pointee ? argv[i]->ptr.pointee : st,
            };
            c->slots[sp.binding].val = inner;
            air_set(c, &f->args[i], air_val_ptr(c, sp));
            continue;
         }
         /* the caller's own resolution, with its offsets rebuilt here */
         struct air_val *bound = rebind_here(c, argv[i], i);
         if (!bound && argv[i]) {
            ok = false;
            break;
         }
         air_set(c, &f->args[i], bound);
         continue;
      }
      const struct air_type *at = f->args[i].type;
      if (at->kind == AIR_TYPE_STRUCT || at->kind == AIR_TYPE_ARRAY) {
         nir_def *lv[AIR_MAX_ARG_LEAVES];
         unsigned need = 0, taken = 0;
         if (!type_leaves(c, at, NULL, &need)) {
            ok = false;
            break;
         }
         for (unsigned k = 0; k < need; k++)
            lv[k] = nir_load_param(&c->b, pidx++);
         struct air_val *v = val_from_leaves(c, at, lv, &taken);
         if (!v) {
            ok = false;
            break;
         }
         air_set(c, &f->args[i], v);
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
   /* operand 0 is the callee; the arguments follow it. One source per NUMBER
    * inside each argument, in the order translate_callee made its parameters
    * (type_leaves): a struct or an array passed by value is not one NIR value.
    */
   for (unsigned i = 0; i < f->num_args; i++) {
      if (!is_param[i]) {
         /* a bound pointer's byte offset, if it is a value: ptr_offset_to_pass.
          * Narrowed to the 32 bits air_ptr documents an offset as and the
          * parameter declares, because a gep of an i64 index can leave a wider
          * def behind and a call source must match its parameter exactly.
          */
         nir_def *off = ptr_offset_to_pass(argv[i]);
         if (off) {
            call->params[idx++] = nir_src_for_ssa(nir_u2u32(&c->b, off));
            continue;
         }
         /* a slot's value leaves, in the order translate_callee declared them
          * (slot_leaves), and unconverted because each parameter was declared
          * from the def that fills it
          */
         nir_def *sv[AIR_MAX_SLOT_LEAVES];
         unsigned ns = 0;
         if (!slot_arg_leaves(c, argv[i], sv, &ns))
            return air_error(c, "intrinsic: a call passing a local that holds a pointer "
                                "as argument %u, whose contents this cannot hand across "
                                "a function boundary", i);
         for (unsigned k = 0; k < ns; k++)
            call->params[idx++] = nir_src_for_ssa(sv[k]);
         continue;
      }
      nir_def *lv[AIR_MAX_ARG_LEAVES];
      unsigned n = 0;
      if (!val_leaves(c, f->args[i].type, argv[i], lv, &n))
         return false;   /* val_leaves has said what the argument was */
      for (unsigned k = 0; k < n; k++)
         call->params[idx++] = nir_src_for_ssa(lv[k]);
   }
   /* The two sides derive the count from the same type with the same function,
    * so they agree -- and the agreement is checked rather than trusted, because
    * a call left one source short is a null nir_src that nir_instr_insert
    * dereferences while adding uses: a segfault, not a message.
    */
   if (idx != call->num_params)
      return air_error(c, "intrinsic: a call of %s built with %u sources for %u "
                          "parameters", name, idx, call->num_params);
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

/* A texture, a sampler or a constant buffer: an argument that is BOUND rather
 * than passed, at the binding the caller names.
 *
 * Eleven of the corpus's library functions are CoreImage's sampling helpers,
 * and they take what CoreImage hands a stitched function: a texture, a sampler
 * and a transform. Apple passes the first two as one-field structs named
 * metal::texture2d and metal::sampler -- the field being exactly the
 * address-space-1 or -2 pointer an entry point's texture or sampler argument
 * is -- and the transform as a bare constant-space pointer. A texture is not a
 * value in this frontend (section 7.4), so each of the three resolves to a
 * binding and the runtime binds a texture, a sampler and a constant buffer
 * there.
 *
 * Keyed on the struct's name, which is Apple's and is in these modules, and
 * not on "a struct with one pointer in it": the second would quietly accept
 * anything shaped alike.
 *
 * *kind is what the argument turned out to be, so a caller that was told by a
 * graph which resource an argument is can check that against the type instead
 * of trusting either one alone. Returns NULL without an error when the type is
 * none of the three -- then the argument is a value and the caller supplies it.
 */
static struct air_val *
bind_resource_argument(struct air_ctx *c, const struct air_type *t, unsigned arg,
                       const struct air_md *md, unsigned binding,
                       enum air_resource_kind *kind)
{
   const char *sname = t->kind == AIR_TYPE_STRUCT ? t->name : NULL;
   bool is_tex = sname && !strncmp(sname, "struct.metal::texture", 21);
   bool is_samp = sname && !strcmp(sname, "struct.metal::sampler");

   if (is_tex || is_samp) {
      if (t->num_members != 1) {
         air_error(c, "library: %s has %u members, not the one pointer this expects",
                   sname, t->num_members);
         return NULL;
      }
      struct air_ptr p = {
         .class = is_tex ? AIR_PTR_TEXTURE : AIR_PTR_SAMPLER,
         .binding = binding,
      };
      *kind = is_tex ? AIR_RESOURCE_TEXTURE : AIR_RESOURCE_SAMPLER;
      add_resource(c, *kind, binding, 0, true, false, arg, md);
      /* the value is the struct, so the pointer goes inside an aggregate of
       * one: the body reaches it with extractvalue
       */
      struct air_val *agg = air_val_agg(c, 1);
      agg->elems[0] = air_val_ptr(c, p);
      return agg;
   }

   if (t->kind == AIR_TYPE_POINTER && t->addrspace == 2) {
      /* the transform, as a constant buffer the runtime fills. With the
       * argument's own metadata, so the resource carries Apple's name for it --
       * "float4x4" for the transform these functions take, which is what lets a
       * harness put something meaningful there rather than the byte pattern.
       */
      struct air_ptr p = { .class = AIR_PTR_UBO, .binding = binding, .pointee = t->elem };
      *kind = AIR_RESOURCE_CONSTANT_BUFFER;
      add_resource(c, *kind, binding, 256, true, false, arg, md);
      return air_val_ptr(c, p);
   }

   *kind = AIR_RESOURCE_DEVICE_BUFFER;   /* not a bound resource: a value */
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
      bool is_ubo = t->kind == AIR_TYPE_POINTER && t->addrspace == 2;

      if (is_tex || is_samp || is_ubo) {
         /* The wrapper numbers these itself, in the order the arguments come:
          * nothing tells it otherwise, where a stitched graph is given each
          * one's bind index by the runtime (stitch_graph).
          */
         enum air_resource_kind got;
         argv[i] = bind_resource_argument(c, t, i, arg_md(in_md, i),
                                          is_tex ? next_tex : is_samp ? next_samp
                                                                      : next_ubo, &got);
         if (!argv[i])
            return false;      /* bind_resource_argument has said why */
         next_tex += is_tex;
         next_samp += is_samp;
         next_ubo += is_ubo;
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
      if (!is_param[i]) {
         /* no argument this wrapper supplies has a run-time offset -- they are
          * bindings and 16-byte slots -- but the parameter would exist if one
          * did, so the source is pushed here rather than left to the count
          * check below to catch
          */
         nir_def *off = ptr_offset_to_pass(argv[i]);
         if (off) {
            call->params[idx++] = nir_src_for_ssa(nir_u2u32(&c->b, off));
            continue;
         }
         /* and no argument this wrapper supplies is a slot either, for the same
          * reason: a slot is something a module's own code allocates
          */
         nir_def *sv[AIR_MAX_SLOT_LEAVES];
         unsigned ns = 0;
         if (!slot_arg_leaves(c, argv[i], sv, &ns))
            return air_error(c, "library: argument %u is a local holding a pointer "
                                "whose contents cannot be passed", i);
         for (unsigned k = 0; k < ns; k++)
            call->params[idx++] = nir_src_for_ssa(sv[k]);
         continue;
      }
      nir_def *a = argv[i] ? argv[i]->def : NULL;
      if (!a)
         return air_error(c, "library: argument %u has no value to pass", i);
      call->params[idx++] = nir_src_for_ssa(a);
   }
   /* Every argument here is a scalar or a vector -- the loop above refuses
    * anything else -- so each takes exactly one parameter and this holds. It is
    * checked because translate_callee decides the parameters and this decides
    * the sources, and the two would otherwise agree only by inspection.
    */
   if (idx != call->num_params)
      return air_error(c, "library: a call built with %u sources for %u parameters",
                       idx, call->num_params);
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

/* -- function stitching ----------------------------------------------------
 *
 * A graph of air.visible functions compiled into one entry point, which is
 * what Metal does with these functions and what CoreImage actually uses
 * (newLibraryWithStitchedDescriptor:, docs/air.md section 5, "The stitching
 * ABI, before any of it is built"). The wrapper above runs one body; this runs
 * a DAG of them, each node's arguments coming from another node's result, from
 * an argument of the stitched function, or from a bound resource.
 *
 * A node's arguments are the callee's OWN arguments, one for one, and not the
 * CoreImage-level ones: `_pf_sobelH` takes a `coreimage::Sampler` in Apple's
 * source and five arguments in the module, and the flattening between them is
 * CoreImage's -- its StitchedFunctionDescriptor::MetalArgumentInfo has
 * `ci_sampler_builder_args_start` and `..._end`, a RANGE of Metal arguments for
 * one of its own. So the graph this takes is the Metal-level one, which is the
 * level MTLFunctionStitchingFunctionNode.arguments is at.
 */
static const struct air_function *
visible_function_of(const struct air_module *m, const struct air_md **entry)
{
   if (module_stage(m, entry) != MESA_SHADER_NONE)
      return NULL;
   const struct air_md *e = *entry;
   if (!e->num_ops || !e->ops[0] || e->ops[0]->kind != AIR_MD_VALUE ||
       e->ops[0]->value->kind != AIR_VALUE_FUNCTION)
      return NULL;
   return (const struct air_function *)e->ops[0]->value;
}

/* One function a graph's nodes can name: read once, however many nodes call
 * it, so that two nodes calling the same function meet the same
 * air_function and translate_callee's cache applies to the second.
 */
struct stitch_fn {
   const struct air_function *f;
   const struct air_md *entry;        /* its air.visible node */
   struct air_module *m;
   struct air_global_plan *plan;      /* its module's globals, planned */
};

static bool
stitch_prepare(struct air_ctx *c, struct stitch_fn *sf, const char *name,
               struct stitch_fn *done, unsigned num_done)
{
   /* A function two nodes call is read once: the second node then meets the
    * same air_function, so translate_callee's cache gives it the same
    * nir_function and the body is translated once however often it is called.
    */
   for (unsigned i = 0; i < num_done; i++) {
      if (done[i].f && done[i].f->base.name && !strcmp(done[i].f->base.name, name)) {
         *sf = done[i];
         return true;
      }
   }

   const struct air_stitch_function *src = NULL;
   for (unsigned i = 0; i < c->options->num_stitch_functions; i++) {
      const struct air_stitch_function *k = &c->options->stitch_functions[i];
      if (k->name && name && !strcmp(k->name, name)) {
         src = k;
         break;
      }
   }
   if (!src)
      return air_error(c, "stitch: the graph names %s, which is not one of the %u "
                          "functions given to stitch from", name ? name : "(nothing)",
                       c->options->num_stitch_functions);

   const char *why = NULL;
   sf->m = air_module_read(c->mem_ctx, src->bitcode, src->size, &why);
   if (!sf->m)
      return air_error(c, "stitch: %s: reader: %s", name, why ? why : "?");
   sf->f = visible_function_of(sf->m, &sf->entry);
   if (!sf->f)
      return air_error(c, "stitch: %s is not an air.visible function of its module, so "
                          "it is not a function a graph can call", name);
   if (sf->f->base.name && strcmp(sf->f->base.name, name))
      return air_error(c, "stitch: the module given for %s holds %s instead", name,
                       sf->f->base.name);

   /* its module's globals, planned in its own module: plan_globals indexes by
    * the module's own global numbering, so the plan belongs to the module and
    * not to the shader, and translate_callee materialises from whichever plan
    * is current when it runs.
    */
   const struct air_module *saved_m = c->m;
   struct air_global_plan *saved_plan = c->global_plan;
   c->m = sf->m;
   c->global_plan = NULL;
   plan_globals(c);
   sf->plan = c->global_plan;
   if (!c->error[0])
      run_static_initializers(c);     /* in the entry point, before any call */
   c->m = saved_m;
   c->global_plan = saved_plan;
   return !c->error[0];
}

/* The air.visible_input node list of a function, which arg_md searches. */
static const struct air_md *
stitch_inputs_md(const struct stitch_fn *sf)
{
   return sf->entry->num_ops > 2 ? sf->entry->ops[2] : NULL;
}

/* Its result's metadata: air.visible_output, which carries Apple's own name
 * for the type and is what the result buffer is labelled with.
 */
static const struct air_md *
stitch_output_md(const struct stitch_fn *sf)
{
   const struct air_md *rn = sf->entry->num_ops > 1 ? sf->entry->ops[1] : NULL;
   return rn && rn->num_ops ? rn->ops[0] : NULL;
}

/* The result type of a node, which is what an argument fed from it receives. */
static const struct air_type *
stitch_result_type(const struct stitch_fn *sf)
{
   return sf->f->fn_type->elem;
}

static bool
stitch_graph(struct air_ctx *c)
{
   const struct air_stitch_graph *g = c->options->stitch;
   if (!g->num_nodes)
      return air_error(c, "stitch: a graph with no nodes");
   if (g->output_node >= g->num_nodes)
      return air_error(c, "stitch: the output node is %u of %u nodes", g->output_node,
                       g->num_nodes);

   struct stitch_fn *fns = rzalloc_array(c->mem_ctx, struct stitch_fn, g->num_nodes);
   nir_def **value = rzalloc_array(c->mem_ctx, nir_def *, g->num_nodes);

   /* The stitched function's own arguments, and the resources its nodes bind.
    * The argument slots are the library wrapper's -- argument i at byte 16*i of
    * device buffer 0, the result in device buffer 1 -- on purpose: a one-node
    * graph then has to produce exactly the words the wrapper produces for the
    * same function, which is a comparison that already has a reference.
    */
   unsigned num_inputs = 0;
   for (unsigned n = 0; n < g->num_nodes; n++)
      for (unsigned a = 0; a < g->nodes[n].num_args; a++)
         if (g->nodes[n].args[a].kind == AIR_STITCH_INPUT)
            num_inputs = MAX2(num_inputs, g->nodes[n].args[a].index + 1);

   add_resource(c, AIR_RESOURCE_DEVICE_BUFFER, 0, 16 * MAX2(num_inputs, 1),
                true, false, ~0u, NULL);

   for (unsigned n = 0; n < g->num_nodes; n++) {
      const struct air_stitch_node *node = &g->nodes[n];
      if (!stitch_prepare(c, &fns[n], node->function, fns, n))
         return false;
      const struct air_function *f = fns[n].f;

      if (node->num_args != f->num_args)
         return air_error(c, "stitch: node %u gives %s %u arguments and it takes %u",
                          n, node->function, node->num_args, f->num_args);

      struct air_val **argv = rzalloc_array(c->mem_ctx, struct air_val *,
                                            MAX2(f->num_args, 1));
      bool *is_param = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_args, 1));

      for (unsigned a = 0; a < f->num_args; a++) {
         const struct air_type *t = f->args[a].type;
         struct air_stitch_arg src = node->args[a];
         enum air_resource_kind want;

         switch (src.kind) {
         case AIR_STITCH_TEXTURE:
         case AIR_STITCH_SAMPLER:
         case AIR_STITCH_BUFFER: {
            want = src.kind == AIR_STITCH_TEXTURE ? AIR_RESOURCE_TEXTURE :
                   src.kind == AIR_STITCH_SAMPLER ? AIR_RESOURCE_SAMPLER :
                                                    AIR_RESOURCE_CONSTANT_BUFFER;
            enum air_resource_kind got;
            argv[a] = bind_resource_argument(c, t, a,
                                             arg_md(stitch_inputs_md(&fns[n]), a),
                                             src.index, &got);
            if (!argv[a])
               return c->error[0] ? false :
                  air_error(c, "stitch: node %u binds argument %u of %s as a resource "
                               "and its type is not a texture, a sampler or a "
                               "constant pointer", n, a, node->function);
            /* The graph says which resource it is and the module's own type
             * says which it can be. Both are checked against each other
             * because a bind index that reached the wrong KIND of resource
             * would still compile and still read bytes.
             */
            if (got != want)
               return air_error(c, "stitch: node %u binds argument %u of %s as a %s and "
                                   "the module declares it a %s", n, a, node->function,
                                want == AIR_RESOURCE_TEXTURE ? "texture" :
                                want == AIR_RESOURCE_SAMPLER ? "sampler" : "buffer",
                                got == AIR_RESOURCE_TEXTURE ? "texture" :
                                got == AIR_RESOURCE_SAMPLER ? "sampler" : "buffer");
            is_param[a] = false;
            continue;
         }

         case AIR_STITCH_NODE: {
            if (src.index >= n)
               return air_error(c, "stitch: node %u takes the result of node %u, which "
                                   "is not an earlier node: a graph is given in "
                                   "dependency order", n, src.index);
            const struct air_type *rt = stitch_result_type(&fns[src.index]);
            if (air_nir_components(rt) != air_nir_components(t) ||
                air_nir_bit_size(c, rt) != air_nir_bit_size(c, t))
               return air_error(c, "stitch: node %u takes node %u's result into an "
                                   "argument of a different shape (%ux%u into %ux%u)",
                                n, src.index, air_nir_components(rt),
                                air_nir_bit_size(c, rt), air_nir_components(t),
                                air_nir_bit_size(c, t));
            argv[a] = air_val_ssa(c, value[src.index]);
            is_param[a] = true;
            continue;
         }

         case AIR_STITCH_INPUT: {
            const struct air_type *e = t->kind == AIR_TYPE_VECTOR ? t->elem : t;
            bool numeric = e && (e->kind == AIR_TYPE_FLOAT || e->kind == AIR_TYPE_HALF ||
                                 e->kind == AIR_TYPE_INT);
            if (!numeric || (t->kind != AIR_TYPE_VECTOR && t->kind != AIR_TYPE_FLOAT &&
                             t->kind != AIR_TYPE_HALF && t->kind != AIR_TYPE_INT))
               return air_error(c, "stitch: node %u takes the stitched function's "
                                   "argument %u into argument %u of %s, which is "
                                   "neither a scalar nor a vector", n, src.index, a,
                                node->function);
            if (air_nir_components(t) * air_nir_bit_size(c, t) > 16 * 8)
               return air_error(c, "stitch: argument %u does not fit the 16-byte slot "
                                   "the stitched function's arguments are read from",
                                src.index);
            struct air_ptr p = { .class = AIR_PTR_SSBO, .binding = 0, .pointee = t };
            nir_def *v = air_load_ptr(c, air_ptr_add(c, p, nir_imm_int(&c->b,
                                                                       16 * src.index)),
                                      t, 16);
            if (!v)
               return false;
            argv[a] = air_val_ssa(c, v);
            is_param[a] = true;
            continue;
         }

         case AIR_STITCH_GRID_POSITION:
            /* In the API and not implemented, which is not the same as
             * forgotten: no case here dispatches more than one thread, so a
             * grid position would be translated and never measured, and an
             * untested path is worse than a refusal.
             */
            return air_error(c, "stitch: node %u takes thread_position_in_grid, which "
                                "is not implemented", n);
         }
         return air_error(c, "stitch: node %u argument %u has an unknown source", n, a);
      }

      /* its module is current while its body is translated: the globals the
       * body reads are at the offsets ITS plan decided
       */
      const struct air_module *saved_m = c->m;
      struct air_global_plan *saved_plan = c->global_plan;
      c->m = fns[n].m;
      c->global_plan = fns[n].plan;
      nir_function *fn = translate_callee(c, f, argv, is_param);
      c->m = saved_m;
      c->global_plan = saved_plan;
      if (!fn)
         return false;   /* translate_callee has said why */

      const struct air_type *rt = stitch_result_type(&fns[n]);
      if (!rt || rt->kind == AIR_TYPE_VOID)
         return air_error(c, "stitch: node %u calls %s, which returns nothing, so no "
                             "other node can take its result", n, node->function);
      const struct air_type *re = rt->kind == AIR_TYPE_VECTOR ? rt->elem : rt;
      if (!re || (re->kind != AIR_TYPE_FLOAT && re->kind != AIR_TYPE_HALF &&
                  re->kind != AIR_TYPE_INT))
         return air_error(c, "stitch: node %u calls %s, whose result is neither a "
                             "scalar nor a vector", n, node->function);

      unsigned bits = air_nir_bit_size(c, rt);
      unsigned comps = air_nir_components(rt);
      const struct glsl_type *gt =
         glsl_vector_type(bits == 16 ? GLSL_TYPE_FLOAT16 :
                          bits == 64 ? GLSL_TYPE_UINT64 : GLSL_TYPE_UINT, comps);
      nir_variable *tmp = nir_local_variable_create(c->b.impl, gt, "node");

      nir_call_instr *call = nir_call_instr_create(c->nir, fn);
      unsigned idx = 0;
      call->params[idx++] = nir_src_for_ssa(&nir_build_deref_var(&c->b, tmp)->def);
      for (unsigned a = 0; a < f->num_args; a++) {
         if (!is_param[a]) {
            /* as in the wrapper above: a bound pointer takes no parameter
             * unless its byte offset is a value (ptr_offset_to_pass)
             */
            nir_def *off = ptr_offset_to_pass(argv[a]);
            if (off) {
               call->params[idx++] = nir_src_for_ssa(nir_u2u32(&c->b, off));
               continue;
            }
            nir_def *sv[AIR_MAX_SLOT_LEAVES];
            unsigned ns = 0;
            if (!slot_arg_leaves(c, argv[a], sv, &ns))
               return air_error(c, "stitch: node %u argument %u is a local holding a "
                                   "pointer whose contents cannot be passed", n, a);
            for (unsigned k = 0; k < ns; k++)
               call->params[idx++] = nir_src_for_ssa(sv[k]);
            continue;
         }
         nir_def *v = argv[a] ? argv[a]->def : NULL;
         if (!v)
            return air_error(c, "stitch: node %u argument %u has no value to pass", n, a);
         call->params[idx++] = nir_src_for_ssa(v);
      }
      /* as in the wrapper above: one parameter per argument holds here because
       * every argument is a scalar or a vector, and is checked anyway
       */
      if (idx != call->num_params)
         return air_error(c, "stitch: node %u built with %u sources for %u parameters",
                          n, idx, call->num_params);
      nir_builder_instr_insert(&c->b, &call->instr);
      value[n] = nir_load_var(&c->b, tmp);
   }

   /* the result buffer, declared once the output node's own metadata is in
    * hand: it carries Apple's air.arg_type_name for the result, which is how a
    * harness knows what the bytes coming back are
    */
   const struct air_type *rt = stitch_result_type(&fns[g->output_node]);
   add_resource(c, AIR_RESOURCE_DEVICE_BUFFER, 1, 16, false, true, ~0u,
                stitch_output_md(&fns[g->output_node]));
   struct air_ptr out = { .class = AIR_PTR_SSBO, .binding = 1, .pointee = rt };
   if (!air_store_ptr(c, out, value[g->output_node], 16))
      return false;

   c->info->entry_point = g->name ? g->name : "stitched";
   c->nir->info.name = ralloc_strdup(c->nir, c->info->entry_point);
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
 * Refused when they name different resources. This COULD go through the
 * AIR_VAL_PTR_SELECT machinery that already loads both sides and selects the
 * values, but nothing in reach needs it, a texture or a sampler is not a value
 * that path can load, and an untested path is worse than a refusal.
 *
 * ONE RESOURCE AT DIFFERENT OFFSETS is not that, and is carried. Its offset is
 * a 32-bit number like any phi's value, so it gets a register: each edge
 * stores the offset its operand has, and the phi is the resource at the
 * register's value. That is what LLVM's duplicated GEP needs when the copies
 * are two defs (mediaserverd's kernelComputeStage1: the same member of the
 * same buffer, computed in each of two blocks, which same_ptr cannot prove
 * equal), and what a pointer stepped round a loop needs -- an operand over a
 * back edge is then stored when the edge is, and checked to name the same
 * resource there (the phi stores below), not assumed to.
 *
 * `live` is why this is sound rather than optimistic: the blocks are
 * walked in reverse post order, so an operand coming in over a BACK edge has
 * not been translated yet. Skipping an operand merely because it is missing
 * would accept a loop-carried pointer by ignoring the edge that changes it, so
 * a missing operand from a live block is a refusal and only an edge the entry
 * cannot take at all is passed over.
 *
 * `live` is narrower than CFG reachability and that is the point: a block
 * reached only through a branch whose condition is decided while translating
 * is not entered, so a phi between two different resources has ONE operand
 * and resolves. That is how a shader specialised on its function constants
 * gets through here (translate_body, and docs/air.md section 3), and nothing
 * about this function had to change for it -- only what it is told is live.
 */
/* An `undef` or `poison` operand: the phi may be anything over that edge, so
 * it is whatever its other operands name. LLVM leaves one when it splits a
 * pointer into a phi per path and only one path uses each (MPS's
 * cnnConvArray_*: two threadgroup pointers, each undef over the other's edge),
 * and counting it as a resource of its own refused a phi that names one.
 */
static bool
undef_operand(const struct air_value *v)
{
   if (!v || v->kind != AIR_VALUE_CONSTANT)
      return false;
   const enum air_const_kind k = ((const struct air_constant *)v)->ckind;
   return k == AIR_CONST_UNDEF || k == AIR_CONST_POISON;
}

static bool
resolve_pointer_phi(struct air_ctx *c, const struct air_inst *in, const bool *live,
                    const int *taken_succ, unsigned my_block)
{
   struct air_val *first = NULL;
   bool carried = false;   /* an operand over a loop edge, or offsets same_ptr cannot equate */
   for (unsigned k = 0; k < in->num_operands; k++) {
      if (!in->targets[k] || !live[in->targets[k]->index])
         continue;
      /* AND THE EDGE, not just the block. A block whose branch folded still
       * EXECUTES -- it is live -- but one of its two edges does not exist, and
       * a phi on the far side of the one that does not must not count the
       * operand arriving over it. Getting this wrong is not academic: it is
       * exactly what the fifteen texture phis of fixed_frag_lph_cph look like,
       * where `br i1 %144, label %null, label %join` folds to the null side
       * and `%join`'s phi still saw an operand from the branching block. The
       * refusal then said "a phi between pointers that name different places"
       * about a shader that names one.
       */
      int taken = taken_succ[in->targets[k]->index];
      if (taken >= 0 && taken != (int)my_block)
         continue;
      if (undef_operand(in->operands[k]))
         continue;
      struct hash_entry *e = _mesa_hash_table_search(c->values, in->operands[k]);
      if (!e) {
         carried = true;   /* over a loop edge: stored, and checked, when the edge is */
         continue;
      }
      struct air_val *v = e->data;
      if (!v || v->kind != AIR_VAL_PTR)
         return air_error(c, "instruction: a phi of pointers one of whose operands is "
                             "%s", v && v->kind == AIR_VAL_PTR_SELECT
                                      ? "itself a pointer chosen at run time"
                                      : "not a resolved pointer");
      if (!first)
         first = v;
      else if (first->ptr.class != v->ptr.class || first->ptr.binding != v->ptr.binding ||
               first->ptr.is_image != v->ptr.is_image)
         return air_error(c, "instruction: a phi between pointers that name different "
                             "places, which would have to be chosen at run time");
      else if (!same_ptr(&first->ptr, &v->ptr))
         carried = true;
   }
   if (!first && carried)
      return air_error(c, "instruction: a phi of pointers whose operand comes in over "
                          "a loop edge, so which resource it is depends on the "
                          "iteration");   /* every live operand over a back edge: nothing to name the resource by */
   if (!first)
      return air_error(c, "instruction: a phi of pointers with no reachable operand");
   if (!carried) {
      air_set(c, &in->base, first);
      return true;
   }
   /* memory alone has an offset a register can carry: a texture, a sampler or a
    * slot is a handle, and an argument buffer's offset is resolved to a member
    * while translating, which a value known only at run time cannot be
    */
   const enum air_ptr_class cl = first->ptr.class;
   if (first->ptr.is_image || (cl != AIR_PTR_UBO && cl != AIR_PTR_SSBO && cl != AIR_PTR_SHARED &&
                               cl != AIR_PTR_SCRATCH && cl != AIR_PTR_CONSTANT_DATA))
      return air_error(c, "instruction: a phi of a pointer that is not into memory (class %u) "
                          "and changes round a loop or is computed twice", (unsigned)cl);
   nir_builder entry = nir_builder_at(nir_before_impl(c->b.impl));
   nir_def *reg = nir_decl_reg(&entry, 1, 32, 0);
   _mesa_hash_table_insert(c->phi_regs, in, reg);
   struct air_ptr p = first->ptr;
   p.offset = nir_load_reg(&c->b, reg);
   air_set(c, &in->base, air_val_ptr(c, p));
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
         /* A phi of a width NIR has no register for is exact: the register
          * is the next width up and a phi only COPIES, so a canonical value
          * in is a canonical value out (narrow_int_bits). What may be done
          * with it afterwards is narrow_int_gate's business, and every use
          * of it goes through there.
          */
         nir_def *reg = nir_decl_reg(&entry, air_nir_components(in->base.type),
                                     air_narrow_bits(c, in->base.type), 0);
         _mesa_hash_table_insert(c->phi_regs, in, reg);
      }
   }

   /* NIR's first block is the function's first; the rest are made as they
    * are reached, and nir_lower_goto_ifs puts them in order afterwards
    */
   _mesa_hash_table_insert(c->blocks, &f->blocks[0], nir_start_block(c->b.impl));

   unsigned *order = rzalloc_array(c->mem_ctx, unsigned, MAX2(f->num_blocks, 1));
   unsigned num_order = reverse_post_order(c, f, order);

   /* WHICH BLOCKS ARE ENTERED, which is not the same as which are reachable
    * in the CFG. A branch whose condition is already decided while translating
    * -- a function constant, through the program-scope variable the module's
    * air.static_init copies it into -- has one edge and not two, and the block
    * on the other side is never entered. It is then not translated either,
    * because translating it would build instructions for a path that does not
    * exist and, worse, would make a phi of pointers see two operands where the
    * shader has one (resolve_pointer_phi).
    *
    * This is filled as the walk goes rather than before it: the condition is a
    * translated value, so it cannot be known earlier. Reverse post order is
    * what makes that work -- every block but a loop header is reached after
    * all of its predecessors, so by the time a block is visited every forward
    * edge into it has been decided. A block nothing live branches to stays
    * false and is skipped; it gets no NIR block, no phi cursor, and the phi
    * store loop at the end already passes over an edge whose source has no
    * cursor.
    *
    * It is also strictly narrower than what stood here, which marked every
    * block of `order` live: a module that folds nothing gets exactly the old
    * behaviour, because then every edge of every branch is taken.
    */
   bool *live = rzalloc_array(c->mem_ctx, bool, MAX2(f->num_blocks, 1));
   if (num_order)
      live[order[0]] = true;

   /* THE ONE SUCCESSOR A BLOCK ACTUALLY BRANCHES TO, per block, or -1 for "all
    * of them". Recorded rather than derived, because it is the only thing a
    * phi on the far side can use: a block whose terminator folded still
    * EXECUTES -- it is live -- and the operand arriving over an edge it does
    * not take must not be counted.
    *
    * It is the taken successor and not the untaken ones because a folded
    * SWITCH prunes every case but one, and one number says that as cheaply as
    * it says a branch's single pruned edge.
    */
   int *taken_succ = rzalloc_array(c->mem_ctx, int, MAX2(f->num_blocks, 1));
   for (unsigned i = 0; i < f->num_blocks; i++)
      taken_succ[i] = -1;

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
            if (!resolve_pointer_phi(c, in, live, taken_succ, bi))
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
      switch (term->op) {
      case AIR_OP_RET:
         if (!emit_return(c, term))
            return false;
         nir_goto(&c->b, c->b.impl->end_block);
         break;
      case AIR_OP_BR:
         if (term->num_targets == 1) {
            live[term->targets[0]->index] = true;
            nir_goto(&c->b, nir_block_of(c, term->targets[0]));
         } else {
            nir_def *cond = air_get_ssa(c, term->operands[0]);
            if (!cond)
               return false;
            bool taken;
            if (term->targets[0] == term->targets[1]) {
               live[term->targets[0]->index] = true;
               nir_goto(&c->b, nir_block_of(c, term->targets[0]));
            } else if (cond_const(c, cond, &taken)) {
               /* ONE EDGE, because the condition is a constant here and not
                * merely foldable later. The other block is left out of `live`
                * and never translated, which is what lets a phi of pointers on
                * the far side resolve; emitting nir_goto_if with a constant
                * and trusting nir_opt_dead_cf would be too late for that, and
                * would also leave the dead block's instructions translated.
                */
               struct air_block *to = term->targets[taken ? 0 : 1];
               live[to->index] = true;
               taken_succ[bi] = (int)to->index;
               nir_goto(&c->b, nir_block_of(c, to));
            } else {
               live[term->targets[0]->index] = true;
               live[term->targets[1]->index] = true;
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
         /* A SWITCH WHOSE SELECTOR IS ALREADY DECIDED takes one case, exactly
          * as a branch does.
          *
          * What stood here said the opposite, and said why: "the only switches
          * in this corpus are the texture/blend/colour function dispatches of
          * the GL-on-Metal uber shaders, where the selector is a uchar function
          * constant and folding it would delete thirty-odd blocks at once -- a
          * much larger change in what is compiled than the branch fold, for no
          * module that the branch fold does not already reach." The first half
          * is true and the last clause is FALSE, and fixed_frag_lph_cph is the
          * counterexample: its asg77 filter case loads through the weights
          * pointer, that pointer is `select fc_has_asg_77_weights, null, %14`
          * and folds to NULL, and the case is reachable only through a switch
          * on `fc_filter0`. Leaving the switch alone therefore translated a
          * block the shader never enters and refused the module for a null
          * dereference inside it -- a true sentence about code that does not
          * run. The branch fold could not reach that; only this can.
          *
          * The default is targets[0] and the cases follow, so a selector that
          * matches no case takes the default -- which is what makes folding
          * total rather than a guess: there is always exactly one answer.
          */
         uint64_t sel;
         if (uint_const(c, cond, &sel)) {
            const struct air_block *to = term->targets[0];   /* the default */
            for (unsigned k = 1; k < term->num_targets; k++) {
               nir_def *cv = air_get_ssa(c, term->operands[k]);
               uint64_t cval;
               if (!cv)
                  return false;
               if (uint_const(c, cv, &cval) && cval == sel) {
                  to = term->targets[k];
                  break;
               }
            }
            live[to->index] = true;
            taken_succ[bi] = (int)to->index;
            nir_goto(&c->b, nir_block_of(c, (struct air_block *)to));
            break;
         }
         for (unsigned k = 0; k < term->num_targets; k++)
            live[term->targets[k]->index] = true;
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

      /* The same reason as inside the instruction loop, for the TERMINATOR,
       * which is translated here and not there. Without this a refusal raised
       * while building a switch's case constants was only noticed at the first
       * instruction of the next block in reverse-post-order, and the report
       * then pointed at that instruction: resolve_sample_fragment's 3-bit
       * switch in block 0 was attributed to a shufflevector in block 26, which
       * is a location that sends the reader to the wrong line of the module.
       */
      if (c->error[0]) {
         char where[64];
         snprintf(where, sizeof(where), " (block %u, terminator, opcode %u)",
                  bi, term->op);
         if (strlen(c->error) + strlen(where) < sizeof(c->error))
            strcat(c->error, where);
         return false;
      }
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
            nir_def *val;
            if (in->base.type->kind == AIR_TYPE_POINTER && undef_operand(in->operands[k]))
               continue;   /* the register holds whatever it held: undef over this edge */
            if (in->base.type->kind == AIR_TYPE_POINTER) {
               /* a carried pointer phi (resolve_pointer_phi): the operand's offset, once
                * it is known to name the phi's resource -- over a loop edge this is the
                * first time it is seen at all
                */
               struct hash_entry *pe = _mesa_hash_table_search(c->values, &in->base);
               struct hash_entry *oe = _mesa_hash_table_search(c->values, in->operands[k]);
               const struct air_val *pv = pe ? pe->data : NULL, *ov = oe ? oe->data : NULL;
               if (!pv || !ov || ov->kind != AIR_VAL_PTR || ov->ptr.class != pv->ptr.class ||
                   ov->ptr.binding != pv->ptr.binding || ov->ptr.is_image != pv->ptr.is_image)
                  return air_error(c, "instruction: a phi of pointers whose operand over "
                                      "an edge names another resource, so which one it is "
                                      "depends on the path taken");
               val = ov->ptr.offset ? nir_u2u32(&c->b, ov->ptr.offset) : nir_imm_int(&c->b, 0);
            } else {
               val = air_get_ssa(c, in->operands[k]);
            }
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

/* The NAME and the required-ness of a function constant, by index: the other
 * direction from function_constant_index, and needed where only the index is
 * in hand -- the load path, which has a binding and not a global.
 *
 * `!{ptr @g, !"uint", !"name", i32 index, i1 required}`: the name is the last
 * string before the first integer, and the required flag the boolean after it.
 * Matched that way rather than by position, because the two keys this reads
 * are the two whose places are attested across the corpus and nothing says the
 * node cannot grow an operand.
 */
static const char *
function_constant_decl(struct air_ctx *c, unsigned index, bool *required)
{
   const struct air_named_md *n = named_md(c->m, "air.function_constants");
   for (unsigned i = 0; i < (n ? n->num_ops : 0); i++) {
      const struct air_md *node = n->ops[i];
      if (!node || node->kind != AIR_MD_NODE)
         continue;
      const char *name = NULL;
      for (unsigned k = 0; k < node->num_ops; k++) {
         const char *str = md_str(node, k);
         if (str) {
            name = str;
            continue;
         }
         uint64_t idx;
         if (!md_int(node, k, &idx))
            continue;
         if (idx != index)
            break;         /* a different constant: on to the next node */
         if (required) {
            uint64_t req = 0;
            *required = md_int(node, k + 1, &req) && req != 0;
         }
         return name;
      }
   }
   return NULL;
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

/* Does this value, or any constant built out of it, name that global?
 *
 * A constant expression is where a global's address escapes without any
 * instruction mentioning it: `getelementptr (%struct, ptr @g, 0, 3)` is a
 * CONSTANT in the bitcode, and the instruction that uses it names the
 * expression and not @g. A scan that only compared instruction operands would
 * therefore have called @_ZL25fc_fragment_shader_config -- whose every field
 * is reached through exactly that shape -- a global nothing but whole loads
 * touch, and folded a byte of it over the struct the shader actually reads.
 */
static bool
const_mentions(const struct air_value *v, const struct air_value *target,
               unsigned depth)
{
   if (!v || depth == 0)
      return false;
   if (v == target)
      return true;
   const struct air_constant *k = air_value_as_constant(v);
   if (!k)
      return false;
   /* ON THE KIND, and not on whichever count happens to be non-zero.
    * air_constant shares `num_elems` between an AGGREGATE, whose members are
    * in `elems`, and DATA, whose numbers are in `data` and whose `elems` is
    * NULL (air_module.c, CST_CODE_DATA) -- so walking `elems` whenever
    * num_elems is set dereferences NULL on every string and every
    * `<4 x i8>` data constant. That is what it did: 42 modules of the corpus
    * died in here with SIGSEGV, every one of them a module whose
    * air.static_init made this run at all.
    */
   switch (k->ckind) {
   case AIR_CONST_AGGREGATE:
      for (unsigned i = 0; i < k->num_elems; i++)
         if (const_mentions(k->elems[i], target, depth - 1))
            return true;
      return false;
   case AIR_CONST_EXPR:
      for (unsigned i = 0; i < k->num_operands; i++)
         if (const_mentions(k->operands[i], target, depth - 1))
            return true;
      return false;
   default:
      /* INT, FLOAT, NULL, UNDEF, POISON, DATA: numbers, and no value in them */
      return false;
   }
}

/* THE RESERVED SLOT A NULL TEXTURE LIVES IN -- road B of docs/air.md section 3,
 * the frontend's half of it.
 *
 * `air.get_null_texture_2d` and its fifteen siblings are what a Metal shader
 * asks for when the pipeline bound no image, and there is no index here that
 * means "nothing bound": a texture is Metal's own `[[texture(n)]]`, resolved
 * while translating, which is what lets a sample be one instruction with no
 * descriptor indirection. So one index past the module's own is reserved for
 * it and the resource is flagged, for the runtime to bind a 1x1 texture of
 * zeros at (air_resource.null_texture in nir_air.h, which carries what that
 * rests on).
 *
 * ONE PAST THE HIGHEST DECLARED INDEX, and that is deliberately NOT Apple's
 * rule. Apple uses the texture COUNT -- createTextureHeapIndexArray stores the
 * SmallVector's own Size (0x1bbe988b4) -- which is the same number only when
 * the indices run 0..n-1. For a module that declares 0, 1 and 5, Apple's rule
 * gives 3, which is a slot nothing declared but which a later version of the
 * same shader could; one past the highest gives 6, which cannot collide with
 * anything the module asked for. The safe one is chosen over the faithful one
 * because a collision here binds a zero texture over a real image in silence.
 *
 * REFUSED BY NAME when it does not fit, rather than wrapped. AIR_MAX_TEXTURES
 * is freedreno's own 16 (view_seqno[16] in fd6_texture_state), and
 * fixed_frag_lph_cph declares 0..14 -- so its reserved slot is 15, the last
 * one there is, and it fits with nothing to spare. A module that declares
 * sixteen textures gets a refusal here and not an index of 16: the runtime's
 * own check would otherwise refuse it too, but with the sentence "binds a
 * texture at index 16", which blames the module for a slot THIS function
 * added. That is the fourth fixed array of the day found smaller than
 * something real, and the only one where the array's own maximum is the
 * reserved slot itself.
 */
int
air_null_texture_slot(struct air_ctx *c)
{
   if (c->null_texture_index >= 0)
      return c->null_texture_index;

   struct air_shader_info *info = c->info;
   int highest = -1;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      if (r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE)
         highest = MAX2(highest, (int)r->index);
   }
   unsigned slot = (unsigned)(highest + 1);
   if (slot >= AIR_MAX_TEXTURES) {
      air_error(c, "intrinsic: a null texture needs a reserved slot and this module "
                   "declares textures up to %d, so the slot would be %u -- past the %u "
                   "this GPU's descriptor table has (AIR_MAX_TEXTURES)",
                highest, slot, AIR_MAX_TEXTURES);
      return -1;
   }

   info->resources = reralloc(c->mem_ctx, info->resources, struct air_resource,
                              info->num_resources + 1);
   /* Not through add_resource: there is no metadata node behind this one, and
    * the two "no group"/"no argument buffer" fields have to be written rather
    * than left zero -- a zeroed raster_order_group reads as group 0 and a
    * zeroed argbuf_set as "member 0 of the set at Metal index 0", which is the
    * same care plan_globals takes for an inline sampler.
    */
   info->resources[info->num_resources++] = (struct air_resource) {
      .kind = AIR_RESOURCE_TEXTURE, .index = slot, .read = true,
      .null_texture = true, .param = ~0u, .name = "(null texture)",
      .raster_order_group = -1, .argbuf_set = ~0u, .argbuf_id = ~0u,
   };
   c->null_texture_index = (int)slot;
   return (int)slot;
}

/* Which program-scope variables are a VALUE rather than memory.
 *
 * This is the whole of what "specialise the shader on its function constants"
 * needs from the frontend, and it is a census rather than an optimisation: the
 * GL-on-Metal uber shaders decide 58 function constants in an air.static_init
 * constructor, copy them into program-scope variables, and then branch on
 * those variables fifteen times to choose between a bound texture and a null
 * one. plan_globals puts such a variable in SCRATCH, so what the body sees is
 * a load_scratch -- opaque -- and every one of those branches survives
 * translation, which is what left the two modules with fifteen phis between
 * two different textures (docs/air.md section 3). Metal does not compile them
 * that way: it specialises at pipeline creation, and this frontend already has
 * every function constant as an immediate (AIR_PTR_FUNCTION_CONSTANT). The
 * only thing in the way was the round trip through memory.
 *
 * A global qualifies only when all four hold, and each one is a way of being
 * wrong that was considered rather than a box ticked:
 *
 *   - its type is a scalar integer of 8, 16, 32 or 64 bits. Not a float, not a
 *     vector, not an aggregate: `bits` has to BE the value, and a narrower or
 *     odd width would go through air_narrow_bits and stop being the thing a
 *     later load wants.
 *   - every instruction that names it names it as the whole pointer of a load
 *     or of a store, and the type of that load or store is the global's own.
 *     A getelementptr, a call argument, a stored VALUE, a phi, a select: all
 *     disqualify, because then some other access reaches the same bytes and
 *     the value and the memory could disagree.
 *   - no constant expression and no other global's initialiser mentions it, so
 *     its address does not escape into something this scan cannot follow.
 *   - exactly one store, and that store is in a function whose section is
 *     air.static_init. One store is what makes the value independent of where
 *     in the shader the load is; the section is what makes it run before the
 *     body, which run_static_initializers inlines at the top of the entry
 *     point. A variable written by the body is a variable, and stays memory.
 *
 * What this does NOT decide is whether the value is a constant at all. That is
 * known only when the store is translated, so `fold` means "a load of this may
 * be answered" and `known` means "and here is the answer"; a store of
 * something that is not constant simply never sets `known`, and the scratch
 * underneath answers as it always did.
 *
 * AND IT DECIDES NOTHING ABOUT WHOSE VALUE IT IS. The constant comes from
 * air_load_ptr's AIR_PTR_FUNCTION_CONSTANT case, which answers with
 * air_constant_value.bits and does not look at `supplied` -- and
 * air_module_function_constants registers every constant a module DECLARES
 * with bits = 0 and supplied = false, so in every harness in tools/air this
 * folds a shader on zeros that nobody set. That is not a new invention: the
 * unfolded shader took the same branch for the same reason, at run time, off
 * an immediate the same case produced. What changes is where the consequence
 * lives -- a block that is not compiled rather than a branch that is not
 * taken -- so the specialisation is exactly as good as the constants the
 * runtime hands in and no better.
 *
 * It matters most for the one kind of constant that is allowed to be absent.
 * The law (`!{ptr, !"type", !"name", i32 index, i1 required}`) is that a
 * declaration ending `i1 false` is always guarded by
 * air.is_function_constant_defined and one ending `i1 true` never is: 58 of
 * 58 in fixed_frag_lph_cph, 24 required and unguarded against 34 optional and
 * guarded, no exceptions. Fourteen of that module's fifteen texture
 * predicates are REQUIRED, so Metal obliges the pipeline to supply them and
 * folding them is specialisation. The fifteenth, fc_has_lut_tex at index 23,
 * is optional -- and its constructor copies the RAW load into the variable
 * rather than the guarded one, so the module itself reads an unsupplied
 * constant as whatever the implementation gives. This passes that reading
 * through unchanged instead of deciding it, which is the only honest thing to
 * do from here: the decision belongs with whoever fills `supplied` in.
 */
static void
plan_global_folding(struct air_ctx *c)
{
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      struct air_global_plan *pl = &c->global_plan[i];
      const struct air_global *g = c->m->globals[i];
      if (pl->skip || pl->class != AIR_PTR_SCRATCH)
         continue;

      const struct air_type *t = g->value_type;
      if (!t || t->kind != AIR_TYPE_INT ||
          (t->bits != 8 && t->bits != 16 && t->bits != 32 && t->bits != 64))
         continue;

      bool ok = true;
      unsigned stores = 0;

      for (unsigned k = 0; ok && k < c->m->num_globals; k++) {
         if (k != i && const_mentions(c->m->globals[k]->init, &g->base, 8))
            ok = false;
      }

      for (unsigned fi = 0; ok && fi < c->m->num_functions; fi++) {
         const struct air_function *f = c->m->functions[fi];
         bool is_init = f->section && !strcmp(f->section, "air.static_init");
         for (unsigned bi = 0; ok && bi < f->num_blocks; bi++) {
            const struct air_block *blk = &f->blocks[bi];
            for (unsigned ii = 0; ok && ii < blk->num_insts; ii++) {
               const struct air_inst *in = blk->insts[ii];
               for (unsigned oi = 0; ok && oi < in->num_operands; oi++) {
                  const struct air_value *v = in->operands[oi];
                  if (v == &g->base) {
                     if (in->op == AIR_OP_LOAD && oi == 0 &&
                         in->base.type == t) {
                        continue;
                     }
                     if (in->op == AIR_OP_STORE && oi == 1 &&
                         in->operands[0]->type == t && is_init) {
                        stores++;
                        continue;
                     }
                     ok = false;
                  } else if (const_mentions(v, &g->base, 8)) {
                     ok = false;
                  }
               }
            }
         }
      }

      if (ok && stores == 1)
         pl->fold = true;
   }
}

/* The global this operand IS -- not one it points into. Only a global used
 * directly as a load's or a store's pointer can be folded, and
 * plan_global_folding has already refused every other shape, so a linear scan
 * over the module's globals answers it exactly.
 */
static struct air_global_plan *
foldable_global_plan(struct air_ctx *c, const struct air_value *v)
{
   if (!c->global_plan || !v || v->kind != AIR_VALUE_GLOBAL)
      return NULL;
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      if (&c->m->globals[i]->base == v)
         return c->global_plan[i].fold ? &c->global_plan[i] : NULL;
   }
   return NULL;
}

/* A constant's bytes as the module lays the type out (air_type_alloc_size, member
 * offsets): what a read-only global holds, for nir->constant_data. False for what
 * is not bytes -- a constant expression (a pointer's value), a type with no size.
 */
static bool
write_constant(struct air_ctx *c, const struct air_type *t, const struct air_value *v, uint8_t *dst)
{
   const struct air_constant *k = air_value_as_constant(v);
   if (!k)
      return false;
   const uint64_t size = air_type_alloc_size(c->m, t);
   switch (k->ckind) {
   case AIR_CONST_NULL:
   case AIR_CONST_UNDEF:
   case AIR_CONST_POISON:
      memset(dst, 0, size);
      return true;
   case AIR_CONST_INT:
   case AIR_CONST_FLOAT: {
      const unsigned bytes = t->kind == AIR_TYPE_INT ? (t->bits + 7) / 8 : t->kind == AIR_TYPE_HALF || t->kind == AIR_TYPE_BFLOAT ? 2 :
                             t->kind == AIR_TYPE_FLOAT ? 4 : t->kind == AIR_TYPE_DOUBLE ? 8 : 0;
      if (bytes == 0 || bytes > 8)
         return false;
      memcpy(dst, &k->u, bytes);   /* little-endian, as the GPU reads it */
      return true;
   }
   case AIR_CONST_DATA:
   case AIR_CONST_AGGREGATE: {
      if (t->kind != AIR_TYPE_ARRAY && t->kind != AIR_TYPE_VECTOR && t->kind != AIR_TYPE_STRUCT)
         return false;
      memset(dst, 0, size);
      for (unsigned i = 0; i < k->num_elems; i++) {
         const struct air_type *et = t->kind == AIR_TYPE_STRUCT ? t->members[i] : t->elem;
         /* a vector's elements are packed at the element's size, an array's at its stride */
         const uint64_t at = t->kind == AIR_TYPE_STRUCT ? air_struct_member_offset(c->m, t, i)
                           : t->kind == AIR_TYPE_VECTOR ? i * (uint64_t)((air_type_alloc_size(c->m, et)))
                           : i * air_type_alloc_size(c->m, et);
         if (k->ckind == AIR_CONST_DATA) {
            const uint64_t eb = air_type_alloc_size(c->m, et);
            if (eb > 8)
               return false;
            memcpy(dst + at, &k->data[i], eb);
         } else if (!write_constant(c, et, k->elems[i], dst + at)) {
            return false;
         }
      }
      return true;
   }
   default:
      return false;
   }
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
         if (g->init && !air_const_uint(g->init, &word)) {
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
      } else if (g->is_const && g->init && type_has_pointer(g->value_type, 8)) {
         /* A READ-ONLY GLOBAL WHOSE INITIALISER HOLDS A POINTER, which scratch
          * cannot hold any more than an alloca can: a pointer here is a
          * resolution and not bytes. It becomes a translation-time slot
          * (AIR_PTR_SLOT), pre-filled from its own initialiser by
          * materialise_globals -- in whatever builder is asking, because the
          * values inside it are defs and a def belongs to one impl.
          *
          * The shape this is for is one line of Metal:
          *
          *   constexpr sampler unused_sampler;
          *
          * which Apple's compiler emits as
          * `constant %"struct.metal::sampler" { ptr addrspace(2) @__air_sampler_state }`
          * and QuartzCore's two YCbCr uber shaders then `select` against, before
          * loading the sampler back out of whichever side won. Put in scratch,
          * that global was bytes nothing ever wrote -- its initialiser holds a
          * pointer, so there was nothing to write -- and a load through it
          * would have read uninitialised memory and called it a sampler. The
          * refusal it produced instead ("a pointer loaded from memory") was
          * right, and this is what answers it.
          *
          * `is_const` is what makes the slot sound rather than convenient: the
          * value cannot change, so one fill per function is the whole story,
          * and AIR_OP_STORE refuses a store into a slot that has an
          * initialiser rather than quietly overwriting one.
          */
         pl->class = AIR_PTR_SLOT;
         pl->binding = new_slot(c, g->value_type, g->init);
      } else if (g->is_const && g->init && !type_has_pointer(g->value_type, 8) &&
                 air_type_alloc_size(c->m, g->value_type) > 0) {
         /* A READ-ONLY GLOBAL WITH ITS DATA -- a table, `constant float4 at[6]
          * = {...}` -- is the shader's constant data (nir->constant_data, read
          * by load_constant), its bytes written here from the initialiser. As a
          * program-scope variable it went to scratch, where nothing writes the
          * initialiser: sokol's bufferless triangle read its corners as zeros
          * and drew nothing.
          */
         unsigned align = MAX2(g->align, air_type_abi_align(c->m, g->value_type));
         uint32_t at = ALIGN_POT(c->nir->constant_data_size, MAX2(align, 4));
         uint64_t size = air_type_alloc_size(c->m, g->value_type);
         uint8_t *data = rzalloc_size(c->nir, at + size);
         if (c->nir->constant_data_size)
            memcpy(data, c->nir->constant_data, c->nir->constant_data_size);
         if (!write_constant(c, g->value_type, g->init, data + at)) {
            air_error(c, "interface: the constant global %s has an initialiser that is not plain data", name);
            return;
         }
         c->nir->constant_data = data;
         c->nir->constant_data_size = at + size;
         pl->class = AIR_PTR_CONSTANT_DATA;
         pl->offset = at;
         pl->has_offset = true;
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

   /* after the loop, because it reads the class every global ended up with */
   plan_global_folding(c);
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

   /* THEN the slots, in a pass of their own, because an initialiser may name
    * another global: `constant %"struct.metal::sampler" { ptr addrspace(2)
    * @__air_sampler_state }` is a slot whose contents is the value of a global
    * further down the list. Filling it inside the loop above read whichever
    * globals happened to come first and left the rest unresolved, which came
    * out as "a load through a null pointer" from inside a callee -- a true
    * sentence about a pointer this function had not finished building.
    *
    * Re-filled per function rather than once, for the reason the offsets above
    * are rebuilt per function: the values inside a slot are defs, and a def
    * belongs to the impl that made it.
    */
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      const struct air_global_plan *pl = &c->global_plan[i];
      if (pl->skip || pl->class != AIR_PTR_SLOT)
         continue;
      if (pl->binding >= c->num_slots || !c->slots[pl->binding].init)
         continue;
      struct air_val *v = air_get(c, c->slots[pl->binding].init);
      if (!v)
         return;   /* air_get has said why */
      c->slots[pl->binding].val = v;
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
         nir_function *fn = translate_callee(c, f, NULL, NULL);
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

/*
 * WHICH `device` BUFFERS THE TRANSLATED SHADER STORES THROUGH, out of the IR itself.
 *
 * A second reading of a fact the metadata already carries, taken because a runtime
 * makes a decision on it that is wrong in SILENCE when the fact is wrong: an
 * unaligned `device` buffer can be served by uploading a copy, exactly as a
 * `constant` one already is, only if nothing writes it -- a copy of a buffer the
 * shader writes would throw the writes away and the draw would look as though it had
 * worked. `air.write`/`air.read_write` answers a different question, which is what
 * Metal lets the shader do: `device T *` is declared writable whether or not the body
 * ever stores through it, and in RenderBox's mask shaders none of them does.
 *
 * The reading is exact for every module that gets this far, and that is a consequence
 * of refusals above rather than an assumption about stores. A store through a pointer
 * chosen at run time is refused ("a store through a chosen pointer, which would have
 * to write both of them"); a pointer written INTO memory, from where it could be
 * fetched back and stored through, is refused ("a store of a pointer into memory");
 * and a pointer whose binding did not resolve never becomes an AIR_PTR_SSBO at all.
 * So every write to a device buffer that a translated module can perform is a
 * store_ssbo or an ssbo_atomic carrying the immediate binding this translator put
 * there -- and this runs after nir_inline_functions, so a callee's store is in the
 * entry point's own IR by now.
 *
 * Where that argument does not reach, the answer is "written" and not "unknown": a
 * store this scan cannot attribute to a declared buffer marks every device buffer in
 * the module, because the alternative to over-refusing one draw is dropping a write.
 */
static void
device_writes_from_ir(nir_shader *nir, struct air_shader_info *info)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            unsigned src;
            switch (intr->intrinsic) {
            case nir_intrinsic_store_ssbo:
               src = 1;
               break;
            case nir_intrinsic_ssbo_atomic:
            case nir_intrinsic_ssbo_atomic_swap:
               src = 0;
               break;
            default:
               continue;
            }

            bool named = false;
            if (nir_src_is_const(intr->src[src])) {
               unsigned binding = nir_src_as_uint(intr->src[src]);
               for (unsigned i = 0; i < info->num_resources; i++) {
                  struct air_resource *r = &info->resources[i];
                  if (r->kind == AIR_RESOURCE_DEVICE_BUFFER && r->index == binding) {
                     r->ir_write = true;
                     named = true;
                  }
               }
            }
            if (named)
               continue;

            for (unsigned i = 0; i < info->num_resources; i++)
               if (info->resources[i].kind == AIR_RESOURCE_DEVICE_BUFFER)
                  info->resources[i].ir_write = true;
         }
      }
   }
}

/* -- the entry point ---------------------------------------------------- */

/* Everything the caller is handed in INFO, copied out of the scratch context
 * that is freed before air_to_nir returns: see the comment inside. Both
 * returns that hand back a shader call it -- the interface-only one used to
 * skip it, and handed out resource tables already freed, which a thread
 * allocating at the same time overwrote: Resident Evil 4's argument encoders
 * came back with every member id 0, wrote nothing, and failed every frame.
 */
static void
keep_info(void *mem_ctx, struct air_shader_info *info)
{
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
      /* -1 and not 0: slot 0 is a real texture index, so a zeroed field would
       * read as "the null texture already lives at index 0" */
      .null_texture_index = -1,
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
   /* A stitched graph is the third thing this can be asked for: not this
    * module as a shader, and not this module's function wrapped, but a graph
    * of library functions compiled into one entry point, of which this
    * module's function is one node like any other (stitch_graph).
    */
   bool stitch = options && options->stitch;
   if (stage == MESA_SHADER_NONE) {
      if (!options || !(options->library_wrapper || options->stitch)) {
         if (error)
            *error = ralloc_strdup(mem_ctx,
                                   "stage: air.visible, a library function (not yet)");
         ralloc_free(ctx);
         return NULL;
      }
      stage = MESA_SHADER_COMPUTE;
      wrap = true;
   } else if (stitch) {
      if (error)
         *error = ralloc_strdup(mem_ctx, "stitch: the module given is an entry point and "
                                         "not an air.visible function");
      ralloc_free(ctx);
      return NULL;
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
   if (!c.error[0] && options && options->interface_only)
      goto interface_read;

   /* the entry point's parameters, as the body will read them */
   for (unsigned i = 0; i < (wrap ? 0 : c.num_params) && !c.error[0]; i++) {
      if (c.params[i])
         air_set(&c, &fn->args[i], c.params[i]);
      else
         air_error(&c, "interface: parameter %u has no metadata", i);
   }

   if (!c.error[0])
      plan_globals(&c);
   if (!c.error[0] && c.tg_param_plus1) {
      /* the threadgroup argument past the static arrays, 16-aligned as Metal
       * aligns threadgroup memory; the value is shared with every use of the
       * parameter (air_set keeps the pointer), so setting it here moves them all
       */
      c.shared_size = ALIGN_POT(c.shared_size, 16);
      c.info->has_threadgroup_arg = true;
      c.info->threadgroup_arg_base = c.shared_size;
      if (c.shared_size)
         c.params[c.tg_param_plus1 - 1]->ptr.offset = nir_imm_int(&c.b, c.shared_size);
   }
   if (!c.error[0])
      materialise_globals(&c);
   if (!c.error[0])
      run_static_initializers(&c);

   if (!c.error[0]) {
      if (stitch) {
         stitch_graph(&c);
      } else if (wrap) {
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

interface_read:
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

   /* the interface is all that was asked for: the passes below are for a body */
   if (options && options->interface_only) {
      ralloc_steal(mem_ctx, c.nir);
      if (info)
         keep_info(mem_ctx, info);
      ralloc_free(ctx);
      return c.nir;
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
      /* A callee with more than one `ret` in it. Every return this translator
       * emits is a goto to the impl's end block, and nir_lower_goto_ifs turns
       * the ones that are not the last into nir_jump_return -- which
       * nir_inline_functions ASSERTS on rather than refusing
       * (nir_functions.c: "nir_instr_as_jump(instr)->type !=
       * nir_jump_return"). nir_lower_returns is the pass that removes them,
       * by making the body's own control flow carry the early exit, and it is
       * what spirv_to_nir runs before inlining for the same reason.
       *
       * It is not hypothetical and it is not rare: RenderBox's blur returns
       * early when the kernel is one tap wide, and before this the module
       * aborted the whole air2nir process -- which a report counts as a crash
       * and not as a refusal, so the measurement was lost as well.
       */
      NIR_PASS(_, c.nir, nir_lower_returns);
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
      device_writes_from_ir(c.nir, info);
      keep_info(mem_ctx, info);
   }
   ralloc_free(ctx);
   if (error)
      *error = NULL;
   return c.nir;
}
