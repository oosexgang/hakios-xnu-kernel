# HakiOS ARM64e ports

English | [简体中文](README.zh-CN.md)

## Current Renoir configuration

The current target is Xiaomi Renoir (Snapdragon 780G / SM7350), using iOS 27.2 (24B119) userland with the compatibility patches in `renoir/kernel/`.

- RAM-only loading through the resident USB loader, started with stock `fastboot boot`.
- CPU/software rendering (`gpu=0`); no external hardware Metal plug-in or its build dependencies.
- Static wallpaper, display-completion hooks, activation/setup hooks and the approved status-bar clock offset remain.
- Software rendering does not implement Liquid Glass and can leave UI elements incomplete.
- Verified on Renoir on 2026-11-10: the rebuilt CPU-only kernel reached the home screen in a RAM-only boot; 40 bootstrap checks, 0 failed. Long-run stability remains unverified.
- Wi-Fi, sound, camera, haptics and battery support are disabled in the current boot defaults. Thermal management and stable 60 FPS are not implemented.
- Brightness and Auto-Brightness: a device-independent kernel backlight (DSI DCS on Qualcomm panels, software dimming elsewhere, QEMU included) and the iOS slider/switch wired to it. The boot default only reads the DSI controller; `--boot-arg backlight=2` hands the panel to iOS. Host and qemu-aarch64 tests pass; not yet verified on the phone: [docs/backlight.md](docs/backlight.md).
- A device-independent thermal limiter: it reads the SoC's TSENS sensors and steps the CPU groups' frequency rows down when hot, capping the panel's brightness too. The boot default only logs temperatures; `--boot-arg thermal=2` turns it on. Host and qemu-aarch64 tests pass; not yet verified on the phone: [docs/thermal.md](docs/thermal.md).

Build and RAM-boot instructions: [docs/ios26-boot.md](docs/ios26-boot.md).

> Experimental. Requires an unlocked bootloader. The Renoir workflow must not flash, erase or write phone storage. Other device tools in this repository are not part of that RAM-only workflow.

## Layout

| Directory | Contents |
| --- | --- |
| `kernel/` | Kernel changes against Apple's xnu-12377.121.6 and `apply.sh` |
| `renoir/` | Renoir kernel adaptation, userland injections, image builders and RAM-boot tool |
| `base/` | HakiOS loader, kernel build infrastructure, device configurations and tools |
| `userland/` | HakiOS userland sources, injections and audio HAL plug-in |
| `tools/` | Publication checks and touch-firmware conversion tool |

See [NOTICE.md](NOTICE.md) and the source headers for licences. Apple system files, SDKs, device firmware and built images are local build inputs/outputs, not repository content.

## Acknowledgements

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel)
- [Data A Live](https://github.com/ShueiTsuki/data-a-live)
- [Mesa](https://mesa3d.org/)
- [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
- Apple's open-source XNU and the Linux drivers cited in individual source files

Apple, iOS and Metal are trademarks of Apple Inc. This project is not affiliated with Apple, Qualcomm or Xiaomi.
