#!/bin/bash
# The kernel booted in QEMU as a phone boots it, to its first process: the loader under a real UEFI, the
# kernel's own start on two processors, the emulator's board found, its virtio network card and input
# devices taken, the root mounted from the payload's RAM disk (tools/qemu-root.sh: nothing of Apple's),
# and /sbin/launchd run. Its shell then answers on the console with the kernel's values; pings QEMU's
# gateway and serves this machine a page over TCP; reads a second disk on the virtio block device; plays a
# tone on the virtio sound device (QEMU writes it to a WAV file); reads a button and a touch from QEMU's
# keyboard and tablet; starts a process and waits for it; reads the kernel's messages; and sends itself
# Mach messages.
#
#   workbench/qemu_boot_test.sh KERNEL
#
# KERNEL is what kernel/build.sh made (../xnu-12377-hakios/BUILD/obj/mach.development.bcm2837); left out
# (exit 0, saying so) where there is none, or no QEMU, or no ld64.lld to build the root's program with.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KERNEL=${1:-}
L=${XNU_CLANG_DIR:-$(dirname "$(command -v clang 2> /dev/null || echo /nonexistent/clang)")}
if [ -z "$KERNEL" ] || [ ! -f "$KERNEL" ] || ! command -v qemu-system-aarch64 > /dev/null || [ ! -x "$L/ld64.lld" ]; then
	echo "qemu_boot_test: no kernel, QEMU or ld64.lld here: left out"
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
XNU_CLANG_DIR=$L "$HERE/../tools/qemu-root.sh" "$W/root.img" > /dev/null || { echo "FAIL the root does not build"; exit 1; }
# a second disk, on the virtio block device
mkdir -p "$W/data"
echo "a second disk, on QEMU's virtio block device" > "$W/data/hello.txt"
python3 "$HERE/../userland/tools/apfs/mkapfs.py" -o "$W/data.img" Data="$W/data" > /dev/null
"$HERE/../tools/qemu-kernel.sh" --cpus 2 --ramdisk "$W/root.img" --disk "$W/data.img" --audio "$W/sound.wav" --boot-args rd=md0 --until 'a shell on the console|panic\(' \
	--seconds 240 --type 'sysctl hw.ncpu hw.board.name kern.ostype\nifconfig en0 10.0.2.15\nping 10.0.2.2 2\nserve 1\nmount apfs /dev/rvblk0 /mnt\ncat /mnt/hello.txt\ntone 440 1\ninput 4\nrun /sbin/launchd ps\ndmesg 3\nmach 200\n' \
	--forward 18080:80 --then "sleep 5; curl -s -m 30 http://127.0.0.1:18080/test > '$W/page'; QEMU_QMP='$W/qmp' python3 '$HERE/../base/qemu/touch.py' key home tap 0.5 0.25" \
	--qmp "$W/qmp" --after 8 \
	--log "$W/serial.log" "$KERNEL" > "$W/out" 2>&1
log=$W/serial.log
check "the loader takes the kernel and jumps to it" grep -aq "jumping to" "$log"
check "the kernel starts and says its version" grep -aq "^Darwin Kernel Version" "$log"
check "it finds the emulator's board" grep -aq "^board: qemu-virt" "$log"
check "its second processor joins it" grep -aq "2 processors running" "$log"
check "its network card becomes en0, link up" grep -aq "en0 attached on QEMU's virtio network device.*link up" "$log"
check "both input devices are found" test "$(grep -ac "virtio-input: device in slot" "$log")" = 2
check "the root is mounted from the RAM disk" grep -aq 'apfs: volume 0 "Root"' "$log"
check "its first process runs, as pid 1" grep -aq "user space: /sbin/launchd, pid 1" "$log"
check "... and hears from the kernel what it is" grep -aq "board      qemu-virt" "$log"
check "its shell reads the console and answers with the kernel's values, typed" \
	bash -c "tr -d '\\r' < '$log' | grep -qx 'hw.ncpu: 2' && tr -d '\\r' < '$log' | grep -qx 'kern.ostype: Darwin'"
check "en0 takes an address and comes up" bash -c "tr -d '\\r' < '$log' | grep -qx $'\\tinet 10.0.2.15'"
check "QEMU's gateway answers its pings, through the virtio network driver" grep -aq "2 sent, 2 answered" "$log"
check "its TCP serves a page to this machine, through QEMU's forwarded port" grep -q "^Served by /sbin/launchd" "$W/page"
check "... which it saw asked for" grep -aq "GET /test HTTP" "$log"
check "a second disk on the virtio block device is mounted, APFS, and read" \
	grep -aq "^a second disk, on QEMU's virtio block device" "$log"
check "its sound device plays a tone of 440 Hz, through the virtio sound driver" \
	bash -c "python3 '$HERE/tone_in_wav.py' '$W/sound.wav' 440 > /dev/null"
check "a button pressed on the virtio keyboard reaches the input device" \
	bash -c "tr -d '\\r' < '$log' | grep -qx 'button down home' && tr -d '\\r' < '$log' | grep -qx 'button up home'"
check "... and a touch on the virtio tablet, where it was made" grep -aq "touch down finger 0 at 3276[0-9],1638[0-9]" "$log"
check "it starts a process of its own (posix_spawn), which runs as its child" grep -aqE "^ +[0-9]+ +1 +0  launchd" "$log"
check "... and waits for it to end, as it ended" grep -aqE "^run: pid [0-9]+ exited with 0" "$log"
check "the kernel's message buffer is read (dmesg)" bash -c "tr -d '\\r' < '$log' | sed -n '/^# dmesg/,\$p' | grep -qE '^\\[ *[0-9]+\\.[0-9]+\\]: '"
check "Mach messages go to a port of its own and come back as they went" grep -aq "^mach: 200 messages to itself through a port" "$log"
check "no panic" bash -c "! grep -aq 'panic(' '$log'"
if [ $failed != 0 ]; then
	echo "    (the serial log's last lines)"
	tail -15 "$log" | sed 's/^/    /'
fi
exit $failed
