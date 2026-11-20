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
| `iokit/Kernel/BoardBattery.cpp` | The battery as Darwin looks for it (IOPMPowerSource). Fed by `board/battery.c` or the audio DSP. Says how long the charge lasts too (below). | any |
| `iokit/Kernel/BoardGPU.cpp` | The GPU for user space: the interface of Linux's msm DRM driver, as Mesa's freedreno speaks it. | any Adreno the kernel has started |
| `libkern/amfi/`, `bsd/kern/kern_minimac.c` | AMFI and Sandbox policies. | any |
| `osfmk/minicrypto/` | corecrypto. | any |

These are reached by any program: none of their user clients asks for an entitlement (the kernel has no IOKit MAC
hook). What a program gives them is held to what it is: a surface's rows are drawn only where they lie in its memory
(`MiniIOSurface::holds()`), a GPU buffer's size is checked before it is rounded, a client that is closed makes nothing
more. **Known, not changed:** a surface is found by its number by anybody, and its pixels mapped for writing (IOSurface's
lookup, and the GPU's buffer made of a surface): a program can read and change another's pictures, as on Darwin with
global surfaces.

## Display, input

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/board/backlight.c` | Panel brightness: MIPI DCS through a Qualcomm DSI controller, or software. Automatic brightness. | `backlight { }` / `backlight=` | data |
| `bsd/dev/board/adreno.c` | Adreno 642/642L bring-up, step by step: clocks, GMU, zap shader, first frame. | `gpu-a660`, `gpu { }` / `gpu=1`, `adreno=` | data for a 642 or 642L (`gpu { }`, and `adreno { }` of the SoC file for where its blocks are); another Adreno generation: the driver |
| `bsd/dev/board/input.c` | `/dev/hakios-touch`: the queue of touches and button presses every source feeds. | — | any |
| `bsd/dev/board/goodix.c` | Goodix GT9916 or FocalTech FT3680 touch controller on an SPI serial engine. | `touch-goodix` / `touchscreen=1`; `touchscreen=-1` only looks | part; which engine, its clocks, the pins and the supply are data (`touch { }`) |
| `bsd/dev/board/fts.c` | FocalTech FT3658U touch controller on an SPI serial engine. | `touch-ft3658u` / `touchscreen=1`; `touchscreen=-1` only looks | part; which engine, its clocks, the pins and the supplies are data (`touch { }`) |
| `bsd/dev/board/ft5x06.c` | FocalTech touch controller of the FT5x06 register family (FT5x06, FT5x46, FT6x36 ...) on I2C, the bus driven by hand on two of the SoC's pins. Reads only: nothing is written into the chip, no firmware is uploaded (a chip without its own flash, an FT8719, is not this driver's). Not run on a phone. | `touch-ft5x06`, `touch-i2c { }` / `touchscreen=1`; `touchscreen=-1` only looks (the pins, the chip's identity); `ft5x06_steps=N` | part; its pins, address, supplies, fingers and coordinates are data (`touch-i2c { }`) |
| `bsd/dev/board/buttons.c` | Power and volume keys on the PMIC, through the SPMI arbiter. | `buttons-pmic` / `buttons=1` | data (`spmi { }`, `buttons { }`); arbiter versions 5, 7 and 8 ([chipsets.md](chipsets.md)) |
| `bsd/dev/board/rtc.c` | The PMIC's real-time clock, read once at boot and never written: `hw.rtc_seconds`, which process 1 keeps the time of day by across boots without the host (`/private/var/db/rtc-offset`). | `rtc { }` / on unless `rtc=0` | data (`spmi { }`, `rtc { }`); the RTC_RW block of the PM8941's layout (PM8998, PM8150, PM8250) and the PMK8350's |
| `bsd/dev/board/virtio_input.c` | QEMU's tablet and keyboard as touch and buttons. | `virtio` / `virtio_input=1` | any (QEMU) |
| `bsd/dev/board/imu.c` | An ST LSM6DSO or a Bosch BMI160 accelerometer and gyroscope on I2C (the board's feature says which chip; the BMI160's part is from its datasheet as remembered and has not met a chip): the phone's orientation, `hw.sensors.*`, which backboardd's library gives to Darwin as an iPhone's always-on processor does (`userland/userland/src/hid_orientation.h`; `orientation_hid=0`: not), and its samples, about 10 a second, to Darwin's motion stack as an iPhone's motion processor does (`userland/userland/src/hid_motion.h`; `motion_hid=0`: not), with the gyroscope's rates in the same samples, from a gyroscope service of their own (`userland/userland/src/hid_gyro.h`; the gyroscope is switched on only for that, and `gyro_hid=0` keeps it powered down). | `imu-lsm6dso` or `imu-bmi160` / `sensors=` | part; which pins it is on, its address, its range and how it is turned on the board are data (`imu { }`); each chip's facts are a header of its own, `board/lsm6dso_core.h` and `board/bmi160_core.h`, and a row of the driver's table of chips; the turn and the orientation are `board/sensor_axes_core.h`, `board/sensors_core.h` |

## Power, CPU, temperature

| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/board/cpufreq.c` | CPU frequency through EPSS (SM8350 and later) or the OSM's firmware before it (SDM845 to SM8250). | `cpufreq-epss` / `cpufreq=1` | data (`cpufreq { }`) |
| `bsd/dev/board/scmi_perf.c` | CPU performance levels and limits through SCMI. | `cpufreq-scmi` / `scmi-perf=1` | data (`scmi-perf { }`) |
| `bsd/dev/board/ddr.c` | Memory bandwidth votes through RPMh. | `ddr-rpmh` / `ddr=` | data (`rpmh { }`), with the slot layout and BCM names of the 7-series |
| `bsd/dev/board/bench.c` | Measurements: how fast the cores get at memory, how busy each core is. | — / `membench=`, `cpuload=` | any |
| `bsd/dev/board/thermal.c` | Reads TSENS; holds CPU groups and the panel back when hot. | `thermal { }` / `thermal=` | data |
| `bsd/dev/board/battery.c` | The battery as the UEFI's charger drivers report it to the loader. | `battery-uefi` / `battery=1` | any phone whose loader provides it |
| `bsd/dev/board/cputop.c` | Which processes use the CPU (`cputop=S`). | — | any |

The frequency table of a group is read as Linux's `qcom-cpufreq-hw.c` reads it for the source and the multiplier, up
to the row it repeats, in either of its layouts (`drivers/board/cpufreq_hw.h`: EPSS's, and the OSM's with its rows 32
bytes apart, its row register at 0x920 and its limit at 0x704); the row register is held inside the table, whatever it
reads, and a group whose enable register (0x0) says the hardware is not in charge of its clock is left alone, as
Linux leaves it. **Not done yet**, for want of a phone to see them on: leaving out the rows the table's core-count field
marks as turbo (Linux does, and keeps the last one as a boost it does not use by default) -- the log says each
group's counts (`cpufreq: group N: the rows' core counts ...`), so that a phone's run tells whether its top rows are
such -- and per-core DCVS (0xb0: a group
whose cores each vote, where only the first is written here). `cpufreq=1` raises every group to its top row and
leaves it there for the loader; `scmi_perf=2` asks its domains back to the levels they were found at, and `ddr=` gives
its vote back, at the run's end.

**How long the charge lasts.** Darwin's power source says it in minutes, as a fuel gauge of Apple's does:
`TimeRemaining` (-1 while not known), `AvgTimeToEmpty` and `AvgTimeToFull` (65535 for the one that is not happening
or not known). `board/battery.c` works the minutes out of every reading it hands on (`board/battery_core.h`):

| | |
| --- | --- |
| the current | averaged over about a minute, each reading weighed by the time since the one before; begun anew when a charger is put in or pulled out, and when the source has said nothing for more than a minute |
| to empty | with no charger: the charge in the battery at the average current out of it |
| to full | while it charges: what is missing to the full charge at the average current into it, 0 once the gauge counts it full. "At this rate" -- the current falls as the cell fills, and the figure is corrected as it does |
| the charge | the gauge's count in uAh; without one, the full charge it has learnt at the percentage shown |
| nothing said | for the first half minute of an average; under 20 mA either way; on a port and not charging; with no battery; when no charge in uAh is known -- the loader's cache alone (`battery-uefi` without `adsp=1`) has a percentage and no charge, so a phone with only that is never told a time; beyond 65534 minutes |

`kern.hakios.battery_minutes` (-1 not known, -2 switched off) and `kern.hakios.battery_average_ma` (more than 0 into
the battery) have what was worked out, and the log says it once each time the charge turns (`battery: at -412 mA on
average, 371 min until it is empty`). `battery_time=0` works nothing out and leaves the three keys off the power
source, as it was before. After the idea of the OnePlus Nord 2 port's piece 0701. **Not run on a phone**: that powerd
of Darwin 26 and 27 takes its time to empty and to full charge from these keys is what the port found on iOS 16 and
has not been seen here; `battery_test` (userland) prints the power source's properties and powerd's list side by
side to see it by.

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
`bt=-1` inspects without power, pin, clock or UART writes (SPMI reads use arbiter command registers), and reads the
serial engine's registers only if its clocks already run: a read of a block whose clock is off can hang the bus.
`bt=1` inspects first, powers the controller, opens 115200 8N1 H4 with CTS/RTS, sends HCI Reset and
Qualcomm EDL_PATCH_GETVER, and logs raw events plus product/patch/ROM/SoC versions when recognized.
Every poll has a bound. Failure powers down; loader return lowers BT_EN, stops UART commands, restores
pins/RCG and removes only the GCC bits it added, then drops RF and rail enable votes in reverse order.
Use dedicated Bluetooth VRM resources; the helper cannot recover another client's prior RPMh voltage vote.
`bt=2` does all of `bt=1` and then hands the controller to Darwin (`board/bt_host.c`): the IORegistry service
`bluetooth` with the address the loader put into `/chosen mac-address-bluetooth0` and transport-encoding 3 (H4BC);
`/dev/btwake`, `/dev/btreset` and `/dev/btpoweroff`; a Skywalk kernel pipe; and the kernel control
`com.apple.uart.sk.bluetooth`, whose socket options give bluetoothd the channel's UUID and keep its termios. A thread
carries whole H4 packets between the channel and the UART (115200 baud, packets longer than the FIFO written as it
empties; a stray byte or a packet cut short throws away what follows until the line is quiet, 50 ms at most, to be in
step again; a packet that goes out in part has its command cancelled, and the controller is sent nothing more).
Broadcom's and Apple's vendor commands, Default Erroneous Data Reporting and the LE periodic advertiser list size are
answered in the kernel; the controller's own vendor events are not handed on; Read Local Supported Commands has the
LE commands bluetoothd counts on said (`board/bt_hci.h`, `bt_quirks=`; after HakiOS's MediaTek port and the OnePlus
Nord 2 port of iOS built on it, APSL 2.0). Before it is handed over, the controller is given the firmware
`bluetooth { }` names -- `rampatch` (the patch to its ROM, a TLV `.tlv` or an ELF `.mbn`) and `nvm` (its
configuration), taken from the phone's own `vendor/firmware` into the small image's `firmware` directory -- in segments
of 243 bytes, each answered as the patch's download mode says and the last always, then HCI_Reset (`board/bt_qca.h`;
Qualcomm's protocol as Linux's `btqca.c` speaks it). The configuration is sent as it is: no in-band sleep, no faster
UART. Without the names, or with `bt_fw=0`, the controller runs its ROM. There is no baud switch, and none of this has
been seen on a phone yet. [bluetooth.md](bluetooth.md) has all of it.

Validated against host register models only, including both wrapper sampling rates and H4 event forms.
SM8845's Android DT describes a v8 SPMI arbiter: the shared helper (`board/spmi.h`) supports v5/v6, v7 (either of its
two buses, `spmi { bus }`) and v8 where the SoC file has `spmi { channel-map }`, and rejects any other before any
PMIC transfer. The v7 and v8 layouts have not been run on a phone. UART/PMIC/HCI register facts follow the sources named in the driver and board
comments; hardware behavior and the controller's actual version-event variant remain unverified.

Wi-Fi (`wifi=1`) shows the radio its memory through the SMMU before the DSP is started, and takes it away at every
ending: the DSP shut down, a run that ended before the DSP was started (the next kernel's `hakios_wifi_quiesce`), a
bring-up whose memory ran out. When TrustZone refuses to shut the DSP down, the radio's stream is sent to fault rather
than left translating into memory that is the next owner's. When the driver's start gives up (60 s) or the run ends
before it is up, its rings' thread is stopped and the registers closed before the DSP goes. A
frame Darwin sends (`lx_station_send`) and a frame received are counted while they use the station (`station_use`), and
the station's leaving (`station_leave`, the network lost or the run's end) waits, at most a second, for those counted
before it frees the station and its key; none starts after. The count takes no lock, so the leaving still waits for the
driver's work (`cancel_work_sync`) as before.


| File | Part | Feature / boot argument | Portability |
| --- | --- | --- | --- |
| `bsd/dev/board/bt_uart.c` | Qualcomm Bluetooth power, H4 identification, and the controller's packets through the UART. | `bluetooth-wcn-uart` / `bt=-1`, `bt=1`, `bt=2` | data (`bluetooth { }`, `pins { }`, `clocks { }`, `rpmh { }`, `spmi { }`); shared SPMI v5/v6 |
| `bsd/dev/board/bt_host.c`, `bt_hci.h`, `iokit/Kernel/BoardBluetooth.cpp` | Darwin's side of Bluetooth: bluetoothd's service, nodes, Skywalk channel and kernel control; what is answered or changed between it and the controller. | `bt=2`, `bt_quirks=` | any controller a driver hands over (`include/board/bt.h`) |
| `bsd/dev/board/wifi.c`, `board/wlfw.c` | A WCN6750 on the SoC's Wi-Fi DSP: firmware load, bring-up. | `wifi-wcn6750` / `wifi=1` | part; the DSP's memory, the radio's pins and its supplies' names are data (`wifi { }`) |
| `bsd/dev/board/lx.c`, `board/qmi.c`, `board/sta.c` | The kernel's side of the Linux Wi-Fi driver's object; QMI; the station boundary. | — | any |
| `bsd/dev/board/apple80211.c` | Apple80211 requests of Darwin's Wi-Fi daemon, answered in the kernel. | — | any |
| `bsd/dev/board/net.c` | `en0`, an Ethernet interface as far as Darwin can tell. | — | any |
| `bsd/dev/board/mpss.c`, `board/ipa.c`, `board/nas.c`, `board/cell.c` | The modem: started through TrustZone, its file systems served from the flash, the SIM and the network's registration, the IPA's data path. | `modem-sm7325` / `modem=1` | part |
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
| `board/rtc.c` | `rtc_driver_test.c` | the PMIC arbiter with a clock's block behind it |
| `board/torch.c` | `torch_driver_test.c` | the PMIC arbiter with a flash module behind it |
| `board/battery.c` | `battery_driver_test.c` | the page the loader writes the battery's state into |
| `board/haptic.c` | `haptic_driver_test.c` | the SoC's pins with an I2C chip on them |
| `board/imu.c` | `imu_driver_test.c` | the SoC's pins with an LSM6DSO on them, or a BMI160 |
| `board/speaker.c` | `speaker_driver_test.c` | the pins with two amplifiers, a configuration file |
| `board/guard.c` | `guard_test.c` | (none: tones in, samples out) |
| `board/ufs.c` | `ufs_driver_test.c` | the loader on core 0: its page and buffer over a made-up disk, noting every write and whether the port may make it |
| `board/ufshc.c` (its whitelist) | `ufs_whitelist_test.c` | (none: commands made up and never sent) |
| `board/ufshc.c` (its reads) | `ufs_native_read_test.c` | the controller's registers and slot 0, a thread that answers its doorbell |
| `board/scmi_perf.c` | `scmi_perf_test.c` | the firmware behind the SCMI channel: its shared memory and doorbell, answering, late, wrongly or not at all |
| `board/audio.c` | `audio_device_test.c` | (none: a program's writes and volume in, a sink and the recording's memory out) |
| `board/sta.c` | `station_test.c` | the radio's driver, built outside this tree: power, scan, join and leave noted, answered as a scenario says, and saying more than there is room for |
| `board/apple80211.c` | `apple80211_test.c` | the station layer: its networks, its state and what was asked of it, with records that say more than there is room for |
| `board/lx.c` (the radio's link: its attach, the queue of frames to send) | `lx_test.c` | the network stack, refusing the link as a scenario says |
| `board/camera.c` (its sensors, its PMIC GPIO, the PM8008i's writes, its tables) | `camera_wiring_test.c` | the PMIC arbiter with the PM8008i's reset GPIO behind it (`spmi_model.h`); a sensor's table as the small image would hold it; otherwise where it takes its sensors from |
| `board/sound.c` (its pins, the SMMU, the stream's events) | `sound_driver_test.c` | the pin controller's registers, with pins that toggle; the apps SMMU's, with what the boot chain set up |
| `board/wifi.c` (the radio's SMMU match, the doorbells, GLINK's channel, the DSP's text, its giving up) | `wifi_driver_test.c` | the apps SMMU's registers, as the boot chain or a kernel before left them; IPCC's doorbells; the shared memory with GLINK's queues and the DSP's items, the DSP's side played by the test |
| `board/mic.c` (its blocks, pins, DSP, SoundWire, codec) | `mic_driver_test.c` | registers that keep what is written; the DSP as a script of answers; the TX SoundWire master's link and command FIFO, with the codec's registers behind it |
| `board/input.c` | `input_queue_test.c` | (none: the drivers' posts in, a program's reads and writes of `/dev/hakios-touch` out) |
| `board/sparse.c` | `sparse_driver_test.c` | the RAM disk's memory, with the image at its end as the build writes it and as it must not be believed |
| `board/goodix.c`, `board/fts.c` (their wiring) | `touch_goodix_wiring_test.c`, `touch_fts_wiring_test.c` | (none: only where each takes its wiring from) |
| `board/goodix.c`, `board/fts.c` (their reports), `board/fts_protocol.h` | `touch_reports_test.c`, `touch_fts_reports_test.c` | (none: a controller's reports in, touches out) |
| `board/fts.c` (its thread: bring-up, polling, powering off) | `touch_fts_thread_test.c` | the controller behind its SPI transfer (its identity, a report, reads that fail, slowly or at once, or never end); the pins' and the clock controller's registers; RPMh and its command database |
| `board/ft5x06.c`, `board/ft5x06_report.h` | `touch_ft5x06_test.c` | the pins' registers with the controller on two of them as an I2C device (`i2c_device_model.h`): its identity, its mode, reports it puts out whole; it answers only powered and out of reset, and not at once; it can fall silent, hang until reset, or hold the bus; RPMh and its command database |
| `board/geni_spi.h` (the touch drivers' SPI) | `geni_spi_test.c` | the serial engine in its FIFO mode, with a device that answers each byte, and that can say an error, stay busy or take no cancel |

The two SPI touch drivers, the camera's, the sound's and the microphones' are compiled into host programs too, but
not all of their parts are modelled: the touch drivers' tests cover their wiring and what they make of a
controller's reports, and the FocalTech driver's thread against a model of its controller, not the talk on the
wire (the I2C touch driver's test has the wire too, bit by bit, but its chip is a model written from the same
memory of the registers as the driver: neither has met a controller); the others' cover their wiring, and the sound's and the microphones' the rings Darwin writes and reads. The
Wi-Fi driver's test calls parts of it one at a time, with no TrustZone and no DSP that runs. The other DSP drivers,
the GPU's and the storage's have no such test: they are checked for syntax, types and the formats of what
they print only (`kernel/syntax-check.sh`), and on the phone.

## Adding one

[writing-a-driver.md](writing-a-driver.md) is the guide: the rules a driver follows and why, the
headers it has at hand (the board's data, the log, registers, waits, the SoC's pins, the PMICs, RPMh),
and the steps, with an example driver and its host test that `make check` builds and runs.

In short: write the driver in `drivers/board/` from the example, give it a feature and take
its addresses from the board, start it with a level that only reads, test it on the host against a
model of its hardware, add it to `bsd/conf/files` in an assembled tree and export
([kernel/README.md](../kernel/README.md)), and name its boot argument in a comment so that `make docs`
lists it.
