# Devices

One directory per board, one file per SoC. This is the only place a device is described: the kernel is
built with a table generated from these files and picks its row at boot.

| Path | What it says |
| --- | --- |
| `devices/NAME/board.toml` | What the device has: its SoC, the drivers it may start (`features`), its panel's brightness control (`[backlight]`), thermal floors (`[thermal]`), its keys, audio DSP and GPU (`[buttons]`, `[adsp]`, `[gpu]`), what iOS's interface is told of its screen (`[screen]`), how the loader recognises it (`[loader]`), its default boot arguments (`[boot]`). |
| `soc/SOC.toml` | Where the blocks of the chip are, once for every board with that chip: the interrupt controller and cores (`[platform]`), clock controllers (`[clocks]`), the DSI controller (`[display]`), the PMIC arbiter (`[spmi]`), RPMh (`[rpmh]`), temperature sensors (`[thermal]`), the CPU frequency block and its groups of cores (`[cpufreq]`). |
| `devices/_template/board.toml`, `soc.toml` | Every key of a board file and of a SoC file, explained. |

```sh
tools/boards.py list                    # the boards
tools/boards.py show renoir             # one board with its SoC's values
tools/boards.py status renoir           # what it has on, what it could turn on, what the rest waits for
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

`tools/boards.py gen` writes three files, all committed:

- `kernel/overlay/bsd/dev/hakios_board_table.h`, the kernel's table. At boot the kernel looks up the
  name the loader passed in `/chosen/darwin-board`. Drivers ask `hakios_board()` for addresses and
  `hakios_board_has(HAKIOS_HAS_...)` before they start (`hakios_board.h` lists the features). A board
  that is not in the table gets no feature: nothing of a phone is touched on a machine nobody described.
- `loader/platforms.h`, the loader's table, for the boards with a `[loader]` section. The loader tells
  them apart by the framebuffer address their UEFI reports.
- `ios/userland/hakios_boards.h`, what the libraries injected into iOS's processes know of a board
  (`[screen]`). They read the board's name from `/chosen/darwin-board` in the registry.

`make check` fails while any of them is not what the data says.

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

## Adding a kind of data

When a driver needs something of a device that no section has yet (an address, a pin, a firmware
directory), it becomes a key, not a constant in the driver. The `[spmi]` and `[buttons]` sections are a
small example to copy; each step is one place:

1. **The kernel's struct.** A field in `struct hakios_board` (`kernel/overlay/bsd/dev/hakios_board.h`),
   with a comment that says which file and section it comes from.
2. **The schema.** The key and its type in `SOC_SCHEMA` or `BOARD_SCHEMA` of `tools/boards.py`: what
   every board with the chip shares goes in the SoC file, what one phone has goes in the board file.
   If a feature cannot work without the section, say so in `FEATURE_NEEDS` or
   `FEATURE_NEEDS_OF_BOARD`; values that can be wrong in a way worth catching get a check in `load()`.
3. **The table.** A line in `table_text()` that writes the field.
4. **The files.** The values in the boards that have them, each with a comment naming where in the
   device tree or in Linux it is from, and the key explained in `devices/_template/`.
5. **The device tree**, if the value is in one: `from_dts()` in `tools/devicetree.py`, so that
   `tools/boards.py new --dts` fills it in for the next phone.
6. **The driver** reads `hakios_board()->…` and says so in its log when the data is missing, rather
   than falling back to one phone's value.
7. **Tests.** `tests/boards_test.py` for what the tool takes and refuses, `tests/board_test.c` for what
   the kernel sees. `make check` also fails while the template does not explain a key the schema has.

Then `tools/boards.py gen` and `make check`.
