#!/bin/bash
# Boot the XNU loader + kernel payload under QEMU (virt, GICv3, UEFI at EL1).
# usage: run.sh [seconds] [extra qemu args...]
# env: QEMU_TB_MB           megabytes for QEMU's cache of translated code (default 384; QEMU's own
#                           default of 1 GB mostly costs host memory)
#      QEMU_SMP             processor cores of the machine (default 1); the kernel uses as many of them as
#                           its boot-arg cpus=N says (CMDLINE="cpus=4" for run-darwin.sh), one without it
#      QEMU_ESP=1           no loader device: the payload is the file \EFI\hakios\payload.bin on the ESP, which the loader
#                           reads itself (UEFI-native boot; what a boot menu or a USB stick does)
#      QEMU_EFI, QEMU_VARS  the UEFI firmware for aarch64 guests (found by itself where Arch's edk2-aarch64
#                           puts it, Debian/Ubuntu's /usr/share/AAVMF/AAVMF_CODE.fd and AAVMF_VARS.fd, or
#                           Homebrew's share/qemu/edk2-aarch64-code.fd on a Mac)
#      QEMU_VARS_RESET=1    start from an empty variable store: vars.fd is this machine's NVRAM and it
#                           persists, so one boot that reaches the EFI shell sends every later boot there
cd "$(dirname "$0")"
T=${1:-20}; shift
# the UEFI firmware: where Arch's edk2-aarch64 puts it, where Debian and Ubuntu's AAVMF does, or
# where Homebrew's qemu puts it on a Mac (only the code half -- see vars.fd below)
EFI=${QEMU_EFI-}; VARS_SRC=${QEMU_VARS-}
if [ -z "$EFI" ]; then
	for f in /usr/share/edk2/aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd \
	         /opt/homebrew/share/qemu/edk2-aarch64-code.fd /usr/local/share/qemu/edk2-aarch64-code.fd; do
		[ -f "$f" ] && { EFI=$f; break; }
	done
fi
if [ -z "$VARS_SRC" ]; then
	for f in /usr/share/edk2/aarch64/QEMU_VARS.fd /usr/share/AAVMF/AAVMF_VARS.fd; do [ -f "$f" ] && { VARS_SRC=$f; break; }; done
fi
[ -f "$EFI" ] || { echo "run.sh: no UEFI firmware found (looked for edk2-aarch64's, AAVMF's and Homebrew's); set QEMU_EFI and QEMU_VARS"; exit 1; }
# vars.fd is this machine's NVRAM: QEMU writes the UEFI boot order back into it and it survives the
# run. Homebrew ships only the code half of the firmware, so on a Mac there is nothing to copy -- and
# a silent "cp" of nothing leaves whatever the last boot wrote. That matters, because one boot that
# ends in the EFI shell writes Boot0002 "EFI Internal Shell" to the front of the boot order and every
# later boot goes straight back there, loader untouched, with nothing to say why. An empty store (all
# ones, as flash erases to) is what the firmware initialises on a first boot, so make one when there
# is no source for it, or when asked.
if [ -f "$VARS_SRC" ] && [ -z "${QEMU_VARS_RESET-}" ]; then
	cp "$VARS_SRC" vars.fd
elif [ ! -f vars.fd ] || [ -n "${QEMU_VARS_RESET-}" ]; then
	python3 -c 'open("vars.fd","wb").write(b"\xff" * (64 << 20))'
fi
# the loader is the boot application of the little FAT volume QEMU makes of esp/:
# the one built here, or else the one that comes with the repository
L=../xnuloader/loader.efi; [ -f $L ] || L=../prebuilt/loader.efi
mkdir -p esp/EFI/BOOT && cp $L esp/EFI/BOOT/BOOTAA64.EFI
# The payload (kernel, root image, command line) reaches the loader in one of two ways. By default QEMU's loader
# device puts it in memory at 0xF0000000, as the boot shim does behind Project Silicium's image on the phone.
# With QEMU_ESP=1 it is not put anywhere: it is the file \EFI\hakios\payload.bin of the ESP, and the loader reads it
# itself from the volume it was started from -- UEFI-native boot, what a boot menu or a USB stick does (docs/uefi-boot.md).
PAYLOAD_DEVICE=(-device loader,file=payload.bin,addr=0xF0000000,force-raw=on)
if [ -n "${QEMU_ESP:-}" ]; then
	mkdir -p esp/EFI/hakios && cp payload.bin esp/EFI/hakios/payload.bin
	PAYLOAD_DEVICE=()
fi
timeout "$T" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp ${QEMU_SMP:-1} -m ${QEMU_MEM:-6G} \
  -accel tcg,tb-size=${QEMU_TB_MB:-384} \
  -drive if=pflash,format=raw,readonly=on,file="$EFI" \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=fat:rw:esp,format=raw,if=virtio \
  "${PAYLOAD_DEVICE[@]}" \
  -display none -serial stdio -monitor none -no-reboot "$@" 2>&1
