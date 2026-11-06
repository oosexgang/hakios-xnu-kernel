#!/bin/bash
# Get the files of iOS 16.5 (20F66, iPhone X) that the system image is made of, straight from
# Apple's servers, and lay them out the way ../userland/build.sh expects them:
#
#   rootfs/                  the chosen part of the root filesystem (see select-system.py)
#   system.files/.manifest   what was chosen, with owners, modes and symbolic links
#   var.skel                 the skeleton of /private/var (see mkvarskel.py)
#   cryptex/                 the OS cryptex without the dyld shared cache
#   cryptex-app.dmg          the App cryptex as Apple ships it (10 MB, an APFS container: Safari's
#                            daemons, its settings and the bundles around it, which the root volume's
#                            launchd jobs and links reach through /System/Cryptexes/App)
#   cryptex-app/             ... and its files
#   20F66__iPhone10,3_6/     the dyld shared cache (arm64)
#
# Nothing of this is in the repository: the two disk images are fetched out of the IPSW on
# Apple's CDN (fetch-from-ipsw.py reads single members of the archive, 4.7 GB + 2.7 GB instead of
# all 6.2 GB) and unpacked with 7z. Needs: python3, 7z (7-Zip 23 or newer: it reads APFS), cc.
# About 8 GB of disk while it runs, 5.3 GB afterwards.
#
#   KEEP=1   keep the downloaded disk images (rootfs.dmg is needed again only after changing
#            select-system.py; with it in place a second run downloads nothing)
set -e
cd "$(dirname "$0")"
ROOTFS_DMG=096-04088-077.dmg                    # the root filesystem, APFS
CRYPTEX_DMG=096-04666-078.dmg                   # the OS cryptex: dyld shared cache and a few libraries
APP_CRYPTEX_DMG=096-03536-083.dmg               # the App cryptex (BuildManifest: Cryptex1,AppOS)
CACHE=20F66__iPhone10,3_6
UNZBM=../xnu-deps/host/bin/unzbm

for tool in python3 7z cc; do
	command -v $tool >/dev/null || { echo "fetch.sh: $tool is not installed"; exit 1; }
done
# 7z leaves files compressed with LZBITMAP as an empty file and two attribute files; unzbm finishes them
if [ ! -x $UNZBM ]; then
	mkdir -p "$(dirname $UNZBM)"
	cc -O2 -w -o $UNZBM ../xnu-deps/build/lzbitmap/unzbm.c ../xnu-deps/build/lzbitmap/libzbitmap.c
fi
unzbm_all() { # unzbm_all DIR: expand every LZBITMAP file below DIR
	find "$1" -type f -name '*:com.apple.decmpfs' | sed 's/:com\.apple\.decmpfs$//' | tr '\n' '\0' | xargs -0 -r -n 200 $UNZBM
	local left
	left=$(find "$1" -name '*:com.apple.*' | wc -l)
	[ "$left" -eq 0 ] || { echo "fetch.sh: $left files in $1 are still compressed"; exit 1; }
}

# --- the root filesystem --------------------------------------------------------------------------
if [ ! -f .done-rootfs ] || [ select-system.py -nt .done-rootfs ]; then
	if [ ! -f rootfs.dmg ]; then
		echo "fetch.sh: downloading the root filesystem image (4.7 GB)"
		python3 fetch-from-ipsw.py $ROOTFS_DMG rootfs.dmg.part
		mv rootfs.dmg.part rootfs.dmg
	fi
	echo "fetch.sh: listing the image"
	7z l -slt rootfs.dmg > rootfs.slt
	python3 select-system.py rootfs.slt system.files system.manifest
	python3 mkvarskel.py rootfs.slt > var.skel
	echo "fetch.sh: extracting $(wc -l < system.files) files"
	rm -rf rootfs
	7z x -y -orootfs rootfs.dmg @system.files > extract-system.log
	# the two fonts too big for the image as they are, made smaller in place (see select-system.py)
	python3 ../tools/font_trim.py emoji rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc \
		rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc 40,64,96
	python3 ../tools/font_trim.py faces rootfs/System/Library/Fonts/LanguageSupport/PingFang.ttc \
		rootfs/System/Library/Fonts/LanguageSupport/PingFang.ttc '^\.?PingFang(SC|TC|HK)-(Regular|Semibold)$'
	touch .done-rootfs
	[ -n "$KEEP" ] || rm -f rootfs.dmg
fi

# --- the OS cryptex -------------------------------------------------------------------------------
if [ ! -f .done-cryptex ]; then
	if [ ! -f cryptex-os.dmg ]; then
		echo "fetch.sh: downloading the OS cryptex image (2.7 GB)"
		python3 fetch-from-ipsw.py $CRYPTEX_DMG cryptex-os.dmg.part
		mv cryptex-os.dmg.part cryptex-os.dmg
	fi
	echo "fetch.sh: extracting the cryptex"
	rm -rf cryptex cryptex.cache $CACHE
	# 7z reports every LZBITMAP file as an error ("Unsupported Method"): expected, unzbm_all checks the result
	7z x -y -ocryptex cryptex-os.dmg '-xr!com.apple.dyld' '-x!System/DriverKit' > extract-cryptex.log 2>&1 || true
	unzbm_all cryptex
	# the dyld shared cache; its .symbols file (0.6 GB) is never mapped and is left out
	7z x -y -ocryptex.cache cryptex-os.dmg System/Library/Caches/com.apple.dyld '-xr!*.symbols*' > extract-dyld.log 2>&1 || true
	unzbm_all cryptex.cache
	mv cryptex.cache/System/Library/Caches/com.apple.dyld $CACHE
	rm -rf cryptex.cache
	[ "$(ls $CACHE | wc -l)" -eq 43 ] || { echo "fetch.sh: the dyld shared cache is incomplete"; exit 1; }
	touch .done-cryptex
	[ -n "$KEEP" ] || rm -f cryptex-os.dmg
fi

# --- the App cryptex ------------------------------------------------------------------------------
# kept as it is (the boot off the flash puts it in memory unchanged, ../userland/build.sh FLASHBOOT=1)
# and unpacked (the system image has its files at /System/Cryptexes/App)
if [ ! -f .done-cryptex-app ]; then
	if [ ! -f cryptex-app.dmg ]; then
		echo "fetch.sh: downloading the App cryptex image (10 MB)"
		python3 fetch-from-ipsw.py $APP_CRYPTEX_DMG cryptex-app.dmg.part
		mv cryptex-app.dmg.part cryptex-app.dmg
	fi
	echo "fetch.sh: extracting the App cryptex"
	rm -rf cryptex-app
	7z x -y -ocryptex-app cryptex-app.dmg > extract-cryptex-app.log 2>&1 || true
	unzbm_all cryptex-app
	[ -f cryptex-app/usr/libexec/webbookmarksd ] || { echo "fetch.sh: the App cryptex is incomplete"; exit 1; }
	touch .done-cryptex-app
fi
echo "fetch.sh: done: $(du -sh rootfs | cut -f1) rootfs, $(du -sh cryptex | cut -f1) cryptex, $(du -sh cryptex-app | cut -f1) App cryptex, $(du -sh $CACHE | cut -f1) dyld shared cache"
