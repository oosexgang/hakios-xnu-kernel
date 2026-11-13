# Tools

What is run from a checkout, besides the builds. Each says what it does with `--help` or in its
first lines; `make` lists the common ones.

| Tool | What it does |
| --- | --- |
| `boards.py` | The boards as data. Validates board and SoC files (`check`), says what a board has on and what it can turn on next (`status`), starts a board from a phone's Android device tree (`new`), writes the tables the code is built with (`gen`). |
| `boards.py compare BOARD --dts FILE` | A board and its SoC file against an Android device tree: each value as in the tree, different, or not in any tree. For checking a file written by hand, or one that is years old, against the phone's own tree. |
| `devicetree.py` | What `boards.py new --dts` reads from a device tree: the blocks' addresses, the panel's brightness control, the cores, the keys. A module, not a command. |
| `device.py` | Device packages: brings a phone's package into the checkout from the `devices` branch or a directory (`add`), lists what there is (`available`, `list`), takes one out (`remove`), sends one made here to where packages are kept (`export`). |
| `qemu-kernel.sh KERNEL` | Boots a kernel of this repository in QEMU's `virt` machine through the loader, as a phone boots it, and keeps what it says on the serial port and, with `--screen FILE`, a picture of its screen. Stops when the kernel waits for its root or panics. |
| `bootargs.py` | Finds every boot argument the kernel reads, with the comment that describes it, and writes `docs/boot-args.md` (`--write`; `--check` says whether it is current). |
| `components.py` | Checks that `components` pins what the fetching scripts pin. |
| `check-links.py` | Checks that every link and every repository path in the documentation points at something. |
| `check-publishable.sh` | Looks for what must never be published: binaries, firmware, Apple's shader code, home directories, keys. |
| `lint.sh` | All of the checks of form in one: syntax of every script, shellcheck, the three checks above. `make lint`. |
| `mk-focaltech-fw.py` | Makes the header one touch driver includes from a firmware file of your own phone. The firmware is not in the repository. |

The tools need Python 3.11 or newer and git, nothing else. They are tested in `workbench/`
(`boards_test.py`, `device_test.py`, `bootargs_test.py`, `tools_test.py`), in small repositories of
their own.

The kernel's own tools (assembling the tree, taking work back, verifying, the syntax check, the
build) are in `kernel/`; the loader's build is `loader/build.sh`; what builds Darwin images and starts a
phone is in `darwin/tools/`.
