#!/bin/bash
# Break a driver on purpose and see whether its test notices: one piece of text in a source file is
# replaced, in a copy, the test is built against the copy and every scenario is run. A change no
# scenario notices is a scenario to write, or a line that does nothing (tests/README.md, "Adding a test").
#
#   tests/mutate.sh TEST SOURCE OLD NEW
#
#   TEST     a test that tests/run.sh runs scenario by scenario: speaker_driver_test, loader_window_test, ...
#   SOURCE   the file to break, as the test includes it: redwood_speaker.c, src/window.c, example_led.c
#   OLD NEW  the text to replace and what to put there; OLD is replaced where it first occurs
#
# for example
#   tests/mutate.sh ddr_driver_test hakios_ddr.c 'level == 0 ||' ''
#
# Nothing of the repository is changed. Exit 0: a scenario failed (the change was caught); 1: none
# did; 2: the change does not compile, or the arguments are wrong.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
die() { echo "mutate.sh: $*" >&2; exit 2; }
[ $# -eq 4 ] || die "usage: tests/mutate.sh TEST SOURCE OLD NEW (the head of this file has an example)"
name=$1 source=$2 old=$3 new=$4
SRC=$HERE/../kernel/overlay/bsd/dev
CC=${CC:-cc}

test=$HERE/$name.c
[ -f "$test" ] || test=$HERE/../docs/examples/$name.c
[ -f "$test" ] || die "no test $name (tests/$name.c, docs/examples/$name.c)"
# where the test finds the file: beside itself, among the kernel's drivers, in the loader
original=
for directory in "$(dirname "$test")" "$SRC" "$SRC/.." "$HERE/../loader"; do
	if [ -f "$directory/$source" ]; then
		original=$directory/$source
		break
	fi
done
[ -n "$original" ] || die "$source is not found where $name looks for it"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
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
"$HERE/../tools/boards.py" --devices "$HERE/devices" --soc "$HERE/soc" gen --out "$W/boards" > /dev/null || die "the test boards' tables could not be made"

# (the flags of tests/run.sh, the copy's directory first)
flags=(-std=gnu11 -w -O2 -pthread -I"$W/copy" -I"$HERE" -I"$HERE/fakekernel" -I"$SRC" -I"$SRC/.." -I"$W/boards" -I"$HERE/../loader")
# the test has to take the copy, itself or through a file it includes: a change to a file it never sees is caught by nothing
"$CC" "${flags[@]}" -MM "$test" 2> /dev/null | grep -qF "$W/copy/$source" || die "$name does not include $source"
if ! "$CC" "${flags[@]}" "$HERE/fakekernel/fakekernel.c" "$test" -o "$W/test" 2> "$W/errors"; then
	head -5 "$W/errors" >&2
	die "the change does not compile"
fi
caught=
list=$("$W/test")
[ -n "$list" ] || die "$name lists no scenario: nothing would be run"
for scenario in $list; do
	timeout 120 "$W/test" "$scenario" > /dev/null 2>&1 || caught="$caught $scenario"
done
if [ -n "$caught" ]; then
	echo "caught by:$caught"
	exit 0
fi
echo "MISSED: every scenario of $name passes with the change"
exit 1
