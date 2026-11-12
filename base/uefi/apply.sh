#!/bin/bash
# Make ../Mu-Silicium: a clone of Project-Silicium/Mu-Silicium at the commit the ports are made on, with
# what every port needs (this directory: the patch and files/) put into it, and a phone's own platform
# (the files/ directory of its device package's UEFI port) over that. See README.md here.
#   uefi/apply.sh PHONE_FILES
set -e
[ $# = 1 ] && [ -d "$1" ] || { echo "usage: uefi/apply.sh PHONE_FILES (a device package's UEFI files/)" >&2; exit 2; }
PHONE=$(cd "$1" && pwd)
cd "$(dirname "$(readlink -f "$0")")"
MU=../Mu-Silicium
[ -e $MU ] && { echo "uefi/apply.sh: $MU already exists"; exit 1; }
git clone https://github.com/Project-Silicium/Mu-Silicium.git $MU
git -C $MU checkout -q "$(cat BASE)"
git -C $MU submodule update --init --recursive
git -C $MU apply --whitespace=nowarn "$PWD/mu-silicium.patch"
cp -r files/. $MU/
cp -r "$PHONE"/. $MU/
echo "uefi/apply.sh: done; next: the phone's UEFI README"
