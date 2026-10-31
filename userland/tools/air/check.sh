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

echo
echo "== the air.* operations, one at a time"
# The section above ends with "0 call an air.* function", which is the whole
# reason this one exists: a module that agrees with LLVM proves the reader and
# the instructions, and says nothing about the intrinsics, because LLVM cannot
# evaluate a call it has no body for. So each operation is called on its own
# here, on inputs chosen to break it, against a reference in the script.
# NOT `| tail -8`. That is what hid the one line that mattered: intrincheck
# prints, before anything else, how many of the corpus's libraries it could not
# read -- and a library it cannot read takes its operations out of the run and
# pushes the pass count UP, so the number is the difference between a result
# and an upper bound. air2nir is relinked constantly in this shared tree, so
# "could not be executed" is an ordinary event, not a freak one; it is how a
# log came to read "134 agree, 0 differ" while the vocabulary is 553
# declarations wide. Every summary line is kept now and the per-operation
# detail is what gets trimmed instead.
intrin_rc=0
python3 "$ROOT/tools/air/intrincheck.py" > "$WORK/intrin.log" 2>&1 || intrin_rc=$?
grep -a -E '^(WRONG|REFUSED|NOCASE|ASIDE|refused|approx|unread) ' "$WORK/intrin.log" | head -40 || :
grep -a '^intrincheck:' "$WORK/intrin.log" || :

echo
echo "== the same operations run, not folded"
# A different check and not a slower one: folding asks NIR's constant
# expression, this builds the arguments into a buffer so nothing can be folded
# and hands the shader to a driver that compiles and executes it. That is how
# air.sign(-0) was caught -- NIR's folder keeps the sign of a zero, softpipe
# does not, and nir_fsign promises neither.
#
# --cases 4 and not the whole pool: 612 cases in 79 seconds against 2586 in
# 300, and it still catches that bug, which is how the subset was chosen rather
# than guessed. The full run is `intrincheck.py --run --few`.
# The same reading as the section above, and for the same reason: this stage's
# summary names the declarations it set aside (narrow types, the fma, a half
# result) and how many modules the log did not answer for, and every one of
# those lines moves what the pass count means.
run_rc=0
python3 "$ROOT/tools/air/intrincheck.py" --run --cases 4 > "$WORK/intrinrun.log" 2>&1 || run_rc=$?
grep -a -E '^(WRONG|NOCASE|ASIDE|refused|approx) ' "$WORK/intrinrun.log" | head -24 || :
grep -a '^intrincheck:' "$WORK/intrinrun.log" || :

echo
echo "== the atomics, with a kernel written to make a lost update show"
# None of Apple's kernels can test an atomic: a sum written by threads that
# never raced reads the same as one written by threads that did. This one
# dispatches sixteen threads whose fetch_add results must be the numbers 0..15
# with nothing repeated, so a lost update leaves a duplicate and a gap.
python3 "$ROOT/tools/air/atomiccheck.py" 2>&1 | tail -8
atomic_rc=${PIPESTATUS[0]}

echo
echo "== the texture operations, on more than one texel"
# texcheck.sh runs the corpus's own texture shaders, which all sample texel
# (1, 1) because that is where air_run's vertex shader puts the coordinate: a
# lowering that ignored the coordinate and always fetched (1, 1) would pass all
# six of them. These ask for the corners too, so swapping x and y is caught on
# the five asymmetric coordinates and correctly not on the symmetric ones.
python3 "$ROOT/tools/air/texopcheck.py" 2>&1 | tail -10
texop_rc=${PIPESTATUS[0]}

echo
echo "== Metal's own sampler state, against Apple's rules"
# Every check above binds the API's sampler, nearest and clamp_to_edge, and
# says so. This one lets the shader's own `constexpr sampler` word decide, and
# compares against what Apple's header and the MSL spec say that word means --
# so a match is evidence about the word and not about our reading of it.
#
# It also runs the comparison function, the one field of that word whose other
# eight values nothing attests -- all three words the corpus carries hold the
# DEFAULT, which is `never` at 8, and refusing that was 653 of 653 failed draws
# in the window server's first boot through this library (docs/air.md 3.3). The
# eight give eight DISTINCT answers over three references, so the table cannot
# be permuted and still pass, and every comparison case is run twice: through
# air_run and through air_metal.c, which are two tables that must agree.
#
# tail -26 and not -8: the summary names what the comparison cases are worth and
# what they deliberately do not claim, and a smaller tail would scroll that off
# and leave a count with nothing to say what it counted.
python3 "$ROOT/tools/air/sampcheck.py" 2>&1 | tail -26
samp_rc=${PIPESTATUS[0]}

