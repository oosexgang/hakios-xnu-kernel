#!/bin/bash
# Build the kernel on Linux or macOS: Apple's release (kernel/UPSTREAM) with kernel/overlay and kernel/patches,
# as plain arm64 (no PAC), MACHINE=BCM2837, DEVELOPMENT. One kernel serves every board: which one it runs
# on it learns at boot (/chosen/darwin-board; arch/arm64/boot/dts/).
#
#   kernel/build.sh [make args]      e.g. build.sh -j12 (default: available logical CPUs)
#
# From scratch it makes, in order (each step is skipped when its result is already there):
#   1. $XNU_SRC (default: next to the repository, named after the release): kernel/assemble.sh; a tree
#      that is there already is brought up to date with the repository (only changed files are touched)
#   2. $XNUDEPS ($XNU_SRC-deps): Apple's open sources and the host tools (kernel/deps/stage1.sh fetches
#      and builds or links them, unless a darwin-renoir checkout has them), kernel/deps' wrappers, its own
#      SDK (links to Xcode's SDK on macOS or theos' iPhoneOS SDK on Linux, plus usr/local)
#   3. the kernel's exported headers, then libfirehose_kernel.a and libpthread_kext.a built against them
#   4. the kernel: $XNU_SRC/BUILD/obj/mach.development.bcm2837
# Needs an iPhoneOS SDK and Mach-O tools: Xcode on macOS, or theos/LLVM on Linux; see Documentation/building.md.
set -e -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"         # (focaltech_stub)
K=$HAKIOS_KERNEL
BD=$K/deps
HOST_OS=$(uname -s)
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)}
log() { echo "build.sh: $*"; }
die() { echo "build.sh: $*" >&2; exit 1; }
link() { [ "$(readlink "$2" 2>/dev/null)" = "$1" ] || ln -sfn "$1" "$2"; }
update() { cat > "$1.new"; if cmp -s "$1.new" "$1"; then rm "$1.new"; else mv "$1.new" "$1"; fi; }

if [ "$HOST_OS" = Darwin ]; then
	PYTHON3=${PYTHON3:-$(command -v python3.12 || command -v python3)}
	"$PYTHON3" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 12) else 1)' ||
		die "Python 3.12 or newer is needed (install Homebrew python@3.12)"
	mkdir -p "$XNUDEPS/host/bin"
	ln -sfn "$PYTHON3" "$XNUDEPS/host/bin/python3"
	export PATH="$XNUDEPS/host/bin:$PATH"
	if [ -z "${XNU_CLANG_DIR:-}" ]; then
		if [ "${TOOLCHAIN:-}" = llvm ]; then
			XNU_CLANG_DIR=$(brew --prefix llvm 2>/dev/null)/bin
		else
			XNU_CLANG_DIR=$(dirname "$(/usr/bin/xcrun --find clang)")
		fi
	fi
	DARWIN_SDK=${DARWIN_SDK:-$(/usr/bin/xcrun --sdk iphoneos --show-sdk-path)}
	TOOLCHAIN=${TOOLCHAIN:-apple}
else
	export XNU_CLANG_DIR=${XNU_CLANG_DIR:-/usr/bin}
fi

# --- 1. the tree ------------------------------------------------------------------------------
if [ ! -e "$XNU_SRC" ]; then
	"$K/assemble.sh" "$XNU_SRC"
elif git -C "$XNU_SRC" rev-parse -q --verify refs/tags/upstream > /dev/null 2>&1; then
	"$K/assemble.sh" --update "$XNU_SRC"
else
	die "$XNU_SRC is not a tree made by kernel/assemble.sh (one of the old apply.sh, perhaps): move it away or set XNU_SRC"
fi
# A FocalTech controller's firmware is not ours to publish (tools/mk-focaltech-fw.py makes it from a phone):
# an empty one keeps board/goodix.c compiling; a board without such a controller needs none
FW=$XNU_SRC/bsd/dev/board/focaltech_fw.h
if [ ! -f "$FW" ]; then
	log "stub $FW (no FocalTech firmware)"
	focaltech_stub > "$FW"
fi
# the board table: a row for every device package that is there now (arch/arm64/boot/dts/ of the repository and
# HAKIOS_DEVICES). Made anew each build and put in the tree only when it differs, so that a build with
# the same boards recompiles nothing for it.
mkdir -p "$XNUDEPS/boards"
"$HAKIOS/tools/gen_headers.py" --out "$XNUDEPS/boards" | sed 's/^/build.sh: /'
update "$XNU_SRC/bsd/dev/board/board_table.h" < "$XNUDEPS/boards/board_table.h"
# ... and the kernel's configuration, of KCONFIG_FRAGMENTS (build.config.arm64): which drivers are started
update "$XNU_SRC/bsd/dev/board/kconfig.h" < "$XNUDEPS/boards/kconfig.h"

