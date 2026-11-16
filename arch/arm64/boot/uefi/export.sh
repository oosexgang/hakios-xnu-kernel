#!/bin/bash
# Save the changes made in ../Mu-Silicium (a clone of Project-Silicium/Mu-Silicium with a phone's port in
# it): what every port needs into this directory -- mu-silicium.patch for the files upstream has, files/
# for the loader's hook -- and, with MAKER/PHONE and a directory, that phone's own platform there (its
# Platforms/MAKER/PHONEPkg, Resources/Configs/PHONE.toml, Resources/DTBs/PHONE.dtb, Binaries/PHONE), for
# its device package. apply.sh puts both into a fresh clone.
#   uefi/export.sh [MAKER/PHONE PHONE_FILES]
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../Mu-Silicium
git -C $MU rev-parse HEAD > BASE
git -C $MU diff -- BootShim Silicon > mu-silicium.patch
rm -rf files
mkdir -p files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib
cp $MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.[ch] files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/
if [ $# = 2 ]; then
	maker=${1%%/*} phone=${1##*/} out=$2
	mkdir -p "$out/Platforms/$maker" "$out/Resources/Configs" "$out/Resources/DTBs" "$out/Binaries"
	cp -r "$MU/Platforms/$maker/${phone}Pkg" "$out/Platforms/$maker/"
	cp "$MU/Resources/Configs/$phone.toml" "$out/Resources/Configs/"
	cp "$MU/Resources/DTBs/$phone.dtb" "$out/Resources/DTBs/"
	cp -r "$MU/Binaries/$phone" "$out/Binaries/"
	find "$out" -name __pycache__ -prune -exec rm -rf {} +
fi
echo "uefi/export.sh: base $(cut -c1-12 BASE), patch $(wc -l < mu-silicium.patch) lines${2:+, the platform of the phone in $2}"
