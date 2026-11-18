#!/bin/bash
# The kernel booted in QEMU as a phone boots it, to its first process: the loader under a real UEFI, the
# kernel's own start on two processors, the emulator's board found, its virtio devices taken, the root
# mounted from the payload's RAM disk (tools/qemu-root.sh: nothing of Apple's), and /sbin/launchd run.
# What is typed on its shell then tries the rest of the kernel:
#   - the network: pings to QEMU's gateway; pages served to this computer and asked of it, 4 MB each way;
#     a million bytes and a datagram over the loopback between two of its processes
#     (TCP both ways); a datagram answered (workbench/qemu_host.py is this computer's side)
#   - the devices: a second disk (virtio block, APFS), read whole and through its file system; a tone
#     (virtio sound, which QEMU writes to a WAV file); a button and a touch from QEMU's keyboard and tablet
#   - processes: one started and waited for, one forked (its memory copied) and made another program,
#     one ended by a signal, two by their own mistakes; signals caught by a handler; a pipe between two;
#     a descriptor sent over a socket pair; threads on both processors at once, and taking turns under one
#     lock; Mach messages; kqueue's timer and a pipe's readiness
#   - IOKit: its registry walked in MIG messages, as IOKit.framework asks it; the serial number the
#     loader made read from it; a picture drawn on a surface and swapped onto the screen
#   - files and memory: a file written, read and removed in ramfs, linked, renamed and linked to, its
#     extended attributes set and taken away; a program's file mapped; memory of its own taken, used and
#     given back; the kernel's messages read; the date
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
python3 "$HERE/../tools/apfs/mkapfs.py" -o "$W/data.img" Data="$W/data" > /dev/null
disk_hash=$(fnv1a "$W/data.img")
file_hash=$(fnv1a "$W/data/big.bin")
# what it asks this computer for (workbench/qemu_host.py serves it)
mkdir -p "$W/www"
echo "a page from the computer QEMU runs on" > "$W/www/page.txt"
head -c 4194304 /dev/urandom > "$W/www/big.bin"
big_hash=$(fnv1a "$W/www/big.bin")
# what the shell's serve makes up for /bytes/N (byte i is i * 7 mod 251), a million of them
python3 -c 'import sys; open(sys.argv[1], "wb").write(bytes(i * 7 % 251 for i in range(1000000)))' "$W/made.bin"
made_hash=$(fnv1a "$W/made.bin")
# ... three million of them, as fill writes them into ramfs, and each turned over (b ^ 0xff) as mmap-write turns them
python3 -c 'import sys; d = bytes(i * 7 % 251 for i in range(3000000)); open(sys.argv[1], "wb").write(d)
open(sys.argv[2], "wb").write(bytes(b ^ 0xff for b in d))' "$W/filled.bin" "$W/turned.bin"
filled_hash=$(fnv1a "$W/filled.bin")
turned_hash=$(fnv1a "$W/turned.bin")
head -c 100 "$W/filled.bin" > "$W/cut.bin"
cut_hash=$(fnv1a "$W/cut.bin")
# what is typed on its console, a line at a time (--type takes them joined by a written \n)
typed=(
	'clear'
	'sysctl hw.ncpu hw.board.name kern.ostype'
	'sysctl kern.maxfiles=20000 kern.procname=shell'
	'sysctl -a'
	'ifconfig en0 10.0.2.15'
	'ping 10.0.2.2 2'
	'serve 2'
	'get 10.0.2.2 18081 /page.txt'
	'get 10.0.2.2 18081 /big.bin'
	'udp 10.0.2.2 18082 a datagram'
	'spawn /sbin/launchd serve 1'
	'sleep 1'
	'get 127.0.0.1 80 /bytes/1000000'
	'wait'
	'spawn /sbin/launchd echo-udp 7777 1'
	'sleep 1'
	'udp 127.0.0.1 7777 over the loopback'
	'wait'
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
	'as 501:20 kill ! 15'
	'kill ! 15'
	'wait'
	'as 501:20 spawn /sbin/launchd sleep 3'
	'ps'
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
	'xattr /tmp/notes/b com.example.colour blue'
	'xattr /tmp/notes/b com.example.size 12'
	'xattr /tmp/notes/b com.example.colour green'
	'xattr -d /tmp/notes/b com.example.size'
	'xattr /tmp/notes/b'
	'xattr -d /tmp/notes/b com.example.size'
	'files 20'
	'unix-name /tmp/notes/socket'
	'mkdir /tmp/notes/x /tmp/notes/y'
	'write /tmp/notes/x/f moved across'
	'mkdir /tmp/notes/x'
	'rm /tmp/notes/x'
	'mv /tmp/notes/x/f /tmp/notes/y/g'
	'ln /tmp/notes/x /tmp/notes/z'
	'cat /tmp/notes/y/g'
	'rm /tmp/notes/y/g /tmp/notes/y /tmp/notes/x'
	'stat /tmp/notes/x'
	'umask 077'
	'write /tmp/notes/private x'
	'umask 027'
	'mkdir /tmp/notes/masked'
	'write /tmp/notes/masked/file x'
	'umask 022'
	'stat /tmp/notes/masked /tmp/notes/masked/file'
	'chown 501:20 /tmp/notes/private'
	'stat /tmp/notes/private'
	'as 501:20 cat /tmp/notes/private'
	'as 502:20 cat /tmp/notes/private'
	'as 502:20 write /tmp/notes/theirs x'
	'as 501:20 chown 0:0 /tmp/notes/private'
	'as 501:20 as 0:0 echo root again'
	'as 501:20 sysctl kern.maxfiles=100'
	'write /tmp/notes/shared x'
	'chmod 640 /tmp/notes/shared'
	'chown 501:20 /tmp/notes/shared'
	'as 501:20 access /tmp/notes/shared /tmp/notes'
	'as 502:20 access /tmp/notes/shared'
	'as 503:30 access /tmp/notes/shared'
	'as 501:20 xattr /tmp/notes/private user.note mine'
	'as 502:20 xattr /tmp/notes/private user.note theirs'
	'as 502:20 xattr /tmp/notes/private'
	'as 501:20 xattr /tmp/notes/private'
	'as 501:20 touch /tmp/notes/shared 5'
	'as 502:20 touch /tmp/notes/shared'
	'chmod 666 /tmp/notes/shared'
	'as 502:20 touch /tmp/notes/shared'
	'as 502:20 touch /tmp/notes/shared 7'
	'as 501:20 chown 501:30 /tmp/notes/shared'
	'cp /sbin/launchd /tmp/notes/su'
	'chmod 6757 /tmp/notes/su'
	'stat /tmp/notes/su'
	'as 501:20 run /tmp/notes/su id'
	'as 501:20 write -a /tmp/notes/su changed'
	'stat /tmp/notes/su'
	'as 501:20 run /tmp/notes/su id'
	'chmod 6757 /tmp/notes/su'
	'as 501:20 truncate /tmp/notes/su 1000'
	'stat /tmp/notes/su'
	'mkdir /tmp/notes/sticky'
	'chmod 1777 /tmp/notes/sticky'
	'as 501:20 write /tmp/notes/sticky/mine x'
	'as 502:20 rm /tmp/notes/sticky/mine'
	'as 501:20 rm /tmp/notes/sticky/mine'
	'stat /tmp/notes/sticky /tmp/notes/sticky/mine'
	'nice -5 nice 10 echo niced'
	'as 501:20 nice 10 nice 15 echo later'
	'as 501:20 nice 10 nice 5 echo sooner'
	'fill /tmp/notes/cut 10000'
	'truncate /tmp/notes/cut 100'
	'hash /tmp/notes/cut'
	'truncate /tmp/notes/cut 70000'
	'truncate /tmp/notes/cut 4000000000'
	'stat /tmp/notes/cut'
	'truncate /tmp/notes/cut 0'
	'df'
	'truncate /tmp/notes/cut 34359738368'
	'df'
	'truncate /tmp/notes/cut 0'
	'df'
	'truncate /etc/motd 3'
	'touch /tmp/notes/old 1000000000'
	'touch /tmp/notes/new'
	'sysctl kern.boottime'
	'touch /etc/motd 5'
	'fifo /tmp/notes/fifo'
	'stat /tmp/notes/fifo'
	'flock /tmp/notes/private'
	'byte-locks /tmp/notes/private'
	'vectors /tmp/notes/vectors'
	'fsize /tmp/notes/limited 1000'
	'appends /tmp/notes/appended 1000'
	'rm /tmp/notes/appended'
	'opens /tmp/notes/opened'
	'watch /tmp/notes/watched'
	'dir-walk /tmp/notes/many 300'
	'write /tmp/notes/one-name x'
	'ln /tmp/notes/one-name /tmp/notes/other-name'
	'mv /tmp/notes/one-name /tmp/notes/other-name'
	'stat /tmp/notes/one-name'
	'write /tmp/notes/replacing new'
	'mv /tmp/notes/replacing /tmp/notes/other-name'
	'stat /tmp/notes/one-name /tmp/notes/other-name'
	'cat /tmp/notes/one-name /tmp/notes/other-name'
	'mkdir /tmp/notes/a-dir /tmp/notes/full-dir /tmp/notes/full-dir/inside'
	'mv /tmp/notes/a-dir /tmp/notes/other-name'
	'mv /tmp/notes/other-name /tmp/notes/a-dir'
	'mv /tmp/notes/a-dir /tmp/notes/full-dir'
	'mkdir /tmp/notes/from /tmp/notes/to /tmp/notes/from/moving'
	'cd /tmp/notes/from/moving'
	'mv /tmp/notes/from/moving /tmp/notes/to/moved'
	'pwd'
	'stat .. /tmp/notes/from /tmp/notes/to'
	'cd /'
	'rm /tmp/notes/to/moved /tmp/notes/to /tmp/notes/from'
	'stat /tmp/notes/limited'
	'ln -s /tmp/notes/loop-b /tmp/notes/loop-a'
	'ln -s /tmp/notes/loop-a /tmp/notes/loop-b'
	'cat /tmp/notes/loop-a'
	'cat /tmp/notes/vectors/inside'
	'ln -s /tmp/notes/vectors /tmp/notes/to-vectors'
	'rm /tmp/notes/to-vectors'
	'stat /tmp/notes/vectors'
	'ln -s nowhere /tmp/notes/dangling'
	'cat /tmp/notes/dangling'
	'rm /tmp/notes/loop-a /tmp/notes/loop-b /tmp/notes/dangling'
	'rm /tmp'
	'mkdir /tmp/notes/other'
	'mount ramfs ramfs /tmp/notes/other'
	'cd /tmp/notes/other/..'
	'pwd'
	'cd /'
	'mv /tmp/notes/vectors /tmp/notes/other/vectors'
	'ln /tmp/notes/vectors /tmp/notes/other/vectors'
	'fill /tmp/notes/other/full 1900000000'
	'write /tmp/notes/other/one-more x'
	'rm /tmp/notes/other/full'
	'write /tmp/notes/other/one-more x'
	'mkdir /tmp/notes/tree /tmp/notes/tree/a /tmp/notes/tree/b /tmp/notes/empty'
	'stat /tmp/notes/tree'
	'mv /tmp/notes/tree/a /tmp/notes/empty'
	'stat /tmp/notes/tree /tmp/notes/empty /tmp/notes/tree/a'
	'rm /tmp/notes/tree/b /tmp/notes/empty /tmp/notes/tree'
	'write /tmp/notes/fixed fixed'
	'chflags 2 /tmp/notes/fixed'
	'write /tmp/notes/fixed changed'
	'rm /tmp/notes/fixed'
	'mv /tmp/notes/fixed /tmp/notes/moved'
	'cat /tmp/notes/fixed'
	'write /tmp/notes/log first'
	'chflags 4 /tmp/notes/log'
	'write /tmp/notes/log over'
	'write -a /tmp/notes/log second'
	'cat /tmp/notes/log'
	'stat /tmp/notes/log'
	'chflags 0 /tmp/notes/fixed /tmp/notes/log'
	'rm /tmp/notes/fixed /tmp/notes/log'
	'stat /tmp/notes/fixed'
	'cloexec /etc/motd'
	'random'
	'write /tmp/notes/script #!/sbin/launchd echo'
	'chmod 755 /tmp/notes/script'
	'run /tmp/notes/script with words'
	'chmod 644 /tmp/notes/script'
	'run /tmp/notes/script'
	'write /tmp/notes/junk not a program'
	'chmod 755 /tmp/notes/junk'
	'run /tmp/notes/junk'
	'fork /tmp/notes/junk'
	'descriptors /tmp/notes/offsets'
	'stat /tmp/notes/offsets'
	'write /tmp/notes/old-name one'
	'write /tmp/notes/new-name two'
	'mv /tmp/notes/old-name /tmp/notes/new-name'
	'cat /tmp/notes/new-name'
	'stat /tmp/notes/old-name'
	'mkdir /tmp/notes/up'
	'mv /tmp/notes/up /tmp/notes/up/down'
	'fill /tmp/notes/big 3000000'
	'df'
	'hash /tmp/notes/big'
	'mmap-write /tmp/notes/big'
	'mmap-private /tmp/notes/big'
	'hash /tmp/notes/big'
	'mmap-coherent /tmp/notes/big'
	'cd /tmp/notes'
	'pwd'
	'cat b'
	'cd'
	'threads 2'
	'threads 2'
	'locks 4'
	'mmap /sbin/launchd'
	'kevent 10'
	'dgram'
	'run /sbin/launchd dgram signal'
	'pipe-size'
	'poll'
	'run /sbin/launchd crash null'
	'run /sbin/launchd crash illegal'
	'run /sbin/launchd crash readonly'
	'alarm 5'
	'run /sbin/launchd mask'
	'vm'
	'time memory 128'
	'vm'
	'time sleep 1'
	'unix /etc/motd'
	'shm'
	'shm'
	'fork'
	'fork /sbin/launchd echo became'
	'ioreg 3'
	'ioprop IOService:/ IOPlatformSerialNumber'
	'draw'
)
# (QEMU itself takes the forwarded port's connections, and closes them while nothing listens in the
# machine: curl tries again until the shell's serve is there. Once it is, an answer may be slow on a busy
# computer: a minute for it, for a request tried again would take a serve's place meant for another)
"$HERE/../tools/qemu-kernel.sh" --cpus 2 --ramdisk "$W/root.img" --disk "$W/data.img" --audio "$W/sound.wav" --boot-args rd=md0 --until 'a shell on the console|panic\(' \
	--seconds 240 --type "$(printf '%s\\n' "${typed[@]}")" \
	--forward 18080:80 --then "python3 '$HERE/qemu_host.py' '$W/www' 18081 18082 120 & for try in \$(seq 90); do curl -s -m 60 http://127.0.0.1:18080/test > '$W/page'; [ -s '$W/page' ] && break; sleep 1; done; curl -s -m 120 -o '$W/sent.bin' http://127.0.0.1:18080/bytes/4194304; QEMU_QMP='$W/qmp' python3 '$HERE/../tools/qemu/touch.py' key home tap 0.5 0.25; wait" \
	--qmp "$W/qmp" --after 8 --screen "$W/screen.png" \
	--log "$W/serial.log" "$KERNEL" > "$W/out" 2>&1
