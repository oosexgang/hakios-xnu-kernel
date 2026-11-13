# Drivers

Every driver in the kernel: what it is for, what turns it on, and how much of it belongs to one
phone. Sources are in `kernel/overlay/`; each file starts with a long comment that is the real
documentation. Boot arguments are in [boot-args.md](boot-args.md).

**Portability** in the tables means:

| | |
| --- | --- |
| **any** | No device knowledge in the code. Works on any board, QEMU included. |
| **data** | Device knowledge comes from the phone's device package: its board file and its SoC file. A new phone is a new package. |
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
| `osfmk/arm64/a64emu.c` | The few arm64e instructions an ARMv8.2 core lacks, emulated when they trap. | any |
| `osfmk/arm64/prof.c`, `user_exception_trace.c` | A sampling profiler (`hakios_prof=NAME`) and crash descriptions (`systrace=`). | any |
| `iokit/Kernel/GenericARMPE.cpp` | The platform expert: CPUs, the time of day. | any |
| `bsd/dev/board/board.c` | Which board this is; `hw.board.*`. | data |
| `bsd/dev/board/drivers.c` | The list every driver below is started from, in order. | any |

## Stand-ins for Apple's closed kernel extensions

| File | Stands in for | Portability |
| --- | --- | --- |
| `iokit/Kernel/MiniIOMobileFramebuffer.cpp` | The display driver family: swaps, vsync, scaling to the panel, software dimming, the Metal plug-in gate (`metal=1`). | any; `fb_scan` timing is **data** (`display-scan`, `display { }` intf1) |
| `iokit/Kernel/MiniIOSurface.cpp` | IOSurface. | any |
| `iokit/Kernel/MiniAppleKeyStore.cpp` | AppleKeyStore: a device without a passcode. | any |
| `iokit/Kernel/MiniIONetworkStack.cpp`, `BoardWLAN.cpp` | The registry entries by which Darwin finds network interfaces. | any |
| `iokit/Kernel/BoardBattery.cpp` | The battery as Darwin looks for it (IOPMPowerSource). Fed by `board/battery.c` or the audio DSP. | any |
| `iokit/Kernel/BoardGPU.cpp` | The GPU for user space: the interface of Linux's msm DRM driver, as Mesa's freedreno speaks it. | any Adreno the kernel has started |
| `libkern/amfi/`, `bsd/kern/kern_minimac.c` | AMFI and Sandbox policies. | any |
| `osfmk/minicrypto/` | corecrypto. | any |

## Display, input

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/board/backlight.c` | Panel brightness: MIPI DCS through a Qualcomm DSI controller, or software. Automatic brightness. | `backlight { }` / `backlight=` | data |
| `bsd/dev/board/adreno.c` | Adreno 642/642L bring-up, step by step: clocks, GMU, zap shader, first frame. | `gpu-a660`, `gpu { }` / `gpu=1`, `adreno=` | data for a 642 or 642L (`gpu { }`, and `adreno { }` of the SoC file for where its blocks are); another Adreno generation: the driver |
| `bsd/dev/board/input.c` | `/dev/hakios-touch`: the queue of touches and button presses every source feeds. | — | any |
| `bsd/dev/board/goodix.c` | Goodix GT9916 or FocalTech FT3680 touch controller on an SPI serial engine. | `touch-goodix` / `touchscreen=1` | part; which engine, its clocks, the pins and the supply are data (`touch { }`) |
| `bsd/dev/board/fts.c` | FocalTech FT3658U touch controller on an SPI serial engine. | `touch-ft3658u` / `touchscreen=1` | part; which engine, its clocks, the pins and the supplies are data (`touch { }`) |
| `bsd/dev/board/buttons.c` | Power and volume keys on the PMIC, through the SPMI arbiter. | `buttons-pmic` / `buttons=1` | data (`spmi { }`, `buttons { }`); arbiter version 5 |
| `bsd/dev/board/virtio_input.c` | QEMU's tablet and keyboard as touch and buttons. | `virtio` / `virtio_input=1` | any (QEMU) |

## Power, CPU, temperature

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/board/cpufreq.c` | CPU frequency through EPSS. | `cpufreq-epss` / `cpufreq=1` | data (`cpufreq { }`) |
| `bsd/dev/board/scmi_perf.c` | CPU performance levels and limits through SCMI. | `cpufreq-scmi` / `scmi-perf=1` | data (`scmi-perf { }`) |
| `bsd/dev/board/ddr.c` | Memory bandwidth votes through RPMh. | `ddr-rpmh` / `ddr=` | data (`rpmh { }`), with the slot layout and BCM names of the 7-series |
| `bsd/dev/board/bench.c` | Measurements: how fast the cores get at memory, how busy each core is. | — / `membench=`, `cpuload=` | any |
| `bsd/dev/board/thermal.c` | Reads TSENS; holds CPU groups and the panel back when hot. | `thermal { }` / `thermal=` | data |
| `bsd/dev/board/battery.c` | The battery as the UEFI's charger drivers report it to the loader. | `battery-uefi` / `battery=1` | any phone whose loader provides it |
| `bsd/dev/board/cputop.c` | Which processes use the CPU (`cputop=S`). | — | any |

