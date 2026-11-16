#!/bin/bash
# RGB9E5 unpack and pack, folded by the translator (tools/air/rgb9e5.ll). Needs the host build
# (tools/air/mk-host.sh).
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
AIR2NIR=$ROOT/build/mesa-host/build/src/compiler/air/air2nir
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
[ -x "$AIR2NIR" ] || { echo "rgb9e5check.sh: run tools/air/mk-host.sh first"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$LLVM/llvm-as" -o "$WORK/e9.bc" "$ROOT/tools/air/rgb9e5.ll"
got=$("$AIR2NIR" --eval "$WORK/e9.bc" 2>&1 | grep -a "^  out" | tr -s ' ' | cut -d' ' -f3-)
if [ "$got" = "1 0.5 0.00390625 -4.59536213e-40" ]; then
	echo "rgb9e5check: unpacked and packed back: ok"
else
	echo "rgb9e5check: WRONG: $got"
	exit 1
fi
