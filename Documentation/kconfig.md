# The kernel's configuration (Kconfig)

Which of this repository's drivers a kernel starts is chosen as Linux chooses its own: each driver is described in a
Kconfig file, a configuration is made of fragments, and the code is built with a header of the result. The language
is Linux's (the part of it this tree uses), the tool is `tools/kconfig.py`, the header is made by
`tools/gen_headers.py`.

A choice decides which drivers are **started** (`drivers/board/drivers.c`). Every driver is compiled whatever is
chosen, so a configuration never breaks the build; what is off is never started, and the kernel says at boot what it
was made without:

```
kconfig: made of bringup.config, without TOUCH_GOODIX TOUCH_FTS TOUCH_FT5X06 ADRENO_A6XX WIFI_WCN6750 ...
```

Whether a driver that is started then does anything is still the board's and the boot arguments' to say, as before:
its feature in `board.dts`, its addresses in the SoC file, its boot argument ([drivers.md](drivers.md)).

## The files

| File | What it describes |
| --- | --- |
| `Kconfig` | The top: it reads the others. |
| `arch/arm64/Kconfig` | The platform: PSCI, the GIC, the way back to the loader. Not choices: described so that the rest can depend on them. |
| `drivers/board/Kconfig` | The phone's drivers, in three menus: **common to every Snapdragon**, **parts of particular chips and phones**, **the emulator**, and the shared helpers (TLMM, the clock controller, RPMh and the command database, SPMI, GENI, SMEM, GLINK, QRTR, TrustZone's calls, the SMMU), which nobody chooses and are on when a driver that uses them is. |
| `drivers/iokit/Kconfig` | The stand-ins for Apple's closed kernel extensions: not choices, each following the driver it stands beside (the GPU service follows `ADRENO_A6XX`). |
| `fs/Kconfig`, `security/Kconfig` | APFS and the RAM file system, the code-signing and MAC policies: not choices. |
| `arch/arm64/configs/kconfig/*.config` | The fragments. |

The common drivers work on every Snapdragon since the SDM845: what they need of a chip is its SoC file's addresses,
and the generations of a block they tell apart by the SoC file or the block itself ([chipsets.md](chipsets.md)). The
others are drivers of a part a chip or a phone may not have: a
GPU generation, a radio, a codec, a camera block.

## The fragments

| Fragment | What it is for |
| --- | --- |
| `hakios` | Every driver: the kernel as it has always been built, and the default (`KCONFIG_FRAGMENTS` in `build.config.arm64`). |
| `phone` | Every driver of a phone's, none of the emulator's. |
| `qemu` | The emulator's drivers only: a kernel for `tools/qemu-kernel.sh`. |
| `bringup` | Only the drivers common to every Snapdragon: the first kernel of a chip or a phone this tree has not run on ([porting.md](porting.md)). |

A fragment is Linux's: `CONFIG_NAME=y`, `CONFIG_NAME=n` or `# CONFIG_NAME is not set`, one a line, comments with `#`.
Several are read in order, the later winning; what one asks is taken where the dependencies allow it, and said where
they do not (a symbol that is selected is on whatever its own dependencies say, as in Linux, and that is said too).

## Making one

```sh
tools/kconfig.py list bringup                    # every symbol, on or off, with its prompt
KCONFIG_FRAGMENTS=bringup kernel/build.sh        # a kernel of it (kconfig.h is made and put in the tree)
KCONFIG_FRAGMENTS="phone my.config" kernel/build.sh
tools/kconfig.py savedefconfig phone my.config   # what differs from the defaults, as one fragment
make kconfig FRAGMENTS=qemu                      # check every file, then list
```

`tools/kconfig.py config` writes the whole configuration as a `.config` (`build/kconfig/.config`), `header` the
`kconfig.h` alone; `tools/gen_headers.py` makes it with the board tables, which is what the builds and the tests run.

## Adding a driver

1. A `config` entry in `drivers/board/Kconfig`, in the menu it belongs to: `bool "what it is"`, `default y`, the
   helpers it uses as `select`, what it needs as `depends on`, and a `help` that says what it does and which file it
   is (`tools/kconfig.py check` refuses a choice without one).
2. Its start in `drivers/board/drivers.c` under `if (CONFIG_NAME) { ... }`: C's own `if`, so that the code is compiled
   either way. `tools/kconfig.py check` refuses a `CONFIG_` asked there that no Kconfig describes.
3. A fragment that leaves it out where it cannot be had (`qemu` for a phone's part, `bringup` for a particular part).

`make check` runs `tools/kconfig.py check` and `workbench/kconfig_test.py` (the language, the fragments, the header),
and `workbench/drivers_kconfig_test.c` builds `drivers.c` with drivers turned off.

## The headers user space shares

`tools/gen_headers.py --user` writes the headers of `include/board/` (`sys/board/` in the kernel's tree) as user space
is to see them into `build/headers/sys/board/`: what is the kernel's alone (`#ifdef KERNEL ... #endif`, the `#else`
of an `#ifndef KERNEL`) is left out, and each one is compiled on its own to be sure it stands without the kernel. A
program of user space takes them with `-I build/headers`. `make headers` makes both kinds.
