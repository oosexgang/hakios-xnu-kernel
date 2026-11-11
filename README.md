# HakiOS on Qualcomm phones

English | [简体中文](README.zh-CN.md)

Apple's open-source XNU kernel and the iOS user space, running on Android phones with Qualcomm chips.
Nothing is flashed: a UEFI is started with `fastboot boot`, the system lives in RAM, and a reboot
brings Android back.

> Experimental. Needs an unlocked bootloader. No tool here may flash, erase or write the phone's storage.

## Status

| Board | Device | SoC | State |
| --- | --- | --- | --- |
| `renoir` | Xiaomi Mi 11 Lite 5G | Snapdragon 780G (SM7350) | iOS 27.2 (24B119) boots from RAM to the home screen. Verified on the phone on 2026-11-10. |
| `redwood` | POCO X5 Pro 5G | Snapdragon 778G (SM7325) | The first port, on iOS 16.5 (`base/`). Its drivers are in this kernel; not yet booted with iOS 26/27. |
| `qemu-virt` | QEMU `virt` | — | The iOS 16.5 generation runs there. Not yet set up for the current kernel. |

On Renoir today:

- **Works:** boot to the home screen, touch, buttons, the CPU at full clock, populated widgets.
- **Software rendering** (`gpu=0`): Liquid Glass, CASDF shapes and the large lock-screen clock are
  not drawn. The plan for Metal: [docs/metal-glass.md](docs/metal-glass.md).
- **Written, tested on the host and under QEMU, not yet verified on the phone:** brightness and
  Auto-Brightness ([docs/backlight.md](docs/backlight.md)), the thermal limiter ([docs/thermal.md](docs/thermal.md)).
  Both only read by default.
- **Off:** Wi-Fi, sound, camera, haptics, battery. Their drivers are wired for redwood's parts.
- **Open:** stable 60 FPS, long-run stability.

## Start here

| You want to | Read |
| --- | --- |
| Understand how it fits together | [docs/architecture.md](docs/architecture.md) |
| Port it to your phone | [docs/porting.md](docs/porting.md) |
| Build the kernel or the loader | [docs/building.md](docs/building.md) |
| Know what a driver or a boot argument does | [docs/drivers.md](docs/drivers.md), [docs/boot-args.md](docs/boot-args.md) |
| Change something and send it back | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Carry over a branch or patch from the old layout | [docs/migrating.md](docs/migrating.md) |

Everything that needs no phone runs with one command:

```sh
make check
```

## Layout

| Path | Contents |
| --- | --- |
| `devices/`, `soc/` | **One file per board and per SoC.** The only place a device is described; the kernel's and the loader's tables are generated from it. |
| `kernel/` | The kernel: Apple's pinned release, the files HakiOS adds (`overlay/`), the changes to Apple's files (`patches/`), and the tools that assemble and build it. |
| `loader/` | The UEFI application that starts the kernel and stays resident for its log. |
| `ios/` | What iOS 26/27 user space needs: libraries inserted into a few processes, the PAC rewriter, image builders, the boot tool. |
| `userland/` | Earlier user-space sources still in use or being ported: the Metal device, the shader compiler for Mesa, the audio plug-in, APFS image tools. |
| `tests/` | Host tests: the kernel's device-independent drivers against models of the hardware, and the tools. |
| `tools/` | `boards.py` (board files), `bootargs.py` (the generated reference), lint and publish checks. |
| `docs/` | Documentation. |
| `base/` | The first generation's workspace (iOS 16.5 on redwood and QEMU). Kept for the parts not yet brought over: the UEFI port, the Wi-Fi layer, the QEMU scripts. |

## Porting in one paragraph

A phone is described by `devices/NAME/board.toml` and its chip by `soc/SOC.toml`.
`tools/boards.py new NAME --soc SOC --dts phone.dts` starts both from the phone's Android device tree.
The kernel starts a driver only on a board that lists it, so a new phone begins with nothing but the
display and gains hardware one line at a time. Parts nobody has a driver for yet (a touch controller,
an amplifier) need one written. The guide: [docs/porting.md](docs/porting.md).

## What is not in this repository

Apple's files (XNU is fetched at build time; iOS comes from Apple's IPSW on your machine), the phones'
firmware (taken from the same phone), and third-party toolchains. Also, for now, the resident loader
and image tooling Renoir is booted with, which live in the team's unpublished working tree: until they
are published, a Renoir boot cannot be reproduced from a clean checkout.
Details: [docs/architecture.md](docs/architecture.md#what-is-not-in-this-repository).

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
