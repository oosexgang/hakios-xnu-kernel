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

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_ADSP`**<br>the audio DSP started and linked (adsp=1, adsp_step=N); the battery's state comes through it too. Needs the DSP's firmware from the phone (fw-audio/) | `hakios.adsp.c` |  | `adsp=`, `adsp_again=`, `adsp_apr_wait=`, `adsp_keep=`, `adsp_listen=`, `adsp_loud=`, `adsp_new=`, `adsp_pause=` and 5 more | built |  | built | |
| **`HAKIOS_SPEAKER`**<br>the two speaker amplifiers on I2C, and their watch of temperature and battery | `hakios.speaker.c` |  | `speaker_only=`, `speaker_probe=`, `speaker_spksw=`, `speaker_sr=` | built |  | built | |
| **`HAKIOS_SOUND`**<br>the speakers playing (sound=N): what the DSP's services are told, the feeder, and the speakers' guard that limits what the amplifiers get | `hakios.sound.c`<br>`hakios.guard.c` | `HAKIOS_ADSP`, `HAKIOS_SPEAKER` | `sound=`, `sound_below=`, `sound_gain=`, `sound_guard_as=`, `sound_matrix=`, `sound_pause=`, `sound_sd=`, `sound_secs=` | built |  | built | |
| **`HAKIOS_MIC`**<br>the microphones, through the audio DSP (mic=N) | `hakios.mic.c` | `HAKIOS_ADSP`, `HAKIOS_SOUND` | `mic=`, `mic_adc=`, `mic_input=`, `mic_main=`, `mic_secs=`, `mic_tap=`, `mic_test=` |  |  | built | |
| **`HAKIOS_VIRTIO_SOUND`**<br>QEMU's virtio sound device as the player of /dev/hakios-audio (virtio_sound=1). QEMU only | `hakios.virtio_sound.c` |  | `virtio_sound=` | built | built | built | |

## bluetooth

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BT`**<br>the Bluetooth half of the WCN6750: step one, a read-only look at its three pins (bt=1) | `hakios.bt.c` |  | `bt=`, `bt_again=` |  |  | built | |

## camera

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_CAMERA`**<br>the cameras (cam=N): the sensors on the CCI bus and the ISP's interface. The sensors, their tables (fw-camera/) and the pins are the POCO X5 Pro's | `hakios.camera.c` |  | `cam_after=`, `cam_fake=`, `cam_fake_fps=`, `cam_frames=`, `cam_gain=`, `cam_keep_mb=`, `cam_mode=`, `cam_pattern=` and 4 more |  |  | built | |
| **`HAKIOS_TORCH`**<br>the flashlight: the flash LED module of the PM8350C (torch=1); it stays off while the battery is too warm | `hakios.torch.c` |  | `torch_max_s=`, `torch_secs=` |  |  | built | |

## core

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_SOCINFO`**<br>the chip's identity from the firmware's shared memory: kern.hakios.soc_* (socinfo=1). Any Qualcomm SoC that keeps SMEM item 137; where the shared memory is comes from soc_smem= | `hakios.socinfo.c` |  | `soc_smem=`, `soc_smem_size=`, `socinfo=` |  |  | built | |

## gpu

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_GPU`**<br>HakiosGPU, the IOKit service Mesa and the window server draw through, over a GPU driver that keeps the GPU on (the hakios_gpu_* calls): redwood's ADRENO, mayfly's ADRENO7 | `hakios.gpu.cpp` |  | `gpu_cached=`, `gpu_keep=` | built |  | built | |
| **`HAKIOS_ADRENO`**<br>the Adreno 642L of the SC7280 woken step by step (adreno=N), and HakiosGPU, the IOKit service Mesa and the window server draw through (gpu=1, adreno_keep=1). Needs the GPU's firmware from the phone (fw-gpu/) | `hakios.adreno.c` | `HAKIOS_GPU` | `adreno=`, `adreno_blind=`, `adreno_dirty=`, `adreno_fine=`, `adreno_keep=`, `adreno_only=`, `adreno_pause=`, `adreno_return=` and 8 more |  |  | built | |

## haptic

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_HAPTIC`**<br>the vibration motor: an Awinic AW86927 driving a linear resonant actuator (haptic=N), its waveforms from fw-haptic/ | `hakios.haptic.c` |  | `haptic=`, `haptic_effect=` |  |  | built | |

