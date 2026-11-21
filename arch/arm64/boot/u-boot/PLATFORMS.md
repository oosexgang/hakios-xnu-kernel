# Platform profiles

The boot menu, HKUB container, console commands, and EFI handoff are shared. Each phone gets a separate build profile because U-Boot must know that board's memory map, device tree, boot chain, USB controller, and firmware interfaces.

## Current status

| Profile | SoC / board | Status |
| --- | --- | --- |
| `redwood` | Qualcomm SM7325, POCO X5 Pro 5G | Built on macOS; RAM boot and the XNU loader's USB fastboot were observed on hardware. The Darwin 27 test reached the Apple logo; userspace boot has not been confirmed. |
| Qualcomm phone base | Several Qualcomm SoCs supported upstream | Reference only. A phone profile needs its device tree and a hardware boot test. |
| MediaTek phone base | MediaTek | Not yet ported. Start with a specific phone and SoC; the BootROM, preloader, DDR setup, and USB download path vary by board. |

## What a phone profile supplies

Each profile records:

- U-Boot defconfig and control device tree.
- SoC family, DRAM layout, and safe RAM ranges for the loader and XNUP payload.
- How the stock bootloader starts U-Boot from RAM.
- USB gadget and fastboot transport used to stage images.
- PSCI, watchdog, display, and input details used by the EFI loader and menu.
- A hardware test record for boot, console, recovery, and reset behavior.

The common layer must not assume Qualcomm's EFI USB protocol. A profile selects a transport adapter; the loader and menu use one host-facing command set. The first supported adapter is the existing Qualcomm Redwood path. A MediaTek adapter will be added alongside a tested phone profile.

## Upstream references

- [U-Boot Qualcomm phone bring-up](https://docs.u-boot.org/en/latest/board/qualcomm/phones.html)
- [U-Boot Qualcomm generic board build](https://docs.u-boot.org/en/latest/board/qualcomm/board.html)
- [U-Boot MediaTek Genio boards](https://docs.u-boot.org/en/latest/board/mediatek/mt8188-genio-evk.html)

