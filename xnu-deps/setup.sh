#!/bin/bash
# Everything the kernel build needs that this repository does not carry: open sources of
# Apple's and others' that are fetched and built here, and the SDK the kernel is compiled
# against. What is already there is left alone, so this can be run again at any time.
#
#   xnu-deps/setup.sh             both stages (the second only once the kernel tree exists)
#   xnu-deps/setup.sh --stage1    sources, host tools, the SDK
#   xnu-deps/setup.sh --stage2    the two static libraries that are compiled against the
#                                 kernel's own exported headers (needs xnu/apply.sh first)
#   xnu-deps/setup.sh --rebuild-libs   stage 2 again from scratch (after the kernel headers changed)
#
# Needs on the machine: git curl tar xz make cc clang flex bison m4 perl python3 autoconf
# automake, and theos ($THEOS, default ~/theos) with its Linux toolchain
# (toolchain/linux/iphone: the cctools linker) and sdks/iPhoneOS16.5.sdk.
#
# What ends up where (all under xnu-deps/, none of it committed):
#   AvailabilityVersions libdispatch libpthread     Apple open source, at the releases of iOS 16.5
#   mm_bootstrap_cmds tcsh unifdef                  sources of mig, csh and unifdef
#   MacOSX13.3.sdk                                  only Kernel.framework: the DriverKit headers
#                                                   that Apple's closed "iig" would generate
#   host/bin                                        mig, migcom, unifdef, tcsh, csh, unzbm, and links
#                                                   to the cctools (next to our own wrappers)
#   sdk/iPhoneOS16.5.xnu.sdk                        the theos SDK (as links) plus usr/local: what
#                                                   Apple's internal SDK has and the public one lacks
set -e -o pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../xnu-env.sh"
cd "$XNUDEPS"
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}   # macOS has no nproc
# The Mach-O linker and its companions: theos carries them for Linux, and macOS has them itself
# in the Command Line Tools, which is where xcrun finds them.
if [ "$(uname -s)" = Darwin ]; then
	CCTOOLS=$(dirname "$(/usr/bin/xcrun -f ld)")
else
	CCTOOLS=$THEOS/toolchain/linux/iphone/bin
fi
IOS_SDK=$THEOS/sdks/iPhoneOS16.5.sdk
MACOS_SDK_URL=https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz

log() { echo "setup: $*"; }
die() { echo "setup: $*" >&2; exit 1; }

