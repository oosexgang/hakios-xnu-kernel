#!/bin/bash
# Make the kernel's source tree: Apple's release (kernel/UPSTREAM) with kernel/overlay copied in and
# kernel/patches applied.
#
#   kernel/assemble.sh DIR              a new tree in DIR (which must not exist yet)
#   kernel/assemble.sh --update DIR     bring a tree made earlier up to date with the repository
#
#   --from SRC      take the release from this git repository or URL instead of Apple's (a mirror, or a
#                   clone already on this machine: no network needed)
#   --discard       (--update) throw away what was changed in the tree and not exported
#
# The tree is a git repository: tag `upstream` is Apple's release, tag `assembled` what this script put
# there, branch `hakios` where work goes on. An update touches only the files whose contents change, so
# the next build compiles only those. Work done in the tree goes back to the repository with
# kernel/export.sh; --update refuses to run over such work.
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

update=0 discard=0 from=$UPSTREAM_URL
while [ $# -gt 0 ]; do
	case "$1" in
	--update) update=1 ;;
	--discard) discard=1 ;;
	--from) from=${2:?--from needs a repository}; shift ;;
	-h|--help) sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option $1 (see --help)" ;;
	*) break ;;
	esac
	shift
done
[ $# -eq 1 ] || die "usage: assemble.sh [--update] [--from SRC] [--discard] DIR"
D=$1

# The tree object of upstream + overlay + patches, made in an index of its own: nothing in the working
# tree is touched by this.
assembled_tree() {
	local patch
	export GIT_INDEX_FILE=$D/.git/hakios-assemble-index
	rm -f "$GIT_INDEX_FILE"
	tree_git "$D" read-tree upstream
	(cd "$OVERLAY" && git --git-dir="$D/.git" --work-tree=. -c core.autocrlf=false add -f --no-all .)
	for patch in $(series_patches); do
		[ -f "$PATCHES/$patch" ] || die "patches/series names $patch, which is not there"
		tree_git "$D" apply --cached --whitespace=nowarn "$PATCHES/$patch" ||
			die "patches/$patch does not apply to $UPSTREAM_TAG"
	done
	tree_git "$D" write-tree
	rm -f "$GIT_INDEX_FILE"
	unset GIT_INDEX_FILE
}

if [ $update = 0 ]; then
	[ ! -e "$D" ] || die "$D exists (to bring it up to date: assemble.sh --update $D)"
	log "fetching $UPSTREAM_TAG from $from"
	git init -q "$D"
	D=$(cd "$D" && pwd)
	git -C "$D" fetch -q --depth 1 "$from" "refs/tags/$UPSTREAM_TAG" ||
		die "cannot fetch refs/tags/$UPSTREAM_TAG from $from"
	got=$(git -C "$D" rev-parse 'FETCH_HEAD^{commit}')
	[ "$got" = "$UPSTREAM_COMMIT" ] ||
		die "$from has $UPSTREAM_TAG at $got, not at $UPSTREAM_COMMIT (kernel/UPSTREAM): not using it"
	git -C "$D" tag upstream "$got"
	printf '%s\n' "${TREE_EXCLUDES[@]}" >> "$D/.git/info/exclude"
else
	is_assembled_tree "$D" || die "$D is not a tree made by assemble.sh"
	D=$(cd "$D" && pwd)
	[ "$(git -C "$D" rev-parse 'upstream^{commit}')" = "$UPSTREAM_COMMIT" ] ||
		die "$D is of another release than kernel/UPSTREAM names: make a new tree"
	if [ $discard = 1 ]; then
		git -C "$D" reset -q --hard
		git -C "$D" clean -q -fd
	elif [ -n "$(git -C "$D" status --porcelain)" ] ||
	    [ "$(git -C "$D" rev-parse HEAD)" != "$(git -C "$D" rev-parse 'assembled^{commit}')" ]; then
		die "$D has work that is not in the repository:
$(git -C "$D" status --short | head -20)
take it over with kernel/export.sh $D, or throw it away with --update --discard"
	fi
fi

tree=$(assembled_tree)
if [ $update = 1 ] && [ "$tree" = "$(git -C "$D" rev-parse 'assembled^{tree}')" ]; then
	log "$D is up to date"
	exit 0
fi
commit=$(tree_git "$D" commit-tree "$tree" -p upstream \
	-m "$UPSTREAM_TAG + HakiOS (kernel/overlay, kernel/patches)")
changed=$(git -C "$D" diff --name-only "$(git -C "$D" rev-parse -q --verify 'assembled^{commit}' || echo upstream)" "$commit" | wc -l)
git -C "$D" checkout -q -B hakios "$commit"
git -C "$D" tag -f assembled "$commit" > /dev/null
log "$D: $UPSTREAM_TAG + HakiOS, $changed files $([ $update = 1 ] && echo updated || echo "added or changed")"
