#!/bin/bash
# Break a driver on purpose and see whether its test notices: one piece of text in a source file is
# replaced, in a copy, the test is built against the copy and every scenario is run. A change no
# scenario notices is a scenario to write, or a line that does nothing (workbench/README.md, "Adding a test").
#
#   workbench/mutate.sh TEST SOURCE OLD NEW
#
#   TEST     a test that workbench/run.sh runs scenario by scenario: speaker_driver_test, loader_window_test, ...
#   SOURCE   the file to break, as the test includes it: board/speaker.c, src/window.c, example_led.c
#   OLD NEW  the text to replace and what to put there; OLD is replaced where it first occurs
#
# for example
#   workbench/mutate.sh ddr_driver_test board/ddr.c 'level == 0 ||' ''
#
# Nothing of the repository is changed. Exit 0: a scenario failed (the change was caught); 1: none
# did; 2: the change does not compile, or the arguments are wrong.
#
# A scenario has MUTATE_SECONDS (default 120) to end: a change that leaves one waiting forever is caught when
# they run out, so a smaller number makes a run over many changes quicker (mutants.py: MUTATE_SECONDS=20).
#
# OLD and NEW the same is no change: the test as it is, and what each scenario that fails says (exit 0:
# all of them pass; 1: one fails). That is the first thing to run on a test being written.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
die() { echo "mutate.sh: $*" >&2; exit 2; }
[ $# -eq 4 ] || die "usage: workbench/mutate.sh TEST SOURCE OLD NEW (the head of this file has an example)"
name=$1 source=$2 old=$3 new=$4
CC=${CC:-cc}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
# the kernel's files of ours at their paths in its tree, as workbench/run.sh builds them (tools/kernel-view.sh)
"$HERE/../tools/kernel-view.sh" "$W/kernel"
SRC=$W/kernel/bsd/dev

test=$HERE/$name.c
[ -f "$test" ] || test=$HERE/../Documentation/examples/$name.c
[ -f "$test" ] || die "no test $name (workbench/$name.c, Documentation/examples/$name.c)"
# where the test finds the file: beside itself, among the kernel's drivers, in the loader
original=
for directory in "$(dirname "$test")" "$SRC" "$SRC/.." "$HERE/../arch/arm64/boot"; do
	if [ -f "$directory/$source" ]; then
		original=$directory/$source
		break
	fi
done
[ -n "$original" ] || die "$source is not found where $name looks for it"

mkdir -p "$W/copy/$(dirname "$source")"
python3 - "$original" "$W/copy/$source" "$old" "$new" <<'PYTHON' || die "\"$old\" is not in $source"
import sys
text = open(sys.argv[1]).read()
if sys.argv[3] not in text:
    sys.exit(1)
open(sys.argv[2], 'w').write(text.replace(sys.argv[3], sys.argv[4], 1))
PYTHON
# a file beside its test is found there before anywhere else: the test is built from a copy beside the copy
if [ "$(dirname "$original")" = "$(dirname "$test")" ]; then
	cp "$test" "$W/copy/"
	test=$W/copy/$name.c
fi
"$HERE/../tools/gen_headers.py" --devices "$HERE/devices" --out "$W/boards" hakios > /dev/null || die "the test boards' tables could not be made"
(source "$HERE/../kernel/lib.sh" && focaltech_stub > "$W/boards/focaltech_fw.h") || die "no stub for the touch firmware"

# (the flags of workbench/run.sh, the copy's directory first; under the sanitizers as there, where the
# compiler has them: a change that reads or writes past a buffer is caught by them)
flags=(-std=gnu11 -w -O2 -pthread -I"$W/copy" -iquote "$SRC/board" -I"$HERE" -I"$HERE/fakekernel" -I"$SRC" -I"$SRC/.." -I"$W/boards" -I"$HERE/../arch/arm64/boot")
if echo 'int main(void) { return 0; }' | "$CC" -fsanitize=address,undefined -x c - -o "$W/sanitizers" 2> /dev/null; then
	flags+=(-O1 '-fsanitize=address,undefined' -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
	export ASAN_OPTIONS=detect_leaks=0
fi
# the test has to take the copy, itself or through a file it includes: a change to a file it never sees is caught by nothing
"$CC" "${flags[@]}" -MM "$test" 2> /dev/null | grep -qF "$W/copy/$source" || die "$name does not include $source"
if ! "$CC" "${flags[@]}" "$HERE/fakekernel/fakekernel.c" "$test" -o "$W/test" 2> "$W/errors"; then
	head -5 "$W/errors" >&2
	die "the change does not compile"
fi
caught=
list=$("$W/test")
[ -n "$list" ] || die "$name lists no scenario: nothing would be run"
if [ "$old" = "$new" ]; then
	for scenario in $list; do
		timeout "${MUTATE_SECONDS:-120}" "$W/test" "$scenario" > /dev/null 2> "$W/said" || { caught="$caught $scenario"; cat "$W/said"; }
	done
	[ -z "$caught" ] || { echo "failed as it is:$caught"; exit 1; }
	echo "every scenario of $name passes"
	exit 0
fi
for scenario in $list; do
	timeout "${MUTATE_SECONDS:-120}" "$W/test" "$scenario" > /dev/null 2>&1 || caught="$caught $scenario"
done
if [ -n "$caught" ]; then
	echo "caught by:$caught"
	exit 0
fi
echo "MISSED: every scenario of $name passes with the change"
exit 1
