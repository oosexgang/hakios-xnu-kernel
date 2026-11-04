# HakiOS

English · [中文](README.zh.md) · [Українська](README.uk.md)

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2) on Android phones**, the way Android is Linux on phones: a boot loader of its
own, drivers of its own, a configuration system and a version of its own. The same kernel runs in QEMU with nothing of Apple's and
nothing of any phone's (`make qemu-open`). A research bench, not a ROM: by default it runs from RAM, and a reboot gives the phone back to
Android.

**Documentation:** [docs/](docs/index.md), and the site, [hackios-web.vercel.app](https://hackios-web.vercel.app).

**News, 4 November 2026:** on the POCO X5 Pro 5G the window server draws through Metal on the Adreno 642L, two ways. hakios-metal: page
swipes 110 to 115 frames a second, Settings' scroll 118, the home button 106, rendering at 828x1792 stretched to the panel; 101, about 90 and about 90 at the full 1125x2436. UniXNU's Metal on Vulkan: swipes 50 to 57, scroll 41. The panel is 120 Hz,
and **120 frames a second has not been reached**. [Metal on the Adreno](docs/reference/metal.md),
[the measurements](docs/reference/metal-measurements.md).

Start with [Getting started](docs/getting-started/index.md), [On the phone](docs/getting-started/phone.md) or
[Port a device](docs/porting/index.md). What works, part by part: [the documentation's front page](docs/index.md#what-works).

## Repositories

| Repository | Contents |
|---|---|
| [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | **common**: the kernel port, every common driver, the loader, user space, the tools, this documentation, and `devices/qemu` |
| [hakios-metal](https://github.com/oosexgang/hakios-metal) | Metal's command model on the Adreno, through the kernel's GPU driver |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G: its defconfig, status, UEFI platform, firmware script, and its SoC's layer (Qualcomm SC7280 family) |
| [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | Infinix Hot 20 (MediaTek Helio G85): an experimental port with its own loader. Not in this repository's manifest |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | the firmware that starts the loader: Mu-Silicium with HakiOS's boot path, banner and name, and HakiOS's U-Boot |
| [hackios-web](https://github.com/oosexgang/hackios-web) | the website |
| [hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | old: a pointer kept for its links. User space is `userland/` here |

How they fit: [the repositories](docs/porting/repositories.md).

## What you bring, and what is never here

Nothing of Apple's and no vendor's firmware is in these repositories. The Darwin user space, if you want it, you lay out yourself from an
IPSW you get yourself, and the firmware comes off your own phone; nothing is downloaded for you, decrypted or circumvented. Apple's
services (activation, Apple ID, iCloud, the App Store) are not supported and not a goal.
[What is in the repository, and what never is](docs/index.md#what-is-in-the-repository-and-what-never-is).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and [COMMUNITY.md](COMMUNITY.md).

## License and legal

The XNU patch and the files derived from it are under the Apple Public Source License 2.0 ([LICENSE.APSL](LICENSE.APSL)), the rest under
MIT ([LICENSE](LICENSE)), as the file headers say. Third parties: [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), [COMPLIANCE.md](COMPLIANCE.md). What is and is not distributed: [LEGAL.md](LEGAL.md).
None of this is legal advice.

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**; "Darwin", "iPhone", "XNU" and "Apple" are
named only to describe what the code works with. If you believe a file here infringes anyone's rights, open an issue and it will be
removed.

## Team

hakios is an experimental project focused on bringing darwin to android hardware. our goal is to make hakios free and accessible to everyone.

@oodze - founder, @netusd - advertising / ceo-dev, @wannqn - dev, @hxfuxyyXD - dev, Rhaenja Hun（杭润嘉） - developer / sponsor

tym sicilium for the sources!
