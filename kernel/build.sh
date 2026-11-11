#!/bin/bash
# Build the kernel on Linux: Apple's release (kernel/UPSTREAM) with kernel/overlay and kernel/patches,
# as plain arm64 (no PAC), MACHINE=BCM2837, DEVELOPMENT. One kernel serves every board: which one it runs
# on it learns at boot (/chosen/darwin-board; devices/).
#
#   kernel/build.sh [make args]      e.g. build.sh -j12 (default -j$(nproc))
#
# From scratch it makes, in order (each step is skipped when its result is already there):
#   1. $XNU_SRC (default: next to the repository, named after the release): kernel/assemble.sh; a tree
#      that is there already is brought up to date with the repository (only changed files are touched)
#   2. $XNUDEPS ($XNU_SRC-deps): Apple's open sources and the host tools (kernel/deps/stage1.sh fetches
#      and builds them there, unless a darwin-renoir checkout has them), kernel/deps' wrappers, its own
#      SDK (links to theos' iPhoneOS16.5.sdk plus usr/local)
#   3. the kernel's exported headers, then libfirehose_kernel.a and libpthread_kext.a built against them
#   4. the kernel: $XNU_SRC/BUILD/obj/mach.development.bcm2837
# Needs theos (the cctools and iPhoneOS16.5.sdk); see docs/building.md.
set -e -o pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"         # (focaltech_stub)
K=$HAKIOS_KERNEL
BD=$K/deps
JOBS=$(nproc)
log() { echo "build.sh: $*"; }
die() { echo "build.sh: $*" >&2; exit 1; }
link() { [ "$(readlink "$2" 2>/dev/null)" = "$1" ] || ln -sfn "$1" "$2"; }
update() { cat > "$1.new"; if cmp -s "$1.new" "$1"; then rm "$1.new"; else mv "$1.new" "$1"; fi; }

# --- 1. the tree ------------------------------------------------------------------------------
if [ ! -e "$XNU_SRC" ]; then
	"$K/assemble.sh" "$XNU_SRC"
elif git -C "$XNU_SRC" rev-parse -q --verify refs/tags/upstream > /dev/null 2>&1; then
	"$K/assemble.sh" --update "$XNU_SRC"
else
	die "$XNU_SRC is not a tree made by kernel/assemble.sh (one of the old apply.sh, perhaps): move it away or set XNU_SRC"
fi
# redwood's touch firmware is not ours to publish (tools/mk-focaltech-fw.py makes it from a phone):
# an empty one keeps redwood_goodix.c compiling; a board without that panel needs none
FW=$XNU_SRC/bsd/dev/redwood_focaltech_fw.h
if [ ! -f "$FW" ]; then
	log "stub $FW (no FocalTech firmware)"
	focaltech_stub > "$FW"
fi

# --- 2. the dependency directory --------------------------------------------------------------
# Apple's open sources and the host tools (mig, tcsh, unifdef): a darwin-renoir checkout has them
# fetched and built already; without one they are fetched and built once, beside the tree
RD=$DARWIN_RENOIR/xnu-deps
if [ ! -x "$RD/host/bin/migcom" ] || [ ! -x "$RD/host/bin/tcsh" ] || [ ! -d "$RD/libdispatch" ]; then
	RD=$XNUDEPS/stage1
	"$BD/stage1.sh" "$RD"
