/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

#include "air_bitcode.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/macros.h"
#include "util/ralloc.h"

enum {
   ABBREV_ID_END_BLOCK = 0,
   ABBREV_ID_ENTER_SUBBLOCK = 1,
   ABBREV_ID_DEFINE_ABBREV = 2,
   ABBREV_ID_UNABBREV_RECORD = 3,
   ABBREV_ID_FIRST = 4,
};

enum {
   BLOCKINFO_BLOCK_ID = 0,
   BLOCKINFO_CODE_SETBID = 1,
};

/* LLVM's writer gives BLOCKINFO abbreviations for three or four block ids.
 * Each block entered looks its id up in this list, so a long one is a cost
 * paid per block: it is kept short.
 */
#define MAX_BLOCKINFO_IDS 64

static void
bs_fail(struct air_bitstream *bs, const char *fmt, ...)
{
   if (bs->error[0])
      return; /* the first error is the one that explains */
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(bs->error, sizeof(bs->error), fmt, ap);
   va_end(ap);
   bs->pos = bs->size_bits;
}

static uint64_t
read_fixed(struct air_bitstream *bs, unsigned width)
{
   if (width == 0)
      return 0;
   if (width > 64 || bs->pos + width > bs->size_bits) {
      bs_fail(bs, "read of %u bits past the end at bit %" PRIu64, width, bs->pos);
      return 0;
   }

   uint64_t v = 0;
   unsigned got = 0;
   while (got < width) {
      unsigned bit = bs->pos & 7;
      unsigned take = MIN2(8 - bit, width - got);
      uint64_t chunk = (bs->data[bs->pos >> 3] >> bit) & ((1u << take) - 1);
      v |= chunk << got;
      got += take;
      bs->pos += take;
   }
   return v;
}

static uint64_t
read_vbr(struct air_bitstream *bs, unsigned width)
{
   if (width < 2 || width > 32) {
      bs_fail(bs, "VBR width %u", width);
      return 0;
   }
   const uint64_t hi = 1ull << (width - 1);
   uint64_t v = 0;
   unsigned shift = 0;
   for (;;) {
      uint64_t chunk = read_fixed(bs, width);
      if (shift < 64)
         v |= (chunk & (hi - 1)) << shift;
      else if (chunk & (hi - 1))
         bs_fail(bs, "VBR value wider than 64 bits");
      if (!(chunk & hi) || bs->error[0])
         return v;
      shift += width - 1;
   }
}

static void
align32(struct air_bitstream *bs)
{
   bs->pos = MIN2(ALIGN_POT(bs->pos, 32), bs->size_bits);
}

static bool
list_add(struct air_bitstream *bs, struct air_abbrev_list *l, struct air_abbrev *a)
{
   if (l->num == l->cap) {
      unsigned cap = MAX2(8, l->cap * 2);
      struct air_abbrev **abbrevs = cap > l->cap ?
         reralloc(bs->mem_ctx, l->abbrevs, struct air_abbrev *, cap) : NULL;
      if (!abbrevs) {
         bs_fail(bs, "no memory for %u abbreviations", l->num + 1);
         return false;
      }
      l->abbrevs = abbrevs;
      l->cap = cap;
   }
   l->abbrevs[l->num++] = a;
   return true;
}

/* the index of block_id's BLOCKINFO entry, or -1 */
static int
blockinfo_index(struct air_bitstream *bs, unsigned block_id, bool create)
{
   for (unsigned i = 0; i < bs->num_blockinfo; i++) {
      if (bs->blockinfo[i].block_id == block_id)
         return i;
   }
   if (!create)
      return -1;
   if (bs->num_blockinfo == MAX_BLOCKINFO_IDS) {
      bs_fail(bs, "BLOCKINFO for more than %u block ids", MAX_BLOCKINFO_IDS);
      return -1;
   }
   __typeof__(bs->blockinfo) info = reralloc(bs->mem_ctx, bs->blockinfo, __typeof__(*bs->blockinfo),
                                             bs->num_blockinfo + 1);
   if (!info) {
      bs_fail(bs, "no memory for BLOCKINFO");
      return -1;
   }
   bs->blockinfo = info;
   bs->blockinfo[bs->num_blockinfo].block_id = block_id;
   memset(&bs->blockinfo[bs->num_blockinfo].abbrevs, 0, sizeof(struct air_abbrev_list));
   return bs->num_blockinfo++;
}

