#!/bin/bash
# The frontend's whole test run, in the order of what it proves (docs/air.md
# section 4): first that the bitcode reader agrees with LLVM on every module of
# the corpus, then how far every module gets through translation and ir3.
#
# This is the project's CI. There is no hosted one -- the corpus is Apple's
# code and stays off the network -- so this runs before every commit that
# touches the frontend, and docs/air-report.md is the report of that commit.
#
#   tools/air/check.sh            check, and write docs/air-report.md
#   tools/air/check.sh -q         only the numbers, write nothing
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
IOS=${IOS:-$ROOT/ios165}
QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1

# The corpus: the MTLB libraries of the device image. The five are named rather
# than searched for, so a missing one is an error and not a smaller corpus.
LIBS=(
	"$IOS/rootfs/System/Library/VideoDecoders/AV1SW.metallib"
	"$IOS/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib"
	"$IOS/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib"
	"$IOS/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/portrait_filters.metallib"
)
# SafariSharedUI's default.metallib is in the corpus list of docs/air.md and is
# deliberately not here: it is 118 bytes and holds no functions at all.
for f in "${LIBS[@]}"; do
	[ -f "$f" ] || { echo "check.sh: missing $f"; exit 1; }
done

AIR2NIR=$ROOT/build/mesa-host/build/src/compiler/air/air2nir
[ -x "$AIR2NIR" ] || { echo "check.sh: run tools/air/mk-host.sh first"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== the reader against llvm-dis"
for f in "${LIBS[@]}"; do
	python3 "$ROOT/tools/metallib.py" dump "$f" "$WORK/$(basename "$(dirname "$f")")" > /dev/null
done
"$ROOT/tools/air/reader-check.sh" "$WORK" | tail -3
reader_rc=${PIPESTATUS[0]}

echo
echo "== translation and ir3, over the corpus"
if [ "$QUIET" = 1 ]; then
	python3 "$ROOT/tools/air/corpus.py" --ir3 --md /dev/null "${LIBS[@]}"
else
	python3 "$ROOT/tools/air/corpus.py" --ir3 \
		--md "$ROOT/docs/air-report.md" --json "$ROOT/build/air-report.json" "${LIBS[@]}"
	echo "check.sh: wrote docs/air-report.md"
fi

echo
echo "== against LLVM on the processor, and drawn, shader by shader"
# The strongest check there is, and it needs no GPU: LLVM runs the same module
# over the same buffer bytes, our NIR is folded over them, and the numbers are
# compared. Only the class where this is decidable -- an entry point whose
# arguments are all buffers and which returns its result -- is counted; the
# rest say why not. --draw also puts each one through a Gallium driver and
# compares the pixel that comes back, which is M1's acceptance test with
# softpipe standing in for the Adreno. See tools/air/cpuref.py and air_run.c.
cpu_rc=0
for f in "${LIBS[@]}"; do
	echo "$(basename "$(dirname "$f")")/$(basename "$f"):"
	python3 "$ROOT/tools/air/cpuref.py" --all --draw "$f" 2>&1 | tail -3 | sed 's/^/  /' ||
		cpu_rc=1
done

exit $((reader_rc | cpu_rc))
