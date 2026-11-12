# SoC files

One file per chip: where its blocks are. A SoC file is shared by every board with that chip, so it
lives here, with the code, and not in a phone's package.

| File | Chip | Used by |
| --- | --- | --- |
| `sm7350.dtsi` | Qualcomm Snapdragon 780G (SM7350, "shima") | the `renoir` package |
| `sm7325.dtsi` | Qualcomm Snapdragon 778G (SM7325, "yupik") | the `redwood` package |
| `qemu-virt.dtsi` | QEMU's `virt` machine | `devices/qemu-virt` |

## What a SoC file says

| Section | Contents | Needed for |
| --- | --- | --- |
| `platform { }` | The interrupt controller, the cores and how they are numbered, the firmware's power interface, one window over the peripherals | A board with a `loader { }` section |
| `clocks { }` | The global and display clock controllers, and the registers that switch the display block's clocks on | All of it: a panel whose brightness is MIPI DCS, `display-scan`, `gpu-a660`. The global controller's address alone (`gcc`): the part drivers that switch a bus's clocks on |
| `display { }` | The first DSI controller, and the interface the panel hangs on | The same |
| `pins { }` | Where the SoC's own pins (TLMM) are | The part drivers that drive pins: the touch controllers, the speaker amplifier, the microphones' codec, the haptic motor, the cameras, Wi-Fi |
| `spmi { }` | The PMIC arbiter's blocks | `buttons-pmic`, the part drivers that write to a PMIC, and Wi-Fi, which reads its supplies there |
| `smmu { }` | The SMMU between the SoC's devices and memory | The part drivers that give a device its addresses: the speakers' audio path, the cameras, Wi-Fi |
| `camera { }` | The camera subsystem's clock controller, one image front end with its CSI decoder, the CSI receivers | `camera-sm7325` |
| `lpass { }` | The low-power audio subsystem's blocks on the microphones' path: two codec macros, a SoundWire master, the subsystem's pins | `mic-wcd938x` |
| `ipc { }` | The always-on processor's mailbox, the block the processors ring each other through, the shared memory and its lock | `adsp`, Wi-Fi |
| `rpmh { }` | The command database and the application processor's sequencer | `ddr-rpmh`, `adsp`, `gpu-a660`, and the part drivers that ask RPMh for a supply: the touch controllers, the speaker amplifier, the cameras, Wi-Fi |
| `adreno { }` | The GPU's blocks: the 3D core, the GMU, its clock controller, its SMMU, the fuses | `gpu-a660`, with a board's `gpu { }` |
| `thermal { }` | The TSENS controllers | A board's `thermal { }` |
| `cpufreq { }` | The EPSS block and its groups of cores | `cpufreq-epss`, a board's `thermal { }` |

Every key is explained in `devices/_template/soc.dtsi`. A board needs only the sections of what it
turns on: one with `features = <>;` (none) and a software backlight needs none.

## Adding a chip

`tools/boards.py new NAME --soc SOC --dts phone.dts` writes `soc/SOC.dtsi` with what the phone's
device tree says, when the file is not there yet: every value with what the template says of its
key. Two things are never in a device tree and are filled in by hand: the display clocks' register
offsets (from Linux's clock drivers for the chip) and the window over the peripherals.
[docs/porting.md](../docs/porting.md) has both, in step 1. `camera { }` and `lpass { }` are not read from
a tree yet; the template says where in the vendor's trees their values are.

`tools/boards.py compare NAME --dts phone.dts` holds a SoC file (and a board that uses it) against a
device tree at any later time: the two that are here agree with their SoCs' trees in every value a
tree has.

A SoC file names where each value is from, in a comment: the device tree node, the Linux file. It is
sent to this branch; the phone's package goes to the `devices` branch.

`tools/boards.py check` validates every SoC file with the boards that use it, and
`tools/boards.py status NAME` says which section a feature of a board still waits for.
