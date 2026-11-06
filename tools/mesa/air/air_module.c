/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Bitcode records -> struct air_module. The record layouts are those LLVM
 * has written since the 4.0/5.0 days (string table, relative value ids,
 * explicit types on loads, GEPs and calls), which is what Apple's
 * metalfe-31001 emits: typed pointers, MODULE_CODE_VERSION 2.
 */

#include "air_module.h"
#include "air_bitcode.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include "util/macros.h"
#include "util/ralloc.h"

/* block ids */
enum {
   MODULE_BLOCK_ID = 8,
   PARAMATTR_BLOCK_ID = 9,
   PARAMATTR_GROUP_BLOCK_ID = 10,
   CONSTANTS_BLOCK_ID = 11,
   FUNCTION_BLOCK_ID = 12,
   IDENTIFICATION_BLOCK_ID = 13,
   VALUE_SYMTAB_BLOCK_ID = 14,
   METADATA_BLOCK_ID = 15,
   METADATA_ATTACHMENT_ID = 16,
   TYPE_BLOCK_ID_NEW = 17,
   USELIST_BLOCK_ID = 18,
   OPERAND_BUNDLE_TAGS_BLOCK_ID = 21,
   METADATA_KIND_BLOCK_ID = 22,
   STRTAB_BLOCK_ID = 23,
   SYMTAB_BLOCK_ID = 25,
   SYNC_SCOPE_NAMES_BLOCK_ID = 26,
};

/* module records */
enum {
   MODULE_CODE_VERSION = 1,
   MODULE_CODE_TRIPLE = 2,
   MODULE_CODE_DATALAYOUT = 3,
   MODULE_CODE_SECTIONNAME = 5,
   MODULE_CODE_GLOBALVAR = 7,
   MODULE_CODE_FUNCTION = 8,
   MODULE_CODE_GCNAME = 11,
   MODULE_CODE_VSTOFFSET = 13,
   MODULE_CODE_SOURCE_FILENAME = 16,
   MODULE_CODE_HASH = 17,
};

/* type records */
enum {
   TYPE_CODE_NUMENTRY = 1,
   TYPE_CODE_VOID = 2,
   TYPE_CODE_FLOAT = 3,
   TYPE_CODE_DOUBLE = 4,
   TYPE_CODE_LABEL = 5,
   TYPE_CODE_OPAQUE = 6,
   TYPE_CODE_INTEGER = 7,
   TYPE_CODE_POINTER = 8,
   TYPE_CODE_HALF = 10,
   TYPE_CODE_ARRAY = 11,
   TYPE_CODE_VECTOR = 12,
   TYPE_CODE_X86_FP80 = 13,
   TYPE_CODE_FP128 = 14,
   TYPE_CODE_PPC_FP128 = 15,
   TYPE_CODE_METADATA = 16,
   TYPE_CODE_X86_MMX = 17,
   TYPE_CODE_STRUCT_ANON = 18,
   TYPE_CODE_STRUCT_NAME = 19,
   TYPE_CODE_STRUCT_NAMED = 20,
   TYPE_CODE_FUNCTION = 21,
   TYPE_CODE_TOKEN = 22,
   TYPE_CODE_BFLOAT = 23,
   TYPE_CODE_OPAQUE_POINTER = 25,
};

/* constants records */
enum {
   CST_CODE_SETTYPE = 1,
   CST_CODE_NULL = 2,
   CST_CODE_UNDEF = 3,
   CST_CODE_INTEGER = 4,
   CST_CODE_WIDE_INTEGER = 5,
   CST_CODE_FLOAT = 6,
   CST_CODE_AGGREGATE = 7,
   CST_CODE_STRING = 8,
   CST_CODE_CSTRING = 9,
   CST_CODE_CE_BINOP = 10,
   CST_CODE_CE_CAST = 11,
   CST_CODE_CE_GEP_OLD = 12,
   CST_CODE_CE_SELECT = 13,
   CST_CODE_CE_EXTRACTELT = 14,
   CST_CODE_CE_INSERTELT = 15,
   CST_CODE_CE_SHUFFLEVEC = 16,
   CST_CODE_CE_CMP = 17,
   CST_CODE_CE_INBOUNDS_GEP = 20,
   CST_CODE_DATA = 22,
   CST_CODE_CE_GEP_WITH_INRANGE_INDEX = 24,
   CST_CODE_CE_UNOP = 25,
   CST_CODE_POISON = 26,
};

/* function records */
enum {
   FUNC_CODE_DECLAREBLOCKS = 1,
   FUNC_CODE_INST_BINOP = 2,
   FUNC_CODE_INST_CAST = 3,
   FUNC_CODE_INST_SELECT = 5,
   FUNC_CODE_INST_EXTRACTELT = 6,
   FUNC_CODE_INST_INSERTELT = 7,
   FUNC_CODE_INST_SHUFFLEVEC = 8,
   FUNC_CODE_INST_CMP = 9,
   FUNC_CODE_INST_RET = 10,
   FUNC_CODE_INST_BR = 11,
   FUNC_CODE_INST_SWITCH = 12,
   FUNC_CODE_INST_UNREACHABLE = 15,
   FUNC_CODE_INST_PHI = 16,
   FUNC_CODE_INST_ALLOCA = 19,
   FUNC_CODE_INST_LOAD = 20,
   FUNC_CODE_INST_EXTRACTVAL = 26,
   FUNC_CODE_INST_INSERTVAL = 27,
   FUNC_CODE_INST_CMP2 = 28,
   FUNC_CODE_INST_VSELECT = 29,
   FUNC_CODE_DEBUG_LOC_AGAIN = 33,
   FUNC_CODE_INST_CALL = 34,
   FUNC_CODE_DEBUG_LOC = 35,
   FUNC_CODE_INST_FENCE = 36,
   FUNC_CODE_INST_GEP = 43,
   FUNC_CODE_INST_STORE = 44,
   FUNC_CODE_INST_CMPXCHG = 46,
   FUNC_CODE_OPERAND_BUNDLE = 55,
   FUNC_CODE_INST_UNOP = 56,
   FUNC_CODE_INST_FREEZE = 58,
   FUNC_CODE_INST_ATOMICRMW = 59,
};

/* metadata records */
enum {
   METADATA_STRING_OLD = 1,
   METADATA_VALUE = 2,
   METADATA_NODE = 3,
   METADATA_NAME = 4,
   METADATA_DISTINCT_NODE = 5,
   METADATA_KIND = 6,
   METADATA_LOCATION = 7,
   METADATA_OLD_NODE = 8,
   METADATA_OLD_FN_NODE = 9,
   METADATA_NAMED_NODE = 10,
   METADATA_ATTACHMENT = 11,
   METADATA_STRINGS = 35,
   METADATA_GLOBAL_DECL_ATTACHMENT = 36,
   METADATA_INDEX_OFFSET = 38,
   METADATA_INDEX = 39,
   METADATA_ARG_LIST = 46,
};

enum { VST_CODE_ENTRY = 1, VST_CODE_BBENTRY = 2, VST_CODE_FNENTRY = 3 };

/* call record CC field */
#define CALL_TAIL_BIT 0
#define CALL_CCONV_SHIFT 1
#define CALL_EXPLICIT_TYPE_BIT 15
#define CALL_FMF_BIT 17

struct reader {
   struct air_module *m;
   struct air_bitstream bs;

   /* values of the function being read, after the module's */
   unsigned num_values, cap_values;
   struct air_value **values;

   /* the types the record gave for operands not yet defined, by value id */
   unsigned cap_fwd;
   const struct air_type **fwd;

   /* metadata ids: the module's, then the current function's */
   unsigned num_md, cap_md;
   struct air_md **md;

   const uint8_t *strtab;
   size_t strtab_size;

   unsigned num_bodies_read;
   struct air_function *cur_fn;
   unsigned cur_inst_id;     /* all instructions, for attachments */
   unsigned num_fn_insts, cap_fn_insts;
   struct air_inst **fn_insts;

   const char *pending_md_name;
};

static bool
fail(struct reader *r, const char *fmt, ...)
{
   if (r->m->error[0])
      return false;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(r->m->error, sizeof(r->m->error), fmt, ap);
   va_end(ap);
   return false;
}

static bool
check_bs(struct reader *r)
{
   if (r->bs.error[0])
      return fail(r, "bitstream: %s", r->bs.error);
   return true;
}

