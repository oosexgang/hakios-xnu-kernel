/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * struct air_module -> LLVM assembly, in the syntax LLVM 22 reads (opaque
 * pointers). Attributes are not printed; everything else the reader keeps
 * is. The point is a test of the reader: llvm-as must accept the output,
 * and llvm-as | llvm-dis of it must equal llvm-dis of the original once
 * attributes are stripped from both (tools/air/reader-check.sh).
 */

#include "air_module.h"

#include <inttypes.h>
#include <string.h>

#include "util/hash_table.h"
#include "util/macros.h"
#include "util/ralloc.h"

struct printer {
   const struct air_module *m;
   FILE *fp;
   struct hash_table *md_numbers; /* air_md * -> number + 1 */
   unsigned num_md_numbers;
   struct air_md **md_order;
   unsigned cap_md_order;
   /* slots of the current function */
   struct hash_table *slots;      /* air_value * or air_block * -> slot + 1 */
};

static void print_type(struct printer *p, const struct air_type *t);
static void print_value(struct printer *p, const struct air_value *v);

static bool
is_ident_char(char c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == '$' || c == '-';
}

static void
print_escaped(struct printer *p, const char *s, size_t len)
{
   for (size_t i = 0; i < len; i++) {
      unsigned char c = s[i];
      if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
         fputc(c, p->fp);
      else
         fprintf(p->fp, "\\%02X", c);
   }
}

static void
print_name(struct printer *p, char sigil, const char *name)
{
   bool plain = name[0] != 0 && !(name[0] >= '0' && name[0] <= '9');
   for (const char *c = name; *c && plain; c++)
      plain = is_ident_char(*c);
   fputc(sigil, p->fp);
   if (plain) {
      fputs(name, p->fp);
   } else {
      fputc('"', p->fp);
      print_escaped(p, name, strlen(name));
      fputc('"', p->fp);
   }
}

static void
print_type(struct printer *p, const struct air_type *t)
{
   FILE *fp = p->fp;
   switch (t->kind) {
   case AIR_TYPE_VOID: fputs("void", fp); break;
   case AIR_TYPE_HALF: fputs("half", fp); break;
   case AIR_TYPE_BFLOAT: fputs("bfloat", fp); break;
   case AIR_TYPE_FLOAT: fputs("float", fp); break;
   case AIR_TYPE_DOUBLE: fputs("double", fp); break;
   case AIR_TYPE_LABEL: fputs("label", fp); break;
   case AIR_TYPE_METADATA: fputs("metadata", fp); break;
   case AIR_TYPE_TOKEN: fputs("token", fp); break;
   case AIR_TYPE_INT: fprintf(fp, "i%u", t->bits); break;
   case AIR_TYPE_POINTER:
      if (t->addrspace)
         fprintf(fp, "ptr addrspace(%u)", t->addrspace);
      else
         fputs("ptr", fp);
      break;
   case AIR_TYPE_ARRAY:
      fprintf(fp, "[%" PRIu64 " x ", t->count);
      print_type(p, t->elem);
      fputc(']', fp);
      break;
   case AIR_TYPE_VECTOR:
      fprintf(fp, "<%" PRIu64 " x ", t->count);
      print_type(p, t->elem);
      fputc('>', fp);
      break;
   case AIR_TYPE_STRUCT:
      if (t->name) {
         print_name(p, '%', t->name);
         break;
      }
      fputs(t->packed ? "<{ " : "{ ", fp);
      for (unsigned i = 0; i < t->num_members; i++) {
         if (i)
            fputs(", ", fp);
         print_type(p, t->members[i]);
      }
      fputs(t->num_members ? (t->packed ? " }>" : " }") : (t->packed ? "}>" : "}"), fp);
      break;
   case AIR_TYPE_FUNCTION:
      print_type(p, t->elem);
      fputs(" (", fp);
      for (unsigned i = 0; i < t->num_members; i++) {
         if (i)
            fputs(", ", fp);
         print_type(p, t->members[i]);
      }
      if (t->vararg)
         fputs(t->num_members ? ", ..." : "...", fp);
      fputc(')', fp);
      break;
   default:
      fputs("<unknown type>", fp);
      break;
   }
}

