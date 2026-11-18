#!/bin/bash
# Build the user-space side of the bring-up.
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.apfs        the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/), an APFS container (../../tools/apfs/mkapfs.py). It travels in
#                    the payload next to the kernel (the second RAM disk, /dev/md1) and is mounted
#                    on /hakios. Rebuilt every time.
#                      RC=<file>    etc/rc: rc.launchd (default) hands process 1 to the Darwin launchd
#                                   after init's checks; rc.darwin runs a few Darwin programs and halts
#                      JOBS="..."   labels of the stock Darwin launchd jobs to load (default: DARWIN_JOBS)
#                      REST=1       ... and the rest of them, started only when asked for (see mkjobs.py)
#                      WIFID=0      without wifid, Darwin's Wi-Fi daemon, which is in by default (see below)
#                      OSLOG="..."  labels of the jobs whose os_log output goes to the console ("all": every
#                                   one of JOBS; default: only the window server, which says little)
#                      INJECT="..." the programs started with a library of ours, "LIBRARY NAME..." per line
#                                   (default: src/inject.c in backboardd, src/inject_sb.c in SpringBoard,
#                                   src/inject_media.c in mediaserverd);
#                                   init hands the list to the kernel (etc/inject.conf -> sysctl kern.exec_inject)
#                      STOREFRONT=1 ... and ../../components/apple-id/first-generation/inject_store.c inside appstored: the storefront answered,
#                                   and a bag request watched for (an experiment; see that file)
#                      EXTRA="..."  more programs for /hakios/bin (files on this machine): one-off
#                                   diagnostics, which a file given as RC_JOBS can then run
#                      EXTRA_SHARE="..." directories on this machine to copy whole into /hakios/share,
#                                   each under its own name: the data a one-off diagnostic reads, when it
#                                   needs more than a program (../../components/metal/tools/air/intrincheck.py --emit writes such
#                                   a directory). This goes into the boot volume, which every recipe
#                                   carries and which the flash recipe puts in RAM, so its size is paid
#                                   for at every boot -- a few megabytes, not more
#                      RC_JOBS=<file> what the job com.hakios.demo runs: rc.jobs (default), or
#                                   rc.jobs.unlock, which also swipes the lock screen away (for the phone,
#                                   where nothing else can touch the screen yet).
#                                   A job list cannot assume a shell. Darwin 16.5's own root has none at all
#                                   -- /bin is df and ps, and nothing else -- so a line beginning /bin/sh
#                                   fails with "No such file or directory" and status 127 on a flash-root
#                                   boot. The /var/jb/bin/sh that rc.jobs.jb and rc.jobs.sileo run is the
#                                   package manager's prefix, which only a JB=1 image carries. And init
#                                   runs these lines itself (init/init.c, run_command): words split on
#                                   blanks, no quoting, no variable assignment, no redirection beyond a
#                                   trailing ">> FILE" and "&". So a program that needs a value passed to
#                                   it needs an option for it, not an environment variable -- which is why
#                                   air_run grew --buffer.
#                      GL=1         with an OpenGL ES implementation for the window server: Mesa
#                                   (../../components/metal/tools/mk-mesa.sh fetches and builds it) behind src/eagl.c, as
#                                   lib/libOSMesa.8.dylib (15 MB) and lib/libhakios-gl.dylib. Where the
#                                   kernel has started the phone's GPU, CoreAnimation then draws with its
#                                   OpenGL ES renderer, by that GPU (boot-arg gpu=1); anywhere else -- QEMU,
#                                   no boot-arg, no firmware in ../fw-gpu -- the library is not even loaded
#                                   and nothing changes (src/inject.c, opengl_es()).
#                                   GL=test: only the library and bin/gl_test (a triangle, src/gl_test.c;
#                                   RC_JOBS=rc.jobs.gltest runs it); the window server stays as it is.
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/ (20 MB; for the
#                                   kernel's adsp_step=N, bsd/dev/board/adsp.c, which runs before the root is
#                                   mounted). Not beside GL=1: the two do not fit the 64 MB the loader takes with
#                                   the kernel. adsp=1 needs none of it: the system image has the firmware in /fw.
#   system.apfs      the system image, the root filesystem (the resident RAM disk, /dev/md0), an
#                    APFS container: the part of Darwin 16.5 chosen by ../darwin165/select-system.py at
#                    its real paths, the dyld shared cache, and init/stage0.c as /sbin/launchd (the
#                    real one is /sbin/launchd.darwin). 2.5 GB; upload once per loader session with
#                    `xl ramdisk userland/system.apfs`. Rebuilt when missing, when its inputs
#                    (stage0, the manifest, the cryptex files) are newer, or with SYSTEM=1 (after
#                    changing the recipe below).
#   flashboot.apfs   FLASHBOOT=1: the RAM part of a boot whose root is the Darwin volume on the phone's
#                    flash (see below); with CRYPTEXES=flash the boot volume alone, the cryptexes being
#                    on the flash. FLASHBOOT=1 also decides what goes into root.apfs: the job list is
#                    then made for the whole Darwin volume (mkjobs.py --root-volume), so it holds daemons
#                    -- fairplayd, identityservicesd, passd ... -- that the system image has not got.
#                    That root.apfs is for a flash boot; a RAM boot wants one built without FLASHBOOT=1,
#                    or launchd will fail to start those few when something asks for them.
#
#   PREBUILT=1       do not compile anything: take our programs from ../prebuilt/userland (this is also
#                    what happens when the cross toolchain is not set up, see ../xnu-deps/setup.sh);
#                    the images are then made of them and of the Darwin files alone (python3)
#   CHECK=1          check every image (every container) as it is made: apfsck has to find it clean and
#                    the kernel's apfs_lib has to read every volume back as the directory it was made of
#                    (../../tools/apfs/check-image.sh; the system image takes a few minutes more)
set -e
cd "$(dirname "$(readlink -f "$0")")"
source ../xnu-env.sh

# This script and its helpers are written for GNU userland (sed -i and \t in patterns, stat -c,
# wc without padding, find -printf...).  On macOS put Homebrew's GNU coreutils/findutils/sed first
# so they behave as on Linux; install with `brew install coreutils findutils gnu-sed`.
if [ -n "$XNU_HOST_DARWIN" ]; then
	for g in coreutils findutils gnu-sed; do
		d=$(brew --prefix 2>/dev/null)/opt/$g/libexec/gnubin
		[ -d "$d" ] && PATH="$d:$PATH"
	done
fi

# ARCH is the architecture of our own user-space code and of the Darwin it runs: arm64 (iPhone X, the
# default) or arm64e (iPhone XS, the pointer-authentication work -- docs/ARM64E.md).  CACHE and the
# dyld-cache file names below follow it; set CACHE to the matching darwin165 directory for the device.
ARCH=${ARCH:-arm64}
CC="xnu-clang -arch $ARCH -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
# The Darwin SDK the dynamic (framework-linking) programs build against: theos's on Linux, and on
# macOS without theos, Xcode's current iPhoneOS SDK (its .tbd stubs resolve against the 16.5 cache
# at run time). Override with DARWIN_SDK.
DARWIN_SDK=${DARWIN_SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}
[ -d "$DARWIN_SDK" ] || { [ -n "$XNU_HOST_DARWIN" ] && DARWIN_SDK=$(/usr/bin/xcrun --sdk iphoneos --show-sdk-path); }
DYN="xnu-clang -arch $ARCH -isysroot $DARWIN_SDK -O1"
DARWIN_FW=${DARWIN_FW:-../darwin165}   # DARWIN_FW=../darwin26 for the Darwin 26 system (darwin26/fetch.sh)
CACHE=${CACHE:-$DARWIN_FW/20F66__iPhone10,3_6}
# darwin165/rootfs and darwin165/cryptex must be this copy's own directories of hard links, the way
# tools/new-workspace.sh makes them. A symlink to another copy's tree makes sysroot a symlink too, because
# cp -a does not follow one, and then every rm -rf and mv below edits the shared extraction in place. That
# happened on 19 October 2026: 491 LaunchDaemons, NanoLaunchDaemons, LaunchAngels, xpc/launchd.plist and
# private/var went out of the main copy's tree, and only the hard links in the other copies still held them.
for d in rootfs cryptex cryptex-app; do
	[ -L $DARWIN_FW/$d ] || continue
	echo "build.sh: darwin165/$d is a symlink to $(readlink $DARWIN_FW/$d)" >&2
	echo "build.sh: it has to be a directory of this copy's own: rm it and cp -al the original over," >&2
	echo "build.sh: as tools/new-workspace.sh does. Building against a link damages the shared tree." >&2
	exit 1
done
# The stock Darwin jobs given to launchd, roughly in the order they were brought up.
DARWIN_JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
"}
# Darwin's own Wi-Fi daemon, for Settings to have a Wi-Fi to show: the kernel answers it in
# bsd/dev/board/apple80211.c. With the radio (wifi=1) Settings then shows Wi-Fi on, the network joined and the
# networks the radio hears. NOT FINISHED: a network cannot be joined from Settings yet.
# It is in by default now, because it costs nothing where there is no radio at all: in QEMU it comes up, talks
# to itself about country codes and sits there -- 114 processes started against 119 without it, no crash, never
# restarted -- and it answers what the rest of the system keeps asking for anyway. com.apple.wifi.manager, which
# wifid registers itself rather than declaring in its plist, fell from 30 failed lookups a session to 3,
# com.apple.private.corewifi-xpc from 1 to none, and the session's whole failed-lookup count from 331 to 305.
# (Count a QEMU run log with its duplicate lines removed: run-darwin.sh prints launchd's log twice.)
# WIFID=0 leaves it out.
[ "${WIFID-1}" != 0 ] && DARWIN_JOBS="$DARWIN_JOBS com.apple.wifid"
# The programs (file names) started with a library of ours inside them: "LIBRARY NAME..." per line
# (src/inject.c is libhakios.dylib, src/inject_sb.c is libhakios-sb.dylib, src/inject_media.c is
# libhakios-media.dylib).
# What Sileo runs from /bin (see the package manager's prefix below): the system image's /bin, or the
# boot volume's bin, bound over the Darwin volume's /bin when that is the root (init/init.c, flash_root).
SILEO_BIN="chmod cp mkdir mv rm"
# (libhakios-tweaks is the tweak loader of the package manager's prefix, ../../components/tweaks/inject_tweaks.c: in
# SpringBoard the library above opens it, here it is named for the programs that have no library of
# ours -- Settings, where a tweak's own pane belongs. Add a program's name to have tweaks in it.)
# Darwin 26: only src/inject26.c, in backboardd (the touch screen and the buttons); the Darwin 16.5 libraries
# patch 16.5's addresses
case "$(basename "$DARWIN_FW")" in darwin26*) DARWIN26=1 ;; *) DARWIN26=0 ;; esac
if [ "$DARWIN26" = 1 ]; then
	INJECT=${INJECT-"/hakios/lib/libhakios26.dylib backboardd SpringBoard PosterBoard"}
