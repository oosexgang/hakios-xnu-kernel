#!/bin/sh
# The Metal plug-in's objects on this Mac (tools/metal_plugin_test.m says what that does and does not show), under the address
# sanitizer, with every .metallib under DIR read as a library (an extracted system: sysimage/rootfs). macOS only.
# usage: tools/metal-plugin-test.sh [DIR]
set -e
cd "$(dirname "$0")/.."
[ "$(uname)" = Darwin ] || { echo "metal-plugin-test: needs macOS (its Metal stands in for iOS's)"; exit 0; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
clang -O1 -g -fsanitize=address -Wall -Wno-unused-parameter -Iuserland/src -framework Foundation -framework Metal -lobjc \
	tools/metal_plugin_test.m userland/src/HakiosMetal.m userland/src/hakios_metallib.c -o "$tmp/metal_test"
files=
if [ -n "$1" ]; then
	# the MTLB ones (the GPU driver's own runtime libraries are another format: tools/metallib-test.sh)
	for f in $(find "$1" -name '*.metallib' -type f); do
		if python3 tools/metallib.py list "$f" >/dev/null 2>&1; then files="$files $f"; fi
	done
fi
"$tmp/metal_test" $files 2>&1 | grep -v '^.*HakiosMetal: ' || true
"$tmp/metal_test" $files >/dev/null 2>&1