static struct air_abbrev *
read_abbrev(struct air_bitstream *bs)
{
   uint64_t n = read_vbr(bs, 5);
   /* an operand takes at least 4 bits (a literal flag and a value, or a flag
    * and an encoding), and the count sizes the allocation below
    */
   if (n > air_bs_bits_left(bs) / 4) {
      bs_fail(bs, "abbreviation of %" PRIu64 " operands, more than its block holds", n);
      return NULL;
   }
   struct air_abbrev *a = rzalloc(bs->mem_ctx, struct air_abbrev);
   if (a)
      a->ops = rzalloc_array(bs->mem_ctx, struct air_abbrev_op, MAX2(n, 1));
   if (!a || !a->ops) {
      bs_fail(bs, "no memory for an abbreviation of %" PRIu64 " operands", n);
      return NULL;
   }
   for (unsigned i = 0; i < n && !bs->error[0]; i++) {
      struct air_abbrev_op *op = &a->ops[a->num_ops++];
      if (read_fixed(bs, 1)) {
         op->kind = AIR_ABBREV_LITERAL;
         op->value = read_vbr(bs, 8);
         continue;
      }
      switch (read_fixed(bs, 3)) {
      case 1:
         op->kind = AIR_ABBREV_FIXED;
         op->value = read_vbr(bs, 5);
         break;
      case 2:
         op->kind = AIR_ABBREV_VBR;
         op->value = read_vbr(bs, 5);
         break;
      case 3:
         op->kind = AIR_ABBREV_ARRAY;
         break;
      case 4:
         op->kind = AIR_ABBREV_CHAR6;
         break;
      case 5:
         op->kind = AIR_ABBREV_BLOB;
         break;
      default:
         bs_fail(bs, "unknown abbreviation encoding");
         return NULL;
      }
      /* a zero-width fixed or VBR field reads nothing: it is the literal 0 */
      if ((op->kind == AIR_ABBREV_FIXED || op->kind == AIR_ABBREV_VBR) && op->value == 0) {
         op->kind = AIR_ABBREV_LITERAL;
         op->value = 0;
      }
   }
   return bs->error[0] ? NULL : a;
}

static void
push_op(struct air_bitstream *bs, unsigned *n, uint64_t v)
{
   if (*n == bs->ops_cap) {
      /* (the doubling stops short of wrapping: a capacity that wrapped to a
       * small one would be written past)
       */
      unsigned cap = bs->ops_cap < UINT32_MAX / 2 ? MAX2(64, bs->ops_cap * 2) : 0;
      uint64_t *ops = cap ? reralloc(bs->mem_ctx, bs->ops, uint64_t, cap) : NULL;
      if (!ops) {
         bs_fail(bs, "no memory for a record of %u operands", *n + 1);
         return;
      }
      bs->ops = ops;
      bs->ops_cap = cap;
   }
   bs->ops[(*n)++] = v;
}

