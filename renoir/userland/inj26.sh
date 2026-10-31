#!/bin/bash
set -e
DR=/home/wannq/darwin_kernel/darwin-renoir
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd $DR/userland
source ../xnu-env.sh
O=$RN/userland/obj26; mkdir -p $O $O/src
cp src/*.c src/*.h $O/src/ 2>/dev/null || true
sed -i -e 's/context->uc_mcontext->__ss\.__pc/__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss)/' \
       -e 's/context->uc_mcontext->__ss\.__lr/__darwin_arm_thread_state64_get_lr(context->uc_mcontext->__ss)/' \
       -e 's/context->uc_mcontext->__ss\.__fp/__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss)/' $O/src/inject.c
# SpringBoard's methods that are replaced, at their iOS 26.6.2 (23G90) addresses: only those whose code
# was read and found to do the same as on 16.5 (inject_sb.c checks each against the address it expects
# and leaves a method alone otherwise).
#   -[SBLockdownManager _setupActivationState]: the activation state still an int at +8, 2 = activated
#   -[SBSetupManager _setSetupRequiredReason:]: the reason still kept at +8, 0 = no setup assistant
sed -i -e 's/0x1c4bc8dfcUL/0x22450fdc0UL/' -e 's/0x1c4cdd3a4UL/0x224675b68UL/' $O/src/inject_sb.c
DYN="xnu-clang -arch arm64e -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1 -mios-version-min=16.0"
$DYN -dynamiclib -install_name /redwood/lib/libredwood.dylib -framework CoreFoundation -framework IOKit -lobjc -o $O/libredwood.dylib $O/src/inject.c
$DYN -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib -framework CoreFoundation -framework IOKit -lobjc -o $O/libredwood-sb.dylib $O/src/inject_sb.c $O/src/statusbar.c
$DYN -dynamiclib -install_name /redwood/lib/libredwood-statusbar.dylib -framework CoreFoundation -framework IOKit -lobjc -o $O/libredwood-statusbar.dylib $O/src/statusbar.c
$DYN -dynamiclib -install_name /redwood/lib/libredwood-app.dylib -lobjc -o $O/libredwood-app.dylib $O/src/inject_app.c
for f in $O/*.dylib; do
  $RN/tools/pacstrip macho $f $f.strip > /dev/null && mv $f.strip $f
  ${THEOS}/toolchain/linux/iphone/bin/ldid -S $f
  file -b $f | cut -c1-70
done
ls -la $O
