#!/bin/bash
# Textures out of a local array at an index a loop counts, on softpipe (tools/air/texarraycheck.c,
# tools/air/tex_array.ll, tools/air/tex_int.ll, tools/air/tex_sel.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "texarraycheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/ta.bc" "$ROOT/tools/air/tex_array.ll"
"$LLVM/llvm-as" -o "$WORK/ti.bc" "$ROOT/tools/air/tex_int.ll"
"$LLVM/llvm-as" -o "$WORK/ts.bc" "$ROOT/tools/air/tex_sel.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/texarraycheck" "$ROOT/tools/air/texarraycheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/texarraycheck" "$WORK/ta.bc"
RWA_SOFTWARE=1 "$WORK/texarraycheck" "$WORK/ti.bc" int
RWA_SOFTWARE=1 "$WORK/texarraycheck" "$WORK/ts.bc" sel
