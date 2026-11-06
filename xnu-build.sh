#!/bin/bash
# Build XNU for redwood on Linux. Extra make args are passed through.
source "$(dirname "${BASH_SOURCE[0]}")/xnu-env.sh"
cd "$XNU_SRC" || exit 1
# The Wi-Fi driver's object (tools/mk-ath11k.sh) is linked into the kernel when it is there; make
# does not see it change, so the linked kernel is thrown away when it has.
LX=$XNUDEPS/build/ath11k/out/ath11k.o
STAMP=BUILD/obj/redwood-objects
if [ -f "$LX" ]; then now="$LX $(stat -c %Y "$LX")"; else LX= now=; fi
if [ -d BUILD/obj ] && [ "$(cat $STAMP 2>/dev/null)" != "$now" ]; then
	rm -f BUILD/obj/*/mach.*.unstripped
	echo "$now" > $STAMP
fi
# The tools the build compiles for this machine (SETUP/) must use this machine's own headers. On
# macOS clang takes SDKROOT from the environment, and SDKROOT here says iphoneos, so without this
# they are compiled against the iOS SDK and stop at "architecture not supported".
# xnu-deps/hostinc stands in for what macOS has and Linux does not (getprogname, byteswap.h,
# the Mach-O headers); on macOS it is in the way, and the SDK is this machine's own.
if [ "$(uname -s)" = Darwin ]; then
	HOST_EXTRA="-isysroot $(/usr/bin/xcrun --show-sdk-path)"
else
	HOST_EXTRA="-include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc"
fi

exec make REDWOOD_OBJECTS="$LX" SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-BCM2837} KERNEL_CONFIGS=DEVELOPMENT \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 \
  HOST_CC="clang $HOST_EXTRA -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration" HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4 CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n "$@"
