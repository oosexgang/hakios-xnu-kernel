#!/bin/bash
# The small image (/redwood) for an iOS 26/27 boot towards SpringBoard, and the launchd jobs.
#
# The image is made here from the first generation's sources (ios/userland/env.sh: US): its init and stage0,
# our arm64e libraries (inj26.sh, in $OBJ), the template of /private/var (ios/tools/unpack.sh's var.skel)
# and the phone's own firmware (firmware/BOARD/fw/, never published). The jobs are made from iOS's own
# plists (US/mkjobs.py: the named jobs as they are, every other job with Mach services on demand only) into
# $IOS/jobs26, for the system image (LAUNCHDAEMONS=$IOS/jobs26 mkimage26.py): iOS 26's launchd takes a job
# for a system service only when its plist is on the system volume.
# Writes $IOS/root26-ios.apfs, $IOS/jobs26, $OBJ/stage0 (the system image's /sbin/launchd) and $IOS/flashboot.apfs
# (the RAM part of a boot whose system image is on the flash).
# JOBS="label ..." replaces the named jobs; OSLOG as for mkjobs.py; RC_JOBS=FILE: what com.hakios.demo runs.
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=env.sh
source "$RN/userland/env.sh"
T=$IOS/root26-ios-tree
JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
	com.apple.wifid
"}
# (wifid: iOS's Wi-Fi daemon, which Settings shows Wi-Fi through; the kernel answers its Apple80211 requests,
# bsd/dev/hakios_apple80211.c. Where there is no radio it starts, finds none and waits.)
[ -f "$OBJ/libhakios.dylib" ] || { echo "root26-ios.sh: no libraries in $OBJ: ios/userland/inj26.sh first" >&2; exit 1; }
[ -s "$IOS/var.skel" ] || { echo "root26-ios.sh: no $IOS/var.skel: ios/tools/unpack.sh makes it" >&2; exit 1; }

# --- our programs ------------------------------------------------------------------------------------
# process 1 and the job runner (init), and the system image's /sbin/launchd (stage0, which mounts this
# image on /redwood and starts init): static, without libc or dyld (libmini)
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
$CC -I"$US/libmini" -c "$US/libmini/mini.c" -o "$OBJ/mini.o"
$CC -c "$US/libmini/start.S" -o "$OBJ/start.o"
for prog in init stage0 child var_store; do
	$CC -I"$US/libmini" $LD -o "$OBJ/$prog" "$US/src/$prog.c" "$OBJ/mini.o" "$OBJ/start.o"
done
# what the default job list asks the network (net_test waitaddr), as the libraries are: arm64e, pacstrip
$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -framework Network -o "$OBJ/net_test" "$US/src/net_test.m"
"$PACSTRIP" macho "$OBJ/net_test" "$OBJ/net_test.strip" > /dev/null && mv "$OBJ/net_test.strip" "$OBJ/net_test"
# touches, buttons and notifications for job lists (RC_JOBS=...; usage in its source): a swipe up unlocks, as a finger does
$DYN -o "$OBJ/rwtouch" "$US/src/rwtouch.c"
"$PACSTRIP" macho "$OBJ/rwtouch" "$OBJ/rwtouch.strip" > /dev/null && mv "$OBJ/rwtouch.strip" "$OBJ/rwtouch"
# the flashlight as an application reaches it, through AVFlashlight (rc.jobs.torch; ios/userland/torch_test.c)
$DYN -framework CoreFoundation -lobjc -o "$OBJ/torch_test" "$RN/userland/torch_test.c"
"$PACSTRIP" macho "$OBJ/torch_test" "$OBJ/torch_test.strip" > /dev/null && mv "$OBJ/torch_test.strip" "$OBJ/torch_test"
# the microphone as an application records from it, through AVAudioRecorder (userland/src/record_test.c: SECONDS; no
# sound is played), with the entitlement TCC asks of a recorder
$DYN -framework CoreFoundation -framework Foundation -framework AVFAudio -lobjc -o "$OBJ/record_test" "$US/src/record_test.c"
"$PACSTRIP" macho "$OBJ/record_test" "$OBJ/record_test.strip" > /dev/null && mv "$OBJ/record_test.strip" "$OBJ/record_test"
"$LDID" -S"$US/src/record_test.entitlements" "$OBJ/record_test"

