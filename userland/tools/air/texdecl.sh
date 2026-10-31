#!/bin/bash
# What every function of the corpus declares it wants bound in each texture slot.
#
# This is the host half of the draw sweep in userland/src/metal_drv_test.m. That
# sweep used to bind one texture kind and one pixel format for a whole library,
# so 13 of the 271 fragment shaders of AppleMetalGLRenderer were refused by name
# -- they declare a texture3d, a texture2d<int> or a multisample texture -- which
# measured the harness and said nothing about the shaders. The compiler library
# now answers what a module declares (rwa_module_textures, tools/mesa/air/
# air_metal.h) and the sweep binds that per shader; this prints the same answer
# for the whole corpus, so the expected binding of all 271 can be read without a
# boot and a device log can be checked against it afterwards.
#
# The numbers come from the library and not from a second reading of Apple's
# metadata, which is the only way a row here and a refusal on the phone can be
# compared at all: both are the same two helpers in air_metal.c.
#
# Each row is
#
#   textures NAME  slot N  MTLTextureType...  CLASSBITS  usage U  air.arg_type_name
#
# where usage is MTLTextureUsage (1 ShaderRead, 2 ShaderWrite) and the last
# column is the declaration in the words Apple's compiler wrote it in.
#
# usage: texdecl.sh [FILE.metallib ...]
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
IOS=${IOS:-$ROOT/ios165}
TEST=${AIR_METAL_TEST:-$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno/air_metal_test}
[ -x "$TEST" ] || { echo "texdecl.sh: run tools/air/mk-host.sh first"; exit 1; }

LIBS=("$@")
if [ ${#LIBS[@]} = 0 ]; then
	# The five of docs/air.md section 5, named rather than searched for, so a
	# missing one is an error and not a smaller corpus. SafariSharedUI's is
	# 118 bytes and holds no functions at all, which is why it is last and
	# why its line below says so rather than failing the run.
	LIBS=(
		"$IOS/rootfs/System/Library/VideoDecoders/AV1SW.metallib"
		"$IOS/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib"
		"$IOS/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib"
		"$IOS/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/portrait_filters.metallib"
		"$IOS/cryptex/System/Library/PrivateFrameworks/SafariSharedUI.framework/default.metallib"
	)
	for f in "${LIBS[@]}"; do
		[ -f "$f" ] || { echo "texdecl.sh: missing $f"; exit 1; }
	done
fi

rc=0
for lib in "${LIBS[@]}"; do
	echo "== $(basename "$(dirname "$lib")")/$(basename "$lib")"
	# Functions that declare nothing are the great majority (256 of the 271
	# fragment shaders), and a page of "none" hides the fifteen rows that
	# matter; the per-library summary still counts them all.
	out=$("$TEST" --textures "$lib" 2>&1)
	status=$?
	printf '%s\n' "$out" | grep -v ' none$'
	if [ "$status" != 0 ]; then
		# An empty container has no function list to walk. That is a fact
		# about the file (118 bytes) and not a failure of this tool, so it
		# is said and not counted against the run.
		case "$out" in
		*"the function list runs to 0"*)
			echo "texdecl: (no functions in this container at all)" ;;
		*)
			rc=1 ;;
		esac
	fi
	echo
done
exit $rc
