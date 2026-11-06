#!/bin/bash
# Check an image made by mkapfs.py with two readers that are not it (tools/apfs/check-image.py):
# apfsck -cw has to find the container clean, and the kernel's own apfs_lib (apfs-tool) has to
# read every volume back as the directory it was made of. Builds what that takes, once:
#
#   apfsck      linux-apfs/apfsprogs (GPL-2.0, not in this repository) at the commit below, cloned
#               into xnu-deps/build/apfsprogs and built there
#   apfs-tool   tools/apfs/build-tool.sh -> apfs-work/apfs-tool, when apfs_lib is newer than it
#   fnvsum      tools/apfs/fnvsum.c -> apfs-work/fnvsum (check-image.py builds it)
#
# usage: tools/apfs/check-image.sh IMAGE [VOLUME=]DIR[,manifest=FILE] ...   (the volumes as mkapfs.py had them)
#
# One container: the first one in IMAGE. Containers back to back (flashboot.apfs) are checked one by
# one before they are put together (userland/build.sh, CHECK=1).
set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
APFSPROGS=$ROOT/xnu-deps/build/apfsprogs
APFSPROGS_URL=https://github.com/linux-apfs/apfsprogs.git
APFSPROGS_COMMIT=1bd1c1c2140d13d1b6d2e03379316572bee70aa3   # "mkapfs: set INODE_NO_RSRC_FORK on the directories"

[ $# -ge 2 ] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

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
	"$ROOT"/xnu/files/bsd/miscfs/apfs/*.[ch] "$ROOT"/xnu/files/bsd/miscfs/apfs/libzbitmap/*.[ch]; do
	[ -n "$stale" ] && break
	[ "$f" -nt "$TOOL" ] && stale=1
done
[ -z "$stale" ] || "$HERE/build-tool.sh" "$TOOL" > /dev/null

exec python3 "$HERE/check-image.py" "$@"
