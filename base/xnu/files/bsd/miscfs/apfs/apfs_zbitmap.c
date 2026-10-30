/*
 * LZBITMAP, Apple's method for the files of the iOS 16 cryptexes (decmpfs types 13 and 14:
 * the OS cryptex of the IPSW keeps its dyld shared cache this way), so that a cryptex image
 * can be mounted as Apple ships it.  The same file is built into the kernel and into
 * tools/apfs/apfs-tool.
 *
 * The decoder is libzbitmap (https://github.com/eafer/libzbitmap) as shipped with the
 * linux-apfs-rw driver, Copyright (C) 2022 Corellium LLC, Ernesto A. Fernandez, under
 * "GPL-2.0+ OR MIT" -- taken here under the MIT licence; only its includes are changed
 * (libzbitmap/).
 */
#include "libzbitmap/libzbitmap.c"
