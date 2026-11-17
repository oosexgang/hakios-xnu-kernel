#!/bin/bash
# Check that the repository's kernel sources are consistent: the release assembles (every patch
# applies), and exporting the assembled tree gives back exactly kernel/overlay and kernel/patches --
# so nothing was edited in a patch by hand in a way the tools would not write, and no file sits in
# the wrong place.
#
#   kernel/verify.sh [--from SRC]       SRC: a git repository with the release, instead of Apple's
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

from=()
[ "${1:-}" = --from ] && from=(--from "${2:?--from needs a repository}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$KERNEL_DIR/assemble.sh" "${from[@]}" "$work/xnu"
"$KERNEL_DIR/export.sh" --to "$work/out" "$work/xnu" > /dev/null
diff -r "$OVERLAY" "$work/out/overlay" ||
	die "kernel/overlay is not what an export of the assembled tree gives (above)"
for place in "${OVERLAY_PLACES[@]}"; do
	dir=${place%%:*}
	[ -d "$REPO_DIR/$dir" ] || [ -d "$work/out/places/$dir" ] || continue
	diff -r -x README.md -x Kconfig "$REPO_DIR/$dir" "$work/out/places/$dir" ||
		die "$dir is not what an export of the assembled tree gives (above)"
done
for patch in "$PATCHES"/*.patch "$work"/out/patches/*.patch; do
	name=${patch##*/}
	cmp -s "$PATCHES/$name" "$work/out/patches/$name" || {
		diff -u "$PATCHES/$name" "$work/out/patches/$name" | head -40 || true
		die "kernel/patches/$name is not what an export of the assembled tree gives (above): assemble, then export"
	}
done
log "ok: $UPSTREAM_TAG + $(overlay_files | wc -l) overlay files + $(series_patches | wc -l) patches assemble and export back unchanged"
