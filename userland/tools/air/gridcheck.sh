#!/bin/bash
# A dispatch of whole groups and one whose last group is partial (Metal's -dispatchThreads:), on softpipe
# (tools/air/gridcheck.c, tools/air/grid_partial.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "gridcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/grid.bc" "$ROOT/tools/air/grid_partial.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/gridcheck" "$ROOT/tools/air/gridcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/gridcheck" "$WORK/grid.bc"
