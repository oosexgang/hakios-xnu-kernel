#!/bin/bash
# Build Mesa for the iOS userland: the OpenGL ES library behind userland/src/eagl.c (see there and
# userland/src/gl_osmesa.c). Mesa is fetched (a pinned release), patched with tools/mesa/*.patch and
# built with the cross compiler of the kernel (xnu-deps/host/bin/xnu-clang, see xnu-deps/setup.sh)
# against the iOS SDK that userland/build.sh uses; nothing of it is in the repository.
#
#   DRIVERS="softpipe,freedreno"     (the Infinix: DRIVERS="softpipe,panfrost", see tools/mesa/panfrost-darwin.patch)
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
# Needs on this machine: meson, ninja, bison, flex, python3 with venv (Mako is installed into a
# private environment), llvm-ar.
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

# the compiler is the kernel's wrapper: it names the target (arm64-apple-ios) and the linker (ld64).
# Archiver and strip: LLVM's where there is one, otherwise Apple's (a Mac with only the command line tools)
AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip
command -v llvm-ar >/dev/null || { AR=ar; RANLIB=ranlib; STRIP=strip; }
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = '$AR'
ranlib = '$RANLIB'
strip = '$STRIP'
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
# panfrost travels in the image that goes into the Infinix's 32 MB boot partition: built for size
# (11 MB instead of 14.5), which costs the GPU nothing -- it is the GPU that draws
OPT=
case "$DRIVERS" in *panfrost*) OPT=-Doptimization=s ;; esac
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release $OPT -Db_ndebug=true \
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
	# beside the driver it draws with: freedreno's directory for the Adreno, panfrost's for the Mali
	case ",$DRIVERS," in *,panfrost,*) AIR_DIR=panfrost ;; *) AIR_DIR=freedreno ;; esac
	ninja -C $BUILD src/gallium/drivers/$AIR_DIR/air_run >> ninja.log
	cp $BUILD/src/gallium/drivers/$AIR_DIR/air_run out/air_run
	echo "mk-mesa.sh: $B/out/air_run ($(wc -c < out/air_run) bytes)"
fi

$STRIP -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /redwood/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml out/gl.xml
# (userland/build.sh comes here again when this no longer matches)
cat "$ROOT"/tools/mesa/*.patch "$ROOT"/tools/mesa/redwood-drm/* | sha256sum > out/built-from
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $B/out/libOSMesa.8.dylib, $(wc -c < out/libOSMesa.8.dylib | tr -d " ") bytes," \
	"$(wc -l < out/exported-gl.txt) gl* functions"
