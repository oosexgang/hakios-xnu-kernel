#!/bin/bash
# The Metal plug-in for iOS 26/27, arm64e, into $OBJ (darwin/userland/env.sh): $OBJ/HakiosMetal, the executable of
# HakiosMetal.bundle, which root26-darwin.sh puts into the small image as /hakios/lib/HakiosMetal.bundle and the
# system image links into /System/Library/Extensions (mkimage26.py). Metal loads it in every program that asks for
# a device once the kernel names it (boot-arg metal=1), CoreAnimation's renderer in backboardd first.
#
# It is components/metal/plugin/plugin/HakiosMetalCA.m, the plug-in that serves CoreAnimation (with plugin/ca_paths.c), over components/metal/plugin/'s driver built with its
# compiler (make ios MESA=1) and Mesa's compiler archives (components/metal/plugin/tools/mk-mesa.sh). Like the other libraries:
# pacstrip, then ldid; root26-darwin.sh signs it ad hoc with the rest.
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=env.sh
source "$RN/userland/env.sh"
M=$IOS_REPO/components/metal/plugin
export IOS_ARCH=arm64e
# (every time: it copies the front end in again and ninja rebuilds what changed, so an edit of components/metal/plugin/tools/mesa/air
# is in this plug-in)
mkdir -p "$OBJ"
"$M/tools/mk-mesa.sh" > "$OBJ/mk-mesa.log" 2>&1 || { tail -20 "$OBJ/mk-mesa.log" >&2; exit 1; }
make -s -C "$M" ios MESA=1 IOS_ARCH=$IOS_ARCH XNUDEPS="$XNUDEPS" -j"$(nproc)"
# the archives the driver's compiler links, as components/metal/plugin/tools/compiler.mk lists them (HAKIOS_METAL_IOS_MESA_LIBS)
MESA_LIBS=$(make -s -C "$M" -f /dev/stdin IOS_ARCH=$IOS_ARCH XNUDEPS="$XNUDEPS" print <<'MK'
include tools/compiler.mk
print:
	@echo $(HAKIOS_METAL_IOS_MESA_LIBS)
MK
)
# Metal on Vulkan (boot-arg metal_m2v=1) is not built here: the plug-in imports its five functions weakly and
# finds them NULL, so the linker is told they may stay undefined
M2V_LEFT=
for f in open first_problem status formats variant_hook; do
	M2V_LEFT="$M2V_LEFT -Wl,-U,_native_bridge_metal_$f"
done
# shellcheck disable=SC2086  # (DYN, MESA_LIBS and M2V_LEFT are lists of words)
$DYN -bundle -framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc \
	-I"$M/include" -I"$M/plugin" -o "$OBJ/HakiosMetal" "$M/plugin/HakiosMetalCA.m" "$M/plugin/ca_paths.c" "$M/out/ios/libhakios-metal.a" $MESA_LIBS -lc++ \
	$M2V_LEFT
pac_strip "$OBJ/HakiosMetal"
"$LDID" -S "$OBJ/HakiosMetal"
if [ "$(uname -s)" = Darwin ]; then SIZE=$(stat -f %z "$OBJ/HakiosMetal"); else SIZE=$(stat -c %s "$OBJ/HakiosMetal"); fi
echo "  HakiosMetal: $(file -b "$OBJ/HakiosMetal" | cut -c1-60), $SIZE bytes"
