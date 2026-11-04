# HakiOS

**Apple's open-source Darwin kernel (XNU) on Android phones**, the way Android is Linux on phones: a boot loader of its own, drivers of
its own, a configuration system and a version of its own. The same kernel runs in QEMU with nothing of Apple's and nothing of any
phone's.

**Where it stands.** The POCO X5 Pro 5G runs the Darwin 16.5 user space you lay out yourself, and its window server draws through Metal
on the Adreno 642L, at 100 to 104 frames a second in page swipes on a 120 Hz panel. 120 has not been reached: [the measurements](reference/metal-measurements.md).


## Guide: run it

| Page | What |
|---|---|
| [Getting started](getting-started/index.md) | three ways in, and what your machine needs |
| [QEMU](getting-started/qemu.md) | the open kernel in a few commands, or the Darwin user space you lay out yourself |
| [On the phone](getting-started/phone.md) | the POCO X5 Pro 5G, in RAM with `fastboot boot`, back to Android with a reboot |
| [Windows](windows.md) | the same from a Windows PC: what runs in WSL 2, what in Git Bash; a measured Metal run |
| [Installing beside Android](dual-boot.md) | HakiOS as a second system, a menu at every start, your data kept ([in Russian](ru/dual-boot.md)) |
| [Building from source](getting-started/building.md) | when you change something |

## Porting: bring a device

| Page | What |
|---|---|
| [Port a device](porting/index.md) | a device is a repository of its own over the common one: a defconfig, a status file, its drivers |
| [How the repositories fit](porting/repositories.md) | common, SoC, device, UEFI, and the manifest that names them |
| [Writing a driver](porting/drivers.md) | a driver as an option of the kernel's own configuration |
| [Drivers in the configuration](drivers-config.md) | `CONFIG_HAKIOS_*`, and how a choice reaches XNU |
| [Bring-up on a PKG110](port-pkg110.md) | a phone that reports another brand: what was checked on it, and how |

## Drivers and GPU: what each does, and how far

| Page | What |
|---|---|
| [Metal on the Adreno](reference/metal.md) | the two Metal paths compared: interfaces, build, boot arguments, how to measure, known gaps |
| [Metal measurements](reference/metal-measurements.md) | what the phone did, in numbers, and what did not help |
| [Metal plan](metal-plan.md) | how it got here, step by step |
| [The shader compiler](air.md), [its report](air-report.md) | Apple's shader bitcode into the Adreno's code |
| [Metal, why and where the wall is](ru/metal.md) | the first analysis, in Russian |
| [Development notes](ru/notes.md) | the old README, whole: the history of the work, in Russian |
| [Devices](devices/index.md) | how far each part works, device by device: [redwood](devices/redwood.md), [QEMU](devices/qemu.md) |
| [Cellular](cellular.md) | the modem's bring-up ([in Russian](ru/cellular.md)) |
| [Driver reviews](driver-review/README.md) | each driver read for what it does and what was and was not verified |
| [Network review](net-review.md) | the network stack, read the same way |

## Concepts: how it fits together

| Page | What |
|---|---|
| [Architecture](concepts/architecture.md) | the three programs between a phone's firmware and a user space; what is written to the phone |
| [Configuration](configs.md) | defconfigs and fragments, like Linux's |
| [Versioning](versioning.md) | a version string that says what it is built from |
| [UEFI boot](uefi-boot.md), [U-Boot](u-boot.md) | the two firmwares that can start the loader |
| [Installing: the design](install.md), [safely](install-safe.md) | what installing means here, and what it must never do |

## Reference: look something up

| Page | What |
|---|---|
| [Drivers](reference/drivers.md) | every driver, made from the source |
| [Boot arguments](boot-args.md) | every argument any part of the system reads, made from the source |
| [Commands](reference/tools.md) | `make` targets and the scripts below them |

## The project

| Page | What |
|---|---|
| [Changelog](../CHANGELOG.md) | what changed, newest first |
| [Comparison](comparison.md) | HakiOS next to the other XNU-on-a-phone projects |
| [Legal review](legal-review.md), [LEGAL](../LEGAL.md), [compliance](../COMPLIANCE.md), [third parties](../THIRD_PARTY_LICENSES.md) | what is and is not distributed, and what is still to verify |
| [Contributing](../CONTRIBUTING.md), [community](../COMMUNITY.md) | how to take part |
| [The site](site/README.md) | the installer's guide as one web page |

## What works

As each device's own `device.conf` records it: [all devices](devices/index.md). Where the Metal row is ahead of a device's file, the
measurements are the source. The rows about the lock screen and the apps describe what happens when you add layer 2 below yourself.

