#!/bin/bash
# tools/mk-mesa26.sh on Linux: Mesa 26.2.5 for the Darwin 26/27 userland, built without a Mac -- LLVM's clang
# and ld64.lld (Ubuntu's clang-20, lld-20) with the iPhoneOS 16.5 SDK theos installs. Out: libhakios-air.dylib
# (the library the Metal driver compiles and draws with, tools/mesa/air/air_metal.h) and its test program air_run.
#
# The same release, the same patch (tools/mesa26/darwin.patch) and the same frontend and libdrm stand-in
# (tools/mesa/air, tools/mesa/darwin-drm) as the Mac build. Two things differ, both because ld64.lld is not
# Apple's ld64:
#   - the code is built for arm64 and the header then says arm64e. ld64.lld does not link arm64e with chained
#     fixups, and with classic binding opcodes dyld refuses the library in an arm64e process ("bad bind opcode
#     0x00"); chained fixups of the plain arm64 kind it takes. The code has no pointer authentication, which is
#     what pacstrip makes of Darwin's own on these phones.
#   - the signature is ldid's (ad hoc), not codesign's.
#
# Needs: clang-20 and lld-20 (/usr/lib/llvm-20), bison, flex, ninja, python3; THEOS (default ~/theos) with
# sdks/iPhoneOS16.5.sdk and ldid. meson and Mako go into a private venv. INSTALL_DIR: the directory the
# library says it is in (default /hakios/lib).
#
# Out, in build/mesa26-linux/out: libhakios-air.dylib, air_run, built-from-air.
set -e
VERSION=26.2.5
SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/build/mesa26-linux
LLVM=${LLVM:-/usr/lib/llvm-20/bin}
THEOS=${THEOS:-$HOME/theos}
SDK=${SDK:-${DARWIN_SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}}
INSTALL_DIR=${INSTALL_DIR:-/hakios/lib}
LDID=${LDID:-$(command -v ldid || echo "$THEOS/toolchain/linux/iphone/bin/ldid")}
for need in "$LLVM/clang" "$LLVM/ld64.lld" "$SDK/usr/lib/libSystem.tbd"; do
	[ -e "$need" ] || { echo "mk-mesa26-linux.sh: $need is missing (LLVM=, THEOS=, SDK=)"; exit 1; }
done
mkdir -p "$B/out"
cd "$B"

PATCH_SUM=$(sha256sum "$ROOT/tools/mesa26/darwin.patch" | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.hakios-patch 2>/dev/null)" != "$PATCH_SUM" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		if [ -f "$ROOT/build/mesa-host/mesa-$VERSION.tar.xz" ]; then
			cp "$ROOT/build/mesa-host/mesa-$VERSION.tar.xz" .      # (the host build's, tools/air/mk-host.sh)
		else
			curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
		fi
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
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
export PATH=$B/venv/bin:$PATH

# the linker's flags go to the link only: on the compiler's line they fail every compile-only check meson
# makes (unused-command-line-argument), and a failed check reads as a missing feature (struct timespec)
cat > cross.ini <<INI
[binaries]
c = ['$LLVM/clang', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
cpp = ['$LLVM/clang++', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-stdlib=libc++']
objc = ['$LLVM/clang', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
ar = '$LLVM/llvm-ar'
ranlib = '$LLVM/llvm-ranlib'
strip = '$LLVM/llvm-strip'
pkg-config = 'false'

[built-in options]
c_link_args = ['-fuse-ld=lld', '-Wl,-fixup_chains']
cpp_link_args = ['-fuse-ld=lld', '-Wl,-fixup_chains']
objc_link_args = ['-fuse-ld=lld', '-Wl,-fixup_chains']

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
ninja -C build src/gallium/drivers/freedreno/libhakios-air.dylib src/gallium/drivers/freedreno/air_run > ninja.log ||
	{ grep -A20 FAILED ninja.log | head -30; echo "mk-mesa26-linux.sh: the build failed (ninja.log)"; exit 1; }

"$LLVM/llvm-strip" -x -o out/libhakios-air.dylib build/src/gallium/drivers/freedreno/libhakios-air.dylib
"$LLVM/llvm-install-name-tool" -id "$INSTALL_DIR/libhakios-air.dylib" out/libhakios-air.dylib
cp build/src/gallium/drivers/freedreno/air_run out/air_run
"$LLVM/llvm-install-name-tool" -change /hakios/lib/libhakios-air.dylib "$INSTALL_DIR/libhakios-air.dylib" out/air_run 2> /dev/null || true
# arm64e in the header (CPU_SUBTYPE_ARM64E with the pointer-authentication ABI bit), then signed: the
# signature covers the header
python3 - out/libhakios-air.dylib out/air_run <<'PY'
import struct, sys
for path in sys.argv[1:]:
    with open(path, 'r+b') as f:
        magic, cputype = struct.unpack('<II', f.read(8))
        assert magic == 0xfeedfacf and cputype == 0x0100000c, path
        f.write(struct.pack('<I', 0x80000002))
PY
"$LDID" -S out/libhakios-air.dylib out/air_run
cat "$ROOT"/tools/mesa/air/* | sha256sum > out/built-from-air
echo "mk-mesa26-linux.sh: Mesa $VERSION: $B/out/libhakios-air.dylib ($(stat -c %s out/libhakios-air.dylib) bytes," \
	"$("$LLVM/llvm-nm" -gU out/libhakios-air.dylib | grep -c ' _rwa_') rwa_* functions), $B/out/air_run"
