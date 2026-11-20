# Tools

What is run from a checkout, besides the builds. Each says what it does with `--help` or in its
first lines; `make` lists the common ones.

| Tool | What it does |
| --- | --- |
| `boards.py` | The boards as data. Validates board and SoC files (`check`), says what a board has on and what it can turn on next (`status`), starts a board from a phone's Android device tree (`new`), writes the tables the code is built with (`gen`). |
| `boards.py compare BOARD --dts FILE` | A board and its SoC file against an Android device tree: each value as in the tree, different, or not in any tree. For checking a file written by hand, or one that is years old, against the phone's own tree. |
| `devicetree.py` | What `boards.py new --dts` reads from a device tree: the blocks' addresses, the panel's brightness control, the cores, the keys. A module, not a command. |
| `device.py` | Device packages: brings a phone's package into the checkout from the `devices` branch, a directory or a repository of its own (`add`, `--from URL`), lists what there is (`available`, `list`), takes one out (`remove`), takes each again from where it came (`update`), sends one made here to where packages are kept (`export`), makes a phone's own repository (`init`). |
| `logcheck.py` | What a run's log says, part by part: which came up, which stopped and the line that says why, which were not asked for; the kernel started, panicked, came back. `./bench run` ends with it; `--all` gives every line of each part, `--against OLD` what changed since another run. |
| `image.py` | One image for a phone (Documentation/images.md): builds it (`build`), puts it into a phone's repository without Apple's data (`publish`), gives a published one the identity of your own IPSW (`personalize`), says what one holds (`show`), takes one apart (`extract`). |
| `qemu-kernel.sh KERNEL` | Boots a kernel of this repository in QEMU's `virt` machine through the loader, as a phone boots it, and keeps what it says on the serial port and, with `--screen FILE`, a picture of its screen. Types on its console (`--type`), gives it a disk, sound, a forwarded port and QMP for touches; `--interactive` makes this terminal its console, `--window` shows its screen. Its head lists every option ([Documentation/qemu.md](../Documentation/qemu.md)). |
| `qemu-root.sh OUT.img` | A root for the kernel in QEMU made of nothing of Apple's: an APFS image with `/dev`, `/mnt`, `/tmp` and `/sbin/launchd`, which is `qemu-init/` built as a static program. The kernel runs it as its first process; it says what the kernel says of itself, then is a shell whose commands try the rest of the kernel. |
| `analyze-userland.sh [--why]` | clang's static analyzer on the Darwin side's libraries and programs (`userland/userland/src`, `darwin/userland`) against the SDK; what it finds is listed for a person to judge. The kernel's side is `kernel/syntax-check.sh --analyze [--why]`. |
| `bootargs.py` | Finds every boot argument the kernel reads, with the comment that describes it, and writes `Documentation/boot-args.md` (`--write`; `--check` says whether it is current). |
| `components.py` | Checks that `components` pins what the fetching scripts pin. |
| `check-links.py` | Checks that every link and every repository path in the documentation points at something. |
| `files.py [-z] [--others] [PATTERN...]` | The repository's files, as the checks go over them: `git ls-files` in a checkout; in a release's tree, which has no git, the list the release put there (`.tracked-files`), else what is on disk without build output. |
| `check-publishable.sh` | Looks for what must never be published: binaries, firmware, Apple's shader code, home directories, keys. |
| `heredoc-python.py` | Takes the Python out of the shell scripts' quoted here-documents (`python3 - <<'PY'`), parses it, and writes it out for pyflakes' checks. |
| `lint.sh` | All of the checks of form in one: syntax of every script, shellcheck, pyflakes' checks (ruff or pyflakes) of every Python script and here-document, the checks above. `make lint`. |
| `mk-focaltech-fw.py` | Makes the header one touch driver includes from a firmware file of your own phone. The firmware is not in the repository. |
| `boardfile.py` | Reads and writes board and SoC files: the syntax of a device tree source, the one a porter reads in the phone's Android tree. A module of `boards.py`, not a command. |
| `defconfig.py NAME` | The boot arguments of a defconfig of `arch/arm64/configs` (or of a file), on one line; `--list` names every defconfig with its first line. `./bench`, `tools/qemu-kernel.sh` and `tools/image.py` read them through it. |
| `kconfig.py check`, `list`, `config`, `header`, `savedefconfig` | The kernel's configuration (Kconfig): which drivers the kernel starts, of the fragments in `arch/arm64/configs/kconfig/` (or `KCONFIG_FRAGMENTS`). `check` reads every Kconfig file and fragment and the `CONFIG_` the drivers ask for. [Documentation/kconfig.md](../Documentation/kconfig.md). |
| `gen_headers.py [FRAGMENT...]`, `gen_headers.py --user` | Every header the build makes, in one place: the board tables and `kconfig.h` into `build/boards` (the kernel's build, the loader's and the tests run it); with `--user` the headers user space shares with the kernel, without the kernel's part, into `build/headers`, each compiled on its own. |
| `identity.py` | A phone's identity: the seed the loader makes its serial numbers, ECID and addresses from (`identity=` among the boot arguments), kept per phone. |
| `kernel-view.sh DIR` | The kernel's files of ours laid out at their paths in its tree, as links to the repository's: what the host tests include (`drivers/board` as `bsd/dev/board`, and so on). |
| `mk-libdeflate.sh` | Puts the decompressing half of libdeflate into the kernel tree, for the APFS driver's compressed files. |
| `qupclocks.py` | The clocks of a serial engine, for `touch { }` of a board file, out of Linux's clock driver for the SoC. |
| `recording.py` | A video of a screen recording the kernel's display driver left (`rec_secs=N`, `./bench recording`). |
| `apfs/` | APFS on this machine, from the kernel's own sources: `mkapfs.py` makes images, `apfs-tool` reads and checks them, `mksparse.py` makes the sparse images `/dev/rsp0` stands for; their tests (`run-tests.sh`). |
| `qemu/` | Helpers for a kernel in QEMU: `screendump.sh` (a picture of its screen), `touch.py` (a touch through QMP). |
| `qemu-init/` | The program that is `/sbin/launchd` of `qemu-root.sh`'s root: says what the kernel says of itself, then a shell whose commands try the rest of the kernel. |

The tools need Python 3.12 or newer and git, nothing else. They are tested in `workbench/`
(`boards_test.py`, `device_test.py`, `bootargs_test.py`, `tools_test.py`), in small repositories of
their own.

The kernel's own tools (assembling the tree, taking work back, verifying, the syntax check, the
build) are in `kernel/`; the loader's build is `arch/arm64/boot/build.sh`; what builds Darwin images and starts a
phone is in `darwin/tools/`.