# fetch DIR URL COMMIT: one commit of a repository, without its history
fetch() {
	# there already? (its own repository, not the one this directory is part of)
	[ -d "$1" ] && git -C "$1" rev-parse -q --verify HEAD >/dev/null 2>&1 &&
		[ "$(git -C "$1" rev-parse --show-toplevel)" = "$(cd "$1" && pwd -P)" ] && return 0
	log "fetching $1"
	rm -rf "$1"
	git init -q "$1"
	git -C "$1" remote add origin "$2"
	git -C "$1" fetch -q --depth 1 origin "$3" || die "cannot fetch $3 from $2"
	git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

# link TARGET NAME: a symbolic link, made only when it is not there already
link() {
	[ "$(readlink "$2" 2>/dev/null)" = "$1" ] || ln -sfn "$1" "$2"
}

# replace FILE with what is on standard input, unless it is the same already (make looks at time stamps)
update() {
	cat > "$1.new"
	if cmp -s "$1.new" "$1"; then rm "$1.new"; else mv "$1.new" "$1"; fi
}

stage1() {
	local missing= tool
	local needed="git curl tar make cc clang clang++ flex bison m4 perl python3"
	# only for building mig; and GNU tar calls xz, where macOS's bsdtar unpacks .tar.xz by itself
	[ "$(uname -s)" = Darwin ] || needed="$needed xz autoreconf automake"
	for tool in $needed; do
		command -v $tool >/dev/null || missing="$missing $tool"
	done
	[ -z "$missing" ] || die "not installed:$missing"
	[ -x "$CCTOOLS/ld" ] || die "no cctools in $CCTOOLS: install theos with its Linux toolchain (or set THEOS)"
	[ -d "$IOS_SDK/usr/include" ] || die "no $IOS_SDK (the SDKs of github.com/theos/sdks go to \$THEOS/sdks)"

	# --- sources -------------------------------------------------------------------------------
	fetch AvailabilityVersions https://github.com/apple-oss-distributions/AvailabilityVersions.git \
		5d9baee345b3bc72b26c71adc65f94f2d5e9ced2        # AvailabilityVersions-115.4
	fetch libdispatch https://github.com/apple-oss-distributions/libdispatch.git \
		a641558f447659d90ad002ef0ee6f0212273fbfc        # libdispatch-1415.100.11
	fetch libpthread https://github.com/apple-oss-distributions/libpthread.git \
		a6c70ca0e544347a0377babf748f831577ec24f9        # libpthread-514.100.2
	if [ "$(uname -s)" != Darwin ]; then     # macOS has mig and csh; see "host tools" below
		fetch mm_bootstrap_cmds https://github.com/markmentovai/bootstrap_cmds \
			f337e600741638896fb6056ce87c6479f74b89a3        # cross_platform/bootstrap_cmds-138
		fetch tcsh https://github.com/tcsh-org/tcsh.git b7db07931546178c4cb2542b5fc850c483628ed5
	fi
	fetch unifdef https://github.com/fanf2/unifdef.git 08d12cd3059fe072929ea001ad9833f5487fd355

	# Apple's iig (the DriverKit interface generator) is closed. host/bin/iig hands out the headers it
	# would generate, which the macOS SDK of the same XNU release ships ready-made.
	if [ ! -d MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers/DriverKit ]; then
		log "downloading the macOS 13.3 SDK for its DriverKit headers"
		curl -L --fail --retry 3 -s -o macos-sdk.tar.xz "$MACOS_SDK_URL" || die "download failed: $MACOS_SDK_URL"
		# bsdtar (macOS) matches patterns without --wildcards, which it does not know
		if tar --version 2>/dev/null | grep -q GNU; then
			tar -xJf macos-sdk.tar.xz --wildcards '*MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework*'
		else
			tar -xJf macos-sdk.tar.xz '*MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework*'
		fi || die "cannot unpack the macOS SDK"
		rm -f macos-sdk.tar.xz
	fi

	# --- host tools ----------------------------------------------------------------------------
	mkdir -p host/bin
	# macOS has mig, migcom and csh of its own; on Linux they are built below
	if [ "$(uname -s)" = Darwin ]; then
		local mac_mig mac_migcom
		mac_mig=$(/usr/bin/xcrun -f mig)
		mac_migcom=$(dirname "$(dirname "$mac_mig")")/libexec/migcom
		[ -x "$mac_mig" ] && [ -x "$mac_migcom" ] || die "no mig in the Command Line Tools"
		link "$mac_mig" host/bin/mig
		link "$mac_migcom" host/bin/migcom
		link /bin/tcsh host/bin/tcsh
		# flex runs m4. Some Command Line Tools (15.3) have /usr/bin/m4 and nothing behind it, only gm4
		if ! m4 --version >/dev/null 2>&1; then
			local mac_gm4
			mac_gm4=$(/usr/bin/xcrun -f gm4 2>/dev/null) && [ -x "$mac_gm4" ] ||
				die "m4 does not run and there is no gm4 in the Command Line Tools"
			link "$mac_gm4" host/bin/m4
		fi
	fi
	# mig: Apple's own does not build on glibc; this is a port that does
	if [ ! -x host/bin/migcom ] || [ ! -x host/bin/mig ]; then
		log "building mig"
		(cd mm_bootstrap_cmds && autoreconf --install && sh configure --prefix="$XNUDEPS/host" &&
			make -j$JOBS && make install) > mig-build.log 2>&1 || die "mig did not build, see xnu-deps/mig-build.log"
	fi
	if [ ! -x host/bin/unifdef ]; then
		log "building unifdef"
		# its makefile takes the version from the git history; any text will do
		[ -f unifdef/version.h ] || printf '"@(#) $Version: unifdef-2.12 $\\n"\n"@(#) $Date: 2020-11-06 11:22:51 +0000 $\\n"\n' > unifdef/version.h
		# gnu11: "constexpr", a variable name in unifdef.c, is a keyword in C23
		cc -std=gnu11 -O2 -w -o host/bin/unifdef unifdef/unifdef.c
	fi
	# csh: XNU's SETUP/config/doconf is a csh script
	if [ ! -x host/bin/tcsh ]; then
		log "building tcsh"
		(cd tcsh && ./configure --prefix="$XNUDEPS/host" && make -j$JOBS) > tcsh-build.log 2>&1 ||
			die "tcsh did not build, see xnu-deps/tcsh-build.log"
		cp tcsh/tcsh host/bin/tcsh
	fi
	link tcsh host/bin/csh
	# what 7z leaves undone when it extracts files of the iOS images (see build/lzbitmap/unzbm.c)
	if [ ! -x host/bin/unzbm ]; then
		cc -O2 -w -o host/bin/unzbm build/lzbitmap/unzbm.c build/lzbitmap/libzbitmap.c
	fi
	# the Mach-O linker and its companions come with theos
	for tool in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
		link "$CCTOOLS/$tool" host/bin/$tool
	done
	link "$CCTOOLS/ld" host/bin/ld64
	link xnu-clang host/bin/xnu-clang++

	# --- the SDK -------------------------------------------------------------------------------
	# The makefiles ask xcrun (ours: host/bin/xcrun) for an SDK and a platform directory.
	mkdir -p sdk/Platforms/iPhoneOS.platform
	if [ ! -d "$XNU_SDK_PATH/usr/include" ]; then
		log "making the SDK: links to $IOS_SDK"
		rm -rf "$XNU_SDK_PATH" "$XNU_SDK_PATH.new"
		# (not cp -rs: macOS 14's cp says "Bad file descriptor" for every file. Made aside and moved, so
		# that a run that stopped half way does not leave a directory the test above takes for the SDK)
		python3 - "$(cd "$IOS_SDK" && pwd)" "$XNU_SDK_PATH.new" <<'PY' || die "cannot make the SDK's tree of links"
import os, sys
src, dst = sys.argv[1], sys.argv[2]
for root, dirs, files in os.walk(src):
    out = os.path.join(dst, os.path.relpath(root, src))
    os.makedirs(out, exist_ok=True)
    for name in files + [d for d in dirs if os.path.islink(os.path.join(root, d))]:
        os.symlink(os.path.join(root, name), os.path.join(out, name))
    dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(root, d))]
