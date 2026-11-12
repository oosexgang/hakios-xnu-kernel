# Moving work from the old layout

For people with a branch, a patch or a habit from before the restructuring. Nothing in the kernel's
behaviour on redwood changed, and one thing changed on Renoir (below).

## Where things went

<!-- paths of the old layout -->

| Before | Now |
| --- | --- |
| `kernel/xnu-12377.121.6-hakios.patch`, `kernel/apply.sh` | `kernel/overlay/` (files HakiOS adds), `kernel/patches/` (changes to Apple's files), `kernel/assemble.sh` |
| `renoir/kernel/patches/00NN-*.patch` | Folded into the same two places. No patch series any more. |
| `renoir/kernel/build.sh`, `xnu-env.sh` | `kernel/build.sh`, `kernel/env.sh` |
| `../xnu-12377-renoir` (the tree) | `../xnu-12377-hakios`, made by `kernel/assemble.sh`. A tree of the old kind is refused: move it away. |
| `renoir/userland/`, `renoir/tools/` | `ios/userland/`, `ios/tools/` |
| `renoir/docs/` | `docs/` |
| `renoir/tests/` | `tests/` |
| `base/xnuloader/` | `loader/` (a link stays in `base/`) |
| `base/xnu-deps/` | `kernel/deps/` (a link stays in `base/`) |
| `bsd/dev/redwood_cpufreq.c` (in the tree) | `bsd/dev/hakios_cpufreq.c` (the cores' clocks), `hakios_ddr.c` (the memory vote), `hakios_bench.c` (`membench=`, `cpuload=`). A patch to the old file applies to none of them as it is: take each hunk to the file its function is in now. |
| `bsd/dev/redwood_buttons.c` (in the tree) | `bsd/dev/hakios_buttons.c`; the arbiter's protocol it had is `hakios_spmi.h`. |
| A driver's own copies of `rd()`/`wr()`, of the waits, of the lookup in RPMh's command database, of a request to RPMh, of a pin's registers | The headers `hakios_mmio.h`, `hakios_time.h`, `hakios_cmd_db.h`, `hakios_rpmh.h`, `hakios_tlmm.h` in `bsd/dev/` ([writing-a-driver.md](writing-a-driver.md)). A patch that touches such a copy has nothing to apply to; what it changed belongs in the header, or in the driver's call of it. |
| Board checks in drivers (`redwood_board_is("renoir")`) | `devices/NAME/board.dts` and `hakios_board_has()` for whether a driver starts, and a key of the board file where two phones differ. No code compares the board's name any more: `redwood_board_is()` is gone. |
| Any `redwood_*` or `renoir_fts*` file of the kernel (`bsd/dev/redwood_camera.c`, `bsd/sys/redwood_sta.h`, `iokit/Kernel/RedwoodGPU.cpp`, ...) | The same file as `hakios_*` (`bsd/dev/hakios_camera.c`, `bsd/sys/hakios_sta.h`, `iokit/Kernel/HakiosGPU.cpp`; `renoir_fts.c` is `hakios_fts.c`). Its functions, types and macros were renamed the same way (`redwood_sta_heard()` is `hakios_sta_heard()`, `REDWOOD_CAMERA_SENSOR` is `HAKIOS_CAMERA_SENSOR`), and so were the nodes in `/dev` (`/dev/hakios-touch`, `/dev/hakios-camera`, ...). The file names said which phone the driver was first written on; which phone a driver is for is the board's to say. `sed -e 's/\bredwood_/hakios_/g' -e 's/\bREDWOOD_/HAKIOS_/g' -e 's/\bRedwood\([A-Z]\)/Hakios\1/g'` carries a patch over, with a look at what it changed. |
| The feature `camera-redwood` | `camera-sm7325`: it says the cameras are on an SM7325's camera blocks with a PM8008 feeding them, which is what the driver needs; how they are wired is the board's `[camera]`. |
| `devices/NAME/board.toml`, `soc/SOC.toml` | `devices/NAME/board.dts`, `soc/SOC.dtsi`: the same keys, in the syntax of a device tree source (`key-name = <0x10>;`, `name = "text";`, a section is a node `speaker { ... };`, a camera sensor a node `sensor@0 { ... };`; [devices/README.md](../devices/README.md) has the format). `tools/boards.py convert board.toml` writes the new file beside the old one, comments kept. |
| Default boot arguments in `boot26.py` | `[boot]` of the phone's board file. It was `devices/renoir/board.dts`; the phones' packages have since moved to the `devices` branch (last section). |

<!-- end -->

## Carrying a patch over

A patch against the kernel tree still applies to the tree: its paths are the same. One command puts
its content where it belongs:

```sh
kernel/import-patch.sh my-0026-something.patch      # several patches: in order
git status                                          # new files under kernel/overlay, changed topic patches
```

It assembles a tree (or updates yours), applies the patch there and exports. If the patch does not
apply, it says so and exports nothing.

## Rebasing a branch

Git follows the renames for files that moved whole. What it cannot follow:

- **A change to the monolithic patch or to a `renoir/kernel/patches/` file.** Turn your change into a
  patch against the tree (`git diff` in your old tree), then `kernel/import-patch.sh`.
- **A new `00NN` patch.** The same.

## Working on the kernel now

Edit a driver in `kernel/overlay/` and run `kernel/build.sh`, or edit in the tree and run
`kernel/export.sh ../xnu-12377-hakios`. The second is the only way for Apple's own files.
[kernel/README.md](../kernel/README.md) has both.

## What behaves differently

- **A driver wired for one phone's parts needs that part in the board file.** `haptic=`, `torch=`,
  `cam=`, `speaker_probe=`, `sound=`, `mic=`, `wifi=` and `touchscreen=` now also need the matching
  feature in `features`. redwood lists all of its parts. **Renoir lists only its touch controller**:
  the speaker, microphone, haptic, flashlight, camera and Wi-Fi drivers in the tree are redwood's
  wiring and have no Renoir code path. Asked for on Renoir, they print which line is missing instead
  of writing to redwood's pins. When you port one of them to Renoir, add its feature to
  `hakios_board.h` and to the board file of the renoir package (`renoir/board.dts` on the `devices`
  branch).
- **A part's wiring is in the board file, and a package from before lacks it.** The touch
  controller's serial engine, clocks, pins and supplies (`[touch]`), the keys (`[buttons]`), the
  flashlight (`[torch]`), the vibration motor (`[haptic]`), the speaker amplifiers (`[speaker]`) and
  the partition the kernel may write (`[storage]`) were numbers in the drivers and in the loader.
  The packages on the `devices` branch have them now. A package of your own made before this is
  refused by `tools/boards.py check`, which names each section that is missing; the values are the
  ones the code had, and `devices/_template/board.dts` says where each comes from. Without
  `[storage]` nothing is refused: the kernel just never writes to the phone's storage.
- **`kernel/build.sh` no longer needs a darwin-renoir checkout** for the host tools. It still uses one
  if it is there.
- **The loader's boards and the kernel's boards come from the same files.** Their tables
  (`platforms.h`, `hakios_board_table.h`) are generated: change the board file or the SoC file, never
  the table. Since the phones moved out (next section) the builds generate them, and no table is in the
  repository.
- **`make check` runs in CI on every push.** A stale `docs/boot-args.md` or a hand-edited patch fails it.

## The phones moved out of the main branch

A later change, for people with a checkout or a branch from before it. The main branch now describes
no phone. What works on which phone did not change.

<!-- paths of the old layout -->

| Before | Now |
| --- | --- |
| `devices/renoir/`, `devices/redwood/` on the main branch | Device packages on the `devices` branch: `renoir/` and `redwood/` at its top. `tools/device.py add renoir` (or `make device DEVICE=renoir`) copies one into `devices/` of a checkout, where git ignores it. |
| `kernel/overlay/bsd/dev/hakios_board_table.h`, `loader/platforms.h`, `ios/userland/hakios_boards.h`, committed | Not in the repository. `tools/boards.py gen` makes them into `build/boards/` (ignored by git), and the builds run it themselves: `kernel/build.sh`, `loader/build.sh`, `ios/userland/inj26.sh`, `kernel/syntax-check.sh`, `tests/run.sh`. |
| `tools/boards.py gen` before committing, `tools/boards.py gen --check` in `make check` | Gone: there is no committed table to compare. `make boards` validates and writes the tables if you want to look at them. |
| Host tests against `renoir` and `redwood` | Against stand-ins in `tests/devices/`: `phone-cmd` and `phone-video`, with the same values under other names, and `phone-keys`, with its parts wired otherwise. |
| The loader asking whether it runs on redwood before it reads the battery | `uefi_charger = true` in the board file's `[loader]` section. |

<!-- end -->

What to do:

- **To build for a phone**, bring its package in first: `tools/device.py add renoir`. A kernel or a
  loader built with no package knows only the emulator's board. `tools/device.py list` says which
  packages are here and where each is from. Packages kept elsewhere are found through `--from SOURCE`
  (a directory, a branch or a commit) or the environment variable `HAKIOS_DEVICES`.
- **An assembled tree made before the change** is updated as usual by `kernel/build.sh` or
  `kernel/assemble.sh --update`. The board table stops being one of the tree's sources and becomes a
  file the build makes there: `kernel/build.sh` puts it at `bsd/dev/hakios_board_table.h`, only when it
  changed.
- **A branch that changes a phone's board file** carries that change to the `devices` branch. A change
  to a SoC file, to a driver or to a key of the board file stays on the main branch.
- **Drivers did not move or change names.** `redwood_*.c`, `renoir_fts.c` and features such as
  `camera-redwood` or `touch-ft3658u` are drivers for parts, started only on a board that lists the
  feature.
