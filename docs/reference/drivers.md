<!-- Made by tools/docs-gen.py from drivers/*/conf and the SoC and device repositories' drivers. Do not edit: change that, then run tools/docs-gen.py. -->


# Drivers

Every driver the kernel can be built with. `CONFIG_HAKIOS_<NAME>=y` in a defconfig builds it in; [Drivers in the configuration](../drivers-config.md) says how, and [Writing a driver](../porting/drivers.md) how to add one.

## Always built

| File | What |
|---|---|
| `hakios.audio.c` | /dev/hakios-audio, the sound Darwin plays, to whatever plays it; and where the camera, flashlight, motor and speaker drivers are started from |
| `hakios.version.c` | kern.hakios.version, and the version in the boot log |
| `hakios.platform.c` | what the drivers share (the locks of the PMIC arbiter, the always-on processor, RPMh and the SMMU), and kern.hakios.drivers, the list of the drivers built in |
| `hakios.stubs.c` | what the rest of the kernel calls in a driver that is left out: nothing, and "not there" |
| `hakios.stubs.cpp` | the same, for the calls that are C++ (the display asking whether the GPU is done with a surface) (with XNU's `iokitcpp`) |
| `hakios.input.c` | /dev/hakios-touch, the queue of touches and buttons, and where the phone's drivers are started from |
| `hakios.net.c` | en0, an interface that is Ethernet to the rest of the kernel, for whatever link carries its frames |
| `hakios.image.c` | a file of one of the RAM disks by path, for the drivers that load firmware before there is a root |
| `hakios.sparse.c` | /dev/rsp0, a block device over an image that holds only some of its blocks (apfs_sparse=) |
| `hakios.apfs_check.c` | a walk of the APFS volume on /dev/rvblk0 through VFS, to compare with tools/apfs/apfs.py (with XNU's `apfs`) |

## audio

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_ADSP`**<br>the audio DSP started and linked (adsp=1, adsp_step=N); the battery's state comes through it too. Needs the DSP's firmware from the phone (fw-audio/) | `hakios.adsp.c` |  | `adsp=`, `adsp_again=`, `adsp_apr_wait=`, `adsp_keep=`, `adsp_listen=`, `adsp_new=`, `adsp_pause=`, `adsp_return=` and 3 more |  | built | built | |
| **`HAKIOS_SPEAKER`**<br>the two speaker amplifiers on I2C, and their watch of temperature and battery | `hakios.speaker.c` |  | `speaker_probe=` |  | built |  | |
| **`HAKIOS_SOUND`**<br>the speakers playing (sound=N): what the DSP's services are told, the feeder, and the speakers' guard that limits what the amplifiers get.  The amplifiers themselves are a driver of the phone's (SPEAKER: redwood's AW88261; SPACEWAR_SPEAKER: spacewar's TFA98xx) and provide hakios_speaker_*. | `hakios.sound.c`<br>`hakios.guard.c` | `HAKIOS_ADSP` | `sound=`, `sound_below=`, `sound_gain=`, `sound_guard_as=`, `sound_pause=`, `sound_secs=` |  | built | built | |
| **`HAKIOS_MIC`**<br>the microphones, through the audio DSP (mic=N) | `hakios.mic.c` | `HAKIOS_ADSP`, `HAKIOS_SOUND` | `mic=`, `mic_adc=`, `mic_input=`, `mic_main=`, `mic_secs=`, `mic_tap=`, `mic_test=` |  | built |  | |
| **`HAKIOS_VIRTIO_SOUND`**<br>QEMU's virtio sound device as the player of /dev/hakios-audio (virtio_sound=1). QEMU only | `hakios.virtio_sound.c` |  | `virtio_sound=` | built | built |  | |

## bluetooth

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BT`**<br>the Bluetooth half of the WCN6750: step one, a read-only look at its three pins (bt=1) | `hakios.bt.c` |  | `bt=`, `bt_again=` |  | built | built | |

## camera

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_CAMERA`**<br>the cameras (cam=N): the sensors on the CCI bus and the ISP's interface. The sensors, their tables (fw-camera/) and the pins are the POCO X5 Pro's | `hakios.camera.c` |  | `cam_after=`, `cam_fake=`, `cam_fake_fps=`, `cam_frames=`, `cam_gain=`, `cam_keep_mb=`, `cam_mode=`, `cam_pattern=` and 4 more |  | built |  | |
| **`HAKIOS_TORCH`**<br>the flashlight: the flash LED module of the PM8350C (torch=1); it stays off while the battery is too warm | `hakios.torch.c` |  | `torch_max_s=`, `torch_secs=` |  | built |  | |

## core

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SOCINFO`**<br>the chip's identity from the firmware's shared memory: kern.hakios.soc_* (socinfo=1). Any Qualcomm SoC that keeps SMEM item 137; where the shared memory is comes from soc_smem= | `hakios.socinfo.c` |  | `soc_smem=`, `soc_smem_size=`, `socinfo=` |  | built | built | |

## gpu

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_ADRENO`**<br>the Adreno 642L of the SC7280 woken step by step (adreno=N), and HakiosGPU, the IOKit service Mesa, Metal and the window server draw through (gpu=1, adreno_keep=1; docs/reference/metal.md). Needs the GPU's firmware from the phone (fw-gpu/) | `hakios.adreno.c`<br>`hakios.gpu.cpp` |  | `adreno=`, `adreno_blind=`, `adreno_dirty=`, `adreno_fine=`, `adreno_keep=`, `adreno_only=`, `adreno_pause=`, `adreno_return=` and 10 more |  | built |  | |

## haptic

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_HAPTIC`**<br>the vibration motor: an Awinic AW86927 driving a linear resonant actuator (haptic=N), its waveforms from fw-haptic/ | `hakios.haptic.c` |  | `haptic=`, `haptic_effect=` |  | built |  | |

## input

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_INPUT`**<br>QEMU's virtio tablet and keyboard as the touch screen and the buttons (virtio_input=1). QEMU only | `hakios.virtio_input.c` |  | `virtio_input=` | built | built |  | |
| **`HAKIOS_GOODIX`**<br>the touch screen: a Goodix GT9916 ("Berlin-D") or a FocalTech FT3680 on SPI (touchscreen=1). The bus, pins and supplies are the POCO X5 Pro's; another phone with one of these controllers changes those | `hakios.goodix.c` |  | `touch_flip=`, `touch_poll_us=`, `touch_rate=`, `touch_spi_hz=`, `touchscreen=` |  | built |  | |
| **`HAKIOS_BUTTONS`**<br>the power and volume keys, read from the PMIC through the SPMI arbiter (buttons=1) | `hakios.buttons.c` |  | `buttons=`, `buttons_steps=`, `press_power=`, `press_power_ms=`, `touchscreen=` |  | built | built | |

## modem

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_QMI`**<br>QMI messages over the IPC router: how the Wi-Fi firmware's service and the modem's are spoken to | `hakios.qmi.c` |  |  |  | built | built | |
| **`HAKIOS_MODEM`**<br>the modem (modem=1): started, its file systems served from the flash, the IPA's data path as en1, the SIM and the network registration. Research; needs fw-modem/ from the phone | `hakios.mpss.c`<br>`hakios.cell.c`<br>`hakios.nas.c`<br>`hakios.ipa.c` | `HAKIOS_QMI`, `HAKIOS_UFS` | `modem=`, `modem_again=`, `modem_apn=`, `modem_buffer=`, `modem_data_nobind=`, `modem_data_off=`, `modem_data_wait=`, `modem_dpm_off=` and 45 more |  | built | built | |

## net

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_NET`**<br>QEMU's virtio network device as the link behind en0 (virtio_net=1). QEMU only | `hakios.virtio_net.c` |  | `virtio_net=` | built | built |  | |

## power

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BATTERY`**<br>the battery: kern.hakios.battery_*, and HakiosBattery, the IOPMPowerSource Darwin reads the charge from (the state comes from the audio DSP's battery manager, with adsp=1) | `hakios.battery.c`<br>`hakios.battery.cpp` |  | `battery_shared_pa=` |  | built | built | |
| **`HAKIOS_CPUFREQ`**<br>the clock of the cores the kernel runs on (cpufreq=1), and the memory bus's votes (ddr=) | `hakios.cpufreq.c` |  | `cpufreq=`, `cpuload=`, `ddr=`, `ddr_rest=`, `ddr_vote=`, `membench=` |  | built |  | |
| **`HAKIOS_THERMAL`**<br>the SoC's temperature sensors (thermal=1): kern.hakios.thermal_*, and the run handed back to the loader when the phone gets too hot (thermal_stop=) | `hakios.thermal.c` |  | `thermal=`, `thermal_log=`, `thermal_poll=`, `thermal_stop=`, `thermal_tm0=`, `thermal_tm1=` |  | built | built | |
| **`HAKIOS_PMIC`**<br>the blocks of the power-management chips the SPMI arbiter shows, and the PMIC's clock (pmic=1); read only | `hakios.pmic.c` |  | `pmic=`, `pmic_list=` |  | built | built | |
| **`HAKIOS_BATTGUARD`**<br>the run handed back to the loader before the battery is flat (battery_stop=<percent>) | `hakios.battguard.c` |  | `battery_stop=`, `battery_stop_poll=` |  | built | built | |

## storage

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_UFS`**<br>the phone's UFS flash: a block device for each GPT partition (ufs=1, ufs=2), and the UFS host controller driven from the kernel (ufs_native=). THE ONLY CODE OF THE KERNEL THAT MAY WRITE TO THE PHONE (ufs_write=1, the partition `darwin` only): see tools/provenance.allow | `hakios.ufs.c`<br>`hakios.ufshc.c` |  | `ufs_bench=`, `ufs_check=`, `ufs_native_bench=`, `ufs_native_buffer=`, `ufs_native_check=`, `ufs_native_hang=`, `ufs_native_leave=`, `ufs_native_slots=` and 3 more |  | built | built | |
| **`HAKIOS_VIRTIO_BLK`**<br>a disk image of the host as /dev/rvblk0 (virtio_blk=1). QEMU only | `hakios.virtio_blk.c` |  | `virtio_blk=` | built | built |  | |

## wifi

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_WIFI`**<br>Wi-Fi of the WCN6750 (wifi=1, wifi_step=N): the DSP that runs the radio's firmware, Linux's ath11k (its object, made by tools/mk-ath11k.sh, is linked in only with this), a station, and the Apple80211 surface Darwin's wifid talks to. Needs fw-wifi/ from the phone | `hakios.wifi.c`<br>`hakios.wlfw.c`<br>`hakios.lx.c`<br>`hakios.sta.c`<br>`hakios.apple80211.c`<br>`hakios.wlan.cpp` | `HAKIOS_QMI` | `wifi=`, `wifi_again=`, `wifi_ask=`, `wifi_board=`, `wifi_cal=`, `wifi_dirty=`, `wifi_file=`, `wifi_fine=` and 16 more |  | built | built | |

## audio (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_SPEAKER`**<br>the two NXP TFA98xx amplifiers on the I2C bus of serial engine 2 (0x988000, GPIO 8 data, GPIO 9 clock), the 0x34 one behind reset GPIO 101: a look at their registers (speaker=1), the reset line (speaker=2) and the reset register (speaker=3); and, for the sound path (sound=3/4), the init sequence of the TFA9873 (rev 0x0b73) from Nothing's kernel and this phone's tfa98xx.cnt: the DSP's TDM port plays, the amplifiers start and stop | `hakios.spacewar_speaker.c` |  |  | built | built | built | |

## camera (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_CAMERA`**<br>the Nothing Phone 1's three cameras: Sony IMX766 (50 MP back main, sensor 1), Sony IMX471 (16 MP front selfie, sensor 2), and Samsung S5KJN1 (50 MP back ultra-wide, sensor 3). Supports cam=1 (probe), cam=2 (power/ID), cam=3 (CSID TPG), cam=4 (raw frames), and cam=5 (/dev/hakios-camera for Darwin AVCapture and Camera.app). | `hakios.spacewar_camera.c` |  | `cam_fake=`, `cam_sensor=` | built | built | built | |

## core (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_PMIC`**<br>the SPMI arbiter of spacewar's five PMICs (PMK8350, PM7325, PM8350C, PM8350B, PMR735A): the reads and writes the flashlight, haptics and CPU-frequency drivers sit on. Nothing is transferred until one of them asks | `hakios.spacewar_pmic.c` |  | `pon_probe=` | built | built | built | |

## display (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_DISPLAY`**<br>the panel the boot loader lights: the MDSS DSI controller (0xae94000) and the RM692E5's DCS brightness (0x51, 1..4095, inverted DBV), for the window server's brightness control (brightness=N) and the panel's state (dsi_probe=1) | `hakios.spacewar_display.c` |  | `brightness=`, `dsi_probe=` | built | built | built | |

## gpu (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_ADRENO`**<br>the Adreno 642L of the SM7325 (Nothing Phone 1), woken step by step (adreno=N), and HakiosGPU, the IOKit service Mesa and the window server draw through (gpu=1, adreno_keep=1). Needs the GPU's firmware from the phone (fw-gpu/). | `hakios.spacewar_adreno.c`<br>`hakios.spacewar_gpu.cpp` | `HAKIOS_PMIC`, `HAKIOS_UFS` | `adreno=`, `adreno_blind=`, `adreno_dirty=`, `adreno_fine=`, `adreno_keep=`, `adreno_only=`, `adreno_pause=`, `adreno_return=` and 10 more | built | built | built | |

## haptic (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_HAPTIC`**<br>the PM8350B's high-voltage haptics block (SPMI 0x3f0, window 0xf000): the device tree's vmax of 9150 mV, its brake pattern and effect_1's waveform (haptic=1 read-only, 2 enabled, 3 one effect) | `hakios.spacewar_haptic.c` | `HAKIOS_SPACEWAR_PMIC` |  | built | built | built | |

## input (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_FT3680`**<br>the FocalTech FT3680 on SPI (GENI serial engine at 0xa94000, reset GPIO 105, interrupt GPIO 81, panel 1080 x 2400), with its firmware from the phone's fts_tp.ko: reads the controller, downloads the firmware into PRAM when the ROM boot code answers, and posts touches to /dev/hakios-touch (touchscreen=1). spacewar_focaltech_fw.h is where the array in the file comes from | `hakios.spacewar_ft3680.c` |  |  | built | built | built | |

## power (devices/spacewar)

| Driver | Files | Requires | Boot arguments | qemu | redwood | spacewar | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SPACEWAR_CPUFREQ`**<br>the three EPSS clock groups of the Snapdragon 778G+ at 0x18591000, 0x18592000 and 0x18593000, each taken to its fastest row (cpufreq=1). The device tree's qcom,cpufreq-hw-epss | `hakios.spacewar_cpufreq.c` |  | `cpufreq=` | built | built | built | |
| **`HAKIOS_SPACEWAR_TORCH`**<br>the rear flashlight: channel 0 of the PM8350C's flash LED module (SPMI 0x2ee), as Android's led:torch_0 uses it (torch=1 read-only, 2 a flash, 3 on); every write read back | `hakios.spacewar_torch.c` | `HAKIOS_SPACEWAR_PMIC` |  | built | built | built | |

The columns on the right are the devices' defconfigs: "built" means that defconfig builds the driver in.