## Sound, camera, haptics, flashlight

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/board/adsp.c` | The audio DSP: started, and spoken to. Also carries the battery manager. | `adsp`, `adsp { }` / `adsp=1` | phone: the firmware directory, memory size and the gauge's units are data (`adsp { }`); the DSP's own addresses are the 7-series' |
| `bsd/dev/board/audio.c` | `/dev/hakios-audio`: what Darwin plays, to whichever sink is there. | — | any |
| `bsd/dev/board/sound.c`, `board/guard.c` | What the DSP is told so the AW88261 speakers play; speaker protection. | `speaker-aw88261` / `sound=N` with `adsp=1` | part; the sound's pins, its TDM interface and the DSP's stream are data (`speaker { }`, `smmu { }`) |
| `bsd/dev/board/mic.c` | The microphones on a WCD938x codec, through the DSP. | `mic-wcd938x` / `mic=N` with `adsp=1` | part; the codec's reset pin is data (`mic { }`), and so are the LPASS blocks and the SoundWire pins (`lpass { }`) |
| `bsd/dev/board/speaker.c` | Two Awinic AW88261 amplifiers on I2C. | `speaker-aw88261` / `speaker_probe=` | part; the bus's pins, each amplifier's address and interrupt and the sound's four pins are data (`speaker { }`) |
| `bsd/dev/board/haptic.c` | An Awinic AW86927 vibration motor driver. | `haptic-aw86927` / `haptic=` | part; which pins it is on and the motor's calibration are data (`haptic { }`) |
| `bsd/dev/board/torch.c` | The flash LED module of a PM8350C. | `torch-pm8350c` / `torch=` | part; the arbiter and where the module is on the PMICs' bus are data (`spmi { }`, `torch { }`) |
| `bsd/dev/board/camera.c` | cameras on an SM7325 as the board's `camera { }` wires them: sensors (four chips known), CSI, ISP. | `camera-sm7325` / `cam=` | part; each sensor's wiring and supplies, and which is Darwin's back and front camera, are data (`camera { }`), as are where the camera's clocks are in its clock controller and how its PLLs are configured (the SoC file's `camera { }`) |
| `bsd/dev/board/virtio_sound.c` | QEMU's sound device as the sink. | QEMU only | any (QEMU) |

## Wi-Fi, network

Bluetooth first stage (`board/bt_uart.c`) is opt-in: absent/`bt=0` performs no mapping or register access;
`bt=-1` inspects without power, pin, clock or UART writes (SPMI reads use arbiter command registers).
`bt=1` inspects first, powers the controller, opens 115200 8N1 H4 with CTS/RTS, sends HCI Reset and
Qualcomm EDL_PATCH_GETVER, and logs raw events plus product/patch/ROM/SoC versions when recognized.
Every poll has a bound. Failure powers down; loader return lowers BT_EN, stops UART commands, restores
pins/RCG and removes only the GCC bits it added, then drops RF and rail enable votes in reverse order.
Use dedicated Bluetooth VRM resources; the helper cannot recover another client's prior RPMh voltage vote.
There is no firmware download, baud switch, Darwin interface or claim of a working Bluetooth stack.

Validated against host register models only, including both wrapper sampling rates and H4 event forms.
SM8845's Android DT describes a v8 SPMI arbiter: the existing shared helper supports v5/v6 and rejects
v7 and later before any PMIC transfer. Its map/owner layout needs shared-helper support before this
board can power BT_EN. UART/PMIC/HCI register facts follow the sources named in the driver and board
comments; hardware behavior and the controller's actual version-event variant remain unverified.


| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/board/bt_uart.c` | Qualcomm Bluetooth power and H4 identification only. | `bluetooth-wcn-uart` / `bt=-1`, `bt=1` | data (`bluetooth { }`, `pins { }`, `clocks { }`, `rpmh { }`, `spmi { }`); shared SPMI v5/v6 |
| `bsd/dev/board/wifi.c`, `board/wlfw.c` | A WCN6750 on the SoC's Wi-Fi DSP: firmware load, bring-up. | `wifi-wcn6750` / `wifi=1` | part; the DSP's memory, the radio's pins and its supplies' names are data (`wifi { }`) |
| `bsd/dev/board/lx.c`, `board/qmi.c`, `board/sta.c` | The kernel's side of the Linux Wi-Fi driver's object; QMI; the station boundary. | — | any |
| `bsd/dev/board/apple80211.c` | Apple80211 requests of Darwin's Wi-Fi daemon, answered in the kernel. | — | any |
| `bsd/dev/board/net.c` | `en0`, an Ethernet interface as far as Darwin can tell. | — | any |
| `bsd/dev/board/virtio_net.c` | QEMU's network device behind `en0`. | `virtio` / `virtio_net=1` | any (QEMU) |

