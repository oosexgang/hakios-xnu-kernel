# Drivers

Every driver in the kernel: what it is for, what turns it on, and how much of it belongs to one
phone. Sources are in `kernel/overlay/`; each file starts with a long comment that is the real
documentation. Boot arguments are in [boot-args.md](boot-args.md).

**Portability** in the tables means:

| | |
| --- | --- |
| **any** | No device knowledge in the code. Works on any board, QEMU included. |
| **data** | Device knowledge comes from the board file of the phone's device package and from `soc/`. A new phone is a new package, and a file in `soc/` if its SoC is new. |
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
| `bsd/dev/hakios_drivers.c` | The list every driver below is started from, in order. | any |

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
| `bsd/dev/redwood_adreno.c` | Adreno 642/642L bring-up, step by step: clocks, GMU, zap shader, first frame. | `gpu-a660`, `[gpu]` / `gpu=1`, `adreno=` | data for a 642 or 642L (`[gpu]`, and `[adreno]` of the SoC file for where its blocks are); another Adreno generation: the driver |
| `bsd/dev/redwood_input.c` | `/dev/redwood-touch`: the queue of touches and button presses every source feeds. | — | any |
| `bsd/dev/redwood_goodix.c` | Goodix GT9916 or FocalTech FT3680 touch controller on an SPI serial engine. | `touch-goodix` / `touchscreen=1` | part; which engine, its clocks, the pins and the supply are data (`[touch]`) |
| `bsd/dev/renoir_fts.c` | FocalTech FT3658U touch controller on an SPI serial engine. | `touch-ft3658u` / `touchscreen=1` | part; which engine, its clocks, the pins and the supplies are data (`[touch]`) |
| `bsd/dev/hakios_buttons.c` | Power and volume keys on the PMIC, through the SPMI arbiter. | `buttons-pmic` / `buttons=1` | data (`[spmi]`, `[buttons]`); arbiter version 5 |
| `bsd/dev/redwood_virtio_input.c` | QEMU's tablet and keyboard as touch and buttons. | `virtio` / `virtio_input=1` | any (QEMU) |

## Power, CPU, temperature

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/hakios_cpufreq.c` | CPU frequency through EPSS. | `cpufreq-epss` / `cpufreq=1` | data (`[cpufreq]`) |
| `bsd/dev/hakios_scmi_perf.c` | CPU performance levels and limits through SCMI. | `cpufreq-scmi` / `scmi_perf=1` | data (`[scmi_perf]`) |
| `bsd/dev/hakios_ddr.c` | Memory bandwidth votes through RPMh. | `ddr-rpmh` / `ddr=` | data (`[rpmh]`), with the slot layout and BCM names of the 7-series |
| `bsd/dev/hakios_bench.c` | Measurements: how fast the cores get at memory, how busy each core is. | — / `membench=`, `cpuload=` | any |
| `bsd/dev/hakios_thermal.c` | Reads TSENS; holds CPU groups and the panel back when hot. | `[thermal]` / `thermal=` | data |
| `bsd/dev/redwood_battery.c` | The battery as the UEFI's charger drivers report it to the loader. | `battery-uefi` / `battery=1` | any phone whose loader provides it |
| `bsd/dev/hakios_cputop.c` | Which processes use the CPU (`cputop=S`). | — | any |

## Sound, camera, haptics, flashlight

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/redwood_adsp.c` | The audio DSP: started, and spoken to. Also carries the battery manager. | `adsp`, `[adsp]` / `adsp=1` | phone: the firmware directory, memory size and the gauge's units are data (`[adsp]`); the DSP's own addresses are the 7-series' |
| `bsd/dev/redwood_audio.c` | `/dev/redwood-audio`: what iOS plays, to whichever sink is there. | — | any |
| `bsd/dev/redwood_sound.c`, `redwood_guard.c` | What the DSP is told so the AW88261 speakers play; speaker protection. | `speaker-aw88261` / `sound=N` with `adsp=1` | part |
| `bsd/dev/redwood_mic.c` | The microphones on a WCD938x codec, through the DSP. | `mic-wcd938x` / `mic=N` with `adsp=1` | part; the codec's reset pin is data (`[mic]`) |
| `bsd/dev/redwood_speaker.c` | Two Awinic AW88261 amplifiers on I2C. | `speaker-aw88261` / `speaker_probe=` | part; the bus's pins, each amplifier's address and interrupt and the sound's four pins are data (`[speaker]`) |
| `bsd/dev/redwood_haptic.c` | An Awinic AW86927 vibration motor driver. | `haptic-aw86927` / `haptic=` | part; which pins it is on and the motor's calibration are data (`[haptic]`) |
| `bsd/dev/redwood_torch.c` | The flash LED module of a PM8350C. | `torch-pm8350c` / `torch=` | part; the arbiter and where the module is on the PMICs' bus are data (`[spmi]`, `[torch]`) |
| `bsd/dev/redwood_camera.c` | redwood's four cameras: sensors, CSI, ISP. | `camera-redwood` / `cam=` | part |
| `bsd/dev/redwood_virtio_sound.c` | QEMU's sound device as the sink. | QEMU only | any (QEMU) |

