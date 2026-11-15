#!/bin/bash
# Every fragment, vertex and kernel function of a .metallib made into a pipeline on softpipe, as the Metal
# driver makes CoreAnimation's under QEMU, a process each (tools/air/softpipesweep.c; a vertex function with
# tools/air/rast_frag.ll for its fragment stage): a function whose process dies -- a NIR validation failure
# or an opcode nir_to_tgsi does not know -- is named, because under QEMU it is the window server that dies
# with it. Default library: Darwin 27's QuartzCore.
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
"${LLVM:-/usr/lib/llvm-20/bin}/llvm-as" -o "$WORK/rf.bc" "$ROOT/tools/air/rast_frag.ll"
ok=0 refused=0 died=0
while read -r name stage; do
	# (the file is named as metallib.py dump names it: '/' made '_', at most 120 characters)
	file=${name//\//_}
	file="$WORK/bc/${file:0:120}.bc"
	case "$stage" in
	fragment) args=(fragment "$file" "$name") ;;
	vertex) args=(vertex "$file" "$name" "$WORK/rf.bc" rf) ;;
	kernel) args=(kernel "$file" "$name") ;;
	*) continue ;;
	esac
	if line=$(RWA_SOFTWARE=1 timeout 120 "$WORK/softpipesweep" "${args[@]}" 2> /dev/null); then
		case "$line" in
		ok*) ok=$((ok + 1)) ;;
		*) refused=$((refused + 1)) ;;
		esac
	else
		died=$((died + 1))
		echo "DIED	$stage	$name"
	fi
done < <(python3 "$ROOT/tools/metallib.py" list "$LIBRARY" | awk 'NF >= 3 && $3 == "air" { print $1, $2 }')
echo "softpipesweep.sh: $(basename "$LIBRARY"): $ok made, $refused refused, $died died"
[ "$died" = 0 ]
