# HakiOS · MediaTek Helio (Infinix Hot 20)

English · [Русский](README.ru.md)

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2 — the iOS 16.5 kernel) on a MediaTek phone**, the way Android is Linux on
phones: a boot loader of its own, drivers of its own, a configuration system and a version of its own. The example device of this
branch (`mtk`) is the **Infinix Hot 20 (X6826B) — MediaTek Helio G85 (MT6768), Mali-G52 MC2 GPU**. On top of it runs an unmodified
iOS 16.5 user space: launchd → backboardd → SpringBoard → apps.

The phone loses nothing. `mtkloader` installs into `boot_a` ahead of the Android kernel; on a command from Android it starts XNU once,
and after the session the phone returns to Android by itself. The iOS system image lives in its own `xnusys` partition and is read from
eMMC on demand. A research bench, not a ROM.

**Documentation:** [docs/](docs/README.md); porting to another MediaTek phone: [docs/mtk-porting.md](docs/mtk-porting.md).

## What works

- **Boot** — `mtkloader` on the MT6768 (MMU off, eMMC over MSDC0, a device tree built for XNU); four A55/A75 cores with frequency
  control (`mtk_cpufreq`).
- **Memory & storage** — the system root is on eMMC (`emmc_root=1`), not in RAM; about 2.5 GB is available to iOS.
- **Graphics** — an in-kernel Mali driver (`bsd/dev/redwood_mali.c`), Mesa with **panfrost**, and a **Metal** driver
  (`userland/src/redwoodmetal.m`, an AIR→NIR compiler): CoreAnimation draws through Metal on the Mali-G52. Lock screen, swipe to
  unlock, the full home screen, system gestures and Control Center.
- **Input** — touchscreen (Goodix), multitouch, wake on touch; the phone's real power and volume buttons.
- **Sensors** — the accelerometer through the SCP sensor hub (`bsd/dev/redwood_scp.c`); screen rotation in backboardd.
- **Wi-Fi** — CONNSYS brought up from the phone's own firmware; the phone joins a network from Settings and Safari loads pages over it
  (`docs/wifi-x6826b.md`).
- **Apps & shell** — Settings, Clock, Calendar, Photos, Notes and more; Safari; Sileo with the Procursus prefix (dpkg/apt) in
  `/var/jb`; SSH to the phone by key.

**Not yet, or not a goal:** cellular, Bluetooth, the phone's speakers and microphones, camera capture, vibration, and writing to the
system volume (data iOS writes to `/private/var` is instead archived to the `darwin` partition and survives a reboot). Apple's online
services — activation, Apple ID, iCloud, the App Store — are **not supported and not a goal**: this hardware has no Secure Enclave, and
nothing Apple's servers verify is forged.

## Repositories

| Repository | Contents |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | the kernel port, the common drivers, the loader, user space, the tools and this documentation; the Infinix Hot 20 lives on branch `mtk` |
| [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | the Infinix Hot 20 (MediaTek Helio G85) port, with its own loader |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | the firmware that starts the loader |
| [hackios-web](https://github.com/oosexgang/hackios-web) | the website |

## What you bring, and what is never here

Nothing of Apple's and no vendor firmware is in this repository. The Darwin user space, if you want it, you lay out yourself from an
IPSW you fetch yourself (`ios165/fetch.sh`); the MediaTek firmware — CONNSYS for Wi-Fi/Bluetooth, SCP for the sensors — comes off your
own phone's Android (`docs/wifi-x6826b.md`, `docs/sensors-x6826b.md`). Nothing is downloaded for you, decrypted or circumvented. No iOS
file, no phone firmware and no iOS screenshot is checked in.

## Building

The kernel is built from an external XNU source tree plus `xnu/redwood.patch` (`xnu-build.sh`); the user-space libraries and tools are
built with `userland/build.sh`; the boot image is packed and flashed, and a session is run, by `tools/xnu-cycle.sh`. The system
image is assembled from your own IPSW (`ios165/fetch.sh`). See [docs/mtk-porting.md](docs/mtk-porting.md).

## License and legal

The XNU patch and the files derived from it are under the Apple Public Source License 2.0; everything else is under MIT, as the file
headers say. None of this is legal advice.

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**; "Darwin", "iPhone", "XNU", "iOS" and
"Apple" are named only to describe what the code works with. If you believe a file here infringes anyone's rights, open an issue and it
will be removed.

## Team

An experimental project bringing Apple's Darwin kernel to Android hardware. The goal is to keep HakiOS free and open to everyone.
