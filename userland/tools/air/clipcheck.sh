#!/bin/bash
# Two clip distances a vertex shader writes, through the library on softpipe (tools/air/clipcheck.c,
# tools/air/clip_tri.ll, tools/air/rast_frag.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "clipcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/rv.bc" "$ROOT/tools/air/clip_tri.ll"
"$LLVM/llvm-as" -o "$WORK/r6.bc" "$ROOT/tools/air/clip_six.ll"
"$LLVM/llvm-as" -o "$WORK/rf.bc" "$ROOT/tools/air/rast_frag.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/clipcheck" "$ROOT/tools/air/clipcheck.c" -L"$LIB" -lhakios-air -lm -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/clipcheck" "$WORK/rv.bc" "$WORK/rf.bc"
RWA_SOFTWARE=1 "$WORK/clipcheck" "$WORK/r6.bc" "$WORK/rf.bc"
