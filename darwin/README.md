# Darwin user space

What Darwin 26 and 27 need to run on a Qualcomm phone. It is in two directories:
- `darwin/userland/`: small libraries inserted into a few of Darwin's processes, the Metal plug-in's build, and the
  scripts that build the small image;
- `darwin/tools/`: the tools that make Apple's arm64e code runnable, build the system image and put it on the phone.

Darwin itself is not here: it is unpacked on your machine from Apple's IPSW.

**What the scripts need.** Everything for a boot of Darwin 27 is built from this repository, with:
- the kernel built first (`kernel/build.sh`): `darwin/userland/env.sh` takes the cross compiler from it;
- theos, for the iPhoneOS 16.5 SDK and `ldid`;
- an unpacked IPSW (`darwin/tools/unpack.sh`).

Four older pieces of the renoir work still need the team's unpublished tree, `darwin-renoir`: `mesa26e.sh`,
`gl26.sh`, `root26.sh` and `boot26.py`. The "Needs" columns say which is which.

`IOS` is the unpacked IPSW (`rootfs/`, `cryptex/os/`, `cryptex/app/`, `dyld/`, `var.skel`, `DeviceTree.bin`).
It defaults to `ios27/` beside the repository. The images and the launchd jobs are written into it, and
`OBJ` (default `$IOS/obj`) holds what is built for it.

## `darwin/userland/`

