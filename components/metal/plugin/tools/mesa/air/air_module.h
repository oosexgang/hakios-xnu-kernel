/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The part of LLVM IR that AIR uses, read from bitcode into plain C
 * structures. It is a reader, not an IR library: nothing here is modified
 * after reading, and only what the 456 modules of iOS 16.5's system
 * libraries contain (plus a little margin) is understood. Anything else is an
 * error that names the record, never a silent skip, because a skipped
 * instruction is a wrong shader.
 *
 * air_module_print() writes the module back as LLVM assembly. That output is
 * the reader's test: llvm-as must accept it, and after llvm-as | llvm-dis it
 * must equal what llvm-dis prints for the original bitcode.
 */

#ifndef AIR_MODULE_H
#define AIR_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum air_type_kind {
   AIR_TYPE_VOID,
   AIR_TYPE_HALF,
   AIR_TYPE_BFLOAT,
   AIR_TYPE_FLOAT,
   AIR_TYPE_DOUBLE,
   AIR_TYPE_LABEL,
   AIR_TYPE_METADATA,
   AIR_TYPE_TOKEN,
   AIR_TYPE_INT,
   AIR_TYPE_POINTER,
   AIR_TYPE_ARRAY,
   AIR_TYPE_VECTOR,
   AIR_TYPE_STRUCT,
   AIR_TYPE_FUNCTION,
   AIR_TYPE_UNKNOWN, /* x86_fp80 and the like: legal bitcode, never AIR */
};

struct air_type {
   enum air_type_kind kind;
   unsigned id;
   unsigned bits;                 /* INT */
   unsigned addrspace;            /* POINTER */
   const struct air_type *elem;   /* POINTER (NULL: opaque), ARRAY, VECTOR; FUNCTION: return */
   uint64_t count;                /* ARRAY, VECTOR */
   unsigned num_members;          /* STRUCT members, FUNCTION parameters */
   const struct air_type **members;
   bool packed;                   /* STRUCT */
   bool opaque;                   /* STRUCT without a body */
   bool vararg;                   /* FUNCTION */
   const char *name;              /* named STRUCT */
};

enum air_value_kind {
   AIR_VALUE_GLOBAL,
   AIR_VALUE_FUNCTION,
   AIR_VALUE_CONSTANT,
   AIR_VALUE_ARGUMENT,
   AIR_VALUE_INSTRUCTION,
   AIR_VALUE_METADATA, /* a metadata operand of a call (llvm.* intrinsics only) */
};

struct air_value {
   enum air_value_kind kind;
   const struct air_type *type;
   unsigned id;      /* value number: module-wide for globals/constants, per function otherwise */
   const char *name; /* NULL when the module has none (stripped) */
};

enum air_const_kind {
   AIR_CONST_INT,
   AIR_CONST_FLOAT,
   AIR_CONST_NULL,      /* zeroinitializer / null */
   AIR_CONST_UNDEF,
   AIR_CONST_POISON,
   AIR_CONST_AGGREGATE, /* struct, array or vector of other constants */
   AIR_CONST_DATA,      /* array or vector of plain integers/floats, in .data */
   AIR_CONST_EXPR,      /* a constant expression: .expr_op, .operands */
};

enum air_opcode {
   AIR_OP_RET,
   AIR_OP_BR,
   AIR_OP_SWITCH,
   AIR_OP_UNREACHABLE,
   AIR_OP_BINOP,
   AIR_OP_UNOP,
   AIR_OP_CAST,
   AIR_OP_GEP,
   AIR_OP_SELECT,
   AIR_OP_EXTRACTELT,
   AIR_OP_INSERTELT,
   AIR_OP_SHUFFLEVEC,
   AIR_OP_CMP,
   AIR_OP_PHI,
   AIR_OP_ALLOCA,
   AIR_OP_LOAD,
   AIR_OP_STORE,
   AIR_OP_CALL,
   AIR_OP_EXTRACTVAL,
   AIR_OP_INSERTVAL,
   AIR_OP_FREEZE,
   AIR_OP_FENCE,
   AIR_OP_ATOMICRMW,
   AIR_OP_CMPXCHG,
};

/* the bitcode's numbering, which is also the order of LLVM's own enums */
enum air_binop {
   AIR_BINOP_ADD, AIR_BINOP_SUB, AIR_BINOP_MUL, AIR_BINOP_UDIV, AIR_BINOP_SDIV,
   AIR_BINOP_UREM, AIR_BINOP_SREM, AIR_BINOP_SHL, AIR_BINOP_LSHR, AIR_BINOP_ASHR,
   AIR_BINOP_AND, AIR_BINOP_OR, AIR_BINOP_XOR,
};

