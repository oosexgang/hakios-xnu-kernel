#!/bin/bash
# Unpack an iPhone IPSW you have on your own disk into the layout the image builders read:
#
#   DIR/rootfs/          the system volume (BuildManifest: OS)
#   DIR/cryptex/os/      the OS cryptex, with the dyld shared cache (Cryptex1,SystemOS)
#   DIR/cryptex/app/     the App cryptex (Cryptex1,AppOS)
#   DIR/dyld             a link to the shared cache's directory in cryptex/os
#   DIR/var.skel         the template of /private/var, with owners and modes (mkvarskel.py)
#   DIR/DeviceTree.bin   the iPhone's device tree, decoded: arch/arm64/boot/build.sh takes its identity from it
#   DIR/BuildManifest.plist
#
#   darwin/tools/unpack.sh IPSW [DIR]       DIR: the directory of the IPSW
#
# Nothing of Apple's is downloaded but what decrypts the IPSW's own images: an encrypted one (.aea, iOS 18
# and later) is opened with the key Apple's server gives for it, as `ipsw fw aea` asks. Each image is
# taken out of the IPSW, decrypted, unpacked and removed again, one at a time: the largest of iOS 27 is
# 9 GB as a disk image and 15 GB of files, and the disk never holds much more than that at once.
#
# Needs: unzip, 7z (7-Zip 23 or newer: it reads APFS), ipsw (github.com/blacktop/ipsw) for encrypted
# images and the device tree, cc. 7z leaves two things undone, which this finishes: files compressed
# with LZBITMAP (kernel/deps/build/lzbitmap/unzbm.c expands them) and symbolic links that lead out of
# their directory, which it writes as empty files.
set -euo pipefail
# (and inside $(...) as well: image's path is taken that way, and a failed unzip there went by unnoticed)
shopt -s inherit_errexit
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
[ $# -ge 1 ] && [ $# -le 2 ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
IPSW=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -f "$IPSW" ] || { echo "unpack.sh: $1: no such file" >&2; exit 1; }
OUT=${2:-$(dirname "$IPSW")}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
for tool in unzip 7z ipsw cc python3; do
	command -v $tool > /dev/null || { echo "unpack.sh: $tool is not installed" >&2; exit 1; }
done
WORK=$OUT/unpack.tmp
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
UNZBM=$WORK/unzbm
cc -O2 -w -o "$UNZBM" "$REPO/kernel/deps/build/lzbitmap/unzbm.c" "$REPO/kernel/deps/build/lzbitmap/libzbitmap.c"

unzip -o -q "$IPSW" BuildManifest.plist -d "$OUT"
# NAME PATH, for the images and the device tree of the IPSW's first build identity
python3 - "$OUT/BuildManifest.plist" > "$WORK/parts" <<'PY'
import plistlib, sys
manifest = plistlib.load(open(sys.argv[1], 'rb'))
identity = manifest['BuildIdentities'][0]['Manifest']
print('version', manifest['ProductVersion'], manifest['ProductBuildVersion'])
for key, name in (('OS', 'rootfs'), ('Cryptex1,SystemOS', 'cryptex/os'), ('Cryptex1,AppOS', 'cryptex/app'),
                  ('DeviceTree', 'devicetree')):
    if key in identity:
        print(name, identity[key]['Info']['Path'])
PY
read -r _ version build < <(grep '^version ' "$WORK/parts")
echo "unpack.sh: iOS $version ($build) into $OUT"

# 7z writes a link that leads out of its directory as an empty file and says so in its log ("Dangerous link
# ... was ignored : PATH : TARGET"): those are made what they are.
links() { # links DIR LOG
	python3 - "$1" "$2" <<'PY'
import os, sys
root, made = sys.argv[1], 0
for line in open(sys.argv[2], errors='replace'):
    if 'link' not in line or 'was ignored : ' not in line:
        continue
    path, target = line.split('was ignored : ', 1)[1].rstrip('\n').split(' : ', 1)
    place = os.path.join(root, path)
    if os.path.lexists(place):
        if os.path.islink(place) or not os.path.isfile(place) or os.path.getsize(place) != 0:
            sys.exit(f'unpack.sh: {place} is not the empty file 7z leaves for a link')
        os.unlink(place)
    os.makedirs(os.path.dirname(place), exist_ok=True)
    os.symlink(target, place)
    made += 1
if made:
    print(f'unpack.sh: {made} links made in {root}')
PY
}

