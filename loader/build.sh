#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application: clang and lld, no EDK2.
# platforms.h (the boards it knows) is made here, by tools/boards.py gen, from the device packages that
# are there (devices/, HAKIOS_DEVICES) and soc/: the loader knows the boards it was built with.
#
# Which iPhone iOS is told it runs on is identity.h, made here from the device tree of your own IPSW: the one
# ios/tools/unpack.sh leaves as $IOS/DeviceTree.bin (IOS: ios27 beside the repository), or IDENTITY=FILE.
# Without one the loader gets a placeholder with nothing of Apple's in it: the open kernel starts, iOS does not.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DT=${IDENTITY:-${IOS:-$HERE/../../ios27}/DeviceTree.bin}
cd "$HERE"
if [ -s "$DT" ]; then
	python3 mkidentity.py "$DT" > identity.h.new
elif [ ! -f identity.h ] || grep -q -- '--placeholder' identity.h; then
	python3 mkidentity.py --placeholder > identity.h.new
	echo "build.sh: no iPhone device tree ($DT): the loader is built with a placeholder identity, and iOS will not start" >&2
fi
if [ -f identity.h.new ]; then
	if cmp -s identity.h.new identity.h; then rm identity.h.new; else mv identity.h.new identity.h; fi
fi
../tools/boards.py gen
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -I../build/boards -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
# /Brepro: no link time in the file, so the same sources give the same loader.efi byte for byte.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro /out:loader.efi loader.obj
ls -la loader.efi