enum air_cast {
   AIR_CAST_TRUNC, AIR_CAST_ZEXT, AIR_CAST_SEXT, AIR_CAST_FPTOUI, AIR_CAST_FPTOSI,
   AIR_CAST_UITOFP, AIR_CAST_SITOFP, AIR_CAST_FPTRUNC, AIR_CAST_FPEXT,
   AIR_CAST_PTRTOINT, AIR_CAST_INTTOPTR, AIR_CAST_BITCAST, AIR_CAST_ADDRSPACECAST,
};

enum air_pred {
   AIR_FCMP_FALSE = 0, AIR_FCMP_OEQ, AIR_FCMP_OGT, AIR_FCMP_OGE, AIR_FCMP_OLT,
   AIR_FCMP_OLE, AIR_FCMP_ONE, AIR_FCMP_ORD, AIR_FCMP_UNO, AIR_FCMP_UEQ,
   AIR_FCMP_UGT, AIR_FCMP_UGE, AIR_FCMP_ULT, AIR_FCMP_ULE, AIR_FCMP_UNE,
   AIR_FCMP_TRUE,
   AIR_ICMP_EQ = 32, AIR_ICMP_NE, AIR_ICMP_UGT, AIR_ICMP_UGE, AIR_ICMP_ULT,
   AIR_ICMP_ULE, AIR_ICMP_SGT, AIR_ICMP_SGE, AIR_ICMP_SLT, AIR_ICMP_SLE,
};

/* fast-math flags as the bitcode stores them */
#define AIR_FMF_UNSAFE_ALGEBRA (1u << 0) /* the old "fast": everything below */
#define AIR_FMF_NNAN           (1u << 1)
#define AIR_FMF_NINF           (1u << 2)
#define AIR_FMF_NSZ            (1u << 3)
#define AIR_FMF_ARCP           (1u << 4)
#define AIR_FMF_CONTRACT       (1u << 5)
#define AIR_FMF_AFN            (1u << 6)
#define AIR_FMF_REASSOC        (1u << 7)

/* integer flags */
#define AIR_FLAG_NUW   (1u << 0)
#define AIR_FLAG_NSW   (1u << 1)
#define AIR_FLAG_EXACT (1u << 0)

struct air_block;
struct air_md;

struct air_constant {
   struct air_value base;
   enum air_const_kind ckind;
   uint64_t u;                    /* INT (two's complement, low bits), FLOAT (the bits) */
   unsigned num_elems;
   struct air_value **elems;      /* AGGREGATE */
   uint64_t *data;                /* DATA */
   /* EXPR */
   enum air_opcode expr_op;       /* CAST, BINOP, GEP, CMP, SELECT, EXTRACTELT, ... */
   unsigned expr_sub;             /* the cast/binop opcode or predicate */
   bool inbounds;
   const struct air_type *src_type; /* GEP source element type */
   unsigned num_operands;
   struct air_value **operands;
   /* while reading: value numbers not yet resolved */
   unsigned *pending;
};

struct air_global {
   struct air_value base;          /* type: the pointer */
   const struct air_type *value_type;
   unsigned addrspace;
   bool is_const;
   bool externally_initialized;
   unsigned linkage;
   unsigned align;
   unsigned unnamed_addr;          /* 0 none, 1 unnamed_addr, 2 local_unnamed_addr */
   const char *section;
   struct air_value *init;         /* NULL for a declaration */
   unsigned init_id;               /* while reading: value number + 1, 0 for none */
   uint64_t strtab_offset, strtab_size;
};

struct air_inst {
   struct air_value base;          /* type: the result (void when none) */
   enum air_opcode op;
   unsigned sub;                   /* binop/cast/unop opcode, cmp predicate, rmw operation */
   unsigned flags;                 /* AIR_FLAG_*, AIR_FMF_* */
   unsigned align;                 /* bytes; 0 when not given */
   bool is_volatile;
   bool inbounds;
   bool tail;
   unsigned cc;                     /* call: LLVM's calling convention number */
   const struct air_type *aux_type; /* GEP source type, alloca's allocated type, call's function type */
   unsigned num_operands;
   struct air_value **operands;     /* call: callee first, then the arguments */
   unsigned *operand_ids;           /* while reading */
   unsigned num_indices;
   uint32_t *indices;              /* extractvalue/insertvalue */
   unsigned num_targets;
   struct air_block **targets;     /* br: dest, or then/else; phi: the incoming blocks */
   unsigned *target_ids;           /* while reading */
   struct air_block *parent;
   unsigned num_attachments;
   struct { unsigned kind; struct air_md *md; } *attachments;
};

struct air_block {
   unsigned index;
   const char *name;
   unsigned num_insts, cap_insts;
   struct air_inst **insts;
};

