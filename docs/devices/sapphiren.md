<!-- Made by tools/docs-gen.py from devices/sapphiren/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Redmi Note 13 4G NFC (`sapphiren`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `khaje` |
| Configuration | `make sapphiren_defconfig` |
| Maintainer | NeonDev |
| Repository | [hakios-device-sapphiren](https://github.com/DEV-AdriBOT/hakios-device-sapphiren) |

## What works

| Part | State | |
|---|---|---|
| display | works | the panel (1080x2400 at 0x5c000000, as the bootloader leaves it): U-Boot, the loader and the Darwin user space (SpringBoard) draw on it, by the CPU, at about 9 frames a second; no GPU, no compositing hardware |
| cpu | partly | boots on core 0 alone (cpus=1) on an ARMv8.0 SoC: kernel built with HAKIOS_ARMV8_0=1 (no PAN), GIC group 1 enabled with bits 0\|1\|4, virtual timer on PPI 19 (soc_vtimer=19); a kernel started on another core by PSCI hangs before it prints anything |
| usb | partly | U-Boot's fastboot and the loader's own (xl) work in device mode at high speed through the Synopsys Femto PHY (hsphy@1613000); nothing from the kernel yet |
| storage | no | U-Boot's ufs init hangs; the kernel's UFS driver is not tried |
| touch | partly | the FocalTech FTS (I2C 0x38; the tree lists a Synaptics and a Novatek as well) found and read by hand on GPIO 6/7 (synaptics_tcm=1); no finger seen yet |
| gpu | no | Adreno 610: the Metal path knows only the Adreno of chip ID 0x06030500 |
| audio | no |  |
| battery | partly | a fixed level: Android's, passed as battery_level=N at the start of the run; no gauge |
| bluetooth | no |  |
| cellular | no |  |
| haptic | no |  |
| camera | no |  |
| thermal | no |  |

The state is what has been seen on this device, written down in `devices/sapphiren/device.conf`; a part the table does not name has not been looked at.
