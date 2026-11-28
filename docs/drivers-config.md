# Drivers in the configuration: what the kernel is built with

A Linux kernel builds a driver when its `Kconfig` symbol is on: `drivers/input/touchscreen/Kconfig` declares
`TOUCHSCREEN_GOODIX`, the directory's `Makefile` says `obj-$(CONFIG_TOUCHSCREEN_GOODIX) += goodix.o`, and a device's defconfig
(`arch/arm64/configs/vendor/<device>_defconfig` in an Android kernel) turns on what that phone has. Every phone vendor's tree is
that, with its own drivers added the same way.

This kernel is Apple's XNU, which has the same idea under other names, much older than Kconfig: `config/MASTER` declares the
**options** (`options APFS # <apfs>`), a component's `conf/files` lists its files as `standard` or `optional <option>`, and a
configuration (`DEVELOPMENT = [ ... ]` in `MASTER.<arch>.<platform>`) is the list of what is on; `SETUP/config/doconf` and
`config(8)` turn that into the list of objects and `-D<OPTION>` for the compiler. The drivers of this port are XNU options of
that kind. Nothing here imitates Kconfig's files; what a driver is, is written once per subsystem, in `drivers/<subsystem>/conf`.

```
drivers/<subsystem>/conf      the drivers of a subsystem: their files, what they require, what they are
configs/*_defconfig           CONFIG_HAKIOS_<NAME>=y or `# CONFIG_HAKIOS_<NAME> is not set`, beside the boot-argument symbols
tools/hakios-config.py kernel the choice of the .config put into the kernel tree (make kernel runs it)
```

## `drivers/<subsystem>/conf`

```
standard bsd/dev/hakios.input.c
	help    /dev/hakios-touch, the queue of touches and buttons, and where the phone's drivers are started from

driver GOODIX
	file    bsd/dev/hakios.goodix.c
	help    the touch screen: a Goodix GT9916 or a FocalTech FT3680 on SPI (touchscreen=1) ...

driver SOUND
	file    bsd/dev/hakios.sound.c
	file    bsd/dev/hakios.guard.c
	requires ADSP SPEAKER
	help    the speakers playing (sound=N) ...
```

| Word | Means |
|---|---|
| `standard PATH [OPTION...]` | a file every kernel has (with XNU options of its own, if given: `hakios.apfs_check.c apfs`) |
| `driver NAME` | the symbol `CONFIG_HAKIOS_NAME`, the XNU option `HAKIOS_NAME`, the attribute `hakios_name` |
| `file PATH [OPTION...]` | one of its files, as its path in Apple's tree; a `.cpp` of `iokit/Kernel` also wants `iokitcpp` |
| `requires NAME...` | drivers it cannot be built without (it calls them, and nothing could stand in for them) |
| `default y\|n` | what a configuration that does not name it gets; `y` if not said |
| `help TEXT` | what it is, and which boot-argument starts it; more `help` lines go on |

Every `.c` and `.cpp` of `drivers/map` is built by exactly one `standard` line or `driver`, of the conf of its own subsystem;
`tools/drivers-check.py` (CI) checks that, and a file that none builds would be in the tree and never compiled.

The 26 drivers there are now, by subsystem:

| Subsystem | Drivers (`CONFIG_HAKIOS_*`) | Always built |
|---|---|---|
| core | `SOCINFO` | `hakios.version.c`, `hakios.platform.c`, `hakios.stubs.c` |
| input | `VIRTIO_INPUT`, `GOODIX`, `BUTTONS` | `hakios.input.c` |
| storage | `UFS`, `VIRTIO_BLK` | `hakios.image.c`, `hakios.sparse.c`, `hakios.apfs_check.c` |
| gpu | `ADRENO` | |
| camera | `CAMERA`, `TORCH` | |
| haptic | `HAPTIC` | |
| audio | `ADSP`, `SPEAKER`, `SOUND` (requires ADSP, SPEAKER), `MIC` (requires ADSP, SOUND), `VIRTIO_SOUND` | `hakios.audio.c` |
| power | `BATTERY`, `CPUFREQ`, `THERMAL`, `PMIC`, `BATTGUARD` | |
| wifi | `WIFI_LX` (requires QMI), `WIFI` (requires QMI, WIFI_LX) | |
| bluetooth | `BT` | |
| modem | `QMI`, `MODEM` (requires QMI, UFS) | |
| net | `VIRTIO_NET` | `hakios.net.c` |

`tools/hakios-config.py help HAKIOS_SOUND` says the same of one of them, and what requires it.

## A device's defconfig chooses its drivers

A driver nobody names is built (`default y`): every driver was built before there were these symbols, and a `.config` made
then still gives the same kernel. A device leaves out what it does not have:

```
# configs/qemu_defconfig (in the tree): QEMU's virt machine, the phone's drivers left out of the kernel
#include common_defconfig
#include open.config
#include virt.config
CONFIG_DEVICE="qemu"
# CONFIG_HAKIOS_GOODIX is not set
# CONFIG_HAKIOS_ADRENO is not set
...
```

```bash
make qemu_defconfig        # .config: 4 drivers built in (the virtio ones), 21 left out
make config                # says which are built and which are not
make kernel                # builds that kernel
```

A port to another phone starts the same way: its `configs/<name>.config` leaves out each driver whose hardware it has not
ported yet (the addresses and pins in a driver are the POCO X5 Pro's, and its head says where each came from), and turns one
on when it has been made to work there. A phone with another touch controller adds a driver, not a branch: its file in
`drivers/input/`, a line in `drivers/map`, a `driver` block in `drivers/input/conf`, and `CONFIG_HAKIOS_<IT>=y` in its defconfig.

### What is checked

- `# CONFIG_HAKIOS_X is not set` means **left out**, not "the default": a fragment can take a driver out of a configuration
  that an earlier file put it in, as in Linux.