/* -- types -------------------------------------------------------------- */

static const struct air_type *
type_by_id(struct reader *r, uint64_t id)
{
   if (id >= r->m->num_types) {
      fail(r, "type id %" PRIu64 " out of range (%u types)", id, r->m->num_types);
      return NULL;
   }
   return &r->m->types[id];
}

/* pointers that the bitcode does not list (to functions, to globals with an
 * explicit type), made on the side
 */
static const struct air_type *
pointer_to(struct reader *r, const struct air_type *elem, unsigned as)
{
   for (unsigned i = 0; i < r->m->num_types; i++) {
      const struct air_type *t = &r->m->types[i];
      if (t->kind == AIR_TYPE_POINTER && t->elem == elem && t->addrspace == as)
         return t;
   }
   struct air_type *t = rzalloc(r->m, struct air_type);
   t->kind = AIR_TYPE_POINTER;
   t->id = ~0u;
   t->elem = elem;
   t->addrspace = as;
   return t;
}

static bool
read_type_block(struct reader *r)
{
   struct air_module *m = r->m;
   unsigned next = 0;
   const char *pending_name = NULL;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         air_bs_skip_block(&r->bs);
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "type block ends early");

      if (e.id == TYPE_CODE_NUMENTRY) {
         if (e.num_ops < 1)
            return fail(r, "NUMENTRY without a count");
         m->num_types = e.ops[0];
         m->types = rzalloc_array(m, struct air_type, MAX2(m->num_types, 1));
         for (unsigned i = 0; i < m->num_types; i++) {
            m->types[i].id = i;
            m->types[i].kind = AIR_TYPE_UNKNOWN;
         }
         continue;
      }
      if (e.id == TYPE_CODE_STRUCT_NAME) {
         pending_name = air_bs_ops_to_string(m, e.ops, e.num_ops);
         continue;
      }
      if (next >= m->num_types)
         return fail(r, "more types than NUMENTRY said");

      struct air_type *t = &m->types[next];
      switch (e.id) {
      case TYPE_CODE_VOID: t->kind = AIR_TYPE_VOID; break;
      case TYPE_CODE_HALF: t->kind = AIR_TYPE_HALF; break;
      case TYPE_CODE_BFLOAT: t->kind = AIR_TYPE_BFLOAT; break;
      case TYPE_CODE_FLOAT: t->kind = AIR_TYPE_FLOAT; break;
      case TYPE_CODE_DOUBLE: t->kind = AIR_TYPE_DOUBLE; break;
      case TYPE_CODE_LABEL: t->kind = AIR_TYPE_LABEL; break;
      case TYPE_CODE_METADATA: t->kind = AIR_TYPE_METADATA; break;
      case TYPE_CODE_TOKEN: t->kind = AIR_TYPE_TOKEN; break;
      case TYPE_CODE_X86_FP80:
      case TYPE_CODE_FP128:
      case TYPE_CODE_PPC_FP128:
      case TYPE_CODE_X86_MMX:
         t->kind = AIR_TYPE_UNKNOWN;
         break;
      case TYPE_CODE_INTEGER:
         if (e.num_ops < 1)
            return fail(r, "INTEGER type without a width");
         t->kind = AIR_TYPE_INT;
         t->bits = e.ops[0];
         break;
      case TYPE_CODE_POINTER:
         if (e.num_ops < 1)
            return fail(r, "POINTER type without a pointee");
         t->kind = AIR_TYPE_POINTER;
         t->elem = type_by_id(r, e.ops[0]);
         t->addrspace = e.num_ops > 1 ? e.ops[1] : 0;
         break;
      case TYPE_CODE_OPAQUE_POINTER:
         t->kind = AIR_TYPE_POINTER;
         t->addrspace = e.num_ops > 0 ? e.ops[0] : 0;
         break;
      case TYPE_CODE_ARRAY:
      case TYPE_CODE_VECTOR:
         if (e.num_ops < 2)
            return fail(r, "ARRAY/VECTOR type without a size and element");
         t->kind = e.id == TYPE_CODE_ARRAY ? AIR_TYPE_ARRAY : AIR_TYPE_VECTOR;
         t->count = e.ops[0];
         t->elem = type_by_id(r, e.ops[1]);
         if (e.id == TYPE_CODE_VECTOR && e.num_ops > 2 && e.ops[2])
            return fail(r, "scalable vector type");
         break;
      case TYPE_CODE_OPAQUE:
         t->kind = AIR_TYPE_STRUCT;
         t->opaque = true;
         t->name = pending_name;
         pending_name = NULL;
         break;
      case TYPE_CODE_STRUCT_ANON:
      case TYPE_CODE_STRUCT_NAMED:
         if (e.num_ops < 1)
            return fail(r, "STRUCT type without the packed flag");
         t->kind = AIR_TYPE_STRUCT;
         t->packed = e.ops[0];
         t->num_members = e.num_ops - 1;
         t->members = ralloc_array(m, const struct air_type *, MAX2(t->num_members, 1));
         for (unsigned i = 0; i < t->num_members; i++)
            t->members[i] = type_by_id(r, e.ops[1 + i]);
         if (e.id == TYPE_CODE_STRUCT_NAMED) {
            t->name = pending_name;
            pending_name = NULL;
         }
         break;
      case TYPE_CODE_FUNCTION:
         if (e.num_ops < 2)
            return fail(r, "FUNCTION type without vararg and return");
         t->kind = AIR_TYPE_FUNCTION;
         t->vararg = e.ops[0];
         t->elem = type_by_id(r, e.ops[1]);
         t->num_members = e.num_ops - 2;
         t->members = ralloc_array(m, const struct air_type *, MAX2(t->num_members, 1));
         for (unsigned i = 0; i < t->num_members; i++)
            t->members[i] = type_by_id(r, e.ops[2 + i]);
         break;
      default:
         return fail(r, "unknown type record %u", e.id);
      }
      if (m->error[0])
         return false;
      next++;
   }
}

/* -- values ------------------------------------------------------------- */

static void
push_value(struct reader *r, struct air_value *v)
{
   if (r->num_values == r->cap_values) {
      r->cap_values = MAX2(256, r->cap_values * 2);
      r->values = reralloc(r->m, r->values, struct air_value *, r->cap_values);
   }
   v->id = r->num_values;
   r->values[r->num_values++] = v;
}

static struct air_value *
value_by_id(struct reader *r, unsigned id)
{
   if (id >= r->num_values || !r->values[id]) {
      fail(r, "value %u is not defined (%u values)", id, r->num_values);
      return NULL;
   }
   return r->values[id];
}

static int64_t
decode_signed(uint64_t v)
{
   if ((v & 1) == 0)
      return (int64_t)(v >> 1);
   if (v != 1)
      return -(int64_t)(v >> 1);
   return INT64_MIN;
}

static uint64_t
truncate_to(const struct air_type *t, uint64_t v)
{
   const struct air_type *s = air_type_scalar(t);
   if (s->kind == AIR_TYPE_INT && s->bits < 64)
      return v & ((1ull << s->bits) - 1);
   return v;
}

static struct air_constant *
new_constant(struct reader *r, const struct air_type *t, enum air_const_kind k)
{
   struct air_constant *c = rzalloc(r->m, struct air_constant);
   c->base.kind = AIR_VALUE_CONSTANT;
   c->base.type = t;
   c->ckind = k;
   push_value(r, &c->base);
   return c;
}

static void
expr_operands(struct reader *r, struct air_constant *c, unsigned n)
{
   c->num_operands = n;
   c->operands = rzalloc_array(r->m, struct air_value *, MAX2(n, 1));
   c->pending = rzalloc_array(r->m, unsigned, MAX2(n, 1));
}