image() { # image PATH-IN-IPSW: the disk image out of the IPSW, decrypted, in WORK; its path on stdout
	local image
	image=$WORK/$(basename "$1")
	unzip -o -q -j "$IPSW" "$1" -d "$WORK" || return 1
	if [ "${image%.aea}" != "$image" ]; then
		(cd "$WORK" && ipsw fw aea "$image" -o "$WORK" > /dev/null 2>&1) || { echo "unpack.sh: ipsw could not decrypt $1" >&2; return 1; }
		rm "$image"
		image=${image%.aea}
	fi
	[ -s "$image" ] || { echo "unpack.sh: $1: no image came out of the IPSW" >&2; return 1; }
	echo "$image"
}

# The template of /private/var with its owners and modes, which extraction on Linux does not keep: from the
# system volume's listing (mkvarskel.py). init makes the directories from it at every boot.
# A listing without /private/var (7z could not read the image) would still give the few directories mkvarskel.py
# adds by itself, and root26-darwin.sh takes any var.skel that is not empty: that stops here instead, and the
# file is put in its place only when whole.
var_skel() { # var_skel IMAGE
	7z l -slt "$1" > "$WORK/listing" 2> /dev/null || true
	if ! grep -q '^Path = private/var/' "$WORK/listing"; then
		echo "unpack.sh: 7z lists nothing of /private/var in $1: no var.skel" >&2
		return 1
	fi
	python3 "$HERE/mkvarskel.py" "$WORK/listing" > "$OUT/var.skel.tmp"
	mv "$OUT/var.skel.tmp" "$OUT/var.skel"
	rm "$WORK/listing"
	echo "unpack.sh: var.skel: $(grep -c . "$OUT/var.skel") entries"
}

unpack() { # unpack NAME PATH-IN-IPSW
	local dir=$OUT/$1 image
	if [ -f "$dir/.done" ]; then
		echo "unpack.sh: $1: there already ($dir/.done)"
		if [ "$1" = rootfs ] && [ ! -s "$OUT/var.skel" ]; then
			image=$(image "$2")
			var_skel "$image"
			rm "$image"
		fi
		return
	fi
	rm -rf "$dir"
	echo "unpack.sh: $1: $2"
	image=$(image "$2")
	7z x -y -snl "$image" -o"$dir" > "$WORK/7z.log" 2>&1 || true
	[ "$1" = rootfs ] && var_skel "$image"
	rm "$image"
	if ! grep -q '^Everything is Ok' "$WORK/7z.log"; then
		# what 7z calls errors here are the links above and the LZBITMAP files ("Unsupported Method", finished
		# below); anything else stops the unpacking
		if grep -E '^ERROR' "$WORK/7z.log" | grep -v -e 'link .*was ignored' -e 'Unsupported Method' | grep -q .; then
			grep -E '^ERROR' "$WORK/7z.log" | grep -v -e 'link .*was ignored' -e 'Unsupported Method' | head >&2
			echo "unpack.sh: $1: 7z failed (its log: $WORK/7z.log)" >&2
			trap - EXIT
			exit 1
		fi
	fi
	find "$dir" -type f -name '*:com.apple.decmpfs' -print0 | sed -z 's/:com\.apple\.decmpfs$//' |
		xargs -0 -r -n 200 "$UNZBM"
	local left
	left=$(find "$dir" -name '*:com.apple.*' | wc -l)
	[ "$left" -eq 0 ] || { echo "unpack.sh: $1: $left files are still compressed" >&2; exit 1; }
	links "$dir" "$WORK/7z.log"
	touch "$dir/.done"
	echo "unpack.sh: $1: $(du -sh "$dir" | cut -f1)"
}

while read -r name path; do
	case "$name" in
	rootfs | cryptex/os | cryptex/app) unpack "$name" "$path" ;;
	devicetree)
		unzip -o -q -j "$IPSW" "$path" -d "$WORK"
		ipsw img4 im4p extract "$WORK/$(basename "$path")" -o "$OUT/DeviceTree.bin" > /dev/null 2>&1
		echo "unpack.sh: the device tree: $(basename "$path")" ;;
	esac
done < "$WORK/parts"
ln -sfn cryptex/os/System/Library/Caches/com.apple.dyld "$OUT/dyld"
echo "unpack.sh: done: iOS $version ($build) in $OUT"
