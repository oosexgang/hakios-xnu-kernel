#!/bin/bash
# Every fragment function of a .metallib made into a render pipeline on softpipe, as the Metal driver makes
# CoreAnimation's under QEMU, a process each (tools/air/softpipesweep.c): a function whose process dies -- a
# NIR validation failure or an opcode nir_to_tgsi does not know -- is named, because under QEMU it is the
# window server that dies with it. Default library: Darwin 27's QuartzCore.
#
#   tools/air/softpipesweep.sh [LIBRARY.metallib]      (needs tools/air/mk-host.sh)
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno
LIBRARY=${1:-/home/user/darwin27/rootfs/System/Library/Frameworks/QuartzCore.framework/default.metallib}
[ -f "$LIB/libhakios-air.so" ] || { echo "softpipesweep.sh: run tools/air/mk-host.sh first"; exit 1; }
[ -f "$LIBRARY" ] || { echo "softpipesweep.sh: no $LIBRARY"; exit 0; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cc -O1 -I"$ROOT/tools/mesa/air" -o "$WORK/softpipesweep" "$ROOT/tools/air/softpipesweep.c" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB"
python3 "$ROOT/tools/metallib.py" dump "$LIBRARY" "$WORK/bc" > /dev/null
ok=0 refused=0 died=0
while read -r name; do
	if line=$(RWA_SOFTWARE=1 timeout 120 "$WORK/softpipesweep" "$WORK/bc/$name.bc" "$name" 2> /dev/null); then
		case "$line" in
		ok*) ok=$((ok + 1)) ;;
		*) refused=$((refused + 1)) ;;
		esac
	else
		died=$((died + 1))
		echo "DIED	$name"
	fi
done < <(python3 "$ROOT/tools/metallib.py" list "$LIBRARY" | awk '$2 == "fragment" { print $1 }')
echo "softpipesweep.sh: $(basename "$LIBRARY"): $ok made, $refused refused, $died died"
[ "$died" = 0 ]
