# Chipsets: the drivers common to every Snapdragon

The drivers of the menu **Common to every Snapdragon** (`drivers/board/Kconfig`, [kconfig.md](kconfig.md)) are
written once for every Qualcomm chip since the SDM845: nothing in them is one chip's. What a chip gives them is its
SoC file (`soc.dtsi` of a device package: addresses, read from the phone's device tree by
`tools/boards.py new --dts`), and what differs between the generations of a block is told apart by the SoC file or
by the block itself (its version register), never by the chip's name.

Where a block is of a kind a driver does not know, the driver says what it found and leaves the block alone: a
phone of a chip nobody has run is never written to blindly. The fragment `bringup` builds a kernel of these drivers
alone, the first kernel of a new chip ([porting.md](porting.md)).

## What each one needs, and which kinds of the block it knows

| Driver | What the SoC file gives it | The kinds it knows | What it does with another |
| --- | --- | --- | --- |
| The storage (`board/ufs.c`, `board/ufshc.c`) | `platform { ufs }`: the UFS host controller (`ufs_mem`, Linux's first reg of `qcom,ufshc`) | Through the loader: any UEFI that drives the storage. The kernel's own (`ufs_native=1`): a UFS 2.x or 3.x host as JEDEC lays it out | Its version is said; eMMC (some 4- and 6-series phones) has no driver |
| The keys (`board/buttons.c`) | `spmi { }`, and the board's `buttons { }` (the PON block and the GPIO of volume up) | The PMIC arbiter of version 5, and of version 8 with `channel-map`; the power-on block of every generation Linux's `pm8941-pwrkey.c` reads -- 1 and 2 (the PM8998's, the PM8150's: the keys in bits 0 and 1), 3's half for the operating system (the PMK8350's and after: bits 7 and 6) -- and the PMIC's GPIOs | Arbiter versions 2, 3 and 7: said, no keys. A power-on block without keys (a secondary one): said, left alone |
| The battery (`board/battery.c`) | Nothing: what the loader reads from UEFI | Any UEFI with Qualcomm's charger protocols | Nothing to read: said |
| Temperatures (`board/thermal.c`) | `thermal { tsens }`: each TSENS controller's readings (`tsens_tm_physical`, Linux's first reg of `qcom,tsens-v2`) | TSENS version 2: every chip since the MSM8996 | — |
| The cores' speed (`board/cpufreq.c`) | `cpufreq { }`: the frequency block, its layout and each group's cores | EPSS (SM8350, SM7325, SC7280 and every chip after them) and the OSM's firmware before it (SDM845, SM8150, SM8250, SC7180, SM6350, SM7225): `board/cpufreq_hw.h` | A group whose hardware is not in charge of its clock: said, left alone |
| The cores' speed by the firmware (`board/scmi_perf.c`) | `scmi-perf { }`: the shared memory, the mailbox, the doorbell | SCMI's performance protocol, on the chips whose firmware sets the cores' speed itself | — |
| The memory's vote (`board/ddr.c`) | `rpmh { }`: the command database and the application processor's RSC | RPMh's bus votes, by the names the command database gives (`MC0`, `SH0`), through the sequencer's first slot set: every chip with RPMh, whose tree lists its active slots first (`qcom,tcs-config`) | A name the database has not: said, no vote. A tree whose first slots are not active ones: said by `tools/boards.py new --dts` |
| The panel's light (`board/backlight.c`) | `display { dsi0 }`, `clocks { }`, and the board's `backlight { }` | DCS through MDSS DSI 2.2 and later (any panel that takes 0x51), or in software | A DSI controller off: the light in software |
| The flashlight (`board/torch.c`) | `spmi { }` and the board's `torch { }` | The flash LED module of a PM8350C | Said, left alone |

## Telling the kinds apart

