# The kernel in QEMU

QEMU's `virt` machine runs the kernel of this repository on any Linux computer, booted the way a phone
boots it: a real UEFI starts the loader, the loader takes the kernel from a payload, gives it the
emulator's board (`devices/qemu-virt`) and jumps to it. Nothing of Apple's is needed beyond the kernel's
own sources: the kernel can be linked by LLVM's tools ([building.md](building.md)), and the root it runs
is made of this repository's programs.

## Running it

```sh
sudo apt-get install qemu-system-arm qemu-efi-aarch64 clang-20 lld-20 llvm-20
make kernel-check                                   # the kernel, linked by LLVM's tools
tools/qemu-root.sh root.img                         # a root: /sbin/launchd and /dev, nothing of Apple's
tools/qemu-kernel.sh --ramdisk root.img --boot-args rd=md0 --until 'a shell on the console' \
    --type 'help\nps\n' --screen screen.png ../xnu-12377-hakios/BUILD/obj/mach.development.bcm2837
make qemu                                           # all of that, checked (workbench/qemu_boot_test.sh)
```

`tools/qemu-kernel.sh` keeps what the serial port says (`--log`), stops at a text (`--until`), types on
the console (`--type`, a line once the shell's prompt is back), takes the screen as a PNG (`--screen`),
attaches a disk image to the virtio block device (`--disk`), writes what the virtio sound device plays to
a WAV file (`--audio`), forwards a TCP port of the machine to this
one (`--forward H:G`), opens QEMU's QMP on a socket (`--qmp`, for `base/qemu/touch.py` to touch the screen
and press the buttons), and runs a command here while it types (`--then`). Its head lists every option.

## The machine

| What | How the kernel has it |
| --- | --- |
| Processors | `--cpus N`, started through PSCI: all of them join. |
| Console | The PL011 serial port, and the screen (ramfb) when a picture is asked for (`console_screen=1`). |
| Network | A virtio network card (`en0`) behind QEMU's own network: 10.0.2.15, the gateway 10.0.2.2. |
| Keyboard, tablet | virtio input devices, which the kernel's input path takes. |
| Sound | A virtio sound device (`--audio`): `/dev/hakios-audio`, 48 kHz, two channels of 16 bits. |
| Disks | The root from the payload's RAM disk (`/dev/md0`); a second image on the virtio block device (`/dev/rvblk0`). |

## The root and its shell

`tools/qemu-root.sh` makes an APFS image (`userland/tools/apfs/mkapfs.py`) with `/dev`, `/etc/motd`,
`/mnt` and `/sbin/launchd`, which is `tools/qemu-init/`: a static program that makes its system calls
itself. As the kernel's first process it says what the kernel says of itself, then reads the console a
line at a time:

