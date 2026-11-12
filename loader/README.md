# Loader

A UEFI application that plays iBoot's part for the kernel: it finds the payload (kernel, images, boot
arguments) in RAM, picks XNU's memory window, builds the Apple-format device tree and `boot_args`,
and jumps to the kernel. It stays resident: when the kernel is done it returns here with its log, and
the loader takes commands over USB (the fastboot protocol) for the next run.

| File | What it is |
| --- | --- |
| `loader.c` | The loader: its entry point, and the list of its parts. One compilation unit. |
| `src/` | The parts, by topic (below). `src/platform.h` has `struct platform`, which says what the loader needs to know of a machine. |
| `platforms.h` | The boards it knows. **Generated** at build time, not in the repository: `build.sh` runs `tools/boards.py gen`, which writes it into `build/boards/` from the device packages that are present (`[loader]` of `devices/NAME/board.toml`) and `soc/SOC.toml` (`[platform]`). |
| `efi.h` | The parts of UEFI it uses. |
| `identity.h`, `mkidentity.py` | Which iPhone iOS is told it runs on, taken from that iPhone's own device tree. |
| `mkpayload.py` | Packs the "XNUP" payload: kernel + images + boot arguments. |
| `build.sh` | Makes `platforms.h`, then builds `loader.efi` with clang and lld. No EDK2. The build is reproducible. |

## The parts

`loader.c` includes them in this order; a part may use what the parts before it define. They are not
compiled alone: one unit keeps every function local to it, and the binary is the same byte for byte
as when it was one file.

| Part | What it is |
| --- | --- |
| `src/conventions.h` | What the boot shim, the packer and the kernel agree on with the loader. |
| `src/xnu.h` | The kernel's own structures the loader fills in or reads. |
| `src/base.c` | The loader's globals, memory and strings. |
| `src/console.c` | What the loader says, on the screen and into its log; the caches. |
| `src/logstore.c` | The log kept across runs: a raw write to a partition (phone only). |
| `src/devicetree.c` | The writer of the Apple-format device tree. |
| `src/platform.h` | `struct platform`: what the loader needs to know of a machine; includes the generated `platforms.h`. |
| `src/return.c` | The reset, the way back from the kernel, the jump into it. |
| `src/othercore.c` | The kernel on another core: the stub it starts in, its log read while it runs. |
| `src/charger.c` | The battery, read through the firmware's charger protocols. |
| `src/ufs_survey.c` | `ufs=1`: what the phone's storage holds, read only. |
| `src/ufs_block.c` | `ufs=2`: the kernel reads the phone's storage through the loader: the page they share, the controller, the partitions. |
| `src/dsu_extents.c` | Where the DSU image lies in userdata: Android's `lp_metadata`, read. |
| `src/dsu_place.c` | The DSU image found on the phone, and read through its extents. |
| `src/ufs_service.c` | `ufs=2`, the service itself: the storage's units and partitions listed, the kernel's requests answered. |
| `src/charger_start.c` | The firmware's charger protocols found, the first reading (`charger.c` reads). |
| `src/othercore_start.c` | The kernel on another core: started through the firmware's power interface, watched. |
| `src/watchdog.c` | The hypervisor's watchdog. |
| `src/exception.c` | A fault of the loader into the log instead of onto the screen. |
| `src/payload.c` | The payload: kernel, images and boot arguments, found and taken apart. |
| `src/nvram.c` | The NVRAM image the kernel is given. |
| `src/window.c` | The kernel's memory: the firmware's map and the block chosen from it. |
| `src/pac.c` | Pointer authentication of the kernel image: keys, and the image's pointers signed. |
| `src/run.c` | The kernel loaded, given its device tree and boot arguments, and run. |
| `src/usb.c` | The USB command channel: a small server of the fastboot protocol. |
| `src/dsu_write.c` | The system image written through its extents, and nowhere else. |
| `src/dsu.c` | The iOS system image as an Android DSU image ("oem dsu"). |
| `src/commands.c` | The commands taken over USB. |
| `src/ramfb.c` | QEMU's display in the shape of the phone's screen. |

`tests/loader_test.c` runs the parts that are plain C (`base.c`, `devicetree.c`, `nvram.c`,
`payload.c`) on the host; the whole loader is compiled without a warning and linked by `make check`
where clang and lld are installed.

Where to look for a change:

- a new board needs none of them: `[loader]` in its board file and `[platform]` in its SoC file;
- something the kernel is to be told of the machine: `src/platform.h` for the value, `src/run.c` where the
  device tree and the boot arguments are made, `tools/boards.py` for the key;
- a new command over USB: `src/commands.c`;
- how the kernel's memory is chosen: `src/window.c`.

## How it knows the board

The UEFI reports a framebuffer; the loader compares its base with `fb_base` of each board and takes the
one that matches. A display that matches none is QEMU's. The board's name then goes to the kernel as
`/chosen/darwin-board`, where the kernel finds its own row (`kernel/overlay/bsd/dev/hakios_board.c`).

The storage controller the loader and the kernel may use directly (`ufs_native=`) is where
`ufs` of `[platform]` says; without that key it is not touched.

So a new device needs no code here: its `[loader]` section and its SoC's `[platform]` section, then
`build.sh`. [docs/porting.md](../docs/porting.md) says where each value comes from.

The loader knows the boards whose packages were present when it was built. The repository describes
no phone: `tools/device.py add NAME` brings a phone's package in from the `devices` branch. Built with
none, the loader knows only the emulator's board.

No phone is named in the loader's code, and one phone's values are in it in one place only: the
GPT entry of the partition a write may reach (`src/ufs_block.c`, `BLOCK_DARWIN_…`), which is
redwood's. On another phone it matches nothing, so nothing is written there; see "What is still
tied" in [docs/porting.md](../docs/porting.md). What differs between boards beyond the framebuffer is in `[loader]`
as well: `uefi_charger = true` says the firmware has Qualcomm's charger protocols, and the loader then
reads the battery through them.

## Where it runs

Under a UEFI started with `fastboot boot`: [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
on the phones (`base/uefi/` has the redwood port and the two changes the UEFI needs), stock firmware
under QEMU. Nothing is written to the phone's storage.

Renoir is currently started by a resident USB loader from the team's working tree, which is not in this
repository; it passes the same board name.