log=$W/serial.log
# what the shell said, without the kernel's own lines that come every minute whatever is typed (where one fell
# into a command's answer, a check of the line after the command, or of the lines up to the next prompt, would
# see it), and without the carriage returns
shell=$W/shell.log
# (taken out wherever they came, in the middle of a line typed too: what is left of it is joined up again)
python3 -c 'import re, sys; text = open(sys.argv[1], "rb").read().replace(b"\r", b"")
open(sys.argv[2], "wb").write(re.sub(rb"memory: free [^\n]*\(anybody\)\n|net: so far [^\n]*\n", b"", text))' "$log" "$shell"
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
	bash -c "cat '$shell' | grep -qx 'hw.ncpu: 2' && cat '$shell' | grep -qx 'kern.ostype: Darwin'"
check "the kernel takes values set: a number, and its own name for the process" \
	bash -c "cat '$shell' | grep -qx 'kern.maxfiles: 20000' && cat '$shell' | grep -qx 'kern.procname: shell'"
check "en0 takes an address and comes up" bash -c "cat '$shell' | grep -qx $'\\tinet 10.0.2.15'"
check "QEMU's gateway answers its pings, through the virtio network driver" grep -aq "2 sent, 2 answered" "$log"
check "its TCP serves a page to this machine, through QEMU's forwarded port" grep -q "^Served by /sbin/launchd" "$W/page"
check "... and sends this computer 4 MB, every byte as it should be" python3 -c 'import sys
data = open(sys.argv[1], "rb").read()
sys.exit(not (len(data) == 4194304 and all(b == i * 7 % 251 for i, b in enumerate(data))))' "$W/sent.bin"
check "... which it saw asked for" grep -aq "GET /test HTTP" "$log"
check "it asks this computer for a page over TCP, as a client" bash -c "cat '$shell' | grep -q '^get: HTTP/1.0 200 OK, 38 bytes of body' &&
	cat '$shell' | grep -qx 'a page from the computer QEMU runs on'"
