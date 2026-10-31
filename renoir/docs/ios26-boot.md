# iOS 26 on renoir: building and booting to the home screen

State on 2026-10-31: iOS 26.6.2 (23G90, iPhone12,1 files) boots on renoir (Snapdragon 780G, no PAC) from
RAM through darwin-renoir's resident loader. launchd takes the normal (not restore) path through all of
its boot tasks, about 110 iOS processes run, and SpringBoard shows the home screen with the status bar,
the labels of the applications, widgets and Search. Not there yet: application icons, and whatever iOS
draws with materials (an alert's text). These are drawn with the GPU, and the GPU is off in these runs
(`gpu=0`). Touch, Wi-Fi, sound, battery and the slow cores are off as well (the boot-args below).

## Build

All in WSL. `ios26/` holds the extracted IPSW (`rootfs/`, `cryptex/{os,app}/`, `dyld/`, `bin/ipsw`).

```
renoir/kernel/build.sh                        # kernel: xnu-12377 + HakiOS base + renoir/kernel/patches 0001-0009
cc -O2 -o renoir/tools/pacstrip renoir/tools/pacstrip.c -lcrypto
renoir/tools/pacstrip cache ios26/dyld-strip ios26/dyld/dyld_shared_cache_arm64e{,.[0-9]*}   # not .symbols/.atlas
python3 renoir/tools/fips26.py ios26/dyld-strip
renoir/userland/inj26.sh                      # our libraries, arm64e
renoir/userland/root26-ios.sh                 # /redwood image ios26/root26-ios.apfs, and the jobs ios26/jobs26
LAUNCHDAEMONS=ios26/jobs26 WORK=ios26/image-tree-prune \
    python3 renoir/tools/mkimage26.py --prune ios26/system26-prune.apfs
```

Boot, with the phone in its stock fastboot (nothing is flashed or erased; a reboot brings Android back):

```
python3 renoir/tools/boot26.py --wifi-mac AA:BB:CC:DD:EE:FF
```

It boots darwin-renoir's resident loader, stages `system26-prune.apfs` (136 s over USB), then a payload with
the kernel and `root26-ios.apfs`; the home screen follows about a minute later. Its boot-args (touch,
buttons and all eight cores; `--boot-arg` adds or replaces one, `--return-after N` hands the phone back to
the loader with the log after N seconds). The first runs used these, one core and no touch:

```
haptic=0 cpufreq=0 buttons=0 gpu=0 wifi=0 adsp=0 battmgr=0 speaker_probe=0 renoir_afe_probe=0 sound=0
cam=0 cam_stream_test=0 touchscreen=0 cpus=1 touch_lat=0 launchd_unsecure_cache=1
```

Diagnostic boot-args of the renoir patches: `trace_open=name,name` (0008: every path those processes
look up, in the loader's log), `stacks=name,...` (up to four processes' threads when the run ends).

## What stood between iOS 26 and its home screen, in order

1. **Code signatures after the PAC rewrite.** pacstrip changes code pages, so their hashes no longer
   matched. The kernel checks a Mach-O's first page at exec; a mismatch clears `CS_VALID` and with it the
   platform-binary status. launchd's hostname write was refused (EPERM) and launchd aborted. pacstrip now
   recomputes the page hashes of every CodeDirectory it touches, both Mach-Os and shared cache files
   (`pacstrip resign FILE...` does it again after a later edit).
2. **"Non-system service tried to claim event stream".** iOS 26's launchd treats a job as a system
   service only when its plist was loaded from the system volume. Behind the 16.5 port's link to
   `/redwood` no job was, and notifyd killed itself on `BUG IN LIBNOTIFY: Event publisher error`. The
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
   for our `/redwood/sbin/init`. The kernel's AMFI stand-in counts as platform a signature whose
   CodeDirectory carries `CS_ADHOC`; `ldid -S` leaves the flags 0. root26-ios.sh signs everything with
   `ldid -Cadhoc` and keeps the entitlements.
7. **Personas.** usermanagerd aborts without `/private/var/keybags/persona.kb` ("Daemon failed to load
   persona manifest"), and runningboardd, then backboardd and SpringBoard, wait for it. An empty manifest
   of version 1 is enough: usermanagerd creates the default personas itself.
8. **Data migration.** The 16.5 skeleton's `com.apple.migration.plist` names 20F66. iOS 26 then takes
   itself for just updated and shows the migration progress bar. root26-ios.sh writes this build's number.
9. **Activation and setup.** libredwood-sb answers SpringBoard's activation state and refuses the setup
   assistant, as on 16.5. inj26.sh moves those two hooks to their 23G90 addresses after the methods were
   read and found unchanged. The other SpringBoard hooks still carry 16.5 addresses and are left alone.

## iOS 27

State on 2026-10-31: iOS 27.2 (24B119, iPhone12,1 files) runs on the same kernel (there is no XNU source
for iOS 27; patches 0013-0019 cover what its userland asks of a newer kernel). It shows the lock screen and
the home screen with icons, the Stripe wallpaper, the dock and Search, all in software at full CPU speed.
Not there yet: the lock screen's large clock, widget contents, Wi-Fi, sound.

```
renoir/kernel/build.sh                        # patches 0001-0019
renoir/tools/pacstrip cache ios27/dyld-strip ios27/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 renoir/tools/fips26.py ios27/dyld-strip \
    AE2C707EB9149A03BA76C9B332CA5C2FBF758191271C221E28EACA40D8065C18 \
    F0BBB5196E13FC74FEF80014965B915B0882AD557535BCAA07E34BA4959771D1
BUILD=24B119 OBJ=renoir/userland/obj27 renoir/userland/inj26.sh
IOS=ios27 OBJ=renoir/userland/obj27 renoir/userland/root26-ios.sh
IOS=ios27 LAUNCHDAEMONS=ios27/jobs26 python3 renoir/tools/mkimage26.py --prune ios27/system27-prune.apfs
python3 renoir/tools/boot26.py --wifi-mac AA:BB:CC:DD:EE:FF   # ios27 by default
```

(The two FIPS values come from the first boot's log: "In variable" and "MAC generated".)

What stood between iOS 27 and its home screen, in order:

1. **4 GiB - 1.** fastboot sends a download's size in eight hex digits; iOS 27's image was 4.59 GB.
   mkimage26 `--prune` leaves out what cannot work here (RAW camera networks, Metal portrait filters, the
   camera ISP's firmware, DriverKit drivers, resources of frameworks for Apple Intelligence, camera effects,
   iWork previews) and trims the emoji font as 16.5 did: 4.20 GB.
2. **The shared region** (0013). The cache ends at 0x30794C000 and slides by up to 512 MiB, past the
   kernel's 6 GiB; dyld could not map it ("Library not loaded: libSystem"). 7 GiB now.
3. **`security.mac.lockdown_mode_state_public`** (0014). libsystem's os_lockdown_mode_enabled() aborts
   without it: launchd died at "Early boot complete". `trace_sysctl=1` (0015) logs every name asked for
   that the kernel has not got.
4. **posix_spawnattr** (0016). iOS 27's struct has 8 more bytes after `psa_sec_flags` (attr_size 0x100):
   launchd's crash count became a kqworkloop limit and logd was killed at its second workloop.
5. **Canonical paths** (0019). CoreServicesInternal drops from its URL property attributes what the root
   volume's ATTR_VOL_ATTRIBUTES does not list; the kernel's default list lacked ATTR_CMN_FULLPATH, so no
   bundle had a canonical path, LaunchServices registered no application and SpringBoard died of "Got nil
   for _systemAppInfo". `trace_attr=name` (0017, 0018) logs getattrlist/fsgetpath of those processes.
6. **linkd** needs `AppIntentSchemas.sqlite` (4.6 MB, over 16.5's size rule): mkimage26 KEPT_FILES.
7. **The backlight** (libredwood-backlight, in SpringBoard). BacklightServices waits for the display driver
   to confirm a display mode change and aborts SpringBoard after a watchdog; the library confirms it after
   the ramp. The idle timer's timeouts are a day longer: the lock screen used to go dark seconds after the
   interface first came up.
8. **The wallpaper** (libredwood-poster, in PosterBoard). The first poster of a new device is Mercury,
   drawn with Metal (black) on all but two product types; during the initial setup the library makes it
   the WallpaperKit collections poster with the default wallpaper (Stripe), as on those two.
9. **Speed.** `cpufreq=1`: without it all cores run at 300 MHz and the interface draws at under a frame a
   second. The first boot's daemons (lsd, linkd, ScreenTimeAgent, assetsd: every boot is a first boot, /var
   is in RAM) take a few minutes of CPU.
