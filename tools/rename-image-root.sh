#!/bin/bash
# Carry this repository's renaming of the small image into a working tree that still has the old names
# (darwin-renoir, or an older checkout): the mount point /redwood becomes /hakios -- with /flash/hakios,
# /private/preboot/hakios, the APFS volume "hakios" and /private/var/hakios-stamp -- and the image's
# libraries libredwood*.dylib become libhakios*.dylib. Android's own names (redwood_aac_ov02b10_macro,
# redwood-s-oss, com.qti.sensormodule.redwood_*) and a device package called redwood are left alone.
#
#   tools/rename-image-root.sh DIR          what would change, by file
#   tools/rename-image-root.sh --write DIR  change it
#
# Only text files git tracks in DIR are touched (or every text file, where DIR is not in git). Build the
# tree anew afterwards: what an earlier build left in obj/ and the image trees still has the old names.
set -euo pipefail

write=0
if [ "${1:-}" = --write ]; then
	write=1
	shift
fi
[ $# = 1 ] && [ -d "$1" ] || { echo "usage: $0 [--write] DIR" >&2; exit 2; }
cd "$1"

# the same rules, in the same order, as the change in this repository
rules='
	s{libredwoodair}{libhakios-air}g; s{-lredwoodair}{-lhakios-air}g;
	s{redwoodair_link_args}{hakios_air_link_args}g; s{\x27redwoodair\x27}{\x27hakios-air\x27}g;
	s{^\+libredwoodair = shared_library\(}{+libhakios_air = shared_library(}g; s{link_with : \[libredwoodair\]}{link_with : [libhakios_air]}g;
	s{libredwoodmetal}{libhakios-metal}g; s{libredwood26}{libhakios26}g;
	s{libredwood-}{libhakios-}g; s{libredwood\b}{libhakios}g;
	s{(?<![\w.-])/redwood\b}{/hakios}g;
	s{\b(flash|preboot|flashboot|sysroot)/redwood\b}{$1/hakios}g;
	s{\bredwood=}{hakios=}g;
	s{"/flash redwood"}{"/flash hakios"}g;
	s{redwoodtriangle}{hakiostriangle}g;
	s{redwood-stamp}{hakios-stamp}g;
	s{\x27redwood\x27, \x27private/var\x27}{\x27hakios\x27, \x27private/var\x27}g;
	s{strncmp\(argv\[0\], "/hakios/", 9\)}{strncmp(argv[0], "/hakios/", sizeof("/hakios/") - 1)}g;
'

if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
	list() { git ls-files -z; }
else
	list() { find . -type f -not -path './.git/*' -print0; }
fi

changed=0
while IFS= read -r -d '' file; do
	grep -Iq . "$file" 2> /dev/null || continue             # (text files only)
	grep -qE 'redwood' "$file" || continue
	new=$(perl -pe "$rules" "$file")
	if [ "$new" != "$(cat "$file")" ]; then
		changed=$((changed + 1))
		echo "$file: $(diff <(cat "$file") <(printf '%s\n' "$new") | grep -c '^>') lines"
		if [ $write = 1 ]; then
			perl -pi -e "$rules" "$file"
		fi
	fi
done < <(list)

echo "rename-image-root: $changed files $([ $write = 1 ] && echo changed || echo 'would change (--write to change them)')"
