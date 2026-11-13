#!/bin/bash
# The kernel booted in QEMU as a phone boots it, to its first process: the loader under a real UEFI, the
# kernel's own start on two processors, the emulator's board found, its virtio devices taken, the root
# mounted from the payload's RAM disk (tools/qemu-root.sh: nothing of Apple's), and /sbin/launchd run.
# What is typed on its shell then tries the rest of the kernel:
#   - the network: pings to QEMU's gateway; pages served to this computer and asked of it, 4 MB each way
#     (TCP both ways); a datagram answered (workbench/qemu_host.py is this computer's side)
#   - the devices: a second disk (virtio block, APFS), read whole and through its file system; a tone
#     (virtio sound, which QEMU writes to a WAV file); a button and a touch from QEMU's keyboard and tablet
#   - processes: one started and waited for, one forked (its memory copied) and made another program,
#     one ended by a signal, two by their own mistakes; signals caught by a handler; a pipe between two;
#     a descriptor sent over a socket pair; threads on both processors at once, and taking turns under one
#     lock; Mach messages; kqueue's timer and a pipe's readiness
#   - IOKit: its registry walked in MIG messages, as IOKit.framework asks it; the serial number the
#     loader made read from it; a picture drawn on a surface and swapped onto the screen
#   - files and memory: a file written, read and removed in ramfs, linked, renamed and linked to; a program's file mapped; memory of its
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
# FILE's FNV-1a hash, as the shell's hash and get say it
fnv1a() {
	python3 -c 'import sys
h = 0xcbf29ce484222325
for b in open(sys.argv[1], "rb").read():
    h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
print("%016x" % h)' "$1"
}
# a second disk, on the virtio block device
mkdir -p "$W/data"
echo "a second disk, on QEMU's virtio block device" > "$W/data/hello.txt"
head -c 3000000 /dev/urandom > "$W/data/big.bin"
python3 "$HERE/../userland/tools/apfs/mkapfs.py" -o "$W/data.img" Data="$W/data" > /dev/null
disk_hash=$(fnv1a "$W/data.img")
file_hash=$(fnv1a "$W/data/big.bin")
# what it asks this computer for (workbench/qemu_host.py serves it)
mkdir -p "$W/www"
echo "a page from the computer QEMU runs on" > "$W/www/page.txt"
head -c 4194304 /dev/urandom > "$W/www/big.bin"
big_hash=$(fnv1a "$W/www/big.bin")
# what is typed on its console, a line at a time (--type takes them joined by a written \n)
typed=(
	'sysctl hw.ncpu hw.board.name kern.ostype'
	'sysctl kern.maxfiles=20000 kern.procname=shell'
	'ifconfig en0 10.0.2.15'
	'ping 10.0.2.2 2'
	'serve 2'
	'get 10.0.2.2 18081 /page.txt'
	'get 10.0.2.2 18081 /big.bin'
	'udp 10.0.2.2 18082 a datagram'
	'hash /dev/rvblk0'
	'mount apfs /dev/rvblk0 /mnt'
	'cat /mnt/hello.txt'
	'hash /mnt/big.bin'
	'tone 440 1'
	'input 4'
	'run /sbin/launchd ps'
	'dmesg 1000'
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
	'write /tmp/notes/b six b'
	'ln /tmp/notes/b /tmp/notes/c'
	'ln -s /tmp/notes/b /tmp/notes/s'
	'mv /tmp/notes/c /tmp/notes/d'
	'chmod 600 /tmp/notes/b'
	'stat /tmp/notes/b /tmp/notes/d /tmp/notes/s /tmp/notes/c'
	'cat /tmp/notes/s'
	'threads 2'
	'locks 4'
	'mmap /sbin/launchd'
	'kevent 10'
	'run /sbin/launchd crash null'
	'run /sbin/launchd crash illegal'
	'alarm 5'
	'memory 128'
	'unix /etc/motd'
	'fork'
	'fork /sbin/launchd echo became'
	'ioreg 3'
	'ioprop IOService:/ IOPlatformSerialNumber'
	'draw'
)
# (QEMU itself takes the forwarded port's connections, and closes them while nothing listens in the
# machine: curl tries again until the shell's serve is there)
"$HERE/../tools/qemu-kernel.sh" --cpus 2 --ramdisk "$W/root.img" --disk "$W/data.img" --audio "$W/sound.wav" --boot-args rd=md0 --until 'a shell on the console|panic\(' \
	--seconds 240 --type "$(printf '%s\\n' "${typed[@]}")" \
	--forward 18080:80 --then "python3 '$HERE/qemu_host.py' '$W/www' 18081 18082 120 & for try in \$(seq 90); do curl -s -m 10 http://127.0.0.1:18080/test > '$W/page'; [ -s '$W/page' ] && break; sleep 1; done; curl -s -m 120 -o '$W/sent.bin' http://127.0.0.1:18080/bytes/4194304; QEMU_QMP='$W/qmp' python3 '$HERE/../base/qemu/touch.py' key home tap 0.5 0.25; wait" \
	--qmp "$W/qmp" --after 8 --screen "$W/screen.png" \
	--log "$W/serial.log" "$KERNEL" > "$W/out" 2>&1
