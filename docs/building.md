# Building

What can be built, from the least setup to the most.

| What | Command | Needs |
| --- | --- | --- |
| The checks | `make check` | A C compiler, Python 3.11+, git. Optional: `gcc-aarch64-linux-gnu` and `qemu-user` (driver tests as arm64 code), `shellcheck`, `clang` and `lld` (the loader). |
| The loader | `loader/build.sh` | `clang`, `lld`. |
| The kernel | `kernel/build.sh` | Linux, theos, the system packages below, and the device package of the phone it is for. |
| iOS images, and a boot | [ios26-boot.md](ios26-boot.md) | Apple's IPSW, the phone, and parts of the team's working tree that are not published yet. |

## The checks

```sh
make check
```

Runs the lint, validates the device packages that are present and `soc/`, checks that
`docs/boot-args.md` is current, and runs the host tests. Nothing here needs a phone, a phone's package
or iOS: the tests run against three stand-in phones in `tests/devices/`. CI runs the same on every push, plus
`kernel/verify.sh` and `kernel/syntax-check.sh` against Apple's release. Debian/Ubuntu packages for everything optional:

```sh
sudo apt-get install gcc-aarch64-linux-gnu qemu-user shellcheck clang lld zlib1g-dev libssl-dev
```

## The loader

```sh
loader/build.sh             # loader/loader.efi
```

It includes `platforms.h`, the boards it knows, which the script first makes into `build/boards/` from
the device packages that are present and `soc/`. Nothing is run by hand after changing a board or SoC
file. The loader knows the boards it was built with; with no package, only the emulator's.
The link is reproducible: the same sources and the same packages give the same file.

## The kernel

Requirements:

- Linux (the team builds on Arch under WSL). Packages: `base-devel git curl clang lld python perl xz
  flex bison m4 autoconf automake gawk`. GNU awk is needed even where another awk is the default (Debian's
  and Ubuntu's mawk): the syscall table's generator writes an escape that mawk keeps as it is. On
  Ubuntu: `apt-get install build-essential git curl clang lld python3 perl xz-utils flex bison m4 autoconf
  automake gawk`.
- clang 19 or newer for the kernel itself: xnu-12377's headers use `__builtin_clzg` and `__builtin_ctzg`,
  which clang 18 does not have (`kern/bits.h`: "use of undeclared identifier"). Where the system's
  `clang` is older, install a newer one beside it and point `XNU_CLANG_DIR` at it; on Ubuntu 24.04:
  `apt-get install clang-20 lld-20` and `XNU_CLANG_DIR=/usr/lib/llvm-20/bin`. The host tools keep the
  system's.
- [theos](https://theos.dev/docs/installation-linux) with its Linux toolchain and `iPhoneOS16.5.sdk`,
  in `~/theos` or wherever `THEOS` points. It provides the Mach-O linker and the SDK headers.

The kernel is built with a table of the boards whose device packages are present. The repository
describes no phone, so bring the phone's package in first:

```sh
tools/device.py available   # the packages the devices branch has
tools/device.py add renoir  # copies it into devices/renoir/; or: make device DEVICE=renoir
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
`bsd/dev/hakios_board_table.h`, only when it differs, so a build with the same boards recompiles
nothing for it.

Later runs bring the tree up to date with the repository, touching only changed files, and rebuild
what depends on them. Work done in the tree is never overwritten ([kernel/README.md](../kernel/README.md)).

| Variable | Default | |
| --- | --- | --- |
| `XNU_SRC` | `../xnu-12377-hakios` | The assembled tree. |
| `XNUDEPS` | `$XNU_SRC-deps` | Fetched sources, host tools, the SDK copy. |
| `THEOS` | `~/theos` | theos. |
| `XNU_CLANG_DIR` | `/usr/bin` | The directory of the clang the kernel is compiled with (`kernel/host/xnu-clang`): 19 or newer. |
| `DARWIN_RENOIR` | `../darwin-renoir` | Optional. If this checkout exists and has its `xnu-deps` built, its host tools are used and step 2 fetches less. |

One kernel serves every board it was built with. It is built as `MACHINE=BCM2837`, the open board configuration the
port reuses, `DEVELOPMENT`, plain arm64.

If `kernel/build.sh` says the tree "is not a tree made by kernel/assemble.sh", it found one made by
the earlier `apply.sh`. Move it away or point `XNU_SRC` elsewhere.

## What has been verified, and where

| | Verified |
| --- | --- |
| `make check`, `kernel/verify.sh` | On every push, in CI. |
| The kernel's drivers compile | `kernel/syntax-check.sh`, on every push: the C files of `kernel/overlay` that a stock clang can check, for syntax, types and the formats of what they print (an error in the real build). With the DriverKit headers also the C++ files. The files of `kernel/syntax-check.skip` and linking need the real build; the script prints how many of each there are. |
| The assembled tree | Byte for byte what the previous patch series produced. |
| The loader build | Compiles without a warning with the boards of the repository and the tests' stand-in phones, on every push; the parts that are plain C (the payload, the device tree, NVRAM, the memory window, a download's bounds, the system image's place, the partition a write may reach) also run on the host. Its last on-phone run was before its board table was generated and before `uefi_charger` replaced a board's name in its code. |
| The porting procedure | Its tool and data steps run on a made-up phone in `tests/porting_test.sh`, on every push. Not on a third real phone. |
| Drivers whose constants became board data | The GPU's profile and blocks, the audio DSP's firmware and memory, the keys, the PMIC arbiter, RPMh and the Wi-Fi radio now come from a device package and `soc/`. The values are the ones the code had and the drivers compile. The keys' driver and the flashlight's also run on the host and under `qemu-aarch64` against a model of the arbiter, on two chips (`tests/buttons_driver_test.c`, `tests/torch_driver_test.c`), the CPU frequency driver against a model of the EPSS block (`tests/cpufreq_driver_test.c`), and the memory vote against a model of the RPMh sequencer (`tests/ddr_driver_test.c`). None has run on a phone since the change. |
| The full kernel build, and a boot | By the team, on their machines, before this restructuring. Not repeated since: it needs the Darwin toolchain and a phone. |
| `kernel/deps/stage1.sh` | Run end to end on Ubuntu 24.04 (2026-11-12): every pinned source fetched at its commit, the macOS 13.3 SDK's checksum matched, `mig`, `migcom`, `tcsh`, `unifdef` and `unzbm` built and run. Not in CI: it fetches several hundred megabytes. |

The first full build after the restructuring is the one to watch. If it fails to build, look first at
`kernel/build.sh` steps 1 and 2, the parts of the build whose behaviour changed, and at the ten files
`kernel/syntax-check.sh` cannot check. If a driver behaves differently on the phone, compare
`hw.board.*` and the driver's first log lines with the board file: every value it used to carry is
printed or can be read there.
