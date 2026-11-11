#!/bin/bash
# Mesa for THIS machine (Linux) with the AIR front end in it: the build `make test MESA=1`, `make sweep MESA=1`
# and the fixture writers (tools/kernels.mk) compile and link against. The one that goes on the phone is
# tools/mk-mesa.sh; both take the front end from tools/mesa/air, so what is tested here is what runs there.
#
#   tools/mk-mesa-host.sh       into $XNUDEPS/build/mesa-host: mesa-VERSION/ (patched, src/compiler/air a link
#                               back to tools/mesa/air: an edit of the front end needs no copying) and build/
#
# What comes out, in build/: the archives the driver links (tools/compiler.mk), ir3_compiler (AIR -> NIR -> Adreno)
# and air2nir (the front end alone). Only air-*.patch and ir3-*.patch are applied: the front end, its ir3 entry
# point and ir3's fixes, as in the cross build; freedreno-darwin.patch is the Darwin build's alone.
#
# XNUDEPS as kernel/env.sh sets it. Needs meson, ninja, bison, flex, python3 with venv, libdrm (freedreno's).
set -euo pipefail
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
: "${XNUDEPS:?set XNUDEPS (source kernel/env.sh)}"
B=$XNUDEPS/build/mesa-host
SRC=$B/mesa-$VERSION
mkdir -p "$B"
cd "$B"
PATCHES=$(cd "$HERE/mesa" && cat air-*.patch ir3-round-default.patch ir3-shift-split.patch ir3-ssbo-subdword.patch | sha256sum | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.hakios-patches" 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		# (the cross build may have it already)
		if [ -f "$XNUDEPS/build/mesa/mesa-$VERSION.tar.xz" ]; then
			cp "$XNUDEPS/build/mesa/mesa-$VERSION.tar.xz" .
		else
			curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
		fi
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	for p in air-frontend.patch air-ir3.patch ir3-round-default.patch ir3-shift-split.patch ir3-ssbo-subdword.patch; do
		patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$HERE/mesa/$p"
	done
	# glibc 2.43 and later declare C11's once_flag and call_once in <stdlib.h>, which clashes with Mesa 25.0's own
	# definitions in src/c11/threads.h; only a build against glibc meets it
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
	rm -rf "$SRC/src/compiler/air"
	ln -s "$HERE/mesa/air" "$SRC/src/compiler/air"
	echo "$PATCHES" > "$SRC/.hakios-patches"
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
ARCHIVES="src/compiler/air/libair.a $(awk '/^MESA_ARCHIVE_PATHS *=/ { on = 1; next }
	on { line = $0; more = line ~ /\\[ \t]*$/; sub(/\\[ \t]*$/, "", line); print line; if (!more) exit }' \
	"$HERE/kernels.mk" | tr -s ' \t\n' ' ')"
# shellcheck disable=SC2086  # (a list of targets)
ninja -C build $ARCHIVES src/compiler/air/air2nir src/gallium/drivers/freedreno/ir3_compiler > ninja.log
echo "mk-mesa-host.sh: Mesa $VERSION with the AIR front end for this machine in $B/build"