## input

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_INPUT`**<br>QEMU's virtio tablet and keyboard as the touch screen and the buttons (virtio_input=1). QEMU only | `hakios.virtio_input.c` |  | `virtio_input=` | built | built | built | |
| **`HAKIOS_GOODIX`**<br>the touch screen: a Goodix GT9916 ("Berlin-D") or a FocalTech FT3680 on SPI (touchscreen=1). The bus, pins and supplies are the POCO X5 Pro's; another phone with one of these controllers changes those | `hakios.goodix.c` |  | `touch_flip=`, `touch_poll_us=`, `touch_rate=`, `touch_spi_hz=`, `touchscreen=` |  |  | built | |
| **`HAKIOS_BUTTONS`**<br>the power and volume keys, read from the PMIC through the SPMI arbiter (buttons=1) | `hakios.buttons.c` |  | `buttons=`, `buttons_steps=`, `press_power=`, `press_power_ms=`, `touchscreen=` | built |  | built | |

## modem

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_QMI`**<br>QMI messages over the IPC router: how the Wi-Fi firmware's service and the modem's are spoken to | `hakios.qmi.c` |  |  | built |  | built | |
| **`HAKIOS_MODEM`**<br>the modem (modem=1): started, its file systems served from the flash, the IPA's data path as en1, the SIM and the network registration. Research; needs fw-modem/ from the phone | `hakios.mpss.c`<br>`hakios.cell.c`<br>`hakios.nas.c`<br>`hakios.ipa.c` | `HAKIOS_QMI`, `HAKIOS_UFS` | `modem=`, `modem_again=`, `modem_apn=`, `modem_buffer=`, `modem_data_nobind=`, `modem_data_off=`, `modem_data_wait=`, `modem_dpm_off=` and 45 more |  |  | built | |