static void
print_float(struct printer *p, const struct air_type *t, uint64_t bits)
{
   switch (t->kind) {
   case AIR_TYPE_HALF:
      fprintf(p->fp, "0xH%04" PRIX64, bits & 0xffff);
      break;
   case AIR_TYPE_BFLOAT:
      fprintf(p->fp, "0xR%04" PRIX64, bits & 0xffff);
      break;
   case AIR_TYPE_FLOAT: {
      /* LLVM writes float constants as the double of the same value */
      uint32_t u = bits;
      float f;
      memcpy(&f, &u, 4);
      double d = f;
      uint64_t du;
      memcpy(&du, &d, 8);
      /* a NaN's payload must survive: shift it into the double's mantissa */
      if (f != f) {
         du = ((uint64_t)(u >> 31) << 63) | (0x7ffull << 52) | ((uint64_t)(u & 0x7fffff) << 29);
      }
      fprintf(p->fp, "0x%016" PRIX64, du);
      break;
   }
   default:
      fprintf(p->fp, "0x%016" PRIX64, bits);
      break;
   }
}

static void
print_int(struct printer *p, const struct air_type *t, uint64_t u)
{
   if (t->bits == 1) {
      fputs(u & 1 ? "true" : "false", p->fp);
      return;
   }
   int64_t s = t->bits >= 64 ? (int64_t)u : (int64_t)(u << (64 - t->bits)) >> (64 - t->bits);
   fprintf(p->fp, "%" PRId64, s);
}

static void print_constant(struct printer *p, const struct air_constant *c);

static void
print_typed(struct printer *p, const struct air_value *v)
{
   print_type(p, v->type);
   fputc(' ', p->fp);
   print_value(p, v);
}

static const struct air_type *
elem_type(const struct air_type *t, unsigned i)
{
   if (t->kind == AIR_TYPE_STRUCT)
      return t->members[i];
   return t->elem;
}

static void
print_constant(struct printer *p, const struct air_constant *c)
{
   FILE *fp = p->fp;
   const struct air_type *t = c->base.type;
   switch (c->ckind) {
   case AIR_CONST_INT:
      if (t->kind == AIR_TYPE_INT)
         print_int(p, t, c->u);
      else
         fputs("<int constant of a non-int type>", fp);
      break;
   case AIR_CONST_FLOAT:
      print_float(p, t, c->u);
      break;
   case AIR_CONST_NULL:
      /* a scalar zero prints as a zero of its type; only aggregates get
       * LLVM's zeroinitializer
       */
      if (t->kind == AIR_TYPE_POINTER)
         fputs("null", fp);
      else if (t->kind == AIR_TYPE_INT)
         print_int(p, t, 0);
      else if (air_type_is_float(t))
         print_float(p, t, 0);
      else
         fputs("zeroinitializer", fp);
      break;
   case AIR_CONST_UNDEF:
      fputs("undef", fp);
      break;
   case AIR_CONST_POISON:
      fputs("poison", fp);
      break;
   case AIR_CONST_AGGREGATE:
   case AIR_CONST_DATA: {
      const char *open = t->kind == AIR_TYPE_ARRAY ? "[" : t->kind == AIR_TYPE_VECTOR ? "<" :
                         t->packed ? "<{ " : "{ ";
      const char *close = t->kind == AIR_TYPE_ARRAY ? "]" : t->kind == AIR_TYPE_VECTOR ? ">" :
                          t->packed ? " }>" : " }";
      fputs(open, fp);
      for (unsigned i = 0; i < c->num_elems; i++) {
         if (i)
            fputs(", ", fp);
         if (c->ckind == AIR_CONST_AGGREGATE) {
            print_typed(p, c->elems[i]);
         } else {
            const struct air_type *et = elem_type(t, i);
            print_type(p, et);
            fputc(' ', fp);
            if (air_type_is_float(et))
               print_float(p, et, c->data[i]);
            else
               print_int(p, et, c->data[i]);
         }
      }
      fputs(close, fp);
      break;
   }
   case AIR_CONST_EXPR:
      switch (c->expr_op) {
      case AIR_OP_CAST: {
         static const char *names[] = {
            "trunc", "zext", "sext", "fptoui", "fptosi", "uitofp", "sitofp", "fptrunc",
            "fpext", "ptrtoint", "inttoptr", "bitcast", "addrspacecast",
         };
         const struct air_value *op = c->operands[0];
         /* with opaque pointers a pointer-to-pointer bitcast is nothing */
         if (c->expr_sub == AIR_CAST_BITCAST && t->kind == AIR_TYPE_POINTER &&
             op->type->kind == AIR_TYPE_POINTER && op->type->addrspace == t->addrspace) {
            print_value(p, op);
            break;
         }
         fprintf(fp, "%s (", c->expr_sub < ARRAY_SIZE(names) ? names[c->expr_sub] : "?cast");
         print_typed(p, op);
         fputs(" to ", fp);
         print_type(p, t);
         fputc(')', fp);
         break;
      }
      case AIR_OP_GEP:
         fprintf(fp, "getelementptr %s(", c->inbounds ? "inbounds " : "");
         print_type(p, c->src_type);
         for (unsigned i = 0; i < c->num_operands; i++) {
            fputs(", ", fp);
            print_typed(p, c->operands[i]);
         }
         fputc(')', fp);
         break;
      default:
         fprintf(fp, "<constant expression %u>", c->expr_op);
         break;
      }
      break;
   }
}