static uint64_t
read_scalar(struct air_bitstream *bs, const struct air_abbrev_op *op)
{
   static const char char6[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
   switch (op->kind) {
   case AIR_ABBREV_LITERAL:
      return op->value;
   case AIR_ABBREV_FIXED:
      return read_fixed(bs, op->value);
   case AIR_ABBREV_VBR:
      return read_vbr(bs, op->value);
   case AIR_ABBREV_CHAR6:
      return char6[read_fixed(bs, 6)];
   default:
      bs_fail(bs, "array or blob where a scalar belongs");
      return 0;
   }
}

static struct air_bs_entry
read_abbreviated(struct air_bitstream *bs, const struct air_abbrev *a)
{
   struct air_bs_entry e = { .kind = AIR_BS_RECORD };
   unsigned n = 0;

   if (a->num_ops == 0 || a->ops[0].kind == AIR_ABBREV_ARRAY || a->ops[0].kind == AIR_ABBREV_BLOB) {
      bs_fail(bs, "abbreviation without a record code");
      e.kind = AIR_BS_ERROR;
      return e;
   }
   e.id = read_scalar(bs, &a->ops[0]);

   for (unsigned i = 1; i < a->num_ops && !bs->error[0]; i++) {
      const struct air_abbrev_op *op = &a->ops[i];
      if (op->kind == AIR_ABBREV_ARRAY) {
         if (i + 2 != a->num_ops) {
            bs_fail(bs, "array not second to last in its abbreviation");
            break;
         }
         /* An element must be read from the stream: one that takes no bits
          * (a literal, or a field of width 0) would let a length of 2^32 cost
          * nothing to write and everything to read. LLVM refuses it too.
          */
         const struct air_abbrev_op *elt = &a->ops[i + 1];
         if (elt->kind != AIR_ABBREV_FIXED && elt->kind != AIR_ABBREV_VBR &&
             elt->kind != AIR_ABBREV_CHAR6) {
            bs_fail(bs, "array whose element is not an encoding");
            break;
         }
         uint64_t len = read_vbr(bs, 6);
         if (len > air_bs_bits_left(bs)) {
            bs_fail(bs, "array of %" PRIu64 " elements, more than its block holds", len);
            break;
         }
         for (uint64_t j = 0; j < len && !bs->error[0]; j++)
            push_op(bs, &n, read_scalar(bs, elt));
         break;
      } else if (op->kind == AIR_ABBREV_BLOB) {
         uint64_t len = read_vbr(bs, 6);
         align32(bs);
         /* (len * 8 wraps for a length of 2^61; what is left does not) */
         if (len > (bs->size_bits - bs->pos) / 8) {
            bs_fail(bs, "blob past the end");
            break;
         }
         e.blob = bs->data + bs->pos / 8;
         e.blob_size = len;
         bs->pos += len * 8;
         align32(bs);
      } else {
         push_op(bs, &n, read_scalar(bs, op));
      }
   }

   e.num_ops = n;
   e.ops = bs->ops;
   if (bs->error[0])
      e.kind = AIR_BS_ERROR;
   return e;
}

static struct air_bs_scope *
cur(struct air_bitstream *bs)
{
   return &bs->scopes[bs->depth];
}

bool
air_bs_init(struct air_bitstream *bs, void *mem_ctx, const void *data, size_t size)
{
   memset(bs, 0, sizeof(*bs));
   bs->mem_ctx = mem_ctx;
   bs->data = data;
   bs->size_bits = (uint64_t)size * 8;
   bs->scope_cap = 16;
   bs->scopes = rzalloc_array(mem_ctx, struct air_bs_scope, bs->scope_cap);
   if (!bs->scopes) {
      bs_fail(bs, "no memory for the bitstream's blocks");
      return false;
   }
   bs->scopes[0].block_id = ~0u;
   bs->scopes[0].abbrev_width = 2;
   bs->scopes[0].end_bit = bs->size_bits;

   const uint8_t *b = data;
   if (size < 4 || b[0] != 'B' || b[1] != 'C' || b[2] != 0xc0 || b[3] != 0xde) {
      bs_fail(bs, "not LLVM bitcode (no 'BC' 0xC0DE magic)");
      return false;
   }
   bs->pos = 32;
   return true;
}

static bool
read_blockinfo(struct air_bitstream *bs, unsigned width, uint64_t end)
{
   int target = -1;
   for (;;) {
      if (bs->error[0])
         return false;
      if (bs->pos >= end) {
         bs_fail(bs, "BLOCKINFO without its END_BLOCK");
         return false;
      }
      unsigned id = read_fixed(bs, width);
      if (id == ABBREV_ID_END_BLOCK) {
         align32(bs);
         return true;
      } else if (id == ABBREV_ID_DEFINE_ABBREV) {
         struct air_abbrev *a = read_abbrev(bs);
         if (!a)
            return false;
         if (target < 0) {
            bs_fail(bs, "BLOCKINFO abbreviation before SETBID");
            return false;
         }
         if (!list_add(bs, &bs->blockinfo[target].abbrevs, a))
            return false;
      } else if (id == ABBREV_ID_UNABBREV_RECORD) {
         unsigned code = read_vbr(bs, 6);
         uint64_t n = read_vbr(bs, 6);
         uint64_t first = 0;
         /* (a count of 2^32 past the end would otherwise be counted out to
          * the last, each read failing at once)
          */
         for (uint64_t i = 0; i < n && !bs->error[0]; i++) {
            uint64_t v = read_vbr(bs, 6);
            if (i == 0)
               first = v;
         }
         if (bs->error[0])
            return false;
         if (code == BLOCKINFO_CODE_SETBID) {
            if (n < 1 || first > UINT32_MAX) {
               bs_fail(bs, "SETBID without a block id");
               return false;
            }
            target = blockinfo_index(bs, first, true);
            if (target < 0)
               return false;
         }
         /* BLOCKNAME and SETRECORDNAME only name things for dumpers */
      } else if (id == ABBREV_ID_ENTER_SUBBLOCK) {
         bs_fail(bs, "a block inside BLOCKINFO");
         return false;
      } else {
         bs_fail(bs, "an abbreviated record in BLOCKINFO");
         return false;
      }
   }
}

struct air_bs_entry
air_bs_next(struct air_bitstream *bs)
{
   for (;;) {
      struct air_bs_entry e = { .kind = AIR_BS_ERROR };
      struct air_bs_scope *s = cur(bs);

      if (bs->error[0])
         return e;
      if (bs->depth == 0 && bs->pos + 32 > bs->size_bits) {
         e.kind = AIR_BS_END;
         return e;
      }
      if (bs->pos >= s->end_bit) {
         bs_fail(bs, "block %u runs past its length", s->block_id);
         return e;
      }

      unsigned id = read_fixed(bs, s->abbrev_width);
      switch (id) {
      case ABBREV_ID_END_BLOCK:
         align32(bs);
         if (bs->depth == 0) {
            bs_fail(bs, "END_BLOCK at the top level");
            return e;
         }
         bs->depth--;
         e.kind = AIR_BS_END_BLOCK;
         return e;

      case ABBREV_ID_ENTER_SUBBLOCK: {
         unsigned block_id = read_vbr(bs, 8);
         bs->pending_width = read_vbr(bs, 4);
         align32(bs);
         uint64_t words = read_fixed(bs, 32);
         bs->pending_end = bs->pos + words * 32;
         if (bs->pending_end > bs->size_bits) {
            bs_fail(bs, "block %u longer than the stream", block_id);
            return e;
         }
         if (block_id == BLOCKINFO_BLOCK_ID) {
            if (!read_blockinfo(bs, bs->pending_width, bs->pending_end))
               return e;
            continue;
         }
         e.kind = AIR_BS_ENTER_BLOCK;
         e.id = block_id;
         return e;
      }

      case ABBREV_ID_DEFINE_ABBREV: {
         struct air_abbrev *a = read_abbrev(bs);
         if (!a || !list_add(bs, &s->abbrevs, a))
            return e;
         continue;
      }

      case ABBREV_ID_UNABBREV_RECORD: {
         unsigned n = 0;
         e.kind = AIR_BS_RECORD;
         e.id = read_vbr(bs, 6);
         uint64_t count = read_vbr(bs, 6);
         /* each operand is a VBR6, 6 bits at least */
         if (count > air_bs_bits_left(bs) / 6) {
            bs_fail(bs, "record of %" PRIu64 " operands, more than its block holds", count);
            e.kind = AIR_BS_ERROR;
            return e;
         }
         for (uint64_t i = 0; i < count && !bs->error[0]; i++)
            push_op(bs, &n, read_vbr(bs, 6));
         e.num_ops = n;
         e.ops = bs->ops;
         if (bs->error[0])
            e.kind = AIR_BS_ERROR;
         return e;
      }

      default: {
         unsigned k = id - ABBREV_ID_FIRST;
         if (k < s->num_info)
            return read_abbreviated(bs, bs->blockinfo[s->info].abbrevs.abbrevs[k]);
         if (k - s->num_info >= s->abbrevs.num) {
            bs_fail(bs, "undefined abbreviation %u in block %u", id, s->block_id);
            return e;
         }
         return read_abbreviated(bs, s->abbrevs.abbrevs[k - s->num_info]);
      }
      }
   }
}

bool
air_bs_enter(struct air_bitstream *bs, unsigned block_id)
{
   if (bs->depth + 1 == bs->scope_cap) {
      struct air_bs_scope *scopes =
         reralloc(bs->mem_ctx, bs->scopes, struct air_bs_scope, bs->scope_cap * 2);
      if (!scopes) {
         bs_fail(bs, "no memory for block nesting %u deep", bs->depth + 1);
         return false;
      }
      bs->scopes = scopes;
      bs->scope_cap *= 2;
   }
   struct air_bs_scope *s = &bs->scopes[++bs->depth];
   memset(s, 0, sizeof(*s));
   s->block_id = block_id;
   s->abbrev_width = bs->pending_width;
   s->end_bit = bs->pending_end;

   /* the BLOCKINFO abbreviations as they are now: later ones are not this block's */
   int info = blockinfo_index(bs, block_id, false);
   if (info >= 0) {
      s->info = info;
      s->num_info = bs->blockinfo[info].abbrevs.num;
   }
   return true;
}

bool
air_bs_skip_block(struct air_bitstream *bs)
{
   bs->pos = bs->pending_end;
   return true;
}

char *
air_bs_ops_to_string(void *mem_ctx, const uint64_t *ops, unsigned n)
{
   char *s = ralloc_size(mem_ctx, (size_t)n + 1);
   if (!s)
      return NULL;
   for (unsigned i = 0; i < n; i++)
      s[i] = (char)ops[i];
   s[n] = 0;
   return s;
}
