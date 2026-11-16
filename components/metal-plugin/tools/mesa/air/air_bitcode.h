/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The LLVM bitstream: the container format below LLVM IR bitcode. It knows
 * blocks, abbreviations and records, and nothing of what they mean; that is
 * air_module.c's business. Written from the "LLVM Bitcode File Format"
 * document, not from LLVM's sources, so that the frontend links no LLVM.
 */

#ifndef AIR_BITCODE_H
#define AIR_BITCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum air_abbrev_op_kind {
   AIR_ABBREV_LITERAL,
   AIR_ABBREV_FIXED,
   AIR_ABBREV_VBR,
   AIR_ABBREV_ARRAY,
   AIR_ABBREV_CHAR6,
   AIR_ABBREV_BLOB,
};

struct air_abbrev_op {
   enum air_abbrev_op_kind kind;
   uint64_t value; /* the literal, or the width of FIXED/VBR */
};

struct air_abbrev {
   unsigned num_ops;
   struct air_abbrev_op *ops;
};

struct air_abbrev_list {
   unsigned num, cap;
   struct air_abbrev **abbrevs;
};

struct air_bs_scope {
   unsigned block_id;
   unsigned abbrev_width;
   uint64_t end_bit;
   struct air_abbrev_list abbrevs;
};

enum air_bs_entry_kind {
   AIR_BS_ERROR,
   AIR_BS_END,          /* end of the stream */
   AIR_BS_ENTER_BLOCK,  /* .id; the caller enters it or skips it */
   AIR_BS_END_BLOCK,
   AIR_BS_RECORD,       /* .id is the record code, ops/blob below */
};

struct air_bs_entry {
   enum air_bs_entry_kind kind;
   unsigned id;
   unsigned num_ops;
   const uint64_t *ops;
   const uint8_t *blob;
   size_t blob_size;
};

struct air_bitstream {
   void *mem_ctx;
   const uint8_t *data;
   uint64_t size_bits;
   uint64_t pos;

   unsigned depth, scope_cap;
   struct air_bs_scope *scopes;

   /* BLOCKINFO: the abbreviations that every block of an id starts with */
   unsigned num_blockinfo;
   struct {
      unsigned block_id;
      struct air_abbrev_list abbrevs;
   } *blockinfo;

   /* an ENTER_SUBBLOCK header, read but not yet entered or skipped */
   unsigned pending_width;
   uint64_t pending_end;

   /* the current record's operands */
   unsigned ops_cap;
   uint64_t *ops;

   char error[160];
};

/* data must be the bitstream itself: after the 0x0B17C0DE wrapper, starting
 * with the 'BC' 0xC0DE magic, which this checks and skips.
 */
bool air_bs_init(struct air_bitstream *bs, void *mem_ctx,
                 const void *data, size_t size);

/* The next thing at the current level. Records of BLOCKINFO are taken in
 * here and never returned; an ENTER_BLOCK must be followed by
 * air_bs_enter() or air_bs_skip_block().
 */
struct air_bs_entry air_bs_next(struct air_bitstream *bs);
bool air_bs_enter(struct air_bitstream *bs, unsigned block_id);
bool air_bs_skip_block(struct air_bitstream *bs);

/* Word offset (32-bit units) of the current position, for VSTOFFSET and
 * function-entry offsets.
 */
static inline uint64_t
air_bs_word_offset(const struct air_bitstream *bs)
{
   return bs->pos / 32;
}

/* Copies the operands of a record into a NUL-terminated string. */
char *air_bs_ops_to_string(void *mem_ctx, const uint64_t *ops, unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* AIR_BITCODE_H */
