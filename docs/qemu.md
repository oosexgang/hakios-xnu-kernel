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
| **The shell** (`main.c`) | |
| `help`, `echo WORDS...`, `clear` | The commands, by group; words said back; the screen emptied (VT100's, which the kernel's video console takes). |
| `time COMMAND...` | How long a command took on the clock, and of the processors' time in user space and in the kernel (getrusage). |
| `cd [DIR]`, `pwd` | The directory relative paths start from (chdir), and the kernel's path of it (F_GETPATH, as getcwd() asks it). |
| **The kernel** (`kernel.c`) | |
| `about`, `sysctl NAME[=VALUE]...` | The kernel's values, typed as the kernel types them (sysctl 0.3 and 0.4); with `=VALUE`, set first (`kern.hostname` only a platform binary may set). |
| `sysctl -a [PREFIX]` | Every name the kernel has, from one to the next (sysctl 0.2 and 0.1), and the value of each under PREFIX. |
| `vm` | The virtual memory's pages (free, active, wired, in the compressor) and its faults, as `vm_stat` asks them: host_statistics64 in a MIG message to the host's port. |
| `dmesg [LINES]` | The kernel's latest messages (proc_info's message buffer call). |
| `uptime`, `date` | The time since boot; the time of day, UTC, as the kernel has it from QEMU's clock. |
| **Files** (`files.c`) | |
| `ls [PATH]`, `cat FILE` | Directories (this one, without a PATH) and files: getdirentries64, lstat64, read. |
| `hash FILE...` | Each file or device read to its end, timed, and its FNV-1a hash, for this machine to compare. |
| `mount apfs DEVICE DIR`, `mount ramfs ramfs DIR` | A disk's APFS volume, read only; or the kernel's file system in memory, to be written. |
| `mkdir DIR...`, `write FILE WORDS...`, `rm PATH...` | Directories and files made and removed (where a file system takes writing: ramfs). |
| `xattr FILE [NAME VALUE]`, `xattr -d FILE NAME` | A file's extended attributes listed with their values, one set, one taken away (ramfs keeps them). |
| `stat PATH...`, `ln [-s] TARGET NAME`, `mv OLD NEW`, `chmod MODE PATH...` | What lstat says of a file (its kind and mode, links, inode, size, and where a symbolic link points); another name for it, a symbolic link to it, a new name, other permissions. |
| `files [LIMIT]` | The limit on a process's descriptors lowered (setrlimit), files opened until the kernel says no (EMFILE), and all put back. |
| `fill FILE BYTES` | A file of made-up bytes (byte i is i * 7 mod 251), written in pieces of 16 KB, for `hash` to check against this computer's. |
| `mmap FILE` | The file mapped, its pages brought in by its file system, and checked against what reading it gives. |
| `mmap-write FILE` | The file mapped shared and every byte of it turned over through the mapping, then msync'd: reading the file must give the turned bytes (its file system's page-out). |
| `memory MB` | That much memory of its own (anonymous mmap), a word on every page written and read back; the kernel's free memory before, with it and after. |
| **The network** (`net.c`) | |
| `ifconfig [NAME ADDRESS]` | The interfaces and their addresses; brings one up with an address. |
| `ping ADDRESS` | ICMP echo on a datagram socket, through the virtio network driver. |
| `serve [COUNT]` | A page over HTTP on port 80: the kernel's TCP from listen to close. `GET /bytes/N` gets N bytes made up as they go (byte i is i * 7 mod 251), for TCP to send a lot. |
| `echo-udp PORT [COUNT]` | Datagrams on a UDP port sent back to whoever sent them (a process in the background, for `udp 127.0.0.1 PORT` over the loopback). |
| `get ADDRESS PORT [PATH]`, `udp ADDRESS PORT WORDS...` | A page asked for over HTTP (TCP's connect, as a client): its size, the time it took and its FNV-1a hash, and the page itself if it is short; a datagram sent and its answer read. 10.0.2.2 is this computer. |
| **Processes** (`processes.c`) | |
| `ps` | The processes (proc_info). |
| `run PROGRAM [ARGUMENTS]` | A process of its own (posix_spawn), waited for (wait4): `run /sbin/launchd ps` is this program again, which, not being pid 1, does the one command and ends. |
| `spawn PROGRAM [ARGUMENTS]`, `kill PID [SIGNAL]`, `wait`, `sleep SECONDS` | A process in the background, a signal to it (`kill !`: the one spawned last), and how it ended; a pause. |
| `fork [PROGRAM [ARGUMENTS]]` | A copy of this process: the child writes a value of its memory, which the parent's copy must not see; or, given a program, becomes it (execve). |
| `crash null`, `illegal`, `breakpoint`, `readonly` | In a child (`run /sbin/launchd crash null`): a bad address, an undefined instruction, a breakpoint, a write to a page made read only (mprotect); the shell sees the signal the kernel made of it. |
| `alarm [COUNT]` | A timer of a tenth of a second (setitimer) whose SIGALRM a handler of its own catches, and sigreturn puts the thread back. |
| `unix-name PATH` | A socket bound to a name in a file system (AF_UNIX; ramfs keeps a node for it): a forked child connects to it by the name and writes, the shell reads it. |
| `shm [NAME]` | Memory shared by a name (shm_open, ftruncate, mmap MAP_SHARED) with a forked child, which writes into it; the parent sees the writes, and the name is taken away (shm_unlink). |
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
  (`workbench/qemu_host.py` is this computer's side); a datagram is answered; over the loopback, one of
  its processes serves another a million bytes, every one checked, and sends a datagram back.
- **The devices.** A second disk on the virtio block device is read whole and through its APFS volume,
  by hashes; a tone comes out of the virtio sound device at the frequency asked for
  (`workbench/tone_in_wav.py`); a button pressed on QEMU's keyboard and a touch on its tablet reach the
  input device, the touch where it was made.
- **Processes.** A process started by the shell runs as its child and is waited for; a forked child's
  memory is its own, and it can become another program; a signal ends a process in the background, and a
  process's mistakes end it with SIGSEGV, SIGILL and SIGBUS; a timer's signals reach a handler on time; a pipe
  carries what a child writes into it, and so does memory shared by name; a descriptor sent over a socket
  pair reads the file it was, and a socket in ramfs is reached by its name; two threads run on the two processors at once, and four take turns under
  one lock without losing a count; Mach messages go through a port and come back unchanged; kqueue's
  timer keeps time and it sees a pipe become readable.
- **Files and memory.** The root mounts from the RAM disk; a file is written in ramfs, read back and
  removed; a second name for a file (a hard link) is the same inode and survives a rename, and a
  symbolic link to it is read through; a file moves from one directory to another, and ramfs refuses what
  BSD refuses (a directory that is there, one not empty, a hard link to one); extended attributes are set, replaced, listed and taken away;
  relative paths start from the shell's own directory; a process gets no more descriptors than its
  limit; three megabytes written into ramfs read back as written, and so do bytes written through a
  shared mapping of it; a mapped file is what reading it gives; memory of its own holds what is written and is all
  given back, each of its pages a fault Mach's counts see; the kernel keeps a process's time, none of
  it while asleep, and filling new pages as the kernel's; values are set through sysctl; the kernel's
  messages are read; the date is this year's.
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

## Four processors, and one

`workbench/qemu_smp_test.sh` (part of `make qemu` too) boots on four: all of them start and run, programs
are told four, four threads of a program run side by side and eight take turns under one lock without
losing a count. Then on one, where nothing is sent to another processor: threads and the lock all the same.

## What it is not

A Darwin user space: launchd, dyld and the rest are Apple's and are not here. With an IPSW and the
team's tools the Darwin 16.5 generation ran in QEMU (`base/qemu/run-darwin.sh`); the current Darwin 26
and 27 images are made for phones ([darwin27-ubuntu.ru.md](darwin27-ubuntu.ru.md)).
