#!/bin/bash
# Small kernels whose first words of buffer 0 say whether they ran right, on softpipe
# (tools/air/wordcheck.c). Each line of CASES is a .ll of tools/air, its entry, the threads
# and the words wanted. Needs the host build (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -f "$LIB/libhakios-air.so" ] || { echo "wordcheck.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/wordcheck" "$ROOT/tools/air/wordcheck.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
CASES="
callee_array.ll ca 1 1.0 2.0 3.0 4.0 10.0
"
bad=0
while read -r ll entry threads words; do
	[ -n "$ll" ] || continue
	"$LLVM/llvm-as" -o "$WORK/$entry.bc" "$ROOT/tools/air/$ll"
	RWA_SOFTWARE=1 "$WORK/wordcheck" "$WORK/$entry.bc" "$entry" "$threads" $words || bad=1
done <<< "$CASES"
exit $bad
