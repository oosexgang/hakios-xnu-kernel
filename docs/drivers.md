# Drivers

Every driver in the kernel: what it is for, what turns it on, and how much of it belongs to one
phone. Sources are in `kernel/overlay/`; each file starts with a long comment that is the real
documentation. Boot arguments are in [boot-args.md](boot-args.md).

**Portability** in the tables means:

| | |
| --- | --- |
| **any** | No device knowledge in the code. Works on any board, QEMU included. |
| **data** | Device knowledge comes from `devices/` and `soc/`. A new phone is a change there. |
| **part** | Written for one part with one wiring. Runs only on a board that lists its feature. A phone with another part needs another driver. |
| **phone** | Addresses of one phone or SoC are in the code. Listed under "still tied" in [porting.md](porting.md). |

A driver does nothing until its boot argument asks for it, and a **data** or **part** driver also
needs its feature in the board's `features`.

## Platform

| File | What | Portability |
| --- | --- | --- |
| `osfmk/arm64/gic_v3_irq.c` | Interrupts through a GICv3. | any (addresses from the loader) |
| `osfmk/arm64/psci_smp.c` | The other cores, started through PSCI. `cpus=N`. | any |
| `osfmk/arm64/loader_return.c` | Return to the loader with the log, on exit and on panic. | any |
| `osfmk/arm64/redwood_a64emu.c` | The few arm64e instructions an ARMv8.2 core lacks, emulated when they trap. | any |
| `osfmk/arm64/hakios_prof.c`, `user_exception_trace.c` | A sampling profiler (`hakios_prof=NAME`) and crash descriptions (`systrace=`). | any |
| `iokit/Kernel/GenericARMPE.cpp` | The platform expert: CPUs, the time of day. | any |
| `bsd/dev/hakios_board.c` | Which board this is; `hw.board.*`. | data |

## Stand-ins for Apple's closed kernel extensions

| File | Stands in for | Portability |
| --- | --- | --- |
| `iokit/Kernel/MiniIOMobileFramebuffer.cpp` | The display driver family: swaps, vsync, scaling to the panel, software dimming, the Metal plug-in gate (`metal=1`). | any; `fb_scan` timing is **data** (`display-scan`, `[display]` intf1) |
| `iokit/Kernel/MiniIOSurface.cpp` | IOSurface. | any |
| `iokit/Kernel/MiniAppleKeyStore.cpp` | AppleKeyStore: a device without a passcode. | any |
| `iokit/Kernel/MiniIONetworkStack.cpp`, `RedwoodWLAN.cpp` | The registry entries by which iOS finds network interfaces. | any |
| `iokit/Kernel/RedwoodBattery.cpp` | The battery as iOS looks for it (IOPMPowerSource). Fed by `redwood_battery.c` or the audio DSP. | any |
| `iokit/Kernel/RedwoodGPU.cpp` | The GPU for user space: the interface of Linux's msm DRM driver, as Mesa's freedreno speaks it. | any Adreno the kernel has started |
| `libkern/amfi/`, `bsd/kern/kern_minimac.c` | AMFI and Sandbox policies. | any |
| `osfmk/minicrypto/` | corecrypto. | any |

## Display, input

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/hakios_backlight.c` | Panel brightness: MIPI DCS through a Qualcomm DSI controller, or software. Automatic brightness. | `[backlight]` / `backlight=` | data |
| `bsd/dev/redwood_adreno.c` | Adreno 642/642L bring-up, step by step: clocks, GMU, zap shader, first frame. | `gpu-a660`, `[gpu]` / `gpu=1`, `adreno=` | data for a 642 or 642L (`[gpu]`); another Adreno generation: the driver |
| `bsd/dev/redwood_input.c` | `/dev/redwood-touch`: the queue of touches and button presses every source feeds. | — | any |
| `bsd/dev/redwood_goodix.c` | Goodix GT9916 or FocalTech FT3680 touch controller on redwood's SPI. | `touch-goodix` / `touchscreen=1` | part |
| `bsd/dev/renoir_fts.c` | FocalTech FT3658U touch controller on Renoir's SPI. | `touch-ft3658u` / `touchscreen=1` | part |
| `bsd/dev/redwood_buttons.c` | Power and volume keys on the PMIC, through the SPMI arbiter. | `buttons-pmic` / `buttons=1` | data (`[spmi]`, `[buttons]`); arbiter version 5 |
| `bsd/dev/redwood_virtio_input.c` | QEMU's tablet and keyboard as touch and buttons. | QEMU only | any (QEMU) |

## Power, CPU, temperature

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/redwood_cpufreq.c` | CPU frequency through EPSS; memory bandwidth votes through RPMh. | `cpufreq-epss`, `ddr-rpmh` / `cpufreq=1`, `ddr=` | EPSS: data (`[cpufreq]`). The RPMh vote: data (`[rpmh]`), with the slot layout and BCM names of the 7-series |
| `bsd/dev/hakios_thermal.c` | Reads TSENS; holds CPU groups and the panel back when hot. | `[thermal]` / `thermal=` | data |
| `bsd/dev/redwood_battery.c` | The battery as the UEFI's charger drivers report it to the loader. | `battery-uefi` / `battery=1` | any phone whose loader provides it |
| `bsd/dev/hakios_cputop.c` | Which processes use the CPU (`cputop=S`). | — | any |