static void
print_value(struct printer *p, const struct air_value *v)
{
   switch (v->kind) {
   case AIR_VALUE_GLOBAL:
   case AIR_VALUE_FUNCTION:
      print_name(p, '@', v->name ? v->name : "?");
      return;
   case AIR_VALUE_CONSTANT:
      print_constant(p, (const struct air_constant *)v);
      return;
   case AIR_VALUE_METADATA:
      fputs("!{}", p->fp); /* not in AIR; kept printable */
      return;
   default: {
      if (v->name) {
         print_name(p, '%', v->name);
         return;
      }
      struct hash_entry *e = _mesa_hash_table_search(p->slots, v);
      if (e)
         fprintf(p->fp, "%%%u", (unsigned)((uintptr_t)e->data - 1));
      else
         fputs("%<no slot>", p->fp);
      return;
   }
   }
}

/* -- metadata ----------------------------------------------------------- */

static void
number_md(struct printer *p, struct air_md *md)
{
   if (!md || md->kind != AIR_MD_NODE)
      return;
   if (_mesa_hash_table_search(p->md_numbers, md))
      return;
   _mesa_hash_table_insert(p->md_numbers, md, (void *)(uintptr_t)(++p->num_md_numbers));
   if (p->num_md_numbers > p->cap_md_order) {
      p->cap_md_order = MAX2(64, p->cap_md_order * 2);
      p->md_order = reralloc(NULL, p->md_order, struct air_md *, p->cap_md_order);
   }
   p->md_order[p->num_md_numbers - 1] = md;
   for (unsigned i = 0; i < md->num_ops; i++)
      number_md(p, md->ops[i]);
}

static void
print_md_ref(struct printer *p, const struct air_md *md)
{
   if (!md) {
      fputs("null", p->fp);
      return;
   }
   switch (md->kind) {
   case AIR_MD_STRING:
      fputs("!\"", p->fp);
      print_escaped(p, md->str, md->len);
      fputc('"', p->fp);
      break;
   case AIR_MD_VALUE:
      print_typed(p, md->value);
      break;
   case AIR_MD_NODE: {
      struct hash_entry *e = _mesa_hash_table_search(p->md_numbers, md);
      fprintf(p->fp, "!%u", e ? (unsigned)((uintptr_t)e->data - 1) : ~0u);
      break;
   }
   default:
      fputs("!{}", p->fp);
      break;
   }
}

static void
print_md_node(struct printer *p, const struct air_md *md)
{
   fputs(md->distinct ? "distinct !{" : "!{", p->fp);
   for (unsigned i = 0; i < md->num_ops; i++) {
      if (i)
         fputs(", ", p->fp);
      print_md_ref(p, md->ops[i]);
   }
   fputc('}', p->fp);
}