check "... and 4 MB of it arrive whole (the same FNV-1a hash as here)" \
	grep -aq "^get: HTTP/1.0 200 OK, 4194304 bytes of body in [0-9]* ms, fnv1a $big_hash" "$log"
check "TCP over the loopback between two of its own processes: a million bytes from a server in the background, every one checked" \
	grep -aq "^get: HTTP/1.0 200 OK, 1000000 bytes of body in [0-9]* ms, fnv1a $made_hash" "$log"
check "a datagram over the loopback to another of its processes, and sent back" \
	grep -aq "^udp: 17 bytes back from 127.0.0.1: over the loopback" "$log"
check "a datagram goes to this computer and its answer comes back" grep -aq "^udp: 18 bytes back from 10.0.2.2: echoed: a datagram" "$log"
check "a second disk on the virtio block device is mounted, APFS, and read" \
	grep -aq "^a second disk, on QEMU's virtio block device" "$log"
check "... read whole through the virtio block driver, every byte as the image's" \
	grep -aq "^/dev/rvblk0: $(stat -c %s "$W/data.img") bytes in [0-9]* ms, fnv1a $disk_hash" "$log"
check "... and a file of 3 MB in its APFS volume, as it was put there" grep -aq "^/mnt/big.bin: 3000000 bytes in [0-9]* ms, fnv1a $file_hash" "$log"
check "its sound device plays a tone of 440 Hz, through the virtio sound driver" \
	bash -c "python3 '$HERE/tone_in_wav.py' '$W/sound.wav' 440 > /dev/null"
check "a button pressed on the virtio keyboard reaches the input device" \
	bash -c "cat '$shell' | grep -qx 'button down home' && cat '$shell' | grep -qx 'button up home'"
check "... and a touch on the virtio tablet, where it was made" grep -aq "touch down finger 0 at 3276[0-9],1638[0-9]" "$log"
check "it starts a process of its own (posix_spawn), which runs as its child" grep -aqE "^ +[0-9]+ +1 +0  launchd" "$log"
check "... and waits for it to end, as it ended" grep -aqE "^run: pid [0-9]+ exited with 0" "$log"
# (no wait for a clock that is not there: the first process starts some seconds after the kernel, not 30 more)
check "the kernel starts its first process within 20 seconds of its own start" bash -c "cat '$shell' |
	grep -aoE '^\\[ *[0-9]+\\.[0-9]+\\]: load_init_program: attempting to load /sbin/launchd' | tr -d '[]:' | awk '{ exit !(\$1 < 20) }'"
