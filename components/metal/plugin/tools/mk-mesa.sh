#!/bin/bash
# Mesa's shader compiler for the driver on the phone: the AIR front end (tools/mesa/air) and ir3 with the
# patches in tools/mesa, cross-built for iOS with the kernel's compiler. What `make ios MESA=1` links
# (tools/compiler.mk: HAKIOS_METAL_IOS_MESA_LIBS) and nothing else: the archives of the front end, ir3,
# NIR and Mesa's utilities. No driver of Mesa's and no OpenGL comes out of it.
#
#   tools/mk-mesa.sh            into $XNUDEPS/build/mesa: mesa-VERSION/ (the sources, patched) and
#                               build-$IOS_ARCH/ (the archives)
#
#   XNUDEPS     where the kernel's build keeps what it needs (kernel/env.sh sets it): the cross compiler is
#               $XNUDEPS/host/bin/xnu-clang
#   IOS_ARCH    arm64e (the default: iOS 26 and 27 load nothing else into their processes) or arm64 (16.5)
#   THEOS       theos, for iPhoneOS16.5.sdk
#
# Mesa's release archive is taken from $XNUDEPS/build/mesa when it is there, else fetched from
# archive.mesa3d.org; either way it is checked against the hash below. The sources are unpacked and patched
# again whenever a patch changes; the front end is copied in at every run, so an edit of tools/mesa/air is
# always in the next build. Needs meson, ninja, bison, flex, python3 with venv (Mako and PyYAML go into a
# private environment), llvm-ar.
set -euo pipefail
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
: "${XNUDEPS:?set XNUDEPS (source kernel/env.sh)}"
: "${THEOS:?set THEOS (source kernel/env.sh)}"
IOS_ARCH=${IOS_ARCH:-arm64e}
CC=$XNUDEPS/host/bin/xnu-clang
SDK=${DARWIN_SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}   # (DARWIN_SDK: as kernel/build.sh takes it)
[ -x "$CC" ] || { echo "mk-mesa.sh: no $CC: build the kernel first (kernel/build.sh)" >&2; exit 1; }
[ -d "$SDK" ] || { echo "mk-mesa.sh: no $SDK" >&2; exit 1; }
B=$XNUDEPS/build/mesa
mkdir -p "$B"
cd "$B"

hash256() {
	if [ "$(uname -s)" = Darwin ]; then shasum -a 256 "$@"; else sha256sum "$@"; fi
}
check_hash256() {
	if [ "$(uname -s)" = Darwin ]; then shasum -a 256 -c -s; else sha256sum -c --quiet; fi
}

# the patches of a build of the compiler; ir3-dump.patch is not one of them (it is the host's fixture writer,
# applied by tools/kernels.mk to a copy of ir3_cmdline.c)
PATCHES=(air-frontend.patch air-ir3.patch freedreno-darwin.patch darwin-sdk.patch ir3-round-default.patch
	ir3-shift-split.patch ir3-ssbo-subdword.patch)
STAMP=$(cd "$HERE/mesa" && cat "${PATCHES[@]}" | hash256 | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.hakios-patches 2>/dev/null)" != "$STAMP" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | check_hash256
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "${PATCHES[@]}"; do
		patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$HERE/mesa/$p"
	done
	echo "$STAMP" > mesa-$VERSION/.hakios-patches
fi
# ours: copied in at every run (freedreno-darwin.patch's meson names src/hakios-drm, so it has to be there
# for the build to be set up, though nothing of it is compiled here)
rm -rf mesa-$VERSION/src/compiler/air mesa-$VERSION/src/hakios-drm
cp -r "$HERE/mesa/air" mesa-$VERSION/src/compiler/air
cp -r "$HERE/mesa/hakios-drm" mesa-$VERSION/src/hakios-drm

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH
cat > cross-$IOS_ARCH.ini <<INI
[binaries]
c = ['$CC', '-arch', '$IOS_ARCH', '-isysroot', '$SDK', '-D_DARWIN_C_SOURCE']
cpp = ['$CC++', '-arch', '$IOS_ARCH', '-isysroot', '$SDK', '-D_DARWIN_C_SOURCE', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', '$IOS_ARCH', '-isysroot', '$SDK', '-D_DARWIN_C_SOURCE']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = '$IOS_ARCH'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
# set up as the OpenGL build of the first generation was (softpipe and freedreno behind OSMesa), which is the
# configuration known to set up on this target; only the compiler's archives are built from it
BUILD=build-$IOS_ARCH
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross-$IOS_ARCH.ini --wrap-mode=nofallback \
	--prefix=/hakios --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled \
	-Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled \
	-Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
	-Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup-$IOS_ARCH.log
# the archives tools/compiler.mk links, in its order (MESA_ARCHIVE_PATHS of tools/kernels.mk)
ARCHIVES="src/compiler/air/libair.a $(awk '/^MESA_ARCHIVE_PATHS *=/ { on = 1; next }
	on { line = $0; more = line ~ /\\[ \t]*$/; sub(/\\[ \t]*$/, "", line); print line; if (!more) exit }' \
	"$HERE/kernels.mk" | tr -s ' \t\n' ' ')"
ninja -C $BUILD $ARCHIVES > ninja-$IOS_ARCH.log
for a in $ARCHIVES; do
	[ -s $BUILD/$a ] || { echo "mk-mesa.sh: $BUILD/$a was not made (ninja-$IOS_ARCH.log)" >&2; exit 1; }
done
echo "mk-mesa.sh: Mesa $VERSION's compiler for $IOS_ARCH iOS: $(echo $ARCHIVES | wc -w) archives in $B/$BUILD"
