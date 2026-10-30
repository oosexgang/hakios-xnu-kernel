# HakiOS ARM64e / Renoir

English | [简体中文](README.zh-CN.md)

ARM64e userland and kernel bring-up for Renoir (SM7350), using Apple's open-source
XNU xnu-12377.121.6 and the iOS 26 user space.

Built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), with ARM64e and
Renoir adaptations. Snapshot-specific build and boot notes are under `renoir/docs/`.

> Experimental. Requires an unlocked bootloader. Use the Renoir RAM-only workflow;
> do not flash or erase phone partitions.

## Status

| Part | Status |
| --- | --- |
| Target | Renoir / SM7350, without hardware PAC |
| Kernel | XNU xnu-12377.121.6 with board and ARM64e adaptations |
| User space | iOS 26 bring-up; see the snapshot's boot notes for its verification stage |
| Rendering | CPU/software rendering (`gpu=0`) |

## Layout

| Directory | Contents | Licence |
| --- | --- | --- |
| `kernel/` | `xnu-12377.121.6-hakios.patch`: every kernel change against Apple's xnu-12377.121.6; `apply.sh` makes the kernel tree | APSL 2.0 |
| `base/` | HakiOS EFI loader (`xnuloader/`), build infrastructure, device configs and docs | HakiOS's (APSL 2.0) |
| `userland/` | HakiOS user space with our changes: iOS 26 bring-up, injection, audio HAL plug-in | HakiOS's (APSL 2.0) |
| `tools/` | `check-publishable.sh` looks for anything that must not be published before a commit | MIT |

## Not included

This repository contains no binaries or firmware from Apple, Qualcomm, Xiaomi or anyone else.

## Acknowledgements

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel): the foundation of this project
- [Data A Live](https://github.com/ShueiTsuki/data-a-live): reference for GPU, audio and CPU clocks of iOS on Snapdragon
- [Mesa](https://mesa3d.org/) (Turnip / freedreno)
- [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
- Linux's msm and qcom drivers: reference for register layouts and bring-up order

The drivers are written from Linux's public descriptions. See NOTICE.md for details.

Apple, iOS and Metal are trademarks of Apple Inc. This project is not affiliated with Apple, Qualcomm or Xiaomi.
