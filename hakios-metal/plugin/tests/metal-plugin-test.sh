#!/bin/sh
# The Metal plug-in's objects on this Mac (plugin/tests/metal_plugin_test.m says what that does and does not show), under the
# address sanitizer, with every .metallib under DIR read as a library (an extracted system: hakios-xnu-kernel's sysimage/rootfs).
# macOS only.
# usage: plugin/tests/metal-plugin-test.sh [DIR]
#        GPU=1 plugin/tests/metal-plugin-test.sh [DIR]   the plug-in as built with hakios-metal (HAKIOS_METAL_GPU): this tree's
#                                                        core linked in, which finds no kernel GPU on a Mac
# HAKIOS= the hakios-xnu-kernel tree (default: beside this one), for its tools/metallib.py; without it every .metallib is read.
set -e
cd "$(dirname "$0")/../.."
[ "$(uname)" = Darwin ] || { echo "metal-plugin-test: needs macOS (its Metal stands in for Darwin's)"; exit 0; }
HAKIOS=${HAKIOS:-../hakios-xnu-kernel}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
gpu=
if [ -n "$GPU" ]; then
	# the core for this Mac, as `make test` builds it, under the same sanitizer
	for f in src/pm4.cpp src/gpu.cpp src/command.cpp src/compute.cpp src/texture.cpp src/render.cpp src/link.cpp src/compiler.cpp src/metallib.cpp src/c_api.cpp src/fake_gpu.cpp; do
		clang++ -std=c++17 -O1 -g -fsanitize=address -Wall -Wextra -Werror -Iinclude -c "$f" -o "$tmp/$(basename "$f" .cpp).o"
	done
	gpu="-DHAKIOS_METAL_GPU -Iinclude $tmp/*.o -lc++"
	# with the core's fake GPU (HakiosMetal.m's gpu()): command buffers and blits are tried too
	export HAKIOS_METAL_FAKE=1
fi
clang -O1 -g -fsanitize=address -Wall -Wno-unused-parameter -Iplugin -framework Foundation -framework Metal -lobjc \
	plugin/tests/metal_plugin_test.m plugin/HakiosMetal.m plugin/hakios_metallib.c $gpu -o "$tmp/metal_test"
files=
if [ -n "$1" ]; then
	# the MTLB ones (the GPU driver's own runtime libraries are another format: hakios-xnu-kernel's tools/metallib-test.sh)
	for f in $(find "$1" -name '*.metallib' -type f); do
		if [ ! -f "$HAKIOS/tools/metallib.py" ] || python3 "$HAKIOS/tools/metallib.py" list "$f" >/dev/null 2>&1; then
			files="$files $f"
		fi
	done
fi
"$tmp/metal_test" $files 2>&1 | grep -v '^.*HakiosMetal: ' || true
"$tmp/metal_test" $files >/dev/null 2>&1
