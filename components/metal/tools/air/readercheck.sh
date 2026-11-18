#!/bin/bash
# The AIR reader against bitcode it was not written for (tools/air/readercheck.c), under
# AddressSanitizer and UndefinedBehaviorSanitizer: both copies of it, the driver's (tools/mesa/air)
# and the plug-in's (plugin/tools/mesa/air), each built from its four files and Mesa's ralloc.c
# alone (no NIR, no GPU). The good modules are the plug-in's kernels as its test fixtures hold
# them (plugin/tests/fixtures/*_bc.hpp); plugin/tools/kernels/*.ll through llvm-as are the bytes
# some of the changed modules were made from, and where llvm-as makes other bytes those are
# skipped. Needs a Mesa source tree for util/ (tools/air/mk-host.sh's, or MESA_SRC=).
#
# The changed modules came from a fuzzer as small as this: a good module, one to eight of its
# bytes flipped, set to 0, 0xff, a random byte or a random or all-ones 32-bit word, and read; what
# the reader before its bounds took a crash or a hang on is in readercheck.c.
set -e
shopt -s nullglob
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
MESA_SRC=${MESA_SRC:-$ROOT/build/mesa-host/mesa-26.2.5}
LLVM_AS=${LLVM_AS:-llvm-as}
[ -f "$MESA_SRC/src/util/ralloc.c" ] || { echo "readercheck.sh: run tools/air/mk-host.sh first, or name a Mesa source tree in MESA_SRC"; exit 1; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/good" "$WORK/kernels"

python3 -I - "$ROOT/plugin/tests/fixtures" "$WORK/good" <<'EOF'
import pathlib, re, sys
for h in sorted(pathlib.Path(sys.argv[1]).glob('*_bc.hpp')):
    body = h.read_text().split('[] = {', 1)[1].split('};', 1)[0]
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', body))
    (pathlib.Path(sys.argv[2]) / (h.name[:-len('_bc.hpp')] + '.bc')).write_bytes(data)
EOF
if command -v "$LLVM_AS" > /dev/null; then
	for ll in "$ROOT"/plugin/tools/kernels/*.ll; do
		"$LLVM_AS" -o "$WORK/kernels/$(basename "$ll" .ll).bc" "$ll" 2> /dev/null || true
	done
fi

# Mesa's headers want a few of its -D flags; these are the ones util/ralloc.c and the reader's
# headers need on Linux. The plug-in's nir_air.h names Mesa 25's gl_shader_stage, which Mesa 26
# calls mesa_shader_stage.
CFLAGS="-std=gnu11 -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
	-D_GNU_SOURCE -DHAVE_ENDIAN_H -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -Dgl_shader_stage=mesa_shader_stage"
# (an allocation of more than 256 MB is an error: the reader must refuse a count before it sizes one)
export ASAN_OPTIONS=allocator_may_return_null=0:malloc_limit_mb=256:detect_leaks=1
bad=0
for copy in tools/mesa/air plugin/tools/mesa/air; do
	extra=
	# (the driver's copy reads the fat .metallib of Darwin 26 and 27; the plug-in's does not)
	[ "$copy" = tools/mesa/air ] && extra=-DREADERCHECK_FAT
	# shellcheck disable=SC2086
	cc $CFLAGS $extra -I"$ROOT/$copy" -I"$MESA_SRC/src" -I"$MESA_SRC/include" -o "$WORK/readercheck" \
		"$ROOT/tools/air/readercheck.c" "$ROOT/$copy"/air_bitcode.c "$ROOT/$copy"/air_module.c \
		"$ROOT/$copy"/air_layout.c "$ROOT/$copy"/air_metallib.c "$MESA_SRC/src/util/ralloc.c"
	echo "== $copy"
	"$WORK/readercheck" "$WORK"/good/*.bc -- "$WORK"/kernels/*.bc > "$WORK/out" 2>&1 || bad=1
	grep -v '^refused  \|^read     ' "$WORK/out" || true
	grep -E '^(read|refused)  ' "$WORK/out" | grep -E 'good modules|changed modules' || true
	grep -c '^refused  ' "$WORK/out" | sed 's/$/ inputs refused/'
done
exit $bad
