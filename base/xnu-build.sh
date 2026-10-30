#!/bin/bash
# Build XNU for redwood on Linux. Extra make args are passed through.
source "$(dirname "${BASH_SOURCE[0]}")/xnu-env.sh"
cd "$XNU_SRC" || exit 1
# The Wi-Fi driver's object is linked into the kernel when it is there; make does not see it change, so the linked
# kernel is thrown away when it has. redwood's is ath11k (tools/mk-ath11k.sh, WCN6750).
LX=$XNUDEPS/build/ath11k/out/ath11k.o
# The objects of arm64 and arm64e builds land in the same directory (the machine configuration, not
# the architecture, names it), and make cannot tell them apart: start over when the architecture
# changes. The kernel's own static libraries (xnu-deps/setup.sh) need the same architecture; see
# xnu-deps/setup.sh --rebuild-libs.
ARCHSTAMP=BUILD/obj/redwood-arch
if [ -d BUILD/obj ] && [ "$(cat $ARCHSTAMP 2>/dev/null)" != "${XNU_ARCH:-arm64}" ]; then
	echo "xnu-build.sh: architecture changed to ${XNU_ARCH:-arm64}: discarding BUILD/obj"
	rm -rf BUILD/obj
fi
mkdir -p BUILD/obj && echo "${XNU_ARCH:-arm64}" > $ARCHSTAMP
STAMP=BUILD/obj/redwood-objects
if [ -f "$LX" ]; then now="$LX $(stat -c %Y "$LX" 2>/dev/null || stat -f %m "$LX")"; else LX= now=; fi
if [ -d BUILD/obj ] && [ "$(cat $STAMP 2>/dev/null)" != "$now" ]; then
	rm -f BUILD/obj/*/mach.*.unstripped
	echo "$now" > $STAMP
fi
# The build tools of SETUP/ are compiled for this machine: on Linux with glibc stand-ins for BSD
# and Mach calls, on macOS against the system's own headers.
if [ -n "$XNU_HOST_DARWIN" ]; then
	HOSTCC="clang -I$XNUDEPS/hostinc-darwin -Wno-unused-command-line-argument"
else
	HOSTCC="clang -include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration"
fi
exec make REDWOOD_OBJECTS="$LX" SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-BCM2837} KERNEL_CONFIGS=DEVELOPMENT \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=${XNU_ARCH:-arm64} BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 BUILD_DSYM=0 \
  HOST_CC="$HOSTCC" HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n "$@"