| Command | What it does, and with what of the kernel |
| --- | --- |
| `about`, `sysctl NAME[=VALUE]...` | The kernel's values, typed as the kernel types them (sysctl 0.3 and 0.4); with `=VALUE`, set first (`kern.hostname` only a platform binary may set). |
| `ls [PATH]`, `cat FILE` | Directories and files: getdirentries64, lstat64, read. |
| `ps`, `uptime` | The processes (proc_info) and the time since boot. |
| `ifconfig [NAME ADDRESS]` | The interfaces and their addresses; brings one up with an address. |
| `ping ADDRESS` | ICMP echo on a datagram socket, through the virtio network driver. |
| `serve [COUNT]` | A page over HTTP on port 80: the kernel's TCP from listen to close. |
| `get ADDRESS PORT [PATH]`, `udp ADDRESS PORT WORDS...` | A page asked for over HTTP (TCP's connect, as a client): its size, the time it took and its FNV-1a hash, and the page itself if it is short; a datagram sent and its answer read. 10.0.2.2 is this computer. |
| `mount apfs DEVICE DIR`, `mount ramfs ramfs DIR` | A disk's APFS volume, read only; or the kernel's file system in memory, to be written. |
| `mkdir DIR...`, `write FILE WORDS...`, `rm PATH...` | Directories and files made and removed (where a file system takes writing: ramfs). |
| `input [COUNT]` | What the touch screen and the buttons say (`/dev/hakios-touch`), as the kernel's input path hands it on. |
| `run PROGRAM [ARGUMENTS]` | A process of its own (posix_spawn), waited for (wait4): `run /sbin/launchd ps` is this program again, which, not being pid 1, does the one command and ends. |
| `dmesg [LINES]` | The kernel's latest messages (proc_info's message buffer call). |
| `mach [COUNT]` | Messages sent to a Mach port of its own and received back (`mach_msg2`), and the time each took. |
| `date`, `sleep SECONDS` | The time of day, UTC, as the kernel has it from QEMU's clock; and a pause. |
| `spawn PROGRAM [ARGUMENTS]`, `kill PID [SIGNAL]`, `wait` | A process in the background, a signal to it (`kill !`: the one spawned last), and how it ended. |
| `fork [PROGRAM [ARGUMENTS]]` | A copy of this process: the child writes a value of its memory, which the parent's copy must not see; or, given a program, becomes it (execve). |
| `crash null`, `illegal`, `breakpoint` | In a child (`run /sbin/launchd crash null`): a bad address, an undefined instruction, a breakpoint; the shell sees the signal the kernel made of it. |
| `unix FILE` | FILE opened, its descriptor sent over a socket pair with a word (sendmsg, SCM_RIGHTS), and the file read through the descriptor that comes out. |
| `alarm [COUNT]` | A timer of a tenth of a second (setitimer) whose SIGALRM a handler of its own catches, and sigreturn puts the thread back. |
| `pipe WORDS...` | The words through a pipe: a child writes them into it (`/sbin/launchd >N echo ...`, its descriptor N), the shell reads them. |
| `threads [COUNT]` | Threads of its own (bsdthread_create) sharing some counting, timed against one thread doing it all: on two processors two take about half the time. |
| `mmap FILE` | The file mapped, its pages brought in by its file system, and checked against what reading it gives. |
| `memory MB` | That much memory of its own (anonymous mmap), a word on every page written and read back; the kernel's free memory before, with it and after. |
| `kevent [COUNT]` | A timer of 50 ms through kqueue, COUNT times; then a pipe seen readable. |
| `ioreg [DEPTH]` | IOKit's registry, the IOService plane from its root: asked in MIG messages (host_get_io_main, io_registry_get_root_entry, the children's iterators), as IOKit.framework asks it. |
| `ioprop PATH KEY` | A property of a registry entry (io_registry_entry_from_path, get_property_bytes): `ioprop IOService:/ IOPlatformSerialNumber` is the serial number the loader made. |
| `tone HZ [SECONDS]` | A sine wave written to the sound device, through the virtio sound driver. |

## What `make qemu` holds

The loader jumps to the kernel; the kernel starts on two processors and finds the emulator's board, and
starts its first process within 20 seconds of its own start (in QEMU's emulation, about 7);
`en0` attaches with its link up and both input devices are found; the root mounts from the RAM disk;
`/sbin/launchd` runs as pid 1 and its shell answers what is typed; `en0` takes an address, QEMU's
gateway answers its pings, its TCP serves this machine a page through a forwarded port and asks this
machine for one (and for 4 MB, which arrive whole), and a datagram to this machine is answered (`workbench/qemu_host.py`); a second
APFS disk on the virtio block device is mounted and read; a tone played on the virtio sound device comes
out of QEMU at the frequency asked for (`workbench/tone_in_wav.py`); a button pressed on QEMU's keyboard
and a touch on its tablet reach the input device, the touch where it was made; a process started by the
shell runs as its child and is waited for; the kernel's message buffer is read; Mach messages go through a
port and come back unchanged; the date is this year's; a forked child's memory is its own and it can become another
program; a signal ends a process in the background, and a process's
mistakes end it with the signal they should; a timer's signals reach a handler on time; a pipe
carries what a child writes into it; a file is written in ramfs, read back and removed; two threads run on the two processors at once; a mapped
file is what reading it gives; memory of its own holds what is written and is all given back; a descriptor
sent over a socket pair reads the file it was; IOKit's registry answers in MIG messages, and the serial number the loader made is the
one it gives; kqueue's timer keeps time and it sees a pipe become readable; nothing panics.

## What it is not

A Darwin user space: launchd, dyld and the rest are Apple's and are not here. With an IPSW and the
team's tools the Darwin 16.5 generation ran in QEMU (`base/qemu/run-darwin.sh`); the current Darwin 26
and 27 images are made for phones ([darwin27-ubuntu.ru.md](darwin27-ubuntu.ru.md)).
