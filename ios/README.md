# iOS user space

What iOS 26 and 27 need to run on a Qualcomm phone. It is in two directories:
- `ios/userland/`: small libraries inserted into a few of iOS's processes, the Metal plug-in's build, and the
  scripts that build the small image;
- `ios/tools/`: the tools that make Apple's arm64e code runnable, build the system image and put it on the phone.

iOS itself is not here: it is unpacked on your machine from Apple's IPSW.

**What the scripts need.** Everything for a boot of iOS 27 is built from this repository, with:
- the kernel built first (`kernel/build.sh`): `ios/userland/env.sh` takes the cross compiler from it;
- theos, for the iPhoneOS 16.5 SDK and `ldid`;
- an unpacked IPSW (`ios/tools/unpack.sh`).

Four older pieces of the renoir work still need the team's unpublished tree, `darwin-renoir`: `mesa26e.sh`,
`gl26.sh`, `root26.sh` and `boot26.py`. The "Needs" columns say which is which.

`IOS` is the unpacked IPSW (`rootfs/`, `cryptex/os/`, `cryptex/app/`, `dyld/`, `var.skel`, `DeviceTree.bin`).
It defaults to `ios27/` beside the repository. The images and the launchd jobs are written into it, and
`OBJ` (default `$IOS/obj`) holds what is built for it.

## `ios/userland/`

| File | What it is | Inserted into / used by | Needs |
| --- | --- | --- | --- |
| `env.sh` | What the scripts share: `IOS`, `OBJ`, `BOARD`, `LDID`, the user-space sources of the first generation (`userland/userland`), the cross compiler, `pacstrip` (built when missing). | Every script below | The kernel built. |
| `inject_backlight.c` | `libhakios-backlight.dylib`: tells SpringBoard that a display-mode change is complete (no display driver does), makes the idle timer's timeouts a day longer, and has the panel follow the mode through `hw.backlight.power`. | SpringBoard | Built by `inj26.sh`. |
| `inject_brightness.c` | `libhakios-brightness.dylib`: the brightness slider and the Auto-Brightness switch, to the kernel's `hw.backlight.level` and `hw.backlight.auto`. | SpringBoard, backboardd, Preferences | Built by `inj26.sh`. Does nothing where the kernel has no backlight (`backlight=0`). |
| `inject_poster.c` | `libhakios-poster.dylib`: the first wallpaper of a new device becomes the collections poster with the default wallpaper, which is drawn without Metal. | PosterBoard | Built by `inj26.sh`. The default wallpaper is the one `mkimage26.py` sets. |
| `inject_statusbar.c` | Moves the status bar's leading items past a hole-punch camera at the top left, by the points the board gives. | SpringBoard, as part of `libhakios-sb.dylib`. Also built alone, as `libhakios-statusbar.dylib`. | Built by `inj26.sh`, with the generated `hakios_boards.h`. |
| `console_log.c` | `libhakios-console.dylib`: the standard error of its program goes to the console, so into the loader's log. | The programs named in `CONSOLE_LOG="NAME ..."` for `root26-ios.sh`. | Built by `inj26.sh`. |
| `inj26.sh` | Builds the libraries above and the first generation's (`libhakios`, `libhakios-sb`, `libhakios-app`) and the audio plug-in, arm64e. It puts SpringBoard's addresses for the iOS build in (`BUILD`), then runs `pacstrip` and `ldid` over each. Output: `OBJ`. | `root26-ios.sh` | `env.sh`'s. |
| `metal.sh` | The Metal plug-in, arm64e: `metal/plugin/HakiosMetalCA.m` over `metal/`'s driver and Mesa's compiler (`metal/tools/mk-mesa.sh`), then `pacstrip` and `ldid`. Output: `$OBJ/HakiosMetal`. [docs/metal-glass.md](../docs/metal-glass.md) | `root26-ios.sh` | `env.sh`'s, meson, ninja. |
| `root26-ios.sh` | The small image (`/redwood`) for a boot towards SpringBoard: `init`, `stage0`, the libraries, the Metal plug-in when built, `net_test`, `rwtouch`, the phone's firmware (`firmware/BOARD/fw/`), the lines of `inject.conf`, and what iOS expects in `/private/var`. Every Mach-O is signed ad hoc. It also makes the launchd jobs from iOS's own plists. Writes `$IOS/root26-ios.apfs`, `$IOS/jobs26` and `$IOS/flashboot.apfs` (the RAM disk of a boot whose system image is on the flash). `RC_JOBS=FILE`: the commands the demo job runs. | `mkimage26.py` (the jobs), `xl` (the images) | `env.sh`'s, the IPSW, `inj26.sh` run. |
| `rc.jobs.unlock` | The default commands, plus a swipe up from the bottom edge 100 s after the start: the home screen. | `RC_JOBS=` for `root26-ios.sh` | Nothing. |
| `mesa26e.sh` | Mesa 26.2.5 (softpipe and freedreno, OSMesa) for arm64e: `libOSMesa.8.dylib`. | `gl26.sh` | darwin-renoir (`xnu-env.sh`, `xnu-deps/`, its `mk-mesa.sh` run once), theos, meson, ninja. |
| `gl26.sh` | `libhakios-gl.dylib` and `gl_test` on that Mesa, from darwin-renoir's sources: OpenGL ES for iOS 26, whose CoreAnimation still had a renderer for it. | The root images | darwin-renoir, `mesa26e.sh` run. |
| `root26.sh` | A small image for the first boots of iOS 26: darwin-renoir's root tree as it is, with `rc.jobs.ios26-probe`. Writes `$IOS/root26.apfs`. | No other script here. | darwin-renoir (`userland/root`, `mkapfs.py`), `OBJ` filled. |
| `rc.jobs.ios26-probe` | What the first boots ran, to the console: `uname`, `sw_vers`, `ps`, `id`. | `root26.sh` | Nothing. |