/* -- instructions ------------------------------------------------------- */

static void
print_fmf(struct printer *p, unsigned f)
{
   if ((f & AIR_FMF_UNSAFE_ALGEBRA) ||
       (f & 0xfe) == 0xfe) {
      fputs("fast ", p->fp);
      return;
   }
   static const struct { unsigned bit; const char *name; } flags[] = {
      { AIR_FMF_REASSOC, "reassoc" }, { AIR_FMF_NNAN, "nnan" }, { AIR_FMF_NINF, "ninf" },
      { AIR_FMF_NSZ, "nsz" }, { AIR_FMF_ARCP, "arcp" }, { AIR_FMF_CONTRACT, "contract" },
      { AIR_FMF_AFN, "afn" },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(flags); i++)
      if (f & flags[i].bit)
         fprintf(p->fp, "%s ", flags[i].name);
}

/* "label %x" after a branch, a bare "%x" inside a phi */
static void
print_block_ref_as(struct printer *p, const struct air_block *b, bool with_label)
{
   if (with_label)
      fputs("label ", p->fp);
   if (b->name) {
      print_name(p, '%', b->name);
      return;
   }
   struct hash_entry *e = _mesa_hash_table_search(p->slots, b);
   fprintf(p->fp, "%%%u", e ? (unsigned)((uintptr_t)e->data - 1) : ~0u);
}

static void
print_block_ref(struct printer *p, const struct air_block *b)
{
   print_block_ref_as(p, b, true);
}

