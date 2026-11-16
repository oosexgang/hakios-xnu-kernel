#!/bin/bash
# One texture written at two mip levels by one kernel, on softpipe (tools/air/mipwritecheck.c,
# tools/air/mip_write.ll, tools/air/mip_buffer.ll with the levels out of a buffer, and
# tools/air/mip_read.ll reading at such a level, tools/air/mip_float.ll at a float plus one). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "mipwritecheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/mw.bc" "$ROOT/tools/air/mip_write.ll"
"$LLVM/llvm-as" -o "$WORK/mb.bc" "$ROOT/tools/air/mip_buffer.ll"
"$LLVM/llvm-as" -o "$WORK/mr.bc" "$ROOT/tools/air/mip_read.ll"
"$LLVM/llvm-as" -o "$WORK/mf.bc" "$ROOT/tools/air/mip_float.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/mipwritecheck" "$ROOT/tools/air/mipwritecheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/mipwritecheck" "$WORK/mw.bc"
RWA_SOFTWARE=1 "$WORK/mipwritecheck" "$WORK/mb.bc" buffer
RWA_SOFTWARE=1 "$WORK/mipwritecheck" "$WORK/mr.bc" read
RWA_SOFTWARE=1 "$WORK/mipwritecheck" "$WORK/mf.bc" float