static bool
read_constants_block(struct reader *r)
{
   const struct air_type *cur = NULL;
   unsigned first = r->num_values;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         air_bs_skip_block(&r->bs);
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "constants block ends early");

      if (e.id == CST_CODE_SETTYPE) {
         if (e.num_ops < 1 || !(cur = type_by_id(r, e.ops[0])))
            return fail(r, "SETTYPE without a type");
         continue;
      }
      if (!cur)
         return fail(r, "constant before SETTYPE");

      struct air_constant *c;
      switch (e.id) {
      case CST_CODE_NULL:
         new_constant(r, cur, AIR_CONST_NULL);
         break;
      case CST_CODE_UNDEF:
         new_constant(r, cur, AIR_CONST_UNDEF);
         break;
      case CST_CODE_POISON:
         new_constant(r, cur, AIR_CONST_POISON);
         break;
      case CST_CODE_INTEGER:
         if (e.num_ops < 1)
            return fail(r, "INTEGER constant without a value");
         c = new_constant(r, cur, AIR_CONST_INT);
         c->u = truncate_to(cur, (uint64_t)decode_signed(e.ops[0]));
         break;
      case CST_CODE_WIDE_INTEGER:
         return fail(r, "integer constant wider than 64 bits");
      case CST_CODE_FLOAT:
         if (e.num_ops < 1)
            return fail(r, "FLOAT constant without a value");
         if (!air_type_is_float(cur))
            return fail(r, "FLOAT constant of a non-float type");
         c = new_constant(r, cur, AIR_CONST_FLOAT);
         c->u = e.ops[0];
         break;
      case CST_CODE_AGGREGATE:
         c = new_constant(r, cur, AIR_CONST_AGGREGATE);
         c->num_elems = e.num_ops;
         c->elems = rzalloc_array(r->m, struct air_value *, MAX2(e.num_ops, 1));
         c->pending = rzalloc_array(r->m, unsigned, MAX2(e.num_ops, 1));
         for (unsigned i = 0; i < e.num_ops; i++)
            c->pending[i] = e.ops[i];
         break;
      case CST_CODE_STRING:
      case CST_CODE_CSTRING:
      case CST_CODE_DATA: {
         unsigned n = e.num_ops + (e.id == CST_CODE_CSTRING);
         c = new_constant(r, cur, AIR_CONST_DATA);
         c->num_elems = n;
         c->data = rzalloc_array(r->m, uint64_t, MAX2(n, 1));
         for (unsigned i = 0; i < e.num_ops; i++)
            c->data[i] = e.ops[i];
         break;
      }
      case CST_CODE_CE_CAST:
         if (e.num_ops < 3)
            return fail(r, "CE_CAST too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_CAST;
         c->expr_sub = e.ops[0];
         expr_operands(r, c, 1);
         c->pending[0] = e.ops[2];
         break;
      case CST_CODE_CE_BINOP:
         if (e.num_ops < 3)
            return fail(r, "CE_BINOP too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_BINOP;
         c->expr_sub = e.ops[0];
         expr_operands(r, c, 2);
         c->pending[0] = e.ops[1];
         c->pending[1] = e.ops[2];
         break;
      case CST_CODE_CE_UNOP:
         if (e.num_ops < 2)
            return fail(r, "CE_UNOP too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_UNOP;
         c->expr_sub = e.ops[0];
         expr_operands(r, c, 1);
         c->pending[0] = e.ops[1];
         break;
      case CST_CODE_CE_GEP_OLD:
      case CST_CODE_CE_INBOUNDS_GEP:
      case CST_CODE_CE_GEP_WITH_INRANGE_INDEX: {
         unsigned i = 0;
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_GEP;
         if (e.id == CST_CODE_CE_GEP_WITH_INRANGE_INDEX || (e.num_ops & 1))
            c->src_type = type_by_id(r, e.ops[i++]);
         if (e.id == CST_CODE_CE_GEP_WITH_INRANGE_INDEX)
            c->inbounds = e.ops[i++] & 1;
         else
            c->inbounds = e.id == CST_CODE_CE_INBOUNDS_GEP;
         if ((e.num_ops - i) & 1)
            return fail(r, "CE_GEP with an odd operand list");
         expr_operands(r, c, (e.num_ops - i) / 2);
         for (unsigned k = 0; i < e.num_ops; i += 2, k++)
            c->pending[k] = e.ops[i + 1]; /* (type, value) pairs */
         break;
      }
      case CST_CODE_CE_SELECT:
         if (e.num_ops < 3)
            return fail(r, "CE_SELECT too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_SELECT;
         expr_operands(r, c, 3);
         for (unsigned i = 0; i < 3; i++)
            c->pending[i] = e.ops[i];
         break;
      case CST_CODE_CE_EXTRACTELT:
         if (e.num_ops < 3)
            return fail(r, "CE_EXTRACTELT too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_EXTRACTELT;
         expr_operands(r, c, 2);
         c->pending[0] = e.ops[1];
         c->pending[1] = e.ops[e.num_ops == 4 ? 3 : 2];
         break;
      case CST_CODE_CE_INSERTELT:
         if (e.num_ops < 3)
            return fail(r, "CE_INSERTELT too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_INSERTELT;
         expr_operands(r, c, 3);
         c->pending[0] = e.ops[0];
         c->pending[1] = e.ops[1];
         c->pending[2] = e.ops[e.num_ops == 4 ? 3 : 2];
         break;
      case CST_CODE_CE_SHUFFLEVEC:
         if (e.num_ops < 3)
            return fail(r, "CE_SHUFFLEVEC too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_SHUFFLEVEC;
         expr_operands(r, c, 3);
         for (unsigned i = 0; i < 3; i++)
            c->pending[i] = e.ops[i];
         break;
      case CST_CODE_CE_CMP:
         if (e.num_ops < 4)
            return fail(r, "CE_CMP too short");
         c = new_constant(r, cur, AIR_CONST_EXPR);
         c->expr_op = AIR_OP_CMP;
         c->expr_sub = e.ops[3];
         expr_operands(r, c, 2);
         c->pending[0] = e.ops[1];
         c->pending[1] = e.ops[2];
         break;
      default:
         return fail(r, "unknown constant record %u", e.id);
      }
      if (r->m->error[0])
         return false;
   }

   /* aggregates and expressions may name constants that come after them */
   for (unsigned i = first; i < r->num_values; i++) {
      struct air_constant *c = (struct air_constant *)r->values[i];
      if (!c->pending)
         continue;
      if (c->ckind == AIR_CONST_AGGREGATE) {
         for (unsigned k = 0; k < c->num_elems; k++)
            if (!(c->elems[k] = value_by_id(r, c->pending[k])))
               return false;
      } else {
         for (unsigned k = 0; k < c->num_operands; k++)
            if (!(c->operands[k] = value_by_id(r, c->pending[k])))
               return false;
      }
      c->pending = NULL;
   }
   return true;
}

/* -- metadata ----------------------------------------------------------- */

static struct air_md *
new_md(struct reader *r, enum air_md_kind k)
{
   struct air_md *md = rzalloc(r->m, struct air_md);
   md->kind = k;
   md->id = r->num_md;
   md->function_local = r->cur_fn != NULL;
   if (r->num_md == r->cap_md) {
      r->cap_md = MAX2(256, r->cap_md * 2);
      r->md = reralloc(r->m, r->md, struct air_md *, r->cap_md);
   }
   r->md[r->num_md++] = md;
   return md;
}

static bool
read_md_strings(struct reader *r, const struct air_bs_entry *e)
{
   if (e->num_ops < 2 || !e->blob)
      return fail(r, "METADATA_STRINGS without counts or blob");
   unsigned count = e->ops[0];
   size_t lengths_size = e->ops[1];
   if (lengths_size > e->blob_size)
      return fail(r, "METADATA_STRINGS offsets past the blob");

   /* the lengths are a little bitstream of VBR6 values */
   struct air_bitstream lens = {
      .mem_ctx = r->m,
      .data = e->blob,
      .size_bits = lengths_size * 8,
   };
   const char *chars = (const char *)e->blob + lengths_size;
   size_t avail = e->blob_size - lengths_size;
   size_t off = 0;
   for (unsigned i = 0; i < count; i++) {
      uint64_t len = 0, shift = 0, chunk;
      do {
         if (lens.pos + 6 > lens.size_bits)
            return fail(r, "METADATA_STRINGS lengths run out");
         chunk = 0;
         for (unsigned b = 0; b < 6; b++, lens.pos++)
            chunk |= (uint64_t)((lens.data[lens.pos >> 3] >> (lens.pos & 7)) & 1) << b;
         len |= (chunk & 31) << shift;
         shift += 5;
      } while (chunk & 32);
      if (off + len > avail)
         return fail(r, "METADATA_STRINGS string past the blob");
      struct air_md *md = new_md(r, AIR_MD_STRING);
      md->str = chars + off;
      md->len = len;
      off += len;
   }
   return true;
}

static bool
md_other_code(unsigned code)
{
   /* records that define one metadata node each and that the frontend does
    * not look into: locations and the DI* family
    */
   return code == METADATA_LOCATION || (code >= 12 && code <= 34) || code == 37 ||
          code == 40 || code == 41 || code == 44 || code == 45 || code == 47;
}

static bool
read_metadata_block(struct reader *r)
{
   unsigned first = r->num_md;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         air_bs_skip_block(&r->bs);
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "metadata block ends early");

      struct air_md *md;
      switch (e.id) {
      case METADATA_STRINGS:
         if (!read_md_strings(r, &e))
            return false;
         break;
      case METADATA_STRING_OLD:
         md = new_md(r, AIR_MD_STRING);
         md->str = air_bs_ops_to_string(r->m, e.ops, e.num_ops);
         md->len = e.num_ops;
         break;
      case METADATA_VALUE:
         if (e.num_ops != 2)
            return fail(r, "METADATA_VALUE of %u operands", e.num_ops);
         md = new_md(r, AIR_MD_VALUE);
         md->value_type = type_by_id(r, e.ops[0]);
         md->value_id = e.ops[1];
         break;
      case METADATA_NODE:
      case METADATA_DISTINCT_NODE:
         md = new_md(r, AIR_MD_NODE);
         md->distinct = e.id == METADATA_DISTINCT_NODE;
         md->num_ops = e.num_ops;
         md->ops = rzalloc_array(r->m, struct air_md *, MAX2(e.num_ops, 1));
         md->pending = rzalloc_array(r->m, unsigned, MAX2(e.num_ops, 1));
         for (unsigned i = 0; i < e.num_ops; i++)
            md->pending[i] = e.ops[i]; /* id + 1, 0 for null */
         break;
      case METADATA_NAME:
         r->pending_md_name = air_bs_ops_to_string(r->m, e.ops, e.num_ops);
         break;
      case METADATA_NAMED_NODE: {
         if (!r->pending_md_name)
            return fail(r, "NAMED_NODE without a NAME");
         struct air_module *m = r->m;
         m->named_md = reralloc(m, m->named_md, struct air_named_md, m->num_named_md + 1);
         struct air_named_md *n = &m->named_md[m->num_named_md++];
         n->name = r->pending_md_name;
         r->pending_md_name = NULL;
         n->num_ops = e.num_ops;
         n->ops = rzalloc_array(m, struct air_md *, MAX2(e.num_ops, 1));
         /* resolved below: these ids are plain, not + 1 */
         for (unsigned i = 0; i < e.num_ops; i++)
            n->ops[i] = (struct air_md *)(uintptr_t)(e.ops[i] + 1);
         break;
      }
      case METADATA_KIND: {
         if (e.num_ops < 1)
            return fail(r, "METADATA_KIND without an id");
         struct air_module *m = r->m;
         unsigned id = e.ops[0];
         if (id >= m->num_md_kinds) {
            m->md_kind_names = rerzalloc(m, m->md_kind_names, const char *, m->num_md_kinds, id + 1);
            m->num_md_kinds = id + 1;
         }
         m->md_kind_names[id] = air_bs_ops_to_string(m, e.ops + 1, e.num_ops - 1);
         break;
      }
      case METADATA_INDEX_OFFSET:
      case METADATA_INDEX:
      case METADATA_GLOBAL_DECL_ATTACHMENT:
         break; /* for lazy loading and global attachments; nothing AIR needs */
      case METADATA_OLD_NODE:
      case METADATA_OLD_FN_NODE:
      case METADATA_ARG_LIST:
         return fail(r, "unsupported metadata record %u", e.id);
      default:
         if (md_other_code(e.id)) {
            md = new_md(r, AIR_MD_OTHER);
            md->code = e.id;
            break;
         }
         return fail(r, "unknown metadata record %u", e.id);
      }
      if (r->m->error[0])
         return false;
   }

   /* nodes may name later nodes */
   for (unsigned i = first; i < r->num_md; i++) {
      struct air_md *md = r->md[i];
      if (md->kind != AIR_MD_NODE || !md->pending)
         continue;
      for (unsigned k = 0; k < md->num_ops; k++) {
         unsigned id = md->pending[k];
         if (id == 0)
            continue;
         if (id - 1 >= r->num_md)
            return fail(r, "metadata node names !%u of %u", id - 1, r->num_md);
         md->ops[k] = r->md[id - 1];
      }
      md->pending = NULL;
   }
   return true;
}

/* values named by metadata, once the values exist */
static bool
resolve_md_values(struct reader *r, unsigned first)
{
   for (unsigned i = first; i < r->num_md; i++) {
      struct air_md *md = r->md[i];
      if (md->kind == AIR_MD_VALUE && !md->value)
         if (!(md->value = value_by_id(r, md->value_id)))
            return false;
   }
   return true;
}

static bool
resolve_named_md(struct reader *r)
{
   struct air_module *m = r->m;
   for (unsigned i = 0; i < m->num_named_md; i++) {
      struct air_named_md *n = &m->named_md[i];
      for (unsigned k = 0; k < n->num_ops; k++) {
         uintptr_t id = (uintptr_t)n->ops[k] - 1;
         if (id >= r->num_md)
            return fail(r, "named metadata %s names !%u", n->name, (unsigned)id);
         n->ops[k] = r->md[id];
      }
   }
   return true;
}

static bool
read_metadata_kind_block(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "metadata kind block ends early");
      if (e.id == METADATA_KIND && e.num_ops >= 1) {
         struct air_module *m = r->m;
         unsigned id = e.ops[0];
         if (id >= m->num_md_kinds) {
            m->md_kind_names = rerzalloc(m, m->md_kind_names, const char *, m->num_md_kinds, id + 1);
            m->num_md_kinds = id + 1;
         }
         m->md_kind_names[id] = air_bs_ops_to_string(m, e.ops + 1, e.num_ops - 1);
      }
   }
}

/* -- module records ----------------------------------------------------- */

static bool
read_globalvar(struct reader *r, const struct air_bs_entry *e)
{
   struct air_module *m = r->m;
   if (r->m->version < 2)
      return fail(r, "module version %u: only bitcode with a string table is read", m->version);
   if (e->num_ops < 8)
      return fail(r, "GLOBALVAR too short");

   struct air_global *g = rzalloc(m, struct air_global);
   g->base.kind = AIR_VALUE_GLOBAL;
   g->strtab_offset = e->ops[0];
   g->strtab_size = e->ops[1];
   const struct air_type *t = type_by_id(r, e->ops[2]);
   if (!t)
      return false;
   uint64_t flags = e->ops[3];
   g->is_const = flags & 1;
   if (flags & 2) {
      g->value_type = t;
      g->addrspace = flags >> 2;
   } else {
      if (t->kind != AIR_TYPE_POINTER || !t->elem)
         return fail(r, "GLOBALVAR of a non-pointer type");
      g->value_type = t->elem;
      g->addrspace = t->addrspace;
   }
   g->base.type = pointer_to(r, g->value_type, g->addrspace);
   g->init_id = e->ops[4];
   g->linkage = e->ops[5];
   g->align = e->ops[6] ? 1u << (e->ops[6] - 1) : 0;
   if (e->ops[7]) {
      if (e->ops[7] - 1 >= m->num_section_names)
         return fail(r, "GLOBALVAR names section %" PRIu64, e->ops[7] - 1);
      g->section = m->section_names[e->ops[7] - 1];
   }
   if (e->num_ops > 10)
      g->unnamed_addr = e->ops[10];
   if (e->num_ops > 11)
      g->externally_initialized = e->ops[11];
   if (e->num_ops > 9 && e->ops[9])
      return fail(r, "thread-local global");

   m->globals = reralloc(m, m->globals, struct air_global *, m->num_globals + 1);
   m->globals[m->num_globals++] = g;
   push_value(r, &g->base);
   return true;
}

static bool
read_function_record(struct reader *r, const struct air_bs_entry *e)
{
   struct air_module *m = r->m;
   if (m->version < 2)
      return fail(r, "module version %u: only bitcode with a string table is read", m->version);
   if (e->num_ops < 8)
      return fail(r, "FUNCTION too short");

   struct air_function *f = rzalloc(m, struct air_function);
   f->base.kind = AIR_VALUE_FUNCTION;
   f->strtab_offset = e->ops[0];
   f->strtab_size = e->ops[1];
   const struct air_type *t = type_by_id(r, e->ops[2]);
   if (!t)
      return false;
   if (t->kind == AIR_TYPE_POINTER && t->elem)
      t = t->elem;
   if (t->kind != AIR_TYPE_FUNCTION)
      return fail(r, "FUNCTION record of a non-function type");
   f->fn_type = t;
   unsigned as = e->num_ops > 18 ? e->ops[18] : 0;
   f->base.type = pointer_to(r, t, as);
   f->cc = e->ops[3];
   f->is_declaration = e->ops[4];
   f->linkage = e->ops[5];
   f->paramattr = e->ops[6];
   f->align = e->ops[7] ? 1u << (e->ops[7] - 1) : 0;
   if (e->ops[8]) {
      if (e->ops[8] - 1 >= m->num_section_names)
         return fail(r, "FUNCTION names section %" PRIu64, e->ops[8] - 1);
      f->section = m->section_names[e->ops[8] - 1];
   }
   if (e->num_ops > 11)
      f->unnamed_addr = e->ops[11];
   if (e->num_ops > 16 && e->ops[16])
      return fail(r, "function with a personality");

   f->num_args = t->num_members;
   f->args = rzalloc_array(m, struct air_value, MAX2(f->num_args, 1));
   for (unsigned i = 0; i < f->num_args; i++) {
      f->args[i].kind = AIR_VALUE_ARGUMENT;
      f->args[i].type = t->members[i];
   }

   m->functions = reralloc(m, m->functions, struct air_function *, m->num_functions + 1);
   m->functions[m->num_functions++] = f;
   push_value(r, &f->base);
   return true;
}

/* -- function bodies ---------------------------------------------------- */

struct rec {
   const struct air_bs_entry *e;
   unsigned i;
   bool bad;
};

static uint64_t
take(struct rec *rc)
{
   if (rc->i >= rc->e->num_ops) {
      rc->bad = true;
      return 0;
   }
   return rc->e->ops[rc->i++];
}

static bool
more(const struct rec *rc)
{
   return rc->i < rc->e->num_ops;
}

/* a relative operand without a type (the type is known from context) */
static unsigned
take_value(struct reader *r, struct rec *rc)
{
   return (uint32_t)r->num_values - (uint32_t)take(rc);
}

/* a relative operand that carries its type when it is a forward reference */
static unsigned
take_value_type(struct reader *r, struct rec *rc)
{
   unsigned id = (uint32_t)r->num_values - (uint32_t)take(rc);
   if (id >= r->num_values) {
      const struct air_type *t = type_by_id(r, take(rc));
      if (id >= r->cap_fwd) {
         unsigned cap = MAX2(id + 1, MAX2(64, r->cap_fwd * 2));
         r->fwd = rerzalloc(r->m, r->fwd, const struct air_type *, r->cap_fwd, cap);
         r->cap_fwd = cap;
      }
      r->fwd[id] = t;
   }
   return id;
}

static struct air_inst *
new_inst(struct reader *r, enum air_opcode op, const struct air_type *type, unsigned num_operands)
{
   struct air_inst *in = rzalloc(r->m, struct air_inst);
   in->base.kind = AIR_VALUE_INSTRUCTION;
   in->base.type = type;
   in->op = op;
   in->num_operands = num_operands;
   in->operands = rzalloc_array(r->m, struct air_value *, MAX2(num_operands, 1));
   in->operand_ids = rzalloc_array(r->m, unsigned, MAX2(num_operands, 1));
   return in;
}

static const struct air_type *
type_of_id(struct reader *r, unsigned id)
{
   if (id < r->num_values && r->values[id])
      return r->values[id]->type;
   if (id < r->cap_fwd && r->fwd[id])
      return r->fwd[id];
   fail(r, "operand %u has no type (a forward reference whose record gave none)", id);
   return NULL;
}

static unsigned
decode_align(uint64_t v)
{
   unsigned log2 = v & 31;
   return log2 ? 1u << (log2 - 1) : 0;
}

static const struct air_type *
gep_result_type(struct reader *r, const struct air_type *src, const struct air_type *ptr_type,
                struct air_inst *in, unsigned first_index)
{
   /* the result is a pointer in the base's address space; its element type
    * (for printing typed pointers) is what the indices reach
    */
   /* index 0 steps over the pointer; the ones after it go into the type */
   const struct air_type *t = src;
   for (unsigned i = first_index + 2; i < in->num_operands && t; i++) {
      if (t->kind == AIR_TYPE_STRUCT) {
         unsigned id = in->operand_ids[i];
         uint64_t n;
         if (id >= r->num_values || !r->values[id] || !air_const_uint(r->values[id], &n) ||
             n >= t->num_members)
            return NULL;
         t = t->members[n];
      } else if (t->kind == AIR_TYPE_ARRAY || t->kind == AIR_TYPE_VECTOR) {
         t = t->elem;
      } else {
         return NULL;
      }
   }
   unsigned as = air_type_scalar(ptr_type)->addrspace;
   if (ptr_type->kind == AIR_TYPE_VECTOR)
      return NULL; /* vector GEPs: not in AIR */
   return pointer_to(r, t, as);
}

static bool
add_inst(struct reader *r, struct air_block **block, struct air_inst *in)
{
   struct air_block *b = *block;
   if (!b)
      return fail(r, "instruction after the last block");
   if (b->num_insts == b->cap_insts) {
      b->cap_insts = MAX2(16, b->cap_insts * 2);
      b->insts = reralloc(r->m, b->insts, struct air_inst *, b->cap_insts);
   }
   b->insts[b->num_insts++] = in;
   in->parent = b;

   if (r->num_fn_insts == r->cap_fn_insts) {
      r->cap_fn_insts = MAX2(64, r->cap_fn_insts * 2);
      r->fn_insts = reralloc(r->m, r->fn_insts, struct air_inst *, r->cap_fn_insts);
   }
   r->fn_insts[r->num_fn_insts++] = in;

   if (in->base.type && in->base.type->kind != AIR_TYPE_VOID)
      push_value(r, &in->base);

   /* terminators move on to the next block */
   if (in->op == AIR_OP_RET || in->op == AIR_OP_BR || in->op == AIR_OP_SWITCH ||
       in->op == AIR_OP_UNREACHABLE) {
      struct air_function *f = r->cur_fn;
      unsigned next = b->index + 1;
      *block = next < f->num_blocks ? &f->blocks[next] : NULL;
   }
   return true;
}

static bool
read_instruction(struct reader *r, const struct air_bs_entry *e, struct air_block **block)
{
   struct air_module *m = r->m;
   struct rec rc = { .e = e };
   struct air_inst *in = NULL;
   unsigned a, b, c;

   switch (e->id) {
   case FUNC_CODE_INST_BINOP: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      unsigned opc = take(&rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_BINOP, t, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->sub = opc;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_UNOP: {
      a = take_value_type(r, &rc);
      unsigned opc = take(&rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_UNOP, t, 1);
      in->operand_ids[0] = a;
      in->sub = opc;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_CAST: {
      a = take_value_type(r, &rc);
      const struct air_type *t = type_by_id(r, take(&rc));
      unsigned opc = take(&rc);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_CAST, t, 1);
      in->operand_ids[0] = a;
      in->sub = opc;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_GEP: {
      bool inbounds = take(&rc) & 1;
      const struct air_type *src = type_by_id(r, take(&rc));
      unsigned ids[64], n = 0;
      while (more(&rc) && n < ARRAY_SIZE(ids))
         ids[n++] = take_value_type(r, &rc);
      if (!src || n == 0 || more(&rc))
         return fail(r, "GEP of %u operands", n);
      const struct air_type *pt = type_of_id(r, ids[0]);
      if (!pt)
         return false;
      in = new_inst(r, AIR_OP_GEP, NULL, n);
      memcpy(in->operand_ids, ids, n * sizeof(unsigned));
      in->inbounds = inbounds;
      in->aux_type = src;
      in->base.type = gep_result_type(r, src, pt, in, 0);
      if (!in->base.type)
         return fail(r, "GEP whose result type cannot be worked out");
      break;
   }
   case FUNC_CODE_INST_SELECT:
   case FUNC_CODE_INST_VSELECT: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      c = e->id == FUNC_CODE_INST_VSELECT ? take_value_type(r, &rc) : take_value(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_SELECT, t, 3);
      in->operand_ids[0] = c; /* condition first, like LLVM's operand order */
      in->operand_ids[1] = a;
      in->operand_ids[2] = b;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_EXTRACTELT: {
      a = take_value_type(r, &rc);
      b = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      if (t->kind != AIR_TYPE_VECTOR)
         return fail(r, "extractelement of a non-vector");
      in = new_inst(r, AIR_OP_EXTRACTELT, t->elem, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      break;
   }
   case FUNC_CODE_INST_INSERTELT: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      c = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_INSERTELT, t, 3);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->operand_ids[2] = c;
      break;
   }
   case FUNC_CODE_INST_SHUFFLEVEC: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      c = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      const struct air_type *mt = type_of_id(r, c);
      if (!t || !mt)
         return false;
      if (t->kind != AIR_TYPE_VECTOR || mt->kind != AIR_TYPE_VECTOR)
         return fail(r, "shufflevector of non-vectors");
      /* the result has the mask's length and the inputs' element */
      const struct air_type *rt = NULL;
      for (unsigned i = 0; i < m->num_types; i++) {
         const struct air_type *x = &m->types[i];
         if (x->kind == AIR_TYPE_VECTOR && x->elem == t->elem && x->count == mt->count) {
            rt = x;
            break;
         }
      }
      if (!rt)
         return fail(r, "shufflevector result type not in the type table");
      in = new_inst(r, AIR_OP_SHUFFLEVEC, rt, 3);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->operand_ids[2] = c;
      break;
   }
   case FUNC_CODE_INST_CMP:
   case FUNC_CODE_INST_CMP2: {
      a = take_value_type(r, &rc);
      b = take_value(r, &rc);
      unsigned pred = take(&rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      /* i1, or a vector of i1 as long as the operands */
      const struct air_type *rt = NULL;
      for (unsigned i = 0; i < m->num_types; i++) {
         const struct air_type *x = &m->types[i];
         if (t->kind == AIR_TYPE_VECTOR) {
            if (x->kind == AIR_TYPE_VECTOR && x->count == t->count && x->elem->kind == AIR_TYPE_INT &&
                x->elem->bits == 1) {
               rt = x;
               break;
            }
         } else if (x->kind == AIR_TYPE_INT && x->bits == 1) {
            rt = x;
            break;
         }
      }
      if (!rt)
         return fail(r, "compare result type not in the type table");
      in = new_inst(r, AIR_OP_CMP, rt, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->sub = pred;
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_RET:
      if (e->num_ops == 0) {
         in = new_inst(r, AIR_OP_RET, NULL, 0);
      } else {
         a = take_value_type(r, &rc);
         in = new_inst(r, AIR_OP_RET, NULL, 1);
         in->operand_ids[0] = a;
      }
      break;
   case FUNC_CODE_INST_BR:
      if (e->num_ops == 1) {
         in = new_inst(r, AIR_OP_BR, NULL, 0);
         in->num_targets = 1;
         in->target_ids = ralloc_array(m, unsigned, 1);
         in->target_ids[0] = take(&rc);
      } else if (e->num_ops == 3) {
         in = new_inst(r, AIR_OP_BR, NULL, 1);
         in->num_targets = 2;
         in->target_ids = ralloc_array(m, unsigned, 2);
         in->target_ids[0] = take(&rc);
         in->target_ids[1] = take(&rc);
         in->operand_ids[0] = take_value(r, &rc);
      } else {
         return fail(r, "br of %u operands", e->num_ops);
      }
      break;
   case FUNC_CODE_INST_SWITCH: {
      /* [ type of the condition, condition, default block,
       *   (case constant, block)* ]
       *
       * The condition is a RELATIVE value number and each case constant is an
       * ABSOLUTE one, because that is what the writer does -- pushValue for
       * the condition, getValueID for each case value. Reading them the same
       * way gives the wrong constant for every case in any function that has
       * more than a handful of values, which is every function here.
       *
       * The blocks are block numbers, as br's are, not value numbers.
       */
      if (e->num_ops < 3 || (e->num_ops - 3) % 2)
         return fail(r, "a switch of %u operands", e->num_ops);
      unsigned ty = take(&rc);
      /* LLVM 3.0 wrote switches over ranges, with the case count and a bit
       * width packed into the first operand under this magic instead of a type
       * id. Nothing Apple's compiler emits uses it, and reading it as a type
       * would silently take the wrong path, so it is named and refused.
       */
      if ((ty >> 16) == 0x4B5)
         return fail(r, "a switch in LLVM 3.0's range form (SWITCH_INST_MAGIC)");
      unsigned cond = take_value(r, &rc);
      unsigned deflt = take(&rc);
      unsigned n = (e->num_ops - 3) / 2;
      in = new_inst(r, AIR_OP_SWITCH, NULL, 1 + n);
      in->num_targets = 1 + n;
      in->target_ids = ralloc_array(m, unsigned, 1 + n);
      /* target 0 and operand 0 are the default edge and the condition; the
       * cases follow, so case k is operand k+1 and target k+1
       */
      in->target_ids[0] = deflt;
      in->operand_ids[0] = cond;
      for (unsigned k = 0; k < n; k++) {
         in->operand_ids[1 + k] = take(&rc);
         in->target_ids[1 + k] = take(&rc);
      }
      break;
   }
   case FUNC_CODE_INST_UNREACHABLE:
      in = new_inst(r, AIR_OP_UNREACHABLE, NULL, 0);
      break;
   case FUNC_CODE_INST_PHI: {
      const struct air_type *t = type_by_id(r, take(&rc));
      if (!t)
         return false;
      unsigned n = (e->num_ops - 1) / 2;
      in = new_inst(r, AIR_OP_PHI, t, n);
      in->num_targets = n;
      in->target_ids = ralloc_array(m, unsigned, MAX2(n, 1));
      for (unsigned i = 0; i < n; i++) {
         int64_t rel = decode_signed(take(&rc));
         in->operand_ids[i] = (uint32_t)((int64_t)r->num_values - rel);
         in->target_ids[i] = take(&rc);
      }
      if (more(&rc))
         in->flags = take(&rc);
      break;
   }
   case FUNC_CODE_INST_ALLOCA: {
      uint64_t ty = take(&rc), opty = take(&rc), size = take(&rc), align = take(&rc);
      (void)opty;
      const struct air_type *t = type_by_id(r, ty);
      if (!t)
         return false;
      bool explicit_type = align & (1u << 6);
      const struct air_type *allocated = explicit_type ? t : t->elem;
      if (!allocated)
         return fail(r, "alloca without an allocated type");
      unsigned as = more(&rc) ? take(&rc) : 0;
      in = new_inst(r, AIR_OP_ALLOCA, pointer_to(r, allocated, as), 1);
      in->aux_type = allocated;
      in->operand_ids[0] = size; /* absolute, unlike other operands */
      in->align = ((align & 31) | ((align >> 8) << 5)) ? 1u << (((align & 31) | ((align >> 8) << 5)) - 1) : 0;
      break;
   }
   case FUNC_CODE_INST_LOAD: {
      a = take_value_type(r, &rc);
      const struct air_type *t;
      if (rc.e->num_ops - rc.i == 3) {
         t = type_by_id(r, take(&rc));
      } else {
         const struct air_type *pt = type_of_id(r, a);
         t = pt ? pt->elem : NULL;
      }
      if (!t)
         return fail(r, "load of unknown type");
      in = new_inst(r, AIR_OP_LOAD, t, 1);
      in->operand_ids[0] = a;
      in->align = decode_align(take(&rc));
      in->is_volatile = take(&rc);
      break;
   }
   case FUNC_CODE_INST_STORE: {
      a = take_value_type(r, &rc); /* the pointer */
      b = take_value_type(r, &rc); /* the value */
      in = new_inst(r, AIR_OP_STORE, NULL, 2);
      in->operand_ids[0] = b; /* value first, as LLVM orders them */
      in->operand_ids[1] = a;
      in->align = decode_align(take(&rc));
      in->is_volatile = take(&rc);
      break;
   }
   case FUNC_CODE_INST_EXTRACTVAL: {
      a = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      unsigned n = e->num_ops - rc.i;
      uint32_t *idx = ralloc_array(m, uint32_t, MAX2(n, 1));
      for (unsigned i = 0; i < n; i++) {
         idx[i] = take(&rc);
         if (t->kind == AIR_TYPE_STRUCT && idx[i] < t->num_members)
            t = t->members[idx[i]];
         else if (t->kind == AIR_TYPE_ARRAY && idx[i] < t->count)
            t = t->elem;
         else
            return fail(r, "extractvalue index out of the aggregate");
      }
      in = new_inst(r, AIR_OP_EXTRACTVAL, t, 1);
      in->operand_ids[0] = a;
      in->num_indices = n;
      in->indices = idx;
      break;
   }
   case FUNC_CODE_INST_INSERTVAL: {
      a = take_value_type(r, &rc);
      b = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      unsigned n = e->num_ops - rc.i;
      in = new_inst(r, AIR_OP_INSERTVAL, t, 2);
      in->operand_ids[0] = a;
      in->operand_ids[1] = b;
      in->num_indices = n;
      in->indices = ralloc_array(m, uint32_t, MAX2(n, 1));
      for (unsigned i = 0; i < n; i++)
         in->indices[i] = take(&rc);
      break;
   }
   case FUNC_CODE_INST_CALL: {
      take(&rc); /* paramattrs: an index into PARAMATTR, nothing the translation uses */
      uint64_t cc = take(&rc);
      unsigned fmf = 0;
      if (cc & (1u << CALL_FMF_BIT))
         fmf = take(&rc);
      const struct air_type *fty = NULL;
      if (cc & (1u << CALL_EXPLICIT_TYPE_BIT))
         fty = type_by_id(r, take(&rc));
      unsigned callee = take_value_type(r, &rc);
      if (!fty) {
         const struct air_type *pt = type_of_id(r, callee);
         fty = pt ? pt->elem : NULL;
      }
      if (!fty || fty->kind != AIR_TYPE_FUNCTION)
         return fail(r, "call without a function type");
      unsigned n = fty->num_members;
      unsigned ids[64];
      if (n >= ARRAY_SIZE(ids))
         return fail(r, "call of %u arguments", n);
      for (unsigned i = 0; i < n; i++) {
         if (fty->members[i]->kind == AIR_TYPE_LABEL)
            return fail(r, "call with a label argument");
         ids[i] = take_value(r, &rc);
      }
      unsigned extra = 0;
      if (fty->vararg) {
         while (more(&rc) && n + extra < ARRAY_SIZE(ids)) {
            ids[n + extra] = take_value_type(r, &rc);
            extra++;
         }
      }
      if (rc.bad || more(&rc))
         return fail(r, "call record does not match its function type");
      in = new_inst(r, AIR_OP_CALL, fty->elem, 1 + n + extra);
      in->operand_ids[0] = callee;
      memcpy(in->operand_ids + 1, ids, (n + extra) * sizeof(unsigned));
      in->aux_type = fty;
      in->tail = cc & (1u << CALL_TAIL_BIT);
      in->flags = fmf;
      in->cc = (cc >> CALL_CCONV_SHIFT) & 0x3ff;
      break;
   }
   case FUNC_CODE_INST_FREEZE: {
      a = take_value_type(r, &rc);
      const struct air_type *t = type_of_id(r, a);
      if (!t)
         return false;
      in = new_inst(r, AIR_OP_FREEZE, t, 1);
      in->operand_ids[0] = a;
      break;
   }
   case FUNC_CODE_DEBUG_LOC:
   case FUNC_CODE_DEBUG_LOC_AGAIN:
      return true; /* not instructions */
   case FUNC_CODE_DECLAREBLOCKS:
      return fail(r, "DECLAREBLOCKS twice");
   case FUNC_CODE_OPERAND_BUNDLE:
      return fail(r, "operand bundles");
   default:
      return fail(r, "unsupported instruction record %u", e->id);
   }

   if (rc.bad)
      return fail(r, "instruction record %u too short", e->id);
   r->cur_inst_id++;
   return add_inst(r, block, in);
}

static bool
read_attachments(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "attachment block ends early");
      if (e.id != METADATA_ATTACHMENT)
         continue;
      if (e.num_ops % 2 == 0)
         continue; /* attachments of the function itself */
      unsigned inst = e.ops[0];
      if (inst >= r->num_fn_insts)
         return fail(r, "attachment to instruction %u of %u", inst, r->num_fn_insts);
      struct air_inst *in = r->fn_insts[inst];
      unsigned n = (e.num_ops - 1) / 2;
      in->attachments = ralloc_array(r->m, __typeof__(*in->attachments), n);
      in->num_attachments = n;
      for (unsigned i = 0; i < n; i++) {
         unsigned id = e.ops[2 + 2 * i];
         if (id >= r->num_md)
            return fail(r, "attachment of !%u of %u", id, r->num_md);
         in->attachments[i].kind = e.ops[1 + 2 * i];
         in->attachments[i].md = r->md[id];
      }
   }
}

static bool
read_function_vst(struct reader *r, unsigned first_local)
{
   struct air_function *f = r->cur_fn;
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "symbol table ends early");
      if (e.num_ops < 1)
         continue;
      const char *name = air_bs_ops_to_string(r->m, e.ops + 1, e.num_ops - 1);
      if (e.id == VST_CODE_ENTRY) {
         if (e.ops[0] < first_local)
            continue;
         struct air_value *v = value_by_id(r, e.ops[0]);
         if (!v)
            return false;
         v->name = name;
      } else if (e.id == VST_CODE_BBENTRY) {
         if (e.ops[0] >= f->num_blocks)
            return fail(r, "name for block %" PRIu64, e.ops[0]);
         f->blocks[e.ops[0]].name = name;
      }
   }
}

