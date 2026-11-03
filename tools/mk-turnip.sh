#!/bin/bash
# Build Turnip, Mesa's Vulkan driver for the Adreno, for the Darwin userland: the first piece of a Metal that runs on
# Vulkan (UniXNU's AdrenoMetal: metal2vulkan over Turnip). The tree is tools/mk-mesa.sh's (the pinned release with
# tools/mesa/*.patch on, turnip-darwin.patch among them); run that first. Turnip's msm backend talks to the kernel's
# GPU driver through tools/mesa/hakios-drm, the stand-in for libdrm, whose syncobjs (Turnip's fences and semaphores)
# are kept in the process over the kernel's fences.
#
# What comes out: xnu-deps/build/mesa/out/libvulkan_freedreno.dylib (install name /redwood/lib/libvulkan_freedreno.dylib),
# loaded by its vk_icdGetInstanceProcAddr like any Vulkan driver.
#
# Needs on this machine what mk-mesa.sh needs, and glslangValidator (apt: glslang-tools) for Turnip's own shaders.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=25.0.7
B=$ROOT/xnu-deps/build/mesa
BUILD=build-turnip
cd "$B"
[ -d mesa-$VERSION ] && [ -f cross.ini ] || { echo "mk-turnip.sh: no Mesa tree in $B: tools/mk-mesa.sh first" >&2; exit 1; }
grep -q with_freedreno_vk mesa-$VERSION/meson.build ||
	{ echo "mk-turnip.sh: the tree has not got turnip-darwin.patch: tools/mk-mesa.sh again" >&2; exit 1; }
command -v glslangValidator >/dev/null || { echo "mk-turnip.sh: no glslangValidator (apt: glslang-tools)" >&2; exit 1; }
export PATH=$B/venv/bin:$PATH
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dopengl=false -Dgles1=disabled \
	-Dgles2=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dzstd=disabled -Dxmlconfig=disabled \
	-Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= \
	-Dtools= -Dbuild-tests=false > turnip-setup.log
ninja -C $BUILD src/freedreno/vulkan/libvulkan_freedreno.dylib > turnip-ninja.log
mkdir -p out
cp $BUILD/src/freedreno/vulkan/libvulkan_freedreno.dylib out/
install_name_tool -id /redwood/lib/libvulkan_freedreno.dylib out/libvulkan_freedreno.dylib 2>/dev/null ||
	"$ROOT/xnu-deps/host/bin/install_name_tool" -id /redwood/lib/libvulkan_freedreno.dylib out/libvulkan_freedreno.dylib
echo "mk-turnip.sh: Turnip $VERSION: $B/out/libvulkan_freedreno.dylib ($(stat -c %s out/libvulkan_freedreno.dylib) bytes)"
