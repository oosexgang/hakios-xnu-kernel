#!/bin/bash
# The kernel booted in QEMU as a phone boots it, to its first process: the loader under a real UEFI, the
# kernel's own start on two processors, the emulator's board found, its virtio devices taken, the root
# mounted from the payload's RAM disk (tools/qemu-root.sh: nothing of Apple's), and /sbin/launchd run.
# What is typed on its shell then tries the rest of the kernel:
#   - the network: pings to QEMU's gateway; a page served to this computer and one asked of it (TCP both
#     ways); a datagram answered (workbench/qemu_host.py is this computer's side)
#   - the devices: a second disk (virtio block, APFS); a tone (virtio sound, which QEMU writes to a WAV
#     file); a button and a touch from QEMU's keyboard and tablet
#   - processes: one started and waited for, one ended by a signal, two by their own mistakes; signals
#     caught by a handler; a pipe between two; a descriptor sent over a socket pair; threads on both
#     processors at once; Mach messages; kqueue's timer and a pipe's readiness
#   - files and memory: a file written, read and removed in ramfs; a program's file mapped; memory of its
#     own taken, used and given back; the kernel's messages read; the date
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
# what it asks this computer for (workbench/qemu_host.py serves it)
mkdir -p "$W/www"
echo "a page from the computer QEMU runs on" > "$W/www/page.txt"
# what is typed on its console, a line at a time (--type takes them joined by a written \n)
typed=(
	'sysctl hw.ncpu hw.board.name kern.ostype'
	'ifconfig en0 10.0.2.15'
	'ping 10.0.2.2 2'
	'serve 1'
	'get 10.0.2.2 18081 /page.txt'
	'udp 10.0.2.2 18082 a datagram'
	'mount apfs /dev/rvblk0 /mnt'
	'cat /mnt/hello.txt'
	'tone 440 1'
	'input 4'
	'run /sbin/launchd ps'
	'dmesg 3'
	'mach 200'
	'date'
	'spawn /sbin/launchd sleep 60'
	'kill ! 15'
	'wait'
	'pipe through the kernel'
	'mount ramfs ramfs /tmp'
	'mkdir /tmp/notes'
	'write /tmp/notes/a kept in memory'
	'cat /tmp/notes/a'
	'rm /tmp/notes/a'
	'ls /tmp/notes'
	'threads 2'
	'mmap /sbin/launchd'
	'kevent 10'
	'run /sbin/launchd crash null'
	'run /sbin/launchd crash illegal'
	'alarm 5'
	'memory 128'
	'unix /etc/motd'
)
"$HERE/../tools/qemu-kernel.sh" --cpus 2 --ramdisk "$W/root.img" --disk "$W/data.img" --audio "$W/sound.wav" --boot-args rd=md0 --until 'a shell on the console|panic\(' \
	--seconds 240 --type "$(printf '%s\\n' "${typed[@]}")" \
	--forward 18080:80 --then "python3 '$HERE/qemu_host.py' '$W/www' 18081 18082 120 & sleep 5; curl -s -m 30 http://127.0.0.1:18080/test > '$W/page'; QEMU_QMP='$W/qmp' python3 '$HERE/../base/qemu/touch.py' key home tap 0.5 0.25; wait" \
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
check "it asks this computer for a page over TCP, as a client" bash -c "tr -d '\\r' < '$log' | grep -q '^get: HTTP/1.0 200 OK, 38 bytes of body' &&
	tr -d '\\r' < '$log' | grep -qx 'a page from the computer QEMU runs on'"
check "a datagram goes to this computer and its answer comes back" grep -aq "^udp: 18 bytes back from 10.0.2.2: echoed: a datagram" "$log"
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
check "it knows the date (QEMU's clock)" grep -aqE "^$(date -u +%Y)-[0-9]{2}-[0-9]{2} [0-9:]{8} UTC" "$log"
check "a signal ends a process in the background, and wait says so" grep -aqE "^wait: pid [0-9]+ ended by signal 15" "$log"
check "a pipe carries what a child writes into it" grep -aqE "^pipe: 19 bytes from pid [0-9]+: through the kernel" "$log"
check "a file is written in the kernel's file system in memory (ramfs), read back, and removed" \
	bash -c "tr -d '\\r' < '$log' | grep -qx 'kept in memory' && tr -d '\\r' < '$log' | sed -n '/^# ls \\/tmp\\/notes/,\$p' | grep -c '  \\.\\.\\?\$' | grep -qx 2"
# (two threads on two processors take well under the time of one doing it all: they do run side by side)
check "threads of its own run on both processors at once" bash -c "grep -aoE '^threads: 2 threads on 2 processors did in [0-9]+ ms what one does in [0-9]+ ms' '$log' |
	awk '{ exit !(\$9 * 10 < \$15 * 8) }'"
check "a program's file mapped (its pages from APFS) is what reading it gives" grep -aq "^mmap: /sbin/launchd, [0-9]* bytes: its pages and its reads agree" "$log"
check "kqueue delivers a timer's events on time, and a pipe's readiness" \
	bash -c "grep -aoE '^kevent: 10 timer events of 50 ms in [0-9]+ ms; a pipe with 5 bytes in it is readable' '$log' | awk '{ exit !(\$9 >= 450 && \$9 < 1500) }'"
check "a process's mistakes reach it as signals: a bad address SIGSEGV, an undefined instruction SIGILL" \
	bash -c "grep -aqE 'run: pid [0-9]+ ended by signal 11' '$log' && grep -aqE 'run: pid [0-9]+ ended by signal 4' '$log'"
check "a timer's signals are caught by a handler and the thread goes back where it was (sigreturn)" \
	bash -c "grep -aoE '^alarm: 5 SIGALRM caught by a handler in [0-9]+ ms' '$log' | awk '{ exit !(\$9 >= 450 && \$9 < 1500) }'"
check "memory of its own is given, holds what is written, and is all given back" \
	bash -c "grep -aoE '^memory: 128 MB written and read back in [0-9]+ ms, all as written; free: [0-9]+ MB before, [0-9]+ MB with it, [0-9]+ MB after' '$log' |
	awk '{ exit !(\$15 - \$18 >= 120 && \$22 - \$15 > -8) }'"
check "a descriptor sent over a socket pair (SCM_RIGHTS) comes out and reads the file it was" \
	grep -aq '^unix: "sent" and descriptor [0-9]* came through; it reads: This root was made by tools/qemu-root.sh' "$log"
check "no panic" bash -c "! grep -aq 'panic(' '$log'"
if [ $failed != 0 ]; then
	echo "    (the serial log's last lines)"
	tail -15 "$log" | sed 's/^/    /'
fi
exit $failed
