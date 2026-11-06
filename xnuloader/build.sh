#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
DEVICE=${1:-x6826b}
case "$DEVICE" in
  x6826b) TARGET_FLAGS="-DXNU_PHONE_X6826B"; OUTPUT=loader ;;
  redwood) TARGET_FLAGS=; OUTPUT=loader ;;
  renoir) TARGET_FLAGS="-DXNU_PHONE_RENOIR"; OUTPUT=loader-renoir ;;
  *) echo "usage: $0 [x6826b|redwood|renoir]" >&2; exit 1 ;;
esac
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter $TARGET_FLAGS -c loader.c -o "${OUTPUT}.obj"
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:"${OUTPUT}.efi" "${OUTPUT}.obj"
ls -la "${OUTPUT}.efi"
