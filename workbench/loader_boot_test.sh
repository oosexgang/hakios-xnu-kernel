#!/bin/bash
# The loader as it boots: built for the emulator's board, started by a real UEFI in QEMU's virt machine,
# with a stand-in kernel (one segment, a branch to itself) in the payload. What is held: the identity it
# makes of identity=SEED is the one tools/identity.py says of that seed (the serial number and the ECID it
# prints, which are what it writes into the device tree); without a seed it makes another at each boot;
# and it goes on to the kernel.
#
#   workbench/loader_boot_test.sh LOADER.EFI
#
# Left out (exit 0, saying so) where qemu-system-aarch64 or an aarch64 UEFI firmware is not installed.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
LOADER=${1:?usage: loader_boot_test.sh LOADER.EFI}
FIRMWARE=
for code in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/edk2/aarch64/QEMU_EFI.fd; do
	[ -f "$code" ] && FIRMWARE=$code && break
done
if ! command -v qemu-system-aarch64 > /dev/null || [ -z "$FIRMWARE" ]; then
	echo "loader_boot_test: no qemu-system-aarch64 or aarch64 UEFI firmware here: left out"
	exit 0
fi
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
failed=0
check() {
	local what=$1
	shift
	if "$@"; then echo "ok   $what"; else echo "FAIL $what"; failed=1; fi
}

# the stand-in kernel: a Mach-O the loader takes (its text above the kernel's base, an entry point), whose
# entry is a branch to itself
python3 - "$W/kernel" <<'PY'
import struct, sys
va, size = 0xFFFFFFF007004000, 0x4000
segment = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__TEXT', va, size, 0, size, 5, 5, 0, 0)
registers = [0] * 33
registers[32] = va + 0x1000                                     # pc
thread = struct.pack('<IIII', 0x5, 16 + 68 * 4, 6, 68) + struct.pack('<33Q', *registers) + bytes(68 * 4 - 33 * 8)
header = struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, 2, 2, len(segment) + len(thread), 0, 0)
image = bytearray(size)
image[:len(header) + len(segment) + len(thread)] = header + segment + thread
image[0x1000:0x1004] = struct.pack('<I', 0x14000000)             # b .
open(sys.argv[1], 'wb').write(image)
PY
mkdir -p "$W/esp/EFI/BOOT"
cp "$LOADER" "$W/esp/EFI/BOOT/BOOTAA64.EFI"

# boot: until the loader says it jumps to the kernel (or a minute), what it said
boot() {
	local args=$1 log=$2
	python3 "$HERE/../loader/mkpayload.py" -o "$W/payload.bin" kernel="$W/kernel" ${args:+--cmdline "$args"} > /dev/null
	cp "${FIRMWARE%CODE.fd}VARS.fd" "$W/vars.fd" 2> /dev/null || truncate -s 64M "$W/vars.fd"
	timeout 120 qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -m 4G \
		-drive if=pflash,format=raw,readonly=on,file="$FIRMWARE" -drive if=pflash,format=raw,file="$W/vars.fd" \
		-drive file=fat:rw:"$W/esp",format=raw,if=virtio -device virtio-rng-pci \
		-device loader,file="$W/payload.bin",addr=0xF0000000,force-raw=on \
		-display none -serial stdio -monitor none -no-reboot > "$log" 2>&1 &
	local qemu=$!
	for _ in $(seq 120); do
		grep -aq "jumping to" "$log" && break
		sleep 1
	done
	kill "$qemu" 2> /dev/null
	wait "$qemu" 2> /dev/null
}

expected=$(python3 "$HERE/../tools/identity.py" --seed 0123456789abcdef)
serial=$(echo "$expected" | awk '/serial number/ { print $3 }')
ecid=$(echo "$expected" | awk '/ECID/ { sub(/^0x/, "", $2); print $2 }')
boot "identity=0123456789abcdef" "$W/seeded.log"
check "the seed's identity: serial number $serial, ECID $ecid, as tools/identity.py says" \
	grep -aq "identity: serial number $serial, ECID $ecid" "$W/seeded.log"
check "taken as given, not random" bash -c "! grep -aq 'a random one' '$W/seeded.log'"
check "and on to the kernel" grep -aq "jumping to" "$W/seeded.log"
boot "" "$W/first.log"
boot "" "$W/second.log"
first=$(grep -a -o "identity: serial number [0-9A-Z]*" "$W/first.log")
second=$(grep -a -o "identity: serial number [0-9A-Z]*" "$W/second.log")
check "without a seed, a random one, said to be: $first" grep -aq "a random one" "$W/first.log"
check "of the form: 10 of the alphabet without I and O" \
	bash -c "[[ '${first##* }' =~ ^[0-9A-HJ-NP-Z]{10}\$ ]]"
check "another at the next boot: $second" test -n "$second" -a "$first" != "$second"
[ $failed = 0 ] && echo "loader_boot_test: ok"
exit $failed
