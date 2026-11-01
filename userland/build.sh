#!/bin/bash
# Build the user-space side of the bring-up.
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.apfs        the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/), an APFS container (../tools/apfs/mkapfs.py). It travels in
#                    the payload next to the kernel (the second RAM disk, /dev/md1) and is mounted
#                    on /redwood. Rebuilt every time.
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
#                      EXTRA="..."  more programs for /redwood/bin (files on this machine): one-off
#                                   diagnostics, which a file given as RC_JOBS can then run
#                      EXTRA_SHARE="..." directories on this machine to copy whole into /redwood/share,
#                                   each under its own name: the data a one-off diagnostic reads, when it
#                                   needs more than a program (tools/air/intrincheck.py --emit writes such
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
#                                   runs these lines itself (src/init.c, run_command): words split on
#                                   blanks, no quoting, no variable assignment, no redirection beyond a
#                                   trailing ">> FILE" and "&". So a program that needs a value passed to
#                                   it needs an option for it, not an environment variable -- which is why
#                                   air_run grew --buffer.
#                      GL=1         with an OpenGL ES implementation for the window server: Mesa
#                                   (tools/mk-mesa.sh fetches and builds it) behind src/eagl.c, as
#                                   lib/libOSMesa.8.dylib (15 MB) and lib/libhakios.gl.dylib. Where the
#                                   kernel has started the phone's GPU, CoreAnimation then draws with its
#                                   OpenGL ES renderer, by that GPU (boot-arg gpu=1); anywhere else -- QEMU,
#                                   no boot-arg, no firmware in ../fw-gpu -- the library is not even loaded
#                                   and nothing changes (src/inject.c, opengl_es()).
#                                   GL=test: only the library and bin/gl_test (a triangle, src/gl_test.c;
#                                   RC_JOBS=rc.jobs.gltest runs it); the window server stays as it is.
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/ (20 MB; for the
#                                   kernel's adsp_step=N, drivers/audio/hakios.adsp.c, which runs before the root is
#                                   mounted). Not beside GL=1: the two do not fit the 64 MB the loader takes with
#                                   the kernel. adsp=1 needs none of it: the system image has the firmware in /fw.
#                      MODEM=1      stage checked local MPSS firmware from ../fw-modem in system.apfs /fw.
#                                   This does not start the modem; the XNU MPSS driver is still required.
#   system.apfs      the system image, the root filesystem (the resident RAM disk, /dev/md0), an
#                    APFS container: the part of Darwin userland 16.5 chosen by ../sysimage/select-system.py at
#                    its real paths, the dyld shared cache, and src/stage0.c as /sbin/launchd (the
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
#                    (../tools/apfs/check-image.sh; the system image takes a few minutes more)
set -e
# a copy of a tree that shares its files' data: hard links (Linux), clones of the files (macOS's APFS, whose cp has no -l)
linkcopy() { if [ "$(uname)" = Darwin ]; then cp -cRp "$1" "$2"; else cp -al "$1" "$2"; fi; }
cd "$(dirname "$(readlink -f "$0")")"
source ../xnu-env.sh

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
DYN="xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"
DARWIN=../sysimage
CACHE=$DARWIN/20F66__iPhone10,3_6
# sysimage/rootfs and sysimage/cryptex must be this copy's own directories of hard links, the way
# tools/new-workspace.sh makes them. A symlink to another copy's tree makes sysroot a symlink too, because
# cp -a does not follow one, and then every rm -rf and mv below edits the shared extraction in place. That
# happened on 19 October 2026: 491 LaunchDaemons, NanoLaunchDaemons, LaunchAngels, xpc/launchd.plist and
# private/var went out of the main copy's tree, and only the hard links in the other copies still held them.
for d in rootfs cryptex cryptex-app; do
	[ -L $DARWIN/$d ] || continue
	echo "build.sh: sysimage/$d is a symlink to $(readlink $DARWIN/$d)" >&2
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
# drivers/wifi/hakios.apple80211.c. With the radio (wifi=1) Settings then shows Wi-Fi on, the network joined and the
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
# (src/inject.c is libhakios.dylib, src/inject_sb.c is libhakios.sb.dylib, src/inject_media.c is
# libhakios.media.dylib).
# What Sileo runs from /bin (see the package manager's prefix below): the system image's /bin, or the
# boot volume's bin, bound over the Darwin volume's /bin when that is the root (src/init.c, flash_root).
SILEO_BIN="chmod cp mkdir mv rm"
# (libhakios.tweaks is the tweak loader of the package manager's prefix, src/inject_tweaks.c: in
# SpringBoard the library above opens it, here it is named for the programs that have no library of
# ours -- Settings, where a tweak's own pane belongs. Add a program's name to have tweaks in it.)
INJECT=${INJECT-"/redwood/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libhakios.sb.dylib SpringBoard
/redwood/lib/libhakios.media.dylib mediaserverd
/redwood/lib/libhakios.app.dylib Sileo
/redwood/lib/libhakios.tweaks.dylib Preferences"}
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
	$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
	$CC -c libmini/start.S -o obj/start.o
	for prog in init child stage0 var_store; do
		$CC -Ilibmini $LD -o obj/$prog.new src/$prog.c obj/mini.o obj/start.o
		# keep the old file (and its time stamp) when nothing changed: system.apfs depends on stage0
		cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
	done
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
cp obj/child root/bin/child
# The data volume into the flash and back (src/var_store.c), and the job that saves it when a run
# ends: the program and its job together, away from the Darwin job list below, which several working
# copies are editing at once.
[ -f obj/var_store ] && cp obj/var_store root/bin/
cp com.hakios.varstore.plist root/LaunchDaemons/
echo "Darwin on hakios: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
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
if [ -d $DARWIN/rootfs/usr/lib ]; then
	if [ -z "$PREBUILT" ]; then
		# ordinary dynamically linked programs of our own, built against the Darwin SDK
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		$DYN -o obj/rwtouch src/rwtouch.c
		$DYN -framework CoreFoundation -framework AudioToolbox -framework AVFoundation -lobjc -o obj/sound_test src/sound_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/torch_test src/torch_test.c
		$DYN -O2 -o obj/camera_test src/camera_test.c src/camera_isp.c
		$DYN -fobjc-arc -framework Foundation -framework AVFoundation -o obj/camera_avtest src/camera_avtest.m
		# (the microphone as an application records: allowed to, by an entitlement, if ldid is there)
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/record_test src/record_test.c &&
			{ ${LDID:-ldid} -Ssrc/record_test.entitlements obj/record_test 2>/dev/null ||
			  echo "build.sh: no ldid: record_test is unsigned, and the system may not let it record"; }
		$DYN -o obj/cpu_test src/cpu_test.c
		# which processes take the processors, every few seconds (src/cputop.c; rc.jobs.cputop)
		$DYN -o obj/cputop src/cputop.c
		$DYN -framework CoreFoundation -framework IOSurface -o obj/cow_test src/cow_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework IOSurface -o obj/cg_test src/cg_test.c
		# what Darwin here believes about Metal, and what CoreImage does without it (src/metal_test.m);
		# OpenGLES for the CIContext made over an EAGLContext, which is deprecated and the point
		$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics \
			-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES \
			-lMobileGestalt -o obj/metal_test src/metal_test.m
		# what CoreAnimation and UIKit believe the screen's frame rate is (src/fps_info.m)
		$DYN -fobjc-arc -framework Foundation -framework QuartzCore -o obj/fps_info src/fps_info.m
		$DYN -o obj/shm_test src/shm_test.c
		$DYN -o obj/apfs_mmap_test src/apfs_mmap_test.c
		$DYN -o obj/vol_ls src/vol_ls.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o obj/snap_test src/snap_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework CoreText -o obj/font_test src/font_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		# (with an entitlement for "battery_test open URL", if ldid is there: src/battery_test.c)
		$DYN -framework CoreFoundation -framework IOKit -lsqlite3 -o obj/battery_test src/battery_test.c &&
			{ ${LDID:-ldid} -Ssrc/battery_test.entitlements obj/battery_test 2>/dev/null ||
			  echo "build.sh: no ldid: battery_test is unsigned, and SpringBoard will not open URLs for it"; }
		# (the keychain answers only a program that has a keychain access group: signed with one, if ldid is there)
		$DYN -framework CoreFoundation -framework Security -o obj/keychain_test src/keychain_test.c &&
			{ ${LDID:-ldid} -Ssrc/keychain_test.entitlements obj/keychain_test 2>/dev/null ||
			  echo "build.sh: no ldid: keychain_test is unsigned, and the keychain will refuse it (-34018)"; }
		# (and wifid's way of keeping a network's password, by wifid's name: src/wifipw_test.c)
		$DYN -framework CoreFoundation -framework Security -o obj/wifipw_test src/wifipw_test.c &&
			{ ${LDID:-ldid} -Ssrc/wifipw_test.entitlements obj/wifipw_test 2>/dev/null ||
			  echo "build.sh: no ldid: wifipw_test is unsigned, and the keychain will refuse it (-34018)"; }
		# our library for inside stock Darwin programs (the jobs in INJECT are started with it)
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios.dylib src/inject.c
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios.sb.dylib src/inject_sb.c
		# ... the tweak loader of the package manager's prefix (src/inject_tweaks.c): it goes into a
		# program either as the one library the kernel inserts or opened by a library of ours
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.tweaks.dylib -framework CoreFoundation -lobjc \
			-o obj/libhakios.tweaks.dylib src/inject_tweaks.c
		# ... and inside an application, to say what killed it (src/inject_app.c)
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.app.dylib -lobjc \
			-o obj/libhakios.app.dylib src/inject_app.c
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libhakios.media.dylib -framework CoreFoundation -framework CoreMedia \
			-framework CoreVideo -framework IOSurface -lobjc \
			-o obj/libhakios.media.dylib src/inject_media.c src/camera_device.c src/camera_isp.c
		# the output device of the audio server: a plug-in for CoreAudio's HAL
		$DYN -bundle -framework CoreFoundation -o obj/HakiosAudio src/audio_plugin.c
		# the plug-in Metal loads when the kernel names it (boot-arg metal=1): a device, its buffers and the libraries it reads
		# (src/HakiosMetal.m, src/hakios_metallib.c; tools/metal-plugin-test.sh tries the same objects on a Mac)
		# With hakios-metal (github.com/oosexgang/hakios-metal, `make ios` in it, beside this tree or at HAKIOS_METAL=): buffers in
		# the GPU's memory, command buffers, and blits done by the GPU when the kernel has one (gpu=1). Without it, the plug-in as before.
		# (out/ios/ since hakios-metal took vcvkk's history; out/darwin/ is where the older trees built it)
		HM=${HAKIOS_METAL:-../../hakios-metal}
		METAL_GPU=
		for HM_OUT in $HM/out/ios $HM/out/darwin; do
			if [ -f $HM_OUT/libhakios-metal.a ]; then
				METAL_GPU="-DHAKIOS_METAL_GPU -I$HM/include $HM_OUT/libhakios-metal.a -lc++ -framework IOKit -framework IOSurface"
				# built with its compiler (make ios MESA=1): Mesa's archives of our iOS build too, as hakios-metal's
				# tools/compiler.mk lists them (HAKIOS_METAL_IOS_MESA_LIBS)
				if nm -g $HM_OUT/libhakios-metal.a 2>/dev/null | grep -q " U _air_"; then
					MB=../xnu-deps/build/mesa/build-softpipe-freedreno
					for a in src/compiler/air/libair.a src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a \
					    src/freedreno/isa/libir3decode.a src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a \
					    src/compiler/libcompiler.a src/freedreno/fdl/libfreedreno_layout.a \
					    src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a src/util/libmesa_util_sse41.a \
					    src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a; do
						METAL_GPU="$METAL_GPU $MB/$a"
					done
					METAL_GPU="$METAL_GPU -lz -lm"
					echo "build.sh: ... with its compiler (Mesa's archives from $MB)"
				fi
				echo "build.sh: HakiosMetal with hakios-metal ($HM_OUT)"
				break
			fi
		done
		# the plug-in itself is hakios-metal's now (plugin/, where it is tested); the copy in src/ is for a tree without it
		METAL_SRC=src
		if [ -f $HM/plugin/HakiosMetal.m ]; then
			METAL_SRC=$HM/plugin
			echo "build.sh: HakiosMetal from $HM/plugin"
		fi
		if [ "${METAL_PLUGIN:-}" = ca ]; then
			# METAL_PLUGIN=ca: the plug-in that serves CoreAnimation's window server (src/HakiosMetalCA.m, vcvkk's with
			# this tree's shader choice), over hakios-metal built with its compiler (make ios MESA=1); the window server
			# also wants QuartzCore's default.metallib (tools/ca-metallib.py, EXTRA_SHARE=, boot-arg ca_metallib=)
			echo "build.sh: HakiosMetal for CoreAnimation (src/HakiosMetalCA.m)"
			$DYN -bundle -framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc \
				-I$HM/include -o obj/HakiosMetal src/HakiosMetalCA.m $METAL_GPU
		else
		$DYN -bundle -framework Foundation -framework Metal -lobjc -I$METAL_SRC -o obj/HakiosMetal $METAL_SRC/HakiosMetal.m \
			$METAL_SRC/hakios_metallib.c $METAL_GPU
		fi
		if [ -n "$GL" ]; then
			# the OpenGL ES library, and the table of functions that hands CoreAnimation's calls to it
			# (Mesa says beside the library what it was built from: one from other patches is built again)
			M=../xnu-deps/build/mesa/out
			[ "$(cat $M/built-from 2>/dev/null)" = "$(cat ../tools/mesa/*.patch ../tools/mesa/hakios-drm/* | sha256sum)" ] ||
				../tools/mk-mesa.sh
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -dynamiclib -install_name /redwood/lib/libhakios.gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libhakios.gl.dylib \
				src/eagl.c src/gl_osmesa.c
			$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
			# ... and what this GPU makes of an unsigned integer turned into a float (src/u2f_test.c)
			$DYN $M/libOSMesa.8.dylib -o obj/u2f_test src/u2f_test.c
			cp $M/libOSMesa.8.dylib obj/
			# air_run draws with a Metal shader translated by the AIR frontend
			# and reads the pixels back (docs/air.md). It is built inside the
			# Mesa build, because it needs Gallium itself and not the gl*
			# entry points this library exports; AIR=1 tools/mk-mesa.sh makes
			# it, and it is copied in when it is there.
			[ -f $M/air_run ] && cp $M/air_run obj/air_run
		fi
	fi
	cp obj/hello_dyn obj/svc_test obj/surf_test obj/rwtouch obj/mg_test root/bin/
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/        # (a prebuilt/ from before this program has none)
	[ -f obj/cputop ] && cp obj/cputop root/bin/
	[ -f obj/cow_test ] && cp obj/cow_test root/bin/
	[ -f obj/cg_test ] && cp obj/cg_test root/bin/
	[ -f obj/shm_test ] && cp obj/shm_test root/bin/
	[ -f obj/apfs_mmap_test ] && cp obj/apfs_mmap_test root/bin/
	[ -f obj/vol_ls ] && cp obj/vol_ls root/bin/
	[ -f obj/snap_test ] && cp obj/snap_test root/bin/
	[ -f obj/font_test ] && cp obj/font_test root/bin/
	[ -f obj/metal_test ] && cp obj/metal_test root/bin/
	[ -f obj/fps_info ] && cp obj/fps_info root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	[ -f obj/battery_test ] && cp obj/battery_test root/bin/
	[ -f obj/keychain_test ] && cp obj/keychain_test root/bin/
	[ -f obj/wifipw_test ] && cp obj/wifipw_test root/bin/
	[ -f obj/sound_test ] && cp obj/sound_test root/bin/
	[ -f obj/torch_test ] && cp obj/torch_test root/bin/
	[ -f obj/camera_test ] && cp obj/camera_test root/bin/
	[ -f obj/camera_avtest ] && cp obj/camera_avtest root/bin/
	[ -f obj/record_test ] && cp obj/record_test root/bin/
	for f in $EXTRA; do cp "$f" root/bin/; done
	# /dev and /private/var exist so that this image can also be the root on its own:
	# devfs is mounted on /dev, and init puts a ramfs on /private/var
	mkdir -p root/dev root/private/var
	mkdir -p root/lib
	cp obj/libhakios.dylib obj/libhakios.sb.dylib root/lib/
	[ -f obj/libhakios.media.dylib ] && cp obj/libhakios.media.dylib root/lib/     # (as with cpu_test)
	[ -f obj/libhakios.app.dylib ] && cp obj/libhakios.app.dylib root/lib/
	[ -f obj/libhakios.tweaks.dylib ] && cp obj/libhakios.tweaks.dylib root/lib/
	if [ -f obj/HakiosAudio ]; then
		mkdir -p root/lib/HakiosAudio.driver
		cp obj/HakiosAudio root/lib/HakiosAudio.driver/
		cp src/audio_plugin.plist root/lib/HakiosAudio.driver/Info.plist
	fi
	if [ -f obj/HakiosMetal ]; then
		mkdir -p root/lib/HakiosMetal.bundle
		cp obj/HakiosMetal root/lib/HakiosMetal.bundle/
		cp src/HakiosMetal.plist root/lib/HakiosMetal.bundle/Info.plist
	fi
	if [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		if [ -f obj/u2f_test ]; then cp obj/u2f_test root/bin/; fi   # an "if": see the inject.conf loop below
		[ -f obj/air_run ] && cp obj/air_run root/bin/
		[ "$GL" = test ] || cp obj/libhakios.gl.dylib root/lib/
	fi
	# a program is not started with a library that is not there: it would not start at all
	echo "$INJECT" | while read -r library names; do
		# a /redwood/lib library only if it was actually built; any other path (e.g. a tweak loader
		# in /var/jb) as it is, since it lives on the prefix and is there at run time
		case "$library" in
		"") ;;
		# an "if" and not "&& echo": a last line whose library is absent would make the loop,
		# and with it the pipeline and the whole build under set -e, fail -- which is what a tree
		# with only prebuilt/userland in it does, since not every injection library is shipped there
		/redwood/lib/*) if [ -f "root/lib/${library#/redwood/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf
	cp $DARWIN/var.skel root/etc/var.skel              # the directories init creates in /private/var
	# time zones: the template's links go in a circle until the time zone daemon has run (/usr/share/zoneinfo
	# points back into /var); point them at the data that ships with the system, and use this machine's zone
	zone=$(timedatectl show -p Timezone --value 2>/dev/null || true); zone=${zone:-UTC}
	sed -i.bak -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|' \
		-e "s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime\$|l\t/var/db/timezone/zoneinfo/$zone\t/private/var/db/timezone/localtime|" root/etc/var.skel
	rm -f root/etc/var.skel.bak
	# ... and the files of ours it puts there (var/): those under mobile/ belong to the user mobile
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
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
	# QUIET=1: launchd's own standard error goes nowhere (src/init.c). It says a few thousand
	# lines about its own business in the first minutes and pushes everything else out of the
	# kernel's log; the daemons keep their own standard error on the console all the same.
	rm -f root/etc/quiet
	[ -n "$QUIET" ] && : > root/etc/quiet
	# the jobs the Darwin launchd will see (/System/Library/LaunchDaemons is a symlink to this directory).
	# With FLASHBOOT=1 the root will be Apple's whole volume on the flash, not the part of it that fits in
	# the system image, so mkjobs.py judges "is this daemon's program there" by the listing of the volume
	# and not by what select-system.py extracted here (--root-volume; seven jobs turn on the difference,
	# fairplayd among them).
	cp com.hakios.demo.plist com.hakios.wifi.plist root/LaunchDaemons/
	VOLUME=""
	[ -n "$FLASHBOOT" ] && [ -f $DARWIN/rootfs.slt ] && VOLUME="--root-volume $DARWIN/rootfs.slt"
	[ -n "$DARWIN_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $VOLUME $DARWIN/rootfs root/LaunchDaemons $DARWIN_JOBS
fi
# firmware of the phone's own hardware, where somebody has pulled it from the phone's Android into ../fw-gpu
# (it is the vendor's and never in the repository): the kernel's GPU bring-up, drivers/gpu/hakios.adreno.c, finds it in fw/
if [ -d ../fw-gpu ]; then
	mkdir -p root/fw
	cp ../fw-gpu/* root/fw/
fi
# ... and the image of the Wi-Fi DSP, from /vendor/firmware_mnt/image of the same Android into ../fw-wifi
# (drivers/wifi/hakios.wifi.c; 7.6 MB), with what the radio's firmware is sent once it runs: this phone's
# board data and the regulatory database (image/qca6750 there; 60 and 24 KB)
if [ -f ../fw-wifi/wpss.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/wpss.mdt ../fw-wifi/wpss.b0[2-9] root/fw/
	for f in bd_m20gl.elf regdb_xiaomi.bin; do
		[ -f ../fw-wifi/qca6750/$f ] && install -m 0644 ../fw-wifi/qca6750/$f root/fw/
	done
	# the network the radio is to join: its name on the first line, its passphrase on the second. Yours, and
	# so not in the repository; it goes where the firmware goes and the kernel reads it there (wifi_join=1)
	rm -f root/fw/network
	# SHARE=1: leave it out, for an image that is going to somebody else -- it is a passphrase
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ]; then install -m 0600 ../fw-wifi/network root/fw/network; fi
fi
# ... and the vibration motor's waveforms, the phone's /vendor/firmware/aw8697_haptic.bin, from ../fw-haptic
# (drivers/haptic/hakios.haptic.c, haptic=3; 3.6 KB, the vendor's and not in the repository either)
if [ -f ../fw-haptic/aw8697_haptic.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-haptic/aw8697_haptic.bin root/fw/
fi
# ... and the cameras' register tables (drivers/camera/hakios.camera.c, cam=2 and up), made by tools/camsensor.py out of
# the sensors' module files of Android's camera stack, the phone's /vendor/lib64/camera/com.qti.sensormodule.
# hakios_<maker>_<sensor>_<camera>.bin, in ../fw-camera (the vendor's: neither they nor the tables are in the
# repository): camera-<sensor>.tab
for module in ../fw-camera/com.qti.sensormodule.hakios_*.bin; do
	[ -f "$module" ] || continue
	sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.hakios_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camsensor.py "$module" "root/fw/camera-$sensor.tab" >/dev/null
done
# ... and their colour tuning (libhakios.media's processor, src/camera_isp.c), made by tools/camtuning.py out of the
# cameras' tuning files beside them, com.qti.tuned.hakios_<maker>_<sensor>_<camera>.bin: camera-<sensor>.isp
for tuned in ../fw-camera/com.qti.tuned.hakios_*.bin; do
	[ -f "$tuned" ] || continue
	sensor=$(basename "$tuned" .bin | sed 's/^com\.qti\.tuned\.hakios_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camtuning.py "$tuned" "root/fw/camera-$sensor.isp" >/dev/null
done
# ... and with ADSP=1 the image of the audio DSP, from the same place into ../fw-audio (drivers/audio/hakios.adsp.c):
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
fwhave= fwnone=
for d in gpu wifi haptic camera audio; do
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
# with flashboot.apfs resident; src/init.c, flash_root): an empty directory to hide Darwin's
# NanoLaunchDaemons and LaunchAngels with,
# xpc without launchd.plist (launchd would read that list of all of Darwin's jobs instead of
# LaunchDaemons) and the audio server's plug-in directory with only our device in it.
mkdir -p root/empty root/xpc root/hal
for f in extensions.plist extensions.plist.sig; do
	[ -f $DARWIN/rootfs/System/Library/xpc/$f ] && cp $DARWIN/rootfs/System/Library/xpc/$f root/xpc/
done
[ -d root/lib/HakiosAudio.driver ] && ln -s /redwood/lib/HakiosAudio.driver root/hal/HakiosAudio.driver
# the image: zlib where it saves room (a small file whole in its attribute, as Apple keeps them); fw/
# as it is, which the kernel's drivers use where it lies in memory (drivers/storage/hakios.image.c)
python3 ../tools/apfs/mkapfs.py -z --plain fw -o root.apfs redwood=root | tail -1
[ -z "$CHECK" ] || ../tools/apfs/check-image.sh root.apfs redwood=root

# --- the RAM part of a boot off the flash: FLASHBOOT=1 -> flashboot.apfs --------------------------------
# With the Darwin volume on the phone's flash as the root, all that has to come from RAM is what is not
# on it -- the two cryptexes, OS with the dyld shared cache and App -- and what starts the boot: three
# APFS containers back to back in the one resident image. The first, the boot volume, is the root the
# kernel starts from (rd=md0): stage0 as /sbin/launchd mounts the small image on /redwood and runs its
# init, which with boot-arg root_apfs=DEV mounts the volume on /flash, makes it the root and moves the
# boot volume to the volume's /private/preboot, then mounts the second container -- the OS cryptex -- on
# the boot volume's empty Cryptexes/OS and the third -- Apple's App cryptex as it is -- on Cryptexes/App:
# /private/preboot/Cryptexes/OS and App, where the volume's own /System/Cryptexes/OS and App point and
# where Darwin grafts the cryptexes (src/init.c, flash_root; the kernel's apfs takes "/dev/md0,c=1" and
# "c=2", and bsd/vm/vm_unix.c takes a shared cache from Cryptexes/OS). CRYPTEX_IMAGE=FILE:
# Apple's own image of the cryptex as it is instead of this one remade (2.6 GB against 1 GB: too much
# for the phone's memory beside Darwin, not for QEMU's). Upload it with `xl ramdisk userland/flashboot.apfs`.
# CRYPTEXES=flash: the boot volume alone (20 MB) -- the cryptexes are Apple's images as they are on the
# flash, in darwin right after the Darwin volume (../tools/darwin-write.py put them there), and init mounts
# them from there, the same containers 1 and 2 of the root's own device.
if [ -n "$FLASHBOOT" ] && [ -f $CACHE/dyld_shared_cache_arm64 ]; then
	rm -rf flashboot flashboot.os
	mkdir -p flashboot/sbin flashboot/redwood flashboot/dev flashboot/flash flashboot/Cryptexes/OS flashboot/Cryptexes/App
	cp obj/stage0 flashboot/sbin/launchd
	# the OS cryptex, and the shared cache in it, only where this image will carry it:
	# with CRYPTEXES=flash the cryptexes are read from the phone's flash and this tree is
	# built and thrown away unused. The guard ends with the cache: everything after it goes
	# into the boot volume, which every recipe carries, flash cryptexes included
	if [ "$CRYPTEXES" != flash ]; then
		if [ -d $DARWIN/cryptex ]; then linkcopy $DARWIN/cryptex flashboot.os; else mkdir -p flashboot.os; fi
		D=flashboot.os/System/Library/Caches/com.apple.dyld
		mkdir -p $D
		for f in $CACHE/dyld_shared_cache_arm64*; do
			case "$f" in *.symbols) continue ;; esac
			# -f: a cryptex fetched with the cache already in it would otherwise stop the build
			ln -f "$f" $D/
		done
	fi
	# the audio DSP's firmware and the speaker amplifiers' configuration, as in the system image's /fw: the kernel
	# reads the firmware as soon as there is a root (this image, adsp=1), the configuration maybe later -- by then
	# from the Darwin volume's /fw, which src/init.c (flash_root) binds to this
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] flashboot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/aw882xx_acf.bin flashboot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: /jb of the boot volume, which is
	# /private/preboot/jb once the Darwin volume is the root -- the volume where a rootless jailbreak keeps its
	# Procursus on an iPhone as well; init grafts it onto /var/jb (src/init.c, mount_writable)
	if [ -d ../jb/root/var/jb ]; then
		cp -a ../jb/root/var/jb flashboot/jb
		mkdir -p flashboot/jb-rw        # where init mounts the prefix's writable copy (src/init.c)
		# and what Sileo runs from /bin, for init to put over the Darwin volume's /bin (see the system image below)
		mkdir -p flashboot/jb-bin
		for c in $SILEO_BIN; do
			ln -s /var/jb/usr/bin/$c flashboot/jb-bin/$c
		done
		echo "  the package manager's prefix: $(du -sh flashboot/jb | cut -f1) as /jb of the boot volume"
	fi
	python3 ../tools/apfs/mkapfs.py -z --plain fw -o flashboot.boot.apfs Boot=flashboot | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.boot.apfs Boot=flashboot
	# the third container: Apple's App cryptex as it is (10 MB; unpack.sh), for the boot volume's Cryptexes/App
	APP=""
	[ -f $DARWIN/cryptex-app.dmg ] && APP=$DARWIN/cryptex-app.dmg
	if [ "$CRYPTEXES" = flash ]; then
		mv flashboot.boot.apfs flashboot.apfs
	elif [ -n "$CRYPTEX_IMAGE" ]; then
		cat flashboot.boot.apfs "$CRYPTEX_IMAGE" $APP > flashboot.apfs
	else
		python3 ../tools/apfs/mkapfs.py -z -o flashboot.os.apfs OS=flashboot.os | tail -1
		[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.os.apfs OS=flashboot.os
		cat flashboot.boot.apfs flashboot.os.apfs $APP > flashboot.apfs
		rm -f flashboot.os.apfs
	fi
	rm -rf flashboot flashboot.os flashboot.boot.apfs
	echo "flashboot.apfs: $(( $(stat -c %s flashboot.apfs) / 1048576 )) MB"
fi

# --- the system image -----------------------------------------------------------------------------
if [ -f $DARWIN/system.manifest ] && [ -f $CACHE/dyld_shared_cache_arm64 ] &&
	{ [ ! -f system.apfs ] || [ obj/stage0 -nt system.apfs ] || [ $DARWIN/system.manifest -nt system.apfs ] ||
	  [ $DARWIN/cryptex -nt system.apfs ] || [ $DARWIN/cryptex-app -nt system.apfs ] || [ -n "$SYSTEM" ]; }; then
	# what sysimage/select-system.py has chosen since sysimage/rootfs was extracted is not in it, and would not be
	# in the image either (so it was once with the audio plug-ins and the system's sounds: Darwin found no
	# speaker and played nothing, though our device was there and the kernel's side worked)
	if [ -f $DARWIN/.done-rootfs ] && [ $DARWIN/select-system.py -nt $DARWIN/.done-rootfs ]; then
		echo "build.sh: sysimage/select-system.py has changed since sysimage/rootfs was extracted: run KEEP=1 sysimage/unpack.sh" >&2
		exit 1
	fi
	rm -rf sysroot
	linkcopy $DARWIN/rootfs sysroot                   # hard links or clones: no copies of the 0.5 GB
	# launchd must load our choice of jobs, not all 500 that Darwin has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	ln -s /redwood/LaunchDaemons sysroot/System/Library/LaunchDaemons
	mkdir -p sysroot/redwood sysroot/sbin
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
	# the applications' frame rate: on a phone CoreAnimation holds every process to 60 frames a second unless its
	# bundle says CADisableMinimumFrameDurationOnPhone, which no application of an iPhone X's system does; the panel
	# here runs at 120 (IOMFBDisplayRefresh, MiniIOMobileFramebuffer.cpp). New files, not edits: sysroot's are links
	python3 mkhighrate.py sysroot
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (drivers/audio/hakios.adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# MPSS firmware is much too large for the small boot image. Keep it in the system APFS,
	# only when explicitly requested, after checking every loadable segment against this DTB.
	# This stages the files; a separate XNU MPSS driver is still required to start the modem.
	if [ -n "$MODEM" ]; then
		[ -f ../fw-modem/modem.mdt ] || { echo "build.sh: MODEM=1 needs ../fw-modem/modem.mdt" >&2; exit 1; }
		../devices/redwood/tools/modem_firmware.py --dtb ../devices/redwood/prebuilt/device-tree.dtb --firmware-dir ../fw-modem \
			--node /soc/qcom,mss@4080000 >/dev/null
		mkdir -p sysroot/fw
		cp ../fw-modem/modem.mdt ../fw-modem/modem.b[0-9][0-9] sysroot/fw/
		# ... and the firmware partition's image/ (its mcfg configurations and the registry's *.jsn, pulled from
		# the phone's /vendor/firmware_mnt/image into ../fw-modem/image), which the modem reads over TFTP (hakios.mpss.c)
		if [ -d ../fw-modem/image ]; then
			rm -rf sysroot/fw/image && cp -R ../fw-modem/image sysroot/fw/image
		fi
	fi
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (drivers/audio/hakios.speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: the Procursus bootstrap with
	# Sileo in it, as /jb. It cannot live at /var/jb in the image -- /private/var is a memory
	# filesystem made afresh at every boot -- so init copies it there (src/init.c, copy_tree).
	if [ -d ../jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../jb/root/var/jb sysroot/jb
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
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /redwood/lib/HakiosAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver
	# the plug-in Metal opens when the accelerator names it (boot-arg metal=1; without it nothing names this bundle)
	mkdir -p sysroot/System/Library/Extensions
	[ -d root/lib/HakiosMetal.bundle ] && ln -sfn /redwood/lib/HakiosMetal.bundle sysroot/System/Library/Extensions/HakiosMetal.bundle
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $DARWIN/cryptex ]; then linkcopy $DARWIN/cryptex sysroot/System/Cryptexes/OS; fi
	# the App cryptex, the same way: Safari's daemons (eight launchd jobs run them from there), its settings and
	# the bundles around it, which the root's links reach (System/Library/PreferenceBundles/MobileSafariSettings.bundle...)
	if [ -d $DARWIN/cryptex-app ]; then linkcopy $DARWIN/cryptex-app sysroot/System/Cryptexes/App; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	# zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): 5.4 GB of
	# files in 2.5 GB, what fits into the phone's RAM beside Darwin
	python3 ../tools/apfs/mkapfs.py -z -o system.apfs System=sysroot,manifest=$DARWIN/system.manifest | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh system.apfs System=sysroot,manifest=$DARWIN/system.manifest
	rm -rf sysroot
fi
