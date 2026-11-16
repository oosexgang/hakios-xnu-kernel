#!/bin/bash
# A compute kernel's [[stage_in]] on softpipe, without a stage-in and with one registered
# (tools/air/stageincheck.c, tools/air/stage_in.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "stageincheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/si.bc" "$ROOT/tools/air/stage_in.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/stageincheck" "$ROOT/tools/air/stageincheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/stageincheck" "$WORK/si.bc"
RWA_SOFTWARE=1 "$WORK/stageincheck" "$WORK/si.bc" registered
