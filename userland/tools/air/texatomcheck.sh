#!/bin/bash
# Texture atomics on softpipe (tools/air/texatomcheck.c, tools/air/texture_atomic.ll). Needs the host build
# (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "texatomcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/ta.bc" "$ROOT/tools/air/texture_atomic.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/texatomcheck" "$ROOT/tools/air/texatomcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/texatomcheck" "$WORK/ta.bc"
