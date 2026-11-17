<!-- Made by tools/docs-gen.py from devices/spacewar/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Nothing Phone 1 (`spacewar`)

| | |
|---|---|
| Vendor | Nothing |
| SoC | `sm7325` |
| Configuration | `make spacewar_defconfig` |
| Maintainer | nitheshd30 (README.md) |
| Repository | [hakios-device-spacewar](https://github.com/nitheshd30/hakios-device-spacewar) |
| Firmware | `devices/spacewar/pull-firmware.sh` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | works | the 1080 x 2400 AMOLED panel through UEFI's framebuffer at 60 refreshes a second, beam-synced (fb_beam=1 fb_measure=1), brightness through the RM692E5's DCS register (brightness=N); the window server still lays out for 320 pt (Control Center gaps) and Display Zoom freezes the phone |
| touch | works | FocalTech FT3680 on GENI SPI (CONFIG_HAKIOS_SPACEWAR_FT3680, touchscreen=1); presses reach /dev/hakios-touch and the UI |
| buttons | works | power through the PMK8350 PON and volume through PM7325 GPIO6 (CONFIG_HAKIOS_BUTTONS); power presses seen at 150 ms |
| cpu | partly | the 8 Kryo 670 cores boot (cpus=8 bootcpu=7); the three EPSS clock groups (CONFIG_HAKIOS_SPACEWAR_CPUFREQ, cpufreq=1) are written but not yet verified on the phone |
| gpu | works | Adreno 642L through CONFIG_HAKIOS_SPACEWAR_ADRENO (gpu=1); Metal command buffers and draws complete (metal=3), the window server still presents through the framebuffer |
| storage | works | UFS read and write through the kernel's native path (ufs=2 ufs_native=1 ufs_write=1): 595 reads, 0 errors; /private/var kept on darwin_data (var_hfs) |
| audio | partly | the audio DSP comes up and shuts down cleanly (adsp=1 sound=4) and the two TFA98xx amplifiers are read (CONFIG_HAKIOS_SPACEWAR_SPEAKER); no sound test has passed yet: the amplifiers never start |
| battery | partly | the battery level and charging state in the UI come from the PMIC's QG block; charging under Darwin is not controlled |
| haptic | partly | the PM8350B high-voltage haptics block (CONFIG_HAKIOS_SPACEWAR_HAPTIC, haptic=N) is written; not yet run on the phone |
| camera | partly | the IMX766 main, IMX471 front and S5KJN1 ultra-wide drivers are built (CONFIG_HAKIOS_SPACEWAR_CAMERA, cam=5); bringing a sensor up data-aborts in titan_hardware_start+0x4b8 |
| torch | works | channel 0 of the PM8350C flash LED module (CONFIG_HAKIOS_SPACEWAR_TORCH, torch=3); Control Center's flashlight through /dev/hakios-torch at 125 mA |
| wifi | partly | WCN6750/WPSS: the radio's DSP starts and shuts down cleanly (wifi=1) and the SMMU refuses it nothing; no network is joined (apple80211 CURRENT_NETWORK returns error 6) |
| bluetooth | partly | the WCN6750's Bluetooth half: the common read-only look at its reset and switch pins (CONFIG_HAKIOS_BT, bt=1); no UART, firmware or HCI yet |
| cellular | partly | the modem, QMI and IPA through the common drivers (CONFIG_HAKIOS_MODEM, modem=1); not yet verified on spacewar |
| network | partly | USB networking through the loader and CONFIG_HAKIOS_NET; the Wi-Fi path is not joined yet, so no network is reachable |
| thermal | works | 27 of 27 TSENS sensors read through the common driver (thermal=1), the hottest CPU core at 55 C |
| usb | partly | the DWC3 peripheral that carries the loader's fastboot; networking and charging over it are not verified |
| sensors | no | the SLPI/SSC sensor DSP has no driver yet, so auto-brightness and auto-rotate wait on it |

The state is what has been seen on this device, written down in `devices/spacewar/device.conf`; a part the table does not name has not been looked at.
