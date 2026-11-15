#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application: clang and lld, no EDK2.
# platforms.h (the boards it knows) is made here, by tools/boards.py gen, from the device packages that
# are there (devices/, HAKIOS_DEVICES): the loader knows the boards it was built with.
#
# LOADER_IDENTITY=FILE: the identity Darwin is told (a header mkidentity.py made from another iPhone's device tree) in
# place of identity.h -- the one of the iPhone whose files the system image holds. Darwin 27.2's 24B119 for iPhone12,1
# needs that iPhone's (DeviceTree.n104ap.im4p): with identity.h's, SpringBoard asks for exclave-backed indicators that
# device has, and stops (docs/darwin26-boot.md, "Darwin 27 in QEMU").
#
# LOADER_CFLAGS adds to the compiler's flags: where the boot shim of a phone's UEFI puts the payload
# elsewhere than at 0xF0000000, or reserves more than 64 MB for it, say so here, e.g.
#   LOADER_CFLAGS="-DPAYLOAD_BASE=0xE8000000ULL -DPAYLOAD_MAX_SIZE=0x08000000ULL" loader/build.sh
# (loader/src/conventions.h has both).
set -e
cd "$(dirname "$0")"
../tools/boards.py gen
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -I../build/boards ${LOADER_CFLAGS:-} \
  ${LOADER_IDENTITY:+-DLOADER_IDENTITY="\"$(realpath "$LOADER_IDENTITY")\""} -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
# /Brepro: no link time in the file, so the same sources give the same loader.efi byte for byte.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro /out:loader.efi loader.obj
ls -la loader.efi
