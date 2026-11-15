#!/bin/bash
# An explicit imageblock in a kernel, on softpipe (tools/air/imageblockcheck.c, tools/air/imageblock.ll). Needs
# the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "imageblockcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/ib.bc" "$ROOT/tools/air/imageblock.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/imageblockcheck" "$ROOT/tools/air/imageblockcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/imageblockcheck" "$WORK/ib.bc"
