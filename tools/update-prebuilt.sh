#!/bin/bash
# Refresh prebuilt/ from what is built in this tree: the kernel, the loader, our user-space programs
# and the pieces of the phone's UEFI. With them somebody without the cross toolchain can still make the
# images and run the system (QEMU or the phone); see README.md.
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
# KERNEL=... to take the kernel from a tree built elsewhere (xnu/apply.sh makes one anywhere)
K=${KERNEL:-xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}
MU=Mu-Silicium
mkdir -p prebuilt/userland prebuilt/uefi
cp $K prebuilt/mach.development.bcm2837
cp xnuloader/loader.efi prebuilt/loader.efi
cp userland/hello prebuilt/userland/
for f in init child stage0 hello_dyn svc_test surf_test rwtouch mg_test cpu_test sound_test libredwood.dylib libredwood-sb.dylib \
	libredwood-media.dylib libredwood-app.dylib libredwood-tweaks.dylib RedwoodAudio net_test; do
	cp userland/obj/$f prebuilt/userland/
done
if [ -f $MU/Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd ]; then
	cp $MU/Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd $MU/BootShim/BootShim.bin $MU/Resources/DTBs/redwood.dtb \
		$MU/Resources/ramdisk $MU/Resources/Scripts/mkbootimg.py prebuilt/uefi/
fi
{
	echo "Built $(date '+%Y-%m-%d %H:%M') from this tree."
	echo
	sha256sum prebuilt/mach.development.bcm2837 prebuilt/loader.efi prebuilt/userland/* prebuilt/uefi/* 2>/dev/null
} > prebuilt/SHA256SUMS
ls -la prebuilt prebuilt/userland prebuilt/uefi
