#!/bin/bash
# Build the driver of the phone's Wi-Fi radio for our kernel: ath11k, Qualcomm's driver for the
# WCN6750 (and its kin) as it is in Linux, unmodified, on a layer that gives it the kernel
# interfaces it expects. Three things meet here, and only the third is in the repository:
#
#   Linux's ath11k        drivers/net/wireless/ath/ath11k of a pinned release of Linux
#                         (BSD-3-Clause-Clear; the one header of ath/ it includes is ISC);
#   FreeBSD's LinuxKPI    the headers with which FreeBSD compiles Linux drivers, with the kernel
#                         headers those are written against (BSD-2-Clause, of a pinned release
#                         of FreeBSD); patched with wifi/lx/patches/*.patch. Its source files
#                         are not used: what they do is done in wifi/lx/src;
#   wifi/lx               ours: the headers LinuxKPI lacks (device tree, platform devices), the
#                         headers of FreeBSD that cannot serve under XNU as they are, and the
#                         functions underneath (locks, sleeping, work, memory, firmware, the
#                         radio's bus) written on the handful of calls that the kernel's
#                         bsd/dev/redwood_lx.c offers (bsd/sys/redwood_lx.h of the kernel tree).
#
# Not all of the driver is built yet: DRIVER below lists the files that are. What they call in
# the files that are not becomes a function that says its name in the log and answers 0
# (out/stubs.txt lists them), unless wifi/lx/src has one that does more.
#
# Everything is compiled as FreeBSD kernel code for an Apple arm64 kernel and tied into one
# object that shows nothing but its entry points; the kernel links it when it is there
# (xnu-build.sh).
#
#   VERSION   of Linux. 7.0.14: LinuxKPI's 802.11 headers follow the mac80211 of Linux 7.0, and
#             the driver has to be the one they were written for. For the WCN6750 the driver
#             asks the same of firmware and platform as in the 6.6 series.
#   FREEBSD   15.1-RELEASE, the first release whose LinuxKPI is at Linux 7.0.
#
# What comes out, in xnu-deps/build/ath11k:
#   src/ath11k/        the driver's files, untouched       freebsd/sys/   FreeBSD's, patched
#   out/ath11k.o       the object for the kernel           out/stubs.txt  what is not there yet
# Needs on this machine: curl, tar, xz, sha256sum, patch, awk, clang, and the kernel's linker
# (xnu-deps/host/bin/ld64, see xnu-deps/setup.sh). The two archives are 150 and 253 MB and are kept.
#
# usage: tools/mk-ath11k.sh [fetch]        "fetch" stops when the driver's sources are there
set -e
VERSION=7.0.14
SHA256=de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/xnu-deps/build/ath11k
ATH=drivers/net/wireless/ath
mkdir -p "$B"
cd "$B"

