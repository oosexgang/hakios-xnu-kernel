# Building

What can be built, from the least setup to the most.

| What | Command | Needs |
| --- | --- | --- |
| The checks | `make check` | A C compiler, Python 3.12+, git. Optional: `gcc-aarch64-linux-gnu` and `qemu-user` (driver tests as arm64 code), `shellcheck`, `clang` and `lld` (the loader). |
| The loader | `arch/arm64/boot/build.sh` | `clang`, `lld`. |
| The kernel | `kernel/build.sh` | Linux or macOS, the host packages below, the iPhoneOS SDK and the device package of the phone it is for. |
| Darwin images, and a boot | [darwin26-boot.md](darwin26-boot.md) | Apple's IPSW, the phone, and parts of the team's working tree that are not published yet. |

## The checks

```sh
make check
```

Runs the lint, validates the device packages that are present, checks that
`Documentation/boot-args.md` is current, and runs the host tests. Nothing here needs a phone, a phone's package
or Darwin: the tests run against three stand-in phones in `workbench/devices/`. CI runs the same on every push, plus
`kernel/verify.sh` and `kernel/syntax-check.sh` against Apple's release. It runs the same in the tree a release's
tarball unpacks to, which has no git: the checks go over the files the release lists (`.tracked-files`,
`tools/files.py`). Debian/Ubuntu packages for everything optional:

```sh
sudo apt-get install gcc-aarch64-linux-gnu qemu-user shellcheck clang lld zlib1g-dev libssl-dev
```

## The loader

```sh
arch/arm64/boot/build.sh             # arch/arm64/boot/loader.efi
```

It includes `platforms.h`, the boards it knows, which the script first makes into `build/boards/` from
the device packages that are present. Nothing is run by hand after changing a board or SoC
file. The loader knows the boards it was built with; with no package, only the emulator's.
The link is reproducible: the same sources and the same packages give the same file.

## The build's configuration

What a build is, in files at the repository's top, as Android's kernels keep theirs: `build.config.common`
(Apple's kernel configuration `DEVELOPMENT`, warnings not errors, no link-time optimisation, the SDK's
version), read by each target's own:

| File | Read by | What it sets |
|---|---|---|
| `build.config.arm64` | `kernel/env.sh` (`kernel/build.sh` and every kernel tool) | `ARCH_CONFIG=ARM64`, `MACHINE_CONFIG=BCM2837`, `XNU_ARCH=arm64` (no pointer authentication): the kernel is `BUILD/obj/mach.development.bcm2837` |
| `build.config.darwin27` | `darwin/userland/env.sh` (the image scripts) | `IOS` (the unpacked IPSW, `../ios27`), `WALLPAPER=yellow`, `METAL` |
| `build.config.arm64.llvm` | `kernel/env.sh` with `BUILD_CONFIG=build.config.arm64.llvm` (`make kernel-check`) | the kernel of `build.config.arm64`, its Mach-O tools LLVM's: `TOOLCHAIN=llvm`, `XNU_CLANG_DIR` the newest `/usr/lib/llvm-N/bin` with `ld64.lld` |
| `build.config.loader` | `arch/arm64/boot/build.sh` (`make loader`) | `LOADER_CC=clang`, `LOADER_TARGET`, `LOADER_LINK=lld-link`, `LOADER_BASE=0xA1000000`, `LOADER_CFLAGS` (a payload reserved elsewhere: `-DPAYLOAD_BASE=`, `-DPAYLOAD_MAX_SIZE=`) |
| `build.config.qemu` | `tools/qemu-kernel.sh`, `workbench/loader_boot_test.sh` | `QEMU`, `QEMU_CPU=cortex-a76`, `QEMU_MEMORY=6G`, `QEMU_ACCEL`, `QEMU_GIC=3`, `QEMU_FIRMWARE` (else the first of `QEMU_FIRMWARE_PLACES`) |
| `build.config.darwin27` | `darwin/userland/env.sh` (the image scripts) | `IOS` (the unpacked IPSW, `../ios27`), `WALLPAPER=yellow`, `METAL` |
| `build.config.bench` | `./bench` | `BENCH_BOARD`, `BENCH_SECS=180`, `BENCH_FOLLOW=10` |
| `build.config.release` | `scripts/release.sh` | `RELEASE_NAME=hakios-kernel`, `RELEASE_DIR=build/release`, `RELEASE_GZIP_LEVEL=9` |

