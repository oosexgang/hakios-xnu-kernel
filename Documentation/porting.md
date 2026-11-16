# Porting to another Qualcomm phone

What a port takes, in the order to do it. Read [architecture.md](architecture.md) first: it says
which parts are data, which are drivers and which are the same for every device.

A realistic target is a Qualcomm phone with an unlockable bootloader and a UEFI port in
[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium). A phone on a SoC that is already
described (a package with its `soc.dtsi`: SM7325, SM7350) needs the least; a new SoC is one more file
in the package.

Nothing in this procedure writes to the phone's storage. The UEFI is started with `fastboot boot`,
everything runs from RAM, and a reboot brings Android back.

Steps 1, 3, 8 and the data side of 4 and 5 are run on every push by `workbench/porting_test.sh`: a made-up
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

This writes the board file (`devices/NAME/board.dts`, here with `mido` for NAME) and the SoC file beside
it (`devices/NAME/soc.dtsi`): a copy of another package's when one that is present has that SoC, else a
new one from the tree. The directory `devices/NAME/` is your phone's device package. Git ignores it in a
checkout of the main branch, because packages are kept on the `devices` branch (step 8). From the tree it
reads:

- the DSI controller, the TSENS controllers, the CPU frequency block, the clock controllers' addresses;
- the interrupt controller, the cores with their MPIDR numbering, which cores are the small ones, the
  PSCI method;
- the panel: brightness range, byte order, command or video mode. Pass `--panel NODE` if it reports
  several panels;
- the PMIC arbiter, and which PMIC peripherals the power and volume keys and the flashlight are on;
- the touch controller's reset and interrupt pins (and its serial engine, where the tree has the
  controller under its bus), as a `touch { }` section for you to complete;
- the SoC's own pins, its SMMU, the way to its other processors, the UFS host controller, the audio
  DSP's reserved memory;
- RPMh's command database and the application processor's sequencer;
- the blocks on the microphones' path (the TX and VA macros, the SoundWire master and its hardware
  control register, the audio subsystem's own pins), in an `lpass { }` left commented out: the
  SoundWire pins are not in words the tool reads, and only `mic-wcd938x` needs the section.

It prints what it could not find. Two things are never in a device tree and are yours to fill in:

- **Clock register offsets** in the SoC file's `clocks { }`: `gcc-disp-ahb-cbcr` (Linux `gcc-SOC.c`,
  `gcc_disp_ahb_clk`), `dispcc-mdss-gdscr` (the MDSS GDSC) and `dispcc-mdss-ahb-cbcr`
  (`disp_cc_mdss_ahb_clk`), both in `dispcc-SOC.c`. They are asked for by what touches the display's
  registers: `backlight type = "dcs"`, `display-scan` and the GPU. Until you have them, set
  `type = "soft"`, which works anywhere; `gcc` alone is enough for everything else.
- **The I/O window** in `platform { }`: `io-base` and `io-size`, one range that covers the SoC's
  peripherals and does not start at 0. It is asked for once the board has a `loader { }` section (step 3). The 7-series SoCs here use `0x00400000` and `0x3FC00000`.

Then:

```sh
tools/boards.py check        # says which file and why, until everything is consistent
make check
```

A file that was written by hand, or taken from someone else's port, can be held against the phone's own
tree at any time:

```sh
tools/boards.py compare mido --dts phone.dts     # each value: as in the tree, different, or not in any tree
```

There are no tables to regenerate by hand: every build makes them itself from the packages that are
present (`make boards` validates and writes them into `build/boards/` if you want to look at them).

Leave `features = <>;` (none) for now. Every key is explained in `devices/_template/board.dts` and
`devices/_template/soc.dtsi`. To see how an existing phone is described, bring its package in:
`tools/device.py available` lists them, `tools/device.py add renoir` copies one into `devices/`.

## 2. The UEFI

The loader runs under a UEFI. Port Mu-Silicium for the phone: the platform package with the phone's
memory map, the UEFI drivers extracted from the phone's own firmware -- the first phone's package has a
whole port under `legacy/uefi/` on the `devices` branch, with its README -- and what every port needs,
in `base/uefi/` (`apply.sh PHONE_FILES` puts both into a clone): the two changes in
`base/uefi/mu-silicium.patch`:

