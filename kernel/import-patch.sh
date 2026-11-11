#!/bin/bash
# Take patches written against the kernel tree into the repository: each is applied to an assembled
# tree, and the tree is exported, so their content ends up where it belongs -- new files in
# kernel/overlay, changes to Apple's files in the topic patches of kernel/patches.
#
#   kernel/import-patch.sh [--tree DIR] [--from SRC] PATCH...
#
# This is how a patch of the earlier layout (renoir/kernel/patches/00NN-*.patch, or one sent by
# somebody who works that way) is carried over: its paths are those of the same tree.
#
#   --tree DIR      the assembled tree to use (default: $XNU_SRC of kernel/env.sh if it is there, else a
#                   temporary one, removed afterwards)
#   --from SRC      (for a temporary tree) take Apple's release from this repository instead of the network
#
# The tree is first brought up to date with the repository; one with work that was not exported is
# refused, as by assemble.sh. A patch that does not apply stops the import with nothing exported; the
# tree is then as far as the patches before it got, for looking at (`git -C DIR status`).
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

tree='' from=()
while [ $# -gt 0 ]; do
	case "$1" in
	--tree) tree=${2:?--tree needs a directory}; shift ;;
	--from) from=(--from "${2:?--from needs a repository}"); shift ;;
	-h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option $1 (see --help)" ;;
	*) break ;;
	esac
	shift
done
[ $# -ge 1 ] || die "usage: import-patch.sh [--tree DIR] [--from SRC] PATCH..."
for patch in "$@"; do
	[ -f "$patch" ] || die "no such patch: $patch"
done

temporary=''
if [ -z "$tree" ]; then
	default=$(cd "$KERNEL_DIR/../.." && pwd)/${UPSTREAM_TAG%%.*}-hakios
	if is_assembled_tree "${XNU_SRC:-$default}"; then
		tree=${XNU_SRC:-$default}
	else
		temporary=$(mktemp -d)
		trap 'rm -rf "$temporary"' EXIT
		tree=$temporary/xnu
	fi
fi
if [ -n "$temporary" ]; then
	"$KERNEL_DIR/assemble.sh" "${from[@]}" "$tree"
else
	"$KERNEL_DIR/assemble.sh" --update "$tree"
	# (an update that found unexported work leaves it and succeeds: not a state to apply patches on)
	[ -z "$(git -C "$tree" status --porcelain)" ] ||
		die "$tree has work that is not in the repository: export it first (kernel/export.sh $tree)"
fi

for patch in "$@"; do
	log "applying ${patch##*/}"
	git -C "$tree" apply --whitespace=nowarn "$(cd "$(dirname "$patch")" && pwd)/${patch##*/}" ||
		die "${patch##*/} does not apply to the tree as the repository has it (nothing was exported)"
done
"$KERNEL_DIR/export.sh" "$tree"
log "done: review with git diff, then commit"
