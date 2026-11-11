# Building

What can be built, from the least setup to the most.

| What | Command | Needs |
| --- | --- | --- |
| The checks | `make check` | A C compiler, Python 3.11+, git. Optional: `gcc-aarch64-linux-gnu` and `qemu-user` (driver tests as arm64 code), `shellcheck`, `clang` and `lld` (the loader). |
| The loader | `loader/build.sh` | `clang`, `lld`. |
| The kernel | `kernel/build.sh` | Linux, theos, and the packages below. |
| iOS images, and a boot | [ios26-boot.md](ios26-boot.md) | Apple's IPSW, the phone, and parts of the team's working tree that are not published yet. |

## The checks

```sh
make check
```

Runs the lint, validates `devices/` and `soc/`, checks that the generated files are current, and runs
the host tests. Nothing here needs a phone or iOS. CI runs the same on every push, plus
`kernel/verify.sh` and `kernel/syntax-check.sh` against Apple's release. Debian/Ubuntu packages for everything optional:

```sh
sudo apt-get install gcc-aarch64-linux-gnu qemu-user shellcheck clang lld
```

## The loader

```sh
loader/build.sh             # loader/loader.efi
```

It includes `loader/platforms.h`. After changing a board or SoC file, run `tools/boards.py gen` first.
The link is reproducible: the same sources give the same file.

## The kernel

Requirements:

- Linux (the team builds on Arch under WSL). Packages: `base-devel git curl clang lld python perl xz
  flex bison m4 autoconf automake`.
- [theos](https://theos.dev/docs/installation-linux) with its Linux toolchain and `iPhoneOS16.5.sdk`,
  in `~/theos` or wherever `THEOS` points. It provides the Mach-O linker and the SDK headers.

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

Later runs bring the tree up to date with the repository, touching only changed files, and rebuild
what depends on them. Work done in the tree is never overwritten ([kernel/README.md](../kernel/README.md)).

| Variable | Default | |
| --- | --- | --- |
| `XNU_SRC` | `../xnu-12377-hakios` | The assembled tree. |
| `XNUDEPS` | `$XNU_SRC-deps` | Fetched sources, host tools, the SDK copy. |
| `THEOS` | `~/theos` | theos. |
| `DARWIN_RENOIR` | `../darwin-renoir` | Optional. If this checkout exists and has its `xnu-deps` built, its host tools are used and step 2 fetches less. |

One kernel serves every board. It is built as `MACHINE=BCM2837`, the open board configuration the
port reuses, `DEVELOPMENT`, plain arm64.

If `kernel/build.sh` says the tree "is not a tree made by kernel/assemble.sh", it found one made by
the earlier `apply.sh`. Move it away or point `XNU_SRC` elsewhere.

## What has been verified, and where

| | Verified |
| --- | --- |
| `make check`, `kernel/verify.sh` | On every push, in CI. |
| The kernel's drivers compile | `kernel/syntax-check.sh`, on every push: the 53 C files of `kernel/overlay` that a stock clang can check, for syntax and types. With the DriverKit headers also the 9 C++ files. The 10 of `kernel/syntax-check.skip` and linking need the real build. |
| The assembled tree | Byte for byte what the previous patch series produced. |
| The loader build | Compiles without a warning; identical to the build before its board table was generated. |
| The porting procedure | Its tool and data steps run on a made-up phone in `tests/porting_test.sh`, on every push. Not on a third real phone. |
| The full kernel build, and a boot | By the team, on their machines, before this restructuring. Not repeated since: it needs the Darwin toolchain and a phone. |
| `kernel/deps/stage1.sh` | Not run end to end. It runs the commands of the setup script the team builds with. |

The first full build after the restructuring is the one to watch. If it fails, the cause is in
`kernel/build.sh` steps 1 and 2, the only parts whose behaviour changed.