| | POCO X5 Pro 5G (`redwood`) | QEMU `virt` |
|---|---|---|
| Lock screen, home screen, Control Center, the stock apps | yes | yes (slow: the CPU is emulated) |
| Touch, multitouch, buttons | yes | yes (the mouse and the keyboard) |
| All 8 CPU cores, the big one at 2.4 GHz | yes | no |
| GPU (OpenGL ES through Mesa) | yes | no |
| Metal | partly: the window server draws through it, on two paths ([Metal](reference/metal.md)), off by default; swipes 100 to 104 frames a second, not 120; other programs not shown | partly: a device Metal loads; no GPU, nothing drawn |
| Wi-Fi | yes | yes (virtio-net) |
| Sound, microphone | yes | sound only (virtio) |
| The battery's level | yes | no |
| Cameras, vibration | yes | no |
| `/private/var` kept across reboots, Sileo, SSH | yes | no |
| Flashlight, temperature guard | partly: written, some of it seen ([redwood](devices/redwood.md)) | no |
| Cellular | partly: the modem is up, the SIM bound, the network seen; no registration yet ([cellular](cellular.md)) | no |
| Bluetooth, charging under this kernel, Apple's services | no | no |

## The three layers

1. **The open core**: the kernel port, the loader, the drivers, the tools. All source, in these repositories.
2. **What you put on top**: the kernel can start a user space that you supply. It was built so that it can start the Darwin 16.5 user
   space, which you lay out yourself from files you download from Apple; nothing of it is shipped, and nothing is circumvented to get
   it. Whether you may do that, and whether you want to, is your decision.
3. **Apple's services** (activation, Apple ID, iCloud, the App Store): not supported and not a goal.

By default the loader goes into RAM through `fastboot boot`, the system lives in RAM for a session, and a reboot brings the phone back to
its own Android. The one thing written by default is the loader's own log (a few KB) to the `logdump` partition; with `ufs_write=1` the
kernel writes only to a partition `darwin` that you create yourself. What is and is not written to a phone:
[Architecture](concepts/architecture.md#what-is-written-to-the-phone).

## What is in the repository, and what never is

In it: only our own code and data: the patch to Apple's open-source XNU (`xnu/hakios.patch`, APSL 2.0) and our files, the boot loader, our
programs and libraries, the tools, `prebuilt/` (built from this source; the loader **without** any Apple data: `make loader` makes the one
that starts Darwin, on your machine, from your own IPSW), and the device and SoC directories. Third parties' files and patches to their
code are listed with their licenses in [THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

Never in it: any file of Darwin (you build the system image on your machine from an IPSW you get yourself and hand to `sysimage/unpack.sh`;
nothing is downloaded for you, decrypted or circumvented); the phone's firmware (pulled from your own phone into `fw-*/`, which git
ignores); keys, certificates, activation, or anything that would make the phone pass as an iPhone to Apple.

The core kernel, the boot loader and the open tools build and run with nothing of Apple's: `make qemu-open` is the open kernel, a
placeholder loader and a tiny raw-syscall user space, in QEMU, with no IPSW, no Darwin user space and no firmware. Booting the stock
Darwin user space (`make qemu`, `make phone`) is an optional path that needs the IPSW and the phone's firmware, which you supply from your
own device. Licenses: the XNU patch under the Apple Public Source License 2.0, the terms met as [COMPLIANCE.md](../COMPLIANCE.md) says;
the rest as the file headers say. None of this is legal advice.

## The repositories

| Repository | What |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel/tree/main) | **common**: the kernel port, every common driver, the loader, user space, the tools, this documentation, and `devices/qemu` |
| [hakios-metal](https://github.com/oosexgang/hakios-metal) | Metal's command model on the Adreno, through the kernel's GPU driver |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G: its defconfig, status, UEFI platform, firmware script, and its SoC's layer (`soc/`: the Qualcomm SC7280 family) |
| [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | Infinix Hot 20 (MediaTek Helio G85): an experimental port with its own loader; not in `devices/manifest.conf` |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | the firmware that starts the loader: Mu-Silicium with HakiOS's boot path, banner and name, and HakiOS's U-Boot (`u-boot/`) |
| [hackios-web](https://github.com/oosexgang/hackios-web) | the website, which carries a copy of these pages |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | old: a pointer. User space is `userland/` in the common repository |

`devices/manifest.conf` in the common repository names the device and UEFI repositories and the revision each is at;
`tools/hakios-device.py sync` clones them into `devices/` and `uefi/`. One repository per device: it carries its SoC's layer, and
`tools/hakios-device.py new` makes one from the phone's own device tree, with every phone driver left out until it is ported. Each driver is
an option of the kernel's own configuration (`CONFIG_HAKIOS_*`, XNU's `options` and `conf/files`), so a device's defconfig builds exactly the
drivers it has. [How the repositories fit](porting/repositories.md).
