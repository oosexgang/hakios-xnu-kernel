#!/bin/bash
# A Metal encoder's raster state through the library on softpipe: cull modes, windings, lines (tools/air/rastcheck.c,
# tools/air/rast_tri.ll, tools/air/rast_frag.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "rastcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/rv.bc" "$ROOT/tools/air/rast_tri.ll"
"$LLVM/llvm-as" -o "$WORK/rf.bc" "$ROOT/tools/air/rast_frag.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/rastcheck" "$ROOT/tools/air/rastcheck.c" -L"$LIB" -lhakios-air -lm -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/rastcheck" "$WORK/rv.bc" "$WORK/rf.bc"