PY
		mv "$XNU_SDK_PATH.new" "$XNU_SDK_PATH"
	fi
	local sdk_local=$XNU_SDK_PATH/usr/local
	mkdir -p $sdk_local/include/kernel/os $sdk_local/include/os $sdk_local/lib/kernel $sdk_local/libexec
	update $sdk_local/libexec/availability.pl < AvailabilityVersions/availability.pl
	[ -x $sdk_local/libexec/availability.pl ] || chmod +x $sdk_local/libexec/availability.pl
	# the kernel's half of a private libdispatch header (unifdef exits with 1 when it removed something)
	{ host/bin/unifdef -DOS_FIREHOSE_SPI=1 -DKERNEL=1 libdispatch/os/firehose_buffer_private.h || [ $? -eq 1 ]; } |
		update $sdk_local/include/kernel/os/firehose_buffer_private.h
	update $sdk_local/include/kernel/os/firehose_types.defs < libdispatch/src/firehose/firehose_types.defs
	log "stage 1 done"
}

# The firehose (os_log) buffer code of libdispatch and the pthread kernel extension are open
# source but not part of XNU: Apple links the first from its internal SDK and loads the second
# as a kext. Here both become static libraries in the SDK (makedefs/MakeInc.def links them into
# the kernel). They are compiled against the headers the kernel build exports, so that step
# of the kernel build comes first.
stage2() {
	[ -f "$XNU_SRC/makedefs/MakeInc.def" ] || die "no kernel tree in $XNU_SRC (xnu/apply.sh makes it)"
	[ -x host/bin/migcom ] && [ -d "$XNU_SDK_PATH/usr/local/lib/kernel" ] || die "stage 1 has not been done"
	local lib=$XNU_SDK_PATH/usr/local/lib/kernel hdrs=$XNU_SRC/BUILD/obj/EXPORT_HDRS file
	# three Mach-O headers for the build tools the kernel build compiles for this machine (SETUP/)
	mkdir -p hostinc/mach-o
	for file in loader.h nlist.h fat.h; do
		[ -f hostinc/mach-o/$file ] || cp "$XNU_SRC/EXTERNAL_HEADERS/mach-o/$file" hostinc/mach-o/
	done
	if [ -n "$REBUILD_LIBS" ]; then rm -f $lib/libfirehose_kernel.a $lib/libpthread_kext.a; fi
	if [ -f $lib/libfirehose_kernel.a ] && [ -f $lib/libpthread_kext.a ]; then
		log "stage 2: nothing to do"
		return 0
	fi
	log "exporting the kernel's headers (xnu-build.sh exporthdrs)"
	"$DARWIN_REDWOOD/xnu-build.sh" -j$JOBS exporthdrs > exporthdrs.log 2>&1 && [ -d "$hdrs/osfmk" ] ||
		die "the kernel headers were not exported, see xnu-deps/exporthdrs.log"
	if [ ! -f $lib/libfirehose_kernel.a ]; then
		log "building libfirehose_kernel.a"
		mkdir -p build/firehose
		xnu-clang -arch arm64 -c -Os -mkernel -nostdinc -fno-builtin \
			-DKERNEL=1 -DKERNEL_PRIVATE=1 -DXNU_KERNEL_PRIVATE=0 -DPRIVATE=1 -D__DARWIN_64_BIT_INO_T=1 \
			-DDISPATCH_USE_DTRACE=0 -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 \
			-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 -DOS_FIREHOSE_SPI=1 -DARM64_BOARD_CONFIG_BCM2837 \
			-Ilibdispatch -Ilibdispatch/src -Ilibdispatch/src/firehose \
			-I$hdrs/osfmk -I$hdrs/bsd -I$hdrs/libkern -I$hdrs/pexpert -I$hdrs/iokit -I$hdrs/security \
			-I$XNU_SRC/libkern/firehose -I$hdrs/libkern/os -I$XNU_SRC/EXTERNAL_HEADERS \
			-I$XNU_SDK_PATH/usr/local/include/kernel \
			-o build/firehose/firehose_buffer.o libdispatch/src/firehose/firehose_buffer.c
		libtool -static -o $lib/libfirehose_kernel.a build/firehose/firehose_buffer.o
	fi
	if [ ! -f $lib/libpthread_kext.a ]; then
		log "building libpthread_kext.a"
		build/pthread/build.sh
	fi
	log "stage 2 done; now: ./xnu-build.sh -j$JOBS -k"
}

case "$1" in
--stage1) stage1 ;;
--stage2) stage2 ;;
--rebuild-libs) REBUILD_LIBS=1 stage2 ;;
"")
	stage1
	if [ -f "$XNU_SRC/makedefs/MakeInc.def" ]; then
		stage2
	else
		log "no kernel tree yet: run xnu/apply.sh, then this script again"
	fi ;;
*) sed -n '2,12p' "$HERE/setup.sh"; exit 1 ;;
esac
