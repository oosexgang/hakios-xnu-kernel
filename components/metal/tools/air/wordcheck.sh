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
ptr_table.ll pt 8 0 0 0 0 0 0 0 0 0 1001 0 1003 0 1005 0 1007
ptr_loop.ll pl 8 0 0 0 0 0 0 0 0 1000 1001 1002 1003 1004 1005 1006 1007
ptr_return.ll pr 8 0 0 0 0 0 0 0 0 0 1001 0 1003 0 1005 0 1007
ptr_atomic.ll pa 8 4 0 0 0 0 0 0 0 2 1 2 1 2 1 2 1
ptr_int.ll pi 8 1002 1003 1004 1005 1006 1007 1008 1009
wait_notify.ll wn 8 2008 2010 2012 2014 2016 2018 2020 2022
imageblock_size.ll ibs 8 8 1
irem.ll ir 8 4294967295 0 4294967294 4294967295 0 1 2 0
const_table.ll ct 8 8.0 7.0 6.0 5.0 4.0 3.0 2.0 1.0 0.5 1.5 2.5 3.5 4.5 5.5 6.5 7.5 10.0 20.0 30.0 40.0 10.0 20.0 30.0 40.0
"
bad=0
while read -r ll entry threads words; do
	[ -n "$ll" ] || continue
	"$LLVM/llvm-as" -o "$WORK/$entry.bc" "$ROOT/tools/air/$ll"
	RWA_SOFTWARE=1 "$WORK/wordcheck" "$WORK/$entry.bc" "$entry" "$threads" $words || bad=1
done <<< "$CASES"
exit $bad
