#!/bin/bash
# Boot a kernel of this repository in QEMU's virt machine, as a phone boots it: a real UEFI starts the
# loader (arch/arm64/boot/loader.efi), which takes the kernel from a payload and gives it the emulator's board.
# The machine has a display (ramfb) and a serial port; what the kernel says on either is kept.
#
#   tools/qemu-kernel.sh [options] KERNEL
#   tools/qemu-kernel.sh [options] --image FILE
#     --image FILE      an image tools/image.py built (its payload, or its boot.img): its own loader, kernel and boot
#                       arguments, as a phone boots it; --boot-args, --defconfig, --ramdisk and --identity are not for it
#     --until TEXT      stop once the serial log has TEXT (default: when the kernel waits for its root, or
#                       panics)
#     --seconds N       stop after N seconds in any case (default 300)
#     --screen FILE     a picture of the screen, as a PNG, when it stops
#     --type TEXT       once the --until text is there, type TEXT on the console (\n: the end of a line; the
#                       serial port is the console's keyboard), a line once the shell's prompt "# " is back
#                       after the last one (at a line's start), then wait --after seconds (default 5); a line
#                       nothing of which is echoed within 15 s is killed (Ctrl-U) and typed again
#     --log FILE        the serial log (default: qemu-kernel.log in the working directory)
#     --boot-args ARGS  added to the kernel's boot arguments (-v, the verbose console on the screen, is
#                       there unless --quiet)
#     --defconfig NAME  the boot arguments of arch/arm64/configs/NAME_defconfig (tools/defconfig.py --list), before
#                       --boot-args', which replace one of the same name
#     --ramdisk FILE    a disk image the kernel gets as /dev/md0 (its root: --boot-args rd=md0)
#     --identity FILE   which iPhone Darwin is told it is, carried in the payload as ./bench carries it
#                       (BENCH_IDENTITY): an iPhone's DeviceTree.bin, or a blob arch/arm64/boot/mkidentity.py --blob
#                       made; for a loader built without one (arch/arm64/boot/src/identity_given.c). Not for --image:
#                       tools/image.py personalize gives an image one
#     --audio FILE      a virtio sound device, whose output QEMU writes into FILE (a WAV file); the kernel
#                       plays /dev/hakios-audio through it
#     --disk FILE       a disk image on a virtio disk, read only, which the kernel gets as the block device /dev/rvblk0
#     --cpus N          processors (default 1)
#     --qemu ARGS       more for QEMU: devices, e.g. --qemu "-device virtio-keyboard-device" (the kernel's virtio
#                       drivers take the MMIO transports: "-device virtio-*-device", not the PCI ones)
#     --quiet           no -v
#     --forward H:G     the machine's TCP port G reachable as port H of this machine (QEMU's own network)
#     --then COMMAND    run COMMAND here while it types, and wait for it before the --after wait (its output
#                       is printed): e.g. curl to a --forward port
#     --monitor PATH    QEMU's monitor as a socket at PATH, for another program to ask it while it runs
#     --qmp PATH        QEMU's QMP as a socket at PATH: what touches the screen and presses the buttons
#                       (QEMU_QMP=PATH tools/qemu/touch.py tap 0.5 0.5)
#     --no-devices      without the machine's virtio devices: by default it has a network card (QEMU's own
#                       network behind it), a keyboard and a tablet, and the kernel is told to take them
#     --interactive     the console on this terminal, to type on (the shell of tools/qemu-root.sh's root);
#                       Ctrl-C ends it. --until, --type and --screen are not for it
#     --window          the screen in a window of QEMU's, whose keyboard and pointer are the machine's
#                       virtio keyboard and tablet (the kernel's buttons and touch screen)
#
# Needs qemu-system-aarch64 and an aarch64 UEFI (Debian/Ubuntu: qemu-system-arm qemu-efi-aarch64).
# The kernel: kernel/build.sh (with LLVM's tools or theos'); the loader: arch/arm64/boot/build.sh, made here if
# it is not there.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
die() { echo "qemu-kernel: $*" >&2; exit 1; }
# the emulator, its processor, memory and firmware: build.config.qemu (QEMU_BUILD_CONFIG: another file of the top)
HAKIOS=$HERE
export HAKIOS
# shellcheck source=../build.config.qemu
source "$HERE/${QEMU_BUILD_CONFIG:-build.config.qemu}"
until_text='Waiting on <dict|panic\(' seconds=300 screen='' log=$PWD/qemu-kernel.log args='' defconfig='' ramdisk='' cpus=1 verbose=-v extra=() typed='' after=5 forward='' then='' monitor='' qmp='' disk='' audio='' interactive='' display=none
with_devices=1 image='' identity=''
while [ $# -gt 0 ] && [ "${1#--}" != "$1" ]; do
	case $1 in
	--image) image=$(realpath -e "$2") || exit 1; shift 2 ;;
	--until) until_text=$2; shift 2 ;;
	--seconds) seconds=$2; shift 2 ;;
	--screen) screen=$(realpath -m "$2"); shift 2 ;;
	--type) typed=$2; shift 2 ;;
	--after) after=$2; shift 2 ;;
	--log) log=$(realpath -m "$2"); shift 2 ;;
	--boot-args) args=$2; shift 2 ;;
	--defconfig) defconfig=$("$HERE/tools/defconfig.py" "$2") || exit 1; shift 2 ;;
	--ramdisk) ramdisk=$(realpath -e "$2") || exit 1; shift 2 ;;
	--identity) identity=$(realpath -e "$2") || exit 1; shift 2 ;;
	--disk) disk=$(realpath -e "$2") || exit 1; shift 2 ;;
	--audio) audio=$(realpath -m "$2"); shift 2 ;;
	--cpus) cpus=$2; shift 2 ;;
	--qemu) read -ra extra <<< "$2"; shift 2 ;;
	--quiet) verbose=''; shift ;;
	--no-devices) with_devices=''; shift ;;
	--forward) forward=",hostfwd=tcp:127.0.0.1:${2%%:*}-:${2##*:}"; shift 2 ;;
	--then) then=$2; shift 2 ;;
	--monitor) monitor=$(realpath -m "$2"); shift 2 ;;
	--qmp) qmp=$(realpath -m "$2"); shift 2 ;;
	--interactive) interactive=1; shift ;;
	--window) display=gtk; shift ;;
	*) die "unknown option $1 (the head of this file lists them)" ;;
	esac
