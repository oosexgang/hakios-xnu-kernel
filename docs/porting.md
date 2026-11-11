# Porting to another Qualcomm phone

What a port takes, in the order to do it. Read [architecture.md](architecture.md) first: it says
which parts are data, which are drivers and which are the same for every device.

A realistic target is a Qualcomm phone with an unlockable bootloader and a UEFI port in
[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium). A phone on a SoC that is already
described (`soc/sm7325.toml`, `soc/sm7350.toml`) needs the least; a new SoC adds one file.

Nothing in this procedure writes to the phone's storage. The UEFI is started with `fastboot boot`,
everything runs from RAM, and a reboot brings Android back.

Steps 1, 3 and the data side of 4 and 5 are run on every push by `tests/porting_test.sh`: a made-up
phone on a made-up SoC, with none of the addresses the two real phones have, is taken through them in
a copy of the repository. What that cannot show is the hardware answering; for that the guide gives
the log line to look for at each step.

## What you need from the phone

| What | How to get it | Used for |
| --- | --- | --- |
| The device tree, as source | From a rooted Android: `adb shell su -c 'cat /sys/firmware/fdt' > fdt.dtb`, then `dtc -I dtb -O dts fdt.dtb > phone.dts`. This is the live tree, with the overlays (the panel) already applied. | The board and SoC files. |
| The panel's name | `adb shell cat /proc/cmdline`, the value of `msm_drm.dsi_display0=`. | Choosing the right panel node when the tree has several. |
| The vendor's kernel source for the SoC | The maker's open-source release. | Clock controller register offsets (`dispcc-*.c`, `gcc-*.c`). |
| Firmware files | `adb pull` from `/vendor/firmware*` on the same phone. | Only later, for the GPU, DSPs and touch. Never committed. |

## 1. Describe the board

```sh
tools/boards.py new mido --soc sm7325 --dts phone.dts --description "Maker Model"
```

This writes the board file (`devices/NAME/board.toml`, here with `mido` for NAME), and the SoC file if
that SoC is new. From the tree it reads:

- the DSI controller, the TSENS controllers, the CPU frequency block, the clock controllers' addresses;
- the interrupt controller, the cores with their MPIDR numbering, which cores are the small ones, the
  PSCI method;
- the panel: brightness range, byte order, command or video mode. Pass `--panel NODE` if it reports
  several panels.

It prints what it could not find. Two things are never in a device tree and are yours to fill in:

- **Clock register offsets** in the SoC file's `[clocks]`: `gcc_disp_ahb_cbcr` (Linux `gcc-SOC.c`,
  `gcc_disp_ahb_clk`), `dispcc_mdss_gdscr` (the MDSS GDSC) and `dispcc_mdss_ahb_cbcr`
  (`disp_cc_mdss_ahb_clk`), both in `dispcc-SOC.c`. They are only needed for `backlight type = "dcs"`;
  until you have them, set `type = "soft"`, which works anywhere.
- **The I/O window** in `[platform]`: `io_base` and `io_size`, one range that covers the SoC's
  peripherals and does not start at 0. The 7-series SoCs here use `0x00400000` and `0x3FC00000`.

Then:

```sh
tools/boards.py check        # says which file and why, until everything is consistent
tools/boards.py gen          # rewrites the kernel's and the loader's tables
make check
```

Leave `features = []` for now. Every key is explained in `devices/_template/board.toml`.

## 2. The UEFI

The loader runs under a UEFI. Port Mu-Silicium for the phone the way `base/uefi/` does for redwood
(`base/uefi/README.md`): the platform package with the phone's memory map, the UEFI drivers extracted
from the phone's own firmware, and the two changes in `base/uefi/mu-silicium.patch`:

- the boot shim copies the payload that follows the UEFI image to a reserved address;
- the boot manager starts the loader from it instead of showing a menu.

## 3. The loader

Build it and start it once without a kernel:

```sh
loader/build.sh
```

At start it prints `display W x H at ADDRESS`. Put that address in the board file:

```toml
[loader]
fb_base = 0xE1000000
```

and run `tools/boards.py gen` and `loader/build.sh` again. From now on the loader recognises the
phone, prints `platform mido`, and passes `darwin-board = "mido"` to the kernel.

## 4. First boot: nothing but the display

Build the kernel ([building.md](building.md)) and boot with no hardware feature on. The kernel's log
comes back through the loader. Look for, in this order:

| Line in the log | Meaning |
| --- | --- |
| `board: mido (Maker Model, sm7325): no features listed` | The kernel found its row. If it says `not one of the N this kernel knows`, the loader passed another name or the table was not regenerated. |
| `MiniIOMobileFramebuffer: display … on a panel of …` | The UEFI's framebuffer is the display. |
| launchd, then SpringBoard | iOS is up, drawn in software. |

Start with one core (`cpus=1`), then all of them. A hang at the second core is PSCI: check `psci` and
`core_shift` in the SoC file.

