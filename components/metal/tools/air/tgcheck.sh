#!/bin/bash
# A kernel with threadgroup memory of its own and a threadgroup buffer sized at dispatch, and one with two
# such buffers, on softpipe (tools/air/tgcheck.c, tools/air/threadgroup_both.ll, tools/air/threadgroup_two.ll).
# Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "tgcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/tg.bc" "$ROOT/tools/air/threadgroup_both.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/tgcheck" "$ROOT/tools/air/tgcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
"$LLVM/llvm-as" -o "$WORK/tg2.bc" "$ROOT/tools/air/threadgroup_two.ll"
RWA_SOFTWARE=1 "$WORK/tgcheck" "$WORK/tg.bc"
RWA_SOFTWARE=1 "$WORK/tgcheck" "$WORK/tg2.bc" two
