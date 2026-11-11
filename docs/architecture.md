# Architecture

How the pieces fit together, and where each one lives in the repository. Read this before changing
anything; [porting.md](porting.md) is the step-by-step guide for a new device.

## The boot chain

```
stock fastboot ──"fastboot boot"──► UEFI (Mu-Silicium port for the phone; nothing is flashed)
                                      │
                                      ▼
                         loader (loader/, a UEFI application)
                           - finds its board by the framebuffer the UEFI reports
                           - builds the Apple-format device tree and boot_args
                           - passes the board's name in /chosen/darwin-board
                           - stays resident: receives the kernel's log, takes the next run over USB
                                      │
                                      ▼
                         kernel (kernel/: Apple's XNU + overlay + patches)
                           - looks its board up in the generated table (hakios_board())
                           - starts only the drivers the board lists and the boot-args ask for
                           - the root file system and /var live in RAM
                                      │
                                      ▼
                         iOS user space, unmodified apart from pointer authentication
                           - launchd → backboardd → SpringBoard → apps
                           - small libraries of ours are inserted into a few processes (ios/userland/)
```

When the kernel is done, or panics, it returns to the loader, which still holds the phone's USB link.
A reboot brings Android back.

## Three layers, three kinds of change

| Layer | Where | Changes when |
| --- | --- | --- |
| **Device data** | `devices/NAME/board.toml`, `soc/SOC.toml` | A new phone, or a new SoC. No code. |
| **Drivers for parts** | `kernel/overlay/bsd/dev/`, `kernel/overlay/iokit/Kernel/` | A phone has a part nobody wrote a driver for yet (a touch controller, an amplifier). |
| **The port itself** | `kernel/patches/`, the rest of `kernel/overlay/`, `loader/`, `ios/` | Apple's kernel or iOS changes, or the port learns something new. Same for every device. |

The aim is that a new phone on a known SoC is a change to the first row only, and a new SoC adds one
file to it. Where that is not yet true, [drivers.md](drivers.md) says so driver by driver.

## Device data

```
devices/renoir/board.toml ─┐
soc/sm7350.toml ───────────┤  tools/boards.py gen
                           ├──────────────────────►  kernel/overlay/bsd/dev/hakios_board_table.h  (the kernel's table)
devices/…  soc/… ──────────┘                         loader/platforms.h                            (the loader's)
```

- A **board file** says what the device has: its SoC, the drivers it may start (`features`), how its
  panel's brightness is set, thermal floors, how the loader recognises it, its default boot arguments.
- A **SoC file** says where the blocks of the chip are: interrupt controller, cores, clock
  controllers, DSI controller, temperature sensors, the CPU frequency block.
- Both generated files are committed. `make check` fails if they are not what the data says, so
  the data and the code cannot drift apart.
- At boot the kernel finds its row by `/chosen/darwin-board`. A driver asks
  `hakios_board_has(HAKIOS_HAS_…)` before it starts and reads addresses from `hakios_board()`.
  A board that is not in the table has no feature: nothing of a phone is touched on a machine nobody
  described.

Details and every key: [devices/README.md](../devices/README.md), `devices/_template/board.toml`.

## The kernel

| Path | Contents |
| --- | --- |
| `kernel/UPSTREAM` | The Apple release, pinned by commit. |
| `kernel/overlay/` | The files HakiOS adds (130 of them), as ordinary sources at their place in the XNU tree. |
| `kernel/patches/` | The changes to Apple's own 89 files, as 7 patches by topic. |
| `kernel/assemble.sh`, `export.sh`, `verify.sh` | Make the tree that is compiled; take work back; check the round trip. |
| `kernel/build.sh`, `deps/`, `host/`, `iig-impl/` | The build on Linux. |

What the overlay provides, in the order a boot meets it:

1. **Platform** (`osfmk/arm64/`): GICv3 interrupts, PSCI for the other cores, the return to the
   loader with the log, emulation of the few arm64e instructions an ARMv8.2 core lacks.
2. **Stand-ins for Apple's closed kernel extensions** (`iokit/Kernel/`, `libkern/amfi/`,
   `osfmk/minicrypto/`): the platform expert, IOSurface, IOMobileFramebuffer, AppleKeyStore, the
   network stack's registry side, code-signing policy, crypto.
