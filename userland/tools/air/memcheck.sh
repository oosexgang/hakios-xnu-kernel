#!/bin/bash
# The phone's path over a whole .metallib under valgrind's memcheck: every function translated, finalised
# and compiled by ir3 (tools/air/ir3sweep.c) in a process of its own, and a line for each that valgrind
# found an error in, with its report kept. A read past a buffer here is a crash of backboardd or
# SpringBoard on the phone, where nothing reports it.
#
#   tools/air/memcheck.sh LIBRARY.metallib [CHIP_ID] [JOBS]      (needs tools/air/mk-host.sh, valgrind)
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=$ROOT/build/ir3sweep
[ -f "${1:-}" ] || { echo "usage: memcheck.sh LIBRARY.metallib [CHIP_ID] [JOBS]"; exit 2; }
command -v valgrind > /dev/null || { echo "memcheck.sh: no valgrind"; exit 1; }
CHIP=${2:-0x06060010} JOBS=${3:-4}
# (ir3sweep.sh builds the program; one function of nothing is enough to make it do that)
[ -x "$BIN" ] || "$ROOT/tools/air/ir3sweep.sh" "$1" "$CHIP" > /dev/null || true
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
python3 "$ROOT/tools/metallib.py" dump "$1" "$WORK" > /dev/null
export BIN CHIP
# shellcheck disable=SC2016
find "$WORK" -name '*.bc' -print0 | xargs -0 -P "$JOBS" -I{} sh -c '
	n=$(basename "$1" .bc)
	if valgrind -q --error-exitcode=99 "$BIN" "$CHIP" "$1=$n" > /dev/null 2> "$1.vg"; [ $? = 99 ]; then
		echo "ERRORS	$n"; sed "s/^/	/" "$1.vg" | head -40
	else
		echo "clean	$n"
	fi' _ {} > "$WORK/report"
grep -v "^clean" "$WORK/report" || true
echo "memcheck.sh: $(basename "$1"): $(grep -c "^clean" "$WORK/report") clean, $(grep -c "^ERRORS" "$WORK/report") with errors"
