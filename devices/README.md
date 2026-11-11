# Devices

One directory per board, one file per SoC. This is the only place a device is described: the kernel is
built with a table generated from these files and picks its row at boot.

| Path | What it says |
| --- | --- |
| `devices/NAME/board.toml` | What the device has: its SoC, the drivers it may start (`features`), its panel's brightness control (`[backlight]`), thermal floors (`[thermal]`), its keys, audio DSP and GPU (`[buttons]`, `[adsp]`, `[gpu]`), how the loader recognises it (`[loader]`), its default boot arguments (`[boot]`). |
| `soc/SOC.toml` | Where the blocks of the chip are, once for every board with that chip: the interrupt controller and cores (`[platform]`), clock controllers (`[clocks]`), the DSI controller (`[display]`), the PMIC arbiter (`[spmi]`), RPMh (`[rpmh]`), temperature sensors (`[thermal]`), the CPU frequency block and its groups of cores (`[cpufreq]`). |
| `devices/_template/board.toml` | Every key, explained. |

```sh
tools/boards.py list                    # the boards
tools/boards.py show renoir             # one board with its SoC's values
tools/boards.py check                   # validate everything; says which file and why
tools/boards.py gen                     # rewrite the kernel's table after a change
tools/boards.py new NAME --soc SOC --dts device-tree.dts    # start a new board from an Android device tree
```

| Board | Device | SoC | State |
| --- | --- | --- | --- |
| `renoir` | Xiaomi Mi 11 Lite 5G | SM7350 | iOS 27.2 boots to the home screen from RAM (see the top-level README) |
| `redwood` | POCO X5 Pro 5G | SM7325 | The first port (iOS 16.5, `base/`); its drivers are in this kernel, not yet booted on it with iOS 26/27 |
| `qemu-virt` | QEMU `virt` | — | No phone needed |

## How the kernel uses it

`tools/boards.py gen` writes two files, both committed:

- `kernel/overlay/bsd/dev/hakios_board_table.h`, the kernel's table. At boot the kernel looks up the
  name the loader passed in `/chosen/darwin-board`. Drivers ask `hakios_board()` for addresses and
  `hakios_board_has(HAKIOS_HAS_...)` before they start (`hakios_board.h` lists the features). A board
  that is not in the table gets no feature: nothing of a phone is touched on a machine nobody described.
- `loader/platforms.h`, the loader's table, for the boards with a `[loader]` section. The loader tells
  them apart by the framebuffer address their UEFI reports.

`make check` fails while either file is not what the data says.

`sysctl hw.board` shows what the running kernel found: `name`, `soc`, `description`, `features`.

## Adding a board

The whole procedure, with what to take from the device tree and in which order to try the drivers, is
[docs/porting.md](../docs/porting.md). In short:

1. `tools/boards.py new NAME --soc SOC --dts your.dts` and fill in what it could not read.
2. `tools/boards.py check && tools/boards.py gen`.
3. Make the loader pass `darwin-board = "NAME"`.
4. Boot with features off, then enable them one at a time.

Drivers that are specific to one device's parts (a touch controller, a speaker amplifier) are code,
not data: they live in `kernel/overlay/bsd/dev/` and are started for the boards that have the part.
