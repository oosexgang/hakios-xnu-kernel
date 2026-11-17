#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application: clang and lld, no EDK2.
# platforms.h (the boards it knows) is made here, by tools/gen_headers.py, from the device packages that
# are there (arch/arm64/boot/dts/, HAKIOS_DEVICES): the loader knows the boards it was built with.
#
# LOADER_IDENTITY=FILE: the identity Darwin is told (a header mkidentity.py made from another iPhone's device tree) in
# place of identity.h -- the one of the iPhone whose files the system image holds. Darwin 27.2's 24B119 for iPhone12,1
# needs that iPhone's (DeviceTree.n104ap.im4p): with identity.h's, SpringBoard asks for exclave-backed indicators that
# device has, and stops (Documentation/darwin26-boot.md, "Darwin 27 in QEMU").
#
# LOADER_CFLAGS adds to the compiler's flags: where the boot shim of a phone's UEFI puts the payload
# elsewhere than at 0xF0000000, or reserves more than 64 MB for it, say so here, e.g.
#   LOADER_CFLAGS="-DPAYLOAD_BASE=0xE8000000ULL -DPAYLOAD_MAX_SIZE=0x08000000ULL" arch/arm64/boot/build.sh
# (arch/arm64/boot/src/conventions.h has both).
#
# Which iPhone Darwin is told it runs on is identity.h, made here from the device tree of your own IPSW: the one
# darwin/tools/unpack.sh leaves as $IOS/DeviceTree.bin (IOS: ios27 beside the repository), or IDENTITY=FILE.
# Without one the loader gets a placeholder with nothing of Apple's in it: the open kernel starts, Darwin does not.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)     # (the repository: this is arch/arm64/boot)
# the compiler, the linker, the load address, more flags: build.config.loader (LOADER_BUILD_CONFIG: another file of the top)
HAKIOS=$TOP
export HAKIOS
# shellcheck source=../../../build.config.loader
. "$TOP/${LOADER_BUILD_CONFIG:-build.config.loader}"
DT=${IDENTITY:-${IOS:-$TOP/../ios27}/DeviceTree.bin}
cd "$HERE"
if [ -s "$DT" ]; then
	python3 mkidentity.py "$DT" > identity.h.new
elif [ ! -f identity.h ] || grep -q -- '--placeholder' identity.h; then
	python3 mkidentity.py --placeholder > identity.h.new
	echo "build.sh: no iPhone device tree ($DT): the loader is built with a placeholder identity, and Darwin will not start" >&2
fi
if [ -f identity.h.new ]; then
	if cmp -s identity.h.new identity.h; then rm identity.h.new; else mv identity.h.new identity.h; fi
fi
"$TOP/tools/gen_headers.py" > /dev/null
# shellcheck disable=SC2086  # (LOADER_CFLAGS: flags, one a word)
"$LOADER_CC" --target="$LOADER_TARGET" -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -I"$TOP/build/boards" $LOADER_CFLAGS \
  ${LOADER_IDENTITY:+-DLOADER_IDENTITY="\"$(realpath "$LOADER_IDENTITY")\""} -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
# /Brepro: no link time in the file, so the same sources give the same loader.efi byte for byte.
"$LOADER_LINK" /subsystem:efi_application /entry:efi_main /nodefaultlib /base:"$LOADER_BASE" /Brepro /out:loader.efi loader.obj
ls -la loader.efi