log=$W/serial.log
check "the loader takes the kernel and jumps to it" grep -aq "jumping to" "$log"
check "the kernel starts and says its version" grep -aq "^Darwin Kernel Version" "$log"
check "it finds the emulator's board" grep -aq "^board: qemu-virt" "$log"
check "its second processor joins it" grep -aq "2 processors running" "$log"
check "its network card becomes en0, link up" grep -aq "en0 attached on QEMU's virtio network device.*link up" "$log"
check "both input devices are found" test "$(sed -n '1,/a shell on the console/p' "$log" | grep -ac "virtio-input: device in slot")" = 2
check "the root is mounted from the RAM disk" grep -aq 'apfs: volume 0 "Root"' "$log"
check "its first process runs, as pid 1" grep -aq "user space: /sbin/launchd, pid 1" "$log"
check "... and hears from the kernel what it is" grep -aq "board      qemu-virt" "$log"
check "its shell reads the console and answers with the kernel's values, typed" \
	bash -c "tr -d '\\r' < '$log' | grep -qx 'hw.ncpu: 2' && tr -d '\\r' < '$log' | grep -qx 'kern.ostype: Darwin'"
check "the kernel takes values set: a number, and its own name for the process" \
	bash -c "tr -d '\\r' < '$log' | grep -qx 'kern.maxfiles: 20000' && tr -d '\\r' < '$log' | grep -qx 'kern.procname: shell'"
check "en0 takes an address and comes up" bash -c "tr -d '\\r' < '$log' | grep -qx $'\\tinet 10.0.2.15'"
check "QEMU's gateway answers its pings, through the virtio network driver" grep -aq "2 sent, 2 answered" "$log"
check "its TCP serves a page to this machine, through QEMU's forwarded port" grep -q "^Served by /sbin/launchd" "$W/page"
check "... and sends this computer 4 MB, every byte as it should be" python3 -c 'import sys
data = open(sys.argv[1], "rb").read()
sys.exit(not (len(data) == 4194304 and all(b == i * 7 % 251 for i, b in enumerate(data))))' "$W/sent.bin"
check "... which it saw asked for" grep -aq "GET /test HTTP" "$log"
check "it asks this computer for a page over TCP, as a client" bash -c "tr -d '\\r' < '$log' | grep -q '^get: HTTP/1.0 200 OK, 38 bytes of body' &&
	tr -d '\\r' < '$log' | grep -qx 'a page from the computer QEMU runs on'"
check "... and 4 MB of it arrive whole (the same FNV-1a hash as here)" \
	grep -aq "^get: HTTP/1.0 200 OK, 4194304 bytes of body in [0-9]* ms, fnv1a $big_hash" "$log"
check "a datagram goes to this computer and its answer comes back" grep -aq "^udp: 18 bytes back from 10.0.2.2: echoed: a datagram" "$log"
check "a second disk on the virtio block device is mounted, APFS, and read" \
	grep -aq "^a second disk, on QEMU's virtio block device" "$log"
check "... read whole through the virtio block driver, every byte as the image's" \
	grep -aq "^/dev/rvblk0: $(stat -c %s "$W/data.img") bytes in [0-9]* ms, fnv1a $disk_hash" "$log"
check "... and a file of 3 MB in its APFS volume, as it was put there" grep -aq "^/mnt/big.bin: 3000000 bytes in [0-9]* ms, fnv1a $file_hash" "$log"
check "its sound device plays a tone of 440 Hz, through the virtio sound driver" \
	bash -c "python3 '$HERE/tone_in_wav.py' '$W/sound.wav' 440 > /dev/null"
check "a button pressed on the virtio keyboard reaches the input device" \
	bash -c "tr -d '\\r' < '$log' | grep -qx 'button down home' && tr -d '\\r' < '$log' | grep -qx 'button up home'"
check "... and a touch on the virtio tablet, where it was made" grep -aq "touch down finger 0 at 3276[0-9],1638[0-9]" "$log"
check "it starts a process of its own (posix_spawn), which runs as its child" grep -aqE "^ +[0-9]+ +1 +0  launchd" "$log"
check "... and waits for it to end, as it ended" grep -aqE "^run: pid [0-9]+ exited with 0" "$log"
# (no wait for a clock that is not there: the first process starts some seconds after the kernel, not 30 more)
check "the kernel starts its first process within 20 seconds of its own start" bash -c "tr -d '\\r' < '$log' |
	grep -aoE '^\\[ *[0-9]+\\.[0-9]+\\]: load_init_program: attempting to load /sbin/launchd' | tr -d '[]:' | awk '{ exit !(\$1 < 20) }'"
