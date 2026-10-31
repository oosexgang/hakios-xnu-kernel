#!/bin/bash
# Make the case files tools/air/glasscase draws (on the phone: userland/rc.launchd26.glasscase), one for every
# render pipeline of QuartzCore's iOS 26 library glassref knows how to feed, both precisions, each drawn by
# Apple's GPU here and checked once against llvmpipe (GALLIUM_DRIVER=llvmpipe, the build-lp library) as it is
# written. The summary of that check is DIR/host.txt.
#
#   tools/air/glasscases.sh QC_AIR.metallib PAIRS.txt DIR [EXTRA.metallib VERTEX FRAGMENT ...]
#
# QC_AIR.metallib: the AIR slice of /System/Library/Frameworks/QuartzCore.framework/default.metallib
# (tools/metallib.py). PAIRS.txt: "FRAGMENT VERTEX" a line, the _lpf names; the _lph twins are added.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$1 PAIRS=$2 DIR=$3
shift 3
GL=$ROOT/xnu-deps/build/glassref/glassref-lp
mkdir -p "$DIR"
rm -f "$DIR"/*.case "$DIR/host.txt"
export GALLIUM_DRIVER=llvmpipe
# a target of halves, what CA's EDR layers draw into: a half4 output keeps its 16 bits on the phone
export GLASSREF_TARGET=rgba16f
# the shadow's samples inside the source: llvmpipe takes constexpr samplers' address modes as repeat
SHADOW="--set shadow_offset=0,0 --set displacement_mat=0,0,0,0 --set shadow_dist_offset=0"
# small: the cases travel in the boot volume, which goes with the kernel in the loader's 64 MB
SIZE=${SIZE:-32x32}
one() {         # LIB VERTEX FRAGMENT NAME
	local out
	out=$(timeout 120 "$GL" $SHADOW --size "$SIZE" --case "$DIR/$4.case" "$1" "$2" "$3" 2>&1 | head -3) || true
	echo "$4: ${out:0:220}" >> "$DIR/host.txt"
	[ -f "$DIR/$4.case" ] || echo "glasscases: no case for $4: ${out:0:160}"
}
while read -r frag vert; do
	[ -n "$frag" ] || continue
	# std_vert0 leaves texcoord0_0 unwritten: std_vert1 is the one that pairs with the standard fragments
	[ "$vert" = std_vert0_lpf ] && vert=std_vert1_lpf
	one "$LIB" "$vert" "$frag" "$frag"
	case $frag in *_lpf)
		one "$LIB" "${vert%_lpf}_lph" "${frag%_lpf}_lph" "${frag%_lpf}_lph" ;;
	esac
done < "$PAIRS"
while [ $# -ge 3 ]; do
	one "$1" "$2" "$3" "$3"
	shift 3
done
echo "glasscases: $(ls "$DIR"/*.case | wc -l | tr -d ' ') cases in $DIR ($(du -sh "$DIR" | cut -f1)); llvmpipe: \
$(grep -c ' 0 of ' "$DIR/host.txt") match, $(grep -c 'pixels differ' "$DIR/host.txt") drawn"
