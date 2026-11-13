# Darwin user space

What Darwin 26 and 27 need to run on a Qualcomm phone from RAM: small libraries inserted into a few of
Darwin's processes (`darwin/userland/`), and the tools that make Apple's arm64e code runnable, build the
images and boot them (`darwin/tools/`). Darwin itself is not here: it is extracted on your machine from
Apple's IPSW.

**The limitation.** The build scripts, the image builder and the boot tool also need the team's working
tree, `darwin-renoir`, which has not been published. From a clean checkout the two C programs compile
and a few tools run on files you give them; no image is built and no phone is booted. The "Needs"
columns say which is which.

The scripts look in the directory that holds this repository (`DK`) for `darwin-renoir/` (`DR`) and for
the extracted IPSW, `darwin26/` or `darwin27/` (`DARWIN_FW`: `rootfs/`, `cryptex/os/`, `cryptex/app/`, `dyld/`).
The root images and the launchd jobs are written into `DARWIN_FW` as well. `boot26.py` reads none of these
variables: it looks beside the repository and takes `--kernel`, `--system` and `--root`; its defaults are
Darwin 27's images.

## `darwin/userland/`

| File | What it is | Inserted into / used by | Needs |
| --- | --- | --- | --- |
| `inject_backlight.c` | `libhakios-backlight.dylib`: tells SpringBoard that a display-mode change is complete (no display driver does), makes the idle timer's timeouts a day longer, and has the panel follow the mode through `hw.backlight.power`. | SpringBoard | Built by `inj26.sh`. |
| `inject_brightness.c` | `libhakios-brightness.dylib`: the brightness slider and the Auto-Brightness switch, to the kernel's `hw.backlight.level` and `hw.backlight.auto`. | SpringBoard, backboardd, Preferences | Built by `inj26.sh`. Does nothing where the kernel has no backlight (`backlight=0`). |
| `inject_poster.c` | `libhakios-poster.dylib`: the first wallpaper of a new device becomes the collections poster with the default wallpaper, which is drawn without Metal. | PosterBoard | Built by `inj26.sh`. The default wallpaper is the one `mkimage26.py` sets. |
| `inject_statusbar.c` | Moves the status bar's leading items past a hole-punch camera at the top left, by the points the board gives. | SpringBoard, as part of `libhakios-sb.dylib`. Also built alone, as `libhakios-statusbar.dylib`. | Built by `inj26.sh`, with the generated `user_boards.h`. |
| `console_log.c` | `libhakios-console.dylib`: the standard error of its program goes to the console, so into the loader's log. | The programs named in `CONSOLE_LOG="NAME ..."` for `root26-darwin.sh`. | Built by `inj26.sh`. |
| `inj26.sh` | Builds the five above as arm64e, with darwin-renoir's own libraries and the audio plug-in; puts SpringBoard's addresses of the Darwin build in (`BUILD`); runs `pacstrip` and `ldid` over each. Output: `obj26/` beside it, or `OBJ`. | `root26-darwin.sh` | darwin-renoir (`xnu-env.sh`, `userland/src/`), theos (`THEOS`: the iPhoneOS 16.5 SDK, `ldid`), `pacstrip` built, `mesa26e.sh` and `gl26.sh` run. |
| `mesa26e.sh` | Mesa 26.2.5 (softpipe and freedreno, OSMesa) for arm64e: `libOSMesa.8.dylib`, into `obj26/`. | `gl26.sh`, the root images | darwin-renoir (`xnu-env.sh`, `xnu-deps/`, its `mk-mesa.sh` run once), theos, meson, ninja, `pacstrip` built. |
| `gl26.sh` | `libhakios-gl.dylib` and `gl_test` on that Mesa, from darwin-renoir's sources, into `obj26/`. | The root images | darwin-renoir (`xnu-env.sh`, `userland/src/`, `xnu-deps/`), theos, `mesa26e.sh` run, `pacstrip` built. |
| `root26-darwin.sh` | The small image (`/hakios`) for a boot towards SpringBoard: darwin-renoir's root tree with the libraries, every Mach-O signed ad hoc, the lines of `inject.conf`, and what Darwin expects to find in `/private/var`. Also the launchd jobs, made from Darwin's own plists. Writes `$DARWIN_FW/root26-darwin.apfs` and `$DARWIN_FW/jobs26`. | `mkimage26.py` (the jobs), `boot26.py` (the image) | darwin-renoir (`userland/root`, `mkjobs.py`, `mkapfs.py`, `ldid`), the IPSW, `inj26.sh` run. |
| `root26.sh` | A small image for the first boots: darwin-renoir's root tree as it is, with the libraries and `rc.jobs.darwin26-probe` as its `rc.jobs`. Writes `$DARWIN_FW/root26.apfs`. | No other script here. | darwin-renoir (`userland/root`, `mkapfs.py`), `obj26/` filled. |
| `rc.jobs.darwin26-probe` | What the first boots ran, to the console: `uname`, `sw_vers`, `ps`, `id`. `root26.sh` writes it. | `root26.sh` | Nothing. |

