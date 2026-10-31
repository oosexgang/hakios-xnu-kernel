#!/bin/bash
# Build tools/air/glassref (Apple's Metal against our AIR -> NIR -> softpipe, one draw, pixel by pixel)
# against the host build of libredwoodair. Rebuild that first when the frontend changed:
#   (cd xnu-deps/build/mesa26 && PATH=$PWD/venv/bin:/opt/homebrew/opt/bison/bin:$PATH \
#    ninja -C build-host src/gallium/drivers/freedreno/libredwoodair.dylib)
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
# BUILD=build-lp: against the llvmpipe build (16-bit arithmetic, which softpipe cannot run), as glassref-lp
BUILD=${BUILD:-build-host}
NAME=glassref; [ "$BUILD" = build-lp ] && NAME=glassref-lp
LIB=$ROOT/xnu-deps/build/mesa26/$BUILD/src/gallium/drivers/freedreno
OUT=${OUT:-$ROOT/xnu-deps/build/glassref}
mkdir -p "$OUT"
clang -fobjc-arc -O1 -g -Wall -Wno-unused-variable -I"$ROOT/tools/mesa/air" \
	-framework Metal -framework Foundation "$ROOT/tools/air/glassref.m" \
	-DGLASSCASE_NO_MAIN "$ROOT/tools/air/glasscase.c" \
	-L"$LIB" -lredwoodair -Wl,-rpath,"$LIB" -Wl,-headerpad_max_install_names -o "$OUT/$NAME"
# and tools/air/glasscase, which draws the cases glassref --case kept (the phone's copy: userland/build.sh)
CASE=glasscase; [ "$BUILD" = build-lp ] && CASE=glasscase-lp
clang -O1 -g -Wall -I"$ROOT/tools/mesa/air" "$ROOT/tools/air/glasscase.c" \
	-L"$LIB" -lredwoodair -Wl,-rpath,"$LIB" -Wl,-headerpad_max_install_names -o "$OUT/$CASE"
# (the library's install name is the phone's /redwood/lib path: point the tools at the host build)
install_name_tool -change /redwood/lib/libredwoodair.dylib "$LIB/libredwoodair.dylib" "$OUT/$NAME"
install_name_tool -change /redwood/lib/libredwoodair.dylib "$LIB/libredwoodair.dylib" "$OUT/$CASE"
echo "$OUT/$NAME $OUT/$CASE"
