#!/bin/bash
# The image builder and the kernel's APFS reader, against each other: tools/apfs/mkapfs.py makes containers
# of made-up files the way the port's images are made (two volumes; compressed files, firmware under
# --plain left as it is), and the kernel's own reader (bsd/miscfs/apfs/apfs_lib.c, as apfs-tool) has to read
# every entry back as it was (tools/apfs/check-image.py); then the drivers' lookup of firmware in the RAM
# disks (board/image.c) runs on them (workbench/image_file_test.c). Before that, the steps of the APFS suite
# (tools/apfs/run-tests.sh) that need no image of Darwin. apfsck, the third reader check-image.sh
# adds, is not built here: it is fetched from elsewhere.
#
#   workbench/apfs_image_test.sh
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
APFS=$HERE/../tools/apfs
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# (the reader decompresses with zlib and checks sealed volumes with OpenSSL's SHA-256: their headers are
# zlib1g-dev and libssl-dev; without them this is left out, as the aarch64 runs are without qemu)
if ! printf '#include <zlib.h>\n#include <openssl/sha.h>\n' | cc -E -x c - > /dev/null 2>&1; then
	echo "== (no zlib or OpenSSL headers: the APFS images are not checked)"
	exit 0
fi

"$APFS/build-tool.sh" "$WORK/apfs-tool" > /dev/null 2>&1 || { echo "FAIL: apfs-tool does not build"; exit 1; }
cc -O2 -o "$WORK/fnvsum" "$APFS/fnvsum.c"

# what an image of the port has: firmware stored as it is, text that compresses, a file in its attribute,
# a file in a directory below, an empty one, a symbolic link; and a second volume
in=$WORK/in
mkdir -p "$in/sys/fw" "$in/sys/etc/deep/er" "$in/data" "$in/md0/fw"
python3 - "$in" <<'PY'
import random, sys
root = sys.argv[1]
r = random.Random(12377)
open(f'{root}/sys/fw/a660_zap.mbn', 'wb').write(bytes(r.randrange(256) for _ in range(300000)))
open(f'{root}/sys/etc/big.txt', 'w').write('a line of text that compresses well\n' * 25000)
open(f'{root}/sys/etc/small', 'w').write('small\n')
open(f'{root}/sys/etc/deep/er/random.bin', 'wb').write(bytes(r.randrange(256) for _ in range(70000)))
open(f'{root}/sys/etc/empty', 'w').close()
open(f'{root}/data/x', 'w').write('data\n')
open(f'{root}/md0/fw/a660_zap.mbn', 'wb').write(bytes(r.randrange(256) for _ in range(5000)))
open(f'{root}/md0/fw/only0.bin', 'wb').write(bytes(r.randrange(256) for _ in range(9000)))
PY
ln -s ../etc/small "$in/sys/fw/link"

failed=0
# the APFS suite's own steps that need no image of Darwin: the on-disk layout against apfs.py, holes in files,
# reading contents without the lock (the steps that need an IPSW's image say they were skipped)
if "$APFS/run-tests.sh" > "$WORK/suite.txt" 2>&1; then
	grep -E "^OK|cases, [0-9]+ failed" "$WORK/suite.txt"
else
	grep -E "^==|FAIL|failed|not build|^  [^o]" "$WORK/suite.txt"
	failed=1
fi
python3 "$APFS/mkapfs.py" -o "$WORK/md1.img" -z --plain fw sys="$in/sys" data="$in/data" > /dev/null
python3 "$APFS/mkapfs.py" -o "$WORK/md0.img" -z --plain fw md0="$in/md0" > /dev/null
check() {
	APFSCK='' APFS_TOOL=$WORK/apfs-tool APFS_FNVSUM=$WORK/fnvsum python3 "$APFS/check-image.py" "$@" |
		grep -v "apfsck: not run" | sed "s|$WORK/||g"
}
check "$WORK/md1.img" "$in/sys" "$in/data" || failed=1
check "$WORK/md0.img" "$in/md0" || failed=1

# (the kernel's files at their paths in its tree, tools/kernel-view.sh: the reader includes <miscfs/apfs/apfs.h>)
"$HERE/../tools/kernel-view.sh" "$WORK/kernel"
SRC=$WORK/kernel/bsd
A=$SRC/miscfs/apfs
cc -std=c11 -O2 -w -I"$APFS/host-shim" -c -o "$WORK/deflate.o" "$A/apfs_deflate.c"
if cc -std=gnu11 -D_DEFAULT_SOURCE -O1 -Wall -Wextra -Wno-unused-parameter -pthread -I"$HERE/fakekernel" -I"$SRC/dev" -I"$SRC" \
	"$HERE/fakekernel/fakekernel.c" "$HERE/image_file_test.c" "$A/apfs_lib.c" "$A/apfs_zbitmap.c" "$WORK/deflate.o" \
	-lz -lcrypto -o "$WORK/image_file_test"; then
	"$WORK/image_file_test" "$WORK/md1.img" "$WORK/md0.img" "$in/sys" || failed=1
else
	echo "FAIL: image_file_test does not build"
	failed=1
fi
exit $failed