## Storage, file systems

| File | What | Portability |
| --- | --- | --- |
| `bsd/miscfs/apfs/` | APFS: reads the Darwin images in RAM; writes for the data volume. | any |
| `bsd/miscfs/ramfs/` | The RAM file system for `/private/var`. | any |
| `bsd/dev/board/image.c` | A file of one of the RAM disks by path, for drivers that need firmware before there is a root. | any |
| `bsd/dev/board/ufs.c`, `board/ufshc.c` | The phone's UFS: through the resident UEFI's driver, or the host controller directly (read only). | Through UEFI: any phone the loader runs on. Directly: where the controller is is data (`ufs` in `platform { }` of the SoC file), the controller is taken to be Qualcomm's, as on the SM7325 and SM7350 |
| `bsd/dev/board/sparse.c`, `board/virtio_blk.c`, `board/apfs_check.c` | An image with only some of its blocks; QEMU's block device; a walk of a volume for comparison with the host tools. | any |

## Which of them run on the host

These drivers are compiled unchanged into a host program and run against a model of their hardware
by `make check`, on the host and under qemu-aarch64. Changing one of them, or moving one of its
constants into the board file, can be tried without a phone. [workbench/README.md](../workbench/README.md)
says what each test covers and what it leaves out.

| Driver | Test | The hardware's model |
| --- | --- | --- |
| `board/board.c` | `board_test.c` | (none: the generated tables) |
| `board/backlight.c` | `backlight_driver_test.c` | the DSI controller and the display's clocks |
| `board/thermal.c` | `thermal_driver_test.c` | two TSENS controllers, three frequency domains |
| `board/cpufreq.c` | `cpufreq_driver_test.c` | the frequency block's tables |
| `board/ddr.c` | `ddr_driver_test.c` | RPMh's slot set, a command database |
| `board/buttons.c` | `buttons_driver_test.c` | the PMIC arbiter |
| `board/torch.c` | `torch_driver_test.c` | the PMIC arbiter with a flash module behind it |
| `board/battery.c` | `battery_driver_test.c` | the page the loader writes the battery's state into |
| `board/haptic.c` | `haptic_driver_test.c` | the SoC's pins with an I2C chip on them |
| `board/speaker.c` | `speaker_driver_test.c` | the pins with two amplifiers, a configuration file |
| `board/guard.c` | `guard_test.c` | (none: tones in, samples out) |
| `board/ufs.c` | `ufs_driver_test.c` | the loader on core 0: its page and buffer over a made-up disk, noting every write and whether the port may make it |
| `board/ufshc.c` (its whitelist) | `ufs_whitelist_test.c` | (none: commands made up and never sent) |
| `board/scmi_perf.c` | `scmi_perf_test.c` | the firmware behind the SCMI channel: its shared memory and doorbell, answering, late, wrongly or not at all |
| `board/audio.c` | `audio_device_test.c` | (none: a program's writes and volume in, a sink and the recording's memory out) |
| `board/sta.c` | `station_test.c` | the radio's driver, built outside this tree: power, scan, join and leave noted, answered as a scenario says, and saying more than there is room for |
| `board/apple80211.c` | `apple80211_test.c` | the station layer: its networks, its state and what was asked of it, with records that say more than there is room for |
| `board/camera.c` (its sensors, its PMIC GPIO) | `camera_wiring_test.c` | the PMIC arbiter with the PM8008i's reset GPIO behind it (`spmi_model.h`); otherwise only where it takes its sensors from |
| `board/sound.c` (its pins, the SMMU, the stream's events) | `sound_driver_test.c` | the pin controller's registers, with pins that toggle; the apps SMMU's, with what the boot chain set up |
| `board/mic.c` (its blocks, pins, DSP, SoundWire, codec) | `mic_driver_test.c` | registers that keep what is written; the DSP as a script of answers; the TX SoundWire master's link and command FIFO, with the codec's registers behind it |
| `board/input.c` | `input_queue_test.c` | (none: the drivers' posts in, a program's reads and writes of `/dev/hakios-touch` out) |
| `board/sparse.c` | `sparse_driver_test.c` | the RAM disk's memory, with the image at its end as the build writes it and as it must not be believed |
| `board/goodix.c`, `board/fts.c` | `touch_goodix_wiring_test.c`, `touch_fts_wiring_test.c` | (none: only where each takes its wiring from) |

The two touch drivers, the camera's, the sound's and the microphones' are compiled into host programs too, but
nothing of their parts is modelled: their tests cover the wiring only. The Wi-Fi, DSP, GPU and storage drivers have no such test: they
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