| File | What it is | Inserted into / used by | Needs |
| --- | --- | --- | --- |
| `env.sh` | What the scripts share: `IOS`, `OBJ`, `BOARD`, `LDID`, the user-space sources of the first generation (`userland/userland`), the cross compiler, `pacstrip` (built when missing). | Every script below | The kernel built. |
| `inject_backlight.c` | `libhakios-backlight.dylib`: tells SpringBoard that a display-mode change is complete (no display driver does), makes the idle timer's timeouts a day longer, and has the panel follow the mode through `hw.backlight.power`. | SpringBoard | Built by `inj26.sh`. |
| `inject_brightness.c` | `libhakios-brightness.dylib`: the brightness slider and the Auto-Brightness switch, to the kernel's `hw.backlight.level` and `hw.backlight.auto`. | SpringBoard, backboardd, Preferences | Built by `inj26.sh`. Does nothing where the kernel has no backlight (`backlight=0`). |
| `inject_capture.c` | `libhakios-capture.dylib`: the flashlight's server (`FigFlashlightLocal`) given a device and stream of ours whose torch level goes to the kernel's `/dev/hakios-torch`; the capture stack's device vendor made with the first generation's capture device (`camera_device.c`, `camera_isp.c`, with the iPhone 11's stream formats: `HAKIOS_IOS27`), whose frames come from `/dev/hakios-camera`; and what the capture graph asks of an iPhone's ISP refused or passed by. | cameracaptured | Built by `inj26.sh`. `torch=3` for the flashlight; `cam=5`, `metal_procs=cameracaptured` and `camera-*.tab`/`.isp` in `firmware/BOARD/fw/` for the camera. |
| `inject_poster.c` | `libhakios-poster.dylib`: the first wallpaper of a new device becomes the collections poster with the default wallpaper, which is drawn without Metal. | PosterBoard | Built by `inj26.sh`. The default wallpaper is the one `mkimage26.py` sets. |
| `inject_wallpaper.m` | `libhakios-wallpaper.dylib`: the default wallpaper's stripes, six SwiftUI shape layers, rasterized, so that the window server draws them once into a texture rather than by its path renderer every frame. `wallpaper_raster=0` leaves them alone. | CollectionsPoster | Built by `inj26.sh`. |
| `inject_statusbar.c` | Moves the status bar's leading items past a hole-punch camera at the top left, by the points the board gives. | SpringBoard, as part of `libhakios-sb.dylib`. Also built alone, as `libhakios-statusbar.dylib`. | Built by `inj26.sh`, with the generated `hakios_boards.h`. |
| `console_log.c` | `libhakios-console.dylib`: the standard error of its program goes to the console, so into the loader's log. | The programs named in `CONSOLE_LOG="NAME ..."` for `root26-darwin.sh`. | Built by `inj26.sh`. |
| `tweak_test.c`, `tweak_test.plist` | `hakios-tweaktest.dylib`: a tweak as a package installs it, to check the tweak path: the tweak loader (`libhakios-tweaks.dylib`, `components/tweaks/inject_tweaks.c`) finds it in `/var/jb/usr/lib/TweakInject` by its filter (Settings), opens it, and it hooks `-[UIViewController viewDidLoad]` through ElleKit's `MSHookMessageEx`, saying each step on the console. The loader opens no tweak with the boot-arg `notweaks=1`, and none in a SpringBoard started after one that did not run 30 s past opening them. | Settings, once a job list has copied both into `/var/jb/usr/lib/TweakInject` | Built by `inj26.sh`; the package manager's prefix in the system image (`components/tweaks/tools/mk-jb.sh`, then `mkimage26.py`). |
| `inj26.sh` | Builds the libraries above and the first generation's (`libhakios`, `libhakios-sb`, `libhakios-app`) and the audio plug-in, arm64e. It puts SpringBoard's addresses for the Darwin build in (`BUILD`), then runs `pacstrip` and `ldid` over each. Output: `OBJ`. | `root26-darwin.sh` | `env.sh`'s. |
| `metal.sh` | The Metal plug-in, arm64e: `components/metal/plugin/plugin/HakiosMetalCA.m` over `components/metal/plugin/`'s driver and Mesa's compiler (`components/metal/plugin/tools/mk-mesa.sh`), then `pacstrip` and `ldid`. Output: `$OBJ/HakiosMetal`. [Documentation/metal-glass.md](../Documentation/metal-glass.md) | `root26-darwin.sh` | `env.sh`'s, meson, ninja. |
| `root26-darwin.sh` | The small image (`/hakios`) for a boot towards SpringBoard: `init`, `stage0`, the libraries, the Metal plug-in when built, `net_test`, `rwtouch`, the phone's firmware (`firmware/BOARD/fw/`; its `network`, the Wi-Fi passphrase, not with `SHARE=1`, and otherwise an image readable by its owner alone), the lines of `inject.conf`, and what Darwin expects in `/private/var`. Every Mach-O is signed ad hoc. It also makes the launchd jobs from Darwin's own plists. Writes `$IOS/root26-darwin.apfs`, `$IOS/jobs26` and `$IOS/flashboot.apfs` (the RAM disk of a boot whose system image is on the flash). `RC_JOBS=FILE`: the commands the demo job runs. | `mkimage26.py` (the jobs), `bench` (the images) | `env.sh`'s, the IPSW, `inj26.sh` run. |
| `ssh.sh` | An SSH server and a shell for a running Darwin, arm64e: dropbear 2026.94 (keys only, patched by `ssh/dropbear-ios.patch` for a system without `/bin/sh` or `/etc/shells`) with its `scp`, and mksh R59c, downloaded once and checked against their SHA-256. The keys are this machine's `../ssh/` (`authorized_keys`, `ssh_host_ed25519_key`, converted to dropbear's format here), never the repository's. Output: `$OBJ/dropbear`, `scp`, `sh`, `ssh-keys/`. | `root26-darwin.sh`, which puts them into `/hakios/bin` and `/hakios/etc/ssh` (no keys with `SHARE=1`) | `env.sh`'s, curl, a C compiler for this machine. |
| `rc.jobs.ssh` | The server started as soon as Wi-Fi is asked for, the lock screen swiped away, the address in the log. Then `ssh -i ../ssh/id_phone root@ADDRESS`, and `scp -O` for files. Darwin has almost no commands of its own (no `ls`, `uname`): the shell's built-ins (`cat` among them), `ps`, `netstat`, `ifconfig`, `ioreg`, and what is in `/hakios/bin`. `net_test announce en0 5` beside it says the phone's address to everybody every 5 s: a router may not pass a computer's ARP request on to the phone (one on 2.4 GHz asking for one on 5 GHz), and `ssh` then says "no route to host" although the phone is there. | `RC_JOBS=` for `root26-darwin.sh` | `ssh.sh` run, `wifi=1`, a long `BENCH_SECS`. |
| `rc.jobs.haptic` | Gestures Darwin answers with its haptics: Settings' icon held until its menu opens, twice, and the home screen held until the icons wiggle. The audio plug-in's "the actuator plays" lines and the motor driver's "haptic:" lines say what was felt. | `RC_JOBS=` for `root26-darwin.sh` | `haptic=5`, the phone's `aw8697_haptic.bin` in `firmware/BOARD/fw/`. |
| `rc.jobs.torch` | The flashlight as an application reaches it: `torch_test` (built by `root26-darwin.sh`, `torch_test.c`) makes an AVFlashlight, lights it at full and at a quarter for 3 s each, then puts it out, saying each step; `libhakios-capture.dylib` says each level it hands the kernel. | `RC_JOBS=` for `root26-darwin.sh` | `torch=3`. |
| `rc.jobs.unlock` | The default commands, plus a swipe up from the bottom edge 100 s after the start: the home screen. | `RC_JOBS=` for `root26-darwin.sh` | Nothing. |
| `rc.jobs.fps` | Frame pacing with something moving: unlock, the home screen's pages swiped both ways, Settings opened and its list flung under the glass search field, then closed. The display driver's "from one frame to the next" lines and `metal_fps=1`'s give the frame times; with `gpu_log=1 touch_lat=1`, `darwin/tools/frames.py` gives them per gesture. | `RC_JOBS=` for `root26-darwin.sh` | Nothing; the default home screen layout (Settings the third row's second icon). |
| `mesa26e.sh` | Mesa 26.2.5 (softpipe and freedreno, OSMesa) for arm64e: `libOSMesa.8.dylib`. | `gl26.sh` | darwin-renoir (`xnu-env.sh`, `xnu-deps/`, its `mk-mesa.sh` run once), theos, meson, ninja. |
| `gl26.sh` | `libhakios-gl.dylib` and `gl_test` on that Mesa, from darwin-renoir's sources: OpenGL ES for Darwin 26, whose CoreAnimation still had a renderer for it. | The root images | darwin-renoir, `mesa26e.sh` run. |
| `root26.sh` | A small image for the first boots of Darwin 26: darwin-renoir's root tree as it is, with `rc.jobs.ios26-probe`. Writes `$IOS/root26.apfs`. | No other script here. | darwin-renoir (`userland/root`, `mkapfs.py`), `OBJ` filled. |
| `rc.jobs.ios26-probe` | What the first boots ran, to the console: `uname`, `sw_vers`, `ps`, `id`. | `root26.sh` | Nothing. |

