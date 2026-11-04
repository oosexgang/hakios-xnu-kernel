# HakiOS next to the other XNU-on-a-phone projects

There are other projects that put Apple's open-source XNU on hardware that is not Apple's. This page says, as plainly
as it can, where this one is different. It is written from their public pages as read on 2026-11-04 (for UniXNU also its
documentation, at its commit `c1ecc5b`); they change, so read theirs, and tell us if a line here is out of date or wrong
(an issue is enough). Where the table says something about **our** runtime state, it is what
[the documentation's front page](index.md#what-works) claims; the things this repository has *tested* are marked.

| | [ios-sweet](https://github.com/dipa-studio/ios-sweet) | [UniXNU](https://github.com/UniXNU/UniXNU) | [Bergamota](https://github.com/ry-diffusion/bergamota) | HakiOS |
|---|---|---|---|---|
| What it is | one phone, bring-up of Darwin userland 16 | a general UEFI environment for Darwin/macOS on ARM64, in the manner of OpenCore: shared build tools, OS profiles, a component catalog, SoC and device repositories | one phone, bring-up of Darwin userland 18 (a fork of ios-sweet) | a Darwin kernel port with its own loader and driver set, and a Linux-style configuration system |
| Hardware | Redmi Note 10 Pro (SM7150) | Xiaomi Pad 6S Pro (SM8550) as the reference; 234 device entries in its `devices.toml`, many of them other people's ports (how far each goes: its own pages) | Poco F1 (SDM845) | POCO X5 Pro 5G (SM7325), the Infinix Hot 20 (MT6768, [a port of its own](https://github.com/oosexgang/hakios-mt6768)), and a generator for a new device from its own device tree |
| Kernel | Apple's open xnu-8796 | by OS profile: xnu-10063.141.1 for an arm64 profile; an arm64e series for newer ones | xnu-11215 ("Heartwood") | Apple's open xnu-8796.121.2 |
| Boot | `fastboot boot` of a shim; HFS+ root in `userdata` | UEFI | its own bootloader from `boot` | `fastboot boot` (RAM only); U-Boot in `boot_a` beside Android ([installing](dual-boot.md)); or a UEFI loader that reads its payload from the volume it was started from (tested in QEMU; not yet on a phone) |
| GPU | none: drawn by the CPU, 20 to 30 frames a second by its page | Metal on Vulkan on the Adreno 740: [AdrenoMetal](https://github.com/UniXNU/AdrenoMetal), metal2vulkan, Turnip, its own kernel driver | none yet: drawn by the CPU; the Adreno 630 is in progress | Adreno 642L: OpenGL ES through Mesa by default; Metal for the window server, by a direct path and by a port of UniXNU's ([numbers](reference/metal-measurements.md): 100 to 104 frames a second at best in page swipes, not 120) |
| Licence of its code | MIT | BSD-2-Clause-Patent | none stated on the page | MIT; kernel changes APSL 2.0 |
| Says it ships Apple binaries | no | not stated | no | no, and CI fails if a binary of a kind that cannot be ours is added |

## What this project does that the others' pages do not describe

These are things that exist in this repository and that a reader can check:

- **A configuration system, like Linux's.** `configs/defconfig`, `configs/<device>_defconfig`, fragments, a `.config`,
  `savedefconfig`, and dependency and length checks ([configs.md](configs.md)). A kernel that is configured by its
  command line gets a file format for it, with the typo caught before the boot rather than after.
- **A version string that says what it is built from**, shown by the loader and by `sysctl kern.hakios.version`
  ([versioning.md](versioning.md)).
- **A catalogue of every boot-argument**, made from the source by a scanner, checked in CI, so the list cannot go
  stale ([boot-args.md](boot-args.md), 300+ arguments).
- **An open configuration that needs nothing of Apple's and nothing of the phone's**: `make qemu-open` boots the kernel
  with its own userland and prints the banner, `kern.hakios.version` and a hello. This is the part anybody can
  contribute to without touching an IPSW or a firmware file. (Tested.)
- **A provenance guard** (`tools/provenance-check.py`, in CI): it refuses a binary that is not explained, a file of
  a kind the project promises never to carry, and a writer to the phone's flash that is not declared. The answer to
  "what exactly does this write to my phone?" is a file you can read ([legal-review.md](legal-review.md),
  [LEGAL.md](../LEGAL.md)).
- **Its own loader, its own drivers**: GPU (Mesa and Metal over a Darwin driver), Wi-Fi, audio, haptics, cameras, touch, storage,
  modem bring-up. [driver-review/](driver-review/README.md) says what each is, and what has and has not been verified.

## What the others have that this one does not

- ios-sweet and Bergamota are small enough to read in an afternoon, and Bergamota targets a newer Darwin (18) with its
  own tool for the arm64e/pointer-authentication gap. HakiOS targets the Darwin userland 16.5 generation, whose userland the
  open xnu-8796 matches.
- UniXNU is a shared UEFI layer that many devices use, with a catalog of components and a page per driver. HakiOS's loader is one
  device family's, so far.
- UniXNU had Metal on Vulkan first, and it is theirs: HakiOS's second Metal path is AdrenoMetal's frontend and metal2vulkan, built by
  `tools/mk-m2v.sh` with patches for an older system. Their kernel driver for the Adreno 740 does ring scheduling and preemption by its
  documentation, which HakiOS's does not. Their Adreno 740 is another GPU and their system a newer one, so our numbers say nothing
  about how theirs runs.
- UniXNU has OS profiles for several systems, arm64e among them. HakiOS targets the Darwin 16.5 generation alone.
- Bergamota ships a macOS control app. HakiOS has `xl` and `make`, which are command lines.

## What nobody here can do

Run an Apple account, activation, iMessage or the other Apple services: no project here can, and ours does not try
(see [LEGAL.md](../LEGAL.md)). None of the four is an iPhone to Apple's servers, and none should pretend to be.

## What "different" does not mean

It does not mean better on a given day. The things in the first section are about how the project is built and checked,
not about how many apps open. If you want to see which of these the phone has actually passed, read
[the status table](index.md#what-works) and `docs/port-pkg110.md`, and run it yourself.
