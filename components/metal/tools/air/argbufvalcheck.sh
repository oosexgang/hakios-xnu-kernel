#!/bin/bash
# Values in argument buffers on softpipe: constants, an array, an inline struct, an atomic
# (tools/air/argbufvalcheck.c, tools/air/argbuf_values.ll, tools/air/argbuf_atomic.ll,
# tools/air/argbuf_halfpad.ll). Needs the host
# build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "argbufvalcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/av.bc" "$ROOT/tools/air/argbuf_values.ll"
"$LLVM/llvm-as" -o "$WORK/aa.bc" "$ROOT/tools/air/argbuf_atomic.ll"
"$LLVM/llvm-as" -o "$WORK/ap.bc" "$ROOT/tools/air/argbuf_halfpad.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/argbufvalcheck" "$ROOT/tools/air/argbufvalcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/argbufvalcheck" "$WORK/av.bc" av
RWA_SOFTWARE=1 "$WORK/argbufvalcheck" "$WORK/aa.bc" aa
RWA_SOFTWARE=1 "$WORK/argbufvalcheck" "$WORK/ap.bc" ap