# (microseconds from the timebase's own frequency: QEMU's 62.5 MHz is no whole number of MHz)
check "the kernel's timestamps have no more than a second's microseconds" bash -c "tr -d '\\r' < '$log' |
	grep -aoE '^\\[ *[0-9]+\\.[0-9]+\\]' | awk -F. '{ n++; if (length(\$2) != 7) bad++ } END { exit !(n > 0 && bad == 0) }'"
check "the kernel's message buffer is read (dmesg)" bash -c "tr -d '\\r' < '$log' | sed -n '/^# dmesg/,\$p' | grep -qE '^\\[ *[0-9]+\\.[0-9]+\\]: '"
check "Mach messages go to a port of its own and come back as they went" grep -aq "^mach: 200 messages to itself through a port" "$log"
check "it knows the date (QEMU's clock)" grep -aqE "^$(date -u +%Y)-[0-9]{2}-[0-9]{2} [0-9:]{8} UTC" "$log"
check "a signal ends a process in the background, and wait says so" grep -aqE "^wait: pid [0-9]+ ended by signal 15" "$log"
check "a pipe carries what a child writes into it" grep -aqE "^pipe: 19 bytes from pid [0-9]+: through the kernel" "$log"
check "a file is written in the kernel's file system in memory (ramfs), read back, and removed" \
	bash -c "tr -d '\\r' < '$log' | grep -qx 'kept in memory' && tr -d '\\r' < '$log' | sed -n '/^# ls \\/tmp\\/notes/,\$p' | grep -c '  \\.\\.\\?\$' | grep -qx 2"
# (b and d one file, by its inode; c renamed to d, so gone; s a symbolic link to b, read through)
check "a file has two names (link), one renamed, its mode changed, and a symbolic link to it is read through" \
	bash -c "tr -d '\\r' < '$log' | awk '
		/^\/tmp\/notes\/b: -rw------- 2 links, inode [0-9]+, 6 bytes\$/ { b = \$6 }
		/^\/tmp\/notes\/d: -rw------- 2 links, inode [0-9]+, 6 bytes\$/ { d = \$6 }
		/^\/tmp\/notes\/s: l[-rwx]{9} 1 link, inode [0-9]+, 12 bytes -> \/tmp\/notes\/b\$/ { s = 1 }
		/^stat: \/tmp\/notes\/c: error 2\$/ { c = 1 }
		/^# cat \/tmp\/notes\/s\$/ { cat = NR }
		cat && NR == cat + 1 && /^six b\$/ { read = 1 }
		END { exit !(b != \"\" && b == d && s && c && read) }'"
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
check "fork copies the process: a write in the child's copy is not the parent's" grep -aqE "^fork: child [0-9]+ saw 2 in its copy, the parent still 1" "$log"
check "... and the child becomes another program (execve)" bash -c "tr -d '\\r' < '$log' | grep -qx became && grep -aqE '^fork: child [0-9]+ ran it and exited with 0' '$log'"
check "IOKit's registry answers MIG messages: its root, and the platform expert under it" \
	bash -c "grep -aq '^+-o Root  <class IORegistryEntry>' '$log' && grep -aq '+-o GenericARMPE  <class GenericARMPE>' '$log'"
check "the serial number the loader made is the one IOKit gives user space" bash -c "serial=\$(grep -aoE 'identity: serial number [A-Z0-9]+' '$log' | awk '{ print \$4 }') &&
	[ -n \"\$serial\" ] && tr -d '\\r' < '$log' | grep -qx \"IOPlatformSerialNumber = \\\"\$serial\\\"\""
check "a picture drawn in user space reaches the screen (IOSurface, the framebuffer's swap)" \
	bash -c "grep -aq '^draw: a picture of [0-9]* x [0-9]* on the screen' '$log' && python3 '$HERE/screen_shows_draw.py' '$W/screen.png' > /dev/null"
check "threads take turns under one lock, asleep in the kernel meanwhile (ulock_wait, ulock_wake)" \
	grep -aqE "^locks: 4 threads added 800000 of 800000 under one lock, waited in the kernel [1-9][0-9]* times" "$log"
check "no panic" bash -c "! grep -aq 'panic(' '$log'"
if [ $failed != 0 ]; then
	echo "    (the serial log's last lines)"
	tail -15 "$log" | sed 's/^/    /'
fi
exit $failed