3. **File systems** (`bsd/miscfs/apfs/`, `ramfs/`): an APFS implementation that reads the iOS images
   from RAM, and the RAM file system for `/private/var`.
4. **Drivers** (`bsd/dev/`): display and GPU, touch, buttons, battery, sound, camera, Wi-Fi, storage,
   CPU frequency, backlight, thermal. Each does nothing until its boot argument asks.

Naming: files and symbols called `redwood_*` carry the name of the first phone. It is the historical
prefix of the platform layer and of the kernel/user-space interfaces (`/dev/redwood-touch`, the
`RedwoodGPU` service), not a statement about which phone the code is for. New device-independent code
is named `hakios_*`.

More: [kernel/README.md](../kernel/README.md), [kernel/patches/README.md](../kernel/patches/README.md).

## The loader

`loader/loader.c` is one file: UEFI glue, the payload format ("XNUP": kernel + images + boot
arguments), the device tree it builds for XNU, the USB link. It needs nothing of EDK2 to build.
Its knowledge of machines is `loader/platforms.h`, generated from the board files.
More: [loader/README.md](../loader/README.md).

## iOS user space

iOS's own programs and frameworks are used as Apple ships them, with one transformation: on CPUs
without pointer authentication, `ios/tools/pacstrip.c` rewrites PAC instructions to their plain
equivalents and re-signs the pages.

| Path | Contents |
| --- | --- |
| `ios/userland/inject_*.c` | Libraries inserted into single processes by the kernel (`kern.exec_inject`): display-mode completion and panel power, brightness, the first wallpaper, the status bar's clock. Each checks the method it replaces and leaves it alone if it is not what it expects. |
| `ios/userland/*.sh` | Build those libraries and the small root image. |
| `ios/tools/` | `pacstrip`, the system image builder (`mkimage26.py`), the FIPS self-test fix, the RAM budget tools, the boot tool (`boot26.py`). |
| `userland/` | The earlier generation's user-space sources that are still used or still being ported: `init`, the audio plug-in, the Metal device, the AIR-to-NIR shader compiler for Mesa, the APFS image tools. |

How iOS 26 and 27 were brought to the home screen, step by step: [ios26-boot.md](ios26-boot.md).

## What is not in this repository

Being honest about this is part of the architecture: these are the inputs a build or a boot needs
from elsewhere. Everything that is fetched is pinned to an exact version, listed in one place:
`components.toml` at the top of the repository (`tools/components.py` prints it, and the lint checks it
against the scripts that fetch).

| What | Why not here | Where it comes from |
| --- | --- | --- |
| Apple's XNU release | It is Apple's; only the pin is kept. | `kernel/assemble.sh` fetches it. |
| iOS itself (IPSW contents) | Apple's files are never redistributed. | Extracted on your machine from Apple's IPSW. |
| The phone's firmware (GPU, DSP, touch) | The vendor's; taken from the same phone it runs on. | `adb pull` from its Android. |
| theos (cctools linker, iPhoneOS SDK) | A third-party toolchain. | [theos.dev](https://theos.dev/docs/installation-linux) |
| The UEFI for a phone | A port of Mu-Silicium per device. | `base/uefi/` has the redwood port and the changes a port needs. |
| **Renoir's resident loader, its recovery payload tool and the user-space root tree** | They live in the team's working tree (`darwin-renoir`), which has not been published. | Not available yet. `ios/tools/boot26.py` and the `ios/userland/*.sh` scripts need it. |

The last row is the gap between this repository and booting Renoir from a clean checkout.
Everything under `make check` and the kernel build itself do not depend on it.

## Legacy

`base/` is the workspace of the first generation: iOS 16.5 on the POCO X5 Pro (redwood) and QEMU, with
its own README, scripts and porting notes. Parts of it are still the only place for something (the
UEFI port, the Wi-Fi compatibility layer, the QEMU scripts) and are waiting to be brought to the
current kernel. Its kernel (`base/xnu/`) is superseded by `kernel/`.
