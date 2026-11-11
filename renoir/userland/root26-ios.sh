#!/bin/bash
# The small image (/redwood) for an iOS 26 boot towards SpringBoard: darwin-renoir's root with our
# arm64e libraries; and the launchd jobs, made from iOS 26's own plists (darwin-renoir's mkjobs.py: the
# named jobs as they are, every other job with Mach services on demand only) into ios26/jobs26, for
# the system image (LAUNCHDAEMONS=.../jobs26 mkimage26.py): iOS 26's launchd takes a job for a system
# service only when its plist is on the system volume.
# Writes $IOS/root26-ios.apfs and $IOS/jobs26 (IOS=.../ios27 for iOS 27, with OBJ=.../obj27).
# JOBS="label ..." replaces the named jobs; OSLOG as for mkjobs.py.
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}   # the directory holding this repository, darwin-renoir and ios26/ios27
DR=${DR:-$DK/darwin-renoir}
IOS=${IOS:-$DK/ios26}            # the extracted IPSW (ios26: 23G90, ios27: 24B119); the outputs go there
OBJ=${OBJ:-$RN/userland/obj26}   # our arm64e libraries (inj26.sh), built for that IPSW's addresses
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

# mkjobs.py looks for the cryptexes beside the root filesystem, as ios165/ has them
S=$IOS/jobsrc
rm -rf $S; mkdir -p $S
ln -s $IOS/rootfs $S/rootfs
ln -s $IOS/cryptex/os $S/cryptex
ln -s $IOS/cryptex/app $S/cryptex-app

J=$IOS/jobs26
rm -rf $T $J; cp -a $DR/userland/root $T
mkdir -p $J; cp $T/LaunchDaemons/com.redwood.demo.plist $J/
rm -f $T/LaunchDaemons/*.plist
cp $OBJ/*.dylib $T/lib/
cp $OBJ/gl_test $T/bin/gl_test
# the audio server's output device (inj26.sh): arm64e, in place of 16.5's arm64 one, which the audio server cannot load;
# the system image links /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver to it (mkimage26.py)
if [ -f $OBJ/RedwoodAudio ]; then
	cp $OBJ/RedwoodAudio $T/lib/RedwoodAudio.driver/RedwoodAudio
	cp "$(dirname "$RN")/userland/userland/src/audio_plugin.plist" $T/lib/RedwoodAudio.driver/Info.plist
fi
# iOS 26's launchd, on a normal boot, refuses to start a job whose program is not a platform binary
# ("unexpected exec of non-platform binary: /redwood/sbin/init"); the kernel's AMFI stand-in takes a
# signature marked ad hoc (CodeDirectory flag CS_ADHOC) for one. darwin-renoir's programs are either
# unsigned or signed by ldid -S, which leaves the flags 0: every Mach-O here is signed again with
# -Cadhoc, keeping the entitlements it has.
LDID=${LDID:-$DR/build-tools/theos/toolchain/linux/iphone/bin/ldid}
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
(cd $DR/userland && ALLOW="audio bluetooth ${ALLOW:-}" ./mkjobs.py --rest $S/rootfs $J $JOBS)
ls $J | grep -E "audiomxd|bluetoothd" | sed 's/^/  job /'
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
# The last migration: darwin-renoir's skeleton says 20F66 (16.5), so iOS 26 took itself for just updated
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
# PosterBoard: a first wallpaper that draws without Metal, the collections poster (src: renoir/userland/inject_poster.c)
echo "/redwood/lib/libredwood-poster.dylib PosterBoard" >> $T/etc/inject.conf
# SpringBoard: display mode changes that complete, there being no display driver to say so (src:
# renoir/userland/inject_backlight.c)
echo "/redwood/lib/libredwood-backlight.dylib SpringBoard" >> $T/etc/inject.conf
# The brightness slider (Control Center in SpringBoard, Settings) and Auto-Brightness, to the kernel's
# hw.backlight; backboardd for CoreBrightness's own side (src: renoir/userland/inject_brightness.c). It does
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
# RC_JOBS=FILE: that file instead (darwin-renoir's convention, e.g. gestures by rwtouch)
if [ -n "${RC_JOBS:-}" ]; then
	cp "$RC_JOBS" $T/etc/rc.jobs
else
cat > $T/etc/rc.jobs <<'EOF'
# iOS 26 on renoir: towards SpringBoard
/bin/ps -A
/redwood/bin/net_test waitaddr en0 60
/bin/ps -A
/redwood/bin/net_test waitaddr en0 60
/bin/ps -A
EOF
fi
cd $DR/userland
python3 ../tools/apfs/mkapfs.py -z --plain fw --plain fw-renoir --plain fw-wifi-renoir --plain fw-audio-renoir \
	-o $IOS/root26-ios.apfs redwood=$T | tail -1
ls -la $IOS/root26-ios.apfs
