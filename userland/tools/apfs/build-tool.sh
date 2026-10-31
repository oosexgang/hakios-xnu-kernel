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
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-$ROOT/apfs-work/apfs-tool}

SRC=""
for d in "$ROOT/xnu-src/xnu-8796.121.2/bsd/miscfs/apfs" "$ROOT/xnu/files/bsd/miscfs/apfs"; do
	[ -f "$d/apfs_lib.c" ] && { SRC=$d; break; }
done
[ -n "$SRC" ] || { echo "build-tool.sh: apfs_lib.c is in neither the kernel tree nor xnu/files" >&2; exit 1; }

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
