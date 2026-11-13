#!/bin/bash
# clang's static analyzer on the user space this repository has for Darwin: the libraries put into
# Darwin's processes and the programs of the small image (userland/userland/src, darwin/userland), against
# the SDK the builds use. What it finds is listed, file by file, for a person to judge -- the kernel's
# side is kernel/syntax-check.sh --analyze.
#
#   tools/analyze-userland.sh [--why] [FILE...]
#
#   --why       each finding with the path the analyzer took to it
#   FILE...     files of userland/userland/src or darwin/userland (all of their .c and .m if none)
#
# DARWIN_SDK: the SDK (default: $THEOS/sdks/iPhoneOS16.5.sdk). Needs clang; nothing is built or linked.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${DARWIN_SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}
[ -d "$SDK" ] || { echo "analyze-userland: no SDK at $SDK (DARWIN_SDK=DIR)" >&2; exit 1; }
why=0
if [ "${1:-}" = --why ]; then
	why=1
	shift
fi
if [ $# -eq 0 ]; then
	mapfile -t files < <(cd "$HERE" && ls userland/userland/src/*.c userland/userland/src/*.m darwin/userland/*.c 2> /dev/null)
else
	files=("$@")
fi
found=0
for f in "${files[@]}"; do
	# (the same target as the builds; Objective-C without ARC, as the libraries are built)
	out=$(cd "$HERE/$(dirname "$f")" && clang -target arm64-apple-ios16.5 -isysroot "$SDK" -I. -I"$HERE/build/boards" \
		-I"$HERE/userland/userland/src" --analyze -Xanalyzer -analyzer-output=text -w "$(basename "$f")" -o /dev/null 2>&1)
	name=$(basename "$f")
	findings=$(grep -E "^$name:[0-9]+:[0-9]+: warning" <<< "$out" | sort -u)
	[ -n "$findings" ] || continue
	found=$((found + $(wc -l <<< "$findings")))
	if [ "$why" = 1 ]; then
		grep -E "^$name:[0-9]+:[0-9]+: (warning|note)" <<< "$out" | sed "s|^|$(dirname "$f")/|"
	else
		while read -r line; do echo "$(dirname "$f")/$line"; done <<< "$findings"
	fi
done
echo "analyze-userland: $found findings in ${#files[@]} files (each for a person to judge)"