- A driver built without one it `requires` is refused: `CONFIG_HAKIOS_SOUND requires CONFIG_HAKIOS_ADSP (drivers/audio/conf:13)`.
- A boot-argument that only left-out drivers read is refused, with the driver named:
  `CONFIG_TOUCHSCREEN gives touchscreen=, which only hakios.buttons.c, hakios.goodix.c read, and they are not built
  (CONFIG_HAKIOS_BUTTONS, CONFIG_HAKIOS_GOODIX)`. The kernel would not complain: the argument would just never be looked at.
  (The readers are found the way `tools/boot-args.py` finds them, in the source.)

## How the choice reaches XNU's build

`tools/hakios-config.py kernel` (run by `make kernel` through `tools/make-kernel.sh`, and by `xnu/apply.sh`) writes, into the
kernel tree, only what has changed:

| File in the tree | What | In `xnu/hakios.patch`? |
|---|---|---|
| `bsd/conf/files`, `iokit/conf/files` | one block between `# HakiOS drivers: ...` and `# end of the HakiOS drivers`: each file `standard` or `optional hakios_<name>` | yes: it does not depend on the configuration |
| `config/MASTER.hakios` | `#  HAKIOS_DRIVERS = [ hakios_goodix ... ]` (the attributes that are on) and an `options HAKIOS_<NAME> # <hakios_<name>>` line for every driver | no: made for each build, kept out by `xnu/export.sh` |
| `bsd/dev/hakios.drivers.h` | the list, for `sysctl kern.hakios.drivers` and the boot log line `hakios-kernel: N drivers built in` | no |

and what the patch carries once: `SETUP/config/doconf` reads `MASTER.hakios` after `MASTER` and `MASTER.arm64.bcm2837`; the
kernel configurations of `MASTER.arm64.bcm2837` (`RELEASE`, `DEVELOPMENT`, `DEBUG`, `KASAN`) include `HAKIOS_DRIVERS`; each
component's `conf/Makefile` makes its configuration again when `MASTER.hakios` changes. XNU's build passes the options as
`-DHAKIOS_<NAME>` in the compiler's command line rather than as a file an object depends on, so when the choice changes,
`kernel` also removes the objects of the files that ask about it (`#ifdef HAKIOS_*`): they are built again.

Linux's `ath11k` object (`tools/mk-ath11k.sh`) is linked in only with `CONFIG_HAKIOS_WIFI` (`xnu-build.sh`): its calls are
answered by `hakios.lx.c`.

## Leaving a driver out without breaking the others

The rest of the kernel calls the drivers in a few places: where they are started (`hakios.input.c`, `hakios.audio.c`,
`bsd_init.c`), where a run ends or a panic leaves (`loader_return.c`, the thermal and battery guards, `machine_routines.c`),
and a few questions on the way ("is this device one of the storage's?", "how warm is the battery?"). Those calls stay as they
are. For a driver that is left out, `drivers/core/hakios.stubs.c` (XNU keeps `bsd/kern/bsd_stubs.c` for the same purpose) has
what each such call then does: nothing, and the answer "no", "not known" or "not there". With every driver built it compiles to
nothing, so the phone's kernel is the one it was.

What several drivers share was moved where none of them owns it, `drivers/core/hakios.platform.c`: the PMIC arbiter's lock
(from `hakios.buttons.c`), the locks of the always-on processor's mailbox, the shared memory's allocation, RPMh and the SMMU
(from `hakios.wifi.c`), and the `kern.hakios` sysctl node (from `hakios.battery.c`). Before, the camera could not be linked
without the Wi-Fi driver, and every driver's sysctls without the battery's. The camera's and the motor's firmware is read with
`hakios_image_file()` directly instead of through the Wi-Fi driver.

## What was checked

On a Mac (Apple clang, the toolchain of `xnu-deps/`), 27 October 2026: the kernel built and linked with every driver
(`make kernel` without a `.config`), and with `make qemu_defconfig`, four drivers of 25. The QEMU kernel was not booted yet,
and nothing here was run on the phone. `tools/hakios-config-test.py` and `tools/drivers-check.py` pass.