A line of the small image's `etc/inject.conf` names a library and the executables that get it; the kernel
inserts it when one of them starts (`kern.exec_inject`). A library that replaces methods finds them by
name, checks their types, and leaves alone what is not as it expects.

## `darwin/tools/`

| File | What it does | Needs |
| --- | --- | --- |
| `pacstrip.c` | Rewrites every PAC instruction into its plain equivalent, in a copy of a dyld shared cache or of a Mach-O, and makes the page hashes of the code signature again. `census` only counts; `resign` is for a later edit. | Compiles from a clean checkout: a C compiler and OpenSSL's libcrypto. Its input is Apple's files. |
| `a64census.c` | Counts, in the executable mappings of a dyld shared cache, the instructions an ARMv8.2 core cannot execute. | Compiles from a clean checkout: a C compiler. Its input is Apple's cache. |
| `fips26.py` | Puts the HMAC that corecrypto computes over its rewritten code in place of the stored one, so that the FIPS self test at boot passes; then `pacstrip resign`. The values in the file are Darwin 26.6.2's (23G90); another build's are given as arguments. | Runs from a clean checkout: Python, `pacstrip` built beside it. Its input is the rewritten cache. |
| `mkimage26.py` | The system image, the RAM disk the loader stages: the chosen files of the IPSW, every arm64e Mach-O through `pacstrip`, the rewritten cache, the launchd jobs and job cache (`LAUNCHDAEMONS=DIR`). `--min` for a few programs only, `--prune` for 6 GB of RAM. Refuses an image above fastboot's 4 GiB - 1. | darwin-renoir (`darwin165/select-system.py`, `userland/obj/stage0`, `mkapfs.py`, `font_trim.py`), the IPSW with `dyld-strip/`, `pacstrip` built. |
| `boot26.py` | Boots a phone from RAM: stock fastboot starts the resident loader, which takes the system image, then the kernel and the root image, over USB. Nothing is flashed or erased. | darwin-renoir (`recovery/payload.py`, `prebuilt/NAME/resident-usb.img`, `recovery/assets/NAME-darwin-share.bin`), a device package with `boot { }`, fastboot, the kernel and both images. |
| `ram-budget/` | The measurements behind [docs/ram-budget.md](../docs/ram-budget.md), by the six scripts below. Their intermediate files go to `/tmp`. | |
| `ram-budget/zchunks.py` | The size of files as the image stores them, compressed in 64 KB pieces; writes the pieces to a pickle. | Runs from a clean checkout: Python. Its input is the cache files. |
| `ram-budget/cache_images.py` | The cache's images, the dependency closure of what a boot runs (`jobs`, `apps`, `broad`), and how much of the compressed cache lies outside it. | The IPSW, the pickle of `zchunks.py`. |
| `ram-budget/rootfs_select.py` | The files the Darwin 16.5 rules choose from the root file system and the cryptexes, raw and compressed, by category. Writes `/tmp/rb_rootfs26.pkl`. | darwin-renoir (`darwin165/select-system.py`), the IPSW. |
| `ram-budget/loctable_trim.py` | What cutting every chosen `.loctable` to English and Base would save. | As `rootfs_select.py`, and its pickle. |
| `ram-budget/prune_candidates.py` | The chosen applications and other groups of files by size: what could be left out. | The pickle of `rootfs_select.py`. |
| `ram-budget/slt_summary.py` | A `7z l -slt` listing of an APFS image, summed by category. | Runs from a clean checkout: Python. |

