#!/bin/bash
# What the kernel build needs that is neither in this repository nor in theos, fetched and built into a
# directory outside the repository: Apple's open sources at pinned commits, and three host tools.
#
#   kernel/deps/stage1.sh DIR
#
#   DIR/AvailabilityVersions DIR/libdispatch DIR/libpthread     Apple open source (the kernel's firehose
#                                                               and pthread libraries are built from them)
#   DIR/MacOSX13.3.sdk          only Kernel.framework: DriverKit headers Apple's closed `iig` would generate
#   DIR/host/bin                mig and migcom (a port that builds on glibc), tcsh (XNU's doconf is a csh
#                               script), unifdef, unzbm
#
# What is there already is left alone, so it can be run again at any time. kernel/build.sh runs it by
# itself when no darwin-renoir checkout provides the same (Documentation/building.md).
# Needs: git curl tar xz make cc flex bison m4 perl autoconf automake on Linux; Xcode's tools on macOS.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ $# -eq 1 ] || { echo "usage: stage1.sh DIR" >&2; exit 1; }
mkdir -p "$1"
D=$(cd "$1" && pwd)
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)}
MACOS_SDK_URL=https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz
MACOS_SDK_SHA256=518e35eae6039b3f64e8025f4525c1c43786cc5cf39459d609852faf091e34be

log() { echo "stage1: $*"; }
die() { echo "stage1: $*" >&2; exit 1; }

missing=
for tool in git curl tar xz make cc flex bison m4 perl autoreconf automake; do
	command -v "$tool" > /dev/null || missing="$missing $tool"
done
[ -z "$missing" ] || die "not installed:$missing"

