# shellcheck shell=bash
# What the iOS 26/27 scripts share; sourced by them, not run. Each can be set before:
#
#   IOS     the unpacked IPSW (ios/tools/unpack.sh) and the images made from it; default: ios27 beside the
#           repository
#   OBJ     our arm64e programs and libraries for that IPSW (inj26.sh, root26-ios.sh); default: $IOS/obj
#   BOARD   the phone the small image is for: its firmware/BOARD/fw/ goes in. Default: HAKIOS_BOARD, or the
#           only device package here with a [uefi] section; none: an image without a phone's firmware
#   LDID    ldid (procursus' or theos'), for the signatures of what we build
#
# and they get US, the user-space sources of the first generation that iOS 26/27 still uses
# (userland/userland: init, the injected libraries, mkjobs.py), and the cross compiler, xnu-clang, from
# the kernel's build (kernel/env.sh: the kernel is built first).
IOS_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=../../kernel/env.sh
source "$IOS_REPO/kernel/env.sh"
[ -x "$XNUDEPS/host/bin/xnu-clang" ] || { echo "the cross compiler is made by kernel/build.sh: build the kernel first" >&2; exit 1; }
US=$IOS_REPO/userland/userland
IOS=${IOS:-$(cd "$IOS_REPO/.." && pwd)/ios27}
OBJ=${OBJ:-$IOS/obj}
LDID=${LDID:-$(command -v ldid || echo "$THEOS/toolchain/linux/iphone/bin/ldid")}
if [ -z "${BOARD+set}" ]; then
	BOARD=$(cd "$IOS_REPO" && python3 -c '
import os, sys
sys.path.insert(0, "tools")
import boards
named = os.environ.get("HAKIOS_BOARD")
found = [n for n, b in boards.load()[1].items() if "uefi" in b]
print(named or (found[0] if len(found) == 1 else ""))')
fi
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
# our own arm64e code: built against the 16.5 SDK (theos), run on iOS 26/27 after pacstrip has taken its
# pointer authentication out
DYN="xnu-clang -arch arm64e -isysroot $SDK -O1 -mios-version-min=16.0"
export IOS OBJ BOARD LDID US SDK
# pacstrip (ios/tools/pacstrip.c): made here when it is missing or older than its source
PACSTRIP=$IOS_REPO/ios/tools/pacstrip
if [ ! -x "$PACSTRIP" ] || [ "$IOS_REPO/ios/tools/pacstrip.c" -nt "$PACSTRIP" ]; then
	cc -O2 -o "$PACSTRIP" "$IOS_REPO/ios/tools/pacstrip.c" -lcrypto || { echo "pacstrip did not build (it needs libcrypto)" >&2; exit 1; }
fi