A value given in the environment is kept (`KERNEL_CONFIG=RELEASE kernel/build.sh`, `WALLPAPER=blue
darwin/userland/root26-darwin.sh`, `QEMU_MEMORY=8G tools/qemu-kernel.sh ...`), and `BUILD_CONFIG=FILE`,
`DARWIN_BUILD_CONFIG=FILE`, `LOADER_BUILD_CONFIG=FILE`, `QEMU_BUILD_CONFIG=FILE` or `RELEASE_BUILD_CONFIG=FILE` names
another file of the top to read instead: a configuration of your own is a file there that reads the one it changes
(or `build.config.common`) first.

## The kernel

### macOS

Use Xcode with an installed iPhoneOS SDK and Homebrew. The setup check lists what is missing; add `--install` to
install the Homebrew packages it needs:

```sh
scripts/setup-macos.sh --check
scripts/setup-macos.sh --install       # gawk, LLVM, Python 3.12 and xz
```

Build with Xcode's clang, iPhoneOS SDK and Mach-O tools. The XNU source and its dependencies are kept beside the
checkout, not in it:

```sh
kernel/build.sh
```

On macOS the build selects `TOOLCHAIN=apple` by default. This uses Xcode's linker for the phone kernel. To compile
and link with Homebrew LLVM instead (a build check, not the image to boot on the phone):

```sh
BUILD_CONFIG=build.config.arm64.llvm kernel/build.sh
```

`DARWIN_SDK` can point to another iPhoneOS SDK, and `XNU_CLANG_DIR` can select another clang/toolchain directory.
Python 3.12 is placed first for the build because some project tools use syntax added in that version.

### Linux

Requirements:

