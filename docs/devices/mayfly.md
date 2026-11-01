<!-- Made by tools/docs-gen.py from devices/mayfly/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Xiaomi 12S (`mayfly`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sm8450` |
| Configuration | `make mayfly_defconfig` |
| Maintainer | cotunjr |
| Repository | [hakios-mayfly](https://github.com/cotunjr/hakios-mayfly) |
| Firmware | `devices/mayfly/docs/FIRMWARE.md` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | works | 1080x2400 DSC command-mode panel at 120 Hz through the framebuffer ABL leaves live; double buffered (fb_flip=1), DCS brightness |
| touch | works | ST FTS on bit-banged SPI (GENI SE4 has its FIFO disabled by TZ), 16-bit coordinates, multitouch (touchscreen=1) |
| buttons | works | power and volume through the PMIC arbiter v7 (touchscreen=1) |
| cpu | works | 7 cores in use (cpus=7 bootcpu=7), EPSS perf states set by the loader, cpu_mhz= ceiling and a 90 C governor |
| gpu | works | Adreno 730 (hakios.adreno7.c, adreno7=18): GMU, zap shader, CP; OpenGL ES through Mesa freedreno at 580 MHz |
| storage | works | UFS read/write through the loader's own driver (no UFSDxe); Darwin root on the userdata partition |
| audio | works | both AW88261 speakers through the ADSP: AudioReach graph with a tertiary TDM sink (adsp=1 sound=4); no microphone |
| battery | partly | level through ADSP battmgr; Battery Health page spins; charging control not done |
| haptic | no | PM8350B hv-haptics, not started |
| camera | no | not started |
| torch | no | not started |
| wifi | works | QCA6490 over PCIe with ath11k (LX_CHIP=qca6490): scan, join, internet (wifi_pcie=12 wifi_lx=2 wifi_station=1); start fails now and then |
| bluetooth | works | QCA6490 over polled UART SE20 at 3 Mbaud, stock bluetoothd, A2DP audio to a speaker (qcabt=6, userland BT=1) |
| cellular | no | not started |
| thermal | partly | TSENS read, CPU governor at 90 C; resets under heavy load near 86-94 C not explained |
| usb | partly | the loader's own DWC3 peripheral (fastboot oem run/log); no USB in iOS |
| sensors | no | not started |

The state is what has been seen on this device, written down in `devices/mayfly/device.conf`; a part the table does not name has not been looked at.
