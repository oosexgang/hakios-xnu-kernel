#!/bin/bash
# The sources of a release, as they are published: a tarball of a commit, checked first.
#
#   scripts/release.sh VERSION [--no-check] [--tag]
#
# What it does, in order, stopping at the first that fails:
#   1. the checkout has nothing uncommitted (what is released is a commit, not a working copy)
#   2. tools/check-publishable.sh finds nothing (no binaries, firmware, Apple's files, home paths, keys)
#   3. make check passes (--no-check leaves it out, when it has just been run)
#   4. build/release/hakios-kernel-VERSION.tar.gz: `git archive` of HEAD, everything under hakios-kernel-VERSION/,
#      with .tracked-files beside it: the list of its files, which the checks of a tree that has no git go over
#      (tools/files.py), so that make check runs in the unpacked tarball as it does in a checkout
#   5. build/release/SHA256SUMS, and build/release/NOTES.md: the CHANGELOG's sections that are not released yet
#   6. --tag: the annotated tag vVERSION here (pushing it is yours to do: git push origin vVERSION)
#
# Nothing is uploaded anywhere. What a release holds is what git holds: no device package (the devices branch and
# the phones' own repositories have those), no image, no build output.
set -euo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP"
die() { echo "release.sh: $*" >&2; exit 1; }
# the tarball's name, its folder, its compression: build.config.release (RELEASE_BUILD_CONFIG: another file of the top)
HAKIOS=$TOP
export HAKIOS
# shellcheck source=../build.config.release
. "$TOP/${RELEASE_BUILD_CONFIG:-build.config.release}"
[[ $RELEASE_GZIP_LEVEL =~ ^[1-9]$ ]] || die "RELEASE_GZIP_LEVEL is 1 to 9: $RELEASE_GZIP_LEVEL"
version='' check=1 tag=''
for argument in "$@"; do
	case $argument in
	--no-check) check='' ;;
	--tag) tag=1 ;;
	-*) die "unknown option $argument (the head of this file lists them)" ;;
	*) [ -z "$version" ] || die "one VERSION"; version=$argument ;;
	esac
done
[ -n "$version" ] || die "usage: scripts/release.sh VERSION [--no-check] [--tag]"
[[ $version =~ ^[0-9A-Za-z][0-9A-Za-z.+-]*$ ]] || die "a version is letters, digits, dots, plus and minus signs: $version"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the checkout has changes that are not committed: commit them first"
! git rev-parse -q --verify "refs/tags/v$version" > /dev/null || [ -z "$tag" ] || die "the tag v$version is there already"

echo "== what must not be published (tools/check-publishable.sh)"
tools/check-publishable.sh || die "something that must not be published is in the repository"
if [ -n "$check" ]; then
	echo "== make check"
	make check > build/release-check.log 2>&1 || die "make check fails: build/release-check.log"
fi

name=$RELEASE_NAME-$version
out=$TOP/$RELEASE_DIR
mkdir -p "$out"
rm -f "$out/$name.tar.gz"
commit=$(git rev-parse HEAD)
# the list of the files, in git's order (bytes), the list itself among them; of the commit's time, as is the rest
listing=$(mktemp -d)
trap 'rm -rf "$listing"' EXIT
{ git ls-tree -r --name-only HEAD; echo .tracked-files; } | LC_ALL=C sort > "$listing/.tracked-files"
touch -d "@$(git log -1 --format=%ct)" "$listing/.tracked-files"
# (the commit's time in the tarball, not now: the same commit gives the same file)
git archive --format=tar --prefix="$name/" --add-file="$listing/.tracked-files" HEAD | gzip -n "-$RELEASE_GZIP_LEVEL" > "$out/$name.tar.gz"
(cd "$out" && sha256sum "$name.tar.gz" > SHA256SUMS)
{
	echo "# HakiOS kernel $version"
	echo
	echo "The sources at commit $commit ($(git log -1 --format=%cs))."
	echo
	# the sections of CHANGELOG.md not released yet: from the first "## Unreleased" to the first released one
	awk '/^## / { inside = ($0 ~ /^## Unreleased/) } inside' CHANGELOG.md | sed -E 's/^## Unreleased: (.)/## \U\1/'
} > "$out/NOTES.md"
echo "release.sh: $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1)), SHA256SUMS, NOTES.md"
if [ -n "$tag" ]; then
	git tag -a "v$version" -m "HakiOS kernel $version" "$commit"
	echo "release.sh: the tag v$version is made here; git push origin v$version publishes it"
fi
