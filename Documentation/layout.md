# Where everything is

The tree is laid out as Linux's (and Android's kernel) tree is. The kernel's files of ours are kept in the
directories below, and `kernel/lib.sh` (`OVERLAY_PLACES`) says which directory of the kernel's tree each is:
`kernel/assemble.sh` puts them there, `kernel/export.sh` takes them back, `tools/kernel-view.sh DIR` lays them out
at their paths in the tree for the host's tests.

| Directory | In the kernel's tree | What |
| --- | --- | --- |
| `arch/arm64/boot/` | | The loader: the UEFI application that starts the kernel, and the tools that pack its payload and the phone's boot image; `uefi/`, the changes every UEFI port needs. |
| `arch/arm64/boot/dts/` | | The boards: a `board.dts` and its `soc.dtsi` each, the emulator's and the template here, a phone's brought in from the `devices` branch (`tools/device.py add NAME`). `devices/` at the top, where they were, is still looked in. |
| `arch/arm64/kernel/` | `osfmk/arm64/` | The other processors through PSCI, the GICv3, idle times, the arm64e instructions an ARMv8.2 core lacks, the way back to the loader, the profiler. |
| `arch/arm64/include/pexpert/` | `pexpert/pexpert/arm64/` | The machine's header (`BCM2837.h`). |
| `arch/arm64/configs/` | `xnu/`: `config/` | `*_defconfig`: the boot arguments of each kind of run (`tools/defconfig.py`; `./bench` with `BENCH_DEFCONFIG`, `tools/qemu-kernel.sh --defconfig`). `xnu/`: the kernel's build configuration. |
| `arch/arm64/tools/` | | `pacstrip` (arm64e made to run without pointer authentication), `a64census`. |
| `drivers/board/`, `drivers/iokit/` | `bsd/dev/board/`, `iokit/Kernel/` | The drivers, and the stand-ins for Apple's closed kernel extensions. |
| `include/board/`, `include/external/` | `bsd/sys/board/`, `EXTERNAL_HEADERS/` | The headers user space shares; the headers of Apple's closed parts. |
| `fs/apfs/`, `fs/ramfs/` | `bsd/miscfs/apfs/`, `ramfs/` | Our APFS, read and written, and the file system in memory. `tools/apfs/` makes and checks APFS images on this machine from the same sources. |
| `crypto/` | `osfmk/minicrypto/` | The kernel's cryptography provider in place of corecrypto. |
| `security/amfi/` | `libkern/amfi/` | The stand-in for AMFI: entitlements and code-signing policy. |
| `kernel/overlay/` | the same paths | The two files that belong to none of those. |
| `init/` | | The first programs Darwin runs: `stage0`, `init` (process 1 until Apple's launchd), `var_store`. |
| `lib/libmini/` | | The small C library they are linked with. |
| `components/` | | The parts that make a subsystem of Darwin work, a directory each: `metal` (with the second device in `metal/plugin`), `apple-id`, `sep`, `camera`, `audio`, `brightness`, `wallpaper`, `statusbar`, `tweaks`; `upstream.dts`, everything taken from elsewhere. |
| `build.config.*` | | What a build is: `common`, `arm64` and `arm64.llvm` (the kernel), `loader`, `qemu` (the emulator), `darwin27` (the images), `bench` (a run on the phone), `release` (the sources published). |
| `bench` | | The phone on the bench: the resident loader, the images, a run, its log. |
| `kernel/` | | Apple's release as it is pinned (`UPSTREAM`), the topic patches to its files (`patches/`), the two files of ours that belong nowhere else (`overlay/`), and the tools that assemble, export, check and build the tree (`assemble.sh`, `export.sh`, `verify.sh`, `syntax-check.sh`, `build.sh`). [kernel/README.md](../kernel/README.md). |
| `darwin/` | | Darwin's user space: the tools that unpack an IPSW and build the images from it (`tools/`), and the inserted libraries (`userland/`). [darwin/README.md](../darwin/README.md). |
| `userland/` | | The small image's own programs and libraries and the script that builds it (`userland/build.sh`), with the phone's firmware it takes from `../fw-*`. [userland/README.md](../userland/README.md). |
| `tools/` | | What the builds and the checks run, and what a person asks a question of: boards, devices, images, logs, defconfigs, QEMU. [tools/README.md](../tools/README.md). |
| `workbench/` | | The tests that need no phone: the drivers on the host and under `qemu-aarch64` against models of their hardware, the loader in QEMU, the tools. [workbench/README.md](../workbench/README.md). |
| `Documentation/`, `LICENSES/` | | Everything else to read ([README.md](README.md) lists it); the licences ([NOTICE.md](../NOTICE.md) says which file has which). |
| `samples/` | | Starting points to copy: a phone's own repository (`device-repo/`, which `tools/device.py init` uses), a defconfig of your own. |
| `scripts/` | | What a person runs by hand: setting a machine up (`setup-ubuntu.sh`), a release of the sources (`release.sh`). `tools/` has what the builds and checks run. |

At the top, beside them: `Makefile` (`make` lists what it does), `MAINTAINERS` (each part's files, tests and documents;
`scripts/tests-for.py` reads it), `CONTRIBUTING.md`, `SECURITY.md`, `NOTICE.md`, `CHANGELOG.md`, `.clang-format` and
`.editorconfig` (the style, for an editor), `.github/` (the check every push runs, and the issue forms).

Made here and never committed (`.gitignore`): `build/` (the boards' tables, images, releases, the repositories phones
were brought in from), `logs/` (./bench's), `firmware/NAME/` (a phone's UEFI build and its own files:
[firmware/README.md](../firmware/README.md) says which), a phone's package
in `arch/arm64/boot/dts/NAME/` once brought in, and `arch/arm64/boot/identity.h` (made from your IPSW).