echo
echo "== Metal's packed 16-bit colour formats: which field became which channel"
# The window server asks for MTLPixelFormatBGR5A1Unorm (43) twice a boot, and all four
# of these were declined until the convention on each side was read instead of guessed.
# Gallium's side is mechanical (the generated unpackers in u_format_table.c); Metal's is
# one bit of information, and Gallium carries BOTH spellings of each of these four, so
# the two readings are both expressible and they exchange red and blue.
#
# This is the half that is decidable here, and it is a draw and not a table lookup: a
# texture of each format, every texel the same 16-bit word with ONE FIELD SET, sampled by
# Apple's own texturedFragment2DFloat, and the channel that came back 1 is the answer. A
# field of all ones is exactly 1.0, so there is no rounding in the expectation. What it
# cannot decide is Apple's convention -- see air_metal.c's format table and the last line
# this prints.
AMT=$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno/air_metal_test
if [ -x "$AMT" ]; then
	"$AMT" --pixel-formats \
		"$IOS/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib" \
		texturedFragment2DFloat 2>&1 | grep -E "^pixfmt" | tail -22
	pixfmt_rc=${PIPESTATUS[0]}
else
	echo "check.sh: no air_metal_test, so the packed formats are unchecked"
	pixfmt_rc=1
fi

echo
echo "== colour blending: the equation, and what each way of being wrong would have drawn"
# The newest piece of the contract, and the one whose absence was visible in a
# photograph rather than in a log: with no blend state in rwa_render_shape
# every pipeline was opaque, and CoreAnimation composites premultiplied
# layers -- so a transparent-black icon label landed as a BLACK BOX.
#
# A pass here is deliberately not "it blended". Four plausible mistakes all
# produce something that looks blended -- the factors exchanged, premultiplied
# read as straight alpha, the alpha half taken from the RGB half, and the
# enumerations CAST instead of mapped -- so every case prices all of them and
# the run fails if any one of them is distinguished by no case at all.
# MTLBlendFactor and pipe_blendfactor agree on two of nineteen values and
# MTLColorWriteMask's bits run backwards from Gallium's colormask, so the
# write-mask cases include 0xe and 0x7 on purpose: a suite built out of All and
# None alone would pass with the nibble reversed.
#
# It also carries the row that matters most on this device: Apple's own
# UNTOUCHED descriptor, which 504 of the 1079 pipelines in this phone's
# CoreAnimation archive arrive as -- source factor One, writeMask 0xf, blending
# OFF, because those layers blend inside their fragment shader. Every field but
# one is non-zero there, so a driver that inferred "there is blend state here"
# from a non-default field would blend half the home screen twice.
#
# tail -22 and not -8: the per-case lines come first and the summary ends with
# what this cannot reach at all (the Adreno, Min/Max with a factor other than
# One, dither), which is the half a count cannot say.
if [ -x "$AMT" ]; then
	"$AMT" --draw-blend 		"$IOS/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib" 		texturedFragment2DFloat 2>&1 |
		grep -a -E '^blend: (FAIL|WRONG|NOT REFUSED|[0-9]+ (cases|way)|the expectations|what each|nothing here)|^blend:   [^ ]' |
		tail -14
	blend_rc=${PIPESTATUS[0]}
else
	echo "check.sh: no air_metal_test, so blending is unchecked"
	blend_rc=1
fi

echo
echo "== the lowerings done by hand, against LLVM on this processor"
# Two things AIR has and NIR does not, so the frontend builds them out of
# something else: a switch (a chain of nir_goto_if) and a call returning a
# struct (one out-parameter per member). Counting modules says neither computes
# the right thing -- a chain that always fell to the default would print the
# same .ll as reader-check compares, and swapped struct members would translate
# and reach ir3 without complaint. So cpuref has llc run each generated module
# on this processor and compares LLVM's result with our folded NIR. The arm
# count is what makes agreement mean anything.
python3 "$ROOT/tools/air/loweringcheck.py" 2>&1 | tail -4
lowering_rc=${PIPESTATUS[0]}

echo
echo "== framebuffer fetch: the shape, because no value can be compared here"
# The one interface key with no oracle on this machine. cpuref runs the module
# under LLVM, where there is no framebuffer; air_run draws it, but on softpipe,
# and softpipe's TGSI interpreter has no FBFETCH case at all. So this checks
# what is decidable -- the intrinsic at the target the module names, the flag
# a6xx needs to flush between overlapping primitives, the narrowing of a half
# target, an integer target refused, an unused argument not turned into a fetch
# -- plus that ir3 emits the instruction, and says in its own last line that it
# compares no value (docs/air.md section 3.1).
python3 "$ROOT/tools/air/fbfetchcheck.py" 2>&1 | tail -8
fbfetch_rc=${PIPESTATUS[0]}