static void
print_inst(struct printer *p, const struct air_inst *in)
{
   FILE *fp = p->fp;
   const struct air_type *t = in->base.type;
   fputs("  ", fp);
   if (t && t->kind != AIR_TYPE_VOID) {
      print_value(p, &in->base);
      fputs(" = ", fp);
   }

   switch (in->op) {
   case AIR_OP_BINOP: {
      static const char *iname[] = { "add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
                                     "shl", "lshr", "ashr", "and", "or", "xor" };
      bool fp_op = air_type_is_float(air_type_scalar(t));
      const char *name = in->sub < ARRAY_SIZE(iname) ? iname[in->sub] : "?binop";
      if (fp_op) {
         static const char *fname[] = { "fadd", "fsub", "fmul", NULL, "fdiv", NULL, "frem" };
         name = in->sub < ARRAY_SIZE(fname) && fname[in->sub] ? fname[in->sub] : "?fbinop";
      }
      fprintf(fp, "%s ", name);
      if (fp_op) {
         print_fmf(p, in->flags);
      } else if (in->sub == AIR_BINOP_ADD || in->sub == AIR_BINOP_SUB ||
                 in->sub == AIR_BINOP_MUL || in->sub == AIR_BINOP_SHL) {
         if (in->flags & AIR_FLAG_NUW)
            fputs("nuw ", fp);
         if (in->flags & AIR_FLAG_NSW)
            fputs("nsw ", fp);
      } else if (in->sub == AIR_BINOP_UDIV || in->sub == AIR_BINOP_SDIV ||
                 in->sub == AIR_BINOP_LSHR || in->sub == AIR_BINOP_ASHR) {
         if (in->flags & AIR_FLAG_EXACT)
            fputs("exact ", fp);
      } else if (in->sub == AIR_BINOP_OR && (in->flags & 1)) {
         fputs("disjoint ", fp);
      }
      print_typed(p, in->operands[0]);
      fputs(", ", fp);
      print_value(p, in->operands[1]);
      break;
   }
   case AIR_OP_UNOP:
      fputs("fneg ", fp);
      print_fmf(p, in->flags);
      print_typed(p, in->operands[0]);
      break;
   case AIR_OP_CAST: {
      static const char *names[] = {
         "trunc", "zext", "sext", "fptoui", "fptosi", "uitofp", "sitofp", "fptrunc",
         "fpext", "ptrtoint", "inttoptr", "bitcast", "addrspacecast",
      };
      fprintf(fp, "%s ", in->sub < ARRAY_SIZE(names) ? names[in->sub] : "?cast");
      if (in->sub == AIR_CAST_ZEXT && (in->flags & 1))
         fputs("nneg ", fp);
      print_typed(p, in->operands[0]);
      fputs(" to ", fp);
      print_type(p, t);
      break;
   }
   case AIR_OP_GEP:
      fprintf(fp, "getelementptr %s", in->inbounds ? "inbounds " : "");
      print_type(p, in->aux_type);
      for (unsigned i = 0; i < in->num_operands; i++) {
         fputs(", ", fp);
         print_typed(p, in->operands[i]);
      }
      break;
   case AIR_OP_SELECT:
      fputs("select ", fp);
      print_fmf(p, in->flags);
      print_typed(p, in->operands[0]);
      fputs(", ", fp);
      print_typed(p, in->operands[1]);
      fputs(", ", fp);
      print_typed(p, in->operands[2]);
      break;
   case AIR_OP_EXTRACTELT:
      fputs("extractelement ", fp);
      print_typed(p, in->operands[0]);
      fputs(", ", fp);
      print_typed(p, in->operands[1]);
      break;
   case AIR_OP_INSERTELT:
      fputs("insertelement ", fp);
      for (unsigned i = 0; i < 3; i++) {
         if (i)
            fputs(", ", fp);
         print_typed(p, in->operands[i]);
      }
      break;
   case AIR_OP_SHUFFLEVEC:
      fputs("shufflevector ", fp);
      for (unsigned i = 0; i < 3; i++) {
         if (i)
            fputs(", ", fp);
         print_typed(p, in->operands[i]);
      }
      break;
   case AIR_OP_CMP: {
      static const char *fpred[] = { "false", "oeq", "ogt", "oge", "olt", "ole", "one", "ord",
                                     "uno", "ueq", "ugt", "uge", "ult", "ule", "une", "true" };
      static const char *ipred[] = { "eq", "ne", "ugt", "uge", "ult", "ule", "sgt", "sge",
                                     "slt", "sle" };
      if (in->sub < 16) {
         fprintf(fp, "fcmp ");
         print_fmf(p, in->flags);
         fprintf(fp, "%s ", fpred[in->sub]);
      } else if (in->sub >= 32 && in->sub < 42) {
         fprintf(fp, "icmp %s ", ipred[in->sub - 32]);
      } else {
         fprintf(fp, "?cmp%u ", in->sub);
      }
      print_typed(p, in->operands[0]);
      fputs(", ", fp);
      print_value(p, in->operands[1]);
      break;
   }
   case AIR_OP_PHI:
      fputs("phi ", fp);
      if (air_type_is_float(air_type_scalar(t)))
         print_fmf(p, in->flags);
      print_type(p, t);
      for (unsigned i = 0; i < in->num_operands; i++) {
         fputs(i ? ", [ " : " [ ", fp);
         print_value(p, in->operands[i]);
         fputs(", ", fp);
         print_block_ref_as(p, in->targets[i], false);
         fputs(" ]", fp);
      }
      break;
   case AIR_OP_ALLOCA: {
      fputs("alloca ", fp);
      print_type(p, in->aux_type);
      const struct air_constant *n = air_value_as_constant(in->operands[0]);
      if (!n || n->ckind != AIR_CONST_INT || n->u != 1) {
         fputs(", ", fp);
         print_typed(p, in->operands[0]);
      }
      if (in->align)
         fprintf(fp, ", align %u", in->align);
      if (t->addrspace)
         fprintf(fp, ", addrspace(%u)", t->addrspace);
      break;
   }
   case AIR_OP_LOAD:
      fprintf(fp, "load %s", in->is_volatile ? "volatile " : "");
      print_type(p, t);
      fputs(", ", fp);
      print_typed(p, in->operands[0]);
      if (in->align)
         fprintf(fp, ", align %u", in->align);
      break;
   case AIR_OP_STORE:
      fprintf(fp, "store %s", in->is_volatile ? "volatile " : "");
      print_typed(p, in->operands[0]);
      fputs(", ", fp);
      print_typed(p, in->operands[1]);
      if (in->align)
         fprintf(fp, ", align %u", in->align);
      break;
   case AIR_OP_CALL: {
      fputs(in->tail ? "tail call " : "call ", fp);
      print_fmf(p, in->flags);
      /* LLVM's CallingConv::ID, of which AIR uses only C and fastcc */
      if (in->cc == 8)
         fputs("fastcc ", fp);
      else if (in->cc)
         fprintf(fp, "cc%u ", in->cc);
      const struct air_type *fty = in->aux_type;
      if (fty->vararg)
         print_type(p, fty);
      else
         print_type(p, fty->elem);
      fputc(' ', fp);
      print_value(p, in->operands[0]);
      fputc('(', fp);
      for (unsigned i = 1; i < in->num_operands; i++) {
         if (i > 1)
            fputs(", ", fp);
         print_typed(p, in->operands[i]);
      }
      fputc(')', fp);
      break;
   }
   case AIR_OP_EXTRACTVAL:
      fputs("extractvalue ", fp);
      print_typed(p, in->operands[0]);
      for (unsigned i = 0; i < in->num_indices; i++)
         fprintf(fp, ", %u", in->indices[i]);
      break;
   case AIR_OP_INSERTVAL:
      fputs("insertvalue ", fp);
      print_typed(p, in->operands[0]);
      fputs(", ", fp);
      print_typed(p, in->operands[1]);
      for (unsigned i = 0; i < in->num_indices; i++)
         fprintf(fp, ", %u", in->indices[i]);
      break;
   case AIR_OP_FREEZE:
      fputs("freeze ", fp);
      print_typed(p, in->operands[0]);
      break;
   case AIR_OP_RET:
      if (in->num_operands) {
         fputs("ret ", fp);
         print_typed(p, in->operands[0]);
      } else {
         fputs("ret void", fp);
      }
      break;
   case AIR_OP_BR:
      fputs("br ", fp);
      if (in->num_targets == 1) {
         print_block_ref(p, in->targets[0]);
      } else {
         print_typed(p, in->operands[0]);
         fputs(", ", fp);
         print_block_ref(p, in->targets[0]);
         fputs(", ", fp);
         print_block_ref(p, in->targets[1]);
      }
      break;
   case AIR_OP_SWITCH:
      fputs("switch ", fp);
      print_typed(p, in->operands[0]);
      fputs(", ", fp);
      print_block_ref(p, in->targets[0]);
      fputs(" [", fp);
      for (unsigned k = 1; k < in->num_targets; k++) {
         fputs("\n    ", fp);
         print_typed(p, in->operands[k]);
         fputs(", ", fp);
         print_block_ref(p, in->targets[k]);
      }
      fputs("\n  ]", fp);
      break;
   case AIR_OP_UNREACHABLE:
      fputs("unreachable", fp);
      break;
   default:
      fprintf(fp, "<opcode %u>", in->op);
      break;
   }

   for (unsigned i = 0; i < in->num_attachments; i++) {
      unsigned k = in->attachments[i].kind;
      const char *kind = k < p->m->num_md_kinds && p->m->md_kind_names[k] ? p->m->md_kind_names[k] : "?";
      fputs(", ", fp);
      print_name(p, '!', kind);
      fputc(' ', fp);
      print_md_ref(p, in->attachments[i].md);
   }
   fputc('\n', fp);
}

