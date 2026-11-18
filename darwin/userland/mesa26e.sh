#!/bin/bash
# Mesa 26.2.5 (softpipe + freedreno, OSMesa) for arm64e Darwin 26 processes on renoir: darwin-renoir's
# tools/mk-mesa.sh with -arch arm64e, then pacstrip (pointer authentication made the identity).
set -eo pipefail
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}   # the directory holding this repository and darwin-renoir
DR=${DR:-$DK/darwin-renoir}
source $DR/xnu-env.sh
B=$DR/xnu-deps/build/mesa
SDK=${DARWIN_SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}   # (DARWIN_SDK: as kernel/build.sh takes it)
CC=$DR/xnu-deps/host/bin/xnu-clang
# pacstrip is built by darwin/userland/env.sh (arch/arm64/tools/pacstrip), or PACSTRIP=FILE; looked for before the build
PACSTRIP=${PACSTRIP:-$RN/../arch/arm64/tools/pacstrip}
[ -x "$PACSTRIP" ] || { echo "mesa26e.sh: no pacstrip at $PACSTRIP (darwin/userland/env.sh builds it)" >&2; exit 1; }
cd $B
[ -d mesa-26.2.5 ] || { echo "run darwin-renoir tools/mk-mesa.sh once first"; exit 1; }
export PATH=$B/venv/bin:$PATH
cat > cross-arm64e.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64e', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64e', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64e', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64e'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
BUILD=build-26.2.5-arm64e
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-26.2.5 --cross-file cross-arm64e.ini --wrap-mode=nofallback \
	--prefix=/hakios --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled \
	-Dlmsensors=disabled -Dbuild-tests=false > meson-setup-arm64e.log
ninja -C $BUILD src/gallium/targets/osmesa/libOSMesa.8.dylib > ninja-arm64e.log
O=$RN/userland/obj26; mkdir -p $O
llvm-strip -x -o $O/libOSMesa.8.dylib.tmp $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /hakios/lib/libOSMesa.8.dylib $O/libOSMesa.8.dylib.tmp
"$PACSTRIP" macho "$O/libOSMesa.8.dylib.tmp" "$O/libOSMesa.8.dylib" | tail -6
rm $O/libOSMesa.8.dylib.tmp
$THEOS/toolchain/linux/iphone/bin/ldid -S $O/libOSMesa.8.dylib
file -b $O/libOSMesa.8.dylib; ls -la $O/libOSMesa.8.dylib
