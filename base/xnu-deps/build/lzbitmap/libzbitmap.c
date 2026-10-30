// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Copyright (C) 2022 Corellium LLC
 *
 * Author: Ernesto A. Fernández <ernesto@corellium.com>
 *
 * Ported from libzbitmap (https://github.com/eafer/libzbitmap). Only the
 * decompression code is included.
 */

#include <errno.h>
#include <string.h>
#include "libzbitmap.h"

#define ZBM_MAGIC       "ZBM\x09"
#define ZBM_MAGIC_SZ    4

#define ZBM_MAX_DECMP_CHUNK_SIZE        0x8000
#define ZBM_MAX_DECMP_CHUNK_SIZE_BITS   15

struct uint24 {
    uint8_t     low;
    uint8_t     mid;
    uint8_t     hig;
};

/* This header is shared by both compressed and decompressed chunks */
struct zbm_chunk_hdr {
    struct uint24   len;        /* Length of the chunk */
    struct uint24   decmp_len;  /* Length of the chunk after decompression */
};

/* The full header for compressed chunks */
struct zbm_cmp_chunk_hdr {
    /* Shared with decompressed chunks */
    struct zbm_chunk_hdr hdr;

    /* Offset for each of the three metadata areas */
    struct uint24   meta_off_1;
    struct uint24   meta_off_2;
    struct uint24   meta_off_3;
};

/* Pointer to a half-byte */
struct nybl_ptr {
    uint8_t         *addr;  /* Address of the byte */
    int             nibble; /* Which of the two nibbles? */
};

/* 0-2 and 0xf are not real bitmap indexes */
#define ZBM_BITMAP_COUNT        (16 - 1 - 3)
#define ZBM_BITMAP_BASE         3
#define ZBM_BITMAP_BYTECNT      17
#define ZBM_MAX_PERIOD_BYTECNT  2

struct zbm_bmap {
    uint8_t     bitmap;         /* The bitmap */
    uint8_t     period_bytecnt; /* Read this many bytes to get the new period */
};

struct zbm_state {
    /* Updated during a chunk read */
    uint8_t         *dest;      /* Write the next byte here */
    size_t          dest_left;  /* Room left in destination buffer */
    uint32_t        written;    /* Bytes written so far for current chunk */
    uint16_t        period;     /* Repetition period for decompression, in bytes */

    /* Updated right before a chunk read */
    const uint8_t   *src_end;   /* End of current chunk */
    uint32_t        len;        /* Length of the chunk */
    uint32_t        decmp_len;  /* Expected chunk length after decompression */

    /* Updated after a chunk read */
    const uint8_t   *src;       /* Start of buffer, or current chunk if any */
    size_t          src_left;   /* Room left in the source buffer */
    size_t          prewritten; /* Bytes written for previous chunks */

    /* Current position in data and metadata areas for this chunk */
    const uint8_t   *data;
    const uint8_t   *meta_1;
    const uint8_t   *meta_2;
    struct nybl_ptr meta_3;

    /* Array of bitmaps for the current chunk */
    struct zbm_bmap bitmaps[ZBM_BITMAP_COUNT];
};

static int zbm_check_magic(struct zbm_state *state)
{
    if(state->src_left < ZBM_MAGIC_SZ)
        return -EINVAL;

    if(memcmp(state->src, ZBM_MAGIC, ZBM_MAGIC_SZ))
        return -EINVAL;

    state->src += ZBM_MAGIC_SZ;
    state->src_left -= ZBM_MAGIC_SZ;
    return 0;
}

static uint32_t zbm_u24_to_u32(struct uint24 n)
{
    uint32_t res;

    res = n.hig;
    res <<= 8;
    res += n.mid;
    res <<= 8;
    res += n.low;
    return res;
}

/* Some chunks just have regular uncompressed data, but with a header */
static int zbm_chunk_is_uncompressed(struct zbm_state *state)
{
    return state->len == state->decmp_len + sizeof(struct zbm_chunk_hdr);
}

static int zbm_handle_uncompressed_chunk(struct zbm_state *state)
{
    state->meta_1 = state->meta_2 = NULL;
    state->meta_3.addr = NULL;
    state->meta_3.nibble = 0;
    state->data = state->src + sizeof(struct zbm_chunk_hdr);
    memcpy(state->dest, state->data, state->decmp_len);

    state->dest += state->decmp_len;
    state->dest_left -= state->decmp_len;
    state->written = state->decmp_len;
    return 0;
}

static int zbm_read_nibble(struct nybl_ptr *nybl, const uint8_t *limit, uint8_t *result)
{
    if(nybl->addr >= limit)
        return -EINVAL;

    if(nybl->nibble == 0) {
        *result = *nybl->addr & 0xf;
        nybl->nibble = 1;
    } else {
        *result = (*nybl->addr >> 4) & 0xf;
        nybl->nibble = 0;
        ++nybl->addr;
    }
    return 0;
}

static void zbm_rewind_nibble(struct nybl_ptr *nybl)
{
    if(nybl->nibble == 0) {
        nybl->nibble = 1;
        --nybl->addr;
    } else {
        nybl->nibble = 0;
    }
}

static int zbm_apply_bitmap(struct zbm_state *state, struct zbm_bmap *bitmap)
{
    int i;

    /* The periods are stored in the first metadata area */
    if(bitmap->period_bytecnt) {
        state->period = 0;
        for(i = 0; i < bitmap->period_bytecnt; ++i) {
            if(state->meta_1 >= state->src_end)
                return -EINVAL;
            state->period |= *state->meta_1 << i * 8;
            ++state->meta_1;
        }
    }
    if(state->period == 0)
        return -EINVAL;

    for(i = 0; i < 8; ++i) {
        if(state->written == state->decmp_len)
            break;
        if(bitmap->bitmap & 1 << i) {
            if(state->data >= state->src_end)
                return -EINVAL;
            *state->dest = *state->data;
            ++state->data;
        } else {
            if(state->prewritten + state->written < state->period)
                return -EINVAL;
            *state->dest = *(state->dest - state->period);
        }
        ++state->dest;
        --state->dest_left;
        ++state->written;
    }

    return 0;
}

static int zbm_apply_bitmap_number(struct zbm_state *state, uint8_t bmp_num)
{
    struct zbm_bmap next = {0};

    /* Not a valid bitmap number (it signals a repetition) */
    if(bmp_num == 0xf)
        return -EINVAL;

    /* An actual index in the bitmap array */
    if(bmp_num > ZBM_MAX_PERIOD_BYTECNT)
        return zbm_apply_bitmap(state, &state->bitmaps[bmp_num - ZBM_BITMAP_BASE]);

    /* For < 2, use the next bitmap in the second metadata area */
    if(state->meta_2 >= state->src_end)
        return -EINVAL;
    next.bitmap = *state->meta_2;
    next.period_bytecnt = bmp_num;
    ++state->meta_2;
    return zbm_apply_bitmap(state, &next);
}

/* Find out how many times we need to repeat the current bitmap operation */
static int zbm_read_repetition_count(struct zbm_state *state, uint16_t *repeat)
{
    uint8_t nibble;
    uint16_t total;
    int err;

    /* Don't confuse the trailing bitmaps with a repetition count */
    if(state->decmp_len - state->written <= 8) {
        *repeat = 1;
        return 0;
    }

    err = zbm_read_nibble(&state->meta_3, state->src_end, &nibble);
    if(err)
        return err;

    if(nibble != 0xf) {
        /* No repetition count: the previous bitmap number gets applied once */
        zbm_rewind_nibble(&state->meta_3);
        *repeat = 1;
        return 0;
    }

    /*
     * Under this scheme, repeating a bitmap number 3 times wouldn't save any
     * space, so the repetition count starts from 4.
     */
    total = 4;
    while(nibble == 0xf) {
        err = zbm_read_nibble(&state->meta_3, state->src_end, &nibble);
        if(err)
            return err;
        total += nibble;
        if(total < nibble)
            return -EINVAL;
    }

    *repeat = total;
    return 0;
}

static int zbm_decompress_single_bitmap(struct zbm_state *state)
{
    uint8_t bmp_num;
    uint16_t repeat;
    int i;
    int err;

    /* The current nibble is the offset of the next bitmap to apply */
    err = zbm_read_nibble(&state->meta_3, state->src_end, &bmp_num);
    if(err)
        return err;

    err = zbm_read_repetition_count(state, &repeat);
    if(err)
        return err;

    for(i = 0; i < repeat; ++i) {
        err = zbm_apply_bitmap_number(state, bmp_num);
        if(err)
            return err;
    }
    return 0;
}

/* Pointer to a bit */
struct bit_ptr {
    uint8_t         *addr;  /* Address of the byte */
    int             offset; /* Bit number */
};

/* This function does not perform boundary checks, the caller must do it */
static int zbm_read_single_bit(struct bit_ptr *bit)
{
    int res = *bit->addr >> bit->offset & 1;

    ++bit->offset;
    if(bit->offset != 8)
        return res;
    bit->offset = 0;
    ++bit->addr;
    return res;
}

static int zbm_read_single_bitmap(struct bit_ptr *bit, const uint8_t *limit, struct zbm_bmap *result)
{
    int i;

    result->bitmap = 0;
    result->period_bytecnt = 0;

    /* The bitmap itself */
    for(i = 0; i < 8; ++i) {
        if(bit->addr >= limit)
            return -EINVAL;
        result->bitmap |= zbm_read_single_bit(bit) << i;
    }

    /*
     * The two trailing bits tell us how many bytes to read for the next
     * repetition period
     */
    for(i = 0; i < 2; ++i) {
        if(bit->addr >= limit)
            return -EINVAL;
        result->period_bytecnt |= zbm_read_single_bit(bit) << i;
    }

    return 0;
}

static int zbm_read_bitmaps(struct zbm_state *state)
{
    struct bit_ptr bmap = {0};
    int err, i;

    if(state->len < ZBM_BITMAP_BYTECNT)
        return -EINVAL;

    bmap.addr = (uint8_t *)state->src_end - ZBM_BITMAP_BYTECNT;
    bmap.offset = 0;

    for(i = 0; i < ZBM_BITMAP_COUNT; ++i) {
        err = zbm_read_single_bitmap(&bmap, state->src_end, &state->bitmaps[i]);
        if(err)
            return err;
        if(state->bitmaps[i].period_bytecnt > ZBM_MAX_PERIOD_BYTECNT)
            return -EINVAL;
    }
    return 0;
}

static int zbm_handle_compressed_chunk(struct zbm_state *state)
{
    const struct zbm_cmp_chunk_hdr *hdr = NULL;
    uint32_t meta_off_1, meta_off_2, meta_off_3;
    int err;

    state->written = 0;
    state->period = 8;

    if(state->len < sizeof(*hdr))
        return -EINVAL;
    hdr = (struct zbm_cmp_chunk_hdr *)state->src;
    state->data = state->src + sizeof(*hdr);

    meta_off_1 = zbm_u24_to_u32(hdr->meta_off_1);
    meta_off_2 = zbm_u24_to_u32(hdr->meta_off_2);
    meta_off_3 = zbm_u24_to_u32(hdr->meta_off_3);
    if(meta_off_1 >= state->len || meta_off_2 >= state->len || meta_off_3 >= state->len)
        return -EINVAL;
    state->meta_1 = state->src + meta_off_1;
    state->meta_2 = state->src + meta_off_2;
    state->meta_3.addr = (uint8_t *)state->src + meta_off_3;
    state->meta_3.nibble = 0;

    err = zbm_read_bitmaps(state);
    if(err)
        return err;

    while(state->written < state->decmp_len) {
        err = zbm_decompress_single_bitmap(state);
        if(err)
            return err;
    }

    return 0;
}

static int zbm_handle_chunk(struct zbm_state *state)
{
    const struct zbm_chunk_hdr *decmp_hdr = NULL;

    if(state->src_left < sizeof(*decmp_hdr))
        return -EINVAL;
    decmp_hdr = (struct zbm_chunk_hdr *)state->src;

    state->len = zbm_u24_to_u32(decmp_hdr->len);
    if(state->len > state->src_left)
        return -EINVAL;
    state->src_end = state->src + state->len;

    state->decmp_len = zbm_u24_to_u32(decmp_hdr->decmp_len);
    if(state->decmp_len > ZBM_MAX_DECMP_CHUNK_SIZE)
        return -EINVAL;
    if(!state->dest) /* We just wanted the length, so we are done */
        return 0;
    if(state->decmp_len > state->dest_left)
        return -ERANGE;

    if(zbm_chunk_is_uncompressed(state))
        return zbm_handle_uncompressed_chunk(state);

    return zbm_handle_compressed_chunk(state);
}

int zbm_decompress(void *dest, size_t dest_size, const void *src, size_t src_size, size_t *out_len)
{
    struct zbm_state state = {0};
    int err;

    state.src = src;
    state.src_left = src_size;
    state.dest = dest;
    state.dest_left = dest_size;
    state.prewritten = 0;

    err = zbm_check_magic(&state);
    if(err)
        return err;

    /* The final chunk has zero decompressed length */
    do {
        err = zbm_handle_chunk(&state);
        if(err)
            return err;
        state.src += state.len;
        state.src_left -= state.len;
        state.prewritten += state.decmp_len;
    } while(state.decmp_len != 0);

    *out_len = state.prewritten;
    return 0;
}