- the boot shim copies the payload that follows the UEFI image to a reserved address;
- the boot manager starts the loader from it instead of showing a menu.

The reserved address is a convention between the shim and the loader: 0xF0000000, 64 MB, on the
first phones. It has to be RAM in your phone's memory map that the firmware does not use for
anything else. If it cannot be there, change it in the shim and build the loader to match
(`LOADER_CFLAGS="-DPAYLOAD_BASE=... -DPAYLOAD_MAX_SIZE=..." arch/arm64/boot/build.sh`;
`arch/arm64/boot/src/conventions.h` has both). The loader keeps the kernel's memory clear of that range.

## 3. The loader

Build it and start it once without a kernel:

```sh
arch/arm64/boot/build.sh
```

At start it prints `display W x H at ADDRESS`. Put that address in the board file:

```dts
	loader {
		fb-base = <0xE1000000>;
	};
```

and run `arch/arm64/boot/build.sh` again (it makes its table of boards itself). From now on the loader
recognises the phone, prints `platform mido`, and passes `darwin-board = "mido"` to the kernel.

## 4. First boot: nothing but the display

Build the kernel ([building.md](building.md)) and boot with no hardware feature on. The kernel's log
comes back through the loader. Look for, in this order:

| Line in the log | Meaning |
| --- | --- |
| `board: mido (Maker Model, sm7325): no features listed` | The kernel found its row. If it says `not one of the N this kernel knows`, the loader passed another name or the kernel was built without the phone's package present. |
| `MiniIOMobileFramebuffer: display … on a panel of …` | The UEFI's framebuffer is the display. |
| launchd, then SpringBoard | Darwin is up, drawn in software. |

Start with one core (`cpus=1`), then all of them. A hang at the second core is PSCI: check `psci` and
`core-shift` in the SoC file.

## 5. Turn hardware on, one piece at a time

`tools/boards.py status NAME` lists every feature for your board: on, ready to be listed, or the
section it still needs. Each row below is independent. Add the feature to `features`, rebuild, and
pass the boot argument. Most drivers have a "look only" level that reads and logs without writing: use it
first. Every argument is in [boot-args.md](boot-args.md); every driver in [drivers.md](drivers.md);
what each feature needs written down before it can be listed, key by key, in [features.md](features.md).

| Step | In the board or SoC file | Boot argument | It worked when |
| --- | --- | --- | --- |
| Frames without tearing (command-mode panels) | `display-scan`, `display { }` intf1 | `fb_scan=1` (the default) | `panel scan:` lines with a period near 16.7 ms. Without it frames are written on a timer, which works. |
| Brightness | `backlight { }`, `clocks { }`, `display { }` | `backlight=1` (reads only), then `backlight=2` | `backlight: dcs: DSI controller at …: version 2.x … (on, …)`, then the panel follows the slider. `NOT SENT` means the wrong controller or mode. [backlight.md](backlight.md) |
| Temperature | `thermal { }` in both files | `thermal=1` | `thermal: hottest sensor NN.N C` with plausible numbers. Compare with Android's. [thermal.md](thermal.md) |
| CPU frequency | `cpufreq-epss`, `cpufreq { }` | `cpufreq=1` | `cpufreq: group N …: row A -> B …, NNNN MHz` for each group. Then `thermal=2`. |
| Memory bandwidth | `ddr-rpmh`, `rpmh { }` | `ddr=1` (looks), `ddr=2` with `cpufreq=1` | The `ddr:` lines show the RPMh resources found, then the vote. |
| GPU (Adreno 642, 642L, 643) | `gpu-a660`, `gpu { }`, `adreno { }` in the SoC file | `gpu=1` | `adreno: NAME: Adreno … (chip …)`, then the numbered steps up to the GPU's clock being measured at the speed asked for. Not needed for a picture: Darwin draws in software without it. [metal-glass.md](metal-glass.md) |
| Buttons | `buttons-pmic`, `spmi { }`, `buttons { }` | `buttons=1` | `buttons: PMIC arbiter version 0x5…`, each key's peripheral found and of the kind expected, then each press and release logged. The driver only reads. |
| Battery | `battery-uefi` | `battery=1` | A state of charge that matches Android's. |
| Audio DSP | `adsp`, `adsp { }` | `adsp=1` | The DSP's services come up. |

