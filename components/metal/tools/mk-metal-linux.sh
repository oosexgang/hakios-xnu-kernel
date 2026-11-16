#!/bin/bash
# The Metal driver and its two test programs for Darwin 26/27, built on Linux: LLVM's clang and ld64.lld with
# theos's iPhoneOS 16.5 SDK, as tools/mk-mesa26-linux.sh builds the library the driver loads (libhakios-air).
#
#   HakiosMetal       the driver (src/metal_driver.m, and the userland's drawable.m) as the bundle executable Metal's
#                     discovery opens: /System/Library/Extensions/HakiosMetal.bundle (Info.plist:
#                     src/metal_plugin.plist)
#   metal_props       puts Metal's two discovery properties on the kernel's IOAcceleratorES (boot-arg metal=1)
#   metal_drv_test    Metal's own steps 4 and 5 against the driver, in a process of its own
#
# rw_null_air.h, the driver's stand-in fragment shader as AIR, is made from src/rw_null.ll with
# llvm-as: the Mac build compiled rw_null.metal with Apple's Metal compiler, which Linux has not got.
# As with libhakios-air, the code is arm64 with chained fixups and the header says arm64e (ld64.lld does not
# link arm64e with chained fixups, and dyld in an arm64e process refuses the classic binding opcodes); then ldid.
#
# Needs: clang-20 and lld-20 (/usr/lib/llvm-20), THEOS (default ~/theos) with sdks/iPhoneOS16.5.sdk and ldid.
# Out, in build/metal-linux: HakiosMetal.bundle/{HakiosMetal,Info.plist}, metal_props, metal_drv_test.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src
USRC=$(cd "$ROOT/../../userland/userland/src" && pwd)    # (drawable.m and board/drawable.h, shared with OpenGL ES)
B=$ROOT/build/metal-linux
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
THEOS=${THEOS:-$HOME/theos}
SDK=${SDK:-${DARWIN_SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}}
LDID=${LDID:-$(command -v ldid || echo "$THEOS/toolchain/linux/iphone/bin/ldid")}
for need in "$LLVM/clang" "$LLVM/llvm-as" "$LLVM/ld64.lld" "$SDK/usr/lib/libSystem.tbd"; do
	[ -e "$need" ] || { echo "mk-metal-linux.sh: $need is missing (LLVM=, THEOS=, SDK=)"; exit 1; }
done
mkdir -p "$B/HakiosMetal.bundle"
cd "$B"
CC=("$LLVM/clang" -target arm64-apple-ios16.0 -isysroot "$SDK" -O1 -fuse-ld=lld "-Wl,-fixup_chains")

"$LLVM/llvm-as" -o rw_null.bc "$SRC/rw_null.ll"
python3 - rw_null.bc rw_null_air.h <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
rows = [', '.join('0x%02x' % x for x in b[i:i + 16]) for i in range(0, len(b), 16)]
with open(sys.argv[2], 'w') as f:
    f.write('/* made from src/rw_null.ll by tools/mk-metal-linux.sh: not edited, not committed */\n')
    f.write('static const unsigned char rw_null_air[] = {\n\t' + ',\n\t'.join(rows) + '\n};\n')
    f.write('static const unsigned int rw_null_air_len = %d;\n' % len(b))
PY

METAL=(-framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc)
"${CC[@]}" -bundle "${METAL[@]}" -I "$B" -o HakiosMetal.bundle/HakiosMetal "$SRC/metal_driver.m" "$USRC/drawable.m"
cp "$SRC/metal_plugin.plist" HakiosMetal.bundle/Info.plist
"${CC[@]}" -framework CoreFoundation -framework IOKit -o metal_props "$SRC/metal_props.c"
"${CC[@]}" "${METAL[@]}" -o metal_drv_test "$SRC/metal_drv_test.m"

python3 - HakiosMetal.bundle/HakiosMetal metal_props metal_drv_test <<'PY'
import struct, sys
for path in sys.argv[1:]:
    with open(path, 'r+b') as f:
        magic, cputype = struct.unpack('<II', f.read(8))
        assert magic == 0xfeedfacf and cputype == 0x0100000c, path
        f.write(struct.pack('<I', 0x80000002))
PY
"$LDID" -S HakiosMetal.bundle/HakiosMetal metal_props metal_drv_test
echo "mk-metal-linux.sh: $B/HakiosMetal.bundle ($(stat -c %s HakiosMetal.bundle/HakiosMetal) bytes), metal_props, metal_drv_test"
