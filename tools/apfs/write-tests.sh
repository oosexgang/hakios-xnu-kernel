#!/bin/bash
# Judge apfs_write.c -- the kernel's writing code -- on images on this machine, where a ruined
# image costs nothing and the phone's flash is not involved at all.  Builds what that takes:
#
#   apfs-tool   tools/apfs/build-tool.sh -> apfs-work/apfs-tool, the kernel's own apfs_lib.c and
#               apfs_write.c as a program here; the one thing under test
#   apfsck      linux-apfs/apfsprogs (GPL-2.0, not in this repository) at the commit
#               check-image.sh pins, cloned into xnu-deps/build/apfsprogs and built there: the
#               reader that has nothing to do with us and checks the space manager's bitmaps
#               against every block anything refers to
#   7-Zip       a third APFS reader, already needed by ios165/fetch.sh
#
# usage: tools/apfs/write-tests.sh [-k] [TEST ...]       (-k: keep the images in apfs-work)
# The tests: baseline writes refusals volume-gates extent-gates power-failure real-images
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
APFSPROGS=$ROOT/xnu-deps/build/apfsprogs
APFSPROGS_URL=https://github.com/linux-apfs/apfsprogs.git
APFSPROGS_COMMIT=1bd1c1c2140d13d1b6d2e03379316572bee70aa3   # the one check-image.sh pins

if [ ! -x "$APFSPROGS/apfsck/apfsck" ]; then
	if [ ! -d "$APFSPROGS/.git" ]; then
		mkdir -p "$(dirname "$APFSPROGS")"
		git clone -q "$APFSPROGS_URL" "$APFSPROGS"
	fi
	git -C "$APFSPROGS" checkout -q "$APFSPROGS_COMMIT"
	make -C "$APFSPROGS/apfsck" --silent --no-print-directory > /dev/null
fi

TOOL=$ROOT/apfs-work/apfs-tool
stale=""
[ -x "$TOOL" ] || stale=1
for f in "$HERE/apfs-tool.c" "$HERE/build-tool.sh" \
	"$ROOT"/xnu-src/xnu-8796.121.2/bsd/miscfs/apfs/*.[ch] \
	"$ROOT"/xnu/files/bsd/miscfs/apfs/*.[ch]; do
	[ -n "$stale" ] && break
	[ -e "$f" ] && [ "$f" -nt "$TOOL" ] && stale=1
done
[ -z "$stale" ] || "$HERE/build-tool.sh" "$TOOL" > /dev/null

exec python3 "$HERE/write-tests.py" "$@"
