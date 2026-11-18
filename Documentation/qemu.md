# The kernel in QEMU

QEMU's `virt` machine runs the kernel of this repository on any Linux computer, booted the way a phone
boots it: a real UEFI starts the loader, the loader takes the kernel from a payload, gives it the
emulator's board (`arch/arm64/boot/dts/qemu-virt`) and jumps to it. Nothing of Apple's is needed beyond the kernel's
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
on a socket (`--qmp`, for `tools/qemu/touch.py` to touch the screen and press the buttons), and runs a
command here while it types (`--then`). `--identity DeviceTree.bin` gives the payload which iPhone Darwin is told it
is, as `./bench` does on a phone (`BENCH_IDENTITY`), for a loader built without one; `--image FILE` boots an image of
`tools/image.py` as it is. Its head lists every option.

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

`tools/qemu-root.sh` makes an APFS image (`tools/apfs/mkapfs.py`) with `/dev`, `/etc/motd`,
`/mnt` and `/sbin/launchd`, which is `tools/qemu-init/`: a static program that makes its system calls
itself. As the kernel's first process it says what the kernel says of itself, then reads the console a
line at a time:

The commands, by what of the kernel they try (one file of `tools/qemu-init/` each):

| Command | What it does, and with what of the kernel |
| --- | --- |
| **The shell** (`main.c`) | |
| `help`, `echo WORDS...`, `clear` | The commands, by group; words said back; the screen emptied (VT100's, which the kernel's video console takes). |
| `time COMMAND...` | How long a command took on the clock, and of the processors' time in user space and in the kernel (getrusage). |
| `as UID:GID COMMAND...` | The command in a child that has become that user and group (setgroups, setgid, setuid): what the kernel lets a user who is not root do, and what it does not (EACCES, EPERM). |
| `nice N COMMAND...` | The command in a child of that priority (setpriority, getpriority): -20 runs first, 20 last; only root may put a process sooner. |
| `cd [DIR]`, `pwd` | The directory relative paths start from (chdir), and the kernel's path of it (F_GETPATH, as getcwd() asks it). |
| **The kernel** (`kernel.c`) | |
| `about`, `sysctl NAME[=VALUE]...` | The kernel's values, typed as the kernel types them (sysctl 0.3 and 0.4); with `=VALUE`, set first (`kern.hostname` only a platform binary may set). |
| `sysctl -a [PREFIX]` | Every name the kernel has, from one to the next (sysctl 0.2 and 0.1), and the value of each under PREFIX. |
| `vm` | The virtual memory's pages (free, active, wired, in the compressor) and its faults, as `vm_stat` asks them: host_statistics64 in a MIG message to the host's port. |
| `random [KB]` | The kernel's random bytes: two from getentropy differ, and it gives no more than 256 at once; KB of /dev/random (64 without) has every byte value in it, none under half or over twice its share. |
| `dmesg [LINES]` | The kernel's latest messages (proc_info's message buffer call). |
| `uptime`, `date` | The time since boot; the time of day, UTC, as the kernel has it from QEMU's clock. |
| **Files** (`files.c`; their pages, and memory: `memory.c`) | |
| `ls [PATH]`, `cat FILE` | Directories (this one, without a PATH) and files: getdirentries64, lstat64, read. |
| `hash FILE...` | Each file or device read to its end, timed, and its FNV-1a hash, for this machine to compare. |
| `mount apfs DEVICE DIR`, `mount ramfs ramfs DIR` | A disk's APFS volume, read only; or the kernel's file system in memory, to be written. |
| `df` | The file systems mounted (getfsstat, each asked now): where, of what kind, from what, their size and room, and which are read only. |
| `mkdir DIR...`, `write [-a] FILE WORDS...`, `rm PATH...` | Directories and files made (or, with `-a`, added to: O_APPEND) and removed (where a file system takes writing: ramfs). |
| `xattr FILE [NAME VALUE]`, `xattr -d FILE NAME` | A file's extended attributes listed with their values, one set, one taken away (ramfs keeps them). |
| `umask [MODE]`, `chown UID:GID PATH...` | What a new file is made without, and whose a file is. |
| `chflags FLAGS PATH...` | A file's flags (chflags, in hexadecimal; `stat` says them): UF_IMMUTABLE (2) keeps it from being written, renamed or removed, UF_APPEND (4) lets it only be added to. |
| `access PATH...` | Whether this process may read, write and run each (access(), by the real user and group: `as` shows another's). |
| `cp SOURCE TARGET` | The file's bytes into a new file of its permissions (a program too: set-user-ID, it runs as its owner). |
| `truncate FILE BYTES` | A file cut short, or grown: what it grew by must read as zeros, not as what the file held before it was cut. |
| `touch FILE [SECONDS]` | A file made if it is not there and its times set (utimes), to now or to a second given; the times lstat then says. |
| `stat PATH...`, `ln [-s] TARGET NAME`, `mv OLD NEW`, `chmod MODE PATH...` | What lstat says of a file (its kind and mode, links, inode, size, owner, and where a symbolic link points); another name for it, a symbolic link to it, a new name, other permissions. |
| `files [LIMIT]` | The limit on a process's descriptors lowered (setrlimit), files opened until the kernel says no (EMFILE), and all put back. |
| `appends FILE COUNT` | This process and a forked child each add COUNT records of 1000 bytes to the file at the same time (O_APPEND): the file must end up 2000 * COUNT bytes long, every record whole and none written over another. |
| `fsize FILE BYTES` | In a child whose files may be no larger than BYTES (RLIMIT_FSIZE, SIGXFSZ ignored): a write of twice that is cut at it, one past it refused (EFBIG), and so is a truncate to more. |
| `opens FILE` | A file made anew and a symbolic link to it, and what open says as BSD has it: O_EXCL of one that is there (EEXIST), O_NOFOLLOW of the link (ELOOP), O_DIRECTORY of the file (ENOTDIR), a directory opened to write (EISDIR), and the file through the link. |
| `dir-walk DIR COUNT` | COUNT files made in a new directory, its entries read 512 bytes at a time (each once), then read again and each batch removed as it comes, as a program that empties a directory does: none passed over, the directory empty at the end. |
| `descriptors FILE` | A file made anew and what descriptors do with it, as BSD has them: lseek, pread and pwrite (which move no offset), a dup's offset shared, dup2, O_APPEND, a file read and written after its unlink until it is closed, and an empty non-blocking pipe's EAGAIN. |
| `vectors FILE` | A file made from three pieces in one writev, read back in one readv into three pieces of other sizes: the bytes in order. |
| `fill FILE BYTES` | A file of made-up bytes (byte i is i * 7 mod 251), written in pieces of 16 KB, for `hash` to check against this computer's. |
| `mmap FILE` | The file mapped, its pages brought in by its file system, and checked against what reading it gives. |
| `mmap-write FILE` | The file mapped shared and every byte of it turned over through the mapping, then msync'd: reading the file must give the turned bytes (its file system's page-out). |
| `mmap-private FILE` | The file mapped private and every byte turned over through the mapping: the mapping has the turned bytes (its own copies of the pages), and the file, through a shared mapping and through read(), what it had. |
| `mmap-coherent FILE` | The file mapped shared and every page of it read through the mapping, then written with pwrite in a page and across two: the mapping has the bytes at once (ramfs brings the cached pages up to date in place); and what is written through the mapping, read() has at once, no msync between. |
| `memory MB` | That much memory of its own (anonymous mmap), a word on every page written and read back; the kernel's free memory before, with it and after. |
| **The network** (`net.c`) | |
| `ifconfig [NAME ADDRESS]` | The interfaces and their addresses; brings one up with an address. |
| `ping ADDRESS` | ICMP echo on a datagram socket, through the virtio network driver. |
| `serve [COUNT]` | A page over HTTP on port 80: the kernel's TCP from listen to close. `GET /bytes/N` gets N bytes made up as they go (byte i is i * 7 mod 251), for TCP to send a lot. |
| `echo-udp PORT [COUNT]` | Datagrams on a UDP port sent back to whoever sent them (a process in the background, for `udp 127.0.0.1 PORT` over the loopback). |
| `get ADDRESS PORT [PATH]`, `udp ADDRESS PORT WORDS...` | A page asked for over HTTP (TCP's connect, as a client): its size, the time it took and its FNV-1a hash, and the page itself if it is short; a datagram sent and its answer read. 10.0.2.2 is this computer. |
| **Processes** (`processes.c`; between them: `ipc.c`; threads: `threads.c`) | |
| `ps` | The processes (proc_info). |
| `id` | This process's user and group, real and effective (a set-user-ID program's effective user is its file's owner). |
| `run PROGRAM [ARGUMENTS]` | A process of its own (posix_spawn), waited for (wait4): `run /sbin/launchd ps` is this program again, which, not being pid 1, does the one command and ends. |
| `spawn PROGRAM [ARGUMENTS]`, `kill PID [SIGNAL]`, `wait`, `sleep SECONDS` | A process in the background, a signal to it (`kill !`: the one spawned last), and how it ended; a pause. |
| `fork [PROGRAM [ARGUMENTS]]` | A copy of this process: the child writes a value of its memory, which the parent's copy must not see; or, given a program, becomes it (execve). |
| `crash null`, `illegal`, `breakpoint`, `readonly` | In a child (`run /sbin/launchd crash null`): a bad address, an undefined instruction, a breakpoint, a write to a page made read only (mprotect); the shell sees the signal the kernel made of it. |
| `mask` | In a child (`run /sbin/launchd mask`: nobody may signal the first process): SIGUSR1 blocked (sigprocmask) and sent by a child of its own; it waits, pending (sigpending), and its handler runs once it is let through. |
| `alarm [COUNT]` | A timer of a tenth of a second (setitimer) whose SIGALRM a handler of its own catches, and sigreturn puts the thread back. |
| `unix-name PATH` | A socket bound to a name in a file system (AF_UNIX; ramfs keeps a node for it): a forked child connects to it by the name and writes, the shell reads it. |
| `shm [NAME]` | Memory shared by a name (shm_open, ftruncate, mmap MAP_SHARED) with a forked child, which writes into it; the parent sees the writes, and the name is taken away (shm_unlink). |
| `fds N...` | Whether each descriptor is open in this process (fcntl F_GETFD). |
| `cloexec FILE` | Descriptors of the file marked FD_CLOEXEC (by O_CLOEXEC and F_DUPFD_CLOEXEC) and not (by dup2, which leaves the mark behind): a program started with posix_spawn has the unmarked ones, and not the marked, which closed as it started. |
| `pipe WORDS...` | The words through a pipe: a child writes them into it (`/sbin/launchd >N echo ...`, its descriptor N), the shell reads them. |
| `unix FILE` | FILE opened, its descriptor sent over a socket pair with a word (sendmsg, SCM_RIGHTS), and the file read through the descriptor that comes out. |
| `threads [COUNT]` | Threads of its own (bsdthread_create) sharing some counting, timed against one thread doing it all: on two processors two take about half the time. |
| `locks [COUNT]` | Threads adding to one count under one lock, as `os_unfair_lock` makes one: a word, and the kernel's `ulock_wait` and `ulock_wake` for those that must wait. None of the count is lost. |
| `mach [COUNT]` | Messages sent to a Mach port of its own and received back (`mach_msg2`), and the time each took. |
| `fifo PATH` | A named pipe made in ramfs (mkfifo): a forked child opens it to write, the shell to read, each open waiting for the other's. |
| `flock FILE` | The file locked: a child's try, without waiting, is refused (EAGAIN); once it is let go, a child's is taken. |
| `byte-locks FILE` | Bytes 0 to 9 of the file locked to write (fcntl F_SETLK): a child's lock on 5 to 14 is refused, to read too, and one on 10 to 19 taken; F_GETLK names the shell as what is in the way; let go, the child has them. |
| `watch FILE` | A file made anew and watched through kqueue (EVFILT_VNODE) while it is written, its mode changed, linked, renamed and removed: what each gives (NOTE_WRITE and NOTE_EXTEND, NOTE_ATTRIB, NOTE_LINK, NOTE_RENAME, NOTE_DELETE). |
| `dgram [signal]` | Datagrams through a socket pair (AF_UNIX, SOCK_DGRAM): three of 5, 300 and 1 bytes read as three of those sizes, one looked at first (MSG_PEEK) still there, one longer than its room cut to it, and a send after shutdown refused (EPIPE); with `signal` (in a child: `run`), SIGPIPE ends the process. |
| `pipe-size` | A pipe filled a byte at a time without waiting, until the kernel says EAGAIN: how much it holds; full, poll does not find it writable, and with 512 bytes (PIPE_BUF) read out of it, it does. |
| `poll` | poll and select on a pipe: empty, each waits out its 100 ms and finds nothing; with a byte in it, each finds it readable at once. |
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
  process's mistakes end it with SIGSEGV, SIGILL and SIGBUS; a timer's signals reach a handler on time, and a signal blocked waits until it is let through; a pipe
  carries what a child writes into it, and so does memory shared by name; a descriptor sent over a socket
  pair reads the file it was, and a socket in ramfs is reached by its name; two threads run on the two processors at once, and four take turns under
  one lock without losing a count; Mach messages go through a port and come back unchanged; kqueue's
  timer keeps time and it sees a pipe become readable, and so do poll and select; a named pipe in ramfs
  carries what a child writes into it, and a lock on a file, or on some of its bytes, keeps another process out until it is let go; a
  script runs through the program its `#!` line names; a user who is not root reads what is theirs and not
  what is another's, and may lower a process's priority, not raise it; a set-user-ID program runs as its file's
  owner until a user who is not root writes it; in a sticky directory a file is removed by its owner alone;
  a user who is not root sends no signal to root's process, and a process whose parent ends is the first
  process's; descriptors marked close-on-exec are closed in a program started, and only they; datagrams keep
  their sizes, and a send after shutdown is EPIPE, or SIGPIPE; a pipe holds 64 KB; a file that is not a program
  is not run.
- **Files and memory.** The root mounts from the RAM disk; a file is written in ramfs, read back and
  removed; a second name for a file (a hard link) is the same inode and survives a rename, and a symbolic
  link to it is read through; a file moves from one directory to another, and ramfs refuses what BSD
  refuses (a directory that is there, one not empty, a hard link to one), and renames as POSIX has it (two
  names of one file onto each other, a directory onto a file or onto one not empty); paths fail as BSD's do
  (symbolic links that lead to each other, a file taken for a directory, a mount point removed); a file's
  flags keep it from being changed, or let it only be added to; extended attributes are set,
  replaced, listed and taken away; relative paths start from the shell's own directory; a file made under
  a mask is made without what it masks, and is given to another owner; descriptors do what BSD's do; a
  process gets no more descriptors than its limit; a file cut short and grown again reads as zeros past
  where it was cut, and its times are set to a second given; a process's files are held to its limit
  (RLIMIT_FSIZE); ramfs filled refuses more, and has its room back once a file goes; a directory of hundreds
  of files is read a little at a time, and emptied as it is read; a file changed is seen through kqueue;
  a file moves to no other file system, and the .. of one mounted on a directory leads to that directory's
  parent; three megabytes written into ramfs read back as
  written, and so do bytes written through a shared mapping of it, while a private mapping's writes stay its
  own; a file written while mapped is seen through the mapping at once, and what is written through the
  mapping is read at once; a mapped file is what reading it gives;
  memory of its own holds what is written and is all given back, each of its pages a fault Mach's counts
  see; the kernel keeps a process's time, none of it while asleep, and filling new pages as the kernel's;
  values are set through sysctl; the kernel's random bytes are as even as random bytes are; the kernel's messages are read; the date is this year's.
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
losing a count; and what goes between processes and through ramfs is tried with the processes on any of
the four: a lock on a file's bytes, a named pipe, a signal held while blocked, a directory emptied as it is
read, a file watched through kqueue, files mapped private and shared, datagrams, a file size limit. Then on
one, where nothing is sent to another processor: threads and the lock all the same.

## What it is not

A Darwin user space: launchd, dyld and the rest are Apple's and are not here. With an IPSW and the
team's tools the Darwin 16.5 generation ran in QEMU; the current Darwin 26
and 27 images are made for phones ([darwin27-ubuntu.ru.md](darwin27-ubuntu.ru.md)).
