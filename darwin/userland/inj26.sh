#!/bin/bash
# Our libraries for iOS 26/27, arm64e, into $OBJ (darwin/userland/env.sh): the ones the kernel inserts into
# a few processes (etc/inject.conf of the small image), and the audio server's output device.
# BUILD: the iOS build they are for (23G90 = 26.6.2, 24B119 = 27.2); default: the unpacked IPSW's.
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=env.sh
source "$RN/userland/env.sh"
sed_inplace() {
	if [ "$(uname -s)" = Darwin ]; then sed -i '' "$@"; else sed -i "$@"; fi
}
BUILD=${BUILD:-$(python3 -c "import plistlib, sys; print(plistlib.load(open(sys.argv[1], 'rb'))['ProductBuildVersion'])" "$IOS/BuildManifest.plist")}
O=$OBJ
mkdir -p "$O" "$O/src"
cp "$US"/src/*.c "$O/src/"
cp "$US"/src/*.h "$O/src/"
cp -R "$US/src/board" "$O/src/"    # (the headers the libraries share with the kernel's drivers: board/input.h, ...)
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
SB_DEFS=()
case $BUILD in
23G90)	SB_ACTIVATION=0x22450fdc0 SB_SETUP=0x224675b68 SB_SNAPSHOT_FORMAT='' SB_LONG_PRESS='' SB_POSTER='' ;;
24B119)	SB_ACTIVATION=0x2274356b8 SB_SETUP=0x22757d3fc SB_SNAPSHOT_FORMAT=0x2b42c8d38 SB_LONG_PRESS=0x227922350 SB_POSTER=0x2272817c4 SB_DEFS=(-DHAKIOS_IOS27) ;;   # 27.2: the reason at +0x38 now; the hook passes 0 on
*)	echo "inj26.sh: no SpringBoard addresses for $BUILD: those hooks will be left alone" >&2
	SB_ACTIVATION='' SB_SETUP='' SB_SNAPSHOT_FORMAT='' SB_LONG_PRESS='' SB_POSTER='' ;;
esac
[ -z "$SB_ACTIVATION" ] || sed_inplace -e "s/0x1c4bc8dfcUL/${SB_ACTIVATION}UL/" -e "s/0x1c4cdd3a4UL/${SB_SETUP}UL/" "$O/src/inject_sb.c"
[ -z "$SB_SNAPSHOT_FORMAT" ] || sed_inplace -e "s/0x1f1c91480UL/${SB_SNAPSHOT_FORMAT}UL/" "$O/src/inject_sb.c"
[ -z "$SB_LONG_PRESS" ] || sed_inplace -e "s/0x1c4f6ab44UL/${SB_LONG_PRESS}UL/" "$O/src/inject_sb.c"
[ -z "$SB_POSTER" ] || sed_inplace -e "s/0x1c507e0fcUL/${SB_POSTER}UL/" -e 's/"SBMainDisplayPolicyAggregator"/"SBPolicyAggregator"/' "$O/src/inject_sb.c"
$DYN -dynamiclib -install_name /hakios/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios.dylib" "$O/src/inject.c"
# the status bar's clock past the hole punch: components/statusbar/inject_statusbar.c (16.5's statusbar.c checks 16.5's addresses)
# (what it knows of each board, user_boards.h, is made from the device packages that are there now)
"$RN/../tools/boards.py" gen --out "$O/boards" > /dev/null
$DYN "${SB_DEFS[@]}" -DHAKIOS_POSTER_SWITCHER=1 -dynamiclib -install_name /hakios/lib/libhakios-sb.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios-sb.dylib" "$O/src/inject_sb.c" -I"$O/boards" "$RN/../components/statusbar/inject_statusbar.c"
# Camera.app's preview given a size where AVFoundation leaves it none (components/camera/src/inject_camera_app.c)
$DYN -dynamiclib -install_name /hakios/lib/libhakios-camapp.dylib -lobjc -o "$O/libhakios-camapp.dylib" "$RN/../components/camera/src/inject_camera_app.c"
# (libhakios-statusbar.dylib, in every application, also names an uncaught exception on the console: darwin/userland/uncaught.c)
# the App Store's SAP (Mescal) signature, made by the host's sapd, and the storefront it answers with
# (components/apple-id/src/inject_mescal.m and inject_store.c; root26-darwin.sh STOREFRONT=1 injects them into the store daemons)
$DYN -dynamiclib -install_name /hakios/lib/libhakios-mescal.dylib -framework Foundation -lobjc -fobjc-arc -o "$O/libhakios-mescal.dylib" "$RN/../components/apple-id/src/inject_mescal.m"
# ... and the one keychain class Apple ID sign-in cannot store on a phone with no passcode
# (components/apple-id/src/inject_kc.c, 16.5's: it needs the prefix's MSHookFunction; root26-darwin.sh SIGNIN=1)
$DYN -O2 -fblocks -dynamiclib -install_name /hakios/lib/libhakios-kc.dylib -framework CoreFoundation \
	-framework Security -lobjc -o "$O/libhakios-kc.dylib" "$RN/../components/apple-id/src/inject_kc.c"
$DYN -fblocks -dynamiclib -install_name /hakios/lib/libhakios-store.dylib -framework CoreFoundation -lobjc -lc++abi -framework Foundation -o "$O/libhakios-store.dylib" "$RN/../components/apple-id/src/inject_store.c"
# hactivation: inside mobileactivationd, make MobileGestalt read as a development-fused part so the daemon
# short-circuits activation state to "Activated" (ported from 16.5; unblocked the App Store there). The
# never-set-up device is otherwise Unactivated, which a real browsing iPhone never is. (root26 INJECT_MORE)
$DYN -fblocks -dynamiclib -install_name /hakios/lib/libhakios-mad.dylib -framework CoreFoundation -lobjc -Wl,-undefined,dynamic_lookup -o "$O/libhakios-mad.dylib" "$RN/../components/apple-id/src/inject_mad.c"
# the layer tree of any application on the notification com.hakios.layers (rwtouch notify): inject_sb.c alone, which in a
# program that is not SpringBoard does only that (root26-darwin.sh LAYERS="NAME ...")
$DYN -dynamiclib -install_name /hakios/lib/libhakios-layers.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios-layers.dylib" "$O/src/inject_sb.c"
$DYN -dynamiclib -install_name /hakios/lib/libhakios-statusbar.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libhakios-statusbar.dylib" -I"$O/boards" "$RN/../components/statusbar/inject_statusbar.c" "$RN/userland/uncaught.c"
# the audio server's output device (AudioServerPlugIn, arm64e: the audio server is): the speaker route and its
# volume, Control Center's slider included; the samples go to /dev/hakios-audio when the kernel has it
$DYN -DHAKIOS_IOS27 -bundle -framework CoreFoundation -o "$O/HakiosAudio" "$RN/../components/audio/audio_plugin.c"
$DYN -dynamiclib -install_name /hakios/lib/libhakios-app.dylib -lobjc -o "$O/libhakios-app.dylib" "$O/src/inject_app.c"
# the tweak loader of the package manager's prefix (components/tweaks/inject_tweaks.c, as on 16.5): SpringBoard's library
# opens it when there is a /var/jb (inject_sb.c, load_tweaks), and etc/inject.conf names it for Settings
$DYN -dynamiclib -install_name /hakios/lib/libhakios-tweaks.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-tweaks.dylib" "$RN/../components/tweaks/inject_tweaks.c"
# ... and a tweak to check that path with (components/tweaks/tweak_test.c: Settings, hooked through ElleKit)
$DYN -dynamiclib -install_name /var/jb/usr/lib/TweakInject/hakios-tweaktest.dylib -lobjc -o "$O/hakios-tweaktest.dylib" "$RN/../components/tweaks/tweak_test.c"
$DYN -dynamiclib -install_name /hakios/lib/libhakios-console.dylib -o "$O/libhakios-console.dylib" "$RN/userland/console_log.c"
$DYN -dynamiclib -install_name /hakios/lib/libhakios-poster.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-poster.dylib" "$RN/../components/wallpaper/inject_poster.c"
$DYN -dynamiclib -install_name /hakios/lib/libhakios-wallpaper.dylib -framework Foundation -framework QuartzCore -framework CoreGraphics -lobjc -o "$O/libhakios-wallpaper.dylib" "$RN/../components/wallpaper/inject_wallpaper.m"
$DYN -dynamiclib -install_name /hakios/lib/libhakios-backlight.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-backlight.dylib" "$RN/../components/brightness/inject_backlight.c"
# the flashlight and the camera in cameracaptured, iOS 27's camera daemon: the kernel's /dev/hakios-torch and
# /dev/hakios-camera (components/camera/src/inject_capture.c; the capture device and its picture processor are 16.5's,
# components/camera/src/camera_device.c and camera_isp.c, with the iPhone 11's formats: HAKIOS_IOS27)
$DYN -O2 -DHAKIOS_IOS27 -dynamiclib -install_name /hakios/lib/libhakios-capture.dylib -framework CoreFoundation \
	-framework CoreMedia -framework CoreVideo -framework IOSurface -lobjc -o "$O/libhakios-capture.dylib" \
	"$RN/../components/camera/src/inject_capture.c" "$RN/../components/camera/src/camera_device.c" "$RN/../components/camera/src/camera_isp.c"
# locationd's Mescal signer given up before it waits on fairplayd for good (components/location/inject_locationd.c)
$DYN -dynamiclib -install_name /hakios/lib/liblocationd-silo.dylib -lobjc -o "$O/liblocationd-silo.dylib" "$RN/../components/location/inject_locationd.c"
# the brightness slider and Auto-Brightness to the kernel's hw.backlight (any device: components/brightness/inject_brightness.c)
$DYN -dynamiclib -install_name /hakios/lib/libhakios-brightness.dylib -framework CoreFoundation -lobjc -o "$O/libhakios-brightness.dylib" "$RN/../components/brightness/inject_brightness.c"
for f in "$O"/*.dylib "$O/HakiosAudio"; do
	pac_strip "$f"
	"$LDID" -S "$f"
	echo "  $(basename "$f"): $(file -b "$f" | cut -c1-60)"
done
