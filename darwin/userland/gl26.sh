#!/bin/bash
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}   # the directory holding this repository and darwin-renoir
DR=${DR:-$DK/darwin-renoir}
cd "$DR/userland"
source ../xnu-env.sh
O=$RN/userland/obj26
# pacstrip is built by darwin/userland/env.sh (arch/arm64/tools/pacstrip), or PACSTRIP=FILE
PACSTRIP=${PACSTRIP:-$RN/../arch/arm64/tools/pacstrip}
[ -x "$PACSTRIP" ] || { echo "gl26.sh: no pacstrip at $PACSTRIP (darwin/userland/env.sh builds it)" >&2; exit 1; }
DYN="xnu-clang -arch arm64e -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1 -mios-version-min=16.0"
[ -f obj/eagl_gen.h ] || ./mkeagl.py src/eagl_slots.txt ../xnu-deps/build/mesa/out/gl.xml ../xnu-deps/build/mesa/out/exported-gl.txt src/eagl.c > obj/eagl_gen.h
$DYN -dynamiclib -install_name /hakios/lib/libhakios-gl.dylib -Iobj -framework CoreFoundation \
	-framework IOKit -framework IOSurface -lobjc "$O/libOSMesa.8.dylib" -o "$O/libhakios-gl.dylib" src/eagl.c src/gl_osmesa.c
$DYN -framework CoreFoundation -framework IOKit "$O/libOSMesa.8.dylib" -o "$O/gl_test" src/gl_test.c
# (a file pacstrip refuses stops the build: "pacstrip ... && mv" let it through as it was, PAC and all)
for f in "$O/libhakios-gl.dylib" "$O/gl_test"; do
  "$PACSTRIP" macho "$f" "$f.strip" > /dev/null || { echo "gl26.sh: pacstrip refused $f" >&2; exit 1; }
  mv "$f.strip" "$f"
  "$THEOS/toolchain/linux/iphone/bin/ldid" -S "$f"
  file -b "$f" | cut -c1-60
done
