# SoC files

One file per chip: where its blocks are. A SoC file is shared by every board with that chip, so it
lives here, with the code, and not in a phone's package.

| File | Chip | Used by |
| --- | --- | --- |
| `sm7350.toml` | Qualcomm Snapdragon 780G (SM7350, "shima") | the `renoir` package |
| `sm7325.toml` | Qualcomm Snapdragon 778G (SM7325, "yupik") | the `redwood` package |
| `qemu-virt.toml` | QEMU's `virt` machine | `devices/qemu-virt` |

## What a SoC file says

| Section | Contents | Needed for |
| --- | --- | --- |
| `[platform]` | The interrupt controller, the cores and how they are numbered, the firmware's power interface, one window over the peripherals | A board with a `[loader]` section |
| `[clocks]` | The global and display clock controllers, and the registers that switch the display block's clocks on | A panel whose brightness is MIPI DCS; `display-scan` |
| `[display]` | The first DSI controller, and the interface the panel hangs on | The same |
| `[spmi]` | The PMIC arbiter's blocks | `buttons-pmic`, and the part drivers that write to a PMIC |
| `[rpmh]` | The command database and the application processor's sequencer | `ddr-rpmh` |
| `[thermal]` | The TSENS controllers | A board's `[thermal]` |
| `[cpufreq]` | The EPSS block and its groups of cores | `cpufreq-epss`, a board's `[thermal]` |

Every key is explained in `devices/_template/soc.toml`. A board needs only the sections of what it
turns on: one with `features = []` and a software backlight needs none.

## Adding a chip

`tools/boards.py new NAME --soc SOC --dts phone.dts` writes `soc/SOC.toml` with what the phone's
device tree says, when the file is not there yet. Two things are never in a device tree and are filled
in by hand: the display clocks' register offsets (from Linux's clock drivers for the chip) and the
window over the peripherals. [docs/porting.md](../docs/porting.md) has both, in step 1.

A SoC file names where each value is from, in a comment: the device tree node, the Linux file. It is
sent to this branch; the phone's package goes to the `devices` branch.

`tools/boards.py check` validates every SoC file with the boards that use it, and
`tools/boards.py status NAME` says which section a feature of a board still waits for.
