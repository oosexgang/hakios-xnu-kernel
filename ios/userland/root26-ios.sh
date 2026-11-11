#!/bin/bash
# The small image (/redwood) for an iOS 26/27 boot towards SpringBoard, and the launchd jobs.
#
# The image is made here from the first generation's sources (ios/userland/env.sh: US): its init and stage0,
# our arm64e libraries (inj26.sh, in $OBJ), the template of /private/var (ios/tools/unpack.sh's var.skel)
# and the phone's own firmware (firmware/BOARD/fw/, never published). The jobs are made from iOS's own
# plists (US/mkjobs.py: the named jobs as they are, every other job with Mach services on demand only) into
# $IOS/jobs26, for the system image (LAUNCHDAEMONS=$IOS/jobs26 mkimage26.py): iOS 26's launchd takes a job
# for a system service only when its plist is on the system volume.
# Writes $IOS/root26-ios.apfs, $IOS/jobs26 and $OBJ/stage0 (the system image's /sbin/launchd).
# JOBS="label ..." replaces the named jobs; OSLOG as for mkjobs.py; RC_JOBS=FILE: what com.redwood.demo runs.
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
"}
[ -f "$OBJ/libredwood.dylib" ] || { echo "root26-ios.sh: no libraries in $OBJ: ios/userland/inj26.sh first" >&2; exit 1; }
[ -s "$IOS/var.skel" ] || { echo "root26-ios.sh: no $IOS/var.skel: ios/tools/unpack.sh makes it" >&2; exit 1; }

# --- our programs ------------------------------------------------------------------------------------
# process 1 and the job runner (init), and the system image's /sbin/launchd (stage0, which mounts this
# image on /redwood and starts init): static, without libc or dyld (libmini)
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
$CC -I"$US/libmini" -c "$US/libmini/mini.c" -o "$OBJ/mini.o"
$CC -c "$US/libmini/start.S" -o "$OBJ/start.o"
for prog in init stage0; do
	$CC -I"$US/libmini" $LD -o "$OBJ/$prog" "$US/src/$prog.c" "$OBJ/mini.o" "$OBJ/start.o"
done
# what the default job list asks the network (net_test waitaddr), as the libraries are: arm64e, pacstrip
$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o "$OBJ/net_test" "$US/src/net_test.m"
"$PACSTRIP" macho "$OBJ/net_test" "$OBJ/net_test.strip" > /dev/null && mv "$OBJ/net_test.strip" "$OBJ/net_test"

# --- the tree ----------------------------------------------------------------------------------------
rm -rf "$T"
mkdir -p "$T/sbin" "$T/bin" "$T/etc" "$T/lib" "$T/share" "$T/LaunchDaemons"
cp "$OBJ/init" "$T/sbin/init"
cp "$OBJ/net_test" "$T/bin/net_test"
cp "$OBJ"/*.dylib "$T/lib/"
cp "$US/rc.launchd" "$T/etc/rc"
python3 -c "import plistlib, sys; m = plistlib.load(open(sys.argv[1], 'rb')); print('HakiOS: iOS', m['ProductVersion'], m['ProductBuildVersion'])" \
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
/redwood/lib/libredwood.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libredwood-sb.dylib SpringBoard
/redwood/lib/libredwood-statusbar.dylib @apps
CONF
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
# the system image links /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver to it (mkimage26.py)
mkdir -p "$T/lib/RedwoodAudio.driver"
cp "$OBJ/RedwoodAudio" "$T/lib/RedwoodAudio.driver/RedwoodAudio"
cp "$US/src/audio_plugin.plist" "$T/lib/RedwoodAudio.driver/Info.plist"

# mkjobs.py looks for the cryptexes beside the root filesystem, as ios165/ has them
S=$IOS/jobsrc
rm -rf "$S"; mkdir -p "$S"
ln -s "$IOS/rootfs" "$S/rootfs"
ln -s "$IOS/cryptex/os" "$S/cryptex"
ln -s "$IOS/cryptex/app" "$S/cryptex-app"

J=$IOS/jobs26
rm -rf "$J"
mkdir -p "$J"
cp "$US/com.redwood.demo.plist" "$J/"
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
python3 - "$T/var/mobile/Library/Preferences/com.apple.migration.plist" "$IOS/rootfs/System/Library/CoreServices/SystemVersion.plist" <<'PY'
import plistlib, sys
build = plistlib.load(open(sys.argv[2], 'rb'))['ProductBuildVersion']
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'LastSystemVersion': build, 'DMLastMigrationResults': {'success': True, 'buildVersion': build}}, f,
                  fmt=plistlib.FMT_BINARY)
PY
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
echo "/redwood/lib/libredwood-poster.dylib PosterBoard" >> $T/etc/inject.conf
# SpringBoard: display mode changes that complete, there being no display driver to say so (src:
# ios/userland/inject_backlight.c)
echo "/redwood/lib/libredwood-backlight.dylib SpringBoard" >> $T/etc/inject.conf
# The brightness slider (Control Center in SpringBoard, Settings) and Auto-Brightness, to the kernel's
# hw.backlight; backboardd for CoreBrightness's own side (src: ios/userland/inject_brightness.c). It does
# nothing where the kernel has no backlight (boot-arg backlight=0).
echo "/redwood/lib/libhakios-brightness.dylib SpringBoard backboardd Preferences" >> $T/etc/inject.conf
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
        if name:
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
# CONSOLE_LOG="NAME ...": these programs get libredwood-console.dylib (standard error on the console; with
# boot-arg exec_env=NAME,...:OS_ACTIVITY_DT_MODE=1 their os_log as well)
if [ -n "${CONSOLE_LOG:-}" ]; then
	echo "/redwood/lib/libredwood-console.dylib $CONSOLE_LOG" >> $T/etc/inject.conf
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
/redwood/bin/net_test waitaddr en0 60
/bin/ps -A
/redwood/bin/net_test waitaddr en0 60
/bin/ps -A
EOF
fi
python3 "$IOS_REPO/userland/tools/apfs/mkapfs.py" -z "${PLAIN[@]}" -o "$IOS/root26-ios.apfs" redwood="$T" | tail -1
ls -la "$IOS/root26-ios.apfs"
