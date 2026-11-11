# Loader

A UEFI application that plays iBoot's part for the kernel: it finds the payload (kernel, images, boot
arguments) in RAM, picks XNU's memory window, builds the Apple-format device tree and `boot_args`,
and jumps to the kernel. It stays resident: when the kernel is done it returns here with its log, and
the loader takes commands over USB (the fastboot protocol) for the next run.

| File | What it is |
| --- | --- |
| `loader.c` | The loader itself. `struct platform` there says what it needs to know of a machine. |
| `platforms.h` | The boards it knows. **Generated** at build time, not in the repository: `build.sh` runs `tools/boards.py gen`, which writes it into `build/boards/` from the device packages that are present (`[loader]` of `devices/NAME/board.toml`) and `soc/SOC.toml` (`[platform]`). |
| `efi.h` | The parts of UEFI it uses. |
| `identity.h`, `mkidentity.py` | Which iPhone iOS is told it runs on, taken from that iPhone's own device tree: `build.sh` makes `identity.h` from the `DeviceTree.bin` that `ios/tools/unpack.sh` leaves beside the IPSW (`IDENTITY=FILE` for another). It is Apple's data and is never in the repository; without it the loader is built with a placeholder, and iOS does not start. |
| `mkpayload.py` | Packs the "XNUP" payload: kernel + images + boot arguments. |
| `mkboot.py` | Packs a phone's UEFI and a payload into the Android boot image `fastboot boot` takes. The board's `[uefi]` section says where the UEFI runs; its build (`SILICIUM_UEFI.fd`, `BootShim.bin`, `device-tree.dtb`) is in `firmware/NAME/uefi/`, which is never published. |
| `build.sh` | Makes `platforms.h`, then builds `loader.efi` with clang and lld. No EDK2. The build is reproducible. |

## How it knows the board

The UEFI reports a framebuffer; the loader compares its base with `fb_base` of each board and takes the
one that matches. A display that matches none is QEMU's. The board's name then goes to the kernel as
`/chosen/darwin-board`, where the kernel finds its own row (`kernel/overlay/bsd/dev/hakios_board.c`).

So a new device needs no code here: its `[loader]` section and its SoC's `[platform]` section, then
`build.sh`. [docs/porting.md](../docs/porting.md) says where each value comes from.

The loader knows the boards whose packages were present when it was built. The repository describes
no phone: `tools/device.py add NAME` brings a phone's package in from the `devices` branch. Built with
none, the loader knows only the emulator's board.

No phone is named in `loader.c`. What differs between boards beyond the framebuffer is in `[loader]`
as well: `uefi_charger = true` says the firmware has Qualcomm's charger protocols, and the loader then
reads the battery through them.

## Where it runs

Under a UEFI started with `fastboot boot`: [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
on the phones (`base/uefi/` has the redwood port and the two changes the UEFI needs), stock firmware
under QEMU. Nothing is written to the phone's storage.

Renoir is currently started by a resident USB loader from the team's working tree, which is not in this
repository; it passes the same board name.
