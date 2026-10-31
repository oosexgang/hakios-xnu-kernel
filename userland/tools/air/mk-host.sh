#!/bin/bash
# Build Mesa for THIS machine (x86-64 Linux) with the AIR frontend in it: the
# frontend itself, ir3 (so a Metal shader can be compiled for the Adreno
# without the phone) and softpipe (so it can be run without a GPU at all).
#
# This is the frontend's development and test build. The one that goes on the
# phone is tools/mk-mesa.sh, which cross-compiles for iOS; both take the
# frontend from tools/mesa/air, so what is tested here is what runs there.
#
# Nothing of Mesa is in the repository: the pinned release is fetched, patched
# with tools/mesa/*.patch, and src/compiler/air is a symbolic link back to
# tools/mesa/air so that editing the frontend needs no copying.
#
# What comes out, in build/mesa-host/build:
#   src/compiler/air/air2nir                           the frontend alone
#   src/gallium/drivers/freedreno/ir3_compiler         AIR -> NIR -> Adreno
#   src/gallium/drivers/freedreno/air_run              AIR -> NIR -> drawn, pixel read back
#   src/gallium/drivers/freedreno/libredwoodair.so     the same path as the library the Metal
#                                                      driver links (tools/mesa/air/air_metal.h)
#   src/gallium/drivers/freedreno/air_metal_test       that library driven through its own header
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
B=$ROOT/build/mesa-host
SRC=$B/mesa-$VERSION
mkdir -p "$B"
cd "$B"

# air-*.patch is the frontend and the ir3 entry point; ir3-*.patch is a fix to ir3 itself, and it
# belongs here as much as in the cross build -- this tool is where the corpus's "compiled by ir3"
# column comes from and where its Adreno instructions are read, so a host ir3 that differs from the
# device's would make both of those say something about a compiler nothing runs. The other patches
# in tools/mesa are for the Darwin cross build and are deliberately left out.
PATCHES=$(cat "$ROOT"/tools/mesa/air*.patch "$ROOT"/tools/mesa/ir3-*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.air-patches" 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		# the cross build may already have fetched it
		if [ -f "$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz" ]; then
			cp "$ROOT/xnu-deps/build/mesa/mesa-$VERSION.tar.xz" .
		else
			curl -sL --fail -o mesa-$VERSION.tar.xz \
				https://archive.mesa3d.org/mesa-$VERSION.tar.xz
		fi
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/tools/mesa/air-*.patch "$ROOT"/tools/mesa/ir3-*.patch; do
		patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"
	done

	# glibc 2.43 and later declare C11's once_flag and call_once in <stdlib.h>,
	# which clashes with Mesa 25.0's own definitions in src/c11/threads.h. Only
	# this host build hits it; the iOS cross build has its own libc.
	python3 - "$SRC" <<-'PY'
		import sys
		root = sys.argv[1]
		p = root + '/src/c11/threads.h'
		s = open(p).read()
		old = "typedef pthread_once_t  once_flag;\n#  define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
		new = ("#  if defined(__GLIBC__) && __GLIBC_USE(ISOC23) && !defined(__cplusplus)\n"
		       "#    define C11_LIBC_HAS_CALL_ONCE 1\n"
		       "#  else\n"
		       "typedef pthread_once_t  once_flag;\n"
		       "#    define ONCE_FLAG_INIT PTHREAD_ONCE_INIT\n"
		       "#  endif\n")
		if old in s:
		    open(p, 'w').write(s.replace(old, new))
		p = root + '/src/c11/impl/threads_posix.c'
		s = open(p).read()
		old = "call_once(once_flag *flag, void (*func)(void))\n{\n    pthread_once(flag, func);\n}\n"
		if old in s and 'C11_LIBC_HAS_CALL_ONCE' not in s:
		    i = s.index(old)
		    j = s.rfind('\n\n', 0, i)
		    s = (s[:j] + "\n\n#ifndef C11_LIBC_HAS_CALL_ONCE" + s[j:i + len(old)] +
		         "#endif\n" + s[i + len(old):])
		    open(p, 'w').write(s)
	PY

	# the frontend itself, as a link: no copy to forget to refresh. The one
	# line of meson that reaches it comes from tools/mesa/air-frontend.patch,
	# applied above, so this build and the cross build get it the same way.
	rm -rf "$SRC/src/compiler/air"
	ln -s "$ROOT/tools/mesa/air" "$SRC/src/compiler/air"

	echo "$PATCHES" > "$SRC/.air-patches"
fi

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

[ -f build/build.ninja ] || meson setup build "$SRC" --buildtype=debugoptimized \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm \
	-Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
	-Dopengl=true -Dgles2=enabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= \
	-Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
	-Dlmsensors=disabled -Dbuild-tests=true -Dzstd=disabled -Dxmlconfig=disabled \
	-Dexpat=disabled > meson-setup.log

ninja -C build src/compiler/air/air2nir \
	src/gallium/drivers/freedreno/ir3_compiler \
	src/gallium/drivers/freedreno/air_run \
	src/gallium/drivers/freedreno/air_metal_test > ninja.log

echo "mk-host.sh: $B/build/src/compiler/air/air2nir"
echo "mk-host.sh: $B/build/src/gallium/drivers/freedreno/ir3_compiler"
echo "mk-host.sh: $B/build/src/gallium/drivers/freedreno/air_run"
# air_metal_test links the library, so asking for the test builds it too
echo "mk-host.sh: $B/build/src/gallium/drivers/freedreno/air_metal_test"
