#!/bin/bash
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}   # the directory holding this repository and darwin-renoir
DR=${DR:-$DK/darwin-renoir}
cd $DR/userland
source ../xnu-env.sh
O=$RN/userland/obj26
DYN="xnu-clang -arch arm64e -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1 -mios-version-min=16.0"
[ -f obj/eagl_gen.h ] || ./mkeagl.py src/eagl_slots.txt ../xnu-deps/build/mesa/out/gl.xml ../xnu-deps/build/mesa/out/exported-gl.txt src/eagl.c > obj/eagl_gen.h
$DYN -dynamiclib -install_name /hakios/lib/libhakios-gl.dylib -Iobj -framework CoreFoundation \
	-framework IOKit -framework IOSurface -lobjc $O/libOSMesa.8.dylib -o $O/libhakios-gl.dylib src/eagl.c src/gl_osmesa.c
$DYN -framework CoreFoundation -framework IOKit $O/libOSMesa.8.dylib -o $O/gl_test src/gl_test.c
for f in $O/libhakios-gl.dylib $O/gl_test; do
  $RN/tools/pacstrip macho $f $f.strip > /dev/null && mv $f.strip $f
  $THEOS/toolchain/linux/iphone/bin/ldid -S $f
  file -b $f | cut -c1-60
done
