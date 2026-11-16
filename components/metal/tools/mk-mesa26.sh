#!/bin/bash
# Build Mesa 26.2.5 for the Darwin 26 userland on this Mac (Xcode's iPhoneOS SDK, arm64e): the library the
# Metal driver compiles and draws with, libhakios-air.dylib (tools/mesa/air/air_metal.h), and its test
# program air_run. 26.2 is the first Mesa whose freedreno knows the Adreno 830 (A8XX);
# tools/mk-mesa.sh is the older recipe for the SM7325's Adreno on Linux (Mesa 25.0.7, with OSMesa,
# which Mesa has since dropped).
#
# The sources are the release archive with tools/mesa26/darwin.patch on top (the Darwin stand-ins for
# DRM and sysinfo, the AIR frontend's meson, freedreno's air_run and libhakios-air targets, the ir3
# fixes that are still needed), plus tools/mesa/air (the frontend, copied to src/compiler/air) and
# tools/mesa/darwin-drm (libdrm's stand-in, src/darwin-drm), always the current ones.
#
# Needs: Xcode, Homebrew's llvm (llvm-ar), bison >= 3 and flex (brew install bison flex), ninja.
# meson and Mako go into a private venv here.
#
# Out, in xnu-deps/build/mesa26/out: libhakios-air.dylib (install name /hakios/lib/libhakios-air.dylib),
# air_run, built-from-air (what the library was built from, for ../../userland/userland/build.sh to compare).
set -e
VERSION=26.2.5
SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/../../userland/xnu-deps/build/mesa26
LLVM=/opt/homebrew/opt/llvm/bin
mkdir -p "$B/out"
cd "$B"
export PATH=/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$B/venv/bin:$PATH

PATCH_SUM=$(shasum -a 256 "$ROOT/tools/mesa26/darwin.patch" | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.hakios-patch 2>/dev/null)" != "$PATCH_SUM" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf mesa-$VERSION build
	tar xJf mesa-$VERSION.tar.xz
	patch -s -p1 -d mesa-$VERSION < "$ROOT/tools/mesa26/darwin.patch"
	echo "$PATCH_SUM" > mesa-$VERSION/.hakios-patch
fi
rm -rf mesa-$VERSION/src/darwin-drm mesa-$VERSION/src/compiler/air
cp -r "$ROOT/tools/mesa/darwin-drm" mesa-$VERSION/src/darwin-drm
cp -r "$ROOT/tools/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson mako pyyaml packaging
fi

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="'xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64e', '-isysroot', '$SDK', '-miphoneos-version-min=16.0'"
cat > cross.ini <<INI
[binaries]
c = [$CC]
cpp = ['xcrun', '--sdk', 'iphoneos', 'clang++', '-arch', 'arm64e', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
objc = [$CC]
ar = '$LLVM/llvm-ar'
ranlib = '$LLVM/llvm-ranlib'
strip = '$LLVM/llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
[ -f build/build.ninja ] || meson setup build mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/hakios --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= \
	-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled -Dlmsensors=disabled \
	-Dbuild-tests=false > meson-setup.log
ninja -C build src/gallium/drivers/freedreno/libhakios-air.dylib src/gallium/drivers/freedreno/air_run > ninja.log || { grep -A20 FAILED ninja.log | head -30; echo "mk-mesa26.sh: the build failed (ninja.log)"; exit 1; }

$LLVM/llvm-strip -x -o out/libhakios-air.dylib build/src/gallium/drivers/freedreno/libhakios-air.dylib
install_name_tool -id /hakios/lib/libhakios-air.dylib out/libhakios-air.dylib 2>/dev/null
cp build/src/gallium/drivers/freedreno/air_run out/air_run
# an ad-hoc signature: arm64e code on Darwin is not run unsigned by dyld, whatever this kernel's AMFI says
codesign -f -s - out/libhakios-air.dylib out/air_run 2>/dev/null
cat "$ROOT"/tools/mesa/air/* | shasum -a 256 > out/built-from-air
echo "mk-mesa26.sh: Mesa $VERSION: $B/out/libhakios-air.dylib ($(stat -f %z out/libhakios-air.dylib) bytes," \
	"$(nm -gU out/libhakios-air.dylib | grep -c ' _rwa_') rwa_* functions), $B/out/air_run"
