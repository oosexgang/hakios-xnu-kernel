<p align="center">
  <img src="docs/assets/banner.svg" alt="hakios" width="830">
</p>

<p align="center">
  Apple's open-source XNU kernel and the iOS user space, on phones with Qualcomm chips.
</p>

<p align="center">
  <a href="docs/architecture.md">Architecture</a> · <a href="docs/porting.md">Porting</a> · <a href="docs/building.md">Building</a> · <a href="docs/drivers.md">Drivers</a> · <a href="https://github.com/oosexgang/xnu-kernel-qcom/tree/devices">Devices</a> · <a href="CONTRIBUTING.md">Contributing</a>
</p>

<p align="center">
  <a href="https://github.com/oosexgang/xnu-kernel-qcom/actions/workflows/check.yml"><img alt="checks" src="https://github.com/oosexgang/xnu-kernel-qcom/actions/workflows/check.yml/badge.svg"></a>
  <a href="docs/README.md"><img alt="documentation" src="https://img.shields.io/badge/docs-read-5b6143"></a>
  <a href="https://github.com/oosexgang/xnu-kernel-qcom/tree/devices"><img alt="device packages" src="https://img.shields.io/badge/devices-branch-5b6143"></a>
  <a href="https://t.me/oosexgangdevs"><img alt="Telegram" src="https://img.shields.io/badge/Telegram-oosexgangdevs-d8392b?logo=telegram&logoColor=white"></a>
  <a href="kernel/UPSTREAM"><img alt="XNU release" src="https://img.shields.io/badge/XNU-12377.121.6-3a402e"></a>
</p>

<p align="center">
  English | <a href="README.zh-CN.md">简体中文</a>
</p>

# HakiOS

HakiOS runs Apple's open-source XNU kernel and an iOS user space on Android phones with Qualcomm
chips. Nothing is flashed: a UEFI is started with `fastboot boot`, the system lives in RAM, and a
reboot brings Android back.

> Experimental. Needs an unlocked bootloader. No tool here may flash, erase or write the phone's storage.

