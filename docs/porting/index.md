# Port a device

What it takes to bring HakiOS to a phone that does not have it yet, in the order that works: first a repository and a
configuration, then a boot with nothing of the phone's, then the drivers one at a time. Each step says how you know it is done.

> **Before you start.** A port runs from RAM and writes nothing by default, but you are working on a phone's hardware. Read
> [What is written to the phone](../concepts/architecture.md#what-is-written-to-the-phone), keep a way back to Android
> (`fastboot` and the stock images), and pull the phone's firmware before anything else.

## What you need

- a phone with an ARM64 Qualcomm SoC, its boot loader unlocked; a UEFI port for it (a Mu-Silicium port, as `devices/redwood/uefi` is, or your own);
- the phone's own device tree (`adb shell` with root, or from its boot image) and its Android running, for the firmware;
- a machine that builds the kernel ([Building from source](../getting-started/building.md)).

## 1. The repository, from the phone's own device tree

```sh
adb pull /sys/firmware/fdt device-tree.dtb          # the tree of the phone's own Android (or --adb below, which pulls it)
tools/hakios-device.py new mydevice --soc sc7280 --title "My Phone" --vendor Someone --maintainer "you" --fdt device-tree.dtb
make mydevice_defconfig
make config
```

That makes `devices/mydevice/` with:

- `soc.args`, `pull-soc.sh` and a README from `tools/new-device.py`: what the kernel needs to know about the SoC (`soc_*`: the
  modem's blocks, the IPA, the mailboxes) read out of your phone's tree and compared with the reference phone. The same values
  as the reference: the SoC code already fits. Differences go to the kernel through `soc.args`;
- `device.conf`, a defconfig of three `#include` lines and a fragment in which **every phone driver of the common repository is
  left out**: their addresses, pins and rails are the phone they were written for, and a driver of another phone touching yours
  is the last thing you want. The drivers of your SoC (from its fragment) and QEMU's stay in.

The SoC's layer comes with the device, in `devices/mydevice/soc/`: a copy of the one another device carries when the SoC is one
HakiOS knows (`--soc sc7280` copies redwood's), or a new one to fill in for a SoC nobody has ported yet (`--soc sm8250
--soc-title "Snapdragon 865"`). `--repo` makes the directory a git repository, ready to push as `hakios-mydevice`.

**Done when** `tools/hakios-device.py check mydevice` says it holds together, and the README says which SoC values differ and you
know why.

## 2. The first boot, with nothing of the phone's

```sh
make kernel                         # the kernel with only what the fragment kept
make loader
make phone SECS=120                 # or ./xl enter, ./xl ramdisk, ./xl run, one at a time
```

What to look for in `logs/`: the loader's banner, `kernel.ver=`, the kernel's first lines, `hakios-kernel: N drivers built in (...)`
with only the drivers you meant, and launchd. The display shows UEFI's framebuffer.

**Done when** a run reaches launchd and ends by itself, and the phone comes back.

## 3. The drivers, one at a time

Bring them back in the order each needs the ones before it:

| Order | Driver | Why then |
|---|---|---|
| 1 | `CPUFREQ`, `THERMAL`, `SOCINFO` | the SoC's: they read what the chip is and how warm, and touch nothing that is the phone's |
| 2 | `BUTTONS`, then the touch screen (`GOODIX` or one of your own) | input, so that the system can be used |
| 3 | `UFS` | the phone's storage, read only; the modem needs it |
| 4 | `ADRENO` | the GPU; needs `fw-gpu/` |
| 5 | `ADSP`, `BATTERY`, `SPEAKER`, `SOUND`, `MIC` | audio and the battery's level come through the audio DSP |
| 6 | `WIFI` (and `QMI`) | needs `fw-wifi/` |
| 7 | `HAPTIC`, `CAMERA`, `TORCH`, `MODEM`, `BT` | the rest |

For each: read the head of the driver (`drivers/<subsystem>/hakios.<name>.c` says where every address and number came from), find
the same in your phone's device tree, and change what differs. A value that is a SoC fact goes to the SoC's fragment or the
kernel's defaults; a value that is the phone's goes into a boot-argument of the device's fragment where the driver reads one, or
into a driver of the device's own when the hardware is another chip altogether ([Writing a driver](drivers.md)). Then
`CONFIG_HAKIOS_<NAME>=y` in `configs/mydevice.config`, and the driver's boot-argument.

**Done when** the part works on the phone: write it down as a `status` line in `device.conf`, with how it is turned on.

## 4. Write it down

```sh
python3 tools/docs-gen.py           # the device's page, from device.conf
tools/hakios-device.py check mydevice
```

`device.conf` is the record of what works. Only what you have seen on the phone is `works`; what is written and not yet seen is
`partial` with a note; what is not there is `missing`.

## 5. Publish it

[Publishing a device](repositories.md#publishing-a-device): split it into a repository of its own and add it to
`devices/manifest.conf` in a pull request.

## Metal and the panel

The window server's Metal path is built for one GPU and one display controller so far, and it says so instead of guessing:

- **The GPU.** `hakios-metal/` has the tables of the Adreno of chip ID `0x06030500` (`include/hakios-metal/a6xx.hpp`). Another's is
  refused by `hakios_metal_open()`, the reason goes to the log and the window server stays on OpenGL ES. A port of another
  Adreno starts there, with Mesa's `freedreno_devices.py`.
- **The scan.** `fb_beam` (frames written behind the panel's scan) and `fb_measure` read the display interface's registers.
  They are known for `sc7280`; on another SoC nothing is read and frames go on their beat. `fb_intf=PHYSICAL` names them,
  from Linux's catalogue for your SoC. `fb_beam` checks that they count like a line counter before it trusts them.
- **The run.** `tools/metal-run.sh` reads `devices/<your phone>/metal.args`, which `tools/hakios-device.py new` makes: its big core,
  its cores, its panel's rate and its touch poll, for instance `bootcpu=7 cpus=8 fb_hz=60 fb_threads=4 touch_poll_us=8000`.
  `DEVICE_ARGS=` overrides it for one run. Without either, a phone other than redwood gets `fb_hz=60 fb_threads=3`.
- **The picture.** Any panel is stretched to from the screen class chosen: `fbscale=`, or `fbwidth=`, `fbheight=`, `fbsubtype=`.

## When a step does not work

| Symptom | Look at |
|---|---|
| nothing after the loader's banner | the UEFI port: the loader needs the framebuffer and its file from UEFI |
| the kernel stops early | the SoC's addresses (step 1); `soc.args` |
| a driver's step fails | its log lines name the step and the register; the head of the driver says what each step needs |
| `make config` reports a problem | it names the symbol and why: a driver left out that an argument needs, firmware the open profile does not load |