if [ "$(cat src/.release 2>/dev/null)" != "$VERSION $SHA256" ]; then
	[ -f linux-$VERSION.tar.xz ] || curl -sL --fail -o linux-$VERSION.tar.xz \
		https://cdn.kernel.org/pub/linux/kernel/v${VERSION%%.*}.x/linux-$VERSION.tar.xz
	echo "$SHA256  linux-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf src unpacked
	mkdir unpacked
	tar xJf linux-$VERSION.tar.xz -C unpacked --strip-components=1 linux-$VERSION/$ATH/ath11k \
		linux-$VERSION/$ATH/spectral_common.h
	# every file must say itself that it may be used so: the licence is checked, not assumed
	for f in unpacked/$ATH/ath11k/*.[ch] unpacked/$ATH/spectral_common.h; do
		case "$(head -6 "$f")" in
		*"SPDX-License-Identifier: BSD-3-Clause-Clear"*) ;;
		*"Permission to use, copy, modify, and/or distribute this software"*) ;;      # (ISC, spelt out)
		*) echo "mk-ath11k.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mkdir src
	mv unpacked/$ATH/ath11k src/ath11k
	mv unpacked/$ATH/spectral_common.h src/
	rm -rf unpacked
	echo "$VERSION $SHA256" > src/.release
fi
[ "$1" = fetch ] && { echo "mk-ath11k.sh: ath11k of Linux $VERSION: $B/src/ath11k"; exit 0; }
W=$ROOT/wifi/lx

# --- FreeBSD's LinuxKPI and the kernel headers under it, with our patches on top. When the patches
# change, the tree is taken from the archive again (as tools/mk-mesa.sh does with Mesa's).
FREEBSD=15.1
FREEBSD_SHA256=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1
FSYS="compat/linuxkpi sys arm64/include net net80211 netinet netinet6 vm kern tools libkern ddb
	security fs/pseudofs crypto opencrypto geom bsm isa cam rpc xdr x86/include ofed/include
	dev/pci dev/ofw dev/fdt dev/acpica dev/usb dev/iicbus dev/backlight
	contrib/ck/include contrib/dev/acpica/include contrib/libnv"
PATCHES=$(cat "$W"/patches/*.patch 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat freebsd/.release 2>/dev/null)" != "$FREEBSD $FREEBSD_SHA256 $PATCHES" ]; then
	[ -f freebsd-$FREEBSD-src.txz ] || curl -sL --fail -o freebsd-$FREEBSD-src.txz \
		https://download.freebsd.org/releases/amd64/$FREEBSD-RELEASE/src.txz
	echo "$FREEBSD_SHA256  freebsd-$FREEBSD-src.txz" | sha256sum -c --quiet
	rm -rf freebsd gen obj
	mkdir freebsd
	tar xJf freebsd-$FREEBSD-src.txz -C freebsd --strip-components=2 usr/src/COPYRIGHT \
		$(for d in $FSYS; do echo usr/src/sys/$d; done)
	for p in "$W"/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d freebsd -p1 < "$p"
	done
	echo "$FREEBSD $FREEBSD_SHA256 $PATCHES" > freebsd/.release
fi
S=$B/freebsd/sys

# --- what FreeBSD's build would have generated: the interfaces of its device framework, and the
# option headers, which here say only that there is an Internet
if [ ! -d gen ]; then
	mkdir gen
	(
		cd gen
		ln -s $S/arm64/include machine
		for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m dev/pci/pci_iov_if.m dev/pci/pcib_if.m \
			dev/iicbus/iicbus_if.m dev/iicbus/iicbb_if.m dev/backlight/backlight_if.m dev/usb/usb_if.m \
			dev/ofw/ofw_bus_if.m; do
			awk -f $S/tools/makeobjops.awk $S/$m -h
		done
		for o in h p q; do awk -f $S/tools/vnode_if.awk $S/kern/vnode_if.src -$o; done
		for o in $(grep -rhoE "opt_[a-z0-9_]+\.h" $S | sort -u) lkpi_iic_if.h; do : > $o; done
		printf '#define INET 1\n#define INET6 1\n' > opt_inet.h
		printf '#define INET6 1\n' > opt_inet6.h
		printf '#define SMP 1\n' > opt_global.h
	)
fi

# --- compile: the driver's files that are built so far, and ours
DRIVER="core hal qmi ce hw fw htc wmi mac reg peer dp dp_rx dp_tx hal_rx hal_tx dbring p2p"
CC="clang -target arm64-apple-ios16.5"
LD=$ROOT/xnu-deps/host/bin/ld64
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/include -Igen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Isrc/ath11k -Isrc -include $W/include/lx_prelude.h -DCONFIG_ATH11K_AHB -DCONFIG_ATH11K_DEBUG
	-DKBUILD_MODNAME=\"ath11k\""
# two headers are the kernel's: what it offers this object (wifi/lx/include/lx_os.h), and its QMI
# helpers (wifi/lx/include/linux/soc/qcom/qmi.h)
source "$ROOT/xnu-env.sh"
for h in lx qmi; do
	[ -f "$XNU_SRC/bsd/sys/redwood_$h.h" ] || { echo "mk-ath11k.sh: the kernel tree has no bsd/sys/redwood_$h.h" >&2; exit 1; }
done
DFLAGS="$DFLAGS -DLX_REDWOOD_LX_H=\"$XNU_SRC/bsd/sys/redwood_lx.h\" -DLX_REDWOOD_QMI_H=\"$XNU_SRC/bsd/sys/redwood_qmi.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
for f in $DRIVER; do compile src/ath11k/$f.c obj/ath11k_$f.o $DFLAGS & done
for f in "$W"/src/*.c; do [ -f "$f" ] && compile "$f" obj/lx_$(basename "$f" .c).o $DFLAGS -I$W/src & done
wait
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
OBJECTS=$(for f in $DRIVER; do echo obj/ath11k_$f.o; done; for f in "$W"/src/*.c; do echo obj/lx_$(basename "$f" .c).o; done)

# --- tie it all into one object. What it then still wants is one of three things: a call of the
# kernel's (redwood_lx.h; the QMI helpers; the handful of C library functions XNU has under the same
# names with the same meaning), a function of a part of the driver that is not built yet, or a
# mistake. The second kind gets a stub each that says its name and answers 0; anything else
# missing ends the build.
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
llvm-nm -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|qmi_*|redwood_qmi_*|section\$*) continue ;;
	ath11k_*|__ath11k_*) ;;
	*) echo "mk-ath11k.sh: nothing defines $s" >&2; missing=1; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
[ -z "$missing" ] || exit 1
$CC -c out/stubs.s -o out/stubs.o
# four names are shown: where the kernel enters (lx_prepare(), lx_start(), lx_stop() and lx_transmit() of redwood_lx.h)
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/ath11k.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-ath11k.sh: out/ath11k.o: $(llvm-size out/ath11k.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$(echo $DRIVER | wc -w) of the driver's files; $(wc -l < out/stubs.txt) functions of the others are stubs (out/stubs.txt);" \
	"asks the kernel for $(llvm-nm -u --format=just-symbols out/ath11k.o | grep -vc 'section\$') names"