# --- the tree ----------------------------------------------------------------------------------------
rm -rf "$T"
mkdir -p "$T/sbin" "$T/bin" "$T/etc" "$T/lib" "$T/share" "$T/LaunchDaemons"
cp "$OBJ/init" "$T/sbin/init"
cp "$OBJ/child" "$T/bin/child"           # (init execs it and collects its exit status: one of its checks)
# /private/var between boots (userland/src/var_store.c): init unpacks the last one with boot-arg var_store=1, and the
# job list's first line saves it when the run ends (ufs_write=1 lets it write darwin past the system's volumes)
cp "$OBJ/var_store" "$T/bin/var_store"
cp "$OBJ/net_test" "$T/bin/net_test"
# EXTRA_BIN="FILE ...": more programs of ours for a job list (a probe), into /redwood/bin, through pacstrip as the others
for f in ${EXTRA_BIN:-}; do
	"$PACSTRIP" macho "$f" "$T/bin/$(basename "$f")" > /dev/null && chmod 755 "$T/bin/$(basename "$f")"
done
cp "$OBJ/rwtouch" "$T/bin/rwtouch"
cp "$OBJ/torch_test" "$T/bin/torch_test"
cp "$OBJ/record_test" "$T/bin/record_test"
# the SSH server and its shell, when ios/userland/ssh.sh has built them (rc.jobs.ssh starts the server); the keys of
# this machine's ../ssh/ with them, unless SHARE=1 (an image for somebody else: no host key of ours in it). dropbear
# refuses keys whose file or directory anyone but their owner may write: 600 and 700
for p in dropbear scp sh; do
	[ -f "$OBJ/$p" ] && cp "$OBJ/$p" "$T/bin/$p"
done
if [ -z "$SHARE" ] && [ -d "$OBJ/ssh-keys" ] && [ -f "$T/bin/dropbear" ]; then
	mkdir -p "$T/etc/ssh"
	cp "$OBJ/ssh-keys/authorized_keys" "$OBJ/ssh-keys/dropbear_ed25519_host_key" "$T/etc/ssh/"
	chmod 700 "$T/etc/ssh"
	chmod 600 "$T/etc/ssh/"*
	echo "  ssh: the server, its shell and this machine's keys"
fi
# Metal's compiled stages from an earlier boot (ios/tools/metal-cache-pull.sh takes them off a running phone into
# $OBJ/metal-cache-seed): /private/var starts empty at every boot here, and without them every pipeline and every
# specialisation of CoreAnimation's uber shader is compiled again the first time a frame needs it, mid-gesture. The
# plug-in looks in /redwood/share/hakios-metal-cache after its own cache (HakiosMetalCA.m: HM_CACHE_SEED); a stage of
# another build of the compiler is never taken (its name has the build in it), only room.
SEED=${METAL_CACHE_SEED:-$OBJ/metal-cache-seed}
if [ -d "$SEED" ] && [ -n "$(ls "$SEED"/*.stage 2> /dev/null)" ]; then
	mkdir -p "$T/share/hakios-metal-cache"
	cp "$SEED"/*.stage "$T/share/hakios-metal-cache/"
	echo "  Metal: $(ls "$SEED"/*.stage | wc -l) compiled stages from an earlier boot ($(du -sh "$SEED" | cut -f1))"
fi
cp "$OBJ"/*.dylib "$T/lib/"
# (the test tweak's filter beside it: a scenario copies both into /var/jb/usr/lib/TweakInject, ios/userland/tweak_test.c)
cp "$RN/userland/tweak_test.plist" "$T/lib/hakios-tweaktest.plist"
cp "$US/rc.launchd" "$T/etc/rc"
# (init reads it back and looks for "Darwin" at its start: one of its checks)
python3 -c "import plistlib, sys; m = plistlib.load(open(sys.argv[1], 'rb')); print('Darwin with iOS', m['ProductVersion'], m['ProductBuildVersion'], 'user space, HakiOS')" \
	"$IOS/BuildManifest.plist" > "$T/etc/motd"
# 300000 bytes, byte i = (i*31 + (i>>8)) & 0xff; init verifies it through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > "$T/share/pattern.bin"
# the template of /private/var, which init makes at every boot; the time zone's links point at the data the
# system ships (the template's go in a circle until the time zone daemon has run), in this machine's zone
cp "$IOS/var.skel" "$T/etc/var.skel"
zone=$(timedatectl show -p Timezone --value 2> /dev/null || readlink /etc/localtime 2> /dev/null | sed 's#.*/zoneinfo/##' || true)
zone=${zone:-UTC}
sed -i -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|' \
	-e "s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime\$|l\t/var/db/timezone/zoneinfo/$zone\t/private/var/db/timezone/localtime|" "$T/etc/var.skel"