static struct air_function *
next_body(struct reader *r)
{
   struct air_module *m = r->m;
   unsigned seen = 0;
   for (unsigned i = 0; i < m->num_functions; i++) {
      if (m->functions[i]->is_declaration)
         continue;
      if (seen++ == r->num_bodies_read) {
         r->num_bodies_read++;
         return m->functions[i];
      }
   }
   return NULL;
}

static bool
read_function_block(struct reader *r)
{
   struct air_function *f = next_body(r);
   if (!f)
      return fail(r, "a function body for no function");

   unsigned module_values = r->num_values;
   unsigned module_md = r->num_md;
   r->cur_fn = f;
   r->cur_inst_id = 0;
   r->num_fn_insts = 0;
   if (r->cap_fwd)
      memset(r->fwd, 0, r->cap_fwd * sizeof(*r->fwd));

   for (unsigned i = 0; i < f->num_args; i++)
      push_value(r, &f->args[i]);
   unsigned first_local = module_values;

   struct air_block *block = NULL;
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;
      if (e.kind == AIR_BS_ENTER_BLOCK) {
         bool ok;
         switch (e.id) {
         case CONSTANTS_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_constants_block(r);
            break;
         case METADATA_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_metadata_block(r);
            break;
         case METADATA_ATTACHMENT_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_attachments(r);
            break;
         case VALUE_SYMTAB_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_function_vst(r, first_local);
            break;
         default:
            ok = air_bs_skip_block(&r->bs); /* USELIST and the like */
            break;
         }
         if (!ok)
            return false;
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "function block ends early");

      if (e.id == FUNC_CODE_DECLAREBLOCKS) {
         if (e.num_ops < 1 || f->blocks)
            return fail(r, "bad DECLAREBLOCKS");
         f->num_blocks = e.ops[0];
         f->blocks = rzalloc_array(r->m, struct air_block, MAX2(f->num_blocks, 1));
         for (unsigned i = 0; i < f->num_blocks; i++)
            f->blocks[i].index = i;
         block = f->num_blocks ? &f->blocks[0] : NULL;
         continue;
      }
      if (!read_instruction(r, &e, &block))
         return false;
   }

   /* operands, now that every value of the function exists */
   for (unsigned i = 0; i < r->num_fn_insts; i++) {
      struct air_inst *in = r->fn_insts[i];
      for (unsigned k = 0; k < in->num_operands; k++) {
         if (in->op == AIR_OP_CALL && k > 0 && k - 1 < in->aux_type->num_members &&
             in->aux_type->members[k - 1]->kind == AIR_TYPE_METADATA) {
            /* a metadata argument: the id is a metadata id */
            unsigned id = in->operand_ids[k];
            if (id >= r->num_md)
               return fail(r, "metadata argument !%u", id);
            struct air_value *v = rzalloc(r->m, struct air_value);
            v->kind = AIR_VALUE_METADATA;
            v->type = in->aux_type->members[k - 1];
            v->id = id;
            in->operands[k] = v;
            continue;
         }
         if (!(in->operands[k] = value_by_id(r, in->operand_ids[k])))
            return fail(r, "instruction %u of %s: operand %u (value %u) undefined", i,
                        f->base.name ? f->base.name : "?", k, in->operand_ids[k]);
      }
      if (in->num_targets) {
         in->targets = ralloc_array(r->m, struct air_block *, in->num_targets);
         for (unsigned k = 0; k < in->num_targets; k++) {
            if (in->target_ids[k] >= f->num_blocks)
               return fail(r, "branch to block %u of %u", in->target_ids[k], f->num_blocks);
            in->targets[k] = &f->blocks[in->target_ids[k]];
         }
      }
      in->operand_ids = NULL;
      in->target_ids = NULL;
   }
   for (unsigned i = 0; i < f->num_blocks; i++) {
      if (f->blocks[i].num_insts == 0)
         return fail(r, "block %u of a function is empty", i);
   }
   if (!resolve_md_values(r, module_md))
      return false;

   f->num_values = r->num_values - module_values;
   /* the function's values and metadata are its own: the next one starts over */
   r->num_values = module_values;
   r->num_md = module_md;
   r->cur_fn = NULL;
   return true;
}

