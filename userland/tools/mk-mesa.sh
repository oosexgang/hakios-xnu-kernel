#!/bin/bash
# Build Mesa for the iOS userland: the OpenGL ES library behind userland/src/eagl.c (see there and
# userland/src/gl_osmesa.c). Mesa is fetched (a pinned release), patched with tools/mesa/*.patch and
# built with the cross compiler of the kernel (xnu-deps/host/bin/xnu-clang, see xnu-deps/setup.sh)
# against the iOS SDK that userland/build.sh uses; nothing of it is in the repository.
#
#   DRIVERS="softpipe,freedreno"
#                        the gallium drivers to build. softpipe: a rasterizer in plain C; it needs no
#                        LLVM and nothing of the machine but its processor. freedreno: the Adreno's;
#                        it talks to the kernel's GPU driver (the service RedwoodGPU) through
#                        tools/mesa/redwood-drm, which stands in for libdrm (tools/mesa/freedreno-darwin.patch
#                        makes Mesa build it where there is no DRM). The library takes the GPU when the
#                        kernel has one and the environment does not say OSMESA_SOFTWARE=1.
#
# What comes out, in xnu-deps/build/mesa/out:
#   libOSMesa.8.dylib    everything in one library (install name /redwood/lib/libOSMesa.8.dylib): the
#                        OpenGL (ES) entry points gl*, the compiler of the shading language, the drivers,
#                        and the off-screen interface that hands out contexts
#   exported-gl.txt      the gl* functions it exports, and
#   gl.xml               their prototypes (the Khronos registry as Mesa has it): for userland/mkeagl.py
# and with AIR=1, the two things the Metal subsystem needs (docs/air.md, docs/metal.md 10.12):
#   air_run              the test program: a Metal shader translated, drawn or dispatched, read back
#   libredwoodair.dylib  the same path as a library (tools/mesa/air/air_metal.h), install name
#                        /redwood/lib/libredwoodair.dylib, which is what userland's Metal driver
#                        links against to compile a function Metal hands it
# Needs on this machine: meson, ninja, bison, flex, python3 with venv (Mako is installed into a
# private environment), llvm-ar.
#
# This build is NOT reproducible byte for byte, so do not compare one of its outputs with another
# by hash. Measured on 20 October: two builds from the same source tree, the second with
# build-softpipe-freedreno deleted first, gave different libOSMesa.8.dylib and air_run -- same size
# to the byte, 9 132 774 of 14 855 048 bytes different in the library and 4 171 752 of 9 789 824 in
# air_run, with 5 of the 6 named text symbols at new addresses. So the layout moves between builds.
# What moves it was not established; it is not source filenames, which -Db_ndebug=true compiles out.
#
# This is Mesa's property and not the build system's, which is worth knowing before anyone
# distrusts every hash here: the userland programs reproduce exactly. prebuilt/userland/init built
# in one working copy came out byte for byte identical to the same source built in another, at a
# different time -- so there a hash does compare builds, and only for Mesa it does not.
#
# The practical consequence: a hash answers whether this is the same FILE, never whether it holds
# the same CODE, and the size answers neither -- the one-line ir3 rounding patch
# (tools/mesa/ir3-round-default.patch) produced a library of exactly the same 14 855 048 bytes as
# the build without it. To tell whether a patch is in a build, read the source in
# xnu-deps/build/mesa/mesa-$VERSION, which is where this script unpacks and patches it.
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
B=$ROOT/xnu-deps/build/mesa
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/xnu-deps/host/bin/xnu-clang
mkdir -p "$B/out"
cd "$B"

# The sources are the archive's with tools/mesa/*.patch on top. When the patches change, a tree that was
# patched with the earlier ones is of no use (and builds without a word into a library that does not match
# the rest): the sources are taken from the archive again and everything is built anew, about a minute.
PATCHES=$(cat "$ROOT"/tools/mesa/*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.redwood-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/tools/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$PATCHES" > mesa-$VERSION/.redwood-patches
fi
# ours, kept in the repository: always the current ones
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$ROOT/tools/mesa/redwood-drm" mesa-$VERSION/src/redwood-drm
# the AIR frontend (docs/air.md): the Metal runtime will call it in this
# library's own process, and tools/mesa/air-frontend.patch is the meson that
# reaches it. Copied rather than linked, because this tree is cross-built.
rm -rf mesa-$VERSION/src/compiler/air
cp -r "$ROOT/tools/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

# the compiler is the kernel's wrapper: it names the target (arm64-apple-ios) and the linker (ld64)
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${DRIVERS//,/-}
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log
# air_run draws with a translated Metal shader and reads the pixels back; it is
# the only way to find out whether the Adreno takes work that did not come from
# the window server. Built only when asked, since it is a test program.
if [ "${AIR:-}" = 1 ]; then
	ninja -C $BUILD src/gallium/drivers/freedreno/air_run >> ninja.log
	cp $BUILD/src/gallium/drivers/freedreno/air_run out/air_run
	echo "mk-mesa.sh: $B/out/air_run ($(stat -c %s out/air_run) bytes)"
	# and the same path as a library, which is not a test program: the Metal driver links
	# against it to compile the bitcode Metal hands a function. Stripped and named the way
	# libOSMesa is below -- -x keeps the global symbols, which here are the rwa_* of
	# tools/mesa/air/air_metal.h, and the install name has to be the path on the device
	# because meson's own is @rpath.
	ninja -C $BUILD src/gallium/drivers/freedreno/libredwoodair.dylib >> ninja.log
	llvm-strip -x -o out/libredwoodair.dylib $BUILD/src/gallium/drivers/freedreno/libredwoodair.dylib
	install_name_tool -id /redwood/lib/libredwoodair.dylib out/libredwoodair.dylib
	echo "mk-mesa.sh: $B/out/libredwoodair.dylib ($(stat -c %s out/libredwoodair.dylib) bytes," \
		"$(nm -gU out/libredwoodair.dylib | grep -c ' _rwa_') rwa_* functions)"
	# what this library was built from, so userland/build.sh can tell that it is older than the
	# sources and come back here instead of shipping it beside a driver that disagrees with it
	cat "$ROOT"/tools/mesa/air/* | sha256sum > out/built-from-air
fi

llvm-strip -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /redwood/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml out/gl.xml
# (userland/build.sh comes here again when this no longer matches)
cat "$ROOT"/tools/mesa/*.patch "$ROOT"/tools/mesa/redwood-drm/* | sha256sum > out/built-from
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $B/out/libOSMesa.8.dylib, $(stat -c %s out/libOSMesa.8.dylib) bytes," \
	"$(wc -l < out/exported-gl.txt) gl* functions"