This branch is the project itself, for any phone: the kernel, the loader, the tools, the description
of each supported chip. It names no phone. A phone is a **device package**, a small directory kept on
the [`devices` branch](https://github.com/oosexgang/xnu-kernel-qcom/tree/devices), and one command
brings it into a checkout.

## What is here

- **One kernel for every board.** The board is found at boot; a driver starts only where the board's
  file lists it, so a machine nobody described is never written to.
- **Boards are data.** A phone is one TOML file and its chip another. Addresses, pins, firmware
  directories and boot defaults live there, not in the drivers. A tool starts both files from the
  phone's Android device tree and says what can be turned on next.
- **Device packages.** Phones are kept apart from the code. The kernel, the loader and the libraries
  are built for the packages that are present.
- **Checks that need no phone.** The drivers run on the host against models of the hardware, the
  porting guide is run on a made-up phone, the kernel sources are verified against Apple's release and
  compiled for syntax and types, on every push.
- **Pinned inputs.** Everything taken from elsewhere is in [`components.toml`](components.toml), at an
  exact version.

## Devices

| Package | Device | SoC | State |
| --- | --- | --- | --- |
| `renoir` | Xiaomi Mi 11 Lite 5G | Snapdragon 780G (SM7350) | iOS 27.2 (24B119) boots from RAM to the home screen. Verified on the phone on 2026-11-10. |
| `redwood` | POCO X5 Pro 5G | Snapdragon 778G (SM7325) | The first port, on iOS 16.5 (`base/`). Its drivers are in this kernel; not yet booted with iOS 26/27. |
| `qemu-virt` | QEMU `virt` | — | In this branch. The iOS 16.5 generation runs there; not yet set up for the current kernel. |

What works on each phone, what is written but not yet verified on it, and what is still off is in the
package's own README on the [`devices` branch](https://github.com/oosexgang/xnu-kernel-qcom/tree/devices).
SoC support is shared and lives here, in `soc/`: SM7350 and SM7325 so far.

## Quick start

```sh
git clone https://github.com/oosexgang/xnu-kernel-qcom.git
cd xnu-kernel-qcom
make check                        # everything that needs no phone

tools/device.py available         # the phones there are packages for
tools/device.py add renoir        # bring one in (devices/renoir)
tools/boards.py status renoir     # what it has on, what it can turn on next
```

Then build: [docs/building.md](docs/building.md). For a phone that has no package yet:
[docs/porting.md](docs/porting.md).

## Start here

| You want to | Read |
| --- | --- |
| Understand how it fits together | [docs/architecture.md](docs/architecture.md) |
| Port it to your phone | [docs/porting.md](docs/porting.md) |
| Build the kernel or the loader | [docs/building.md](docs/building.md) |
| Know what a driver or a boot argument does | [docs/drivers.md](docs/drivers.md), [docs/boot-args.md](docs/boot-args.md) |
| Know how boards and packages are described | [devices/README.md](devices/README.md) |
| Change something and send it back | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Carry over a branch or patch from an earlier layout | [docs/migrating.md](docs/migrating.md) |

## Layout

| Path | Contents |
| --- | --- |
| `devices/` | The board template, the emulator's board, and the packages you bring in. |
| `soc/` | **One file per chip**: where its blocks are, shared by every board with that chip. |
| `kernel/` | The kernel: Apple's pinned release, the files HakiOS adds (`overlay/`), the changes to Apple's files (`patches/`), and the tools that assemble, check and build it. |
| `loader/` | The UEFI application that starts the kernel and stays resident for its log. |
| `ios/` | What iOS 26/27 user space needs: libraries inserted into a few processes, the PAC rewriter, image builders, the boot tool. |
| `userland/` | Earlier user-space sources still in use or being ported: the Metal device, the shader compiler for Mesa, the audio plug-in, APFS image tools. |
| `tests/` | Host tests: the kernel's device-independent drivers against models of the hardware, and the tools. |
| `tools/` | `boards.py` (board files) with `devicetree.py` (what it reads from a phone's device tree), `device.py` (packages), `bootargs.py` (the generated reference), lint and publish checks. |
| `components.toml` | Everything taken from elsewhere, pinned to an exact version. |
| `docs/` | Documentation. |
| `base/` | The first generation's workspace (iOS 16.5). Kept for the parts not yet brought over: the UEFI port, the Wi-Fi layer, the QEMU scripts. |

## Porting in one paragraph

A phone is described by one board file and its chip by `soc/SOC.toml`.
`tools/boards.py new NAME --soc SOC --dts phone.dts` starts both from the phone's Android device tree.
The kernel starts a driver only on a board that lists it, so a new phone begins with nothing but the
display and gains hardware one line at a time; `tools/boards.py status NAME` says what can be turned
on next and what each of the rest still needs. Parts nobody has a driver for yet (a touch controller,
an amplifier) need one written. When it works, the package goes to the `devices` branch. The guide:
[docs/porting.md](docs/porting.md).

## What is not in this repository

Apple's files (XNU is fetched at build time; iOS comes from Apple's IPSW on your machine), the phones'
firmware (taken from the same phone), and third-party toolchains. Also, for now, the resident loader
and image tooling Renoir is booted with, which live in the team's unpublished working tree: until
they are published, a Renoir boot cannot be reproduced from a clean checkout.
Details: [docs/architecture.md](docs/architecture.md#what-is-not-in-this-repository).

## Community

News and progress: [t.me/oosexgangdevs](https://t.me/oosexgangdevs). Questions, bugs and new phones:
the repository's issues and pull requests; [CONTRIBUTING.md](CONTRIBUTING.md) says where a change goes.

## Licences

See [NOTICE.md](NOTICE.md) and the source headers. Apple system files, SDKs, device firmware and built
images are local build inputs and outputs, never repository content; `tools/check-publishable.sh`
looks for them.

## Acknowledgements

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel)
- [Data A Live](https://github.com/ShueiTsuki/data-a-live)
- [Mesa](https://mesa3d.org/)
- [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
- Apple's open-source XNU and the Linux drivers cited in individual source files

Apple, iOS and Metal are trademarks of Apple Inc. This project is not affiliated with Apple, Qualcomm or Xiaomi.