/* -- the module block --------------------------------------------------- */

static bool
read_module_block(struct reader *r)
{
   struct air_module *m = r->m;
   bool module_md_resolved = false;

   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         break;

      if (e.kind == AIR_BS_ENTER_BLOCK) {
         bool ok = true;
         switch (e.id) {
         case TYPE_BLOCK_ID_NEW:
            air_bs_enter(&r->bs, e.id);
            ok = read_type_block(r);
            break;
         case CONSTANTS_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_constants_block(r);
            break;
         case METADATA_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_metadata_block(r);
            break;
         case METADATA_KIND_BLOCK_ID:
            air_bs_enter(&r->bs, e.id);
            ok = read_metadata_kind_block(r);
            break;
         case FUNCTION_BLOCK_ID:
            if (!module_md_resolved) {
               /* module-level values are complete once bodies begin */
               m->num_module_values = r->num_values;
               ok = resolve_md_values(r, 0) && resolve_named_md(r);
               m->num_md = r->num_md;
               module_md_resolved = true;
            }
            air_bs_enter(&r->bs, e.id);
            ok = ok && read_function_block(r);
            break;
         default:
            /* attributes, symbol table, operand bundle tags, sync scopes,
             * use lists: nothing the translation depends on
             */
            ok = air_bs_skip_block(&r->bs);
            break;
         }
         if (!ok)
            return false;
         continue;
      }
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "module block ends early");

      switch (e.id) {
      case MODULE_CODE_VERSION:
         m->version = e.num_ops ? e.ops[0] : 0;
         break;
      case MODULE_CODE_TRIPLE:
         m->triple = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_DATALAYOUT:
         m->datalayout = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_SOURCE_FILENAME:
         m->source_filename = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_SECTIONNAME:
         m->section_names = reralloc(m, m->section_names, const char *, m->num_section_names + 1);
         m->section_names[m->num_section_names++] = air_bs_ops_to_string(m, e.ops, e.num_ops);
         break;
      case MODULE_CODE_GLOBALVAR:
         if (!read_globalvar(r, &e))
            return false;
         break;
      case MODULE_CODE_FUNCTION:
         if (!read_function_record(r, &e))
            return false;
         break;
      case MODULE_CODE_VSTOFFSET:
      case MODULE_CODE_HASH:
      case MODULE_CODE_GCNAME:
         break;
      default:
         return fail(r, "unsupported module record %u", e.id);
      }
   }

   if (!module_md_resolved) {
      m->num_module_values = r->num_values;
      if (!resolve_md_values(r, 0) || !resolve_named_md(r))
         return false;
      m->num_md = r->num_md;
   }
   /* (the arrays may have moved while function bodies grew them) */
   m->module_values = r->values;
   m->md = r->md;

   /* global initializers name module-level constants */
   for (unsigned i = 0; i < m->num_globals; i++) {
      struct air_global *g = m->globals[i];
      if (g->init_id && !(g->init = value_by_id(r, g->init_id - 1)))
         return false;
   }
   if (r->num_bodies_read != 0) {
      unsigned bodies = 0;
      for (unsigned i = 0; i < m->num_functions; i++)
         bodies += !m->functions[i]->is_declaration;
      if (bodies != r->num_bodies_read)
         return fail(r, "%u function bodies for %u definitions", r->num_bodies_read, bodies);
   }
   return true;
}