static const char *
linkage_name(unsigned l)
{
   switch (l) {
   case 0: case 5: case 6: case 15: return "";
   case 2: return "appending ";
   case 3: return "internal ";
   case 7: return "extern_weak ";
   case 8: return "common ";
   case 9: case 13: case 14: return "private ";
   case 12: return "available_externally ";
   case 1: case 16: return "weak ";
   case 10: case 17: return "weak_odr ";
   case 4: case 18: return "linkonce ";
   case 11: case 19: return "linkonce_odr ";
   default: return "?linkage ";
   }
}

static const char *
cc_name(unsigned cc)
{
   /* LLVM's CallingConv::ID; AIR uses only C (0) and fastcc (8) */
   switch (cc) {
   case 0: return "";
   case 8: return "fastcc ";
   case 9: return "coldcc ";
   default: return NULL;
   }
}

static const char *
unnamed_addr_name(unsigned u)
{
   return u == 1 ? "unnamed_addr " : u == 2 ? "local_unnamed_addr " : "";
}

static void
print_function(struct printer *p, const struct air_function *f)
{
   FILE *fp = p->fp;
   const struct air_type *fty = f->fn_type;

   if (f->is_declaration) {
      fprintf(fp, "declare %s", f->linkage == 7 ? "extern_weak " : "");
      if (cc_name(f->cc))
         fputs(cc_name(f->cc), fp);
      else
         fprintf(fp, "cc%u ", f->cc);
      print_type(p, fty->elem);
      fputc(' ', fp);
      print_name(p, '@', f->base.name);
      fputc('(', fp);
      for (unsigned i = 0; i < fty->num_members; i++) {
         if (i)
            fputs(", ", fp);
         print_type(p, fty->members[i]);
      }
      if (fty->vararg)
         fputs(fty->num_members ? ", ..." : "...", fp);
      fputc(')', fp);
      if (f->unnamed_addr)
         fprintf(fp, " %s", f->unnamed_addr == 1 ? "unnamed_addr" : "local_unnamed_addr");
      if (f->align)
         fprintf(fp, " align %u", f->align);
      fputs("\n\n", fp);
      return;
   }

   p->slots = _mesa_pointer_hash_table_create(NULL);
   unsigned slot = 0;
   for (unsigned i = 0; i < f->num_args; i++)
      if (!f->args[i].name)
         _mesa_hash_table_insert(p->slots, &f->args[i], (void *)(uintptr_t)(++slot));
   for (unsigned b = 0; b < f->num_blocks; b++) {
      const struct air_block *blk = &f->blocks[b];
      if (!blk->name)
         _mesa_hash_table_insert(p->slots, blk, (void *)(uintptr_t)(++slot));
      for (unsigned i = 0; i < blk->num_insts; i++) {
         const struct air_inst *in = blk->insts[i];
         if (in->base.type && in->base.type->kind != AIR_TYPE_VOID && !in->base.name)
            _mesa_hash_table_insert(p->slots, in, (void *)(uintptr_t)(++slot));
      }
   }

   fprintf(fp, "define %s", linkage_name(f->linkage));
   if (cc_name(f->cc))
      fputs(cc_name(f->cc), fp);
   else
      fprintf(fp, "cc%u ", f->cc);
   print_type(p, fty->elem);
   fputc(' ', fp);
   print_name(p, '@', f->base.name);
   fputc('(', fp);
   for (unsigned i = 0; i < f->num_args; i++) {
      if (i)
         fputs(", ", fp);
      print_typed(p, &f->args[i]);
   }
   fputc(')', fp);
   /* after the argument list, where the syntax puts it */
   if (f->unnamed_addr)
      fprintf(fp, " %s", f->unnamed_addr == 1 ? "unnamed_addr" : "local_unnamed_addr");
   if (f->align)
      fprintf(fp, " align %u", f->align);
   if (f->section) {
      fputs(" section \"", fp);
      print_escaped(p, f->section, strlen(f->section));
      fputc('"', fp);
   }
   fputs(" {\n", fp);
   for (unsigned b = 0; b < f->num_blocks; b++) {
      const struct air_block *blk = &f->blocks[b];
      if (b > 0 || blk->name) {
         if (b > 0)
            fputc('\n', fp);
         if (blk->name) {
            /* a label is a name without its sigil */
            print_name(p, '%', blk->name);
            fputs(":\n", fp);
         } else {
            struct hash_entry *e = _mesa_hash_table_search(p->slots, blk);
            fprintf(fp, "%u:\n", (unsigned)((uintptr_t)e->data - 1));
         }
      }
      for (unsigned i = 0; i < blk->num_insts; i++)
         print_inst(p, blk->insts[i]);
   }
   fputs("}\n\n", fp);
   _mesa_hash_table_destroy(p->slots, NULL);
   p->slots = NULL;
}

