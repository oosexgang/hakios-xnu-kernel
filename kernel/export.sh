#!/bin/bash
# Take the kernel's sources out of an assembled tree, back into the repository: the files the tree has
# and Apple's release has not become kernel/overlay, the changes to Apple's files become the patches
# of kernel/patches/series. Run it after working in the tree; `git diff` in the repository then shows
# what the work was.
#
#   kernel/export.sh DIR            from the tree in DIR (made by kernel/assemble.sh)
#
#   --to OUT        write OUT/overlay and OUT/patches instead of the repository's (kernel/verify.sh)
#
# What the build leaves in the tree (BUILD/, the firmware stub) is not sources and is left out. A
# changed file of Apple's that no line of patches/series claims stops the export: give it a topic there.
# A patch written by somebody else against the tree (a .patch file) is taken over the same way: apply it
# in the tree (git apply), then export.
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

out=$KERNEL_DIR
while [ $# -gt 0 ]; do
	case "$1" in
	--to) out=${2:?--to needs a directory}; shift ;;
	-h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option $1 (see --help)" ;;
	*) break ;;
	esac
	shift
done
[ $# -eq 1 ] || die "usage: export.sh [--to OUT] DIR"
is_assembled_tree "$1" || die "$1 is not a tree made by assemble.sh"
D=$(cd "$1" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)

work=$(mktemp -d)
trap 'rm -rf "$work"; rm -f "$D/.git/hakios-export-index"' EXIT

# The tree as it lies there (committed or not, new files included) in an index of its own.
export GIT_INDEX_FILE=$D/.git/hakios-export-index
rm -f "$GIT_INDEX_FILE"
tree_git "$D" read-tree upstream
tree_git "$D" add -A .

mkdir -p "$work/overlay" "$work/patches"
unclaimed=
declare -A files_of
while IFS=$'\t' read -r status file; do
	case "$status" in
	A)
		mkdir -p "$work/overlay/$(dirname "$file")"
		cp -p "$D/$file" "$work/overlay/$file"
		;;
	*)
		if patch=$(patch_for "$file"); then
			files_of[$patch]+="$file"$'\n'
		else
			unclaimed+="  $file"$'\n'
		fi
		;;
	esac
done < <(tree_git "$D" diff --cached --no-renames --name-status upstream)
[ -z "$unclaimed" ] || die "changed files of Apple's that no line of kernel/patches/series claims:
$unclaimed"

for patch in $(series_patches); do
	[ -n "${files_of[$patch]:-}" ] || continue
	mapfile -t files < <(printf '%s' "${files_of[$patch]}")
	# (without the "index" lines: their hashes are abbreviated by how many objects a clone has)
	tree_git "$D" diff --cached --no-color --no-ext-diff --no-textconv --no-renames -U3 \
		--src-prefix=a/ --dst-prefix=b/ upstream -- "${files[@]}" |
		sed -E '/^index [0-9a-f]+\.\.[0-9a-f]+( [0-7]+)?$/d' > "$work/patches/$patch"
done
unset GIT_INDEX_FILE

# In place of what was there; the series is the repository's and stays.
rm -rf "$out/overlay"
mv "$work/overlay" "$out/overlay"
mkdir -p "$out/patches"
rm -f "$out"/patches/*.patch
for patch in "$work"/patches/*.patch; do
	[ -e "$patch" ] && mv "$patch" "$out/patches/"
done
[ "$out" = "$KERNEL_DIR" ] || cp "$SERIES" "$out/patches/series"
log "$(find "$out/overlay" -type f | wc -l) files in overlay, $(find "$out/patches" -name '*.patch' | wc -l) patches in $out"
