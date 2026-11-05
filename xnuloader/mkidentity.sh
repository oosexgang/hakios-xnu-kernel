#!/bin/bash
# Make xnuloader/identity.h: the identity of the iPhone whose files Darwin userland 16.5 was built for, taken
# from the device tree in the IPSW (Firmware/all_flash/DeviceTree.d22ap.im4p) at build time, on
# the builder's machine, from their own copy of the firmware. The header is not in the repository.
#
#   sysimage/ipsw-member.py        the one file out of your IPSW (34 KB); sysimage/unpack.sh leaves it
#                                  in sysimage/ too
#   xnuloader/im4p.py              its payload: an LZFSE stream
#   lzfse(1) / compression_tool(1) / python3 -m lzfse / xnu-deps/host/bin/lzfse (built from the
#                                  BSD-licensed github.com/lzfse/lzfse when nothing else is there)
#   xnuloader/mkidentity.py        the header from the decoded tree
#
# usage: xnuloader/mkidentity.sh [DeviceTree.im4p | your.ipsw]
#                                  (with no argument: sysimage/DeviceTree.d22ap.im4p, or $IPSW)
#        xnuloader/mkidentity.sh --placeholder       (no IPSW: an identity.h with no Apple data,
#                                                     enough to build and boot the open kernel under
#                                                     QEMU with an Apple-free user space, not Darwin)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
if [ "$1" = --placeholder ] || [ "${PLACEHOLDER:-}" = 1 ]; then
	python3 "$HERE/mkidentity.py" --placeholder > "$HERE/identity.h.new"
	mv "$HERE/identity.h.new" "$HERE/identity.h"
	echo "mkidentity.sh: xnuloader/identity.h made WITHOUT an IPSW (placeholder; no Apple data, not for Darwin)"
	exit 0
fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mkidentity.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
# nothing is downloaded: the device tree comes from the IPSW you have
SRC=${1:-}
[ -n "$SRC" ] || { [ -f "$TOP/sysimage/DeviceTree.d22ap.im4p" ] && SRC=$TOP/sysimage/DeviceTree.d22ap.im4p; } || SRC=${IPSW:-}
if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
	echo "mkidentity.sh: no device tree. Run sysimage/unpack.sh YOUR.ipsw first, or give the IPSW or" >&2
	echo "  DeviceTree.d22ap.im4p as the argument (or --placeholder for the open kernel without Darwin)" >&2
	exit 1
fi
case $SRC in
*.ipsw) python3 "$TOP/sysimage/ipsw-member.py" "$SRC" Firmware/all_flash/DeviceTree.d22ap.im4p "$WORK/dt.im4p"; IM4P=$WORK/dt.im4p ;;
*) IM4P=$SRC ;;
esac
python3 "$HERE/im4p.py" "$IM4P" "$WORK/dt.lzfse"
decode() {
	if command -v lzfse >/dev/null; then lzfse -decode -i "$1" -o "$2"
	elif [ -x /usr/bin/compression_tool ]; then /usr/bin/compression_tool -decode -a lzfse -i "$1" -o "$2"
	elif python3 -c 'import lzfse' 2>/dev/null; then python3 -c 'import lzfse,sys; open(sys.argv[2],"wb").write(lzfse.decompress(open(sys.argv[1],"rb").read()))' "$1" "$2"
	elif [ -x "$TOP/xnu-deps/host/bin/lzfse" ]; then "$TOP/xnu-deps/host/bin/lzfse" -decode -i "$1" -o "$2"
	else
		echo "mkidentity.sh: no LZFSE decoder: building lzfse (BSD, github.com/lzfse/lzfse) into xnu-deps/host/bin" >&2
		git clone -q --depth 1 https://github.com/lzfse/lzfse "$WORK/lzfse" && make -s -C "$WORK/lzfse" >/dev/null &&
			mkdir -p "$TOP/xnu-deps/host/bin" && cp "$WORK/lzfse/build/bin/lzfse" "$TOP/xnu-deps/host/bin/lzfse" &&
			"$TOP/xnu-deps/host/bin/lzfse" -decode -i "$1" -o "$2"
	fi
}
decode "$WORK/dt.lzfse" "$WORK/dt.bin"
python3 "$HERE/mkidentity.py" "$WORK/dt.bin" > "$HERE/identity.h.new"
mv "$HERE/identity.h.new" "$HERE/identity.h"
echo "mkidentity.sh: xnuloader/identity.h made from the device tree ($(wc -c < "$WORK/dt.bin") bytes)"
