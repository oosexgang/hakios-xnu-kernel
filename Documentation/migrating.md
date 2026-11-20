# Moving work from the old layout

For people with a branch, a patch or a habit from before the restructuring. Nothing in the kernel's
behaviour on the first phone changed, and one thing changed on Renoir (below).

## Where things went

<!-- paths of the old layout -->

| Before | Now |
| --- | --- |
| `kernel/xnu-12377.121.6-hakios.patch`, `kernel/apply.sh` | `kernel/overlay/` (files HakiOS adds), `kernel/patches/` (changes to Apple's files), `kernel/assemble.sh` |
| `renoir/kernel/patches/00NN-*.patch` | Folded into the same two places. No patch series any more. |
| `renoir/kernel/build.sh`, `xnu-env.sh` | `kernel/build.sh`, `kernel/env.sh` |
| `../xnu-12377-renoir` (the tree) | `../xnu-12377-hakios`, made by `kernel/assemble.sh`. A tree of the old kind is refused: move it away. |
| `renoir/userland/`, `renoir/tools/` | `darwin/userland/`, `darwin/tools/` |
| `renoir/docs/` | `docs/` |
| `renoir/workbench/` | `workbench/` |
| `base/xnuloader/` | `loader/` (a link stays in `base/`) |
| `base/xnu-deps/` | `kernel/deps/` (a link stays in `base/`) |
| Everything named after the first phone: the kernel's files, functions, types and `/dev` nodes, its board checks, the feature of its cameras, the small image's libraries, mount point and paths, and that phone's own files in `base/` | `hakios_*` (and `hakios_board_has()`), `camera-sm7325`, `libhakios*.dylib`, `/hakios`, and the phone's package on the `devices` branch. That package's README, "Its former names", has every old name beside the new one, the `sed` line that carries a patch over, and what to do with a working tree that still has the old names. |
| `base/tools/mk-mesa26.sh` and its patch under `base/tools/mesa26/` (Mesa 26 for the Darwin 26 user space) | `userland/tools/mk-mesa26.sh`, `userland/tools/mesa26/darwin.patch`, making `libhakios-air.dylib` in `userland/xnu-deps/build/mesa26/out`, where `userland/build.sh` and the AIR tools look for it. |
| `base/wifi/`, `base/tools/mk-ath11k.sh`, `base/tools/mk-wlfw.sh` against the kernel's headers under their former names | The same against the kernel's `bsd/sys/board/*.h` (`board/lx.h`, `board/qmi.h`, `board/sta.h`), and `mk-wlfw.sh` writes `wlfw_ath11k.h`, which `board/wlfw.c` includes. |
| `base/uefi/apply.sh`, `export.sh` with the first phone's port in `files/` | `apply.sh PHONE_FILES` puts the common changes and a phone's platform (its package's UEFI `files/`) into a clone; `export.sh [MAKER/PHONE PHONE_FILES]` saves them apart again. |
| The earlier names of the system's user space: its directory of tools and libraries, their scripts and images, the boot notes, the tools' test, the rc files, the QEMU script, the camera's board keys, the system volume's mount point, the variables of the firmware and the SDK, launchd's place in the system image | `darwin/` (`darwin/tools/boot26.py`, `darwin/userland/root26-darwin.sh` making `root26-darwin.apfs`, ...), `docs/darwin26-boot.md`, `workbench/darwin_tools_test.py`, `rc.darwin`, `rc.darwin.metal`, `rc.jobs.darwin`, `base/qemu/run-darwin.sh`; `darwin-sensor`, `darwin-front`; `/System/Volumes/Darwin`. The system is called Darwin throughout, and so are the variables `DARWIN_FW` (the extracted firmware) and `DARWIN_SDK` (Apple's SDK), the firmware's default directories `../darwin26` and `../darwin27` (give the directories you have these names), `darwin-renoir`'s `darwin165/`, Apple's launchd in the system image (`/sbin/launchd.darwin`) and the share images (`NAME-darwin-share.bin`); the first phone's package has the script that carries all of it into `darwin-renoir`, with each earlier name beside the new one. Only what Apple's compiler and SDK read stay as Apple spells them: the compiler's target triples, the SDK's own name, IOSurface. |
| `soc/SOC.dtsi`, shared by the boards with that chip, `tools/boards.py --soc DIR` | `devices/NAME/soc.dtsi`, in each package: copy the SoC file into every package whose board names it (`cp soc/sm7350.dtsi devices/renoir/soc.dtsi`). A board's `soc` names its own `soc.dtsi`; `--soc` is gone. |
| The kernel's files with the `hakios_` prefix: `bsd/dev/hakios_NAME.c`, `bsd/sys/hakios_NAME.h`, `osfmk/arm64/hakios_NAME.c`, `iokit/Kernel/HakiosNAME.cpp`, and the generated `hakios_board_table.h`, `hakios_boards.h`, `hakios_focaltech_fw.h`, `hakios_wlfw_ath11k.h` | `bsd/dev/board/NAME.c`, `bsd/sys/board/NAME.h` (included as `<sys/board/NAME.h>`), `osfmk/arm64/NAME.c`, `iokit/Kernel/BoardNAME.cpp`; `board_table.h`, `user_boards.h`, `focaltech_fw.h`, `wlfw_ath11k.h`. The symbols keep their names. A patch carries over with `sed -E 's#bsd/dev/hakios_#bsd/dev/board/#g; s#sys/hakios_#sys/board/#g'`. |
| `userland/userland/src/hakiosmetal.m`, `hakiosdrawable.m`, `userland/tools/mesa/hakios-drm/hakios_drm.c` (copied into Mesa as `src/hakios-drm/`), `userland/tools/mesa26/hakios.patch` | `userland/userland/src/metal_driver.m`, `drawable.m`, `userland/tools/mesa/darwin-drm/darwin_drm.c` (`src/darwin-drm/`), `userland/tools/mesa26/darwin.patch`. What they print (`hakiosmetal:`, `hakiosdrawable:`), the launchd jobs' labels (`com.hakios.*`) and the libraries they make (`libhakios-*.dylib`) keep their names. |
| `components.toml` | `components/upstream.dts` (for a while `components` at the top), in the syntax of the board files: one node a component (`xnu { pins = "..."; pinned-in = "..."; ... };`). `tools/components.py` reads and checks it as before. |
| `docs/` | `Documentation/`, as in Linux's tree. |
| `base/` (the first generation's workspace) | Gone. What was still used of it: `base/uefi/` is `arch/arm64/boot/uefi/`, `base/tools/mk-libdeflate.sh` and `recording.py` are in `tools/`, `base/qemu/touch.py` and `screendump.sh` in `tools/qemu/`. |
| `userland/tools/apfs/` | `tools/apfs/`. |
| `devices/NAME/` (the board files and the packages brought in) | `arch/arm64/boot/dts/NAME/`, as Android's kernels keep their device trees. `tools/boards.py` still finds packages left in `devices/`; move them with `git mv`, or bring them in again with `tools/device.py add NAME`. |
| `loader/` | `arch/arm64/boot/`, where Linux's tree keeps what starts an arm64 kernel: the UEFI application, its `build.sh`, `mkboot.py`, `mkpayload.py`, `mkidentity.py` and `src/`. Its scripts find the repository three levels up. |
| `kernel/overlay/osfmk/arm64/`, `osfmk/minicrypto/`, `bsd/miscfs/apfs/`, `bsd/miscfs/ramfs/` | `arch/arm64/kernel/`, `crypto/`, `fs/apfs/`, `fs/ramfs/`: still those directories of the kernel's tree, where `kernel/assemble.sh` puts them (`kernel/lib.sh`, `OVERLAY_PLACES`). |
| `kernel/overlay/pexpert/pexpert/arm64/`, `kernel/overlay/config/`, `kernel/overlay/libkern/amfi/`; `darwin/tools/pacstrip.c`, `a64census.c` (and the built `darwin/tools/pacstrip`) | `arch/arm64/include/pexpert/`, `arch/arm64/configs/xnu/`, `security/amfi/` (still those directories of the kernel's tree); `arch/arm64/tools/`. |
| `userland/userland/src/init.c`, `stage0.c`, `stage0dsu.c`, `var_store.c`, `child.c`; `userland/userland/libmini/` | `init/`; `lib/libmini/`. |
| `./xl` and its `XL_*` variables | `./bench` and `BENCH_*` (the old names are still read when the new ones are not set); `base/xl`, the first generation's, keeps its name. |
| The Metal device and its compiler in the first generation's userland: `userland/userland/src/metal_*.m`, `metal_props.c`, `metal_plugin.plist`, `rw_null.*`; `userland/tools/mesa/`, `mesa26/`, `air/`, `metal/`, `metallib.py`, `mk-mesa*.sh`, `mk-metal-linux.sh`; `userland/docs/air.md`, `air-report.md` | `components/metal/src/`, `components/metal/tools/` (the same names inside), `components/metal/docs/`. The scripts are run from `components/metal` and build into `components/metal/build/`. |
| `metal/` at the top (the second Metal device, from appstore-ios27-wip) | `components/metal/plugin/`. |
| `darwin/userland/inject_kc.c`, `inject_store.c`, `inject_mescal.m`, `inject_mad.c`, `absinthe_bag.h`; `userland/userland/src/inject_store.c`, `inject_fairplay.c`, `store_test.m` | `components/apple-id/src/`; `components/apple-id/first-generation/`. |
| `userland/userland/src/keychain_test.c` | `components/sep/`. |
| `darwin/userland/inject_poster.c`, `inject_wallpaper.m`; `inject_brightness.c`, `inject_backlight.c` | `components/wallpaper/`; `components/brightness/`. |
| `userland/userland/src/camera_device.c`, `camera_isp.c`, `camera_isp.h`, `camera_test.c`, `camera_avtest.m`; `darwin/userland/inject_capture.c`, `inject_camera_app.c`; `userland/tools/camraw.py`, `camsensor.py`, `camtuning.py` | `components/camera/src/`, `components/camera/tools/`. |
| `userland/userland/src/audio_plugin.c`, `audio_plugin.plist`, `sound_test.c`, `record_test.c` | `components/audio/`. |
| `darwin/userland/inject_statusbar.c` | `components/statusbar/`. |
| `userland/userland/src/inject_tweaks.c`, `darwin/userland/tweak_test.c`, `tweak_test.plist`; `userland/tools/mk-jb.sh`, `jb-preinstall.py`, and the prefix they fetch, `userland/jb/` | `components/tweaks/`, `components/tweaks/tools/`; the prefix is fetched into `components/tweaks/jb/`. |
| `base/tools/camsensor.py`, `camtuning.py`, `camraw.py` | `userland/tools/`, where `userland/userland/build.sh` runs them from |
| A driver's own copies of `rd()`/`wr()`, of the waits, of the lookup in RPMh's command database, of a request to RPMh, of a pin's registers | The headers `board/mmio.h`, `board/time.h`, `board/cmd_db.h`, `board/rpmh.h`, `board/tlmm.h` in `bsd/dev/` ([writing-a-driver.md](writing-a-driver.md)). A patch that touches such a copy has nothing to apply to; what it changed belongs in the header, or in the driver's call of it. |
| `devices/NAME/board.toml`, `soc/SOC.toml` | `devices/NAME/board.dts`, and the SoC file as `devices/NAME/soc.dtsi` (above): the same keys, in the syntax of a device tree source (`key-name = <0x10>;`, `name = "text";`, a section is a node `speaker { ... };`, a camera sensor a node `sensor@0 { ... };`; [devices/README.md](../arch/arm64/boot/dts/README.md) has the format). `tools/boards.py convert board.toml` writes the new file beside the old one, comments kept. |
| Boot argument `renoir_display_clocks=1` | `display_clocks=1`: it reads the display clocks of any board whose SoC file has them. |
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
  feature in `features`. The first phone's package lists all of its parts. **Renoir lists only its
  touch controller**: the speaker, microphone, haptic, flashlight, camera and Wi-Fi drivers in the tree
  were written for the first phone's parts and have no Renoir code path. Asked for on Renoir, they print
  which line is missing instead of writing to another phone's pins. When you port one of them to Renoir, add its feature to
  `board/board.h` and to the board file of the renoir package (`renoir/board.dts` on the `devices`
  branch).
- **A part's wiring is in the board file, and a package from before lacks it.** The touch
  controller's serial engine, clocks, pins and supplies (`[touch]`), the keys (`[buttons]`), the
  flashlight (`[torch]`), the vibration motor (`[haptic]`), the speaker amplifiers (`[speaker]`) and
  the partition the kernel may write (`[storage]`) were numbers in the drivers and in the loader.
  The packages on the `devices` branch have them now. A package of your own made before this is
  refused by `tools/boards.py check`, which names each section that is missing; the values are the
  ones the code had, and `arch/arm64/boot/dts/_template/board.dts` says where each comes from. Without
  `[storage]` nothing is refused: the kernel just never writes to the phone's storage.
- **`kernel/build.sh` no longer needs a darwin-renoir checkout** for the host tools. It still uses one
  if it is there.
- **The loader's boards and the kernel's boards come from the same files.** Their tables
  (`platforms.h`, `hakios_board_table.h`) are generated: change the board file or the SoC file, never
  the table. Since the phones moved out (next section) the builds generate them, and no table is in the
  repository.
- **`make check` runs in CI on every push.** A stale `Documentation/boot-args.md` or a hand-edited patch fails it.

## The phones moved out of the main branch

A later change, for people with a checkout or a branch from before it. The main branch now describes
no phone. What works on which phone did not change.

<!-- paths of the old layout -->

| Before | Now |
| --- | --- |
| The phones' `devices/NAME/` on the main branch | Device packages on the `devices` branch, one directory per phone at its top. `tools/device.py add renoir` (or `make device DEVICE=renoir`) copies one into `arch/arm64/boot/dts/` of a checkout, where git ignores it. |
| `kernel/overlay/bsd/dev/hakios_board_table.h`, `loader/platforms.h`, `darwin/userland/hakios_boards.h`, committed | Not in the repository. `tools/boards.py gen` makes them into `build/boards/` (ignored by git), and the builds run it themselves: `kernel/build.sh`, `loader/build.sh`, `darwin/userland/inj26.sh`, `kernel/syntax-check.sh`, `workbench/run.sh`. |
| `tools/boards.py gen` before committing, `tools/boards.py gen --check` in `make check` | Gone: there is no committed table to compare. `make boards` validates and writes the tables if you want to look at them. |
| Host tests against the phones' own boards | Against stand-ins in `workbench/devices/`: `phone-cmd` and `phone-video`, with the same values under other names, `phone-keys`, with its parts wired otherwise, `phone-scmi`, its speeds over SCMI, `phone-buses`, its PMICs on the second bus of a version 7 arbiter, and `phone-i2c`, its touch controller on I2C. |
| The loader asking whether it runs on the first phone before it reads the battery | `uefi_charger = true` in the board file's `[loader]` section. |

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
- **Drivers are named for what they drive** (`bsd/dev/board/NAME.c`), and are started only on a board that lists
  their feature (`touch-ft3658u`, `camera-sm7325`).
