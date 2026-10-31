#!/bin/bash
# Run every texture shader of the corpus and check the texel it returns.
#
# air_run binds a texture whose texel (x, y, z) is (100+x, 200+y, 300+x+y+z, 400) --
# 4x4 for a 2D sampler, 4x4x4 for a 3D one, because a 3D sample out of a 2D
# texture can come back right by accident and that is not evidence --
# a nearest sampler with clamped addressing, and a coordinate at the centre of
# texel (1, 1). A shader that samples or fetches once must therefore return
# exactly 101 201 302 400 -- an equality, not a tolerance: nearest filtering at
# a texel centre has no weights to round and clamping has no arithmetic.
#
# What this does NOT check is anything that depends on Metal's own sampler
# state: the two modules of the corpus that declare a `constexpr sampler`
# carry it as a 64-bit word that is not decoded (docs/air.md). Here the API
# chooses the sampler, so what is verified is the texture operation and not
# Apple's filtering or addressing.
#
# A shader that combines several samples, or scales the coordinate, will not
# return the bare texel; those are listed as "other" with what they did return,
# which is a thing to read rather than a failure.
#
# usage: texcheck.sh [FILE.metallib ...]
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
AIR_RUN=${AIR_RUN:-$ROOT/build/mesa-host/build/src/gallium/drivers/freedreno/air_run}
AIR2NIR=${AIR2NIR:-$ROOT/build/mesa-host/build/src/compiler/air/air2nir}
[ -x "$AIR_RUN" ] || { echo "texcheck.sh: run tools/air/mk-host.sh first"; exit 1; }

LIBS=("$@")
if [ ${#LIBS[@]} = 0 ]; then
	LIBS=(
		"$ROOT/ios165/rootfs/System/Library/VideoDecoders/AV1SW.metallib"
		"$ROOT/ios165/rootfs/System/Library/Extensions/AppleMetalGLRenderer.bundle/default.metallib"
		"$ROOT/ios165/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib"
		"$ROOT/ios165/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/portrait_filters.metallib"
	)
fi

exact=0 other=0 refused=0 total=0
for lib in "${LIBS[@]}"; do
	[ -f "$lib" ] || continue
	# the functions that touch a texture at all
	"$AIR2NIR" --report "$lib" 2>/dev/null | while IFS= read -r line; do
		case "$line" in *$'\t'calls=*texture*) ;; *) continue ;; esac
		printf '%s\n' "${line%%$'\t'*}" | sed 's/^module=//'
	done > /tmp/texcheck.names
	while IFS= read -r fn; do
		[ -n "$fn" ] || continue
		total=$((total + 1))
		out=$("$AIR_RUN" "$lib" "$fn" 2>&1)
		got=$(printf '%s\n' "$out" | sed -n '/came back as/,$p' | sed -n '2,5p' |
			tr -d ' ' | paste -sd' ')
		# air_run knows what it bound, so it says what to expect; the 3D case
		# wants a different texel from the 2D one
		WANT=$(printf '%s\n' "$out" | sed -n 's/^air_run: expected \(.*\)  (the centre.*/\1/p')
		if [ -z "$got" ]; then
			# the reason, not the first line: air_run says why on stderr, and
			# "<name> on softpipe" is only which screen it got
			# -a: a fault dump can carry a NUL, and a binary verdict here would
			# lose the reason and leave "refused" with nothing said about why
			why=$(printf '%s\n' "$out" |
				grep -aE 'this harness|is a .* shader|Assertion|stage: |error|no gallium|does not bind|stores nothing' |
				head -1)
			[ -n "$why" ] || why=$(printf '%s\n' "$out" | tail -1)
			printf 'refused %-44s %s\n' "$fn" "${why:0:90}"
			refused=$((refused + 1))
		elif [ -n "$WANT" ] && [ "$got" = "$(echo $WANT | tr ' ' '\n' | paste -sd' ')" ]; then
			printf 'EXACT   %-44s %s\n' "$fn" "$got"
			exact=$((exact + 1))
		else
			printf 'other   %-44s %s\n' "$fn" "$got"
			other=$((other + 1))
		fi
	done < /tmp/texcheck.names
done
rm -f /tmp/texcheck.names

echo
echo "texcheck: $total texture shaders run: $exact returned the expected texel exactly,"
echo "texcheck: $other returned something else, $refused could not be run"
echo "texcheck: the sampler here is the API's (nearest, clamped); Metal's own sampler word"
echo "texcheck: is not decoded, so nothing here verifies Apple's filtering or addressing"