fi
CCTOOLS=$THEOS/toolchain/linux/iphone/bin
IOS_SDK=$THEOS/sdks/iPhoneOS16.5.sdk
[ -x "$CCTOOLS/ld" ] && [ -d "$IOS_SDK/usr/include" ] || die "no theos toolchain/SDK in $THEOS"
mkdir -p "$XNUDEPS/host/bin" "$XNUDEPS/build/firehose" "$XNUDEPS/build/pthread" "$XNUDEPS/sdk/Platforms/iPhoneOS.platform"
cd "$XNUDEPS"
for d in AvailabilityVersions libdispatch libpthread MacOSX13.3.sdk; do link "$RD/$d" $d; done
link "$BD/hostinc" hostinc
# iig implementation stand-ins: kernel/deps', plus kernel/iig-impl for classes new in xnu-12377
[ -L iig-impl ] && rm iig-impl
mkdir -p iig-impl
for f in "$BD"/iig-impl/* "$K"/iig-impl/*; do link "$f" "iig-impl/$(basename "$f")"; done
# built on this machine (stage 1)
for t in mig migcom tcsh unifdef unzbm; do link "$RD/host/bin/$t" host/bin/$t; done
link tcsh host/bin/csh
# kernel/deps' wrappers, and the iig and clang wrappers of this release (kernel/host: xnu-12377's DriverKit
# headers, and the Apple-only flags xnu-12377 adds)
for t in codesign install plutil sw_vers xcrun; do link "$BD/host/bin/$t" host/bin/$t; done
chmod +x "$K/host/xnu-clang" "$K/host/iig"
link "$K/host/iig" host/bin/iig
link "$K/host/xnu-clang" host/bin/xnu-clang
link xnu-clang host/bin/xnu-clang++
# the iig output of xnu-12377.121 (what Apple's closed iig would generate for the kernel), as the
# macOS 26.5 SDK ships it: only Kernel.framework is kept, outside the repository
MACOS26=MacOSX26.5.sdk
if [ ! -d $MACOS26/System/Library/Frameworks/Kernel.framework/Headers/DriverKit ]; then
	log "downloading the macOS 26.5 SDK for its DriverKit headers"
	U=https://github.com/socheatsok78/macos-sdks/releases/download/rolling/$MACOS26.tar.xz
	curl -L --fail --retry 3 -s -o macos26.tar.xz $U || die "download failed: $U"
	echo "e3dfe99521e8587b1eb820c5d8e21fa3721e629c8fe61e9ea4a89315ae249190  macos26.tar.xz" | sha256sum -c --quiet ||
		die "$U: unexpected checksum"
	tar -xJf macos26.tar.xz --wildcards "*$MACOS26/System/Library/Frameworks/Kernel.framework*" || die "cannot unpack $U"
	rm -f macos26.tar.xz
fi
for t in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
	link "$CCTOOLS/$t" host/bin/$t
done
link "$CCTOOLS/ld" host/bin/ld64
if [ ! -d "$XNU_SDK_PATH/usr/include" ]; then
	log "making the SDK: links to $IOS_SDK"
	rm -rf "$XNU_SDK_PATH"
	cp -rs "$(cd "$IOS_SDK" && pwd)" "$XNU_SDK_PATH"
fi
SL=$XNU_SDK_PATH/usr/local
mkdir -p $SL/include/kernel/os $SL/include/os $SL/lib/kernel $SL/libexec
update $SL/libexec/availability.pl < AvailabilityVersions/availability.pl
chmod +x $SL/libexec/availability.pl
{ host/bin/unifdef -DOS_FIREHOSE_SPI=1 -DKERNEL=1 libdispatch/os/firehose_buffer_private.h || [ $? -eq 1 ]; } |
	update $SL/include/kernel/os/firehose_buffer_private.h
update $SL/include/kernel/os/firehose_types.defs < libdispatch/src/firehose/firehose_types.defs

# make, with what the old workspace's xnu-build.sh passes, but without an external Wi-Fi object. DEFAULT_TARGET=build
# (as darwin-renoir/xnu-build.sh): the kernel, not the kext SDK's symbol sets (config/), whose
# kextsymboltool check fails on the exports a BCM2837 kernel does not have. xnu-12377's MakeInc.cmd
# names /usr/bin/plutil, so the no-op wrapper is passed as well.
xnumake() {
	(cd "$XNU_SRC" && make DEFAULT_TARGET=build REDWOOD_OBJECTS= SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=BCM2837 \
		KERNEL_CONFIGS=DEVELOPMENT ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=$XNU_ARCH RC_DARWIN_KERNEL_VERSION=${UPSTREAM_TAG#xnu-} \
		BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 BUILD_DSYM=0 PLUTIL=plutil \
		HOST_CC="clang -include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration" \
		HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n "$@")
}

# objects of arm64 and arm64e builds share BUILD/obj (the old workspace did both): start over on a change
if [ -f "$XNU_SRC/BUILD/obj/redwood-arch" ] && [ "$(cat "$XNU_SRC/BUILD/obj/redwood-arch")" != "$XNU_ARCH" ]; then
	log "architecture changed: discarding BUILD/obj"
	rm -rf "$XNU_SRC/BUILD/obj"
fi
mkdir -p "$XNU_SRC/BUILD/obj" && echo "$XNU_ARCH" > "$XNU_SRC/BUILD/obj/redwood-arch"

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
xnumake "$@"
KF=$XNU_SRC/BUILD/obj/mach.development.bcm2837
[ -f "$KF" ] || die "no $KF"
log "kernel: $KF ($(stat -L -c %s "$KF") bytes)"
