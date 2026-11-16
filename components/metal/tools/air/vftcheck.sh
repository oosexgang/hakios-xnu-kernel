#!/bin/bash
# Functions called out of a visible function table, on softpipe (tools/air/vftcheck.c, tools/air/vft.ll,
# tools/air/vft_add.ll, tools/air/vft_mul.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "vftcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
for m in vft vft_add vft_mul; do "$LLVM/llvm-as" -o "$WORK/$m.bc" "$ROOT/tools/air/$m.ll"; done
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/vftcheck" "$ROOT/tools/air/vftcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/vftcheck" "$WORK/vft.bc" "$WORK/vft_add.bc" "$WORK/vft_mul.bc"
