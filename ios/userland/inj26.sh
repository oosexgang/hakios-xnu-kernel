#!/bin/bash
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}   # the directory holding this repository and darwin-renoir
DR=${DR:-$DK/darwin-renoir}
cd $DR/userland
source ../xnu-env.sh
# BUILD: the iOS build the libraries are for (23G90 = 26.6.2, 24B119 = 27.2); OBJ: where they go
BUILD=${BUILD:-23G90}
O=${OBJ:-$RN/userland/obj26}; mkdir -p $O $O/src
# what does not depend on the build (Mesa, our GL library, gl_test: mesa26e.sh, gl26.sh) comes from obj26
for f in libOSMesa.8.dylib libhakios-gl.dylib gl_test; do
	[ -e $O/$f ] || cp $RN/userland/obj26/$f $O/$f
done
cp src/*.c src/*.h $O/src/ 2>/dev/null || true
sed -i -e 's/context->uc_mcontext->__ss\.__pc/__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss)/' \
       -e 's/context->uc_mcontext->__ss\.__lr/__darwin_arm_thread_state64_get_lr(context->uc_mcontext->__ss)/' \
       -e 's/context->uc_mcontext->__ss\.__fp/__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss)/' $O/src/inject.c
# SpringBoard's methods that are replaced, at the build's addresses: only those whose code was read and
# found to do the same as on 16.5 (inject_sb.c checks each against the address it expects and leaves a
# method alone otherwise).
#   -[SBLockdownManager _setupActivationState]: the activation state an int at +8, 2 = activated
#   -[SBSetupManager _setSetupRequiredReason:]: the reason kept at +8, 0 = no setup assistant
case $BUILD in
23G90)	SB_ACTIVATION=0x22450fdc0 SB_SETUP=0x224675b68 ;;
24B119)	SB_ACTIVATION=0x2274356b8 SB_SETUP=0x22757d3fc ;;   # 27.2: the reason at +0x38 now; the hook passes 0 on
*)	echo "inj26.sh: no SpringBoard addresses for $BUILD: those hooks will be left alone" >&2
	SB_ACTIVATION='' SB_SETUP='' ;;
esac
[ -z "$SB_ACTIVATION" ] || sed -i -e "s/0x1c4bc8dfcUL/${SB_ACTIVATION}UL/" -e "s/0x1c4cdd3a4UL/${SB_SETUP}UL/" $O/src/inject_sb.c
DYN="xnu-clang -arch arm64e -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1 -mios-version-min=16.0"
$DYN -dynamiclib -install_name /redwood/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc -o $O/libhakios.dylib $O/src/inject.c
# the status bar's clock past the hole punch: ios/userland/inject_statusbar.c (16.5's statusbar.c checks 16.5's addresses)
# (what it knows of each board, hakios_boards.h, is made from the device packages that are there now)
"$RN/../tools/boards.py" gen --out "$O/boards" > /dev/null
$DYN -dynamiclib -install_name /redwood/lib/libhakios-sb.dylib -framework CoreFoundation -framework IOKit -lobjc -o $O/libhakios-sb.dylib $O/src/inject_sb.c -I$O/boards $RN/userland/inject_statusbar.c
$DYN -dynamiclib -install_name /redwood/lib/libhakios-statusbar.dylib -framework CoreFoundation -framework IOKit -lobjc -o $O/libhakios-statusbar.dylib -I$O/boards $RN/userland/inject_statusbar.c
# the audio server's output device (AudioServerPlugIn, arm64e: the audio server is): the speaker route and its
# volume, Control Center's slider included; the samples go to /dev/hakios-audio when the kernel has it
$DYN -bundle -framework CoreFoundation -o $O/HakiosAudio "$(dirname "$RN")/userland/userland/src/audio_plugin.c"
$DYN -dynamiclib -install_name /redwood/lib/libhakios-app.dylib -lobjc -o $O/libhakios-app.dylib $O/src/inject_app.c
$DYN -dynamiclib -install_name /redwood/lib/libhakios-console.dylib -o $O/libhakios-console.dylib $RN/userland/console_log.c
$DYN -dynamiclib -install_name /redwood/lib/libhakios-poster.dylib -framework CoreFoundation -lobjc -o $O/libhakios-poster.dylib $RN/userland/inject_poster.c
$DYN -dynamiclib -install_name /redwood/lib/libhakios-backlight.dylib -framework CoreFoundation -lobjc -o $O/libhakios-backlight.dylib $RN/userland/inject_backlight.c
# the brightness slider and Auto-Brightness to the kernel's hw.backlight (any device: ios/userland/inject_brightness.c)
$DYN -dynamiclib -install_name /redwood/lib/libhakios-brightness.dylib -framework CoreFoundation -lobjc -o $O/libhakios-brightness.dylib $RN/userland/inject_brightness.c
for f in $O/*.dylib $O/HakiosAudio; do
  $RN/tools/pacstrip macho $f $f.strip > /dev/null && mv $f.strip $f
  ${THEOS}/toolchain/linux/iphone/bin/ldid -S $f
  file -b $f | cut -c1-70
done
ls -la $O