`hw.board.features` in a running system shows what the kernel has for the board.

## 6. Parts that need a driver

Some parts are not a matter of addresses: a touch controller speaks its own protocol on its own bus,
an amplifier has its own registers. These are drivers, in `kernel/overlay/bsd/dev/`, and each has a
feature named after the part and the wiring it expects (`touch-goodix`, `haptic-aw86927`, ...).

- If your phone has **the same part wired the same way**, list the feature and try it.
- If it has **one of the touch controllers** (`touch-goodix`, `touch-ft3658u`) on another serial
  engine or other pins: fill in `touch { }` of the board file. `devices/_template/board.dts` says
  where each value is in the device tree and in the SoC's clock driver;
  `tools/qupclocks.py gcc-SOC.c WRAPPER ENGINE` reads the engine's clocks and their votes out of
  that driver for you.
- If it has **the same part on other pins or another bus** and the driver has them in its code, the driver needs those made data. Move
  the constants into the board file and `board/board.h`, as the buttons' driver has its peripherals
  in `buttons { }`, the flashlight's in `torch { }`, the vibration motor's pins in `haptic { }` and the
  speaker amplifiers' bus and addresses in `speaker { }`:
  [devices/README.md](../devices/README.md#adding-a-kind-of-data) goes through the steps. Those
  drivers are the example of how that is kept safe: the test of each runs it on a second phone
  with the part elsewhere (`workbench/torch_driver_test.c`, `haptic_driver_test.c`,
  `speaker_driver_test.c`), so a value of the first phone left in the code shows as a part that
  does not answer.
- If it has **another part**, write a driver next to the existing one and add a feature for it in
  `board/board.h`. [writing-a-driver.md](writing-a-driver.md) is the guide, with an example driver
  and its test to start from, and the headers a driver has at hand: the SoC's pins, I2C by hand on
  two of them, the PMICs, RPMh. `board/fts.c` beside `board/goodix.c` shows two parts of a kind:
  two touch controllers, one interface to Darwin (`/dev/hakios-touch`, `bsd/sys/board/input.h`).

[drivers.md](drivers.md) lists every driver with the part it is for and how tied to one phone it is.
Touch first: without it the phone is a picture.

A driver asked for on a board that does not list its feature prints which line is missing and does
nothing. That is deliberate: these drivers write to pins and PMIC registers.

## 7. Darwin user space

The user-space side is the same for every phone: the libraries in `darwin/userland/` and the images
built by `darwin/tools/`. What depends on the device is small:

- which iPhone Darwin is told it is (`identity.h`, which `arch/arm64/boot/build.sh` makes from the IPSW's device tree), and through that the screen layout;
- the status bar's clock offset for a hole-punch camera at the top left: `statusbar-leading-shift`
  of `screen { }` in the board file, in points (`components/statusbar/inject_statusbar.c` applies it);
- the boot defaults, in the board file's `boot { }` section.

How the images are built and what Darwin needed on the way to the home screen: [darwin26-boot.md](darwin26-boot.md).

## 8. Send it back

```sh
make check
```

A port goes to two places:

| What | Where it goes |
| --- | --- |
| The device package: the `devices/NAME/` directory with `board.dts` and `soc.dtsi`, and a README and notes if you have them | The `devices` branch, as the directory `NAME/` at its top. |
| Any new driver with its feature, a new key of the board or SoC file | The main branch. |

No generated table is sent: none is in the repository. For the package:

```sh
git worktree add ../hakios-devices devices          # a checkout of the devices branch, beside this one
tools/device.py export NAME ../hakios-devices       # validates the package, then copies it there
```

Write its `README.md` there (what works, what was verified on the phone and when, where the values come
from; the packages that are there show the form), commit and send it. Others then get it with
`tools/device.py add NAME`, which copies it into their checkout and validates it against the code they
have. When a package needs a key or a feature the main branch does not have yet, the two changes belong
together: say so in both. [CONTRIBUTING.md](../CONTRIBUTING.md) has the rest.

## What is still tied to the first two phones

Stated plainly, so nobody finds out the hard way:

- **GPU.** `board/adreno.c` starts an Adreno of the A660 family. What differed between the two phones
  is data (`gpu { }` in the board file, feature `gpu-a660`), and so is where the GPU's blocks are
  (`adreno { }` in the SoC file), so a third 642 or 642L is two sections to fill in. The sequence itself
  (the GMU's registers, the clock controller's layout, the zap shader's hand-over) has only been run on
  these two chips: another generation of Adreno needs work in the driver.
- **The memory bandwidth vote** (`ddr-rpmh`) has its addresses in the SoC file (`rpmh { }`), but expects
  what both phones have: the sequencer's first slot set is an active one, and the memory's bus clock
  managers are named MC0, SH0 and ACV. `ddr=1` only looks and prints what your SoC has.
- **Sound, microphones, camera and Wi-Fi** are drivers of particular parts: a WCD938x codec on
  SoundWire, a WCN6750 radio on the SoC's Wi-Fi DSP, four camera sensor chips (`chips[]` in
  `board/camera.c`) on an SM7325's camera blocks. They are the largest share of the work on a new
  phone. Everything that differs between phones with those parts is data:
  - in the SoC file: where the blocks are (`pins { }`, `clocks { }`, `rpmh { }`, `ipc { }`, `lpass { }`,
    `camera { }`), the LPASS pins of the microphones' SoundWire (`lpass { }` `swr-tx-pins`), and the
    SMMU streams the parts' DMA comes in on (`smmu { }` `audio-stream`, `wifi-stream`, `camera-stream`);
  - in the board file: the speakers' bus, addresses and sound pins and the TDM interface
    (`speaker { }`), the codec's reset (`mic { }`), the radio's pins, supplies and firmware files
    (`wifi { }`), and the cameras (`camera { }`: the PMIC that feeds them, and a `sensor@N` node for each
    sensor with its chip, its pins, its MCLK, its CSI receiver, its supplies and its lens motor's).

  What stays in the code is how each part is spoken to, and for the cameras the kind of PLL the clock
  controller has (a Lucid PLL0 and a Zonda PLL2, as on an SM7325) and the rates its clocks run at; where
  each clock is, how the PLLs are configured and which source each output is, is the SoC file's
  `camera { }`, and the driver works the dividers out of it. For the touch controllers, the keys, the flashlight, the vibration motor and the speaker
  amplifiers that is all: a phone with one of those parts wired differently needs its section of the
  board file filled in and no change to the driver. [features.md](features.md) lists what each
  feature needs.
- **Darwin's camera** (`components/camera/src/camera_device.c`, of the Darwin 16.5 generation) presents two
  cameras the way an iPhone X has them (optics and formats, which Darwin's capture stack matches its
  presets to). Which of the phone's sensors is the back and which the front camera, and whether a
  sensor's pictures are upside down, is the board's (`camera { }` `darwin-sensor`, `darwin-front`, a sensor's
  `upside-down`); a sensor's colour tuning is found by its chip (`fw/camera-CHIP.isp`, made by
  `components/camera/tools/camtuning.py` from the phone's tuning file).
- **Writing to the phone's storage** needs a partition of the port's own, named `darwin`, and its
  GPT entry in the board file's `storage { }`: both GUIDs, the first and last block, and the block from
  which writing is allowed. The loader serves a write only into a partition that is that entry in
  every field (`arch/arm64/boot/src/own_partition.c`), the kernel asks for one only there
  (`bsd/dev/board/ufs.c`), and the firmware bounds it to the partition. A board without
  `storage { }` is never written; reading works, the data volume lives in RAM.
- **Renoir's own boot path** uses a loader and tools that are not in this repository yet
  (see "What is not in this repository" in [architecture.md](architecture.md)).