void
air_module_print(const struct air_module *m, FILE *fp)
{
   struct printer p = {
      .m = m,
      .fp = fp,
      .md_numbers = _mesa_pointer_hash_table_create(NULL),
   };

   if (m->source_filename) {
      fputs("source_filename = \"", fp);
      print_escaped(&p, m->source_filename, strlen(m->source_filename));
      fputs("\"\n", fp);
   }
   if (m->datalayout)
      fprintf(fp, "target datalayout = \"%s\"\n", m->datalayout);
   if (m->triple)
      fprintf(fp, "target triple = \"%s\"\n", m->triple);
   fputc('\n', fp);

   for (unsigned i = 0; i < m->num_types; i++) {
      const struct air_type *t = &m->types[i];
      if (t->kind != AIR_TYPE_STRUCT || !t->name)
         continue;
      print_name(&p, '%', t->name);
      fputs(" = type ", fp);
      if (t->opaque) {
         fputs("opaque\n", fp);
         continue;
      }
      struct air_type anon = *t;
      anon.name = NULL;
      print_type(&p, &anon);
      fputc('\n', fp);
   }
   fputc('\n', fp);

   /* metadata numbers: the module's first, then what instructions attach */
   for (unsigned i = 0; i < m->num_named_md; i++)
      for (unsigned k = 0; k < m->named_md[i].num_ops; k++)
         number_md(&p, m->named_md[i].ops[k]);
   for (unsigned i = 0; i < m->num_md; i++)
      number_md(&p, m->md[i]);
   for (unsigned f = 0; f < m->num_functions; f++) {
      const struct air_function *fn = m->functions[f];
      for (unsigned b = 0; b < fn->num_blocks; b++)
         for (unsigned i = 0; i < fn->blocks[b].num_insts; i++) {
            const struct air_inst *in = fn->blocks[b].insts[i];
            for (unsigned a = 0; a < in->num_attachments; a++)
               number_md(&p, in->attachments[a].md);
         }
   }

   for (unsigned i = 0; i < m->num_globals; i++) {
      const struct air_global *g = m->globals[i];
      print_name(&p, '@', g->base.name);
      fputs(" = ", fp);
      if (!g->init && (g->linkage == 0 || g->linkage == 5 || g->linkage == 6 || g->linkage == 15))
         fputs("external ", fp);
      else
         fputs(linkage_name(g->linkage), fp);
      fputs(unnamed_addr_name(g->unnamed_addr), fp);
      if (g->addrspace)
         fprintf(fp, "addrspace(%u) ", g->addrspace);
      if (g->externally_initialized)
         fputs("externally_initialized ", fp);
      fputs(g->is_const ? "constant " : "global ", fp);
      print_type(&p, g->value_type);
      if (g->init) {
         fputc(' ', fp);
         print_value(&p, g->init);
      }
      if (g->section) {
         fputs(", section \"", fp);
         print_escaped(&p, g->section, strlen(g->section));
         fputc('"', fp);
      }
      if (g->align)
         fprintf(fp, ", align %u", g->align);
      fputc('\n', fp);
   }
   fputc('\n', fp);

   for (unsigned i = 0; i < m->num_functions; i++)
      print_function(&p, m->functions[i]);

   for (unsigned i = 0; i < m->num_named_md; i++) {
      const struct air_named_md *n = &m->named_md[i];
      print_name(&p, '!', n->name);
      fputs(" = !{", fp);
      for (unsigned k = 0; k < n->num_ops; k++) {
         if (k)
            fputs(", ", fp);
         print_md_ref(&p, n->ops[k]);
      }
      fputs("}\n", fp);
   }
   fputc('\n', fp);
   for (unsigned i = 0; i < p.num_md_numbers; i++) {
      fprintf(fp, "!%u = ", i);
      print_md_node(&p, p.md_order[i]);
      fputc('\n', fp);
   }

   _mesa_hash_table_destroy(p.md_numbers, NULL);
   ralloc_free(p.md_order);
}