- Packages: `base-devel git curl clang lld python perl xz
  flex bison m4 autoconf automake gawk`. GNU awk is needed even where another awk is the default (Debian's
  and Ubuntu's mawk): the syscall table's generator writes an escape that mawk keeps as it is. On
  Ubuntu: `apt-get install build-essential git curl clang lld python3 perl xz-utils flex bison m4 autoconf
  automake gawk`.
- clang 19 or newer for the kernel itself: xnu-12377's headers use `__builtin_clzg` and `__builtin_ctzg`,
  which clang 18 does not have (`kern/bits.h`: "use of undeclared identifier"). Where the system's
  `clang` is older, install a newer one beside it and point `XNU_CLANG_DIR` at it; on Ubuntu 24.04:
  `apt-get install clang-20 lld-20` and `XNU_CLANG_DIR=/usr/lib/llvm-20/bin`. The host tools keep the
  system's.
- The headers of `iPhoneOS16.5.sdk`, and Mach-O tools (a linker, `libtool`, `nm`, `strip` ...). Either
  - [theos](https://theos.dev/docs/installation-linux) with its Linux toolchain and that SDK, in `~/theos`
    or wherever `THEOS` points: cctools' `ld64`, as the team builds; or
  - LLVM's own Mach-O tools, from the same LLVM as the clang above (`ld64.lld`, `llvm-libtool-darwin`,
    `llvm-nm` ...: on Ubuntu, `lld-20` and `llvm-20`), with the SDK's directory alone: theos keeps it
    in its `sdks` repository, and `git clone --depth 1 --filter=blob:none --sparse
    https://github.com/theos/sdks && git -C sdks sparse-checkout set iPhoneOS16.5.sdk` fetches that one
    directory (233 MB). `DARWIN_SDK` says where it is. `kernel/host/llvm` makes up for what LLVM's linker
    and libtool take differently. `ld64.lld` does not implement `-image_base`, `-segment_order` or
    `-static`, with which XNU lays the kernel out: the link names the segments so that lld puts them in
    the order XNU asserts, and `kernel/host/llvm/place-kernel.py` moves the image to the kernel's
    address and gives it a kernel's load commands. The kernel this makes **boots in QEMU**
    (`tools/qemu-kernel.sh`, below); on a phone it has not been tried, and the team's kernels are linked
    by theos' `ld64`.

  `kernel/build.sh` takes theos' when it is there and LLVM's otherwise; `TOOLCHAIN=cctools` or
  `TOOLCHAIN=llvm` chooses. `make kernel-check` is the check with LLVM's tools and the compiler's
  warnings shown, whatever else is installed. A whole build with LLVM's tools and no theos, on Ubuntu 24.04 (to check a
  change compiles and links; a kernel for a phone is linked by theos' `ld64`):

  ```sh
  sudo apt-get install build-essential git curl clang lld python3 perl xz-utils flex bison m4 autoconf \
      automake gawk clang-20 lld-20 llvm-20
  XNU_CLANG_DIR=/usr/lib/llvm-20/bin DARWIN_SDK=$PWD/../sdks/iPhoneOS16.5.sdk kernel/build.sh
  ```

The kernel is built with a table of the boards whose device packages are present. The repository
describes no phone, so bring the phone's package in first:

```sh
tools/device.py available   # the packages the devices branch has
tools/device.py add renoir  # copies it into arch/arm64/boot/dts/renoir/; or: make device DEVICE=renoir
tools/device.py add --from https://git.example/you/my-phone.git   # a phone kept in a repository of its own
```

Packages elsewhere are found through the environment variable `HAKIOS_DEVICES` (directories, written
like `PATH`). A kernel built with no package knows only the emulator's board (`qemu-virt`): on a phone
it finds no row for itself and starts no driver of a phone.

```sh
kernel/build.sh             # or: kernel/build.sh -j12
```

The first run does everything, each step skipped when its result is there:

1. **The tree.** `kernel/assemble.sh` fetches Apple's pinned release and makes `../xnu-12377-hakios`
   (next to the repository) from it, `kernel/overlay/` and `kernel/patches/`.
2. **Dependencies**, in `../xnu-12377-hakios-deps`: Apple's open sources at pinned commits and three
   host tools (`mig`, `tcsh`, `unifdef`), fetched and built by `kernel/deps/stage1.sh`; the DriverKit
   headers of the macOS SDK that matches the release; an SDK made of links to theos's.
3. **Two static libraries** of the kernel (firehose, pthread), built against the tree's exported headers.
4. **The kernel**: `../xnu-12377-hakios/BUILD/obj/mach.development.bcm2837`.

Every run makes the board table anew (`tools/boards.py gen`) and puts it in the tree as
`bsd/dev/board/board_table.h`, only when it differs, so a build with the same boards recompiles
nothing for it.

Later runs bring the tree up to date with the repository, touching only changed files, and rebuild
what depends on them. Work done in the tree is never overwritten ([kernel/README.md](../kernel/README.md)).

| Variable | Default | |
| --- | --- | --- |
| `XNU_SRC` | `../xnu-12377-hakios` | The assembled tree. |
| `XNUDEPS` | `$XNU_SRC-deps` | Fetched sources, host tools, the SDK copy. |
| `THEOS` | `~/theos` | theos. |
| `TOOLCHAIN` | theos' if there, else LLVM's | `cctools` (theos) or `llvm` (`ld64.lld` and the rest beside clang in `XNU_CLANG_DIR`). |
| `DARWIN_SDK` | `$THEOS/sdks/iPhoneOS16.5.sdk` | The SDK whose headers the build uses. |
| `XNU_WARNINGS` | `0` | `1`: the compiler's warnings are shown, and at the end those in HakiOS' own files are listed (`$XNUDEPS/warnings-ours.txt`), of the files that build compiled. |
| `XNU_CLANG_DIR` | `/usr/bin` | The directory of the clang the kernel is compiled with (`kernel/host/xnu-clang`): 19 or newer. |
| `DARWIN_RENOIR` | `../darwin-renoir` | Optional. If this checkout exists and has its `xnu-deps` built, its host tools are used and step 2 fetches less. |

One kernel serves every board it was built with. It is built as `MACHINE=BCM2837`, the open board configuration the
port reuses, `DEVELOPMENT`, plain arm64.

If `kernel/build.sh` says the tree "is not a tree made by kernel/assemble.sh", it found one made by
the earlier `apply.sh`. Move it away or point `XNU_SRC` elsewhere.

## The kernel in QEMU

```sh
make qemu        # the kernel built above, booted in QEMU to its own first process, checked
```

QEMU's `virt` machine starts the loader under a real UEFI, which gives the kernel the emulator's board as
a phone's firmware does; the kernel runs a root of nothing of Apple's, whose first process is a small
shell. [qemu.md](qemu.md) says how to run it by hand, what the machine has and what is held.

## Darwin's libraries

`darwin/userland/inj26.sh` builds our libraries for iOS 26/27 into `$OBJ` with the kernel's cross compiler
(`xnu-clang`, which runs the clang the kernel was built with), then takes their PAC instructions out (`pacstrip`)
and signs them (`ldid`).

| Variable | Default | |
| --- | --- | --- |
| `DARWIN_SDK` | `$THEOS/sdks/iPhoneOS16.5.sdk` | The SDK they are built against, as for the kernel. |
| `DARWIN_LD` | `$THEOS/toolchain/linux/iphone/bin/ld` | cctools' ld64, which links them as arm64e. Without it LLVM's `ld64.lld` links them as arm64 with chained fixups, and their header is then named arm64e: dyld refuses what `ld64.lld` makes of arm64e code ([darwin26-boot.md](darwin26-boot.md)). |
| `LDID` | `ldid` on the path, else theos' | For their signatures. |

A file that `pacstrip` refuses stops the build (`pac_strip` in `darwin/userland/env.sh`).

## Wi-Fi

The radio's driver is Linux's ath11k, compiled unmodified on a layer of ours (`kernel/lx/`) into one
object that `kernel/build.sh` links into the kernel when it is there. Without it the kernel has no
radio, and `wifi=1` says so in its log.

```sh
kernel/build.sh             # once first: the tree and the kernel's headers the object is built against
kernel/lx/build.sh          # fetches Linux 7.0.14's ath11k and FreeBSD 15.1's LinuxKPI (150 and 253 MB, kept)
kernel/build.sh             # now with the Wi-Fi driver: "... bytes), with the Wi-Fi driver"
```

The object and the archives are in `$XNUDEPS/build/ath11k`, beside the kernel's tree. The phone's own
firmware for the radio (its DSP's image, its board data, the regulatory database) goes into
`firmware/NAME/fw/`, and the network to join, if any, into `firmware/NAME/fw/network` (its name, then its
passphrase): neither is ever published.

## What has been verified, and where

| | Verified |
| --- | --- |
| `make check`, `kernel/verify.sh` | On every push, in CI. |
| The kernel's drivers compile | `kernel/syntax-check.sh`, on every push: the C files of `kernel/overlay` that a stock clang can check, for syntax, types and the formats of what they print (an error in the real build). With the DriverKit headers also the C++ files. The files of `kernel/syntax-check.skip` and linking need the real build; the script prints how many of each there are. |
| The assembled tree | Byte for byte what the previous patch series produced. |
| The loader build | Compiles without a warning with the boards of the repository and the tests' stand-in phones, on every push; the parts that are plain C (the payload, the device tree, NVRAM, the memory window, a download's bounds, the system image's place, the partition a write may reach) also run on the host. Its last on-phone run was before its board table was generated and before `uefi-charger` replaced a board's name in its code. |
| The porting procedure | Its tool and data steps run on a made-up phone in `workbench/porting_test.sh`, on every push. Not on a third real phone. |
| Drivers whose constants became board data | The GPU's profile and blocks, the audio DSP's firmware and memory, the keys, the PMIC arbiter, RPMh and the Wi-Fi radio now come from a device package and `soc/`. The values are the ones the code had and the drivers compile. The keys' driver and the flashlight's also run on the host and under `qemu-aarch64` against a model of the arbiter, on two chips (`workbench/buttons_driver_test.c`, `workbench/torch_driver_test.c`), the CPU frequency driver against a model of the EPSS block (`workbench/cpufreq_driver_test.c`), and the memory vote against a model of the RPMh sequencer (`workbench/ddr_driver_test.c`). None has run on a phone since the change. |
| The full kernel build | Run to the end on Ubuntu 24.04 (2026-11-12, and again on 2026-11-16 from the layout of `arch/`, `fs/` and `components/`) with LLVM 20's tools and no theos, from a tree `kernel/assemble.sh` made: every file compiled, the ten `syntax-check.sh` cannot check among them, and the kernel linked with nothing undefined (18 MB, the 212 functions of HakiOS' drivers in it), with the two device packages. With `XNU_WARNINGS=1` the files of HakiOS give no warning. LLVM's linker does not lay the kernel out as cctools' does, so this is a check, not a kernel for a phone: with theos' cctools, as the team builds, the build was last run before this restructuring. |
| A boot | In QEMU, the kernel linked by LLVM's linker from the new layout: `make qemu` (2026-11-16) boots it to its first process, its 34 tests pass (1477 checks), and on one processor and on several. On a phone: not since the restructuring. |
| Darwin's libraries and small image | `darwin/userland/inj26.sh` (all 19, for 24B119) both with cctools' ld64 and with LLVM's alone, and `root26-darwin.sh` over 27.2's root file system and cryptexes, on 2026-11-16: arm64e, nothing left for `pacstrip` to rewrite. Not loaded on a phone since. |
| `kernel/deps/stage1.sh` | Run end to end on Ubuntu 24.04 (2026-11-12): every pinned source fetched at its commit, the macOS 13.3 SDK's checksum matched, `mig`, `migcom`, `tcsh`, `unifdef` and `unzbm` built and run. Not in CI: it fetches several hundred megabytes. |

The first boot after the restructuring is the one to watch. If a driver behaves differently on the phone, compare
`hw.board.*` and the driver's first log lines with the board file: every value it used to carry is
printed or can be read there.
