#!/bin/bash
# Boot the XNU loader + kernel payload under QEMU (virt, GICv3, UEFI at EL1).
# usage: run.sh [seconds] [extra qemu args...]
# env: QEMU_TB_MB           megabytes for QEMU's cache of translated code (default 384; QEMU's own
#                           default of 1 GB mostly costs host memory)
#      QEMU_SMP             processor cores of the machine (default 1); the kernel uses as many of them as
#                           its boot-arg cpus=N says (CMDLINE="cpus=4" for run-ios.sh), one without it
#      QEMU_EFI, QEMU_VARS  the UEFI firmware for aarch64 guests (default: where Arch's edk2-aarch64
#                           puts it; Debian/Ubuntu: /usr/share/AAVMF/AAVMF_CODE.fd and AAVMF_VARS.fd)
cd "$(dirname "$0")"
T=${1:-20}; shift
EFI=${QEMU_EFI:-/usr/share/edk2/aarch64/QEMU_EFI.fd}
[ -f "$EFI" ] || { echo "run.sh: no UEFI firmware at $EFI (set QEMU_EFI and QEMU_VARS)"; exit 1; }
cp "${QEMU_VARS:-/usr/share/edk2/aarch64/QEMU_VARS.fd}" vars.fd 2>/dev/null
# the loader is the boot application of the little FAT volume QEMU makes of esp/:
# the one built here, or else the one that comes with the repository
L=../xnuloader/loader.efi; [ -f $L ] || L=../prebuilt/loader.efi
mkdir -p esp/EFI/BOOT && cp -u $L esp/EFI/BOOT/BOOTAA64.EFI
timeout "$T" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp ${QEMU_SMP:-1} -m ${QEMU_MEM:-6G} \
  -accel tcg,tb-size=${QEMU_TB_MB:-384} \
  -drive if=pflash,format=raw,readonly=on,file="$EFI" \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=fat:rw:esp,format=raw,if=virtio \
  -device loader,file=payload.bin,addr=0xF0000000,force-raw=on \
  -display none -serial stdio -monitor none -no-reboot "$@" 2>&1
