#!/bin/bash
# The phone's shader compiler over a whole .metallib, on Linux: every function taken out (tools/metallib.py),
# then each translated, finalised and compiled by ir3 for the Adreno given (tools/air/ir3sweep.c, through the
# host build of libhakios-air: tools/air/mk-host.sh first). One line per function, then the counts.
#
#   tools/air/ir3sweep.sh LIBRARY.metallib [CHIP_ID]      (CHIP_ID default 0x06060010: the Adreno 642L)
#
# LINK="NAME=FUNCTION ..." links functions of the same library into every one, as a pipeline's
# MTLLinkedFunctions does: LINK=custom_fn=glassBackground_v1 compiles RenderBox's custom-effect shaders with
# its glass effect in them.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
[ -f "$LIB/libhakios-air.so" ] || { echo "ir3sweep.sh: run tools/air/mk-host.sh first"; exit 1; }
[ -f "${1:-}" ] || { echo "usage: ir3sweep.sh LIBRARY.metallib [CHIP_ID]"; exit 2; }
CHIP=${2:-0x06060010}
BIN=$ROOT/build/ir3sweep
if [ ! -x "$BIN" ] || [ "$ROOT/tools/air/ir3sweep.c" -nt "$BIN" ]; then
	cc -O1 -Wall -I"$ROOT/tools/mesa/air" -o "$BIN" "$ROOT/tools/air/ir3sweep.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
python3 "$ROOT/tools/metallib.py" dump "$1" "$WORK" > /dev/null
links=()
for l in ${LINK:-}; do
	[ -f "$WORK/${l#*=}.bc" ] || { echo "ir3sweep.sh: LINK: no function ${l#*=} in $(basename "$1")"; exit 2; }
	links+=(-l "${l%%=*}=$WORK/${l#*=}.bc=${l#*=}")
done
ok=0 failed=0 crashed=0
for f in "$WORK"/*.bc; do
	name=$(basename "$f" .bc)
	# one process each: an assertion in the compiler ends that function's line and not the sweep
	if line=$(timeout 300 "$BIN" "${links[@]}" "$CHIP" "$f=$name" 2> /dev/null); then
		ok=$((ok + 1))
	elif [ -n "$line" ]; then
		failed=$((failed + 1))
	else
		line="CRASH	$name"
		crashed=$((crashed + 1))
	fi
	echo "$line"
done
echo "ir3sweep.sh: $(basename "$1"), chip $CHIP${LINK:+, linking $LINK}: $ok compiled, $failed refused, $crashed crashed"
