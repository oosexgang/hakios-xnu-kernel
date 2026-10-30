/*
 * libdeflate's decompressor, for the zlib pieces of compressed files.  The same file is
 * built into the kernel and into tools/apfs/apfs-tool, so the two run one decoder.
 *
 * The sources are bsd/miscfs/apfs/libdeflate as tools/mk-libdeflate.sh takes them from the
 * release, unchanged.  libdeflate is Copyright 2016 Eric Biggers and 2024 Google LLC, under
 * the MIT licence (libdeflate/COPYING).
 *
 * Built FREESTANDING: no C library, memcpy and friends become compiler built-ins, and the
 * CPU's features are only what the compiler is told -- here, NEON and nothing past it, so
 * the Adler-32 over each piece's output is libdeflate's NEON one (the decompressor itself
 * has no arm code of its own).  The kernel saves the vector registers on every exception
 * and its own lz4 decoder uses them, so that is safe here.  <arm_neon.h> comes from the
 * compiler (bsd/conf/Makefile.arm64).  What lib/utils.c would add is two pointers, the
 * allocator, given here instead; its fallback memset/memcpy are not wanted next to the
 * kernel's own.
 */
#define FREESTANDING 1

#include "libdeflate/lib/deflate_decompress.c"
#include "libdeflate/lib/zlib_decompress.c"
#include "libdeflate/lib/adler32.c"

/* in apfs_lib.c, which knows how this side allocates */
extern void *apfs_deflate_malloc(size_t size);
extern void apfs_deflate_free(void *ptr);

malloc_func_t libdeflate_default_malloc_func = apfs_deflate_malloc;
free_func_t libdeflate_default_free_func = apfs_deflate_free;