done
[ $# -eq 1 ] || { [ -n "$image" ] && [ $# -eq 0 ]; } || die "usage: tools/qemu-kernel.sh [options] KERNEL, or [options] --image FILE"
[ -z "$image" ] || [ -z "$identity$ramdisk$defconfig$args" ] ||
	die "--image boots the image as it is: no --boot-args, --defconfig, --ramdisk or --identity (tools/image.py personalize gives it an identity)"
# (the kernel's drivers speak virtio 1.0, which QEMU's MMIO transports offer when not forced to the legacy one)
devices=()
[ -z "$with_devices" ] || devices=(-global virtio-mmio.force-legacy=false -netdev "user,id=net0$forward" -device 'virtio-net-device,netdev=net0'
	-device virtio-keyboard-device -device virtio-tablet-device)
if [ -n "$audio" ]; then
	[ ${#devices[@]} -gt 0 ] || devices=(-global virtio-mmio.force-legacy=false)
	devices+=(-audiodev "wav,id=sound0,path=$audio" -device 'virtio-sound-device,audiodev=sound0')
fi
if [ -n "$disk" ]; then
	[ ${#devices[@]} -gt 0 ] || devices=(-global virtio-mmio.force-legacy=false)
	devices+=(-drive "file=$disk,format=raw,if=none,id=disk0,readonly=on" -device 'virtio-blk-device,drive=disk0')
fi
[ -z "$qmp" ] || devices+=(-qmp "unix:$qmp,server,nowait")
[ -n "$image" ] || { kernel=$(realpath -e "$1") || exit 1; }
command -v "$QEMU" > /dev/null || die "no $QEMU (apt: qemu-system-arm; QEMU in build.config.qemu)"
firmware=
# shellcheck disable=SC2086  # (a list of places, one a word)
for code in $QEMU_FIRMWARE $QEMU_FIRMWARE_PLACES; do
	[ -f "$code" ] && firmware=$code && break
done
[ -n "$firmware" ] || die "no aarch64 UEFI firmware (apt: qemu-efi-aarch64)"
[ -n "$image" ] || [ -f "$HERE/arch/arm64/boot/loader.efi" ] || "$HERE/arch/arm64/boot/build.sh" > /dev/null || die "the loader does not build"

W=$(mktemp -d)
trap 'kill "${qemu:-}" 2> /dev/null; rm -rf "$W"' EXIT       # (no qemu yet when it stops before starting one)
mkdir -p "$W/esp/EFI/BOOT"
if [ -n "$image" ]; then
	# the image's own loader and payload, untouched: what QEMU's UEFI starts is what a phone's would
	"$HERE/tools/image.py" extract "$image" --payload "$W/payload.bin" --loader "$W/esp/EFI/BOOT/BOOTAA64.EFI" > /dev/null ||
		die "$image: not an image tools/image.py made"
else
	cp "$HERE/arch/arm64/boot/loader.efi" "$W/esp/EFI/BOOT/BOOTAA64.EFI"
	# (a device tree made into the blob the loader reads, as ./bench makes it; a blob as it is)
	if [ -n "$identity" ] && [ "$(head -c 4 "$identity")" != IDNT ]; then
		python3 "$HERE/arch/arm64/boot/mkidentity.py" --blob "$W/identity.bin" "$identity" > /dev/null || die "no identity made of $identity"
		identity=$W/identity.bin
	fi
	# the console on the screen too only for a picture of it: in QEMU each line scrolls the whole framebuffer
	cmdline="$verbose cpus=$cpus${screen:+ console_screen=1}${with_devices:+ virtio_net=1 virtio_input=1}${disk:+ virtio_blk=1}${audio:+ virtio_sound=1}${defconfig:+ $defconfig} $args"
	python3 "$HERE/arch/arm64/boot/mkpayload.py" -o "$W/payload.bin" kernel="$kernel" ${ramdisk:+ramdisk="$ramdisk"} \
		${identity:+identity="$identity"} "--cmdline=${cmdline# }" > /dev/null || die "no payload"
fi
cp "${firmware%CODE.fd}VARS.fd" "$W/vars.fd" 2> /dev/null || truncate -s 64M "$W/vars.fd"
[ -n "$monitor" ] || monitor=$W/monitor
: > "$log"
machine=("$QEMU" -M "virt,gic-version=$QEMU_GIC" -cpu "$QEMU_CPU" -smp "$cpus" -m "$QEMU_MEMORY" -accel "$QEMU_ACCEL"
	-drive "if=pflash,format=raw,readonly=on,file=$firmware" -drive "if=pflash,format=raw,file=$W/vars.fd"
	-drive "file=fat:rw:$W/esp,format=raw,if=virtio" -device virtio-rng-pci -device ramfb
	-device "loader,file=$W/payload.bin,addr=0xF0000000,force-raw=on" -display "$display"
	-serial chardev:console -monitor "unix:$monitor,server,nowait" -no-reboot "${devices[@]}" "${extra[@]}")
if [ -n "$interactive" ]; then
	# (in the foreground: a command a script puts in the background reads nothing of the terminal)
	echo "qemu-kernel: the console is this terminal (Ctrl-C ends it); the serial log: $log" >&2
	"${machine[@]}" -chardev "stdio,id=console,signal=on,logfile=$log" 2> "$W/qemu.err"
	echo "qemu-kernel: QEMU has ended; the serial log: $log ($(wc -l < "$log") lines)" >&2
	exit 0
fi
"${machine[@]}" -chardev "socket,id=console,path=$W/console,server=on,wait=off,logfile=$log" 2> "$W/qemu.err" &
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
if [ -n "$reached" ] && [ -n "$typed" ] && kill -0 "$qemu" 2> /dev/null; then
	# a key at a time, as a person types, and a line only once the last one is done: the shell has said its
	# prompt ("# ") again (keys typed while the kernel writes to the port are lost)
	# (--then beside it: what it does may be what a typed line waits for)
	then_pid=''
	if [ -n "$then" ]; then
		# --seconds is "stop after N seconds in any case": give --then only the time left of it, so a command whose
		# guest never answers cannot hang the run for ever
		left=$((seconds - (SECONDS - started)))
		[ "$left" -lt 1 ] && left=1
		timeout "$left" bash -c "$then" &
		then_pid=$!
	fi
	python3 - "$W/console" "$typed" "$log" <<'PY'
import socket, sys, threading, time
console, typed, log = sys.argv[1:4]
# (at a line's start, or after the screen was cleared: clear's ESC [ H ESC [ 2 J)
prompts = lambda: (lambda text: text.count(b'\n# ') + text.count(b'\x1b[2J# '))(open(log, 'rb').read())
def wait_for(count, line=''):
    for _ in range(1800):
        if prompts() >= count:
            return count
        time.sleep(0.1)
    # (a prompt not at a line's start is not seen; counted from what has come, the lines after do not each wait
    # their three minutes for it)
    sys.stderr.write('qemu-kernel: no prompt came back in 3 minutes after: %s\n' % line.strip())
    return prompts()
s = socket.socket(socket.AF_UNIX)
s.connect(console)
# what the port says comes to this socket too: it is read and let go (QEMU's log has it), or the socket
# fills, QEMU holds the port's transmitter busy and the kernel waits on it in the middle of a line
def drain():
    try:
        while s.recv(65536):
            pass
    except OSError:
        pass
threading.Thread(target=drain, daemon=True).start()
wait_for(1)                                     # (the first prompt, which the --until text may come before)
done = prompts()
for text in typed.replace('\\n', '\n').splitlines(keepends=True):
    # a line whose keys the kernel lost while it wrote a line of its own: nothing of it echoed by the console's
    # tty and no prompt after a while -- killed (Ctrl-U, the tty's kill character) and typed again
    for attempt in range(3):
        start = len(open(log, 'rb').read())
        for c in text.encode():
            s.sendall(bytes([c]))
            time.sleep(0.05)
        for _ in range(150):
            if prompts() > done:
                break
            time.sleep(0.1)
        echoed = text.strip().encode()[:3] in open(log, 'rb').read()[start:]
        if prompts() > done or echoed or not text.strip():
            break
        s.sendall(b'\x15')
        time.sleep(0.5)
    done += 1
    done = wait_for(done, text)
time.sleep(0.5)
PY
	[ -z "$then_pid" ] || wait "$then_pid"
	sleep "$after"
fi
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
	# QEMU's own errors (a bad --qemu, accelerator or firmware, or an early exit) are the likely cause: show them
	# before the EXIT trap removes the work directory
	if [ -s "$W/qemu.err" ]; then
		echo "qemu-kernel: QEMU's stderr:" >&2
		cat "$W/qemu.err" >&2
	fi
	exit 1
fi
