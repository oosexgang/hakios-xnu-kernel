#!/bin/bash
# Boot a kernel of this repository in QEMU's virt machine, as a phone boots it: a real UEFI starts the
# loader (loader/loader.efi), which takes the kernel from a payload and gives it the emulator's board.
# The machine has a display (ramfb) and a serial port; what the kernel says on either is kept.
#
#   tools/qemu-kernel.sh [options] KERNEL
#     --until TEXT      stop once the serial log has TEXT (default: when the kernel waits for its root, or
#                       panics)
#     --seconds N       stop after N seconds in any case (default 300)
#     --screen FILE     a picture of the screen, as a PNG, when it stops
#     --log FILE        the serial log (default: qemu-kernel.log in the working directory)
#     --boot-args ARGS  added to the kernel's boot arguments (-v, the verbose console on the screen, is
#                       there unless --quiet)
#     --ramdisk FILE    a disk image the kernel gets as /dev/md0 (its root: --boot-args rd=md0)
#     --cpus N          processors (default 1)
#     --quiet           no -v
#
# Needs qemu-system-aarch64 and an aarch64 UEFI (Debian/Ubuntu: qemu-system-arm qemu-efi-aarch64).
# The kernel: kernel/build.sh (with LLVM's tools or theos'); the loader: loader/build.sh, made here if
# it is not there.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
die() { echo "qemu-kernel: $*" >&2; exit 1; }
until_text='Waiting on <dict|panic\(' seconds=300 screen='' log=$PWD/qemu-kernel.log args='' ramdisk='' cpus=1 verbose=-v
while [ $# -gt 1 ]; do
	case $1 in
	--until) until_text=$2; shift 2 ;;
	--seconds) seconds=$2; shift 2 ;;
	--screen) screen=$(realpath -m "$2"); shift 2 ;;
	--log) log=$(realpath -m "$2"); shift 2 ;;
	--boot-args) args=$2; shift 2 ;;
	--ramdisk) ramdisk=$(realpath "$2"); shift 2 ;;
	--cpus) cpus=$2; shift 2 ;;
	--quiet) verbose=''; shift ;;
	*) die "unknown option $1 (the head of this file lists them)" ;;
	esac
done
[ $# -eq 1 ] || die "usage: tools/qemu-kernel.sh [options] KERNEL"
kernel=$(realpath "$1") || exit 1
command -v qemu-system-aarch64 > /dev/null || die "no qemu-system-aarch64 (apt: qemu-system-arm)"
firmware=
for code in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/edk2/aarch64/QEMU_EFI.fd; do
	[ -f "$code" ] && firmware=$code && break
done
[ -n "$firmware" ] || die "no aarch64 UEFI firmware (apt: qemu-efi-aarch64)"
[ -f "$HERE/loader/loader.efi" ] || "$HERE/loader/build.sh" > /dev/null || die "the loader does not build"

W=$(mktemp -d)
trap 'kill "$qemu" 2> /dev/null; rm -rf "$W"' EXIT
mkdir -p "$W/esp/EFI/BOOT"
cp "$HERE/loader/loader.efi" "$W/esp/EFI/BOOT/BOOTAA64.EFI"
cmdline="$verbose cpus=$cpus $args"
python3 "$HERE/loader/mkpayload.py" -o "$W/payload.bin" kernel="$kernel" ${ramdisk:+ramdisk="$ramdisk"} \
	"--cmdline=${cmdline# }" > /dev/null || die "no payload"
cp "${firmware%CODE.fd}VARS.fd" "$W/vars.fd" 2> /dev/null || truncate -s 64M "$W/vars.fd"
monitor=$W/monitor
: > "$log"
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp "$cpus" -m 6G -accel tcg,tb-size=384 \
	-drive if=pflash,format=raw,readonly=on,file="$firmware" -drive if=pflash,format=raw,file="$W/vars.fd" \
	-drive file=fat:rw:"$W/esp",format=raw,if=virtio -device virtio-rng-pci -device ramfb \
	-device loader,file="$W/payload.bin",addr=0xF0000000,force-raw=on \
	-display none -serial file:"$log" -monitor unix:"$monitor",server,nowait -no-reboot 2> "$W/qemu.err" &
qemu=$!
started=$SECONDS
reached=''
while kill -0 "$qemu" 2> /dev/null && [ $((SECONDS - started)) -lt "$seconds" ]; do
	if grep -aqE "$until_text" "$log"; then
		reached=1
		sleep 2                                         # (the screen catches up with the console)
		break
	fi
	sleep 1
done
if [ -n "$screen" ] && kill -0 "$qemu" 2> /dev/null; then
	python3 - "$monitor" "$screen" <<'PY'
import os, socket, struct, sys, time, zlib
monitor, out = sys.argv[1:3]
ppm = out + '.ppm'
s = socket.socket(socket.AF_UNIX)
s.connect(monitor)
s.sendall(f'screendump {ppm}\n'.encode())
for _ in range(50):
    time.sleep(0.2)
    if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
        time.sleep(0.5)
        break
data = open(ppm, 'rb').read()
os.unlink(ppm)
magic, size, maxval, pixels = data.split(b'\n', 3)
width, height = map(int, size.split())
stride = len(pixels) // height                          # (QEMU pads each row to a multiple of 4 bytes)
rows = b''.join(b'\0' + pixels[y * stride:y * stride + width * 3] for y in range(height))
chunk = lambda kind, body: struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                      chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))
PY
	echo "qemu-kernel: the screen: $screen"
fi
echo "qemu-kernel: the serial log: $log ($(wc -l < "$log") lines)"
if [ -n "$reached" ]; then
	echo "qemu-kernel: after $((SECONDS - started)) s: $(grep -aoE "$until_text" "$log" | head -1)"
else
	echo "qemu-kernel: \"$until_text\" not seen in $((SECONDS - started)) s"
	exit 1
fi
