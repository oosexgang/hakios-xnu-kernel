#!/bin/bash
# Pointer compares on softpipe: two buffer arguments, and a walk to an end pointer
# (tools/air/bufcmpcheck.c, tools/air/buffer_compare.ll, tools/air/pointer_walk.ll).
# Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "bufcmpcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/bc.bc" "$ROOT/tools/air/buffer_compare.ll"
"$LLVM/llvm-as" -o "$WORK/walk.bc" "$ROOT/tools/air/pointer_walk.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/bufcmpcheck" "$ROOT/tools/air/bufcmpcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/bufcmpcheck" "$WORK/bc.bc" "$WORK/walk.bc"