## 5. Turn hardware on, one piece at a time

Each row is independent. Add the feature to `features`, run `tools/boards.py gen`, rebuild, and pass
the boot argument. Most drivers have a "look only" level that reads and logs without writing: use it
first. Every argument is in [boot-args.md](boot-args.md); every driver in [drivers.md](drivers.md).

| Step | In the board or SoC file | Boot argument | It worked when |
| --- | --- | --- | --- |
| Frames without tearing (command-mode panels) | `display-scan`, `[display]` intf1 | `fb_scan=1` (the default) | `panel scan:` lines with a period near 16.7 ms. Without it frames are written on a timer, which works. |
| Brightness | `[backlight]`, `[clocks]`, `[display]` | `backlight=1` (reads only), then `backlight=2` | `backlight: dcs: DSI controller at …: version 2.x … (on, …)`, then the panel follows the slider. `NOT SENT` means the wrong controller or mode. [backlight.md](backlight.md) |
| Temperature | `[thermal]` in both files | `thermal=1` | `thermal: hottest sensor NN.N C` with plausible numbers. Compare with Android's. [thermal.md](thermal.md) |
| CPU frequency | `cpufreq-epss`, `[cpufreq]` | `cpufreq=1` | `cpufreq: group N …: row A -> B …, NNNN MHz` for each group. Then `thermal=2`. |
| Memory bandwidth | `ddr-rpmh` | `ddr=1` (looks), `ddr=2` with `cpufreq=1` | The `ddr:` lines show the RPMh resources found, then the vote. |
| GPU (Adreno 642, 642L, 643) | `gpu-a660`, `[gpu]` | `gpu=1` | `adreno: NAME: Adreno … (chip …)`, then the numbered steps up to the GPU's clock being measured at the speed asked for. Not needed for a picture: iOS draws in software without it. [metal-glass.md](metal-glass.md) |
| Buttons | `buttons-pmic` | `buttons=1` | Each press and release is logged. |
| Battery | `battery-uefi` | `battery=1` | A state of charge that matches Android's. |
| Audio DSP | `adsp`, `[adsp]` | `adsp=1` | The DSP's services come up. |

`hw.board.features` in a running system shows what the kernel has for the board.

## 6. Parts that need a driver

Some parts are not a matter of addresses: a touch controller speaks its own protocol on its own bus,
an amplifier has its own registers. These are drivers, in `kernel/overlay/bsd/dev/`, and each has a
feature named after the part and the wiring it expects (`touch-goodix`, `haptic-aw86927`, ...).

- If your phone has **the same part wired the same way**, list the feature and try it.
- If it has **the same part on other pins or another bus**, the driver needs those made data. Move
  the constants into the board file and `hakios_board.h`, as the backlight driver does.
- If it has **another part**, write a driver next to the existing one and add a feature for it in
  `hakios_board.h`. `renoir_fts.c` beside `redwood_goodix.c` is the example: two touch controllers,
  one interface to iOS (`/dev/redwood-touch`, `bsd/sys/redwood_input.h`).

[drivers.md](drivers.md) lists every driver with the part it is for and how tied to one phone it is.
Touch first: without it the phone is a picture.

A driver asked for on a board that does not list its feature prints which line is missing and does
nothing. That is deliberate: these drivers write to pins and PMIC registers.

## 7. iOS user space

The user-space side is the same for every phone: the libraries in `ios/userland/` and the images
built by `ios/tools/`. What depends on the device is small:

- which iPhone iOS is told it is (`loader/identity.h`), and through that the screen layout;
- the status bar's clock offset for a hole-punch camera (`ios/userland/inject_statusbar.c` reads the
  board name);
- the boot defaults, in the board file's `[boot]` section.

How the images are built and what iOS needed on the way to the home screen: [ios26-boot.md](ios26-boot.md).

## 8. Send it back

```sh
make check
```

A port is the board file, the SoC file if new, the two regenerated tables, and any new driver with its
feature. [CONTRIBUTING.md](../CONTRIBUTING.md) has the rest.

## What is still tied to the first two phones

Stated plainly, so nobody finds out the hard way:

- **GPU.** `redwood_adreno.c` starts an Adreno of the A660 family. What differed between the two phones
  is data (`[gpu]` in the board file, feature `gpu-a660`), so a third 642 or 642L is a section to fill
  in. The sequence itself (the GMU's registers, the clock controller's layout, the zap shader's hand-over)
  has only been run on these two chips: another generation of Adreno needs work in the driver.
- **The memory bandwidth vote** (`ddr-rpmh`) uses the RPMh addresses of the 7-series.
- **Sound, microphones, camera, Wi-Fi, haptics, flashlight, storage** are written for redwood's parts.
  They are the largest share of the work on a new phone.
- **Renoir's own boot path** uses a loader and tools that are not in this repository yet
  (see "What is not in this repository" in [architecture.md](architecture.md)).
