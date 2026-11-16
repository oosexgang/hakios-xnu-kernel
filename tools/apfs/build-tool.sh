#!/bin/bash
# Build the kernel's APFS reading code as a program for this machine, so it can be run
# against a real image before it goes anywhere near the kernel.  The .c files are taken from
# the kernel tree unchanged; only apfs_lib.h's few allocation and logging macros differ
# when KERNEL is not defined.  apfs_deflate.c is libdeflate's decompressor; host-shim/ stands
# in for its x86 code, which is not in the kernel tree, so the host runs the kernel's decoder.
#
# usage: tools/apfs/build-tool.sh [OUT]       (default: apfs-work/apfs-tool)
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../userland" && pwd)   # the first generation's workspace: apfs-work/, xnu-deps/, darwin165/
OUT=${1:-$ROOT/apfs-work/apfs-tool}

# shellcheck source=tools/apfs/apfs-src.sh
. "$HERE/apfs-src.sh"
SRC=$(apfs_src_find "$ROOT")

mkdir -p "$(dirname "$OUT")"
# TOOL_CFLAGS: more for the compiler, e.g. "-fsanitize=thread -g" with APFS_TOOL_THREADS
TOOL_CFLAGS=${TOOL_CFLAGS:-}
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT
# (libdeflate as released compares signed with unsigned in a few places; -Wextra says so)
cc -std=c11 -O2 -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter $TOOL_CFLAGS -I"$HERE/host-shim" -c -o "$OBJ/apfs_deflate.o" \
	"$SRC/apfs_deflate.c"
# Names that are not plain ASCII are hashed and compared after Unicode decomposition (and folding):
# with the kernel's own code for that, bsd/vfs/vfs_unicode.c, when the kernel tree is here.
UNI=""
XNUBSD=$(cd "$SRC/../.." && pwd)
if [ -f "$XNUBSD/vfs/vfs_unicode.c" ] && [ -f "$XNUBSD/sys/unicode.h" ]; then
	mkdir -p "$OBJ/inc/sys" "$OBJ/inc/libkern"
	cp "$XNUBSD/sys/unicode.h" "$OBJ/inc/sys/unicode.h"
	printf '#include <stdbool.h>\n#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n' > "$OBJ/inc/libkern/libkern.h"
	cc -std=c11 -D_DEFAULT_SOURCE -DKERNEL_PRIVATE -O2 -w $TOOL_CFLAGS -I"$OBJ/inc" -I"$XNUBSD/vfs" -c -o "$OBJ/vfs_unicode.o" \
		"$XNUBSD/vfs/vfs_unicode.c"
	UNI="-DAPFS_HOST_UNICODE -DKERNEL_PRIVATE -I$OBJ/inc $OBJ/vfs_unicode.o"
else
	echo "build-tool.sh: no bsd/vfs/vfs_unicode.c: names other than plain ASCII are found by reading the directory" >&2
fi
cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -I"$SRC" -I"$HERE" \
	$TOOL_CFLAGS -o "$OUT" "$HERE/apfs-tool.c" "$SRC/apfs_lib.c" "$SRC/apfs_write.c" "$SRC/apfs_store.c" "$SRC/apfs_zbitmap.c" \
	"$OBJ/apfs_deflate.o" $UNI -lz -lcrypto -pthread
echo "build-tool.sh: $OUT (from $SRC)"
