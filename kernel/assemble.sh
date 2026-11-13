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
# kernel/export.sh. --update never runs over such work: it leaves the tree as it is while the
# repository has nothing new, and stops when both have changed.
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

# The tree object of what lies in the working tree now, new files included.
working_tree() {
	export GIT_INDEX_FILE=$D/.git/hakios-assemble-index
	rm -f "$GIT_INDEX_FILE"
	tree_git "$D" read-tree HEAD
	tree_git "$D" add -A .
	tree_git "$D" write-tree
	rm -f "$GIT_INDEX_FILE"
	unset GIT_INDEX_FILE
}

if [ $update = 0 ]; then
	[ ! -e "$D" ] || die "$D exists (to bring it up to date: assemble.sh --update $D)"
	log "fetching $UPSTREAM_TAG from $from"
	git init -q "$D"
	D=$(cd "$D" && pwd)
	trap 'rm -rf "$D"' ERR EXIT                     # (no half-made tree is left behind)
	git -C "$D" fetch -q --depth 1 "$from" "refs/tags/$UPSTREAM_TAG" ||
		die "cannot fetch refs/tags/$UPSTREAM_TAG from $from"
	got=$(git -C "$D" rev-parse 'FETCH_HEAD^{commit}')
	[ "$got" = "$UPSTREAM_COMMIT" ] ||
		die "$from has $UPSTREAM_TAG at $got, not at $UPSTREAM_COMMIT (kernel/UPSTREAM): not using it"
	git -C "$D" tag upstream "$got"
else
	is_assembled_tree "$D" || die "$D is not a tree made by assemble.sh"
	D=$(cd "$D" && pwd)
	[ "$(git -C "$D" rev-parse 'upstream^{commit}')" = "$UPSTREAM_COMMIT" ] ||
		die "$D is of another release than kernel/UPSTREAM names: make a new tree"
	if [ $discard = 1 ]; then
		git -C "$D" reset -q --hard assembled
		git -C "$D" clean -q -fd
	fi
fi
tree_excludes "$D"

tree=$(assembled_tree "$D")
if [ $update = 1 ]; then
	was=$(git -C "$D" rev-parse 'assembled^{tree}')
	work=$(working_tree)
	if [ "$work" = "$tree" ]; then
		# what lies there is what the repository says (work done in the tree and exported, or nothing new)
		if [ "$was" = "$tree" ] && [ -z "$(git -C "$D" status --porcelain)" ]; then
			log "$D is up to date"
			exit 0
		fi
	elif [ "$work" != "$was" ]; then
		# work in the tree that the repository has not got
		if [ "$tree" = "$was" ]; then
			log "$D has work that is not in the repository yet (kernel/export.sh $D takes it over): left as it is"
			exit 0
		fi
		die "$D has work that is not in the repository, and the repository has changed since the tree was made:
$(git -C "$D" status --short | head -20)
take the work over with kernel/export.sh $D (and settle the two in the repository), or throw it away
with assemble.sh --update --discard $D"
	fi
fi
commit=$(tree_git "$D" commit-tree "$tree" -p upstream \
	-m "$UPSTREAM_TAG + HakiOS (kernel/overlay, kernel/patches)")
changed=$(git -C "$D" diff --name-only "$(git -C "$D" rev-parse -q --verify 'assembled^{commit}' || echo upstream)" "$commit" | wc -l)
if [ $update = 1 ] && [ "$work" = "$tree" ]; then
	# the files are right already: only the tree's own history is brought in line, nothing is touched
	git -C "$D" symbolic-ref HEAD refs/heads/hakios
	git -C "$D" update-ref refs/heads/hakios "$commit"
	git -C "$D" read-tree "$commit"
else
	git -C "$D" checkout -q -B hakios "$commit"
fi
git -C "$D" tag -f assembled "$commit" > /dev/null
trap - ERR EXIT
log "$D: $UPSTREAM_TAG + HakiOS, $changed files $([ $update = 1 ] && echo updated || echo "added or changed")"