**The cores' frequency block.** The device tree says which it is. Linux's trees name it `qcom,cpufreq-epss` or
`qcom,cpufreq-hw` (often with the chip's name before: `qcom,sm8450-cpufreq-epss`), the vendor's
`qcom,cpufreq-hw-epss` or `qcom,cpufreq-hw`. `tools/boards.py new --dts` reads it and writes `block = "osm";` for a
`cpufreq-hw` node, nothing for EPSS (the default). Its groups' blocks are its `freq-domainN` regs: one size apart on
most chips (`epss` and `epss-domain-size`), each given (`domains`) where they are not, as on an SM8150. The two
layouts (Linux's `qcom-cpufreq-hw.c`):

| | EPSS | OSM |
| --- | --- | --- |
| The table's first row | 0x100 | 0x110 |
| From one row to the next | 4 bytes | 32 bytes |
| The row wanted | 0x320 | 0x920 |
| What LMh allows now (× 19.2 MHz) | 0x20, [7:0] | 0x704, [9:0] |
| The hardware in charge (enable) | 0x0, [0] | 0x0, [0] |

A chip whose firmware sets the cores' speed itself has no such block: its SoC file has `scmi-perf { }` instead, the
board the feature `cpufreq-scmi`, and the temperature driver holds the firmware's levels back as it holds rows back.

**The PMIC arbiter.** Its version is its first register, and the drivers read it before anything else. Version 5
keeps its table of channels in the core block; version 8 keeps it apart, which the SoC file says with
`spmi { channel-map }` (the core node's second block in Linux's tree). `tools/boards.py new --dts` reads the blocks
by their reg-names (`core`, `chnls`, `obsrvr`, `cnfg`), which Linux's trees and the vendor's both have.

**The power-on block.** Its subtype (register 0x05) says its generation: 1 or 4 the primary block of before
(the keys in bits 0 and 1 of its status), 9 the half of generation 3's for the operating system (bits 7 and 6); the
buttons driver reads it at its step 3 and says which. The board's `buttons { pon }` is where it is, which
`tools/boards.py new --dts` reads from the vendor's `qcom,qpnp-power-on` node or Linux's `qcom,pm8998-pon`,
`qcom,pmk8350-pon` and the like (a PMK8350's at its half for the operating system, `pon@1300`).

**TSENS.** Every chip since the MSM8996 has version 2: a word a sensor at 0xa0 of the readings' block, valid in
bit 21, tenths of a degree in [11:0]. The vendor's trees name that block `tsens_tm_physical`; Linux's give it as the
first reg of a `qcom,tsens-v2` node, which `tools/boards.py new --dts` reads as well. Up to eight controllers.

**The storage.** The vendor's trees name the host's registers `ufs_mem`, Linux's `std` or not at all (the first
reg of `qcom,ufshc`); both are read.

## Bringing up a chip

1. `tools/boards.py new NAME --soc SOC --dts phone.dts` (the phone's tree, decompiled from its `dtbo` or `dtb`
   with `dtc -I dtb -O dts`): the board and the SoC file, with what was found and a note of what was not.
2. `tools/boards.py check`, then `tools/boards.py status NAME`: what each feature still needs.
3. `KCONFIG_FRAGMENTS=bringup kernel/build.sh`: a kernel of the common drivers alone. At boot it says
   `kconfig: made of bringup.config, without ...`.
4. One driver at a time, by its boot argument ([porting.md](porting.md), step 5): each says in the log what it found
   (the arbiter's version, the frequency table, the sensors' readings) before it writes anything.
5. The parts of the phone (touch, GPU, radio, audio, camera) after: their drivers are of particular parts
   ([drivers.md](drivers.md)), and a fragment of one's own turns on those the phone has.

## Not done yet

- The PMIC arbiter of versions 2 and 3 (older 8-series) and 7 (between the 5 and the 8): their registers are
  elsewhere, and they are not taken from memory without a phone to see them on. The keys and the flashlight say the
  version and do nothing.
- eMMC storage, and UFS 4 hosts' vendor registers beyond what JEDEC lays out.
- The flash LED of PMICs other than the PM8350C.
- The table's core-count field and per-core DCVS of the cores' frequency block ([drivers.md](drivers.md)).
