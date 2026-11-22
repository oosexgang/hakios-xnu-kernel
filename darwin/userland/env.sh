# shellcheck shell=bash
# What the iOS 26/27 scripts share; sourced by them, not run. Each can be set before:
#
#   IOS     the unpacked IPSW (darwin/tools/unpack.sh) and the images made from it; default: ios27 beside the
#           repository
#   OBJ     our arm64e programs and libraries for that IPSW (inj26.sh, root26-darwin.sh); default: $IOS/obj
#   BOARD   the phone the small image is for: its firmware/BOARD/fw/ goes in. Default: HAKIOS_BOARD, or the
#           only device package here with a uefi { } node; none: an image without a phone's firmware
#   LDID    ldid (procursus' or theos'), for the signatures of what we build
#   DARWIN_SDK, DARWIN_LD  the iPhoneOS 16.5 SDK and ld64, when they are not theos' (sdks/, toolchain/)
#
# and they get US, the user-space sources of the first generation that iOS 26/27 still uses
# (userland/userland: init, the injected libraries, mkjobs.py), and the cross compiler, xnu-clang, from
# the kernel's build (kernel/env.sh: the kernel is built first).
IOS_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=../../kernel/env.sh
source "$IOS_REPO/kernel/env.sh"
[ -x "$XNUDEPS/host/bin/xnu-clang" ] || { echo "the cross compiler is made by kernel/build.sh: build the kernel first" >&2; exit 1; }
US=$IOS_REPO/userland/userland
# the image's configuration: build.config.darwin27 at the repository's top (with build.config.common), or the file
# there that DARWIN_BUILD_CONFIG names; what the environment sets already is kept
HAKIOS=${HAKIOS:-$IOS_REPO}
# shellcheck source=../../build.config.darwin27
source "$IOS_REPO/${DARWIN_BUILD_CONFIG:-build.config.darwin27}"
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
SDK=${DARWIN_SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}   # (DARWIN_SDK: another place of it, as kernel/build.sh takes)
# our own arm64e code: built against the 16.5 SDK (theos), run on iOS 26/27 after pacstrip has taken its
# pointer authentication out
# Linked by ld64 (theos' cctools, or DARWIN_LD), as arm64e. Without it, by LLVM's ld64.lld, which writes arm64e
# code into a plain arm64 file with classic binding opcodes that dyld refuses in an arm64e process ("bad bind
# opcode 0x00"): then the code is built for arm64 with chained fixups and its header named arm64e afterwards
# (pac_strip below), as Documentation/darwin26-boot.md found to load (there is no PAC to take out of arm64 code)
DARWIN_LD=${DARWIN_LD:-$THEOS/toolchain/linux/iphone/bin/ld}
# shellcheck disable=SC2034  # for the scripts that source this file
if [ -x "$DARWIN_LD" ]; then
	DYN="xnu-clang -arch arm64e -isysroot $SDK -O1 -mios-version-min=16.0 --ld-path=$DARWIN_LD"
	DARWIN_LLD=0
else
	echo "no ld64 at $DARWIN_LD (theos' toolchain, or DARWIN_LD=FILE): ours are linked by ld64.lld, as arm64" >&2
	# (+pauth: for the assembler, the PAC instructions some of ours write by hand, which pacstrip then rewrites)
	DYN="xnu-clang -arch arm64 -Xclang -target-feature -Xclang +pauth -isysroot $SDK -O1 -mios-version-min=16.0 -Wl,-fixup_chains"
	DARWIN_LLD=1
fi
export IOS OBJ BOARD LDID US SDK
# pacstrip (arch/arm64/tools/pacstrip.c): made here when it is missing or older than its source
PACSTRIP=$IOS_REPO/arch/arm64/tools/pacstrip
if [ ! -x "$PACSTRIP" ] || [ "$IOS_REPO/arch/arm64/tools/pacstrip.c" -nt "$PACSTRIP" ]; then
	PACSTRIP_CFLAGS=()
	PACSTRIP_LDFLAGS=()
	if [ "$(uname -s)" = Darwin ]; then
		for openssl in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 /opt/homebrew/opt/openssl; do
			if [ -f "$openssl/include/openssl/sha.h" ]; then
				PACSTRIP_CFLAGS+=("-I$openssl/include")
				PACSTRIP_LDFLAGS+=("-L$openssl/lib")
				break
			fi
		done
	fi
	cc -O2 "${PACSTRIP_CFLAGS[@]}" -o "$PACSTRIP" "$IOS_REPO/arch/arm64/tools/pacstrip.c" "${PACSTRIP_LDFLAGS[@]}" -lcrypto || { echo "pacstrip did not build (it needs libcrypto)" >&2; exit 1; }
fi
# pac_strip IN [OUT]: IN with its PAC instructions rewritten, into OUT or in place; the build stops when pacstrip
# refuses it (a file that is not arm64e), where "pacstrip ... && mv" let it through as it was
pac_strip() {
	local out=${2:-$1.strip}
	# (linked by ld64.lld: the arm64 file's CPU subtype made arm64e with its pointer authentication ABI, 0x80000002)
	[ "$DARWIN_LLD" = 0 ] || python3 -c '
import struct, sys
f = open(sys.argv[1], "r+b"); h = f.read(12)
if struct.unpack("<III", h) == (0xfeedfacf, 0x0100000c, 0):
	f.seek(8); f.write(struct.pack("<I", 0x80000002))' "$1"
	"$PACSTRIP" macho "$1" "$out" > /dev/null || { echo "pacstrip refused $1: not an arm64e Mach-O" >&2; exit 1; }
	[ -n "${2:-}" ] || mv "$out" "$1"
}
