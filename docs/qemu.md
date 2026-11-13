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
`/mnt` and `/sbin/launchd`, which is `tools/qemu-init.c`: a static program that makes its system calls
itself. As the kernel's first process it says what the kernel says of itself, then reads the console a
line at a time:

| Command | What it does, and with what of the kernel |
| --- | --- |
| `about`, `sysctl NAME...` | The kernel's values, typed as the kernel types them (sysctl 0.3 and 0.4). |
| `ls [PATH]`, `cat FILE` | Directories and files: getdirentries64, lstat64, read. |
| `ps`, `uptime` | The processes (proc_info) and the time since boot. |
| `ifconfig [NAME ADDRESS]` | The interfaces and their addresses; brings one up with an address. |
| `ping ADDRESS` | ICMP echo on a datagram socket, through the virtio network driver. |
| `serve [COUNT]` | A page over HTTP on port 80: the kernel's TCP from listen to close. |
| `mount apfs DEVICE DIR` | A disk's APFS volume, read only. |
| `input [COUNT]` | What the touch screen and the buttons say (`/dev/hakios-touch`), as the kernel's input path hands it on. |
| `tone HZ [SECONDS]` | A sine wave written to the sound device, through the virtio sound driver. |

## What `make qemu` holds

The loader jumps to the kernel; the kernel starts on two processors and finds the emulator's board;
`en0` attaches with its link up and both input devices are found; the root mounts from the RAM disk;
`/sbin/launchd` runs as pid 1 and its shell answers what is typed; `en0` takes an address, QEMU's
gateway answers its pings, and its TCP serves this machine a page through a forwarded port; a second
APFS disk on the virtio block device is mounted and read; a tone played on the virtio sound device comes
out of QEMU at the frequency asked for (`workbench/tone_in_wav.py`); a button pressed on QEMU's keyboard
and a touch on its tablet reach the input device, the touch where it was made; nothing panics.

## What it is not

A Darwin user space: launchd, dyld and the rest are Apple's and are not here. With an IPSW and the
team's tools the Darwin 16.5 generation ran in QEMU (`base/qemu/run-darwin.sh`); the current Darwin 26
and 27 images are made for phones ([darwin27-ubuntu.ru.md](darwin27-ubuntu.ru.md)).