## What depends on the phone

The libraries and the images are the same for every phone. Three things are not:

- **The status bar's shift:** `screen { } statusbar_leading_shift` of the board file, in points. `inj26.sh`
  runs `tools/boards.py gen` and compiles the generated `user_boards.h` into the library, which finds
  its row at run time by the name in `/chosen/darwin-board`. So the library knows the boards whose
  packages were present when it was built: `tools/device.py add NAME` first. On a board that is not in
  the table, or gives no shift, it does nothing.
- **The boot defaults:** `boot26.py` takes the phone from `--board NAME`, else `$HAKIOS_BOARD`, else the
  one package present that has a `boot { }` section. From `boot { }` it reads `fastboot-product` (what the
  phone must answer to `fastboot getvar product`: any other is refused), `cpus` and `bootargs`
  (`--boot-arg` adds or replaces one). The loader's image is looked for in darwin-renoir by the same name.
- **Which iPhone Darwin is told it is,** and through that the screen layout: `loader/identity.h`, not here.

The SpringBoard addresses in `inj26.sh` and the values in `fips26.py` belong to a Darwin build, not to a phone.

## Building

The order, from [docs/darwin26-boot.md](../docs/darwin26-boot.md), for Darwin 26.6.2 (23G90). Set `DARWIN_FW` to the
extracted IPSW; the scripts' own default is `darwin26/` beside the repository.

```sh
# needs neither darwin-renoir nor Darwin
tools/device.py add renoir                 # the phone's package: screen { }, boot { }
kernel/build.sh
cc -O2 -o darwin/tools/pacstrip darwin/tools/pacstrip.c -lcrypto

# needs the extracted IPSW
# (the cache's main file and its numbered subcaches, not .symbols/.atlas)
darwin/tools/pacstrip cache "$DARWIN_FW/dyld-strip" "$DARWIN_FW"/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 darwin/tools/fips26.py "$DARWIN_FW/dyld-strip"

# needs darwin-renoir and theos as well
bash darwin/userland/inj26.sh                 # the libraries; before it, once: mesa26e.sh, then gl26.sh
bash darwin/userland/root26-darwin.sh            # $DARWIN_FW/root26-darwin.apfs and $DARWIN_FW/jobs26
LAUNCHDAEMONS="$DARWIN_FW/jobs26" WORK="$DARWIN_FW/image-tree-prune" \
    python3 darwin/tools/mkimage26.py --prune "$DARWIN_FW/system26-prune.apfs"

# needs darwin-renoir (the loader's image, the payload tool) and the phone in its stock fastboot
python3 darwin/tools/boot26.py --wifi-mac AA:BB:CC:DD:EE:FF \
    --system "$DARWIN_FW/system26-prune.apfs" --root "$DARWIN_FW/root26-darwin.apfs"
```

For Darwin 27.2 (24B119): `DARWIN_FW=.../darwin27`, `OBJ=.../obj27`, `BUILD=24B119` for `inj26.sh`, and that build's
two values for `fips26.py` (`fips26.py CACHE_DIR STORED COMPUTED`; its header says where they are read).

## More

- [docs/architecture.md](../docs/architecture.md): "Darwin user space", and "What is not in this repository".
- [docs/darwin26-boot.md](../docs/darwin26-boot.md): what stood between Darwin and its home screen, in order.
- [docs/ram-budget.md](../docs/ram-budget.md): does the image fit in 6 GB of RAM.
- [docs/backlight.md](../docs/backlight.md): what `inject_backlight.c` and `inject_brightness.c` talk to.
- [docs/porting.md](../docs/porting.md), step 7: the user-space side of a new phone.
- [devices/README.md](../devices/README.md): the board file, and the tables made of it.