# ... and our files there (US/var): those under mobile/ are the user mobile's. The setup assistant is marked
# done the way iOS 26's SetupAssistant and SpringBoard read it (with iOS 16.5's file unlocking started
# Setup.app, whose screen the software renderer cannot draw)
cp -r "$US/var" "$T/var"
python3 - "$T/var/mobile/Library/Preferences/com.apple.purplebuddy.plist" <<'PY'
import datetime, plistlib, sys
p = sys.argv[1]
d = plistlib.load(open(p, 'rb'))
d.update(SetupDone=True, SetupFinishedAllSteps=True, SetupVersion=99, ForceNoBuddy=True,
         SetupState='SetupUsingAssistant', SetupLastExit=datetime.datetime(2026, 10, 1))
plistlib.dump(d, open(p, 'wb'), fmt=plistlib.FMT_BINARY)
PY
printf 'd\t755\t0\t0\t/private/var/root/Library/Preferences/ByHost\nd\t755\t501\t501\t/private/var/mobile/Library/Preferences/ByHost\n' >> "$T/etc/var.skel"
(cd "$T/var" && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
	case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
	printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
done >> "$T/etc/var.skel"
printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> "$T/etc/var.skel"
# the libraries the kernel inserts (kern.exec_inject, set by init): LIBRARY NAME..., one line each; @apps is
# every application (below)
cat > "$T/etc/inject.conf" <<'CONF'
/redwood/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libhakios-sb.dylib SpringBoard
/redwood/lib/libhakios-statusbar.dylib @apps
/redwood/lib/libhakios-tweaks.dylib Preferences
/redwood/lib/libhakios-app.dylib Sileo
CONF
# (libhakios-tweaks: the package manager's tweaks, in Settings, where a tweak's own pane belongs; SpringBoard's
# library opens it by itself. libhakios-app: what killed Sileo, the prefix's application. As on 16.5.)
# INJECT_MORE="LIBRARY NAME ...": one more line of etc/inject.conf, as it is (a library of $OBJ under /redwood/lib, for a probe)
[ -z "${INJECT_MORE:-}" ] || echo "$INJECT_MORE" >> $T/etc/inject.conf
# LAYERS="NAME ...": these applications write down their layer tree on the notification com.hakios.layers
[ -z "${LAYERS:-}" ] || echo "/redwood/lib/libhakios-layers.dylib $LAYERS" >> $T/etc/inject.conf
# WAITSTACK_SB=1: libhakios.dylib in SpringBoard as well, for its thread sampler (boot-arg waitstack=sb, inject.c)
[ -z "${WAITSTACK_SB:-}" ] || echo "/redwood/lib/libhakios.dylib SpringBoard" >> "$T/etc/inject.conf"
# the phone's own firmware, where it was taken from that phone (firmware/BOARD/fw/: the GPU's, the Wi-Fi
# DSP's, the network to join), uncompressed in the image: the drivers read it where it lies in memory
PLAIN=()
if [ -n "$BOARD" ] && [ -d "$IOS_REPO/firmware/$BOARD/fw" ]; then
	cp -r "$IOS_REPO/firmware/$BOARD/fw" "$T/fw"
	PLAIN=(--plain fw)
	echo "  firmware of $BOARD: $(find "$T/fw" -type f | wc -l) files"
else
	echo "root26-ios.sh: no phone's firmware in the image (board: ${BOARD:-none}; firmware/BOARD/fw/): no GPU, no Wi-Fi" >&2
fi
# the audio server's output device (inj26.sh): arm64e, in place of 16.5's arm64 one, which the audio server cannot load;
# the system image links /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver to it (mkimage26.py)
mkdir -p "$T/lib/HakiosAudio.driver"
cp "$OBJ/HakiosAudio" "$T/lib/HakiosAudio.driver/HakiosAudio"
cp "$US/src/audio_plugin.plist" "$T/lib/HakiosAudio.driver/Info.plist"
# the Metal plug-in (ios/userland/metal.sh), when it has been built: Metal opens it with boot-arg metal=1 through the
# system image's /System/Library/Extensions/HakiosMetal.bundle (mkimage26.py)
if [ -f "$OBJ/HakiosMetal" ]; then
	mkdir -p "$T/lib/HakiosMetal.bundle"
	cp "$OBJ/HakiosMetal" "$T/lib/HakiosMetal.bundle/HakiosMetal"
	cp "$IOS_REPO/metal/plugin/HakiosMetal.plist" "$T/lib/HakiosMetal.bundle/Info.plist"
	echo "  the Metal plug-in: lib/HakiosMetal.bundle"
fi

# mkjobs.py looks for the cryptexes beside the root filesystem, as ios165/ has them
S=$IOS/jobsrc
rm -rf "$S"; mkdir -p "$S"
ln -s "$IOS/rootfs" "$S/rootfs"
ln -s "$IOS/cryptex/os" "$S/cryptex"
ln -s "$IOS/cryptex/app" "$S/cryptex-app"

J=$IOS/jobs26
rm -rf "$J"
mkdir -p "$J"
cp "$US/com.hakios.demo.plist" "$J/"
# iOS 26's launchd, on a normal boot, refuses to start a job whose program is not a platform binary
# ("unexpected exec of non-platform binary: /redwood/sbin/init"); the kernel's AMFI stand-in takes a
# signature marked ad hoc (CodeDirectory flag CS_ADHOC) for one. Our programs are either unsigned or
# signed by ldid -S, which leaves the flags 0: every Mach-O here is signed again with -Cadhoc, keeping the
# entitlements it has.
ENT=$(mktemp)
python3 - "$T" <<'PY' | while read -r f; do
import os, struct, sys
for d, _, names in os.walk(sys.argv[1]):
    for n in names:
        p = os.path.join(d, n)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        with open(p, 'rb') as f:
            h = f.read(4)
        if len(h) == 4 and struct.unpack('<I', h)[0] == 0xfeedfacf:
            print(p)
PY
	"$LDID" -e "$f" > "$ENT" 2>/dev/null || : > "$ENT"
	if [ -s "$ENT" ]; then "$LDID" -Cadhoc -S"$ENT" "$f"; else "$LDID" -Cadhoc -S "$f"; fi
	echo "  signed ad hoc: ${f#$T/}"
done
rm -f "$ENT"
# The audio server (audiomxd, iOS 26's) with its output device above: volume, the slider in Control Center and the
# sound of a timer depend on it. bluetoothd stays enabled: without it audiomxd asks for com.apple.bluetooth.xpc
# thousands of times a second.
# shellcheck disable=SC2086  # (JOBS is a list of labels)
(cd "$US" && ALLOW="audio bluetooth ${ALLOW:-}" ./mkjobs.py --rest "$S/rootfs" "$J" $JOBS)
for job in "$J"/*audiomxd* "$J"/*bluetoothd*; do
	if [ -e "$job" ]; then echo "  job $(basename "$job")"; fi
done
# No EDR here: backboardd's job names com.apple.libEDR.Notification, whose server (EDRServerStartService)
# CoreBrightness starts, and CoreBrightness has no display to run here. launchd kept the port and queued
# every request: CoreAnimation in backboardd asks EDR at each display state change while holding the lock
# its display thread needs, and waited out libEDR's timeout, about 40 s: one frame in 40 s on iOS 27.
# Without the name a lookup fails at once and libEDR answers that there is no EDR.
python3 - "$J/com.apple.backboardd.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'rb') as f:
    job = plistlib.load(f)
if job.get('MachServices', {}).pop('com.apple.libEDR.Notification', None) is not None:
    with open(sys.argv[1], 'wb') as f:
        plistlib.dump(job, f)
    print('  backboardd: no com.apple.libEDR.Notification')
PY
# Jobs that cost the phone more than they give it: ScreenTimeAgent (Screen Time) keeps a core busy all the
# time on iOS 27 here (ustacks=top: ~90 % of a core, touched or not) while the window server draws in
# software. Without the job its Mach services are unknown and their clients fail at once.
# airplayd: the audio server starts it to look for AirPlay routes, and it dies at once (SIGSEGV), every 5 s.
# PerfPowerServices is refused by the kernel (its busy loop does not work here). Do not leave its
# Mach services advertised: PLClientLogger's synchronous permission request otherwise waits forever.
for job in com.apple.ScreenTimeAgent com.apple.airplayd com.apple.PerfPowerServices; do
	rm -f "$J/$job.plist" && echo "  no job $job"
done
# The last migration: the first generation's var/ says 20F66 (16.5), so iOS 26 took itself for just updated
# and ran DataMigrator, a progress bar on the screen in place of the lock screen. It is this build's own.
#
# MIGRATION_FROM=BUILD writes that build instead, which makes the system think it has just been updated
# FROM it and run the migration once. That is not a cosmetic difference: the account types this phone has
# none of -- ZACCOUNTTYPE is empty on every boot, so no client is ever authorized for the iTunes type, so
# AMS has no account, so the App Store never leaves its first launch phase -- are made by accountsd's own
# ACDDatabaseInitializer, and nothing ever asks it for them here (measured: its
# updateDefaultContentIfNecessary: is never entered). A migration is what asks.
python3 - "$T/var/mobile/Library/Preferences/com.apple.migration.plist" "$IOS/rootfs/System/Library/CoreServices/SystemVersion.plist" "${MIGRATION_FROM:-}" <<'PY'
import plistlib, sys
build = plistlib.load(open(sys.argv[2], 'rb'))['ProductBuildVersion']
last = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else build
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'LastSystemVersion': last, 'DMLastMigrationResults': {'success': True, 'buildVersion': last}}, f,
                  fmt=plistlib.FMT_BINARY)
PY
# APPSTORED_NO_MIGRATION=1: appstored says "[Migrate] [Daemon] Migrations needed" at every launch and
# then "Attempting to load bag"; two of its own Swift jobs then deadlock on one of its own unfair locks
# (measured: two threads in __os_unfair_lock_lock_slow whose frames land inside appstored itself), and
# the App Store's requests to it are cancelled one after another. Its own binary holds the other
# branch -- "[Daemon] No migration required for build: %{public}@" -- and what it compares against is
# LastOSBuildVersion in its preferences. Writing this build there is what takes that branch.
if [ -n "${APPSTORED_NO_MIGRATION:-}" ]; then
	python3 - "$T/var/mobile/Library/Preferences/com.apple.appstored.plist" \
	    "$IOS/rootfs/System/Library/CoreServices/SystemVersion.plist" <<'APPSTORED_PY'
import plistlib, sys
build = plistlib.load(open(sys.argv[2], 'rb'))['ProductBuildVersion']
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'LastOSBuildVersion': build}, f, fmt=plistlib.FMT_BINARY)
print('root26-ios.sh: appstored is told it has already migrated for build ' + build)
APPSTORED_PY
	# the walk that fills var.skel happens earlier in this script, so this one is listed by hand
	printf 'f\t644\t501\t501\t/private/var/mobile/Library/Preferences/com.apple.appstored.plist\t/var/mobile/Library/Preferences/com.apple.appstored.plist\n' >> $T/etc/var.skel
fi

# The persona manifest. usermanagerd reads /private/var/keybags/persona.kb as it starts and aborts without
# it ("Daemon failed to load persona manifest."), and every daemon that asks it for the user's personas
# waits (runningboardd, so backboardd and SpringBoard). On an iPhone the file is left by the restore; this
# /private/var is new every boot. An empty manifest of the version iOS 26 writes (1: a dictionary with
# UsePersonaManifestVersion, UsePersonaGenerationID and UserPersonaDictionary) has usermanagerd take its
# own way for a user without personas: "creating new set of default personas", in the kernel as well.
mkdir -p $T/var/keybags
python3 - "$T/var/keybags/persona.kb" <<'PY'
import plistlib, sys
manifest = {'UsePersonaManifestVersion': 1, 'UsePersonaGenerationID': 1, 'UserPersonaDictionary': {}}
with open(sys.argv[1], 'wb') as f:
    plistlib.dump(manifest, f, fmt=plistlib.FMT_BINARY)
PY
printf 'd\t700\t0\t0\t/private/var/keybags\nf\t600\t0\t0\t/private/var/keybags/persona.kb\t/var/keybags/persona.kb\n' >> $T/etc/var.skel
# Feature flags (/Library/Preferences/FeatureFlags is /private/var/preferences/FeatureFlags; Domain/NAME.plist,
# {feature: {Enabled: bool}}). MobileTimerFlags/core_data off: with it, mobiletimerd keeps alarms and timers in a
# Core Data store that never finishes loading here, and answers every request "System isn't ready..." -- the
# Clock's start button and Control Center's timer do nothing. Off, it keeps them in its defaults, as before.
mkdir -p $T/var/preferences/FeatureFlags/Domain
python3 - "$T/var/preferences/FeatureFlags/Domain" <<'PY'
import plistlib, sys
flags = {'MobileTimerFlags': {'core_data': {'Enabled': False}}}
for domain, features in flags.items():
    with open(f'{sys.argv[1]}/{domain}.plist', 'wb') as f:
        plistlib.dump(features, f)
PY
printf 'd\t755\t0\t0\t/private/var/preferences/FeatureFlags\nd\t755\t0\t0\t/private/var/preferences/FeatureFlags/Domain\n' >> $T/etc/var.skel
for f in $T/var/preferences/FeatureFlags/Domain/*.plist; do
	printf 'f\t644\t0\t0\t/private/var/preferences/FeatureFlags/Domain/%s\t/var/preferences/FeatureFlags/Domain/%s\n' \
		"${f##*/}" "${f##*/}" >> $T/etc/var.skel
done
# PosterBoard: a first wallpaper that draws without Metal, the collections poster (src: ios/userland/inject_poster.c)
echo "/redwood/lib/libhakios-poster.dylib PosterBoard" >> $T/etc/inject.conf
# CollectionsPoster, the WallpaperKit extension that draws the collections poster (src: ios/userland/inject_wallpaper.m)
echo "/redwood/lib/libhakios-wallpaper.dylib CollectionsPoster" >> $T/etc/inject.conf
# SpringBoard: display mode changes that complete, there being no display driver to say so (src:
# ios/userland/inject_backlight.c)
echo "/redwood/lib/libhakios-backlight.dylib SpringBoard" >> $T/etc/inject.conf
# The brightness slider (Control Center in SpringBoard, Settings) and Auto-Brightness, to the kernel's
# hw.backlight; backboardd for CoreBrightness's own side (src: ios/userland/inject_brightness.c). It does
# nothing where the kernel has no backlight (boot-arg backlight=0).
echo "/redwood/lib/libhakios-brightness.dylib SpringBoard backboardd Preferences" >> $T/etc/inject.conf
# cameracaptured, iOS 27's camera daemon: the flashlight's server given the kernel's flashlight (src:
# ios/userland/inject_capture.c). It does nothing where the kernel has none (boot-arg torch=3).
echo "/redwood/lib/libhakios-capture.dylib cameracaptured" >> $T/etc/inject.conf
# Camera.app: its preview's picture given a size where AVFoundation leaves it none (ios/userland/inject_camera_app.c)
echo "/redwood/lib/libhakios-camapp.dylib Camera" >> $T/etc/inject.conf
# STOREFRONT=1: the App Store's SAP (Mescal) signature from the host's sapd, and a storefront answered, so the
# storefront-token request is signed and amp-api returns the catalog (ios/userland/inject_store.c, inject_mescal.m).
# STORE_FROM_VAR=1 puts the store library in /var instead, with a copy of it in var.skel so it is always
# there at the first exec. /var is writable and kept across runs, so the library can be replaced over SSH
# between two launches of the App Store -- a change to it then costs a rebuild of one dylib and a kill, not
# a new image and a new boot (the difference is about eight minutes a question).
if [ -n "${STOREFRONT:-}" ]; then
	if [ -n "${STORE_FROM_VAR:-}" ]; then
		mkdir -p $T/var/hakios
		cp "$OBJ/libhakios-store.dylib" $T/var/hakios/libhakios-store.dylib
		printf 'd\t755\t0\t0\t/private/var/hakios\n' >> $T/etc/var.skel
		printf 'f\t644\t0\t0\t/private/var/hakios/libhakios-store.dylib\t/var/hakios/libhakios-store.dylib\n' >> $T/etc/var.skel
		echo "/var/hakios/libhakios-store.dylib ${STORE_PROCS:-appstored amsaccountsd AppStore accountsd}" >> $T/etc/inject.conf
	else
		echo "/redwood/lib/libhakios-store.dylib ${STORE_PROCS:-appstored amsaccountsd AppStore accountsd}" >> $T/etc/inject.conf
	fi
	echo "/redwood/lib/libhakios-mescal.dylib appstored itunesstored AppStore" >> $T/etc/inject.conf
	# STORE_CONF="KEY=VALUE;KEY=VALUE": the store library's switches, in the file it reads on the phone
	# (/var/hakios-store.conf). They have to be there before the daemons start, and /var is new every
	# boot, so the file comes from the image. HAKIOS_MAKE_TYPES=1 is the one that matters: with it
	# accountsd builds its default content -- 55 account types, the iTunes Store among them -- which this
	# phone has never had, and without which no client is authorized for a type and AMS has no account.
	if [ -n "${STORE_CONF:-}" ]; then
		mkdir -p $T/var
		printf '%s\n' "$STORE_CONF" | tr ';' '\n' > $T/var/hakios-store.conf
		printf 'f\t644\t0\t0\t/private/var/hakios-store.conf\t/var/hakios-store.conf\n' >> $T/etc/var.skel
	fi
fi
# SIGNIN=1: and the keychain class that Apple ID sign-in cannot store here (ios/userland/inject_kc.c). Apart
# from a sign-in it changes nothing, so it is its own knob.
if [ -n "${SIGNIN:-}" ]; then
	echo "/redwood/lib/libhakios-kc.dylib akd accountsd" >> $T/etc/inject.conf
fi
# The kernel matches executable names, not the build template's @apps token.
python3 - "$T/etc/inject.conf" "$IOS/rootfs" <<'PY'
import pathlib, plistlib, sys
conf, root = map(pathlib.Path, sys.argv[1:])
names = set()
for base in ('Applications', 'private/var/staged_system_apps'):
    for info in (root / base).glob('*.app/Info.plist'):
        with info.open('rb') as f:
            info = plistlib.load(f)
            name = info.get('CFBundleExecutable') if 'hidden' not in info.get('SBAppTags', []) else None
        if name and ' ' not in name:        # (names are separated by blanks in kern.exec_inject)
            names.add(name)
lines = []
for line in conf.read_text().splitlines():
    words = line.split()
    if '@apps' in words:
        words = [word for word in words if word != '@apps'] + sorted(names)
    lines.append(' '.join(words))
text = '\n'.join(lines) + '\n'
if len(text.encode()) >= 4096:
    raise SystemExit('inject.conf exceeds kern.exec_inject capacity')
conf.write_text(text)
PY
# chronod's CoreDuet store cannot create its missing parent directories in the fresh RAM /var.
printf 'd\t755\t501\t501\t/private/var/mobile/Library/CoreDuet\nd\t755\t501\t501\t/private/var/mobile/Library/CoreDuet/Knowledge\n' >> $T/etc/var.skel
# CONSOLE_LOG="NAME ...": these programs get libhakios-console.dylib (standard error on the console; with
# boot-arg exec_env=NAME,...:OS_ACTIVITY_DT_MODE=1 their os_log as well)
if [ -n "${CONSOLE_LOG:-}" ]; then
	echo "/redwood/lib/libhakios-console.dylib $CONSOLE_LOG" >> $T/etc/inject.conf
fi
# PRIVATE_LOG=1: os_log's private arguments shown (com.apple.system.logging's Enable-Private-Data): what "<private>" hides,
# for a diagnostic image only
if [ -n "${PRIVATE_LOG:-}" ]; then
	mkdir -p $T/var/preferences/Logging
	python3 - "$T/var/preferences/Logging/com.apple.system.logging.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'Enable-Private-Data': True}, f)
PY
	printf 'f\t644\t0\t0\t/private/var/preferences/Logging/com.apple.system.logging.plist\t/var/preferences/Logging/com.apple.system.logging.plist\n' >> $T/etc/var.skel
fi
# LOGGING_DEBUG="SUBSYSTEM ...": os_log of those subsystems at every level, Debug included (on the console
# with boot-arg exec_env=NAME:OS_ACTIVITY_DT_MODE=1); /Library/Preferences is /private/var/preferences
for subsystem in ${LOGGING_DEBUG:-}; do
	mkdir -p $T/var/preferences/Logging/Subsystems
	python3 - "$T/var/preferences/Logging/Subsystems/$subsystem.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'DEFAULT-OPTIONS': {'Level': {'Enable': 'Debug', 'Persist': 'Debug'}}}, f)
PY
	printf 'f\t644\t0\t0\t/private/var/preferences/Logging/Subsystems/%s.plist\t/var/preferences/Logging/Subsystems/%s.plist\n' \
		"$subsystem" "$subsystem" >> $T/etc/var.skel
done
# what is running, at the start and twice later (net_test waits out its 60 s with no Wi-Fi);
# RC_JOBS=FILE: that file instead (e.g. gestures by rwtouch)
if [ -n "${RC_JOBS:-}" ]; then
	cp "$RC_JOBS" $T/etc/rc.jobs
else
cat > $T/etc/rc.jobs <<'EOF'
# iOS 26/27: towards SpringBoard
/bin/ps -A
# (with wifi=1: en0 configured by configd's own Wi-Fi service, or else DHCP asked for it; without, nothing)
/redwood/bin/net_test wifi
/redwood/bin/net_test waitaddr en0 60
/bin/ps -A
/redwood/bin/net_test waitaddr en0 60
/bin/ps -A
EOF
fi
# (first: /private/var saved as the run ends, with boot-arg var_store=1; without it var_store ends at once)
{ echo "/redwood/bin/var_store watch &"; cat $T/etc/rc.jobs; } > $T/etc/rc.jobs.new && mv $T/etc/rc.jobs.new $T/etc/rc.jobs
python3 "$IOS_REPO/userland/tools/apfs/mkapfs.py" -z "${PLAIN[@]}" -o "$IOS/root26-ios.apfs" redwood="$T" | tail -1
ls -la "$IOS/root26-ios.apfs"

# The RAM part of a boot whose system image is on the phone's flash (boot-arg root_apfs=DEV): the kernel's first
# root, the resident RAM disk md0 -- stage0 as /sbin/launchd, which mounts this small image on /redwood, and the
# mount points it and init use. init gives it up for the system image on the flash (userland/src/init.c,
# flash_root), and it stays under that one's /private/preboot.
F=$IOS/flashboot-tree
rm -rf "$F"
mkdir -p "$F/sbin" "$F/redwood" "$F/dev" "$F/flash"
cp "$OBJ/stage0" "$F/sbin/launchd"
python3 "$IOS_REPO/userland/tools/apfs/mkapfs.py" -o "$IOS/flashboot.apfs" Boot="$F" | tail -1
# (said again last: the warning above scrolls by in the middle of the build, and an image without the GPU's firmware
# boots to the software renderer, a run's worth of measurements of nothing)
if [ ${#PLAIN[@]} = 0 ]; then
	echo "root26-ios.sh: NO FIRMWARE in this image (board: ${BOARD:-none}; export HAKIOS_DEVICES?): no GPU, no Wi-Fi" >&2
fi
