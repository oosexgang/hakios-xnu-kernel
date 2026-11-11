#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application: clang and lld, no EDK2.
# platforms.h (the boards it knows) is made here, by tools/boards.py gen, from the device packages that
# are there (devices/, HAKIOS_DEVICES) and soc/: the loader knows the boards it was built with.
set -e
cd "$(dirname "$0")"
../tools/boards.py gen
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -I../build/boards -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
# /Brepro: no link time in the file, so the same sources give the same loader.efi byte for byte.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro /out:loader.efi loader.obj
ls -la loader.efi
