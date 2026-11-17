# HakiOS kernel

Apple's XNU kernel (`xnu-12377.121.6`, the kernel of Darwin 26 and 27) with what it takes to start Darwin on
Qualcomm phones and in QEMU: our loader, our drivers, and stand-ins for Apple's closed parts.

Nothing of Apple's is in this repository: no firmware, no IPSW, no system images, no screenshots. They are made
on your machine from files you have.

## Where things are

| Directory | What |
| --- | --- |
| `arch/arm64/` | The machine: `boot/` (the loader and the UEFI port), `boot/dts/` (the boards), `kernel/`, `include/`, `configs/` (defconfigs), `tools/` |
| `drivers/` | The drivers (`board/`) and the IOKit stand-ins (`iokit/`) |
| `include/` | The headers the drivers share with user space |
| `fs/` | Our APFS, read and written, and the file system in memory |
| `crypto/`, `security/` | The kernel's cryptography provider; the AMFI stand-in |
| `init/`, `lib/` | The first programs Darwin runs, and their small C library |
| `components/` | What makes a part of Darwin work: Metal, Apple ID, the keystore, camera, sound, brightness, wallpaper, status bar, tweaks |
| `kernel/` | Apple's release, the patches to it, and the tools that assemble, check and build the kernel |
| `darwin/` | Darwin 26/27 user space: the image and library builders |
| `tools/`, `workbench/` | The tools; the tests that need no phone |
| `scripts/` | What you run by hand: set a machine up (`setup-ubuntu.sh`), release the sources (`release.sh`) |
| `Documentation/` | Everything else ([layout.md](Documentation/layout.md) has the whole map) |

## How do I build it

1. The kernel: `kernel/build.sh` (or `make kernel-check`, with LLVM's tools). It makes the tree beside the repository
   and builds `BUILD/obj/mach.development.bcm2837` in it. `build.config.arm64` says what kind of kernel.
2. The loader: `arch/arm64/boot/build.sh`.
3. Darwin's images: `darwin/userland/root26-darwin.sh` after `darwin/userland/inj26.sh` (`build.config.darwin27`).

What each needs: [Documentation/building.md](Documentation/building.md).

## How do I run it

* In QEMU: `make qemu` (the kernel booted as a phone boots it, its shell and its tests), or
  `tools/qemu-kernel.sh [--defconfig NAME] KERNEL`.
* On a phone: `./bench enter`, `./bench ramdisk IMAGE`, then `BENCH_DEFCONFIG=NAME ./bench run`, NAME going
  `phone_first`, `phone_look`, `phone_metal`, `phone_ufs_check`, `phone_perf` on a phone's first day.
  `tools/defconfig.py --list` names the defconfigs.

A new phone: [Documentation/porting.md](Documentation/porting.md). A phone kept in a repository of its own
(its `board.dts` at the top) is built with `make phone FROM=URL`.

## How do I submit changes

1. BEST: change the file where it is kept (`drivers/board/thermal.c`, not a copy in an assembled tree), or work in
   the tree and take it back with `kernel/export.sh`.
2. LESS GOOD: a patch made elsewhere: apply it to an assembled tree, then `kernel/export.sh`
   (`kernel/import-patch.sh` does both).

Requirements:

* `make check` passes, and so do `kernel/verify.sh` and `kernel/syntax-check.sh` for a change to the kernel.
* One topic a commit. The subject starts with what it touches (`drivers/board/thermal:`, `Metal:`, `arch/arm64/boot:`),
  and the body says why.
* A driver starts only on the boards whose file lists its feature, and does nothing without its boot argument.
* Nothing of Apple's, of a phone maker's or of a vendor's is committed: `tools/check-publishable.sh` looks.

More: [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

Our work is MIT (`LICENSES/preferred/MIT`); what extends Apple's kernel and what came from HakiOS is APSL 2.0
(`LICENSES/other/APSL-2.0`). [NOTICE.md](NOTICE.md) has the list.