## Wi-Fi, network

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/redwood_wifi.c`, `redwood_wlfw.c` | A WCN6750 on the SoC's Wi-Fi DSP: firmware load, bring-up. | `wifi-wcn6750` / `wifi=1` | part; the DSP's memory, the radio's pins and its supplies' names are data (`[wifi]`) |
| `bsd/dev/redwood_lx.c`, `redwood_qmi.c`, `redwood_sta.c` | The kernel's side of the Linux Wi-Fi driver's object; QMI; the station boundary. | — | any |
| `bsd/dev/redwood_apple80211.c` | Apple80211 requests of iOS's Wi-Fi daemon, answered in the kernel. | — | any |
| `bsd/dev/redwood_net.c` | `en0`, an Ethernet interface as far as iOS can tell. | — | any |
| `bsd/dev/redwood_virtio_net.c` | QEMU's network device behind `en0`. | `virtio` / `virtio_net=1` | any (QEMU) |

## Storage, file systems

| File | What | Portability |
| --- | --- | --- |
| `bsd/miscfs/apfs/` | APFS: reads the iOS images in RAM; writes for the data volume. | any |
| `bsd/miscfs/ramfs/` | The RAM file system for `/private/var`. | any |
| `bsd/dev/redwood_image.c` | A file of one of the RAM disks by path, for drivers that need firmware before there is a root. | any |
| `bsd/dev/redwood_ufs.c`, `redwood_ufshc.c` | The phone's UFS: through the resident UEFI's driver, or the host controller directly (read only). | Through UEFI: any phone the loader runs on. Directly: where the controller is is data (`ufs` in `[platform]` of the SoC file), the controller is taken to be Qualcomm's as on redwood |
| `bsd/dev/redwood_sparse.c`, `redwood_virtio_blk.c`, `redwood_apfs_check.c` | An image with only some of its blocks; QEMU's block device; a walk of a volume for comparison with the host tools. | any |

## Which of them run on the host

These drivers are compiled unchanged into a host program and run against a model of their hardware
by `make check`, on the host and under qemu-aarch64. Changing one of them, or moving one of its
constants into the board file, can be tried without a phone. [tests/README.md](../tests/README.md)
says what each test covers and what it leaves out.

| Driver | Test | The hardware's model |
| --- | --- | --- |
| `hakios_board.c` | `board_test.c` | (none: the generated tables) |
| `hakios_backlight.c` | `backlight_driver_test.c` | the DSI controller and the display's clocks |
| `hakios_thermal.c` | `thermal_driver_test.c` | two TSENS controllers, three frequency domains |
| `hakios_cpufreq.c` | `cpufreq_driver_test.c` | the frequency block's tables |
| `hakios_ddr.c` | `ddr_driver_test.c` | RPMh's slot set, a command database |
| `hakios_buttons.c` | `buttons_driver_test.c` | the PMIC arbiter |
| `redwood_torch.c` | `torch_driver_test.c` | the PMIC arbiter with a flash module behind it |
| `redwood_battery.c` | `battery_driver_test.c` | the page the loader writes the battery's state into |
| `redwood_haptic.c` | `haptic_driver_test.c` | the SoC's pins with an I2C chip on them |
| `redwood_speaker.c` | `speaker_driver_test.c` | the pins with two amplifiers, a configuration file |
| `redwood_guard.c` | `guard_test.c` | (none: tones in, samples out) |
| `redwood_ufs.c` | `ufs_driver_test.c` | the loader on core 0: its page and buffer over a made-up disk, noting every write and whether the port may make it |
| `redwood_input.c` | `input_queue_test.c` | (none: the drivers' posts in, a program's reads and writes of `/dev/redwood-touch` out) |
| `redwood_sparse.c` | `sparse_driver_test.c` | the RAM disk's memory, with the image at its end as the build writes it and as it must not be believed |
| `redwood_goodix.c`, `renoir_fts.c` | `touch_goodix_wiring_test.c`, `touch_fts_wiring_test.c` | (none: only where each takes its wiring from) |

The two touch drivers are compiled into host programs too, but nothing of a controller is modelled:
their tests cover the wiring only. The sound, microphone, camera, Wi-Fi, DSP, GPU and storage drivers have no such test: they
are checked for syntax, types and the formats of what they print only (`kernel/syntax-check.sh`), and on the phone.

## Adding one

[writing-a-driver.md](writing-a-driver.md) is the guide: the rules a driver follows and why, the
headers it has at hand (the board's data, the log, registers, waits, the SoC's pins, the PMICs, RPMh),
and the steps, with an example driver and its host test that `make check` builds and runs.

In short: write the driver in `kernel/overlay/bsd/dev/` from the example, give it a feature and take
its addresses from the board, start it with a level that only reads, test it on the host against a
model of its hardware, add it to `bsd/conf/files` in an assembled tree and export
([kernel/README.md](../kernel/README.md)), and name its boot argument in a comment so that `make docs`
lists it.