# (microseconds from the timebase's own frequency: QEMU's 62.5 MHz is no whole number of MHz)
check "the kernel's timestamps have no more than a second's microseconds" bash -c "cat '$shell' |
	grep -aoE '^\\[ *[0-9]+\\.[0-9]+\\]' | awk -F. '{ n++; if (length(\$2) != 7) bad++ } END { exit !(n > 0 && bad == 0) }'"
check "the kernel's message buffer is read (dmesg)" bash -c "cat '$shell' | sed -n '/^# dmesg/,\$p' | grep -qE '^\\[ *[0-9]+\\.[0-9]+\\]: '"
check "Mach messages go to a port of its own and come back as they went" grep -aq "^mach: 200 messages to itself through a port" "$log"
check "it knows the date (QEMU's clock)" grep -aqE "^$(date -u +%Y)-[0-9]{2}-[0-9]{2} [0-9:]{8} UTC" "$log"
check "a signal ends a process in the background, and wait says so" grep -aqE "^wait: pid [0-9]+ ended by signal 15" "$log"
check "a user who is not root sends no signal to root's process (EPERM); a process whose parent ends is the first process's, and waited for by it" \
	bash -c "grep -aq '^kill: error 1 (EPERM)' '$shell' &&
		cat '$shell' | sed -n '/^# ps\$/,/^# /p' | grep -qE '^ +[0-9]+ +1 +501 +launchd\$' &&
		grep -aqE '^wait: pid [0-9]+ exited with 0' '$shell'"
check "clear empties the screen as a VT100 does, and the shell's prompt follows" grep -aq $'\x1b\\[H\x1b\\[2J# ' "$log"
check "every value of the kernel's is read (sysctl -a: over a thousand names), and nothing panics for it" \
	bash -c "grep -aoE '^sysctl: [0-9]+ names, of the [0-9]+ the kernel has' '$log' | awk '{ exit !(\$2 > 1000 && \$2 == \$6) }'"
# (QEMU's Cortex-A76 has the crypto extensions: AES with PMULL, SHA-1 and SHA-256, which programs choose their
# code by)
check "programs are told the processor's crypto extensions (hw.optional.arm.FEAT_AES, _PMULL, _SHA1, _SHA256)" \
	bash -c "for f in AES PMULL SHA1 SHA256; do cat '$shell' | grep -qx \"hw.optional.arm.FEAT_\$f: 1\" || exit 1; done"
# (twice: the name taken away the first time is made anew the second, O_EXCL)
check "memory shared by name with a forked child (POSIX shared memory) carries its writes, and its name is taken away" \
	bash -c "test \$(grep -ac '^shm: /qemu-init.shm: 16 KB shared with a child, its writes seen at both ends' '$log') = 2"
check "a signal blocked is held, pending, and caught once let through (sigprocmask, sigpending)" \
	grep -aqE '^mask: SIGUSR1 from pid [0-9]+: held while blocked \(pending, not caught\), caught once let through' "$log"
check "poll and select wait out their timeout on an empty pipe, and find it readable with a byte in it" \
	bash -c "grep -aoE '^poll: an empty pipe: poll 0 ready after [0-9]+ ms, select 0 after [0-9]+ ms; with a byte in it: poll 1 \\(readable\\), select 1' '$log' |
		awk '{ exit !(\$9 >= 95 && \$9 < 1000 && \$14 >= 95 && \$14 < 1000) }'"
check "a pipe carries what a child writes into it" grep -aqE "^pipe: 19 bytes from pid [0-9]+: through the kernel" "$log"
check "a file is written in the kernel's file system in memory (ramfs), read back, and removed" \
	bash -c "cat '$shell' | grep -qx 'kept in memory' && cat '$shell' | sed -n '/^# ls \\/tmp\\/notes/,/^# /p' | grep -c '  \\.\\.\\?\$' | grep -qx 2"