static bool
read_strtab_block(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "string table ends early");
      if (e.id == 1 && e.blob) {
         r->strtab = e.blob;
         r->strtab_size = e.blob_size;
      }
   }
}

static bool
read_identification_block(struct reader *r)
{
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r->bs);
      if (e.kind == AIR_BS_END_BLOCK)
         return true;
      if (e.kind != AIR_BS_RECORD)
         return check_bs(r) && fail(r, "identification block ends early");
      if (e.id == 1)
         r->m->identification = air_bs_ops_to_string(r->m, e.ops, e.num_ops);
   }
}

static const char *
strtab_name(struct reader *r, uint64_t off, uint64_t size)
{
   if (!r->strtab || off + size > r->strtab_size) {
      fail(r, "name at %" PRIu64 "+%" PRIu64 " outside the string table", off, size);
      return NULL;
   }
   return ralloc_strndup(r->m, (const char *)r->strtab + off, size);
}

struct air_module *
air_module_read(void *mem_ctx, const void *data, size_t size, const char **error)
{
   struct air_module *m = rzalloc(mem_ctx, struct air_module);
   struct reader r = { .m = m };
   m->mem_ctx = m;

   const uint8_t *p = data;
   if (size >= 20 && p[0] == 0xde && p[1] == 0xc0 && p[2] == 0x17 && p[3] == 0x0b) {
      uint32_t off = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
      uint32_t len = p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24;
      if ((uint64_t)off + len > size) {
         fail(&r, "bitcode wrapper points past the data");
         goto out;
      }
      p += off;
      size = len;
   }

   if (!air_bs_init(&r.bs, m, p, size)) {
      fail(&r, "%s", r.bs.error);
      goto out;
   }

   bool seen_module = false;
   for (;;) {
      struct air_bs_entry e = air_bs_next(&r.bs);
      if (e.kind == AIR_BS_END)
         break;
      if (e.kind != AIR_BS_ENTER_BLOCK) {
         check_bs(&r) || fail(&r, "a record at the top level");
         goto out;
      }
      bool ok;
      switch (e.id) {
      case MODULE_BLOCK_ID:
         if (seen_module) {
            fail(&r, "two modules in one bitcode file");
            goto out;
         }
         seen_module = true;
         air_bs_enter(&r.bs, e.id);
         ok = read_module_block(&r);
         break;
      case STRTAB_BLOCK_ID:
         air_bs_enter(&r.bs, e.id);
         ok = read_strtab_block(&r);
         break;
      case IDENTIFICATION_BLOCK_ID:
         air_bs_enter(&r.bs, e.id);
         ok = read_identification_block(&r);
         break;
      default:
         ok = air_bs_skip_block(&r.bs); /* SYMTAB */
         break;
      }
      if (!ok)
         goto out;
   }
   if (!seen_module) {
      fail(&r, "no module block");
      goto out;
   }

   /* the string table comes after the module, so names come last */
   for (unsigned i = 0; i < m->num_globals; i++)
      if (!(m->globals[i]->base.name =
               strtab_name(&r, m->globals[i]->strtab_offset, m->globals[i]->strtab_size)))
         goto out;
   for (unsigned i = 0; i < m->num_functions; i++)
      if (!(m->functions[i]->base.name =
               strtab_name(&r, m->functions[i]->strtab_offset, m->functions[i]->strtab_size)))
         goto out;

out:
   if (m->error[0]) {
      if (error)
         *error = ralloc_strdup(mem_ctx, m->error);
      ralloc_free(m);
      return NULL;
   }
   if (error)
      *error = NULL;
   return m;
}

const struct air_function *
air_module_find_function(const struct air_module *m, const char *name)
{
   for (unsigned i = 0; i < m->num_functions; i++)
      if (m->functions[i]->base.name && !strcmp(m->functions[i]->base.name, name))
         return m->functions[i];
   return NULL;
}

const char *
air_md_string(const struct air_md *md)
{
   if (!md || md->kind != AIR_MD_STRING)
      return NULL;
   /* strings from METADATA_STRINGS point into the blob and are not terminated */
   return ralloc_strndup((void *)md, md->str, md->len);
}
