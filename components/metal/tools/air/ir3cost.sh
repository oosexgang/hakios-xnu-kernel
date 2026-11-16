#!/bin/bash
# Build tools/air/ir3cost against the host build of libhakios-air (build-host), which carries ir3.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
LIB=$ROOT/../../userland/xnu-deps/build/mesa26/build-host/src/gallium/drivers/freedreno
OUT=${OUT:-$ROOT/../../userland/xnu-deps/build/glassref}
mkdir -p "$OUT"
clang -fobjc-arc -O1 -g -Wall -I"$ROOT/tools/mesa/air" -framework Metal -framework Foundation \
	"$ROOT/tools/air/ir3cost.m" -L"$LIB" -lhakios-air -Wl,-rpath,"$LIB" -Wl,-headerpad_max_install_names \
	-o "$OUT/ir3cost"
install_name_tool -change /hakios/lib/libhakios-air.dylib "$LIB/libhakios-air.dylib" "$OUT/ir3cost"
echo "$OUT/ir3cost"