## net

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_NET`**<br>QEMU's virtio network device as the link behind en0 (virtio_net=1). QEMU only | `hakios.virtio_net.c` |  | `virtio_net=` | built | built | built | |

## power

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_BATTERY`**<br>the battery: kern.hakios.battery_*, and HakiosBattery, the IOPMPowerSource Darwin reads the charge from (the state comes from the audio DSP's battery manager, with adsp=1) | `hakios.battery.c`<br>`hakios.battery.cpp` |  | `battery_shared_pa=` | built |  | built | |
| **`HAKIOS_CPUFREQ`**<br>the clock of the cores the kernel runs on (cpufreq=1), and the memory bus's votes (ddr=) | `hakios.cpufreq.c` |  | `cpufreq=`, `cpuload=`, `ddr=`, `ddr_rest=`, `ddr_vote=`, `membench=` |  |  | built | |
| **`HAKIOS_THERMAL`**<br>the SoC's temperature sensors (thermal=1): kern.hakios.thermal_*, and the run handed back to the loader when the phone gets too hot (thermal_stop=) | `hakios.thermal.c` |  | `thermal=`, `thermal_log=`, `thermal_poll=`, `thermal_stop=`, `thermal_tm0=`, `thermal_tm1=` |  |  | built | |
| **`HAKIOS_PMIC`**<br>the blocks of the power-management chips the SPMI arbiter shows, and the PMIC's clock (pmic=1); read only | `hakios.pmic.c` |  | `pmic=`, `pmic_list=` |  |  | built | |
| **`HAKIOS_BATTGUARD`**<br>the run handed back to the loader before the battery is flat (battery_stop=<percent>) | `hakios.battguard.c` |  | `battery_stop=`, `battery_stop_poll=` |  |  | built | |

## storage

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_UFS`**<br>the phone's UFS flash: a block device for each GPT partition (ufs=1, ufs=2), and the UFS host controller driven from the kernel (ufs_native=). THE ONLY CODE OF THE KERNEL THAT MAY WRITE TO THE PHONE (ufs_write=1, the partition `darwin` only): see tools/provenance.allow | `hakios.ufs.c`<br>`hakios.ufshc.c` |  | `ufs_bench=`, `ufs_check=`, `ufs_native_bench=`, `ufs_native_buffer=`, `ufs_native_check=`, `ufs_native_hang=`, `ufs_native_leave=`, `ufs_native_slots=` and 3 more | built |  | built | |
| **`HAKIOS_VIRTIO_BLK`**<br>a disk image of the host as /dev/rvblk0 (virtio_blk=1). QEMU only | `hakios.virtio_blk.c` |  | `virtio_blk=` | built | built | built | |

## wifi

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_WIFI_STACK`**<br>what every phone's Wi-Fi shares: the kernel's side of Linux's ath11k (its object, made by tools/mk-ath11k.sh, is linked in only with this), a station, and the Apple80211 surface Darwin's wifid talks to. The radio's own driver (WIFI here, QCA6490 on the Xiaomi 12S) answers the hakios_wifi_*() calls hakios.lx.c makes | `hakios.lx.c`<br>`hakios.sta.c`<br>`hakios.apple80211.c`<br>`hakios.wlan.cpp` |  |  | built |  | built | |
| **`HAKIOS_WIFI`**<br>Wi-Fi of the WCN6750 (wifi=1, wifi_step=N): the DSP that runs the radio's firmware, Linux's ath11k (its object, made by tools/mk-ath11k.sh, is linked in only with this), a station, and the Apple80211 surface Darwin's wifid talks to. Needs fw-wifi/ from the phone | `hakios.wifi.c`<br>`hakios.wlfw.c` | `HAKIOS_QMI`, `HAKIOS_WIFI_STACK` | `wifi=`, `wifi_again=`, `wifi_ask=`, `wifi_cal=`, `wifi_dirty=`, `wifi_file=`, `wifi_fine=`, `wifi_keep=` and 14 more |  |  | built | |

## bluetooth (devices/mayfly)

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_QCABT`**<br>the Bluetooth half of the QCA6490 over its UART (qcabt=N): engine, loopback, chip version, firmware, HCI, BLE scan | `hakios.qca6490bt.c`<br>`hakios.qcabt_h4.c`<br>`hakios.qcabt_shim.c` |  |  | built |  | built | |

## gpu (devices/mayfly)

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_ADRENO7`**<br>the Adreno 730 of the SM8475 woken step by step (adreno7=N), and kept on for HakiosGPU with gpu=1. Needs the GPU's firmware from the phone in fw-gpu/ (a730_sqe.fw, gmu_gen70000.bin, a730_zap.*) | `hakios.adreno7.c` | `HAKIOS_GPU` |  | built |  | built | |

## input (devices/mayfly)

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_STFTS`**<br>the touch screen of the Xiaomi 12S: an ST FingerTip (FTM5) on SPI, serial engine 4 (touchscreen=1). The bus, pins and supplies are mayfly's | `hakios.stfts.c` |  |  | built |  | built | |

## wifi (devices/mayfly)

| Driver | Files | Requires | Boot arguments | mayfly | qemu | redwood | |
|---|---|---|---|---|---|---|
| **`HAKIOS_QCA6490`**<br>the talk with the QCA6490's firmware (wifi_pcie=11): its memory, what it is, the board's files, m3, the modes. The PCIe and MHI half is in hakios.adreno7.c for now. Needs fw-wifi/ from the phone | `hakios.qca6490.c` | `HAKIOS_QMI`, `HAKIOS_WIFI_STACK` |  | built |  | built | |

The columns on the right are the devices' defconfigs: "built" means that defconfig builds the driver in.
