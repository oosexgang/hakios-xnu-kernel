#!/bin/bash
# Argument buffers at RUN TIME: the same picture through an MTLArgumentEncoder's
# records as through direct arguments, and a named different picture for every
# record that is deliberately wrong.
#
# This is the runtime half of tools/air/argbufcheck.py, which checks the
# frontend's half (the member table, the indices it allocates, nine refusals).
# What it cannot check is what this does, and it says so itself:
#
#   "nothing about the runtime's half. There is no argument buffer to write
#    (docs/air.md section 8.2): the frontend resolved every descriptor, so
#    MTLArgumentEncoder has to RECORD rather than write, and no part of that is
#    exercised or even present here."
#
# and the two gaps it names -- that a sampler member's index is invisible to
# air_run (one sampler state is bound to every slot) and that a texture
# member's index is invisible to softpipe's COMPUTE path (every texture comes
# back from the view bound at slot 0) -- are both closed here, because a
# FRAGMENT shader on the same softpipe distinguishes both and the two pictures
# are named rather than merely different.
#
# Everything runs through the contract in tools/mesa/air/air_metal.h and
# nothing else: air_metal_test knows that header and no more, which is what
# makes it stand in for the Metal driver (userland/src/redwoodmetal.m drives
# the same calls on the phone -- see userland/rc.jobs.argbuf).
#
#   tools/air/argbufruncheck.sh
#
# Exit 0 when every check passed. The modules are tools/air/argbuf_set.ll and
# tools/air/argbuf_direct.ll, which are written by hand and explain themselves:
# no byte of either corpus is in them, and Apple's own libraries have no
# argument buffer at all (0 of 456).
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
TEST=${AIR_METAL_TEST:-$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno/air_metal_test}
[ -x "$TEST" ] || { echo "argbufruncheck.sh: run tools/air/mk-host.sh first"; exit 1; }
command -v llvm-as > /dev/null || { echo "argbufruncheck.sh: no llvm-as"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for m in argbuf_set argbuf_direct; do
	llvm-as -o "$WORK/$m.bc" "$ROOT/tools/air/$m.ll" || {
		echo "argbufruncheck.sh: llvm-as would not assemble $m.ll"
		exit 1
	}
done

# What each module declares, printed before anything is bound: this is the
# answer -[MTLFunction newArgumentEncoderWithBufferIndex:] has to give, and
# `encodedLength` is the number the application allocates its buffer from.
"$TEST" --argbufs "$WORK/argbuf_set.bc"
"$TEST" --argbufs "$WORK/argbuf_direct.bc"

"$TEST" --draw-argbuf "$WORK/argbuf_set.bc" "$WORK/argbuf_direct.bc"
rc=$?

if [ "$rc" = 0 ]; then
	echo "argbufruncheck.sh: every check passed"
else
	echo "argbufruncheck.sh: FAILED"
fi
exit $rc