# --- 2. the dependency directory --------------------------------------------------------------
# Apple's open sources and the host tools (mig, tcsh, unifdef): a darwin-renoir checkout has them
# fetched and built already; without one they are fetched and built once, beside the tree
RD=$DARWIN_RENOIR/xnu-deps
if [ ! -x "$RD/host/bin/migcom" ] || [ ! -x "$RD/host/bin/tcsh" ] || [ ! -d "$RD/libdispatch" ]; then
	RD=$XNUDEPS/stage1
	"$BD/stage1.sh" "$RD"
fi
# The Mach-O tools: Xcode's on macOS (TOOLCHAIN=apple), theos' cctools (TOOLCHAIN=cctools), or LLVM's beside
# the clang the kernel is compiled with (TOOLCHAIN=llvm: ld64.lld and its fellows in XNU_CLANG_DIR). Linux
# defaults to theos when present, else LLVM. DARWIN_SDK selects the SDK.
CCTOOLS=$THEOS/toolchain/linux/iphone/bin
if [ "$HOST_OS" != Darwin ]; then
	DARWIN_SDK=${DARWIN_SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}
fi
if [ -z "${TOOLCHAIN:-}" ]; then
	if [ -x "$CCTOOLS/ld" ]; then
		TOOLCHAIN=cctools
	elif [ -x "$XNU_CLANG_DIR/ld64.lld" ]; then
		TOOLCHAIN=llvm
	fi
