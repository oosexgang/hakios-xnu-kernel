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

Boot from the resident loader: stage `system26-prune.apfs` (`oem ramdisk`, `stage`), then a payload with
the kernel, `root26-ios.apfs` and these boot-args:

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