# (b and d one file, by its inode; c renamed to d, so gone; s a symbolic link to b, read through)
check "a file has two names (link), one renamed, its mode changed, and a symbolic link to it is read through" \
	bash -c "cat '$shell' | awk '
		/^\/tmp\/notes\/b: -rw------- 2 links, inode [0-9]+, 6 bytes, owner 0:0\$/ { b = \$6 }
		/^\/tmp\/notes\/d: -rw------- 2 links, inode [0-9]+, 6 bytes, owner 0:0\$/ { d = \$6 }
		/^\/tmp\/notes\/s: l[-rwx]{9} 1 link, inode [0-9]+, 12 bytes, owner 0:0 -> \/tmp\/notes\/b\$/ { s = 1 }
		/^stat: \/tmp\/notes\/c: error 2 \(ENOENT\)\$/ { c = 1 }
		/^# cat \/tmp\/notes\/s\$/ { cat = NR }
		cat && NR == cat + 1 && /^six b\$/ { read = 1 }
		END { exit !(b != \"\" && b == d && s && c && read) }'"
check "extended attributes are set, replaced, listed and taken away; one taken away twice is not there (ENOATTR)" \
	bash -c "cat '$shell' | sed -n '/^# xattr \/tmp\/notes\/b\$/,/^# /p' | grep -v '^#' | tr '\\n' '|' |
		grep -qx 'com.example.colour = green|' && grep -aq '^xattr: com.example.size: error 93 (ENOATTR)' '$log'"
# (128 MB are 8192 pages of 16 KB, each a fault the kernel fills with zeros, give or take a page the program
# touched before: Mach's counts see them)
check "Mach's host port counts the virtual memory's pages and faults (host_statistics64): 128 MB, some 8192 zero-filled faults" \
	bash -c "grep -aoE '^vm: faults [0-9]+ .*zero filled [0-9]+' '$log' | sed 's/.* //' | tr '\\n' ' ' |
		awk '{ exit !(NF == 2 && \$2 - \$1 >= 8192 - 64 && \$2 - \$1 < 8192 + 1000) }'"
# (a second asleep is a second on the clock and next to nothing of a processor's; 128 MB of new memory is mostly
# the kernel's time, filling pages, not the program's)
check "the kernel keeps a process's time (getrusage): asleep it uses none, and its page faults are the kernel's" \
	bash -c "grep -aA1 '^# time sleep 1' '$log' | grep -aoE '^time: [0-9]+ ms; of the processors. time [0-9]+ ms in user space, [0-9]+ ms' |
		awk '{ exit !(\$2 >= 1000 && \$2 < 1600 && \$8 + \$13 < 100) }' &&
		grep -aE '^time: [0-9]+ ms; of the processors. time [0-9]+ ms in user space, [0-9]+ ms' '$log' | head -1 |
		awk '{ exit !(\$13 > \$8) }'"
check "the kernel holds a process to its limit of descriptors (setrlimit): the 20th is refused with EMFILE" \
	grep -aqE '^files: [0-9]+ opened under a limit of 20 descriptors, the last 19; then error 24 \(EMFILE\)' "$log"
check "a socket by name in ramfs: a child connects to it there and writes, and the name is taken away after" \
	grep -aq '^unix-name: /tmp/notes/socket (a socket in the file system): a child connected and wrote: by its name' "$log"
check "ramfs says what BSD says: a directory that is there (EEXIST), one not empty (ENOTEMPTY), a hard link to a directory (EPERM)" \
	bash -c "grep -aq '^mkdir: /tmp/notes/x: error 17 (EEXIST)' '$log' && grep -aq '^rm: /tmp/notes/x: error 66 (ENOTEMPTY)' '$log' &&
		grep -aq '^ln: /tmp/notes/z: error 1 (EPERM)' '$log'"
check "a file moved from one directory to another reads as it was, and the directories once emptied are taken away" \
	bash -c "cat '$shell' | grep -A1 -x '# cat /tmp/notes/y/g' | grep -qx 'moved across' &&
		grep -aq '^stat: /tmp/notes/x: error 2 (ENOENT)' '$log'"
check "a directory and a file made under a mask of 027 are made without what it masks (0755 and 0644 asked for)" \
	bash -c "grep -aqE '^/tmp/notes/masked: drwxr-x--- 2 links' '$shell' && grep -aqE '^/tmp/notes/masked/file: -rw-r----- 1 link' '$shell'"
check "a file made under a mask of 077 is its owner's alone, and is given to another (umask, chown)" \
	grep -aqE '^/tmp/notes/private: -rw------- 1 link, inode [0-9]+, 2 bytes, owner 501:20' "$shell"
check "a user who is not root reads a file of their own, not another's, and cannot give a file away, be root again or set the kernel's values" \
	bash -c "cat '$shell' | grep -A2 -x '# as 501:20 cat /tmp/notes/private' | tr '\n' '|' | grep -qx '# as 501:20 cat /tmp/notes/private|as: user 501 (effective 501), group 20|x|' &&
		grep -aq '^cat: /tmp/notes/private: error 13 (EACCES)' '$shell' && grep -aq '^write: /tmp/notes/theirs: error 13 (EACCES)' '$shell' &&
		grep -aq '^chown: /tmp/notes/private: error 1 (EPERM)' '$shell' && grep -aq '^as: 0:0 cannot be had: error 1 (EPERM)' '$shell' &&
		grep -aq '^kern.maxfiles: (not taken: error 1 (EPERM))' '$shell' && ! grep -aqx 'root again' '$shell'"
check "what each may do with a file of mode 640 (access): its owner read and write, its group read, the rest nothing; a directory of root's searched, not written" \
	bash -c "grep -aq '^access: /tmp/notes/shared: read yes, write yes, run no$' '$shell' &&
		grep -aq '^access: /tmp/notes: read yes, write no, run yes$' '$shell' &&
		grep -aq '^access: /tmp/notes/shared: read yes, write no, run no$' '$shell' &&
		grep -aq '^access: /tmp/notes/shared: read no, write no, run no$' '$shell'"
check "a file's extended attributes are its owner's to set and read; another, who may not read the file, neither sets nor lists them" \
	bash -c "grep -aq '^xattr: user.note: error 13 (EACCES)' '$shell' && grep -aq '^xattr: /tmp/notes/private: error 13 (EACCES)' '$shell' &&
		cat '$shell' | grep -A2 -x '# as 501:20 xattr /tmp/notes/private' | tail -1 | grep -qx 'user.note = mine'"
check "a file's times: its owner sets any, another who may write it only now, one who may not none; its owner gives it to no group of another's" \
	bash -c "grep -aq '^touch: /tmp/notes/shared: accessed 5, modified 5, ' '$shell' &&
		grep -aq '^touch: /tmp/notes/shared: error 13 (EACCES)' '$shell' &&
		grep -aqE '^touch: /tmp/notes/shared: accessed 1[0-9]{9}, modified 1[0-9]{9}, ' '$shell' &&
		grep -aq '^touch: /tmp/notes/shared: error 1 (EPERM)' '$shell' && grep -aq '^chown: /tmp/notes/shared: error 1 (EPERM)' '$shell'"
check "a set-user-ID and set-group-ID program runs as its file's owner and group; written or cut by another, it is no longer either (as HFS and UFS have it)" \
	bash -c "grep -aqE '^/tmp/notes/su: -rwsr-srwx 1 link, inode [0-9]+, [0-9]+ bytes, owner 0:0' '$shell' &&
		grep -aqE '^/tmp/notes/su: -rwxr-xrwx 1 link' '$shell' &&
		grep -aqE '^/tmp/notes/su: -rwxr-xrwx 1 link, inode [0-9]+, 1000 bytes' '$shell' &&
		cat '$shell' | grep -A2 -x '# as 501:20 run /tmp/notes/su id' | sed -n 3p | tr '\n' '|' | grep -q '^id: user 501, effective 0; group 20, effective 0|' &&
		cat '$shell' | grep -A2 -x '# as 501:20 run /tmp/notes/su id' | grep -q '^id: user 501, effective 501; group 20, effective 20'"
check "in a sticky directory a file is removed by its owner alone (EACCES to another)" \
	bash -c "grep -aqE '^/tmp/notes/sticky: drwxrwxrwt' '$shell' && grep -aq '^rm: /tmp/notes/sticky/mine: error 13 (EACCES)' '$shell' &&
		grep -aq '^stat: /tmp/notes/sticky/mine: error 2 (ENOENT)' '$shell'"
check "a process's priority set and read back (setpriority, getpriority); a user who is not root may put theirs later, not sooner (EACCES)" \
	bash -c "cat '$shell' | grep -A3 -x '# nice -5 nice 10 echo niced' | tr '\n' '|' |
			grep -qx '# nice -5 nice 10 echo niced|nice: priority -5|nice: priority 10|niced|' &&
		cat '$shell' | grep -A4 -x '# as 501:20 nice 10 nice 15 echo later' | tail -2 | tr '\n' '|' | grep -qx 'nice: priority 15|later|' &&
		cat '$shell' | grep -A3 -x '# as 501:20 nice 10 nice 5 echo sooner' | tail -1 | grep -qx 'nice: 5 not had: error 13 (EACCES)'"
check "a file cut short keeps its start, and grown again reads as zeros past it, not as what it held (truncate); and grown to 4 GB" \
	bash -c "grep -aq '^truncate: /tmp/notes/cut: 10000 bytes, now 100\$' '$shell' &&
		grep -aq '^/tmp/notes/cut: 100 bytes in [0-9]* ms, fnv1a $cut_hash' '$shell' &&
		grep -aq '^truncate: /tmp/notes/cut: 100 bytes, now 70000; what it grew by reads as zeros' '$shell' &&
		grep -aq '^truncate: /tmp/notes/cut: 70000 bytes, now 4000000000; what it grew by reads as zeros (its first and last 64 KB)' '$shell' &&
		grep -aqE '^/tmp/notes/cut: -rw-r--r-- 1 link, inode [0-9]+, 4000000000 bytes' '$shell' &&
		grep -aq '^truncate: /etc/motd: error 30 (EROFS)' '$shell'"
# (a file's table of its pages is made for its size, holes and all -- 16 MB for 32 GB in pages of 16 KB -- and ramfs charges it to its
# room: the truncate takes that much room or is refused, and the cut back to nothing gives it all back)
table_charged() {
	awk '/^# truncate \/tmp\/notes\/cut 34359738368$/ { phase = 1; before = last }
		/^# truncate \/tmp\/notes\/cut 0$/ && phase == 2 { phase = 3 }
		/^truncate: \/tmp\/notes\/cut: 0 bytes, now 34359738368; what it grew by reads as zeros/ && phase == 1 { grew = 1 }
		/^truncate: \/tmp\/notes\/cut: error 28 \(ENOSPC\)/ && phase == 1 { refused = 1 }
		/^\/tmp: ramfs / {
			last = $0
			if (phase == 1) { during = $0; phase = 2 } else if (phase == 3) { after = $0; phase = 4 }
		}
		END {
			split(before, b, " "); split(during, d, " ")
			exit !(before != "" && after == before && (grew && b[7] - d[7] >= 16384 || refused && during == before))
		}' "$shell"
}
check "a file in ramfs grown to 32 GB: the table of its pages charged to the room while it is there (or the truncate refused, ENOSPC), and all given back when it is cut to nothing" \
	table_charged
check "two processes adding to one file in ramfs at once (O_APPEND): each write starts at the end as it is then, none over another" \
	grep -aq '^appends: /tmp/notes/appended: 2000000 bytes of 2000000; whole records 1000 of this process and 1000 of the child, 0 torn' "$shell"
check "the kernel says when it started (kern.boottime): under a quarter of an hour before a file was touched" bash -c "
	booted=\$(grep -aoE '^kern.boottime: [0-9]+ s [0-9]+ us since 1970' '$shell' | awk '{ print \$2 }' | head -1)
	touched=\$(grep -a '^touch: /tmp/notes/new: ' '$shell' | awk '{ print \$6 + 0 }' | head -1)
	[ -n \"\$booted\" ] && [ -n \"\$touched\" ] && [ \"\$booted\" -le \"\$touched\" ] && [ \$((touched - booted)) -lt 900 ]"
check "a file's times set to a second given, and to now (utimes); not on the read-only root" \
	bash -c "grep -aq '^touch: /tmp/notes/old: accessed 1000000000, modified 1000000000, changed ' '$shell' &&
		grep -a '^touch: /tmp/notes/new: ' '$shell' | awk '{ exit !(\$4 == \$6 && \$6 > 1700000000) }' &&
		grep -aq '^touch: /etc/motd: error 30 (EROFS)' '$shell'"
check "a named pipe in ramfs (mkfifo): what a child writes into it is read out of it" \
	bash -c "grep -aq '^fifo: /tmp/notes/fifo: read \"written by a child\"' '$shell' &&
		grep -aqE '^/tmp/notes/fifo: prw------- 1 link, inode [0-9]+, 0 bytes' '$shell'"
check "a file locked (flock): another process's try refused, without waiting (EAGAIN), and taken once it is let go" \
	grep -aq "^flock: /tmp/notes/private: held, another's try refused (error 35); let go, another's try taken" "$shell"
check "bytes of a file locked (fcntl F_SETLK): another process kept off them, read or write, and not off the bytes after; F_GETLK names the holder" \
	grep -aq "^byte-locks: /tmp/notes/private: 0-9 held; another's 5-14 refused (to read too), 10-19 taken, F_GETLK names the holder; let go, taken" "$shell"
check "a file renamed onto another's name takes its place, and a directory cannot be moved into itself (EINVAL)" \
	bash -c "cat '$shell' | grep -A1 -x '# cat /tmp/notes/new-name' | tail -1 | grep -qx 'one' &&
		grep -aq '^stat: /tmp/notes/old-name: error 2 (ENOENT)' '$shell' && grep -aq '^mv: .*error 22 (EINVAL)' '$shell'"
check "descriptors marked close-on-exec (O_CLOEXEC, F_DUPFD_CLOEXEC) close as a program starts, and only they (dup2 leaves the mark behind)" \
	bash -c "grep -aq '^cloexec: O_CLOEXEC and F_DUPFD_CLOEXEC mark a descriptor FD_CLOEXEC, dup and dup2 do not' '$shell' &&
		grep -aq '^fds: 10 open, 11 closed, 12 open' '$shell'"
check "a file's flags (chflags): one not to be changed is not written, removed or renamed, one only to be added to is only added to; cleared, they go" \
	bash -c "grep -aq '^write: /tmp/notes/fixed: error 1 (EPERM)' '$shell' && grep -aq '^rm: /tmp/notes/fixed: error 1 (EPERM)' '$shell' &&
		grep -aq '^mv: /tmp/notes/fixed: error 1 (EPERM)' '$shell' &&
		cat '$shell' | grep -A1 -x '# cat /tmp/notes/fixed' | tail -1 | grep -qx 'fixed' &&
		grep -aq '^write: /tmp/notes/log: error 1 (EPERM)' '$shell' &&
		cat '$shell' | grep -A2 -x '# cat /tmp/notes/log' | tail -2 | tr '\n' '|' | grep -qx 'first|second|' &&
		grep -aqE '^/tmp/notes/log: -rw-r--r-- 1 link, inode [0-9]+, 13 bytes, owner 0:0, flags 0x4' '$shell' &&
		grep -aq '^stat: /tmp/notes/fixed: error 2 (ENOENT)' '$shell'"
check "paths as BSD has them: symbolic links that lead to each other (ELOOP), a file taken for a directory (ENOTDIR), a link to nothing (ENOENT), a link removed and not what it leads to, a mount point not removed (EBUSY)" \
	bash -c "grep -aq '^cat: /tmp/notes/loop-a: error 62 (ELOOP)' '$shell' && grep -aq '^cat: /tmp/notes/vectors/inside: error 20 (ENOTDIR)' '$shell' &&
		grep -aqE '^/tmp/notes/vectors: -rw-r--r-- 1 link, inode [0-9]+, 38 bytes' '$shell' &&
		grep -aq '^cat: /tmp/notes/dangling: error 2 (ENOENT)' '$shell' && grep -aq '^rm: /tmp: error 16 (EBUSY)' '$shell' &&
		! grep -aq '^rm: /tmp/notes/loop\|^rm: /tmp/notes/dangling\|^rm: /tmp/notes/to-vectors' '$shell'"
check "a directory's links: two, and one more for each directory in it; one moved out takes its link along, and renamed onto an empty directory takes its place" \
	bash -c "grep -aqE '^/tmp/notes/tree: drwxr-xr-x 4 links, inode [0-9]+' '$shell' &&
		grep -aqE '^/tmp/notes/tree: drwxr-xr-x 3 links, inode [0-9]+' '$shell' &&
		grep -aqE '^/tmp/notes/empty: drwxr-xr-x 2 links, inode [0-9]+' '$shell' &&
		grep -aq '^stat: /tmp/notes/tree/a: error 2 (ENOENT)' '$shell' && ! grep -aq '^rm: /tmp/notes/tree' '$shell'"
check "datagrams through a socket pair keep their sizes and order; a peek leaves one there; one longer than its room is cut; a send after shutdown is EPIPE, or SIGPIPE ends the sender" \
	bash -c "grep -aq '^dgram: sent 5, 300, 1; looked at 5, then read 5, 300, 1; 300 into 100: 100 said (its first 100 as sent); shut down, a send EPIPE' '$shell' &&
		grep -aqE '^run: pid [0-9]+ ended by signal 13' '$shell'"
check "a pipe holds 64 KB, then says EAGAIN; full, poll does not find it writable, and with 512 bytes read out it does" \
	grep -aq '^pipe-size: 65536 bytes, then EAGAIN; full, not writable; 512 read out, writable again' "$shell"
check "ramfs filled: a write past its room refused (ENOSPC), the file kept to what fitted; the file removed, its room is there again" \
	bash -c "grep -aq '^fill: /tmp/notes/other/full: error 28 (ENOSPC)' '$shell' &&
		grep -aqE '^fill: /tmp/notes/other/full, [0-9]{9,10} bytes' '$shell' &&
		grep -aq '^write: /tmp/notes/other/one-more: error 28 (ENOSPC)' '$shell' &&
		grep -aq '^write: 2 bytes to /tmp/notes/other/one-more' '$shell'"
check "the .. of a file system mounted on a directory leads to that directory's own parent" \
	bash -c "cat '$shell' | grep -A1 -x '# cd /tmp/notes/other/..' | grep -qx '# pwd' && cat '$shell' | grep -A2 -x '# cd /tmp/notes/other/..' | tail -1 | grep -qx '/tmp/notes'"
check "a file is not moved, or linked, from one file system to another (EXDEV)" \
	bash -c "grep -aq '^mv: /tmp/notes/vectors: error 18 (EXDEV)' '$shell' && grep -aq '^ln: /tmp/notes/other/vectors: error 18 (EXDEV)' '$shell'"
check "rename as POSIX has it: two names of one file left both; a directory onto a file (ENOTDIR), a file onto a directory (EISDIR), onto one not empty (ENOTEMPTY)" \
	bash -c "grep -aqE '^/tmp/notes/one-name: -rw-r--r-- 2 links' '$shell' &&
		grep -aq '^mv: /tmp/notes/a-dir: error 20 (ENOTDIR)' '$shell' && grep -aq '^mv: /tmp/notes/other-name: error 21 (EISDIR)' '$shell' &&
		grep -aq '^mv: /tmp/notes/a-dir: error 66 (ENOTEMPTY)' '$shell'"
check "a file renamed onto one of another's two names takes that name; the other keeps the old file, with one link" \
	bash -c "grep -aqE '^/tmp/notes/one-name: -rw-r--r-- 1 link' '$shell' && grep -aqE '^/tmp/notes/other-name: -rw-r--r-- 1 link' '$shell' &&
		cat '$shell' | grep -A2 -x '# cat /tmp/notes/one-name /tmp/notes/other-name' | tail -2 | tr '\n' '|' | grep -qx 'x|new|'"
check "a directory moved to another takes its .. and its link along; the shell's own directory moved, its path follows it" \
	bash -c "cat '$shell' | grep -A1 -x '# pwd' | grep -qx '/tmp/notes/to/moved' &&
		grep -aqE '^/tmp/notes/from: drwxr-xr-x 2 links, inode ([0-9]+)' '$shell' &&
		grep -aqE '^/tmp/notes/to: drwxr-xr-x 3 links' '$shell' &&
		[ \"\$(grep -a '^\\.\\.: drwx' '$shell' | awk '{ print \$6 }')\" = \"\$(grep -a '^/tmp/notes/to: drwx' '$shell' | awk '{ print \$6 }')\" ] &&
		! grep -aq '^rm: /tmp/notes/to\|^rm: /tmp/notes/from' '$shell'"
check "a directory of 300 files read 512 bytes at a time gives each once; emptied as it is read, none is passed over" \
	grep -aq '^dir-walk: 300 made, 300 read in [0-9]* reads; removed as read: 300, the directory then empty' "$shell"
check "a file in ramfs watched through kqueue (EVFILT_VNODE): written, its mode changed, linked, renamed and removed, each said" \
	grep -aq '^watch: written: write extend; chmod: attrib; linked: link; renamed: rename; removed: delete link$' "$shell"
check "open as BSD has it: O_EXCL of a file there (EEXIST), O_NOFOLLOW of a link (ELOOP), O_DIRECTORY of a file (ENOTDIR), a directory to write (EISDIR)" \
	grep -aq '^opens: .*through the link: as BSD has them' "$shell"
check "a process's files no larger than its limit (RLIMIT_FSIZE): a write cut there, one past it and a truncate past it refused (EFBIG)" \
	bash -c "grep -aq '^fsize: /tmp/notes/limited: 1000 of 2000 written; past the limit, a write EFBIG, a truncate EFBIG' '$shell' &&
		grep -aqE '^/tmp/notes/limited: -rw-r--r-- 1 link, inode [0-9]+, 1000 bytes' '$shell'"
check "three pieces written in one writev read back in one readv, into pieces of other sizes, in order" \
	grep -aq '^vectors: /tmp/notes/vectors: 38 bytes written from 3 pieces, 38 read into 3 others, in order' "$shell"
check "the kernel's random bytes: getentropy's differ and stop at 256, and 64 KB of /dev/random has each byte value about as often as the others" \
	grep -aqE '^random: getentropy gave two that differ \(and EINVAL past 256\); /dev/random: 65536 bytes, each value [0-9]+ to [0-9]+ times of 256: even' "$shell"
check "a file that is not a program is not run, by posix_spawn or by execve (ENOEXEC)" \
	bash -c "grep -aq '^run: /tmp/notes/junk does not start: error 8 (ENOEXEC)' '$shell' &&
		grep -aq '^fork: the child cannot become /tmp/notes/junk: error 8 (ENOEXEC)' '$shell'"
check "a script run through the program its #! line names, given the script's path and words; not run once it may not be" \
	bash -c "cat '$shell' | grep -A2 -x '# run /tmp/notes/script with words' | tail -2 | tr '\n' '|' |
			grep -qE '^/tmp/notes/script with words\|run: pid [0-9]+ exited with 0\|\$' &&
		grep -aq '^run: /tmp/notes/script does not start: error 13 (EACCES)' '$shell'"
check "descriptors as BSD has them: lseek, pread, pwrite, a dup's offset, dup2, O_APPEND, a file open after its unlink, a non-blocking pipe's EAGAIN" \
	bash -c "grep -aq '^descriptors: .*a file open after its unlink.*: as BSD has them' '$log' && grep -aq '^stat: /tmp/notes/offsets: error 2 (ENOENT)' '$shell'"
check "the mounted file systems listed (getfsstat): the root and the second disk APFS, read only, devfs, and ramfs with three megabytes of it used" \
	bash -c "grep -aqE '^/: apfs from .*, read only' '$shell' && grep -aqE '^/mnt: apfs from /dev/rvblk0, .*, read only' '$shell' &&
		grep -aq '^/dev: devfs from devfs' '$shell' &&
		grep -aoE '^/tmp: ramfs from ramfs, [0-9]+ KB, [0-9]+ KB free' '$shell' | tail -1 | awk '{ exit !(\$5 - \$7 >= 2900) }'"
check "three megabytes written into ramfs read back as written, and written through a shared mapping read back so (its page-out)" \
	bash -c "grep -aq '^/tmp/notes/big: 3000000 bytes in [0-9]* ms, fnv1a $filled_hash' '$log' &&
		grep -aq '^mmap-write: /tmp/notes/big, 3000000 bytes turned through the mapping, and read so' '$log' &&
		grep -aq '^/tmp/notes/big: 3000000 bytes in [0-9]* ms, fnv1a $turned_hash' '$log'"
check "three megabytes mapped private and written through: the mapping has its own copies, the file is as it was" \
	grep -aq '^mmap-private: /tmp/notes/big: the private mapping turned, the file as it was' "$shell"
check "a file written while it is mapped: the mapping has the bytes at once, in a page and across two; and read() has at once what is written through the mapping" \
	grep -aq '^mmap-coherent: /tmp/notes/big: a write in a page seen through the mapping at once, and one across two pages; written through it, read at once' "$shell"
check "a process has a directory of its own, which relative paths start from (chdir, F_GETPATH)" \
	bash -c "cat '$shell' | grep -A1 -x '# pwd' | grep -qx '/tmp/notes' &&
		cat '$shell' | grep -A1 -x '# cat b' | grep -qx 'six b'"
# (two threads on two processors take well under the time of one doing it all: they do run side by side.
# Timed twice, and the better taken: a processor of the computer QEMU runs on, busy elsewhere for a moment, can
# make one timing come out like one thread's)
check "threads of its own run on both processors at once" bash -c "grep -aoE '^threads: 2 threads on 2 processors did in [0-9]+ ms what one does in [0-9]+ ms' '$log' |
	awk '\$9 * 10 < \$15 * 8 { side_by_side = 1 } END { exit !side_by_side }'"
check "a program's file mapped (its pages from APFS) is what reading it gives" grep -aq "^mmap: /sbin/launchd, [0-9]* bytes: its pages and its reads agree" "$log"
check "kqueue delivers a timer's events on time, and a pipe's readiness" \
	bash -c "grep -aoE '^kevent: 10 timer events of 50 ms in [0-9]+ ms; a pipe with 5 bytes in it is readable' '$log' | awk '{ exit !(\$9 >= 450 && \$9 < 1500) }'"
check "a process's mistakes reach it as signals: a bad address SIGSEGV, an undefined instruction SIGILL, a write to a page made read only (mprotect) SIGBUS" \
	bash -c "grep -aqE 'run: pid [0-9]+ ended by signal 11' '$log' && grep -aqE 'run: pid [0-9]+ ended by signal 4' '$log' &&
		grep -aqE 'run: pid [0-9]+ ended by signal 10' '$log'"
check "a timer's signals are caught by a handler and the thread goes back where it was (sigreturn)" \
	bash -c "grep -aoE '^alarm: 5 SIGALRM caught by a handler in [0-9]+ ms' '$log' | awk '{ exit !(\$9 >= 450 && \$9 < 1500) }'"
check "memory of its own is given, holds what is written, and is all given back" \
	bash -c "grep -aoE '^memory: 128 MB written and read back in [0-9]+ ms, all as written; free: [0-9]+ MB before, [0-9]+ MB with it, [0-9]+ MB after' '$log' |
	awk '{ exit !(\$15 - \$18 >= 120 && \$22 - \$15 > -8) }'"
check "a descriptor sent over a socket pair (SCM_RIGHTS) comes out and reads the file it was" \
	grep -aq '^unix: "sent" and descriptor [0-9]* came through; it reads: This root was made by tools/qemu-root.sh' "$log"
check "fork copies the process: a write in the child's copy is not the parent's" grep -aqE "^fork: child [0-9]+ saw 2 in its copy, the parent still 1" "$log"
check "... and the child becomes another program (execve)" bash -c "cat '$shell' | grep -qx became && grep -aqE '^fork: child [0-9]+ ran it and exited with 0' '$log'"
check "IOKit's registry answers MIG messages: its root, and the platform expert under it" \
	bash -c "grep -aq '^+-o Root  <class IORegistryEntry>' '$log' && grep -aq '+-o GenericARMPE  <class GenericARMPE>' '$log'"
check "the serial number the loader made is the one IOKit gives user space" bash -c "serial=\$(grep -aoE 'identity: serial number [A-Z0-9]+' '$log' | awk '{ print \$4 }') &&
	[ -n \"\$serial\" ] && cat '$shell' | grep -qx \"IOPlatformSerialNumber = \\\"\$serial\\\"\""
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
