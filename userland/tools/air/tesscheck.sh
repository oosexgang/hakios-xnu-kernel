#!/bin/bash
# Two quad patches drawn as a post-tessellation vertex function's grid, on softpipe (tools/air/tesscheck.c,
# tools/air/tess_quad.ll, tools/air/tess_tri.ll, tools/air/tess_frag.ll). Needs the host build
# (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "tesscheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/tv.bc" "$ROOT/tools/air/tess_quad.ll"
"$LLVM/llvm-as" -o "$WORK/tf.bc" "$ROOT/tools/air/tess_frag.ll"
"$LLVM/llvm-as" -o "$WORK/tt.bc" "$ROOT/tools/air/tess_tri.ll"
"$LLVM/llvm-as" -o "$WORK/tc.bc" "$ROOT/tools/air/tess_cp.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/tesscheck" "$ROOT/tools/air/tesscheck.c" -L"$LIB" -lhakios-air -lm -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/tesscheck" "$WORK/tv.bc" "$WORK/tf.bc"
RWA_SOFTWARE=1 "$WORK/tesscheck" "$WORK/tt.bc" "$WORK/tf.bc" tri
RWA_SOFTWARE=1 "$WORK/tesscheck" "$WORK/tc.bc" "$WORK/tf.bc" cp
