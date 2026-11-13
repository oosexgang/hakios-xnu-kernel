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
tools/qemu-kernel.sh --interactive --cpus 2 --ramdisk root.img --boot-args rd=md0 KERNEL   # its shell, by hand
```

`--interactive` makes this terminal the machine's console, to type on (Ctrl-C ends it); `--window` shows
its screen in a window of QEMU's, whose keyboard and pointer are the machine's keyboard and tablet.

`tools/qemu-kernel.sh` keeps what the serial port says (`--log`), stops at a text (`--until`), types on
the console (`--type`, a line once the shell's prompt is back), takes the screen as a PNG (`--screen`),
attaches a disk image to the virtio block device (`--disk`), writes what the virtio sound device plays to
a WAV file (`--audio`), forwards a TCP port of the machine to this one (`--forward H:G`), opens QEMU's QMP
on a socket (`--qmp`, for `base/qemu/touch.py` to touch the screen and press the buttons), and runs a
command here while it types (`--then`). Its head lists every option.

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

The commands, by what of the kernel they try (one file of `tools/qemu-init/` each):

| Command | What it does, and with what of the kernel |
| --- | --- |
| **The kernel** (`kernel.c`) | |
| `about`, `sysctl NAME[=VALUE]...` | The kernel's values, typed as the kernel types them (sysctl 0.3 and 0.4); with `=VALUE`, set first (`kern.hostname` only a platform binary may set). |
| `dmesg [LINES]` | The kernel's latest messages (proc_info's message buffer call). |
| `uptime`, `date` | The time since boot; the time of day, UTC, as the kernel has it from QEMU's clock. |
| **Files** (`files.c`) | |
| `ls [PATH]`, `cat FILE` | Directories and files: getdirentries64, lstat64, read. |
| `hash FILE...` | Each file or device read to its end, timed, and its FNV-1a hash, for this machine to compare. |
| `mount apfs DEVICE DIR`, `mount ramfs ramfs DIR` | A disk's APFS volume, read only; or the kernel's file system in memory, to be written. |
| `mkdir DIR...`, `write FILE WORDS...`, `rm PATH...` | Directories and files made and removed (where a file system takes writing: ramfs). |
| `mmap FILE` | The file mapped, its pages brought in by its file system, and checked against what reading it gives. |
| `memory MB` | That much memory of its own (anonymous mmap), a word on every page written and read back; the kernel's free memory before, with it and after. |
| **The network** (`net.c`) | |
| `ifconfig [NAME ADDRESS]` | The interfaces and their addresses; brings one up with an address. |
| `ping ADDRESS` | ICMP echo on a datagram socket, through the virtio network driver. |
| `serve [COUNT]` | A page over HTTP on port 80: the kernel's TCP from listen to close. `GET /bytes/N` gets N bytes made up as they go (byte i is i * 7 mod 251), for TCP to send a lot. |
| `get ADDRESS PORT [PATH]`, `udp ADDRESS PORT WORDS...` | A page asked for over HTTP (TCP's connect, as a client): its size, the time it took and its FNV-1a hash, and the page itself if it is short; a datagram sent and its answer read. 10.0.2.2 is this computer. |
| **Processes** (`processes.c`) | |
| `ps` | The processes (proc_info). |
| `run PROGRAM [ARGUMENTS]` | A process of its own (posix_spawn), waited for (wait4): `run /sbin/launchd ps` is this program again, which, not being pid 1, does the one command and ends. |
| `spawn PROGRAM [ARGUMENTS]`, `kill PID [SIGNAL]`, `wait`, `sleep SECONDS` | A process in the background, a signal to it (`kill !`: the one spawned last), and how it ended; a pause. |
| `fork [PROGRAM [ARGUMENTS]]` | A copy of this process: the child writes a value of its memory, which the parent's copy must not see; or, given a program, becomes it (execve). |
| `crash null`, `illegal`, `breakpoint` | In a child (`run /sbin/launchd crash null`): a bad address, an undefined instruction, a breakpoint; the shell sees the signal the kernel made of it. |
| `alarm [COUNT]` | A timer of a tenth of a second (setitimer) whose SIGALRM a handler of its own catches, and sigreturn puts the thread back. |
| `pipe WORDS...` | The words through a pipe: a child writes them into it (`/sbin/launchd >N echo ...`, its descriptor N), the shell reads them. |
| `unix FILE` | FILE opened, its descriptor sent over a socket pair with a word (sendmsg, SCM_RIGHTS), and the file read through the descriptor that comes out. |
| `threads [COUNT]` | Threads of its own (bsdthread_create) sharing some counting, timed against one thread doing it all: on two processors two take about half the time. |
| `locks [COUNT]` | Threads adding to one count under one lock, as `os_unfair_lock` makes one: a word, and the kernel's `ulock_wait` and `ulock_wake` for those that must wait. None of the count is lost. |
| `mach [COUNT]` | Messages sent to a Mach port of its own and received back (`mach_msg2`), and the time each took. |
| `kevent [COUNT]` | A timer of 50 ms through kqueue, COUNT times; then a pipe seen readable. |
| **IOKit** (`iokit.c`) | |
| `ioreg [DEPTH]` | IOKit's registry, the IOService plane from its root: asked in MIG messages (host_get_io_main, io_registry_get_root_entry, the children's iterators), as IOKit.framework asks it. |
| `ioprop PATH KEY` | A property of a registry entry (io_registry_entry_from_path, get_property_bytes): `ioprop IOService:/ IOPlatformSerialNumber` is the serial number the loader made. |
| `draw` | A picture on the screen as the window server puts one there: IOCoreSurfaceRoot's and the framebuffer's user clients opened (io_service_open_extended), a surface made and its memory mapped (io_connect_method), drawn in, and swapped onto the screen (SwapBegin, SwapEnd). |
| **Devices** (`devices.c`) | |
| `input [COUNT]` | What the touch screen and the buttons say (`/dev/hakios-touch`), as the kernel's input path hands it on. |
| `tone HZ [SECONDS]` | A sine wave written to the sound device, through the virtio sound driver. |

## What `make qemu` holds

`workbench/qemu_boot_test.sh` boots the kernel on two processors with all of the machine's devices, types
on its shell and checks what comes back:

- **The start.** The loader jumps to the kernel; the kernel finds the emulator's board, both processors
  run, and the first process starts within 20 seconds of the kernel's own start (in QEMU's emulation,
  about 7). The kernel's log has no timestamp with more than a second of microseconds.
- **The network.** `en0` attaches with its link up and takes an address; QEMU's gateway answers its
  pings; its TCP serves this computer a page and 4 MB, and asks it for a page and 4 MB, every byte checked
  (`workbench/qemu_host.py` is this computer's side); a datagram is answered.
- **The devices.** A second disk on the virtio block device is read whole and through its APFS volume,
  by hashes; a tone comes out of the virtio sound device at the frequency asked for
  (`workbench/tone_in_wav.py`); a button pressed on QEMU's keyboard and a touch on its tablet reach the
  input device, the touch where it was made.
- **Processes.** A process started by the shell runs as its child and is waited for; a forked child's
  memory is its own, and it can become another program; a signal ends a process in the background, and a
  process's mistakes end it with SIGSEGV and SIGILL; a timer's signals reach a handler on time; a pipe
  carries what a child writes into it; a descriptor sent over a socket pair reads the file it was; two
  threads run on the two processors at once, and four take turns under one lock without losing a count;
  Mach messages go through a port and come back unchanged;
  kqueue's timer keeps time and it sees a pipe become readable.
- **Files and memory.** The root mounts from the RAM disk; a file is written in ramfs, read back and
  removed; a mapped file is what reading it gives; memory of its own holds what is written and is all
  given back; values are set through sysctl; the kernel's messages are read; the date is this year's.
- **IOKit.** Its registry answers in MIG messages, and the serial number the loader made is the one it
  gives user space; a picture drawn on a surface and swapped onto the screen is what QEMU's screen shows
  (`workbench/screen_shows_draw.py`).
- Nothing panics.

## The kernel's own tests

`workbench/qemu_post_test.sh` (also part of `make qemu`) boots with `kernPOST=1`: before its first
process the kernel runs XNU's own tests (`osfmk/tests`: the zone allocator, `os_log`, the console, thread
calls, turnstiles and gates, the VM, the arm64 platform's PAN, speculation guards and system call
mungers ...), some thirty of them and over a thousand checks, each said as PASS or FAIL on the serial
port. None may fail.

## What it is not

A Darwin user space: launchd, dyld and the rest are Apple's and are not here. With an IPSW and the
team's tools the Darwin 16.5 generation ran in QEMU (`base/qemu/run-darwin.sh`); the current Darwin 26
and 27 images are made for phones ([darwin27-ubuntu.ru.md](darwin27-ubuntu.ru.md)).