A line of the small image's `etc/inject.conf` names a library and the executables that get it. The kernel
inserts it when one of them starts (`kern.exec_inject`). A library that replaces methods finds them by name,
checks their types, and leaves alone what is not as it expects.

## `darwin/tools/`

| File | What it does | Needs |
| --- | --- | --- |
| `unpack.sh` | Unpacks an IPSW into `IOS`: the system volume and both cryptexes (encrypted images opened with `ipsw fw aea`), LZBITMAP files expanded, links 7z leaves empty made again, `var.skel`, the decoded device tree. | unzip, 7-Zip 23 or newer, `ipsw`, a C compiler. |
| `fips26.py` | Puts the HMAC that corecrypto computes over its rewritten code in place of the stored one, so that the FIPS self test at boot passes; then `pacstrip resign`. The values in the file are Darwin 26.6.2's (23G90); another build's are given as arguments. | Python, `pacstrip` built beside it. Its input is the rewritten cache. |
| `select-system.py` | Which files of the system volume an image takes (16.5's rules, applied to 26 and 27). | Python. |
| `mkvarskel.py` | The template of `/private/var` with its owners and modes, from a `7z l -slt` listing of the system volume. | Python. Run by `unpack.sh`. |
| `mkimage26.py` | The system image: the chosen files of the IPSW, every arm64e Mach-O through `pacstrip`, the rewritten cache, the launchd jobs and job cache (`LAUNCHDAEMONS=DIR`), the links to the small image's plug-ins. `--min`: a few programs only. `--prune`: smaller for 6 GB of RAM. `--metal`: with the system's Metal libraries. An image above fastboot's 4 GiB - 1 is refused, unless it is `--metal`'s, which is for the flash. | Python, the IPSW with `dyld-strip/`, `$OBJ/stage0`, `pacstrip` built. |
| `darwin-write.py` | Writes a system image into the phone's own partition `darwin`, from Android, and reads it back. It checks the partition against `firmware/BOARD/darwin.json` and the GPT, and writes nothing else. `plan` shows the command without running it. | adb with root on the phone, `firmware/BOARD/darwin.json`. |
| `boot26.py` | Boots renoir from RAM through stock fastboot and darwin-renoir's resident loader. redwood boots with `bench` instead. | darwin-renoir (`recovery/payload.py`, `prebuilt/NAME/resident-usb.img`), a device package with `[boot]`, fastboot. |
| `metal-cache-pull.sh` | Metal's compiled stages off a running phone (the server of `rc.jobs.ssh`), into `$OBJ/metal-cache-seed`; `root26-darwin.sh` puts them into the next small image, where the plug-in looks after its own cache, which starts empty at every boot. | `scp`, the key of `../ssh/`. |
| `frames.py` | Frame pacing while something moves, from a run with `gpu_log=1 touch_lat=1` (and `rc.jobs.fps`): per phase of gestures, the frames a second, the share within 18 ms, the gaps over 50 ms and the GPU time, over the 0.7 s after each gesture. It reads the run's `.live` snapshots too (`BENCH_FOLLOW_KEEP=1`): the log itself keeps only the last minute or two of `tl:` lines. | Python. |
| `ram-budget/` | The measurements behind [Documentation/ram-budget.md](../Documentation/ram-budget.md). Their intermediate files go to `/tmp`; `rootfs_select.py` and the two after it take darwin-renoir's `ios165/select-system.py`. | Python, the IPSW. |

## What depends on the phone

The libraries and the images are the same for every phone. These things are not:

- **The status bar's shift:** `[screen] statusbar_leading_shift` of the board file, in points. `inj26.sh` runs
  `tools/boards.py gen` and compiles the generated `hakios_boards.h` into the library. The library finds its
  row at run time by the name in `/chosen/darwin-board`. So it knows the boards whose packages were present
  when it was built; on a board that is not in the table, or gives no shift, it does nothing.
- **The phone's firmware** (GPU, Wi-Fi DSP), from `firmware/BOARD/fw/` into the small image.
- **The boot arguments:** `[boot]` of the board file, which `bench run` reads (`BENCH_CMDLINE` adds or replaces one).
- **Which iPhone Darwin is told it is,** and through that the screen layout: `identity.h`, which `arch/arm64/boot/build.sh`
  makes from the IPSW's device tree (`arch/arm64/boot/mkidentity.py`).

The SpringBoard addresses in `inj26.sh` and the values in `fips26.py` belong to a Darwin build, not to a phone.

## Building and booting Darwin 27

The kernel built and `HAKIOS_DEVICES` as for its build, the phone's files in `firmware/BOARD/`:

```sh
darwin/tools/unpack.sh iPhone12,1_27.2_24B119_Restore.ipsw ../ios27
arch/arm64/tools/pacstrip cache ../ios27/dyld-strip ../ios27/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 darwin/tools/fips26.py ../ios27/dyld-strip \
    AE2C707EB9149A03BA76C9B332CA5C2FBF758191271C221E28EACA40D8065C18 \
    F0BBB5196E13FC74FEF80014965B915B0882AD557535BCAA07E34BA4959771D1
bash darwin/userland/inj26.sh
bash darwin/userland/metal.sh                 # for metal=1
bash darwin/userland/root26-darwin.sh            # $IOS/root26-darwin.apfs, $IOS/flashboot.apfs, $IOS/jobs26
LAUNCHDAEMONS=../ios27/jobs26 WORK=../ios27/image-tree-prune \
    python3 darwin/tools/mkimage26.py --prune --metal ../ios27/system27-metal.apfs
DARWIN_WRITE=yes python3 darwin/tools/darwin-write.py write ../ios27/system27-metal.apfs 0     # from Android
./bench enter && ./bench ramdisk ../ios27/flashboot.apfs
BENCH_RAMDISK=../ios27/root26-darwin.apfs ./bench run
```

The two FIPS values are 27.2's; another build's come from the first boot's log ("In variable" and "MAC
generated"). The phone's own steps, and what it runs, are in its package's README.

## More

- [Documentation/architecture.md](../Documentation/architecture.md): "Darwin user space", and "What is not in this repository".
- [Documentation/darwin26-boot.md](../Documentation/darwin26-boot.md): what stood between Darwin and its home screen, in order.
- [Documentation/metal-glass.md](../Documentation/metal-glass.md): Metal on the Adreno, and CoreAnimation drawn by it.
- [Documentation/ram-budget.md](../Documentation/ram-budget.md): does the image fit in 6 GB of RAM.
- [Documentation/backlight.md](../Documentation/backlight.md): what `inject_backlight.c` and `inject_brightness.c` talk to.
- [Documentation/porting.md](../Documentation/porting.md), step 7: the user-space side of a new phone.
- [arch/arm64/boot/dts/README.md](../arch/arm64/boot/dts/README.md): the board file, and the tables made of it.
