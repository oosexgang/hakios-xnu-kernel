#!/bin/bash
# Our libraries for iOS 26/27, arm64e, into $OBJ (ios/userland/env.sh): the ones the kernel inserts into
# a few processes (etc/inject.conf of the small image), and the audio server's output device.
# BUILD: the iOS build they are for (23G90 = 26.6.2, 24B119 = 27.2); default: the unpacked IPSW's.
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=env.sh
source "$RN/userland/env.sh"
BUILD=${BUILD:-$(python3 -c "import plistlib, sys; print(plistlib.load(open(sys.argv[1], 'rb'))['ProductBuildVersion'])" "$IOS/BuildManifest.plist")}
O=$OBJ
mkdir -p "$O" "$O/src"
cp "$US"/src/*.c "$US"/src/*.h "$O/src/"
# SpringBoard's methods that are replaced, at the build's addresses: only those whose code was read and
# found to do the same as on 16.5 (inject_sb.c checks each against the address it expects and leaves a
# method alone otherwise).
#   -[SBLockdownManager _setupActivationState]: the activation state an int at +8, 2 = activated
#   -[SBSetupManager _setSetupRequiredReason:]: the reason kept at +8, 0 = no setup assistant
#   +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:]: 0 PNG, 1 ATX (ASTC, compressed on the processor),
#     3 JPEG, chosen by two flags of the device and the snapshot's opacity, as on 16.5
#   -[SBLockHardwareButton longPress:]: the side button's long press (Siri), given the gesture recognizer, as on 16.5
#   -[SBPolicyAggregator _allowsCapabilityPosterSwitcherWithExplanation:] (16.5's SBMainDisplayPolicyAggregator): a BOOL
#     and the explanation through its pointer, as on 16.5; on 27.2 the switcher is offered (HAKIOS_POSTER_SWITCHER=1):
#     with PosterBoard a launch angel again (mkimage26.py) it opens, where 16.5's is black
case $BUILD in
23G90)	SB_ACTIVATION=0x22450fdc0 SB_SETUP=0x224675b68 SB_SNAPSHOT_FORMAT='' SB_LONG_PRESS='' SB_POSTER='' ;;
24B119)	SB_ACTIVATION=0x2274356b8 SB_SETUP=0x22757d3fc SB_SNAPSHOT_FORMAT=0x2b42c8d38 SB_LONG_PRESS=0x227922350 SB_POSTER=0x2272817c4 ;;   # 27.2: the reason at +0x38 now; the hook passes 0 on
*)	echo "inj26.sh: no SpringBoard addresses for $BUILD: those hooks will be left alone" >&2
	SB_ACTIVATION='' SB_SETUP='' SB_SNAPSHOT_FORMAT='' SB_LONG_PRESS='' SB_POSTER='' ;;
esac
[ -z "$SB_ACTIVATION" ] || sed -i -e "s/0x1c4bc8dfcUL/${SB_ACTIVATION}UL/" -e "s/0x1c4cdd3a4UL/${SB_SETUP}UL/" "$O/src/inject_sb.c"
[ -z "$SB_SNAPSHOT_FORMAT" ] || sed -i -e "s/0x1f1c91480UL/${SB_SNAPSHOT_FORMAT}UL/" "$O/src/inject_sb.c"
[ -z "$SB_LONG_PRESS" ] || sed -i -e "s/0x1c4f6ab44UL/${SB_LONG_PRESS}UL/" "$O/src/inject_sb.c"
[ -z "$SB_POSTER" ] || sed -i -e "s/0x1c507e0fcUL/${SB_POSTER}UL/" -e 's/"SBMainDisplayPolicyAggregator"/"SBPolicyAggregator"/' "$O/src/inject_sb.c"
$DYN -dynamiclib -install_name /redwood/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios.dylib" "$O/src/inject.c"
# the status bar's clock past the hole punch: ios/userland/inject_statusbar.c (16.5's statusbar.c checks 16.5's addresses)
# (what it knows of each board, hakios_boards.h, is made from the device packages that are there now)
"$RN/../tools/boards.py" gen --out "$O/boards" > /dev/null
$DYN -DHAKIOS_POSTER_SWITCHER=1 -dynamiclib -install_name /redwood/lib/libhakios-sb.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios-sb.dylib" "$O/src/inject_sb.c" -I"$O/boards" "$RN/userland/inject_statusbar.c"
# the layer tree of any application on the notification com.hakios.layers (rwtouch notify): inject_sb.c alone, which in a
# program that is not SpringBoard does only that (root26-ios.sh LAYERS="NAME ...")
$DYN -dynamiclib -install_name /redwood/lib/libhakios-layers.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios-layers.dylib" "$O/src/inject_sb.c"
$DYN -dynamiclib -install_name /redwood/lib/libhakios-statusbar.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios-statusbar.dylib" -I"$O/boards" "$RN/userland/inject_statusbar.c"
# the audio server's output device (AudioServerPlugIn, arm64e: the audio server is): the speaker route and its
# volume, Control Center's slider included; the samples go to /dev/hakios-audio when the kernel has it
$DYN -DHAKIOS_IOS27 -bundle -framework CoreFoundation -o "$O/HakiosAudio" "$US/src/audio_plugin.c"
$DYN -dynamiclib -install_name /redwood/lib/libhakios-app.dylib -lobjc -o "$O/libhakios-app.dylib" "$O/src/inject_app.c"
# the tweak loader of the package manager's prefix (userland/src/inject_tweaks.c, as on 16.5): SpringBoard's library
# opens it when there is a /var/jb (inject_sb.c, load_tweaks), and etc/inject.conf names it for Settings
$DYN -dynamiclib -install_name /redwood/lib/libhakios-tweaks.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-tweaks.dylib" "$O/src/inject_tweaks.c"
# ... and a tweak to check that path with (ios/userland/tweak_test.c: Settings, hooked through ElleKit)
$DYN -dynamiclib -install_name /var/jb/usr/lib/TweakInject/hakios-tweaktest.dylib -lobjc -o "$O/hakios-tweaktest.dylib" "$RN/userland/tweak_test.c"
$DYN -dynamiclib -install_name /redwood/lib/libhakios-console.dylib -o "$O/libhakios-console.dylib" "$RN/userland/console_log.c"
$DYN -dynamiclib -install_name /redwood/lib/libhakios-poster.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-poster.dylib" "$RN/userland/inject_poster.c"
$DYN -dynamiclib -install_name /redwood/lib/libhakios-wallpaper.dylib -framework Foundation -framework QuartzCore -framework CoreGraphics -lobjc -o "$O/libhakios-wallpaper.dylib" "$RN/userland/inject_wallpaper.m"
$DYN -dynamiclib -install_name /redwood/lib/libhakios-backlight.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-backlight.dylib" "$RN/userland/inject_backlight.c"
# the flashlight and the camera in cameracaptured, iOS 27's camera daemon: the kernel's /dev/hakios-torch and
# /dev/hakios-camera (ios/userland/inject_capture.c; the capture device and its picture processor are 16.5's,
# userland/src/camera_device.c and camera_isp.c, with the iPhone 11's formats: HAKIOS_IOS27)
$DYN -O2 -DHAKIOS_IOS27 -dynamiclib -install_name /redwood/lib/libhakios-capture.dylib -framework CoreFoundation \
	-framework CoreMedia -framework CoreVideo -framework IOSurface -lobjc -I"$O/src" -o "$O/libhakios-capture.dylib" \
	"$RN/userland/inject_capture.c" "$O/src/camera_device.c" "$O/src/camera_isp.c"
# the brightness slider and Auto-Brightness to the kernel's hw.backlight (any device: ios/userland/inject_brightness.c)
$DYN -dynamiclib -install_name /redwood/lib/libhakios-brightness.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-brightness.dylib" "$RN/userland/inject_brightness.c"
for f in "$O"/*.dylib "$O/HakiosAudio"; do
	"$PACSTRIP" macho "$f" "$f.strip" > /dev/null && mv "$f.strip" "$f"
	"$LDID" -S "$f"
	echo "  $(basename "$f"): $(file -b "$f" | cut -c1-60)"
done