fi
INJECT=${INJECT-"/hakios/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system
/hakios/lib/libhakios-sb.dylib SpringBoard
/hakios/lib/libhakios-media.dylib mediaserverd
/hakios/lib/libhakios-app.dylib Sileo
/hakios/lib/libhakios-tweaks.dylib Preferences"}
# STOREFRONT=1: and a library of ours inside appstored as well (../../components/apple-id/first-generation/inject_store.c). It is not in the
# list above because it changes what the daemon believes: AMS's account is made to answer with the
# storefront Apple's bag server returned for this device, which is how the App Store is asked whether
# a storefront is the only thing it is missing. It also writes down whether a bag request is ever
# built at all. For an experiment, not for an image anybody else boots.
[ -n "$STOREFRONT" ] && INJECT="$INJECT
/hakios/lib/libhakios-store.dylib appstored AppStore
/hakios/lib/libhakios-fairplay.dylib fairplayd.H2"
mkdir -p obj

if [ -z "$PREBUILT" ] && ! command -v ld64 >/dev/null; then PREBUILT=1; fi
if [ -n "$PREBUILT" ]; then
	echo "build.sh: not compiling, using the programs in prebuilt/userland"
	for f in ../prebuilt/userland/*; do
		# keep a file that did not change (and its time stamp): system.apfs depends on stage0
		cmp -s "$f" "obj/$(basename "$f")" || cp "$f" obj/
	done
	cp obj/hello hello
else
	$CC $LD -o hello src/hello.c
	# (process 1 and the first programs: init/ at the top of the repository; their libc: lib/libmini)
	$CC -I../../lib/libmini -c ../../lib/libmini/mini.c -o obj/mini.o
	$CC -c ../../lib/libmini/start.S -o obj/start.o
	for prog in init child stage0 stage0dsu var_store; do
		$CC -I../../lib/libmini $LD -o obj/$prog.new ../../init/$prog.c obj/mini.o obj/start.o
		# keep the old file (and its time stamp) when nothing changed: system.apfs depends on stage0
		cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
	done
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
# when the kernel starts from this small image with an Android DSU system volume, its
# /sbin/launchd mounts the system volume from the phone's storage on /flash and makes it the root
[ -f obj/stage0dsu ] && cp obj/stage0dsu root/sbin/launchd
mkdir -p root/flash
cp obj/child root/bin/child
# The data volume into the flash and back (init/var_store.c), and the job that saves it when a run
# ends: the program and its job together, away from the Darwin job list below, which several working
# copies are editing at once.
[ -f obj/var_store ] && cp obj/var_store root/bin/
cp launchd/varstore.plist root/LaunchDaemons/com.hakios.varstore.plist
echo "HakiOS: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
# 300000 bytes, byte i = (i*31 + (i>>8)) & 0xff; init verifies it through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin
# EXTRA_SHARE: whole directories of data for a one-off diagnostic, each under its own name. Here and
# not in root/bin, because what goes in is not a program: a generated test suite is modules and the
# bytes they read, and the job list runs it by its own run.sh. -L so that a directory assembled out
# of symbolic links arrives as files, since the image is made from this tree and not from the links'
# targets.
for d in $EXTRA_SHARE; do
	[ -d "$d" ] || { echo "build.sh: EXTRA_SHARE: $d is not a directory"; exit 1; }
	cp -rL "$d" root/share/
done
if [ -d $DARWIN_FW/rootfs/usr/lib ]; then
	if [ -z "$PREBUILT" ]; then
		# ordinary dynamically linked programs of our own, built against the Darwin SDK
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		# an IOSurface of ours imported as a texture of the GPU, written through one view
		# and read through the other (src/drawable_test.c): the check that tells a texture
		# over the surface's own pages from a texture over a copy of them. It goes through
		# src/drawable.m, which is the bridge under test, and dlopens the compiler
		# library the way the driver does.
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface \
			-framework IOKit -o obj/drawable_test src/drawable_test.c src/drawable.m
		$DYN -o obj/rwtouch src/rwtouch.c
		# the kernel's thermal pressure passed on to Darwin (src/thermald.c)
		$DYN -o obj/thermald src/thermald.c
		$DYN -framework CoreFoundation -framework AudioToolbox -framework AVFoundation -lobjc -o obj/sound_test ../../components/audio/sound_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
		$DYN -framework CoreFoundation -framework CoreMotion -framework CoreLocation -lMobileGestalt -o obj/sensors_probe src/sensors_probe.m
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/torch_test src/torch_test.c
		$DYN -O2 -o obj/camera_test ../../components/camera/src/camera_test.c ../../components/camera/src/camera_isp.c
		$DYN -fobjc-arc -framework Foundation -framework AVFoundation -o obj/camera_avtest ../../components/camera/src/camera_avtest.m
		# (the microphone as an application records: allowed to, by an entitlement, if ldid is there)
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/record_test ../../components/audio/record_test.c &&
			{ ${LDID:-ldid} -S../../components/audio/record_test.entitlements obj/record_test 2>/dev/null ||
			  echo "build.sh: no ldid: record_test is unsigned, and the system may not let it record"; }
		$DYN -o obj/cpu_test src/cpu_test.c
		$DYN -framework CoreFoundation -framework IOSurface -o obj/cow_test src/cow_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework IOSurface -o obj/cg_test src/cg_test.c
		# what Darwin here believes about Metal, and what CoreImage does without it (../../components/metal/src/metal_test.m);
		# OpenGLES for the CIContext made over an EAGLContext, which is deprecated and the point
		$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics \
			-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES \
			-lMobileGestalt -o obj/metal_test ../../components/metal/src/metal_test.m
		# the first question of subsystem (2): can a subclass of the private _MTLDevice be
		# made here, and what does its initWithAcceleratorPort: do (../../components/metal/src/metal_dev.m). No ARC:
		# it calls objc_msgSend by hand and allocates a class at runtime.
		$DYN -framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
			-o obj/metal_dev ../../components/metal/src/metal_dev.m
		# a device object that implements nothing and logs what Apple's own init asks it
		# for (../../components/metal/src/metal_stubdev.m); same reasons for no ARC and for -lobjc
		$DYN -framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
			-o obj/metal_stubdev ../../components/metal/src/metal_stubdev.m
		# the device object of the driver itself, as the library a bundle would hold
		# (../../components/metal/src/metal_driver.m). It links against Metal's private _OBJC_CLASS_$__MTLDevice,
		# which is bindable because IOGPU.framework imports the same symbol; -lobjc for
		# class_addProtocol and no ARC because it calls objc_msgSend by hand.
		# (with src/drawable.m beside it: the IOSurface a CAMetalLayer hands a
		# device, as a texture. A file of its own because it answers CoreAnimation and
		# not Metal, and the two meet in one method -- docs/metal.md 10.46.)
		$DYN -dynamiclib -install_name /hakios/lib/libhakios-metal.dylib \
			-framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
			-o obj/libhakios-metal.dylib ../../components/metal/src/metal_driver.m src/drawable.m
		# and the program that walks Metal's own steps 4 and 5 against it
		$DYN -framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
			-o obj/metal_drv_test ../../components/metal/src/metal_drv_test.m
		# An application of this port's own: a window, a CAMetalLayer, a drawable and a
		# triangle in it (src/triangle_app.m). UIKit because that is the point -- every
		# Metal result so far is from inside one of Apple's daemons, and this is an
		# ordinary bundle in /Applications that SpringBoard launches; IOSurface because
		# it reads the drawable's own surface back to say whether the texture WAS those
		# pages; no ARC and -lobjc because it sends -initWithAcceleratorPort: by hand,
		# the way ../../components/metal/src/metal_drv_test.m does. The bundle is assembled further down, where
		# a PREBUILT tree can assemble it too.
		# (signed, if ldid is there, for the entitlements an application has and a
		# /hakios/bin program does not: see src/triangle_app.entitlements, which says
		# why none of them is needed to RUN. Signed here rather than in the bundle, so
		# that the copy in /hakios/bin carries the same signature.)
		$DYN -framework Foundation -framework UIKit -framework QuartzCore \
			-framework Metal -framework IOSurface -framework IOKit -lobjc \
			-o obj/HakiosTriangle src/triangle_app.m &&
			{ ${LDID:-ldid} -Ssrc/triangle_app.entitlements obj/HakiosTriangle 2>/dev/null ||
			  echo "build.sh: no ldid: HakiosTriangle is unsigned, which still runs (AMFI here accepts everything) but holds no entitlements"; }
		# what the device ANSWERS when an application asks what it supports
		# (../../components/metal/src/metal_caps.m). It takes the device from MTLCreateSystemDefaultDevice() so
		# that the ivars behind Apple's inherited answers are filled by the registration
		# Metal itself ran; no ARC and -lobjc because most of what it asks is MTLDeviceSPI,
		# whose header this port has not got, so every send is an objc_msgSend by hand. MetalFX
		# because the acceptance test's own capability gate is +supportsDevice: on its two
		# scaler descriptors, and that gate can be measured without the game.
		$DYN -framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
			-framework MetalFX -o obj/metal_caps ../../components/metal/src/metal_caps.m
		# ... and the switch that steps 1 and 2 need: it puts MetalPluginName and
		# MetalPluginClassName on the IOAcceleratorES service (../../components/metal/src/metal_props.c). IOKit and
		# CoreFoundation only -- a program that flips this switch must not itself ask Metal,
		# whose conditions are asserts. It needs the kernel booted with metal=1.
		$DYN -framework CoreFoundation -framework IOKit -o obj/metal_props ../../components/metal/src/metal_props.c
		# where every thread of a process that hangs is (src/stackof.c), with the entitlement task_for_pid wants
		$DYN -o obj/stackof src/stackof.c
		$DYN -o obj/shm_test src/shm_test.c
		$DYN -o obj/apfs_mmap_test src/apfs_mmap_test.c
		$DYN -o obj/vol_ls src/vol_ls.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o obj/snap_test src/snap_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework CoreText -o obj/font_test src/font_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		# (the account database answers only a client with the entitlements its own daemons hold: ../../components/apple-id/first-generation/store_test.m)
		$DYN -fobjc-arc -framework Foundation -framework Accounts -o obj/store_test ../../components/apple-id/first-generation/store_test.m &&
			{ ${LDID:-ldid} -S../../components/apple-id/first-generation/store_test.entitlements obj/store_test 2>/dev/null ||
			  echo "build.sh: no ldid: store_test is unsigned, and accountsd will not show it the accounts"; }
		# (with an entitlement for "battery_test open URL", if ldid is there: src/battery_test.c)
		$DYN -framework CoreFoundation -framework IOKit -lsqlite3 -o obj/battery_test src/battery_test.c &&
			{ ${LDID:-ldid} -Ssrc/battery_test.entitlements obj/battery_test 2>/dev/null ||
			  echo "build.sh: no ldid: battery_test is unsigned, and SpringBoard will not open URLs for it"; }
		# (the keychain answers only a program that has a keychain access group: signed with one, if ldid is there)
		$DYN -framework CoreFoundation -framework Security -o obj/keychain_test ../../components/sep/keychain_test.c &&
			{ ${LDID:-ldid} -S../../components/sep/keychain_test.entitlements obj/keychain_test 2>/dev/null ||
			  echo "build.sh: no ldid: keychain_test is unsigned, and the keychain will refuse it (-34018)"; }
		# (and wifid's way of keeping a network's password, by wifid's name: src/wifipw_test.c)
		$DYN -framework CoreFoundation -framework Security -o obj/wifipw_test src/wifipw_test.c &&
			{ ${LDID:-ldid} -Ssrc/wifipw_test.entitlements obj/wifipw_test 2>/dev/null ||
			  echo "build.sh: no ldid: wifipw_test is unsigned, and the keychain will refuse it (-34018)"; }
		# our library for inside stock Darwin programs (the jobs in INJECT are started with it)
		$DYN -dynamiclib -install_name /hakios/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios.dylib src/inject.c
		$DYN -dynamiclib -install_name /hakios/lib/libhakios-sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios-sb.dylib src/inject_sb.c
		$DYN -dynamiclib -install_name /hakios/lib/libhakios26.dylib -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
			-Wl,-U,_IOMobileFramebufferSwapBegin -Wl,-U,_IOMobileFramebufferSwapEnd -Wl,-U,_IOMobileFramebufferSwapSetLayer \
			-Wl,-U,_IOMobileFramebufferSwapSetEventWait -Wl,-U,_IOMobileFramebufferSwapSetEventSignal \
			-o obj/libhakios26.dylib src/inject26.c &&   # (the framework's five calls: dyld finds them in backboardd, which has it)
			/usr/bin/codesign -f -s - obj/libhakios26.dylib   # Darwin 26's dyld: "missing code signature" otherwise
		# ... the tweak loader of the package manager's prefix (../../components/tweaks/inject_tweaks.c): it goes into a
		# program either as the one library the kernel inserts or opened by a library of ours
		$DYN -dynamiclib -install_name /hakios/lib/libhakios-tweaks.dylib -framework CoreFoundation -lobjc \
			-o obj/libhakios-tweaks.dylib ../../components/tweaks/inject_tweaks.c
		# ... and inside an application, to say what killed it (src/inject_app.c)
		$DYN -dynamiclib -install_name /hakios/lib/libhakios-app.dylib -lobjc \
			-o obj/libhakios-app.dylib src/inject_app.c
		$DYN -O2 -dynamiclib -install_name /hakios/lib/libhakios-media.dylib -framework CoreFoundation -framework CoreMedia \
			-framework CoreVideo -framework IOSurface -lobjc \
			-o obj/libhakios-media.dylib src/inject_media.c ../../components/camera/src/camera_device.c ../../components/camera/src/camera_isp.c
		# ... and inside appstored (STOREFRONT=1), which answers with a storefront and watches for a bag
		# request being built (../../components/apple-id/first-generation/inject_store.c)
		# (-lc++abi for __cxa_throw: every C++ exception thrown in the process is watched, which is how
		#  the jetpack parser's rejections are seen -- a throw somebody catches logs nothing of itself)
		$DYN -O2 -dynamiclib -install_name /hakios/lib/libhakios-store.dylib -framework CoreFoundation -lobjc \
			-lc++abi -o obj/libhakios-store.dylib ../../components/apple-id/first-generation/inject_store.c
		# ... and inside fairplayd.H2, which starts and leaves at once: what it looks for, and where it
		# gives up (../../components/apple-id/first-generation/inject_fairplay.c; watches only, replaces nothing)
		$DYN -O2 -dynamiclib -install_name /hakios/lib/libhakios-fairplay.dylib -framework CoreFoundation \
			-framework IOKit -framework Security -lMobileGestalt \
			-o obj/libhakios-fairplay.dylib ../../components/apple-id/first-generation/inject_fairplay.c
		# the output device of the audio server: a plug-in for CoreAudio's HAL
		$DYN -bundle -framework CoreFoundation -o obj/HakiosAudio ../../components/audio/audio_plugin.c
		# The same driver again as a loadable bundle, which is the form Metal's own
		# discovery wants: step 3 builds the path /System/Library/Extensions/<name>.bundle
		# and opens it with NSBundle, and -[NSBundle classNamed:] then tests that the class
		# came out of that bundle (0x1811e0dac). The dylib above cannot serve as the
		# executable: dyld uniques images by install_name, so a process that already has
		# /hakios/lib/libhakios-metal.dylib would bind the bundle to it and fail that test.
		$DYN -bundle -framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
			-o obj/HakiosMetal ../../components/metal/src/metal_driver.m src/drawable.m
		if [ -n "$GL" ]; then
			# the OpenGL ES library, and the table of functions that hands CoreAnimation's calls to it
			# (Mesa says beside the library what it was built from: one from other patches is built again)
			M=../xnu-deps/build/mesa/out
			[ "$(cat $M/built-from 2>/dev/null)" = "$(cat ../../components/metal/tools/mesa/*.patch ../../components/metal/tools/mesa/darwin-drm/* | sha256sum)" ] ||
				../../components/metal/tools/mk-mesa.sh
			# ... and the compiler library separately, because the hash above covers the PATCHES
			# and the library is built from ../../components/metal/tools/mesa/air/, which they do not mention. Two boots
			# of 22 October carried a libhakios-air.dylib three hours older than the driver beside
			# it while rwa_render_shape had grown IN THE MIDDLE -- the image was the mismatch the
			# contract warns about, and nothing in it said so. Silence is what made it expensive:
			# the library is stripped, so the driver found the symbols it wanted and read a stride
			# where a pixel format was.
			[ "$(cat $M/built-from-air 2>/dev/null)" = "$(cat ../../components/metal/tools/mesa/air/* | sha256sum)" ] || {
				echo "build.sh: ../../components/metal/tools/mesa/air/ has changed since libhakios-air.dylib was built"
				AIR=1 ../../components/metal/tools/mk-mesa.sh
			}
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -dynamiclib -install_name /hakios/lib/libhakios-gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libhakios-gl.dylib \
				src/eagl.c src/gl_osmesa.c
			$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
			# ... and what this GPU makes of an unsigned integer turned into a float (src/u2f_test.c)
			$DYN $M/libOSMesa.8.dylib -o obj/u2f_test src/u2f_test.c
			cp $M/libOSMesa.8.dylib obj/
			# air_run draws with a Metal shader translated by the AIR frontend
			# and reads the pixels back (../../components/metal/docs/air.md). It is built inside the
			# Mesa build, because it needs Gallium itself and not the gl*
			# entry points this library exports; AIR=1 ../../components/metal/tools/mk-mesa.sh makes
			# it, and it is copied in when it is there.
			[ -f $M/air_run ] && cp $M/air_run obj/air_run
			# The compiler the Metal driver asks for: AIR in, a dispatch on the
			# Adreno out (../../components/metal/tools/mesa/air/air_metal.h). It is copied whether or not
			# GL is wanted, because ../../components/metal/src/metal_driver.m dlopens it by that path --
			# and its absence is a sentence the driver prints, not a crash.
			[ -f $M/libhakios-air.dylib ] && cp $M/libhakios-air.dylib obj/
		fi
	fi
	# (drawable_test is built in the same block as surf_test, so it is copied with it)
	cp obj/hello_dyn obj/svc_test obj/surf_test obj/drawable_test obj/rwtouch obj/mg_test obj/sensors_probe root/bin/
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/        # (a prebuilt/ from before this program has none)
	[ -f obj/thermald ] && cp obj/thermald root/bin/
	[ -f obj/cow_test ] && cp obj/cow_test root/bin/
	[ -f obj/cg_test ] && cp obj/cg_test root/bin/
	[ -f obj/metal_dev ] && cp obj/metal_dev root/bin/
	[ -f obj/metal_stubdev ] && cp obj/metal_stubdev root/bin/
	[ -f obj/metal_drv_test ] && cp obj/metal_drv_test root/bin/
	[ -f obj/metal_caps ] && cp obj/metal_caps root/bin/
	[ -f obj/metal_props ] && cp obj/metal_props root/bin/
	if [ -f obj/stackof ]; then
		cp obj/stackof root/bin/
		/usr/bin/codesign -f -s - --entitlements src/stackof.entitlements root/bin/stackof 2>/dev/null || true
	fi
	[ -f obj/shm_test ] && cp obj/shm_test root/bin/
	[ -f obj/apfs_mmap_test ] && cp obj/apfs_mmap_test root/bin/
	[ -f obj/vol_ls ] && cp obj/vol_ls root/bin/
	[ -f obj/snap_test ] && cp obj/snap_test root/bin/
	[ -f obj/font_test ] && cp obj/font_test root/bin/
	[ -f obj/metal_test ] && cp obj/metal_test root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	[ -f obj/store_test ] && cp obj/store_test root/bin/
	[ -f obj/battery_test ] && cp obj/battery_test root/bin/
	[ -f obj/keychain_test ] && cp obj/keychain_test root/bin/
	[ -f obj/wifipw_test ] && cp obj/wifipw_test root/bin/
	[ -f obj/sound_test ] && cp obj/sound_test root/bin/
	[ -f obj/torch_test ] && cp obj/torch_test root/bin/
	[ -f obj/camera_test ] && cp obj/camera_test root/bin/
	[ -f obj/camera_avtest ] && cp obj/camera_avtest root/bin/
	[ -f obj/record_test ] && cp obj/record_test root/bin/
	# The same binary as the application below, under /hakios/bin, for its --layer mode:
	# a CAMetalLayer with no window and no UIKit, driven once. That mode cannot say whether
	# a frame reaches the panel, and it answers the two questions that come before it -- does
	# the driver work outside Apple's daemons, and does nextDrawable give a texture -- in a
	# process whose output is certainly in the log, because the job list runs it. An
	# application launched by SpringBoard is a process this file cannot promise that about.
	[ -f obj/HakiosTriangle ] && cp obj/HakiosTriangle root/bin/triangle_app
	for f in $EXTRA; do cp "$f" root/bin/; done
	#
	# ... and the application bundle itself, staged in obj/ and copied into
	# sysroot/Applications by the system image below.
	#
	# A PLAIN DIRECTORY OF REAL FILES, and that is not a style: a bundle made of symbolic
	# links is refused by LaunchServices ("Unable to register"), and the registrar that
	# walks /Applications at boot passes over mount points. So the icons are generated here
	# rather than linked from anywhere, and the plist and the executable are copied.
	if [ -f obj/HakiosTriangle ]; then
		rm -rf obj/HakiosTriangle.app
		mkdir -p obj/HakiosTriangle.app
		cp obj/HakiosTriangle obj/HakiosTriangle.app/
		cp src/triangle_app.plist obj/HakiosTriangle.app/Info.plist
		printf 'APPL????' > obj/HakiosTriangle.app/PkgInfo
		# The icon is the picture the application draws: an orange triangle over the
		# lower left half of a blue square, by the same rule the shader covers a pixel
		# by ((x+0.5)/w < (y+0.5)/h). So the home screen states the prediction, and an
		# icon that comes out with its halves the other way round says the arithmetic in
		# this one line disagrees with the arithmetic in src/triangle_app.m. No alpha
		# channel: Darwin masks an icon itself and an icon with alpha comes out wrong.
		python3 - obj/HakiosTriangle.app <<-'PY'
		import struct, sys, zlib
		out = sys.argv[1]
		for size, scale in ((120, 2), (180, 3)):
		    rows = bytearray()
		    for y in range(size):
		        rows.append(0)                        # PNG filter type: none
		        for x in range(size):
		            inside = (2 * x + 1) * size < (2 * y + 1) * size
		            rows += bytes((255, 128, 0) if inside else (0, 0, 255))
		    def chunk(kind, body):
		        return (struct.pack('>I', len(body)) + kind + body +
		                struct.pack('>I', zlib.crc32(kind + body)))
		    png = (b'\x89PNG\r\n\x1a\n' +
		           chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
		           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
		    open('%s/AppIcon60x60@%dx.png' % (out, scale), 'wb').write(png)
		PY
		# Unsigned, which is enough: AMFI in this kernel accepts every signature and
		# holds no trust cache (libkern/amfi/miniamfi.c), and the kernel is built without
		# config_enforce_signed_code. What a signature would still buy is entitlements,
		# and this application asks the system for nothing that needs one.
		echo "  HakiosTriangle.app: $(du -sk obj/HakiosTriangle.app | cut -f1) KB for /Applications"
	fi
	# /dev and /private/var exist so that this image can also be the root on its own:
	# devfs is mounted on /dev, and init puts a ramfs on /private/var
	mkdir -p root/dev root/private/var
	mkdir -p root/lib
	cp obj/libhakios.dylib obj/libhakios-sb.dylib obj/libhakios26.dylib root/lib/
	[ -f obj/libhakios-media.dylib ] && cp obj/libhakios-media.dylib root/lib/     # (as with cpu_test)
	[ -f obj/libhakios-store.dylib ] && cp obj/libhakios-store.dylib root/lib/
	[ -f obj/libhakios-fairplay.dylib ] && cp obj/libhakios-fairplay.dylib root/lib/
	[ -f obj/libhakios-app.dylib ] && cp obj/libhakios-app.dylib root/lib/
	[ -f obj/libhakios-tweaks.dylib ] && cp obj/libhakios-tweaks.dylib root/lib/
	[ -f obj/libhakios-metal.dylib ] && cp obj/libhakios-metal.dylib root/lib/
	[ -f obj/libhakios-air.dylib ] && cp obj/libhakios-air.dylib root/lib/
	# Darwin 26: the Metal driver's compiler and its test program from Mesa 26
	# (../../components/metal/tools/mk-mesa26.sh), which replace the Mesa 25 ones above when they have been built
	M26=../xnu-deps/build/mesa26/out
	if [ -f $M26/libhakios-air.dylib ]; then
		[ "$(cat $M26/built-from-air 2>/dev/null)" = "$(cat ../../components/metal/tools/mesa/air/* | shasum -a 256)" ] ||
			echo "build.sh: WARNING: ../../components/metal/tools/mesa/air/ changed since $M26/libhakios-air.dylib was built (../../components/metal/tools/mk-mesa26.sh)"
		cp $M26/libhakios-air.dylib root/lib/
		mkdir -p root/bin
		cp $M26/air_run root/bin/
		# tools/air/glasscase: tools/air/glassref's draws again on the phone's GPU (rc.launchd26.glasscase)
		$DYN -I../../components/metal/tools/mesa/air -o obj/glasscase ../../components/metal/tools/air/glasscase.c -L$M26 -lhakios-air &&
			cp obj/glasscase root/bin/
	fi
	# the pipelines a metal_bisect run skips one after another (../../components/metal/src/metal_driver.m), when there is a list
	if [ -f bisect.txt ]; then cp bisect.txt root/bisect.txt; else rm -f root/bisect.txt; fi
	if [ -f "${TAPS:-taps.txt}" ]; then cp "${TAPS:-taps.txt}" root/taps.txt; else rm -f root/taps.txt; fi     # boot-arg taps=1 (inject26.c); TAPS= another script
	if [ -f obj/HakiosAudio ]; then
		mkdir -p root/lib/HakiosAudio.driver
		cp obj/HakiosAudio root/lib/HakiosAudio.driver/
		cp ../../components/audio/audio_plugin.plist root/lib/HakiosAudio.driver/Info.plist
	fi
	# The Metal plug-in bundle, flat and unsigned -- which is enough here: AMFI in this
	# kernel is a stand-in that accepts every signature (libkern/amfi/miniamfi.c), and
	# Apple's own code already loads the unsigned HakiosAudio.driver above. The system image
	# now links this bundle into /System/Library/Extensions as well (see below), but the two
	# properties that would make Metal look for it are still nobody's by default: only a boot
	# with metal=1 lets a process put them on the service (docs/metal.md 10.31), because 10.14
	# measured what a half-made device does to every process that asks Metal for one.
	if [ -f obj/HakiosMetal ]; then
		mkdir -p root/lib/HakiosMetal.bundle
		cp obj/HakiosMetal root/lib/HakiosMetal.bundle/
		cp ../../components/metal/src/metal_plugin.plist root/lib/HakiosMetal.bundle/Info.plist
	fi
	if [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		if [ -f obj/u2f_test ]; then cp obj/u2f_test root/bin/; fi   # an "if": see the inject.conf loop below
		[ -f obj/air_run ] && cp obj/air_run root/bin/
		[ "$GL" = test ] || cp obj/libhakios-gl.dylib root/lib/
	fi
	# a program is not started with a library that is not there: it would not start at all
	echo "$INJECT" | while read -r library names; do
		# a /hakios/lib library only if it was actually built; any other path (e.g. a tweak loader
		# in /var/jb) as it is, since it lives on the prefix and is there at run time
		case "$library" in
		"") ;;
		# an "if" and not "&& echo": a last line whose library is absent would make the loop,
		# and with it the pipeline and the whole build under set -e, fail -- which is what a tree
		# with only prebuilt/userland in it does, since not every injection library is shipped there
		/hakios/lib/*) if [ -f "root/lib/${library#/hakios/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf
	cp $DARWIN_FW/var.skel root/etc/var.skel              # the directories init creates in /private/var
	# time zones: the template's links go in a circle until the time zone daemon has run (/usr/share/zoneinfo
	# points back into /var); point them at the data that ships with the system, and use this machine's zone
	zone=$(timedatectl show -p Timezone --value 2>/dev/null || readlink /etc/localtime 2>/dev/null | sed 's#.*/zoneinfo/##' || true); zone=${zone:-UTC}
	sed -i -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|' \
		-e "s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime\$|l\t/var/db/timezone/zoneinfo/$zone\t/private/var/db/timezone/localtime|" root/etc/var.skel
	# ... and the files of ours it puts there (var/): those under mobile/ belong to the user mobile
	cp -r var root/var
	# Darwin 26: the setup assistant marked as done the way Darwin 26's own SetupAssistant and SpringBoard read it
	# (SetupDone, SetupFinishedAllSteps, SetupVersion, SetupLastExit -- the keys in their binaries). With Darwin
	# 16.5's file (SetupVersion 11, no SetupFinishedAllSteps) unlocking started Setup.app, whose screen the
	# software renderer cannot draw: backboardd ran out of stack in CoreAnimation and SpringBoard went with it.
	if [ "$DARWIN26" = 1 ]; then
		python3 - root/var/mobile/Library/Preferences/com.apple.purplebuddy.plist <<'PY'
import datetime, plistlib, sys
p = sys.argv[1]
d = plistlib.load(open(p, 'rb'))
d.update(SetupDone=True, SetupFinishedAllSteps=True, SetupVersion=99, ForceNoBuddy=True,
         SetupState='SetupUsingAssistant', SetupLastExit=datetime.datetime(2026, 10, 1))
plistlib.dump(d, open(p, 'wb'), fmt=plistlib.FMT_BINARY)
PY
	fi
	# the data migration is marked done for the build in the image, not for Darwin 16.5's: with another
	# LastSystemVersion Darwin 26 runs every migration plugin at boot, and SpringBoard waits for them
	# (BKSSystemShellService ... waitForDataMigration); Darwin 26 also reads it as com.apple.DataMigration
	build=$(python3 -c "import plistlib; print(plistlib.load(open('$DARWIN_FW/BuildManifest.plist','rb'))['ProductBuildVersion'])" 2>/dev/null)
	if [ -n "$build" ]; then
		p=root/var/mobile/Library/Preferences
		sed -i -e "s/20F66/$build/g" $p/com.apple.migration.plist
		cp $p/com.apple.migration.plist $p/com.apple.DataMigration.plist
		# ... and for root: Darwin 26's datamigrator is a system XPC service, and reads com.apple.migration as root
		mkdir -p root/var/root/Library/Preferences
		cp $p/com.apple.migration.plist root/var/root/Library/Preferences/com.apple.migration.plist
		# ... and where it really looks: DMEnvironment lastBuildVersionPref asks for LastSystemVersion of
		# CurrentUser + CurrentHost, i.e. Library/Preferences/ByHost/<domain>.<host id>.plist, and Darwin 26's
		# host id (_CFXPreferencesGetByHostIdentifierString) is the empty string
		for home in root/var/root root/var/mobile; do
			mkdir -p $home/Library/Preferences/ByHost
			cp $p/com.apple.migration.plist "$home/Library/Preferences/ByHost/com.apple.migration..plist"
			cp $p/com.apple.migration.plist "$home/Library/Preferences/ByHost/com.apple.migration.plist"
		done
		# ... and Messages' own record of it: IMDPersistence posts no banner for a message until
		# kIMDPersistenceLastBuild in com.apple.imdsmsrecordstore (read for the current host, so in both places as
		# above) is this build, which Messages' migration plug-in writes and nothing here runs. After the idea of
		# the OnePlus Nord 2 port's 0277y (frxncyyy/ios-denniz), found on its iOS 16.7; not yet seen on ours.
		python3 - "$p" "$build" <<'PY'
import plistlib, sys
for name in ('com.apple.imdsmsrecordstore.plist', 'ByHost/com.apple.imdsmsrecordstore..plist'):
    with open(sys.argv[1] + '/' + name, 'wb') as f:
        plistlib.dump({'kIMDPersistenceLastBuild': sys.argv[2]}, f, fmt=plistlib.FMT_BINARY)
PY
	fi
	# (the ByHost directories, which the image's own skeleton does not have, before the files in them)
	printf 'd\t755\t0\t0\t/private/var/root/Library/Preferences/ByHost\nd\t755\t501\t501\t/private/var/mobile/Library/Preferences/ByHost\n' >> root/etc/var.skel
	(cd root/var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel
	# mobile's own temporary directory, $HOME/tmp, which Darwin's template does not have: an application of
	# mobile's without a data container of its own keeps its temporary files there -- Sileo, registered from
	# the prefix with the root on the flash, downloads every package and index there, and without it each
	# download ended in "No such file or directory"
	printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> root/etc/var.skel
	# A key of ours for root, when there is one: ssh/authorized_keys, never in the repository (like
	# fw-*). dropbear from the package manager's prefix reads it and lets us in over the phone's
	# Wi-Fi, which saves rebuilding an image for every question one wants to ask a running Darwin. It
	# refuses a key whose directory anyone but its owner may write, hence the 700.
	# SHARE=1: no keys in an image that is going to somebody else. ssh_host_ed25519_key is a
	# private key, and it would be leaving this machine inside a 2.5 GB container
	if [ -z "$SHARE" ] && [ -f ../ssh/authorized_keys ]; then
		mkdir -p root/var/root/.ssh
		printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> root/etc/var.skel
		# the key that lets us in, and the host's own key beside it: made here and not on the
		# phone, because ssh-keygen asks who uid 0 is and gets "No user exists for uid 0" --
		# it asks through libiosexec, which reads a database the bootstrap does not ship (see
		# init.c, where it is now built at every boot); the key is made here all the same,
		# since nothing should depend on the order of those two
		for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
			[ -f ../ssh/$k ] || continue
			cp ../ssh/$k root/var/root/.ssh/$k
			printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/var/root/.ssh/%s\n' $k $k >> root/etc/var.skel
		done
	fi
	cp ${RC:-rc.launchd} root/etc/rc
	cp ${RC_JOBS:-rc.jobs} root/etc/rc.jobs
	# QUIET=1: launchd's own standard error goes nowhere (init/init.c). It says a few thousand
	# lines about its own business in the first minutes and pushes everything else out of the
	# kernel's log; the daemons keep their own standard error on the console all the same.
	rm -f root/etc/quiet
	[ -n "$QUIET" ] && : > root/etc/quiet
	# the jobs the Darwin launchd will see (/System/Library/LaunchDaemons is a symlink to this directory).
	# With FLASHBOOT=1 the root will be Apple's whole volume on the flash, not the part of it that fits in
	# the system image, so mkjobs.py judges "is this daemon's program there" by the listing of the volume
	# and not by what select-system.py extracted here (--root-volume; seven jobs turn on the difference,
	# fairplayd among them).
	# (launchd/JOB.plist, in the image as LaunchDaemons/LABEL.plist: the name launchd's own jobs have)
	for job in demo wifi thermald; do cp launchd/$job.plist root/LaunchDaemons/com.hakios.$job.plist; done
	VOLUME=""
	[ -n "$FLASHBOOT" ] && [ -f $DARWIN_FW/rootfs.slt ] && VOLUME="--root-volume $DARWIN_FW/rootfs.slt"
	[ -n "$DARWIN_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $VOLUME $DARWIN_FW/rootfs root/LaunchDaemons $DARWIN_JOBS
fi
# firmware of the phone's own hardware, where somebody has pulled it from the phone's Android into ../fw-gpu
# (it is the vendor's and never in the repository): the kernel's GPU bring-up, bsd/dev/board/adreno.c, finds it in fw/
if [ -d ../fw-gpu ]; then
	mkdir -p root/fw
	cp ../fw-gpu/* root/fw/
fi
# ... and the image of the Wi-Fi DSP, from /vendor/firmware_mnt/image of the same Android into ../fw-wifi
# (bsd/dev/board/wifi.c; 7.6 MB), with what the radio's firmware is sent once it runs: the phone's
# board data and regulatory database (bd*, regdb*, in image/ or a directory of the chip's there), of
# which the kernel takes those the board's wifi { } names (board-data, regdb)
if [ -f ../fw-wifi/wpss.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/wpss.mdt ../fw-wifi/wpss.b0[2-9] root/fw/
	for f in ../fw-wifi/bd* ../fw-wifi/regdb* ../fw-wifi/*/bd* ../fw-wifi/*/regdb*; do
		if [ -f "$f" ]; then install -m 0644 "$f" root/fw/; fi
	done
	# the network the radio is to join: its name on the first line, its passphrase on the second. Yours, and
	# so not in the repository; it goes where the firmware goes and the kernel reads it there (wifi_join=1)
	rm -f root/fw/network
	# SHARE=1: leave it out, for an image that is going to somebody else -- it is a passphrase
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ]; then install -m 0600 ../fw-wifi/network root/fw/network; fi
fi
# ... and the vibration motor's waveforms, the phone's /vendor/firmware/aw8697_haptic.bin, from ../fw-haptic
# (bsd/dev/board/haptic.c, haptic=3; 3.6 KB, the vendor's and not in the repository either)
if [ -f ../fw-haptic/aw8697_haptic.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-haptic/aw8697_haptic.bin root/fw/
fi
# ... and the Bluetooth controller's patch and configuration (bsd/dev/board/bt_uart.c, bt=2: the files bluetooth { }
# names, rampatch and nvm, msbtfw*.mbn or .tlv and msnv*.bin on a WCN6750), the phone's /vendor/bt_firmware/image, from
# ../fw-bt (the vendor's, not in the repository): all of what is there, the board says which it sends
for f in ../fw-bt/*; do
	[ -f "$f" ] || continue
	mkdir -p root/fw
	install -m 0644 "$f" root/fw/
done
# ... and the cameras' register tables (bsd/dev/board/camera.c, cam=2 and up), made by ../components/camera/tools/camsensor.py out of
# the sensors' module files of Android's camera stack, the phone's /vendor/lib64/camera/com.qti.sensormodule.
# <phone>_<maker>_<sensor>_<camera>.bin, in ../fw-camera (the vendor's: neither they nor the tables are in the
# repository): camera-<sensor>.tab
for module in ../fw-camera/com.qti.sensormodule.*.bin; do
	[ -f "$module" ] || continue
	sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.[a-z0-9]*_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../../components/camera/tools/camsensor.py "$module" "root/fw/camera-$sensor.tab" >/dev/null
done
# ... and their colour tuning (libhakios-media's processor, ../../components/camera/src/camera_isp.c), made by ../components/camera/tools/camtuning.py out of the
# cameras' tuning files beside them, com.qti.tuned.<phone>_<maker>_<sensor>_<camera>.bin: camera-<sensor>.isp
for tuned in ../fw-camera/com.qti.tuned.*.bin; do
	[ -f "$tuned" ] || continue
	sensor=$(basename "$tuned" .bin | sed 's/^com\.qti\.tuned\.[a-z0-9]*_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../../components/camera/tools/camtuning.py "$tuned" "root/fw/camera-$sensor.isp" >/dev/null
done
# ... and with ADSP=1 the image of the audio DSP, from the same place into ../fw-audio (bsd/dev/board/adsp.c):
# adsp.mdt and adsp.b02 to adsp.b36, 20 MB. Only when asked for: beside GL=1 the payload would be more than the 64 MB
# the loader takes.
if [ -n "$ADSP" ] && [ -f ../fw-audio/adsp.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] root/fw/
fi
# Firmware that is not there is otherwise silent at build time and speaks only much later, deep in a
# device log -- "adreno: THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin" cost a phone
# slot on 19 October 2026. It is the vendor's and never in the repository, so a copy made with bare
# git worktree add has none of it, and neither has one whose tools/new-workspace.sh predates the fw-*
# loop there. Say what went in and what was not there to go in; what each one is for is above.
fwhave='' fwnone=''
for d in gpu wifi haptic camera audio bt; do
	if [ -d ../fw-$d ]; then fwhave="$fwhave $d"; else fwnone="$fwnone $d"; fi
done
echo "build.sh: fw/: $(ls -1 root/fw 2>/dev/null | wc -l) files from ../fw-*:${fwhave:- none present}${fwnone:+, absent:$fwnone}"
# The images are what somebody would be handed to run this on a second phone, and two of the things
# build.sh puts in them are secrets: the Wi-Fi network's name and passphrase, and the host's own
# private key for ssh. Both are one small file inside 2.5 GB, and nobody would notice either leave.
secrets=
[ -f root/fw/network ] && secrets="$secrets fw/network (the name and passphrase of your Wi-Fi)"
[ -f root/var/root/.ssh/ssh_host_ed25519_key ] &&
	secrets="$secrets /var/root/.ssh/ssh_host_ed25519_key (a private key)"
if [ -n "$secrets" ]; then
	echo "build.sh: this image carries:$secrets"
	echo "build.sh: do not pass it on as it is; SHARE=1 builds the same image without them."
fi
# When the Darwin volume on the phone's flash is to become the root (boot-arg root_apfs=ufs_darwin,
# with flashboot.apfs resident; ../../init/init.c, flash_root): an empty directory to hide Darwin's
# NanoLaunchDaemons and LaunchAngels with,
# xpc without launchd.plist (launchd would read that list of all of Darwin's jobs instead of
# LaunchDaemons) and the audio server's plug-in directory with only our device in it.
mkdir -p root/empty root/xpc root/hal
for f in extensions.plist extensions.plist.sig; do
	[ -f $DARWIN_FW/rootfs/System/Library/xpc/$f ] && cp $DARWIN_FW/rootfs/System/Library/xpc/$f root/xpc/
done
[ -d root/lib/HakiosAudio.driver ] && ln -s /hakios/lib/HakiosAudio.driver root/hal/HakiosAudio.driver
# Darwin 26's dyld loads no arm64e code without a code signature ("missing code signature"), whatever
# this kernel's AMFI would make of it: every Mach-O of ours that has none gets an ad-hoc one. One that
# is signed already (with entitlements, by its own rule above) is left as it is.
if [ "$ARCH" = arm64e ]; then
	xattr -cr root 2>/dev/null || true      # (codesign refuses a bundle with extended attributes: "detritus not allowed")
	# a bundle is signed as a whole (its executable, sealed with its Info.plist). Signed in a temporary
	# directory: this tree may be in a folder that a file provider (iCloud Drive's Desktop) keeps putting
	# Finder attributes on, which codesign refuses in a bundle ("detritus not allowed")
	find root -type d \( -name '*.bundle' -o -name '*.driver' -o -name '*.app' \) | while read -r b; do
		/usr/bin/codesign -v "$b" 2>/dev/null && continue
		t=$(mktemp -d /tmp/hakios-sign.XXXXXX)
		/bin/cp -RX "$b" "$t/"
		xattr -cr "$t" 2>/dev/null || true     # (com.apple.provenance does not come off, and need not)
		/usr/bin/codesign -f -s - "$t/${b##*/}" 2>/dev/null &&
			rm -rf "$b" && /bin/cp -RX "$t/${b##*/}" "$b" || echo "build.sh: could not sign $b"
		rm -rf "$t"
	done
	find root -type f \( -path 'root/bin/*' -o -path 'root/lib/*' -o -path 'root/sbin/*' -o -path 'root/Applications/*' \) | \
	while read -r f; do
		file -b "$f" | grep -q '^Mach-O' || continue
		/usr/bin/codesign -v "$f" 2>/dev/null || /usr/bin/codesign -f -s - "$f" 2>/dev/null || echo "build.sh: could not sign $f"
	done
fi
# the image: zlib where it saves room (a small file whole in its attribute, as Apple keeps them); fw/
# as it is, which the kernel's drivers use where it lies in memory (bsd/dev/board/image.c)
python3 ../../tools/apfs/mkapfs.py -z --plain fw -o root.apfs hakios=root | tail -1; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build.sh: mkapfs failed" >&2; exit 1; }
# (readable by its owner alone while it carries a secret)
[ -z "$secrets" ] || chmod 600 root.apfs
[ -z "$CHECK" ] || ../../tools/apfs/check-image.sh root.apfs hakios=root

# --- the RAM part of a boot off the flash: FLASHBOOT=1 -> flashboot.apfs --------------------------------
# With the Darwin volume on the phone's flash as the root, all that has to come from RAM is what is not
# on it -- the two cryptexes, OS with the dyld shared cache and App -- and what starts the boot: three
# APFS containers back to back in the one resident image. The first, the boot volume, is the root the
# kernel starts from (rd=md0): stage0 as /sbin/launchd mounts the small image on /hakios and runs its
# init, which with boot-arg root_apfs=DEV mounts the volume on /flash, makes it the root and moves the
# boot volume to the volume's /private/preboot, then mounts the second container -- the OS cryptex -- on
# the boot volume's empty Cryptexes/OS and the third -- Apple's App cryptex as it is -- on Cryptexes/App:
# /private/preboot/Cryptexes/OS and App, where the volume's own /System/Cryptexes/OS and App point and
# where Darwin grafts the cryptexes (init/init.c, flash_root; the kernel's apfs takes "/dev/md0,c=1" and
# "c=2", and bsd/vm/vm_unix.c takes a shared cache from Cryptexes/OS). CRYPTEX_IMAGE=FILE:
# Apple's own image of the cryptex as it is instead of this one remade (2.6 GB against 1 GB: too much
# for the phone's memory beside Darwin, not for QEMU's). Upload it with `xl ramdisk userland/flashboot.apfs`.
# CRYPTEXES=flash: the boot volume alone (20 MB) -- the cryptexes are Apple's images as they are on the
# flash, in darwin right after the Darwin volume (../tools/darwin-write.py put them there), and init mounts
# them from there, the same containers 1 and 2 of the root's own device.
if [ -n "$FLASHBOOT" ] && [ -f $CACHE/dyld_shared_cache_$ARCH ]; then
	rm -rf flashboot flashboot.os
	mkdir -p flashboot/sbin flashboot/hakios flashboot/dev flashboot/flash flashboot/Cryptexes/OS flashboot/Cryptexes/App
	cp obj/stage0 flashboot/sbin/launchd
	# the OS cryptex, and the shared cache in it, only where this image will carry it:
	# with CRYPTEXES=flash the cryptexes are read from the phone's flash and this tree is
	# built and thrown away unused. The guard ends with the cache: everything after it goes
	# into the boot volume, which every recipe carries, flash cryptexes included
	if [ "$CRYPTEXES" != flash ]; then
		if [ -d $DARWIN_FW/cryptex ]; then cp -al $DARWIN_FW/cryptex flashboot.os; else mkdir -p flashboot.os; fi
		D=flashboot.os/System/Library/Caches/com.apple.dyld
		mkdir -p $D
		for f in $CACHE/dyld_shared_cache_$ARCH*; do
			case "$f" in *.symbols) continue ;; esac
			# -f: a cryptex fetched with the cache already in it would otherwise stop the build
			ln -f "$f" $D/
		done
	fi
	# the audio DSP's firmware and the speaker amplifiers' configuration, as in the system image's /fw: the kernel
	# reads the firmware as soon as there is a root (this image, adsp=1), the configuration maybe later -- by then
	# from the Darwin volume's /fw, which ../../init/init.c (flash_root) binds to this
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] flashboot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/aw882xx_acf.bin flashboot/fw/
	fi
	# the package manager's prefix, when ../components/tweaks/tools/mk-jb.sh has fetched one: /jb of the boot volume, which is
	# /private/preboot/jb once the Darwin volume is the root -- the volume where a rootless jailbreak keeps its
	# Procursus on an iPhone as well; init grafts it onto /var/jb (init/init.c, mount_writable)
	if [ -d ../../components/tweaks/jb/root/var/jb ]; then
		cp -a ../../components/tweaks/jb/root/var/jb flashboot/jb
		mkdir -p flashboot/jb-rw        # where init mounts the prefix's writable copy (init/init.c)
		# and what Sileo runs from /bin, for init to put over the Darwin volume's /bin (see the system image below)
		mkdir -p flashboot/jb-bin
		for c in $SILEO_BIN; do
			ln -s /var/jb/usr/bin/$c flashboot/jb-bin/$c
		done
		echo "  the package manager's prefix: $(du -sh flashboot/jb | cut -f1) as /jb of the boot volume"
	fi
	python3 ../../tools/apfs/mkapfs.py -z --plain fw -o flashboot.boot.apfs Boot=flashboot | tail -1; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build.sh: mkapfs failed" >&2; exit 1; }
	[ -z "$CHECK" ] || ../../tools/apfs/check-image.sh flashboot.boot.apfs Boot=flashboot
	# the third container: Apple's App cryptex as it is (10 MB; fetch.sh), for the boot volume's Cryptexes/App
	APP=""
	[ -f $DARWIN_FW/cryptex-app.dmg ] && APP=$DARWIN_FW/cryptex-app.dmg
	if [ "$CRYPTEXES" = flash ]; then
		mv flashboot.boot.apfs flashboot.apfs
	elif [ -n "$CRYPTEX_IMAGE" ]; then
		cat flashboot.boot.apfs "$CRYPTEX_IMAGE" $APP > flashboot.apfs
	else
		python3 ../../tools/apfs/mkapfs.py -z -o flashboot.os.apfs OS=flashboot.os | tail -1; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build.sh: mkapfs failed" >&2; exit 1; }
		[ -z "$CHECK" ] || ../../tools/apfs/check-image.sh flashboot.os.apfs OS=flashboot.os
		cat flashboot.boot.apfs flashboot.os.apfs $APP > flashboot.apfs
		rm -f flashboot.os.apfs
	fi
	rm -rf flashboot flashboot.os flashboot.boot.apfs
	echo "flashboot.apfs: $(( $(stat -c %s flashboot.apfs) / 1048576 )) MB"