fi
[ -d "$DARWIN_SDK/usr/include" ] || die "no iPhoneOS16.5.sdk at $DARWIN_SDK (theos' sdks/, or DARWIN_SDK=DIR)"
mkdir -p "$XNUDEPS/host/bin" "$XNUDEPS/build/firehose" "$XNUDEPS/build/pthread" "$XNUDEPS/sdk/Platforms/iPhoneOS.platform"
cd "$XNUDEPS"
for d in AvailabilityVersions libdispatch libpthread; do link "$RD/$d" "$d"; done
if [ "$HOST_OS" != Darwin ]; then link "$RD/MacOSX13.3.sdk" MacOSX13.3.sdk; fi
link "$BD/hostinc" hostinc
# iig implementation stand-ins: kernel/deps', plus kernel/iig-impl for classes new in xnu-12377
[ -L iig-impl ] && rm iig-impl
mkdir -p iig-impl
for f in "$BD"/iig-impl/* "$K"/iig-impl/*; do link "$f" "iig-impl/$(basename "$f")"; done
# built on this machine (stage 1)
for t in mig migcom tcsh unifdef unzbm; do link "$RD/host/bin/$t" host/bin/$t; done
# GNU awk as awk: the syscall table's generator (bsd/kern/makesyscalls.sh) writes "\(" in a string, which
# mawk -- Debian's and Ubuntu's awk -- keeps as it is, and init_sysent.c then does not compile
GAWK=$(command -v gawk) || die "GNU awk (gawk) is needed: the kernel's syscall table is generated with it"
link "$GAWK" host/bin/awk
link tcsh host/bin/csh
# kernel/deps' wrappers, and the iig and clang wrappers of this release (kernel/host: xnu-12377's DriverKit
# headers, and the Apple-only flags xnu-12377 adds)
for t in codesign install plutil sw_vers xcrun; do link "$BD/host/bin/$t" host/bin/$t; done
chmod +x "$K/host/xnu-clang" "$K/host/iig"
link "$K/host/iig" host/bin/iig
link "$K/host/xnu-clang" host/bin/xnu-clang
link xnu-clang host/bin/xnu-clang++
# the clang it runs, kept for what is built with it later (darwin/userland: the libraries need the same
# clang, and a newer one than Ubuntu's /usr/bin/clang for <ptrauth.h>); XNU_CLANG_DIR then still wins
echo "$XNU_CLANG_DIR" > host/clang-dir
# the iig output of xnu-12377.121 (what Apple's closed iig would generate for the kernel), as the
# macOS 26.5 SDK ships it: only Kernel.framework is kept, outside the repository
MACOS26=MacOSX26.5.sdk
if [ ! -d $MACOS26/System/Library/Frameworks/Kernel.framework/Headers/DriverKit ]; then
	log "downloading the macOS 26.5 SDK for its DriverKit headers"
	U=https://github.com/socheatsok78/macos-sdks/releases/download/rolling/$MACOS26.tar.xz
	curl -L --fail --retry 3 -s -o macos26.tar.xz $U || die "download failed: $U"
	if [ "$HOST_OS" = Darwin ]; then
		echo "e3dfe99521e8587b1eb820c5d8e21fa3721e629c8fe61e9ea4a89315ae249190  macos26.tar.xz" | shasum -a 256 -c - > /dev/null ||
			die "$U: unexpected checksum"
		tar -xJf macos26.tar.xz "*$MACOS26/System/Library/Frameworks/Kernel.framework*" ||
			die "cannot unpack $U"
	else
		echo "e3dfe99521e8587b1eb820c5d8e21fa3721e629c8fe61e9ea4a89315ae249190  macos26.tar.xz" | sha256sum -c --quiet ||
			die "$U: unexpected checksum"
		tar -xJf macos26.tar.xz --wildcards "*$MACOS26/System/Library/Frameworks/Kernel.framework*" ||
			die "cannot unpack $U"
	fi
	rm -f macos26.tar.xz
fi
case ${TOOLCHAIN:-} in
apple)
	[ "$HOST_OS" = Darwin ] || die "TOOLCHAIN=apple is only available on macOS"
	log "Mach-O tools: Xcode's, from the iPhoneOS SDK"
	for t in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
		tool=$(/usr/bin/xcrun --sdk iphoneos --find "$t") || die "Xcode does not provide $t"
		link "$tool" "host/bin/$t"
	done
	tool=$(/usr/bin/xcrun --sdk iphoneos --find ld) || die "Xcode does not provide ld"
	link "$tool" host/bin/ld64
	;;
cctools)
	[ -x "$CCTOOLS/ld" ] || die "TOOLCHAIN=cctools: no theos toolchain in $THEOS"
	for t in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
		link "$CCTOOLS/$t" host/bin/$t
	done
	link "$CCTOOLS/ld" host/bin/ld64
	;;
llvm)
	[ -x "$XNU_CLANG_DIR/ld64.lld" ] || die "TOOLCHAIN=llvm: no ld64.lld in $XNU_CLANG_DIR (XNU_CLANG_DIR=DIR)"
	log "Mach-O tools: LLVM's, in $XNU_CLANG_DIR"
	log "  (ld64.lld does not implement -image_base, -segaddr and -segment_order: the kernel shows that everything"
	log "   compiles and links, but it is not laid out as cctools' ld64 lays it out. Do not boot it.)"
	for t in nm otool strip lipo install-name-tool; do
		[ -x "$XNU_CLANG_DIR/llvm-$t" ] || die "TOOLCHAIN=llvm: no llvm-$t in $XNU_CLANG_DIR"
		link "$XNU_CLANG_DIR/llvm-$t" "host/bin/${t//-/_}"
	done
	link "$K/host/llvm/libtool" host/bin/libtool
	link "$K/host/llvm/ld" host/bin/ld64
	for t in nmedit ctf_insert codesign_allocate; do link "$K/host/llvm/unused" host/bin/$t; done
	;;
*)
	die "no Mach-O linker: theos' toolchain in $THEOS, or LLVM's (ld64.lld) in XNU_CLANG_DIR; see Documentation/building.md"
	;;
esac
if [ ! -d "$XNU_SDK_PATH/usr/include" ]; then
	log "making the SDK: links to $DARWIN_SDK"
	rm -rf "$XNU_SDK_PATH"
	if [ "$HOST_OS" = Darwin ]; then
		mkdir -p "$XNU_SDK_PATH/usr"
		for f in "$DARWIN_SDK"/*; do
			[ "$(basename "$f")" = usr ] || ln -s "$f" "$XNU_SDK_PATH/"
		done
		for f in "$DARWIN_SDK"/usr/*; do
			[ "$(basename "$f")" = local ] || ln -s "$f" "$XNU_SDK_PATH/usr/"
		done
	else
		cp -rs "$(cd "$DARWIN_SDK" && pwd)" "$XNU_SDK_PATH"
	fi
fi
SL=$XNU_SDK_PATH/usr/local
mkdir -p $SL/include/kernel/os $SL/include/os $SL/lib/kernel $SL/libexec
update $SL/libexec/availability.pl < AvailabilityVersions/availability.pl
chmod +x $SL/libexec/availability.pl
{ host/bin/unifdef -DOS_FIREHOSE_SPI=1 -DKERNEL=1 libdispatch/os/firehose_buffer_private.h || [ $? -eq 1 ]; } |
	update $SL/include/kernel/os/firehose_buffer_private.h
update $SL/include/kernel/os/firehose_types.defs < libdispatch/src/firehose/firehose_types.defs

# make, with what the old workspace's xnu-build.sh passes. DEFAULT_TARGET=build
# (as darwin-renoir/xnu-build.sh): the kernel, not the kext SDK's symbol sets (config/), whose
# kextsymboltool check fails on the exports a BCM2837 kernel does not have. xnu-12377's MakeInc.cmd
# names /usr/bin/plutil, so the no-op wrapper is passed as well.
# The Wi-Fi driver (Linux's ath11k on kernel/lx), when kernel/lx/build.sh has made it: an object linked into
# the kernel. Without it the kernel has no radio, and wifi=1 says so.
WIFI_OBJECT=$XNUDEPS/build/ath11k/out/ath11k.o
[ -f "$WIFI_OBJECT" ] || WIFI_OBJECT=
# (make does not know the kernel depends on it: an ath11k.o newer than the linked kernel would not be in it, the old
# driver linked again from the link's leftovers. Those go, and the kernel is linked anew with it.)
OBJECT=mach.$(printf '%s' "$KERNEL_CONFIG" | tr '[:upper:]' '[:lower:]').$(printf '%s' "$MACHINE_CONFIG" | tr '[:upper:]' '[:lower:]') # (mach.development.bcm2837)
LINKED=$XNU_SRC/BUILD/obj/${KERNEL_CONFIG}_${ARCH_CONFIG}_${MACHINE_CONFIG}/$OBJECT
if [ -n "$WIFI_OBJECT" ] && [ -e "$LINKED" ] && [ "$WIFI_OBJECT" -nt "$LINKED" ]; then
	rm -rf "$LINKED"*
fi
# The date in the kernel's version string, which config/newvers.pl takes from `date`: said in English and in UTC, so that
# the kernel can read it back as the earliest its calendar can be (drivers/iokit/build_date.h). (version.c is made once in
# a build directory and kept while nothing it is made from changes: an older date, which is still a floor.)
KERNEL_BUILD_DATE=${KERNEL_BUILD_DATE:-$(LC_ALL=C date -u)}
export KERNEL_BUILD_DATE
xnumake() {
	HOST_CC="clang -include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration"
	(cd "$XNU_SRC" && make DEFAULT_TARGET=build HAKIOS_OBJECTS="$WIFI_OBJECT" SDKROOT=iphoneos ARCH_CONFIGS="$ARCH_CONFIG" MACHINE_CONFIGS="$MACHINE_CONFIG" \
		KERNEL_CONFIGS="$KERNEL_CONFIG" ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=$XNU_ARCH RC_DARWIN_KERNEL_VERSION=${UPSTREAM_TAG#xnu-} \
		BUILD_WERROR="$BUILD_WERROR" BUILD_LTO="$BUILD_LTO" DO_CTFMERGE=0 BUILD_DSYM=0 PLUTIL=plutil \
		HOST_CC="$HOST_CC" \
		HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n "$@")
}

# objects of arm64 and arm64e builds share BUILD/obj (the old workspace did both): start over on a change
if [ -f "$XNU_SRC/BUILD/obj/hakios-arch" ] && [ "$(cat "$XNU_SRC/BUILD/obj/hakios-arch")" != "$XNU_ARCH" ]; then
	log "architecture changed: discarding BUILD/obj"
	rm -rf "$XNU_SRC/BUILD/obj"
fi
mkdir -p "$XNU_SRC/BUILD/obj" && echo "$XNU_ARCH" > "$XNU_SRC/BUILD/obj/hakios-arch"

# --- 3. the kernel's own static libraries, against this tree's headers -------------------------
LIB=$SL/lib/kernel
want="$XNU_SRC $XNU_ARCH"
if [ ! -f $LIB/libfirehose_kernel.a ] || [ ! -f $LIB/libpthread_kext.a ] || [ "$(cat $LIB/.for 2>/dev/null)" != "$want" ]; then
	rm -f $LIB/libfirehose_kernel.a $LIB/libpthread_kext.a $LIB/.for
	log "exporting the kernel's headers"
	xnumake -j$JOBS exporthdrs > "$XNUDEPS/exporthdrs.log" 2>&1 || die "exporthdrs failed, see $XNUDEPS/exporthdrs.log"
	E=$XNU_SRC/BUILD/obj/EXPORT_HDRS
	[ -d $E/osfmk ] || die "no exported headers in $E"
	# setup.sh's -DXNU_KERNEL_PRIVATE=0 trips xnu-12377's headers (they test it both with #if and
	# #ifdef: upsi.h, lock_rw.h), so it is left undefined; TargetConditionals.h comes from kernel/deps
	log "building libfirehose_kernel.a"
	xnu-clang -arch $XNU_ARCH -c -Os -mkernel -nostdinc -fno-builtin \
		-DKERNEL=1 -DKERNEL_PRIVATE=1 -DPRIVATE=1 -D__DARWIN_64_BIT_INO_T=1 \
		-DDISPATCH_USE_DTRACE=0 -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 \
		-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 -DOS_FIREHOSE_SPI=1 -DARM64_BOARD_CONFIG_BCM2837 \
		-Ilibdispatch -Ilibdispatch/src -Ilibdispatch/src/firehose \
		-I$E/osfmk -I$E/bsd -I$E/libkern -I$E/pexpert -I$E/iokit -I$E/security \
		-I$XNU_SRC/libkern/firehose -I$E/libkern/os -I$XNU_SRC/EXTERNAL_HEADERS -I$SL/include/kernel -I$BD/build/pthread/inc \
		-o build/firehose/firehose_buffer.o libdispatch/src/firehose/firehose_buffer.c
	libtool -static -o $LIB/libfirehose_kernel.a build/firehose/firehose_buffer.o
	log "building libpthread_kext.a"
	P=$XNUDEPS/libpthread
	for f in kern_init kern_support kern_synch; do
		xnu-clang -arch $XNU_ARCH -c -Os -mkernel -nostdinc -fno-builtin -fno-common -Wno-int-conversion \
			-DKERNEL=1 -DKERNEL_PRIVATE=1 -DPRIVATE=1 -D__PTHREAD_EXPOSE_INTERNALS__=1 -Dpthread_kern=pthread_kext_kern \
			-Dcurrent_uthread=pthread_kext_current_uthread -D__DARWIN_64_BIT_INO_T=1 -DARM64_BOARD_CONFIG_BCM2837 \
			-I$BD/build/pthread/inc -I$P -I$E/osfmk -I$E/bsd -I$E/libkern -I$E/pexpert -I$E/iokit -I$E/security \
			-I$XNU_SRC/EXTERNAL_HEADERS -I$P/include -I$P/private -o build/pthread/pthread_$f.o $P/kern/$f.c
	done
	libtool -static -o $LIB/libpthread_kext.a build/pthread/pthread_kern_init.o build/pthread/pthread_kern_support.o \
		build/pthread/pthread_kern_synch.o
	echo "$want" > $LIB/.for
fi

# --- 4. the kernel ----------------------------------------------------------------------------
cd "$XNU_SRC"
[ $# -gt 0 ] || set -- -j$JOBS
xnumake "$@" 2>&1 | tee "$XNUDEPS/build.log"
# XNU_WARNINGS=1: of the compiler's warnings, those in HakiOS' own files (kernel/overlay), which are to have none
if [ "${XNU_WARNINGS:-0}" = 1 ]; then
	ours=$(cd "$K/overlay" && find . -type f -name '*.[ch]*' | sed "s|^\./|$XNU_SRC/|")
	grep -F -f <(printf '%s:\n' $ours) "$XNUDEPS/build.log" | grep ': warning: ' | sort -u > "$XNUDEPS/warnings-ours.txt" || true
	if [ -s "$XNUDEPS/warnings-ours.txt" ]; then
		log "warnings in HakiOS' files ($(wc -l < "$XNUDEPS/warnings-ours.txt")):"
		sed "s|^$XNU_SRC/|  |" "$XNUDEPS/warnings-ours.txt"
	else
		log "no warning in HakiOS' files (of those this build compiled)"
	fi
fi
KF=$XNU_SRC/BUILD/obj/$OBJECT
[ -f "$KF" ] || die "no $KF"
if [ "$HOST_OS" = Darwin ]; then
	KERNEL_SIZE=$(stat -L -f %z "$KF")
else
	KERNEL_SIZE=$(stat -L -c %s "$KF")
fi
log "kernel: $KF ($KERNEL_SIZE bytes)${WIFI_OBJECT:+, with the Wi-Fi driver}, linked with $TOOLCHAIN's tools"
