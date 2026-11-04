<!-- Made by tools/docs-gen.py from devices/redwood/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# POCO X5 Pro 5G (`redwood`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sc7280` |
| Configuration | `make redwood_defconfig` |
| Maintainer | the HakiOS developers (README.md) |
| Repository | [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) |
| Firmware | `devices/redwood/pull-firmware.sh` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | works | the panel through UEFI's framebuffer; frames drawn by the GPU with gpu=1 |
| touch | works | Goodix GT9916 / FocalTech FT3680 on SPI, multitouch (CONFIG_HAKIOS_GOODIX, touchscreen=1) |
| buttons | works | power and volume through the PMIC (CONFIG_HAKIOS_BUTTONS) |
| cpu | works | all 8 cores, the big core at 2.4 GHz (cpus=8 bootcpu=7 cpufreq=1) |
| gpu | works | Adreno 642L, OpenGL ES through Mesa (gpu=1); Metal: the window server draws through it (metal=1), swipes 100-104 frames a second, not 120 |
| storage | works | UFS read through the loader; /private/var kept with var_store=1 ufs_write=1 |
| audio | works | speakers and microphone through the audio DSP (adsp=1 sound=4 mic=4) |
| battery | partly | the level in the UI through the audio DSP; charging under this kernel is not done |
| haptic | works | Awinic AW86927 (haptic=5) |
| camera | works | Camera.app (cam=5) |
| torch | partly | the PM8350C flash LED and Control Center's button (torch=N); not in the README's list of what works |
| wifi | works | WCN6750 with Linux's ath11k: radio, driver, a network joined (wifi=1) |
| bluetooth | no | step one only: a read-only look at its pins (bt=1) |
| cellular | partly | the modem boots, sees the network, the SIM is bound, IPA and en1 in place; no registration yet |
| thermal | partly | 27 of 27 sensors read on the phone (thermal=1); the stop (thermal_stop=) not yet seen firing |

The state is what has been seen on this device, written down in `devices/redwood/device.conf`; a part the table does not name has not been looked at.