A line of the small image's `etc/inject.conf` names a library and the executables that get it. The kernel
inserts it when one of them starts (`kern.exec_inject`). A library that replaces methods finds them by name,
checks their types, and leaves alone what is not as it expects.

## `ios/tools/`

| File | What it does | Needs |
| --- | --- | --- |
| `unpack.sh` | Unpacks an IPSW into `IOS`: the system volume and both cryptexes (encrypted images opened with `ipsw fw aea`), LZBITMAP files expanded, links 7z leaves empty made again, `var.skel`, the decoded device tree. | unzip, 7-Zip 23 or newer, `ipsw`, a C compiler. |
| `pacstrip.c` | Rewrites every PAC instruction into its plain equivalent, in a copy of a dyld shared cache or of a Mach-O, and makes the page hashes of the code signature again. `census` only counts; `resign` is for a later edit. | A C compiler and OpenSSL's libcrypto. Its input is Apple's files. |
| `a64census.c` | Counts, in the executable mappings of a dyld shared cache, the instructions an ARMv8.2 core cannot execute. | A C compiler. Its input is Apple's cache. |
| `fips26.py` | Puts the HMAC that corecrypto computes over its rewritten code in place of the stored one, so that the FIPS self test at boot passes; then `pacstrip resign`. The values in the file are iOS 26.6.2's (23G90); another build's are given as arguments. | Python, `pacstrip` built beside it. Its input is the rewritten cache. |
| `select-system.py` | Which files of the system volume an image takes (16.5's rules, applied to 26 and 27). | Python. |
| `mkvarskel.py` | The template of `/private/var` with its owners and modes, from a `7z l -slt` listing of the system volume. | Python. Run by `unpack.sh`. |
| `mkimage26.py` | The system image: the chosen files of the IPSW, every arm64e Mach-O through `pacstrip`, the rewritten cache, the launchd jobs and job cache (`LAUNCHDAEMONS=DIR`), the links to the small image's plug-ins. `--min`: a few programs only. `--prune`: smaller for 6 GB of RAM. `--metal`: with the system's Metal libraries. An image above fastboot's 4 GiB - 1 is refused, unless it is `--metal`'s, which is for the flash. | Python, the IPSW with `dyld-strip/`, `$OBJ/stage0`, `pacstrip` built. |
| `darwin-write.py` | Writes a system image into the phone's own partition `darwin`, from Android, and reads it back. It checks the partition against `firmware/BOARD/darwin.json` and the GPT, and writes nothing else. `plan` shows the command without running it. | adb with root on the phone, `firmware/BOARD/darwin.json`. |
| `boot26.py` | Boots renoir from RAM through stock fastboot and darwin-renoir's resident loader. redwood boots with `xl` instead. | darwin-renoir (`recovery/payload.py`, `prebuilt/NAME/resident-usb.img`), a device package with `[boot]`, fastboot. |
| `ram-budget/` | The measurements behind [docs/ram-budget.md](../docs/ram-budget.md). Their intermediate files go to `/tmp`; `rootfs_select.py` and the two after it take darwin-renoir's `ios165/select-system.py`. | Python, the IPSW. |

## What depends on the phone

The libraries and the images are the same for every phone. These things are not:

- **The status bar's shift:** `[screen] statusbar_leading_shift` of the board file, in points. `inj26.sh` runs
  `tools/boards.py gen` and compiles the generated `hakios_boards.h` into the library. The library finds its
  row at run time by the name in `/chosen/darwin-board`. So it knows the boards whose packages were present
  when it was built; on a board that is not in the table, or gives no shift, it does nothing.
- **The phone's firmware** (GPU, Wi-Fi DSP), from `firmware/BOARD/fw/` into the small image.
- **The boot arguments:** `[boot]` of the board file, which `xl run` reads (`XL_CMDLINE` adds or replaces one).
- **Which iPhone iOS is told it is,** and through that the screen layout: `identity.h`, which `loader/build.sh`
  makes from the IPSW's device tree (`loader/mkidentity.py`).

The SpringBoard addresses in `inj26.sh` and the values in `fips26.py` belong to an iOS build, not to a phone.

## Building and booting iOS 27 (redwood)

The kernel built and `HAKIOS_DEVICES` as for its build, the phone's files in `firmware/redwood/`:

```sh
ios/tools/unpack.sh iPhone12,1_27.2_24B119_Restore.ipsw ../ios27
ios/tools/pacstrip cache ../ios27/dyld-strip ../ios27/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 ios/tools/fips26.py ../ios27/dyld-strip \
    AE2C707EB9149A03BA76C9B332CA5C2FBF758191271C221E28EACA40D8065C18 \
    F0BBB5196E13FC74FEF80014965B915B0882AD557535BCAA07E34BA4959771D1
bash ios/userland/inj26.sh
bash ios/userland/metal.sh                 # for metal=1
bash ios/userland/root26-ios.sh            # $IOS/root26-ios.apfs, $IOS/flashboot.apfs, $IOS/jobs26
LAUNCHDAEMONS=../ios27/jobs26 WORK=../ios27/image-tree-prune \
    python3 ios/tools/mkimage26.py --prune --metal ../ios27/system27-metal.apfs
DARWIN_WRITE=yes python3 ios/tools/darwin-write.py write ../ios27/system27-metal.apfs 0     # from Android
./xl enter && ./xl ramdisk ../ios27/flashboot.apfs
XL_RAMDISK=../ios27/root26-ios.apfs ./xl run
```

The two FIPS values are 27.2's; another build's come from the first boot's log ("In variable" and "MAC
generated"). The phone's own steps, and what it runs, are in its package's README.

## More

- [docs/architecture.md](../docs/architecture.md): "iOS user space", and "What is not in this repository".
- [docs/ios26-boot.md](../docs/ios26-boot.md): what stood between iOS and its home screen, in order.
- [docs/metal-glass.md](../docs/metal-glass.md): Metal on the Adreno, and CoreAnimation drawn by it.
- [docs/ram-budget.md](../docs/ram-budget.md): does the image fit in 6 GB of RAM.
- [docs/backlight.md](../docs/backlight.md): what `inject_backlight.c` and `inject_brightness.c` talk to.
- [docs/porting.md](../docs/porting.md), step 7: the user-space side of a new phone.
- [devices/README.md](../devices/README.md): the board file, and the tables made of it.