# fetch NAME URL COMMIT: one commit of a repository, without its history
fetch() {
	[ -d "$D/$1/.git" ] && [ "$(git -C "$D/$1" rev-parse -q --verify HEAD)" = "$3" ] && return 0
	log "fetching $1"
	rm -rf "${D:?}/$1"
	git init -q "$D/$1"
	git -C "$D/$1" fetch -q --depth 1 "$2" "$3" || die "cannot fetch $3 from $2"
	git -C "$D/$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

stage1_darwin() {
	local sdk tool path name
	for tool in git curl tar xz make cc clang flex bison m4 perl xcrun; do
		command -v "$tool" > /dev/null || die "not installed: $tool"
	done
	sdk=$(/usr/bin/xcrun --sdk iphoneos --show-sdk-path) || die "no iPhoneOS SDK in Xcode (xcode-select -p?)"
	[ -d "$sdk/usr/include" ] || die "no iPhoneOS SDK headers in $sdk"

	fetch AvailabilityVersions https://github.com/apple-oss-distributions/AvailabilityVersions.git \
		5d9baee345b3bc72b26c71adc65f94f2d5e9ced2        # AvailabilityVersions-115.4
	fetch libdispatch https://github.com/apple-oss-distributions/libdispatch.git \
		a641558f447659d90ad002ef0ee6f0212273fbfc        # libdispatch-1415.100.11
	fetch libpthread https://github.com/apple-oss-distributions/libpthread.git \
		a6c70ca0e544347a0377babf748f831577ec24f9        # libpthread-514.100.2
	mkdir -p "$D/host/bin"
	for tool in mig migcom unifdef; do
		path=$(/usr/bin/xcrun --sdk iphoneos --find "$tool") || die "Xcode does not provide $tool"
		[ -x "$path" ] || die "Xcode tool is missing: $path"
		ln -sfn "$path" "$D/host/bin/$tool"
	done
	for tool in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool ld; do
		path=$(/usr/bin/xcrun --sdk iphoneos --find "$tool") || die "Xcode does not provide $tool"
		[ -x "$path" ] || die "Xcode tool is missing: $path"
		name=$tool
		[ "$tool" != ld ] || name=ld64
		ln -sfn "$path" "$D/host/bin/$name"
	done
	ln -sfn /bin/tcsh "$D/host/bin/tcsh"
	ln -sfn /bin/csh "$D/host/bin/csh"
	if [ ! -x "$D/host/bin/unzbm" ]; then
		cc -O2 -w -o "$D/host/bin/unzbm" "$HERE/build/lzbitmap/unzbm.c" "$HERE/build/lzbitmap/libzbitmap.c"
	fi
	log "done: $D (macOS, iPhoneOS SDK: $sdk)"
}

if [ "$(uname -s)" = Darwin ]; then
	stage1_darwin
	exit 0
fi

fetch AvailabilityVersions https://github.com/apple-oss-distributions/AvailabilityVersions.git \
	5d9baee345b3bc72b26c71adc65f94f2d5e9ced2        # AvailabilityVersions-115.4
fetch libdispatch https://github.com/apple-oss-distributions/libdispatch.git \
	a641558f447659d90ad002ef0ee6f0212273fbfc        # libdispatch-1415.100.11
fetch libpthread https://github.com/apple-oss-distributions/libpthread.git \
	a6c70ca0e544347a0377babf748f831577ec24f9        # libpthread-514.100.2
fetch mm_bootstrap_cmds https://github.com/markmentovai/bootstrap_cmds \
	f337e600741638896fb6056ce87c6479f74b89a3        # cross_platform/bootstrap_cmds-138
fetch tcsh https://github.com/tcsh-org/tcsh.git b7db07931546178c4cb2542b5fc850c483628ed5
fetch unifdef https://github.com/fanf2/unifdef.git 08d12cd3059fe072929ea001ad9833f5487fd355

if [ ! -d "$D/MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers/DriverKit" ]; then
	log "downloading the macOS 13.3 SDK for its DriverKit headers"
	curl -L --fail --retry 3 -s -o "$D/macos-sdk.tar.xz" "$MACOS_SDK_URL" || die "download failed: $MACOS_SDK_URL"
	echo "$MACOS_SDK_SHA256  $D/macos-sdk.tar.xz" | sha256sum -c --quiet ||
		die "$MACOS_SDK_URL is not the archive this was written with (its SHA-256 differs): not unpacked"
	tar -C "$D" -xJf "$D/macos-sdk.tar.xz" --wildcards '*MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework*' ||
		die "cannot unpack the macOS SDK"
	rm -f "$D/macos-sdk.tar.xz"
fi

mkdir -p "$D/host/bin"
# mig: Apple's own does not build on glibc; this is a port that does
if [ ! -x "$D/host/bin/migcom" ] || [ ! -x "$D/host/bin/mig" ]; then
	log "building mig"
	(cd "$D/mm_bootstrap_cmds" && autoreconf --install && sh configure --prefix="$D/host" &&
		make -j"$JOBS" && make install) > "$D/mig-build.log" 2>&1 || die "mig did not build, see $D/mig-build.log"
fi
if [ ! -x "$D/host/bin/unifdef" ]; then
	log "building unifdef"
	# its makefile takes the version from the git history; any text will do
	[ -f "$D/unifdef/version.h" ] ||
		printf '"@(#) $Version: unifdef-2.12 $\\n"\n"@(#) $Date: 2020-11-06 11:22:51 +0000 $\\n"\n' > "$D/unifdef/version.h"
	# gnu11: "constexpr", a variable name in unifdef.c, is a keyword in C23
	cc -std=gnu11 -O2 -w -o "$D/host/bin/unifdef" "$D/unifdef/unifdef.c"
fi
# csh: XNU's SETUP/config/doconf is a csh script
if [ ! -x "$D/host/bin/tcsh" ]; then
	log "building tcsh"
	(cd "$D/tcsh" && ./configure --prefix="$D/host" && make -j"$JOBS") > "$D/tcsh-build.log" 2>&1 ||
		die "tcsh did not build, see $D/tcsh-build.log"
	cp "$D/tcsh/tcsh" "$D/host/bin/tcsh"
fi
# what 7z leaves undone when it extracts files of the Darwin images (deps/build/lzbitmap/unzbm.c)
if [ ! -x "$D/host/bin/unzbm" ]; then
	cc -O2 -w -o "$D/host/bin/unzbm" "$HERE/build/lzbitmap/unzbm.c" "$HERE/build/lzbitmap/libzbitmap.c"
fi
log "done: $D"
