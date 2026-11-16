# Darwin 26 and 27 on Renoir: RAM-only builds and boot notes

> This is the lab notebook of the bring-up: what was done, in order, with dates and measurements.
> For how the project is organised, start with [architecture.md](architecture.md).
>
> Three things changed after these notes were written, and the commands below were updated for them:
> - The kernel is built with `kernel/build.sh` from `kernel/overlay/` and `kernel/patches/`. The patch
>   numbers in the text (0005 to 0022) are those of the earlier patch series; the same changes are now in
>   the topic patches, mapped in [kernel/patches/README.md](../kernel/patches/README.md).
> - Renoir's default boot arguments are in `boot { }` of its board file, not in `boot26.py`.
> - The main branch no longer describes Renoir. Its board file is in the renoir package
>   (`renoir/board.dts` on the `devices` branch); `tools/device.py add renoir` copies the package into
>   `arch/arm64/boot/dts/` of a checkout. The kernel build, `inj26.sh` and `boot26.py` take the board from there, so
>   bring it in first.
>
> The scripts under `darwin/` expect the team's working tree (`darwin-renoir`) and an extracted IPSW beside
> the repository; see "What is not in this repository" in the architecture document.

State on 2026-10-31: Darwin 26.6.2 (23G90, iPhone12,1 files) boots on renoir (Snapdragon 780G, no PAC) from
RAM through darwin-renoir's resident loader. launchd takes the normal (not restore) path through all of
its boot tasks, about 110 Darwin processes run, and SpringBoard shows the home screen with the status bar,
the labels of the applications, widgets and Search. Not there yet: application icons, and whatever Darwin
draws with materials (an alert's text). These are drawn with the GPU, and the GPU is off in these runs
(`gpu=0`). Touch, Wi-Fi, sound, battery and the slow cores are off as well (the boot-args below).

## Build

All in WSL. `darwin26/` holds the extracted IPSW (`rootfs/`, `cryptex/{os,app}/`, `dyld/`, `bin/ipsw`).

```
tools/device.py add renoir             # Renoir's package, from the devices branch
kernel/build.sh                        # kernel: xnu-12377 + HakiOS base + Renoir patch series
cc -O2 -o arch/arm64/tools/pacstrip arch/arm64/tools/pacstrip.c -lcrypto
arch/arm64/tools/pacstrip cache darwin26/dyld-strip darwin26/dyld/dyld_shared_cache_arm64e{,.[0-9]*}   # not .symbols/.atlas
python3 darwin/tools/fips26.py darwin26/dyld-strip
darwin/userland/inj26.sh                      # our libraries, arm64e
darwin/userland/root26-darwin.sh                 # /hakios image darwin26/root26-darwin.apfs, and the jobs darwin26/jobs26
LAUNCHDAEMONS=darwin26/jobs26 WORK=darwin26/image-tree-prune \
    python3 darwin/tools/mkimage26.py --prune darwin26/system26-prune.apfs
```

Boot, with the phone in its stock fastboot (nothing is flashed or erased; a reboot brings Android back):

```
python3 darwin/tools/boot26.py --wifi-mac AA:BB:CC:DD:EE:FF \
    --system "$HOME/darwin_kernel/darwin26/system26-prune.apfs" \
    --root "$HOME/darwin_kernel/darwin26/root26-darwin.apfs"
```

It boots darwin-renoir's resident loader, stages `system26-prune.apfs` (136 s over USB), then a payload with
the kernel and `root26-darwin.apfs`; the home screen follows about a minute later. Its boot-args (touch,
buttons and all eight cores; `--boot-arg` adds or replaces one, `--return-after N` hands the phone back to
the loader with the log after N seconds). The first runs used these, one core and no touch:

```
haptic=0 cpufreq=0 buttons=0 gpu=0 wifi=0 adsp=0 battmgr=0 speaker_probe=0 renoir_afe_probe=0 sound=0
cam=0 cam_stream_test=0 touchscreen=0 cpus=1 touch_lat=0 launchd_unsecure_cache=1
```

Diagnostic boot-args of the renoir patches: `trace_open=name,name` (0008: every path those processes
look up, in the loader's log), `stacks=name,...` (up to four processes' threads when the run ends).

## What stood between Darwin 26 and its home screen, in order

1. **Code signatures after the PAC rewrite.** pacstrip changes code pages, so their hashes no longer
   matched. The kernel checks a Mach-O's first page at exec; a mismatch clears `CS_VALID` and with it the
   platform-binary status. launchd's hostname write was refused (EPERM) and launchd aborted. pacstrip now
   recomputes the page hashes of every CodeDirectory it touches, both Mach-Os and shared cache files
   (`pacstrip resign FILE...` does it again after a later edit).
2. **"Non-system service tried to claim event stream".** Darwin 26's launchd treats a job as a system
   service only when its plist was loaded from the system volume. Behind the 16.5 port's link to
   `/hakios` no job was, and notifyd killed itself on `BUG IN LIBNOTIFY: Event publisher error`. The
   jobs are now part of the system image.
3. **The restore environment.** launchd reads `rd=md0` in the boot-args as a restore ramdisk. It then runs
   only the restore boot tasks: no `keybagd --init`, no `usermanagerd --init`. Patch 0009 hides that word
   from `kern.bootargs`; the kernel still roots on md0.
4. **FIPS self test.** The boot task `fips` (RequireSuccess) checks an HMAC over corecrypto's code, which
   pacstrip changed. `fips26.py` stores the HMAC of the rewritten code in place of Apple's.
5. **launchd's job cache.** The boot task `launchd_cache_loader` (RequireSuccess) needs
   `System/Library/xpc/launchd.plist`. mkimage26 writes one with our jobs, and `launchd_unsecure_cache=1`
   (a number: the loader parses it with strtoul) has it taken without the `.sig` and AMFI.
6. **Platform binaries.** On a normal boot launchd panics at "unexpected exec of non-platform binary"
   for our `/hakios/sbin/init`. The kernel's AMFI stand-in counts as platform a signature whose
   CodeDirectory carries `CS_ADHOC`; `ldid -S` leaves the flags 0. root26-darwin.sh signs everything with
   `ldid -Cadhoc` and keeps the entitlements.
7. **Personas.** usermanagerd aborts without `/private/var/keybags/persona.kb` ("Daemon failed to load
   persona manifest"), and runningboardd, then backboardd and SpringBoard, wait for it. An empty manifest
   of version 1 is enough: usermanagerd creates the default personas itself.
8. **Data migration.** The 16.5 skeleton's `com.apple.migration.plist` names 20F66. Darwin 26 then takes
   itself for just updated and shows the migration progress bar. root26-darwin.sh writes this build's number.
9. **Activation and setup.** libhakios-sb answers SpringBoard's activation state and refuses the setup
   assistant, as on 16.5. inj26.sh moves those two hooks to their 23G90 addresses after the methods were
   read and found unchanged. The other SpringBoard hooks still carry 16.5 addresses and are left alone.

## Darwin 27

Current configuration (2026-11-10): Darwin 27.2 (24B119, iPhone12,1 files), with CPU/software rendering.
There is no XNU source for Darwin 27; patches 0013-0019 supply the newer userland interfaces.
Unused device-specific CPU/GPU/touch/display/camera/gauge/modem drivers and their loader profiles
and tools have been deleted; the retained Qualcomm paths are those of the SM7325 and SM7350. No external Metal
plug-in is packaged, and `boot26.py` defaults to `gpu=0`. The rebuilt kernel reached Renoir's home
screen in a RAM-only boot on 2026-11-10.

Host verification of the cleaned build:
- The regenerated base patch and all 21 Renoir patches apply to the clean Apple release tree;
  the resulting source tree exactly matches the cleaned patch-validation snapshot.
- The kernel links at 18,360,080 bytes (SHA256
  `e5b6fb0154092949cb65560260b5bdc520fc8ef887c4b8821fa6b342256b6ab9`);
  removed board-driver, MHI transport and Bluetooth runtime symbols are absent.
  The cleaned generic EFI loader also compiles.
- `root26-darwin.apfs` is 14,594,048 bytes and `system27-prune.apfs` is 4,288,716,800 bytes.
  Both pass `apfsck -cw`; the kernel APFS reader reads back all 100 and 84,421 entries respectively,
  matching their source trees.
- The uploaded 32,954,600-byte RAM container embeds this exact kernel and root with `gpu=0`,
  without Metal/Turnip boot arguments; `loader_return_after=0` disables timed return.
- Physical check: stock fastboot reports `product: renoir`; the loader was started with
  `fastboot boot`, then received both images through RAM staging. No storage commands were used.
  At 115 seconds, the screenshot showed the home screen with populated Weather and Calendar widgets.
  The log reports 40 bootstrap checks, 0 failed, SpringBoard PID 33, backboardd PID 36 and the
  +24-point status-bar metric hook. Software-rendered dialogs still have visual artifacts.
  This boot check does not establish long-run stability, stable 60 FPS or lower temperature.

```
cd "$HOME/darwin_kernel/hakios-arm64e-renoir"
export DARWIN_FW="$HOME/darwin_kernel/darwin27"
export OBJ="$PWD/darwin/userland/obj27"
bash kernel/build.sh
arch/arm64/tools/pacstrip cache "$DARWIN_FW/dyld-strip" "$DARWIN_FW"/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 darwin/tools/fips26.py "$DARWIN_FW/dyld-strip" \
    AE2C707EB9149A03BA76C9B332CA5C2FBF758191271C221E28EACA40D8065C18 \
    F0BBB5196E13FC74FEF80014965B915B0882AD557535BCAA07E34BA4959771D1
BUILD=24B119 bash darwin/userland/inj26.sh
bash darwin/userland/root26-darwin.sh
LAUNCHDAEMONS="$DARWIN_FW/jobs26" WORK="$DARWIN_FW/image-tree-prune" \
    python3 darwin/tools/mkimage26.py --prune "$DARWIN_FW/system27-prune.apfs"
python3 darwin/tools/boot26.py --wifi-mac AA:BB:CC:DD:EE:FF
```

(The two FIPS values come from the first boot's log: "In variable" and "MAC generated".)

What stood between Darwin 27 and its home screen, in order:

1. **4 GiB - 1.** fastboot sends a download's size in eight hex digits; Darwin 27's image was 4.59 GB.
   mkimage26 `--prune` leaves out what cannot work here (RAW camera networks, Metal portrait filters, the
   camera ISP's firmware, DriverKit drivers, resources of frameworks for Apple Intelligence, camera effects,
   iWork previews) and trims the emoji font as 16.5 did: 4.20 GB.
2. **The shared region** (0013). The cache ends at 0x30794C000 and slides by up to 512 MiB, past the
   kernel's 6 GiB; dyld could not map it ("Library not loaded: libSystem"). 7 GiB now.
3. **`security.mac.lockdown_mode_state_public`** (0014). libsystem's os_lockdown_mode_enabled() aborts
   without it: launchd died at "Early boot complete". `trace_sysctl=1` (0015) logs every name asked for
   that the kernel has not got.
4. **posix_spawnattr** (0016). Darwin 27's struct has 8 more bytes after `psa_sec_flags` (attr_size 0x100):
   launchd's crash count became a kqworkloop limit and logd was killed at its second workloop.
5. **Canonical paths** (0019). CoreServicesInternal drops from its URL property attributes what the root
   volume's ATTR_VOL_ATTRIBUTES does not list; the kernel's default list lacked ATTR_CMN_FULLPATH, so no
   bundle had a canonical path, LaunchServices registered no application and SpringBoard died of "Got nil
   for _systemAppInfo". `trace_attr=name` (0017, 0018) logs getattrlist/fsgetpath of those processes.
6. **linkd** needs `AppIntentSchemas.sqlite` (4.6 MB, over 16.5's size rule): mkimage26 KEPT_FILES.
7. **The backlight** (libhakios-backlight, in SpringBoard). BacklightServices waits for the display driver
   to confirm a display mode change and aborts SpringBoard after a watchdog; the library confirms it after
   the ramp. The idle timer's timeouts are a day longer: the lock screen used to go dark seconds after the
   interface first came up.
8. **The wallpaper** (libhakios-poster, in PosterBoard). The first poster of a new device is Mercury,
   drawn with Metal (black) on all but two product types; during the initial setup the library makes it
   the WallpaperKit collections poster with the default wallpaper (Stripe), as on those two.
9. **Speed.** Measured with `touch_lat=1 cpuload=1000`, `ustacks=top` and gestures by `rwtouch`
   (`RC_JOBS=FILE` for root26-darwin.sh):
   - `cpufreq=1`: without it all cores run at 300 MHz.
   - **EDR.** backboardd's job names `com.apple.libEDR.Notification`, served by CoreBrightness, which does
     not run here; CoreAnimation asked it at each display state change while holding the lock of its display
     thread and waited out libEDR's timeout: 2 frames reached the panel in 290 s. root26-darwin.sh drops the
     name (lookups fail at once): 3100 frames in the same run.
   - **ScreenTimeAgent** kept a core busy all the time; its job is left out.
   - Then: page swipes 14-29 frames a second, Search and opening Settings 10-17, a list flung up to 43;
     backboardd (CoreAnimation in software) takes most of the processor, the panel copy (kernel_task) the
     rest. `fbscale=2` does not work with Darwin 27 (it stays at the boot spinner); Reduce Transparency and
     Reduce Motion made no measurable difference.
   The first boot's daemons (lsd, linkd, assetsd: every boot is a first boot, /var is in RAM) take a few
   minutes of CPU.
10. **Software graphics (2026-11-10).** Hardware graphics is disabled with `gpu=0`; the root image no
    longer includes a Metal plug-in, and the system image no longer creates a link to one.
    CoreAnimation's software renderer does not draw Darwin 27's Liquid Glass, CASDF shapes or the
    large lock-screen clock. Missing effects and incomplete UI rendering are expected limitations.
    The static collections wallpaper and display-completion hooks remain. The software-only RAM boot
    reached the home screen; old hardware-GPU frame-time measurements do not describe this configuration.
    Checked again under QEMU on 15 November (`gpu=0`, no `metal=1`): the lock screen comes up with the
    Stripe Yellow wallpaper 12 minutes after the kernel starts. `tools/qemu/screendump.sh` takes the
    picture; QEMU pads each row of its PPM to four bytes, and a reader that does not drop the padding sees
    the picture slanted, with its colours turning from row to row.
11. **Library insertion and the clock.** Patch 0022 raises `kern.exec_inject` from 512 to 4096 bytes,
    matching init's input buffer. Longer diagnostic configurations previously failed as a whole with
    `EINVAL`: even the backlight hook was absent, and SpringBoard repeatedly hit its display watchdog.
    `root26-darwin.sh` now expands the template's `@apps` to the executable names of non-hidden applications;
    the kernel accepts exact names, not that token. UIKit-only leading-centering and Pad-inset hooks
    did not move SpringBoard's clock on 24B119: runtime inspection found the live provider was
    `STUIStatusBarVisualProvider_Split1125`. The hook now also adjusts StatusKitUI's native
    `leadingCenteringOffset` by 24 points; the ineffective Pad-inset experiment was removed.
    Verified on RAM-booted Renoir: at 360×800 screenshot size, the clock's leftmost bright pixel moved
    from x=30 to x=54, with y=18 unchanged. The user also confirmed the clock moved.
12. **Frequency checks (2026-11-07, before graphics removal).** The GPU clock counter measured 490,001,684 Hz with 490 MHz requested.
    `cpufreq=1` selects the hardware table's top rows: 1900/2208/2400 MHz. `cpu_test 0` measured dependent
    additions at about 1.7–2.2 GHz under the UI workload; this is not a pinned per-core frequency reading.
    Seven CPUs are available to Darwin in the resident-loader run. The early `membench=1` measured reads at
    14,686 MB/s and copies at 6,725 MB/s; user-space copies under load were 4.96 GB/s. The slow home/lock
    frames are not evidence of CPUs left at 300 MHz. Stable 60 FPS remains open.
13. **White widgets (2026-11-07).** Resource-size pruning had dropped executable files still referenced
    by retained launchd jobs, leaving advertised services without their servers. `mkimage26.py` now
    retains each job's `Program` (or first `ProgramArguments` entry), regardless of the resource-size cap.
    The resulting system image is 4,288,716,800 bytes; the builder rejects images beyond fastboot's
    eight-hex-digit size limit. Both the earlier hardware-graphics run and the 2026-11-10 software-only
    RAM boot showed Weather's “Location Access Needed” and Calendar's date and “No Events Today”.
    This verifies those populated widgets, not every widget or graphical effect.
14. **Heat remains unresolved.** Before graphics removal, a status-bar inspection run reached a GPU
    sensor reading of 69.1°C. The current configuration disables the GPU but still selects fixed CPU
    EPSS rows; it has no Renoir thermal capping. Lower temperature has not been verified.

## Darwin 27 in QEMU (2026-11-14)

Darwin 27.2 (24B119, iPhone12,1 files) on this repository's kernel (xnu-12377, built by LLVM) under QEMU
`virt` with four `cortex-a76` processors and TCG: the lock screen, the home screen with its icons and
labels, Control Center (opened by a swipe through the virtio tablet) and Settings → General → About
("iOS Version 27.2", "iPhone 11"). Graphics are CoreAnimation's software renderer, as `gpu=0` on the phone.
The small image was darwin-redwood's root (its `stage0` and `init`), with `userland/userland/mkjobs.py`'s
jobs and three of our libraries; the system image `mkimage26.py --prune`'s. What it took, in order:

1. **The IPSW on Linux.** Its disk images are Apple Encrypted Archives: `darwin/tools/aea.py` decrypts them
   from the IPSW's URL, reading each once (cryptex 6,178,209,792 bytes, root file system 9,265,217,536,
   every segment authenticated). One read of the root file system ended early without an error, and a
   short read was taken for the end of a segment ("segment 96 of cluster 33 does not authenticate"):
   `aea.py` now takes a short read up again where it stopped, and `decrypt ... 33` went on from there.
2. **Memory.** The system image (4.27 GB) is staged in RAM next to the kernel, so with 8 GB the loader's
   window was 5.2 GB and Darwin had about 500 MB: free memory went to 7 MB and the memory killer took
   daemons. With 10 GB (`-m 10G`, the image at 0xFC000000) the window is 7.26 GB and 2.5 GB stay available.
3. **The device.** The identity `arch/arm64/boot/mkidentity.py` writes (`identity.h`, made by the build, not kept here) describes iPhone18,1 (V53), a device with exclaves: SpringBoard
   asked the secure-indicator manager for its "Camera" indicator, found no ExclaveOS cryptex and threw
   `CASecureIndicatorLayerInvalidName` (`CA_DISABLE_SECURE_INDICATOR=1` in the job did not avoid it).
   A loader built with the identity of the files' own device (`mkidentity.py` over
   `Firmware/all_flash/DeviceTree.n104ap.im4p`, the LZFSE payload of the IM4P) says iPhone12,1, and
   SpringBoard drew its first frame.
4. **Our libraries without cctools.** `clang -target arm64e` with `ld64.lld` writes an arm64 Mach-O with
   classic binding opcodes, and dyld refused it in an arm64e process ("bad bind opcode 0x00"). Built for
   arm64 with `-Wl,-fixup_chains` (chained fixups, `DYLD_CHAINED_PTR_64`) and the header's CPU subtype
   set to arm64e (0x80000002) before `ldid -S`, all three load: `libhakios26.dylib` (from
   `userland/userland/src/inject26.c`: touch, buttons, framebuffer swaps) into backboardd, SpringBoard and
   PosterBoard, `libhakios-backlight.dylib` and the SpringBoard library with 24B119's activation hook.
   `darwin/userland/env.sh` does this by itself when there is no ld64 (`DARWIN_LD`).
5. **lsd.** On the first boot LaunchServices registers every application; under TCG this held SpringBoard's
   main thread for about ten minutes (its stacks, named from the cache's symbol tables, wait in
   `_LSSetSchemeHandler` on lsd). The lock screen came some twelve minutes after power-on.
6. **Watchdogs.** Every application was killed while starting ("Provision violated for watchdog
   process-launch", 10 s of real time). `FBDisableWatchdog`, `BKNoWatchdogs` and the
   `FBLaunchWatchdog*Scale*` keys (20) in mobile's `com.apple.frontboard` and `com.apple.springboard`
   preferences let Settings start and go two levels deep.

Still open there: the Weather and Calendar widgets stay white, libEDR in backboardd retries its lookup
of `com.apple.libEDR.Notification` without end (tens of thousands of lines with `OS_ACTIVITY_DT_MODE`), and
nearbyd aborts at start (`ConstructRegulatoryTxPowerTableMap`).
