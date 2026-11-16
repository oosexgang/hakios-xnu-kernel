#!/bin/bash
# Build Mesa for THIS machine (x86-64 Linux) with the AIR frontend in it: the
# frontend itself, ir3 (so a Metal shader can be compiled for the Adreno
# without the phone) and softpipe (so it can be run without a GPU at all).
# This is the frontend's development and test build. The one that goes on the
# phone is tools/mk-mesa26.sh, which cross-compiles for Darwin with the same
# release and the same patch (tools/mesa26/darwin.patch: the frontend's meson,
# air_run, libhakios-air and air_metal_test, and the ir3 fixes; its Darwin parts
# are conditions on the host system and change nothing here). Both take the
# frontend from tools/mesa/air, so what is tested here is what runs there.
#
# Nothing of Mesa is in the repository: the pinned release is fetched, patched,
# and src/compiler/air is a symbolic link back to tools/mesa/air so that editing
# the frontend needs no copying.
#
# What comes out, in build/mesa-host/build:
#   src/compiler/air/air2nir                           the frontend alone
#   src/gallium/drivers/freedreno/air_run              AIR -> NIR -> drawn, pixel read back
#   src/gallium/drivers/freedreno/libhakios-air.so     the same path as the library the Metal
#                                                      driver links (tools/mesa/air/air_metal.h)
#   src/gallium/drivers/freedreno/air_metal_test       that library driven through its own header
# (Mesa 25.0's ir3_compiler, AIR -> NIR -> Adreno text, came from air-ir3.patch, which the 26.2 patch
# does not carry: libhakios-air runs ir3 itself, rwa_ir3_cost.)
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
VERSION=26.2.5
SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5
B=$ROOT/build/mesa-host
SRC=$B/mesa-$VERSION
mkdir -p "$B"
cd "$B"

PATCH_SUM=$(sha256sum "$ROOT/tools/mesa26/darwin.patch" | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.air-patches" 2>/dev/null)" != "$PATCH_SUM" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		# the cross build may already have fetched it
		if [ -f "$ROOT/../../userland/xnu-deps/build/mesa26/mesa-$VERSION.tar.xz" ]; then
			cp "$ROOT/../../userland/xnu-deps/build/mesa26/mesa-$VERSION.tar.xz" .
		else
			curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
		fi
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$ROOT/tools/mesa26/darwin.patch"
	echo "$PATCH_SUM" > "$SRC/.air-patches"
fi
# the frontend itself, as a link: no copy to forget to refresh
rm -rf "$SRC/src/compiler/air"
ln -s "$ROOT/tools/mesa/air" "$SRC/src/compiler/air"

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

[ -f build/build.ninja ] || meson setup build "$SRC" --buildtype=debugoptimized \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm \
	-Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
	-Dopengl=true -Dgles2=enabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= \
	-Dgallium-va=disabled \
	-Dlmsensors=disabled -Dbuild-tests=true -Dzstd=disabled -Dxmlconfig=disabled \
	-Dexpat=disabled > meson-setup.log

ninja -C build src/compiler/air/air2nir \
	src/gallium/drivers/freedreno/air_run \
	src/gallium/drivers/freedreno/air_metal_test > ninja.log

echo "mk-host.sh: $B/build/src/compiler/air/air2nir"
echo "mk-host.sh: $B/build/src/gallium/drivers/freedreno/air_run"
# air_metal_test links the library, so asking for the test builds it too
echo "mk-host.sh: $B/build/src/gallium/drivers/freedreno/air_metal_test"
