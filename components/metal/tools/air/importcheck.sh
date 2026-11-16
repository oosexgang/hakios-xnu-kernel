#!/bin/bash
# Values a callee takes from its caller beyond its arguments, on softpipe (tools/air/importcheck.c,
# tools/air/callee_import.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "importcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/imp.bc" "$ROOT/tools/air/callee_import.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/importcheck" "$ROOT/tools/air/importcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/importcheck" "$WORK/imp.bc"
