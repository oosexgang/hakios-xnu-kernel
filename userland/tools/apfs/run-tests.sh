#!/bin/bash
# Everything about the APFS reader that can be checked on the computer.
#
#   1. the on-disk structures in apfs_format.h against the offsets apfs.py reads at
#   2. the UDIF reader against the image's own trailer and partition table
#   3. the APFS reader against 7-Zip's, over every entry in the volume (needs ios165/)
#   4. file contents against the files 7-Zip extracted, byte for byte (needs ios165/)
#   5. the C the kernel is built from against apfs.py, over the whole volume
#   6. the hash in directory records, worked out as the C lookup does (needs the image)
#   7. every name looked up by name through the C, against its directory (needs the image)
#
# Steps 3 to 7 need the APFS partition unpacked out of ios165/rootfs.dmg; pass it as
# $1, or set APFS_IMAGE.  Without it only the first two steps run.
#
# APFS_SUBTREE=/some/path keeps step 5 to one subtree, which is the quick way to run it;
# unset, it walks the whole volume and reads every file through both readers.
#
# usage: tools/apfs/run-tests.sh [APFS-PARTITION-OR-DMG]

set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IMAGE=${1:-${APFS_IMAGE:-}}
APFS_SUBTREE=${APFS_SUBTREE:-}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# the header lives in the kernel tree; xnu/files/ has the exported copy
HDR_DIR=""
for d in "$ROOT/xnu-src/xnu-8796.121.2/bsd/miscfs/apfs" "$ROOT/xnu/files/bsd/miscfs/apfs"; do
	[ -f "$d/apfs_format.h" ] && { HDR_DIR=$d; break; }
done
if [ -z "$HDR_DIR" ]; then
	echo "run-tests.sh: apfs_format.h is in neither the kernel tree nor xnu/files" >&2
	exit 1
fi

echo "== 1. apfs_format.h ($HDR_DIR) against the offsets apfs.py reads at"
cc -std=c11 -Wall -Wextra -I"$HDR_DIR" -o "$TMP/fmt_check" "$HERE/fmt_check.c" || exit 1
"$TMP/fmt_check" > "$TMP/fmt.txt" || exit 1
python3 "$HERE/test_format.py" "$TMP/fmt.txt" || fail=1
echo

echo "== 1b. hole handling, on made-up extent lists (the image has no sparse file)"
python3 "$HERE/test_sparse.py" || fail=1
echo

echo "== 1c. the C that reads file contents without the lock, on made-up runs (holes, ends inside blocks)"
if cc -std=c11 -O2 -w -I"$HERE/host-shim" -c -o "$TMP/deflate.o" "$HDR_DIR/apfs_deflate.c" &&
	cc -std=c11 -D_DEFAULT_SOURCE -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
	-I"$HDR_DIR" -o "$TMP/test_fetch" "$HERE/test_fetch.c" "$TMP/deflate.o" -lz -pthread; then
	"$TMP/test_fetch" || fail=1
else
	echo "   would not build"
	fail=1
fi
echo

echo "== 2. the UDIF reader on ios165/rootfs.dmg"
if [ -f "$ROOT/ios165/rootfs.dmg" ]; then
	python3 "$HERE/dmg.py" info "$ROOT/ios165/rootfs.dmg" | tail -n 14 || fail=1
else
	echo "   skipped: ios165/rootfs.dmg is not here (run ios165/fetch.sh with KEEP=1)"
fi
echo

if [ -z "$IMAGE" ] || [ ! -f "$IMAGE" ]; then
	echo "== 3, 4. skipped: no APFS image given"
	echo "   unpack one with: tools/apfs/dmg.py extract ios165/rootfs.dmg OUT.apfs --apfs"
	exit $fail
fi

echo "== 3. every entry in the volume against 7-Zip's listing"
if [ -f "$ROOT/ios165/rootfs.slt" ]; then
	python3 "$HERE/verify.py" listing "$IMAGE" "$ROOT/ios165/rootfs.slt" || fail=1
else
	echo "   skipped: ios165/rootfs.slt is not here"
fi
echo

echo "== 4. file contents against what 7-Zip extracted"
if [ -d "$ROOT/ios165/rootfs" ]; then
	python3 "$HERE/verify.py" sample "$IMAGE" "$ROOT/ios165/rootfs" || fail=1
	echo "   (that is a sample of every kind; --all compares all of them)"
else
	echo "   skipped: ios165/rootfs is not here"
fi
echo

# The C in bsd/miscfs/apfs is what goes into the kernel, so it is run against the reader
# that has already been checked against 7-Zip.  SUBTREE keeps it to part of the volume;
# with no SUBTREE it walks all 318098 entries and reads every file, which takes a while
# on the Python side.
echo "== 5. the kernel's C against apfs.py${APFS_SUBTREE:+ over $APFS_SUBTREE}"
if "$HERE/build-tool.sh" "$TMP/apfs-tool" > /dev/null; then
	"$TMP/apfs-tool" census "$IMAGE" ${APFS_SUBTREE:+"$APFS_SUBTREE"} 2>/dev/null |
		sort > "$TMP/c.txt"
	python3 "$HERE/verify.py" clines "$IMAGE" ${APFS_SUBTREE:+"$APFS_SUBTREE"} |
		sort > "$TMP/py.txt"
	if diff "$TMP/c.txt" "$TMP/py.txt" > "$TMP/d.txt"; then
		echo "   $(wc -l < "$TMP/c.txt") entries, identical in both readers"
	else
		echo "   $(grep -c '^[<>]' "$TMP/d.txt") differing lines of $(wc -l < "$TMP/py.txt"):"
		head -10 "$TMP/d.txt"
		fail=1
	fi
else
	echo "   skipped: the tool would not build"
	fail=1
fi
echo

echo "== 6. the hash in directory records, as the lookup works it out"
python3 "$HERE/test_namehash.py" "$IMAGE" || fail=1
echo

echo "== 7. every name in the volume looked up by name (and with \"#\" after it, not found)"
if [ -x "$TMP/apfs-tool" ]; then
	"$TMP/apfs-tool" lookups "$IMAGE" || fail=1
else
	echo "   skipped: the tool would not build"
	fail=1
fi

exit $fail
