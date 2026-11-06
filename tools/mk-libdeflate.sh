#!/bin/bash
# Put the decompressing half of libdeflate into the kernel tree, as bsd/miscfs/apfs/libdeflate/:
# the APFS driver inflates the zlib pieces of compressed files with it instead of libkern's zlib.
#
# Why: on the restore-ramdisk volume -- 19149 pieces, 180 MB in, 1229 MB out -- libkern's zlib
# inflates at 344 MB/s on the build machine and libdeflate at 740 MB/s, to the same bytes; and
# on the phone zlib was 3819 of the 4227 ms one reader spent in vnop_read over that volume.
#
# Taken as it is from the release, files unchanged; what the kernel lacks (an allocator, no C
# library) is in bsd/miscfs/apfs/apfs_deflate.c, which compiles these with -DFREESTANDING.
# libdeflate is Copyright 2016 Eric Biggers and 2024 Google LLC, under the MIT licence (COPYING,
# copied alongside).
set -e
VERSION=1.24
SHA256=a0dda1c4b804742066db07b9510876edd09cc0ca06cdc32c5dfe1b2016a26463
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
OUT=${XNU_SRC:-$ROOT/xnu-src/xnu-8796.121.2}/bsd/miscfs/apfs/libdeflate
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -sL --fail -o "$TMP/l.tar.gz" \
	https://github.com/ebiggers/libdeflate/releases/download/v$VERSION/libdeflate-$VERSION.tar.gz
echo "$SHA256  $TMP/l.tar.gz" | sha256sum -c --quiet
tar xzf "$TMP/l.tar.gz" -C "$TMP"
S=$TMP/libdeflate-$VERSION
rm -rf "$OUT"
mkdir -p "$OUT/lib/arm"
cp "$S/COPYING" "$S/libdeflate.h" "$S/common_defs.h" "$OUT/"
for f in lib_common.h cpu_features_common.h deflate_constants.h zlib_constants.h \
	decompress_template.h deflate_decompress.c zlib_decompress.c adler32.c; do
	cp "$S/lib/$f" "$OUT/lib/"
done
# the architecture dispatch is included unconditionally; in the kernel it settles on NEON for
# the Adler-32 and the portable code for the rest (see apfs_deflate.c)
cp "$S/lib/arm/"*.h "$OUT/lib/arm/"
echo "mk-libdeflate.sh: $OUT ($(du -sk "$OUT" | cut -f1) KB, libdeflate $VERSION)"