fi

# --- the system image -----------------------------------------------------------------------------
if [ -f $DARWIN_FW/system.manifest ] && [ -f $CACHE/dyld_shared_cache_$ARCH ] &&
	{ [ ! -f system.apfs ] || [ obj/stage0 -nt system.apfs ] || [ $DARWIN_FW/system.manifest -nt system.apfs ] ||
	  [ $DARWIN_FW/cryptex -nt system.apfs ] || [ $DARWIN_FW/cryptex-app -nt system.apfs ] || [ -n "$SYSTEM" ]; }; then
	# what darwin165/select-system.py has chosen since darwin165/rootfs was extracted is not in it, and would not be
	# in the image either (so it was once with the audio plug-ins and the system's sounds: Darwin found no
	# speaker and played nothing, though our device was there and the kernel's side worked)
	if [ -f $DARWIN_FW/.done-rootfs ] && [ $DARWIN_FW/select-system.py -nt $DARWIN_FW/.done-rootfs ]; then
		echo "build.sh: darwin165/select-system.py has changed since darwin165/rootfs was extracted: run KEEP=1 darwin165/fetch.sh" >&2
		exit 1
	fi
	# outside ~/Desktop: iCloud Drive syncs the Desktop, and its file provider stalls a hundred thousand hard links
	# made there (cp -al hung in linkat twice, 30 October); an APFS clone (cp -c) costs no space either
	SYSROOT_DIR=${SYSROOT_DIR:-$HOME/hakios-gpu2/sysroot-work}
	# (a clone finished beforehand, marked .clone-complete, is taken as it is: cloning out of the Desktop is slow)
	rm -f sysroot
	if [ -f "$SYSROOT_DIR/.clone-complete" ]; then
		rm -f "$SYSROOT_DIR/.clone-complete"
	else
		rm -rf "$SYSROOT_DIR"
		/bin/cp -cR $DARWIN_FW/rootfs "$SYSROOT_DIR"
	fi
	ln -s "$SYSROOT_DIR" sysroot
	# launchd must load our choice of jobs, not all 500 that Darwin has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	# Darwin 26's launchd counts a job as the system's only when its file is on the system volume: one read
	# through the symlink into /hakios is "non-system" and may claim no host port or event stream. So
	# there the jobs are copied in (JOBS_IN_SYSTEM=1, the default for ../darwin26), and a change to them
	# means a new system image.
	if [ "${JOBS_IN_SYSTEM:-$([ "$DARWIN26" = 1 ] && echo 1)}" = 1 ]; then
		cp -R root/LaunchDaemons sysroot/System/Library/LaunchDaemons
	else
		ln -s /hakios/LaunchDaemons sysroot/System/Library/LaunchDaemons
	fi
	mkdir -p sysroot/hakios sysroot/sbin
	# where the kernel mounts the Darwin root container off the phone's flash (boot-arg apfs_volume=):
	# Darwin has no /System/Volumes, and nothing can make a directory in this image once it is built
	mkdir -p sysroot/System/Volumes/Darwin
	# the applications Darwin installs on its first boot are system applications here (see select-system.py)
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.darwin
	cp obj/stage0 sysroot/sbin/launchd
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (bsd/dev/board/adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (bsd/dev/board/speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# the package manager's prefix, when ../components/tweaks/tools/mk-jb.sh has fetched one: the Procursus bootstrap with
	# Sileo in it, as /jb. It cannot live at /var/jb in the image -- /private/var is a memory
	# filesystem made afresh at every boot -- so init copies it there (init/init.c, copy_tree).
	if [ -d ../../components/tweaks/jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../../components/tweaks/jb/root/var/jb sysroot/jb
		# the applications of the prefix are system applications here: LaunchServices builds its
		# database afresh at every boot by walking /Applications, which is how their icons appear
		for app in sysroot/jb/Applications/*.app; do
			[ -d "$app" ] && cp -a "$app" sysroot/Applications/
		done
		# Sileo moves, copies and removes files as root with /bin/mv, /bin/cp, /bin/rm, /bin/mkdir and
		# /bin/chmod -- where a jailbreak with a writable root keeps them -- and Darwin's /bin has df and ps
		# alone: every one of those spawns failed with nothing said, and the package it had downloaded never
		# reached apt's archives ("Unable to fetch some archives"). The prefix's own programs, by those names.
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c sysroot/bin/$c
		done
		echo "  the package manager's prefix: $(du -sh sysroot/jb | cut -f1) as /jb (init puts it at /var/jb)"
	fi
	# An application of this port's own, registered the same free way the prefix's are: a plain
	# directory in /Applications, which the LaunchServices database is rebuilt from at every boot
	# by walking that directory -- no installd, no container, no uicache. cp -a and not a link,
	# because a bundle of symbolic links is refused ("Unable to register") and the boot-time
	# registrar passes over mount points.
	#
	# This is in the SYSTEM image and not the small one, so a change to it costs a 2.5 GB rebuild
	# and a re-upload: SYSTEM=1 is what makes this line run again.
	if [ -d obj/HakiosTriangle.app ]; then
		cp -a obj/HakiosTriangle.app sysroot/Applications/
		echo "  HakiosTriangle.app in /Applications (com.hakios.triangle, scheme hakiostriangle:)"
	fi
	# Applications from outside this tree: every .app in ../apps, which is kept out of the
	# repository the way ../fw-* and ../jb are, so that nothing of somebody else's package can
	# reach it. They go into /Applications beside the port's own on exactly the same terms --
	# a plain directory, which LaunchServices finds by walking that folder at every boot.
	#
	# cp -aL and not cp -a: a bundle made of symbolic links is refused ("Unable to register"),
	# so whatever a link points at is copied as the file itself.
	for app in ../apps/*.app; do
		[ -d "$app" ] || continue
		cp -aL "$app" sysroot/Applications/
		echo "  ${app##*/} in /Applications ($(du -sm "$app" | cut -f1) MB from ../apps)"
	done
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /hakios/lib/HakiosAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver
	# ... and our Metal plug-in, at the one path Metal's own discovery can open: step 3 composes
	# "/System/Library/Extensions/" + MetalPluginName + ".bundle" and opens it with NSBundle
	# (docs/metal.md 1). Same shape as the audio plug-in above -- a link into /hakios/lib, where
	# the bundle itself travels in the small image -- because this directory is Darwin's own and
	# nothing can be added to it after the image is built.
	#
	# This link is inert on a boot that does not ask for Metal: nothing composes that path until
	# MetalPluginName is on the IOAcceleratorES service, and only boot-arg metal=1 lets a process
	# put it there (iokit/Kernel/MiniIOMobileFramebuffer.cpp). So it is not behind a switch of its
	# own -- a switch here would cost a 2.5 GB system.apfs rebuild and a re-upload every time it
	# was flipped, and buy nothing a missing boot-arg does not already buy.
	mkdir -p sysroot/System/Library/Extensions
	ln -s /hakios/lib/HakiosMetal.bundle sysroot/System/Library/Extensions/HakiosMetal.bundle
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $DARWIN_FW/cryptex ]; then cp -al $DARWIN_FW/cryptex sysroot/System/Cryptexes/OS; fi
	# the App cryptex, the same way: Safari's daemons (eight launchd jobs run them from there), its settings and
	# the bundles around it, which the root's links reach (System/Library/PreferenceBundles/MobileSafariSettings.bundle...)
	if [ -d $DARWIN_FW/cryptex-app ]; then cp -al $DARWIN_FW/cryptex-app sysroot/System/Cryptexes/App; fi
	# Darwin 26: what of the ExclaveOS cryptex the system reads from where it is mounted at boot, the secure
	# indicators' assets (darwin26/fetch.sh): without them SpringBoard throws CASecureIndicatorLayerInvalidName
	if [ -d $DARWIN_FW/exclave/root ]; then
		mkdir -p sysroot/private/preboot/Cryptexes/ExclaveOS
		cp -R $DARWIN_FW/exclave/root/. sysroot/private/preboot/Cryptexes/ExclaveOS/
	fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_$ARCH*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	# zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): 5.4 GB of
	# files in 2.5 GB, what fits into the phone's RAM beside Darwin
	python3 ../../tools/apfs/mkapfs.py -z -o system.apfs "System=$(cd sysroot && pwd -P),manifest=$DARWIN_FW/system.manifest" | tail -1; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build.sh: mkapfs failed" >&2; exit 1; }
	[ -z "$CHECK" ] || ../../tools/apfs/check-image.sh system.apfs System=sysroot,manifest=$DARWIN_FW/system.manifest
	rm -rf sysroot
fi