## Sound, camera, haptics, flashlight

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/redwood_adsp.c` | The audio DSP: started, and spoken to. Also carries the battery manager. | `adsp`, `[adsp]` / `adsp=1` | phone: the firmware directory, memory size and the gauge's units are data (`[adsp]`); the DSP's own addresses are the 7-series' |
| `bsd/dev/redwood_audio.c` | `/dev/redwood-audio`: what iOS plays, to whichever sink is there. | — | any |
| `bsd/dev/redwood_sound.c`, `redwood_guard.c` | What the DSP is told so the AW88261 speakers play; speaker protection. | `speaker-aw88261` / `sound=N` with `adsp=1` | part |
| `bsd/dev/redwood_mic.c` | The microphones on a WCD938x codec, through the DSP. | `mic-wcd938x` / `mic=N` with `adsp=1` | part |
| `bsd/dev/redwood_speaker.c` | Two Awinic AW88261 amplifiers on I2C. | `speaker-aw88261` / `speaker_probe=` | part |
| `bsd/dev/redwood_haptic.c` | An Awinic AW86927 vibration motor driver. | `haptic-aw86927` / `haptic=` | part |
| `bsd/dev/redwood_torch.c` | The flash LED module of a PM8350C. | `torch-pm8350c` / `torch=` | part; the arbiter is data (`[spmi]`) |
| `bsd/dev/redwood_camera.c` | redwood's four cameras: sensors, CSI, ISP. | `camera-redwood` / `cam=` | part |
| `bsd/dev/redwood_virtio_sound.c` | QEMU's sound device as the sink. | QEMU only | any (QEMU) |

## Wi-Fi, network

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/redwood_wifi.c`, `redwood_wlfw.c` | A WCN6750 on the SoC's Wi-Fi DSP: firmware load, bring-up. | `wifi-wcn6750` / `wifi=1` | part |
| `bsd/dev/redwood_lx.c`, `redwood_qmi.c`, `redwood_sta.c` | The kernel's side of the Linux Wi-Fi driver's object; QMI; the station boundary. | — | any |
| `bsd/dev/redwood_apple80211.c` | Apple80211 requests of iOS's Wi-Fi daemon, answered in the kernel. | — | any |
| `bsd/dev/redwood_net.c` | `en0`, an Ethernet interface as far as iOS can tell. | — | any |
| `bsd/dev/redwood_virtio_net.c` | QEMU's network device behind `en0`. | QEMU only | any (QEMU) |

## Storage, file systems

| File | What | Portability |
| --- | --- | --- |
| `bsd/miscfs/apfs/` | APFS: reads the iOS images in RAM; writes for the data volume. | any |
| `bsd/miscfs/ramfs/` | The RAM file system for `/private/var`. | any |
| `bsd/dev/redwood_image.c` | A file of one of the RAM disks by path, for drivers that need firmware before there is a root. | any |
| `bsd/dev/redwood_ufs.c`, `redwood_ufshc.c` | The phone's UFS: through the resident UEFI's driver, or the host controller directly (read only). | phone (redwood) |
| `bsd/dev/redwood_sparse.c`, `redwood_virtio_blk.c`, `redwood_apfs_check.c` | An image with only some of its blocks; QEMU's block device; a walk of a volume for comparison with the host tools. | any |

## Adding one

1. Write the driver in `kernel/overlay/bsd/dev/`, and add it to `bsd/conf/files` in an assembled
   tree (then `kernel/export.sh`; see [kernel/README.md](../kernel/README.md)).
   `kernel/syntax-check.sh --tree DIR bsd/dev/your_driver.c` compiles it in a second, without the
   kernel toolchain.
2. If it is for a part, add a `HAKIOS_HAS_…` feature in `hakios_board.h` and check it at the start,
   after the boot argument. If it needs addresses, add them to `struct hakios_board` and to
   `tools/boards.py`, as `[backlight]` and `[thermal]` are.
3. Start with a level that only reads and logs. `hakios_say()` (`hakios_log.h`) puts a line in the
   loader's log at once, where it can be read while the phone runs, and in the kernel's own.
4. Give it a comment that names its boot argument as `name=…`: `make docs` puts it in the reference.
5. Where the logic can run without hardware, test it on the host. `tests/backlight_driver_test.c` runs a
   driver unchanged against a model of its registers.
