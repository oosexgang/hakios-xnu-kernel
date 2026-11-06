<!-- Made by tools/docs-gen.py from devices/sapphiren/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Redmi Note 13 4G NFC (`sapphiren`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `khaje` |
| Configuration | `make sapphiren_defconfig` |
| Maintainer | NeonDev |
| Repository | [`devices/sapphiren/`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main/devices/sapphiren) |

## What works

| Part | State | |
|---|---|---|
| display | partly | the panel (1080x2400 at 0x5c000000, as the bootloader leaves it) shows U-Boot's console and the kernel's -v text; MiniIOMobileFramebuffer starts; no GPU, no compositing |
| cpu | partly | boots on core 0 alone (cpus=1) on an ARMv8.0 SoC: kernel built with HAKIOS_ARMV8_0=1 (no PAN), GIC group 1 enabled with bits 0\|1\|4, virtual timer on PPI 19 (soc_vtimer=19); a kernel started on another core by PSCI hangs before it prints anything |
| usb | partly | U-Boot's fastboot and the loader's own (xl) work in device mode at high speed through the Synopsys Femto PHY (hsphy@1613000); nothing from the kernel yet |
| storage | no | U-Boot's ufs init hangs; the kernel's UFS driver is not tried |
| touch | no |  |
| buttons | partly | U-Boot reads power and volume down (PM6125 PON); not the kernel |
| gpu | no | Adreno 610: the Metal path knows only the Adreno of chip ID 0x06030500 |
| audio | no |  |
| battery | no |  |
| wifi | no |  |
| bluetooth | no |  |
| cellular | no |  |
| haptic | no |  |
| camera | no |  |
| thermal | no |  |

The state is what has been seen on this device, written down in `devices/sapphiren/device.conf`; a part the table does not name has not been looked at.
