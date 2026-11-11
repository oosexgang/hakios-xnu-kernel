# shellcheck shell=bash disable=SC2034  # sourced: its variables are used by the scripts that source it
# Shared by kernel/assemble.sh, export.sh and verify.sh (sourced, not run).
#
# The kernel is kept as three things:
#   UPSTREAM          which Apple release it is made from
#   overlay/          the files HakiOS adds, at their place in the XNU tree
#   patches/          the changes to Apple's own files, one patch a topic (patches/series)
# and an "assembled tree" is a checkout of the release with the overlay copied in and the patches
# applied: what is compiled, and where the kernel is worked on. It is a git repository of its own:
#   tag upstream      Apple's release
#   tag assembled     what the last assemble.sh put there (a commit on top of upstream)
#   branch hakios     the working branch
KERNEL_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OVERLAY=$KERNEL_DIR/overlay
PATCHES=$KERNEL_DIR/patches
SERIES=$PATCHES/series
# shellcheck source=UPSTREAM
source "$KERNEL_DIR/UPSTREAM"

log() { echo "${0##*/}: $*"; }
die() { echo "${0##*/}: $*" >&2; exit 1; }

# Files the build makes inside the tree: never part of the kernel's sources.
TREE_EXCLUDES=(/BUILD/ /bsd/dev/redwood_focaltech_fw.h '*.orig' '*.rej')

# The patches of the series, in order.
series_patches() {
	awk '!/^[[:space:]]*(#|$)/ && !seen[$1]++ { print $1 }' "$SERIES"
}

# patch_for FILE: the patch a file of the tree belongs to -- the first line of the series with a
# pattern that matches it (a directory ending in /, a path, or a glob). Nothing if none does.
patch_for() {
	local file=$1 patch pattern
	while read -r patch patterns; do
		case "$patch" in ''|'#'*) continue ;; esac
		for pattern in $patterns; do
			[ "${pattern%/}" = "$pattern" ] || pattern="$pattern*"  # a directory: everything in it
			# shellcheck disable=SC2254  # the pattern is meant to be a glob
			case "$file" in
			$pattern) echo "$patch"; return 0 ;;
			esac
		done
	done < "$SERIES"
	return 1
}

# git on an assembled tree, with nothing of the user's configuration changing what a diff looks like
tree_git() {
	local tree=$1
	shift
	git -C "$tree" -c core.quotepath=false -c diff.noprefix=false -c diff.mnemonicPrefix=false \
		-c diff.algorithm=myers -c core.autocrlf=false -c user.name=hakios -c user.email=hakios@localhost "$@"
}

is_assembled_tree() {
	[ -d "$1/.git" ] && git -C "$1" rev-parse -q --verify refs/tags/upstream > /dev/null 2>&1
}
