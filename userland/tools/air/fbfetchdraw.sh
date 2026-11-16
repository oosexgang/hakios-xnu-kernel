#!/bin/bash
# A framebuffer fetch through the library's own draw path on softpipe (tools/air/fbfetchdraw.c,
# tools/air/fbfetch.ll). Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "fbfetchdraw.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/fbfetch.bc" "$ROOT/tools/air/fbfetch.ll"
# the library takes a module's first entry point: fbFetchHalf first, for the second case
sed 's/^!air.fragment = !{!3, !8}/!air.fragment = !{!8, !3}/' "$ROOT/tools/air/fbfetch.ll" > "$WORK/fbfetch_half.ll"
"$LLVM/llvm-as" -o "$WORK/fbfetch_half.bc" "$WORK/fbfetch_half.ll"
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/fbfetchdraw" "$ROOT/tools/air/fbfetchdraw.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
RWA_SOFTWARE=1 "$WORK/fbfetchdraw" "$WORK/fbfetch.bc"
RWA_SOFTWARE=1 "$WORK/fbfetchdraw" "$WORK/fbfetch_half.bc" half
