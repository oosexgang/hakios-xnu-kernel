/* SPDX-License-Identifier: GPL-2.0+ OR MIT (taken here under MIT) */
/*
 * Copyright (c) 2022 Corellium LLC
 *
 * Author: Ernesto A. Fernández <ernesto@corellium.com>
 *
 * Ported from libzbitmap (https://github.com/eafer/libzbitmap). Only the
 * decompression code is included.
 */

#ifndef _LIBZBITMAP_H
#define _LIBZBITMAP_H

#include <stdint.h>
#include <stddef.h>

/**
 * zbm_decompress - Decompress an LZBITMAP buffer
 * @dest:       destination buffer (may be NULL)
 * @dest_size:  size of the destination buffer
 * @src:        source buffer
 * @src_size:   size of the source buffer
 * @out_len:    on return, the length of the decompressed output
 *
 * May be called with a NULL destination buffer to retrieve the expected length
 * of the decompressed data. Returns 0 on success, or a negative error code in
 * case of failure.
 */
int zbm_decompress(void *dest, size_t dest_size, const void *src, size_t src_size, size_t *out_len);

#endif /* _LIBZBITMAP_H */