struct air_function {
   struct air_value base;          /* type: pointer to the function type */
   const struct air_type *fn_type;
   bool is_declaration;
   unsigned linkage;
   unsigned cc;
   unsigned unnamed_addr;
   unsigned align;
   const char *section;
   unsigned paramattr;             /* index into the module's attribute lists, 0 for none */
   unsigned num_args;
   struct air_value *args;
   unsigned num_blocks;
   struct air_block *blocks;
   unsigned num_values;            /* per-function value numbers in use, all kinds */
   uint64_t strtab_offset, strtab_size;
};

enum air_md_kind {
   AIR_MD_STRING,
   AIR_MD_VALUE,
   AIR_MD_NODE,
   AIR_MD_OTHER, /* debug info and other specialized nodes, kept only to keep the numbering */
};

struct air_md {
   enum air_md_kind kind;
   unsigned id;
   bool distinct;
   unsigned code;            /* OTHER: the record code */
   const char *str;          /* STRING (not NUL-safe: see len) */
   unsigned len;
   struct air_value *value;  /* VALUE */
   unsigned num_ops;
   struct air_md **ops;      /* NODE; NULL operands are null */
   unsigned *pending;        /* while reading: md ids + 1 */
   unsigned value_id;        /* while reading */
   const struct air_type *value_type;
   bool function_local;
};

struct air_named_md {
   const char *name;
   unsigned num_ops;
   struct air_md **ops;
};

struct air_module {
   void *mem_ctx;
   unsigned version;               /* MODULE_CODE_VERSION: 2 = relative ids + strtab */
   const char *identification;
   const char *triple;
   const char *datalayout;
   const char *source_filename;

   unsigned num_types;
   struct air_type *types;

   unsigned num_globals;
   struct air_global **globals;
   unsigned num_functions;
   struct air_function **functions;

   unsigned num_module_values;     /* globals + functions + module-level constants */
   struct air_value **module_values;

   unsigned num_md;                /* module-level metadata */
   struct air_md **md;
   unsigned num_named_md;
   struct air_named_md *named_md;

   unsigned num_md_kinds;
   const char **md_kind_names;     /* indexed by kind id; NULL where none */

   const char **section_names;
   unsigned num_section_names;

   /* first error, with the record that caused it */
   char error[256];
};

/* Reads one module. data may start with the 0x0B17C0DE wrapper or directly
 * with 'BC' 0xC0DE. Returns NULL and fills *error (when non-NULL, allocated
 * under mem_ctx) on failure.
 */
struct air_module *air_module_read(void *mem_ctx, const void *data, size_t size,
                                   const char **error);

void air_module_print(const struct air_module *m, FILE *fp);

/* helpers for the translator */
static inline const struct air_constant *
air_value_as_constant(const struct air_value *v)
{
   return v && v->kind == AIR_VALUE_CONSTANT ? (const struct air_constant *)v : NULL;
}

static inline const struct air_inst *
air_value_as_inst(const struct air_value *v)
{
   return v && v->kind == AIR_VALUE_INSTRUCTION ? (const struct air_inst *)v : NULL;
}

/* A constant integer, where zero may be stored as a null/zeroinitializer
 * rather than as an INTEGER record -- which is how LLVM writes the zero
 * indices of a getelementptr.
 */
static inline bool
air_const_uint(const struct air_value *v, uint64_t *out)
{
   const struct air_constant *c = air_value_as_constant(v);
   if (!c)
      return false;
   if (c->ckind == AIR_CONST_INT) {
      *out = c->u;
      return true;
   }
   if (c->ckind == AIR_CONST_NULL && v->type->kind == AIR_TYPE_INT) {
      *out = 0;
      return true;
   }
   return false;
}

static inline bool
air_type_is_float(const struct air_type *t)
{
   return t->kind == AIR_TYPE_HALF || t->kind == AIR_TYPE_BFLOAT ||
          t->kind == AIR_TYPE_FLOAT || t->kind == AIR_TYPE_DOUBLE;
}

/* the scalar of a vector, the type itself otherwise */
static inline const struct air_type *
air_type_scalar(const struct air_type *t)
{
   return t->kind == AIR_TYPE_VECTOR ? t->elem : t;
}

unsigned air_type_bit_size(const struct air_type *t); /* scalar bits: i1 -> 1, float -> 32, ptr -> 64 */
uint64_t air_type_alloc_size(const struct air_module *m, const struct air_type *t);
unsigned air_type_abi_align(const struct air_module *m, const struct air_type *t);
uint64_t air_struct_member_offset(const struct air_module *m, const struct air_type *t, unsigned i);

const struct air_function *air_module_find_function(const struct air_module *m, const char *name);
const char *air_md_string(const struct air_md *md); /* NUL-terminated copy, or NULL when not a string */

#ifdef __cplusplus
}
#endif

#endif /* AIR_MODULE_H */