echo
echo "== vertex attributes: the shader's half of an MTLVertexDescriptor"
# The other interface key with no oracle here, and for a different reason: a
# `[[stage_in]]` attribute's format, offset, stride and buffer are the
# MTLVertexDescriptor's and are nowhere in the module, and air_run's only vertex
# stage is its own generated quad -- so nothing on this machine draws with a
# translated vertex shader. What is decidable is the shader's half: the Metal
# attribute index reaching ir3's fetch table with the right write mask, the
# 32-bit input and the narrowing that keeps a half attribute out of the half
# register file, the shapes that must be refused by name rather than read from
# the wrong vertex, and an unused attribute costing no fetch at all.
#
# Then the same translation against Apple's own metadata, read a second time
# with a regular expression over `air2nir --ll`: QuartzCore's fourteen
# [[stage_in]] vertex shaders, attribute by attribute (docs/air.md section 3.2).
#
# tail -40 and not -8: the cases come before the summary, and this section has
# twenty lines in a clean run and more when a real module disagrees, so a
# smaller tail would scroll the FAIL off and leave the summary saying 13 of 14
# with nothing to say which.
IOS="$IOS" python3 "$ROOT/tools/air/vattrcheck.py" 2>&1 | tail -40
vattr_rc=${PIPESTATUS[0]}

echo
echo "== argument buffers: the member table and the indices it allocates"
# The one interface key with no example in this corpus at all: 0 of its 456
# modules declare an air.indirect_buffer and 7 620 of Resident Evil 4's 7 879 do
# (docs/air.md section 8.2). So the modules are written here, with a set whose
# members are known, and checked three ways: the interface air2nir reports
# against the table they were generated from, a dispatch on softpipe with
# DIFFERENT BYTES IN EVERY SLOT -- every other buffer check in this file fills
# them all alike, so a shader reading the wrong slot would pass -- and the same
# module run by LLVM on this processor. Then nine shapes that must be refused by
# name.
#
# `argbufcheck.py --census CORPUS` is the other half and is not run here: it
# counts the argument-buffer metadata of a library this repository does not and
# must not contain.
python3 "$ROOT/tools/air/argbufcheck.py" 2>&1 | tail -30
argbuf_rc=${PIPESTATUS[0]}

echo
echo "== argument buffers at run time: the encoder that records instead of writing"
# The runtime half of the stage above, and the first stage in this file that drives
# tools/mesa/air/air_metal.h -- the contract the Metal driver uses -- rather than the
# frontend on its own. There is nothing for an encoder to write here (docs/air.md
# 8.2.2): the frontend resolved every member of every set to a binding, so
# MTLArgumentEncoder RECORDS (resource, Metal id) against (destination buffer,
# destination offset) and the draw binds what it finds.
#
# What makes that mean anything is a differential, which is what this is: the same pixel
# out of an argument buffer as out of five direct arguments, and a named different
# picture for every record made wrong on purpose. It also closes the two things the
# stage above says it cannot check -- a sampler member's index and a texture member's
# index -- because a FRAGMENT shader on softpipe distinguishes both where a compute
# kernel does not, and it is the only stage here that exercises the refusal this feature
# is mostly made of: an argument buffer nobody encoded.
#
# It needs llvm-as (as argbufcheck.py does) and air_metal_test, which mk-host.sh builds.
"$ROOT/tools/air/argbufruncheck.sh" 2>&1 | tail -16
argbufrun_rc=${PIPESTATUS[0]}

echo
echo "== the library functions, wrapped so they can run at all"
# A third of the corpus is air.visible library functions: not entry points, and
# not dispatchable as themselves, so nothing above touches them. --library
# wraps one in a generated entry point (docs/air.md section 5) and this reports
# what each comparison is worth -- LLVM is a full reference only where the
# function calls no air.* at all, and the rest is our folder against a driver.
python3 "$ROOT/tools/air/libcheck.py" 2>&1 | tail -24
lib_rc=${PIPESTATUS[0]}

echo
echo "== the kernels the driver ran on the phone, against softpipe here"
# Everything above runs on this machine and ends at a Gallium driver. This is the
# other end: Metal's own kernels out of the system's libraries, compiled on the
# device by our compiler library and dispatched on the Adreno, against air_run on
# softpipe over the same zeros and the same 4x4x1 grid (docs/metal.md 10.16).
# Those numbers were first read off the screen by hand, which is why this exists.
#
# It reads a boot log and nothing else, so a checkout that has never driven the
# phone has nothing to read: --newest then says "no device log" and the section
# passes, because this check is about a boot that happened and not about the
# frontend. What it must never do is report a pass having compared nothing, so a
# missing rootfs, a broken air_run or a grid that drifted fail it as loudly as a
# wrong word does.
IOS="$IOS" python3 "$ROOT/tools/air/metalcheck.py" --newest 2>&1 | tail -28
metal_rc=${PIPESTATUS[0]}

exit $((reader_rc | cpu_rc | intrin_rc | run_rc | atomic_rc | texop_rc | samp_rc \
	| pixfmt_rc | blend_rc | lowering_rc | fbfetch_rc | vattr_rc | argbuf_rc \
	| argbufrun_rc | lib_rc | metal_rc))
