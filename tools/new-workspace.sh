#!/bin/bash
# A second working copy of everything, for working on something else at the same time: its own
# branch of this repository (a git worktree), its own kernel tree and build, its own images and
# its own QEMU instance. What is big is shared with this copy through reflinks and hard links
# (btrfs or xfs), so a workspace costs about 0.5 GB, most of it the kernel's build output.
#
# usage: tools/new-workspace.sh NAME INSTANCE [DIR]
#   NAME      the branch, and the directory ../darwin-redwood-NAME unless DIR is given
#   INSTANCE  the QEMU instance number of the copy (1, 2, ...; see qemu/run-ios.sh)
#
# The kernel tree is copied as it is now, uncommitted work included, and that state is recorded
# (refs/redwood/baseline in the copy's kernel tree): tools/kernel-delta.sh, run in the workspace,
# prints what was changed in the kernel since. Files below ios165/rootfs, ios165/cryptex and
# ios165/cryptex-app are hard links to this copy's: replace them, never edit them in place.
# The phone is not part of a workspace: there is one, and ./xl belongs to the main copy.
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
NAME=${1:?usage: new-workspace.sh NAME INSTANCE [DIR]}
INSTANCE=${2:?usage: new-workspace.sh NAME INSTANCE [DIR]}
WS=${3:-$(dirname "$ROOT")/darwin-redwood-$NAME}
[ -e "$WS" ] && { echo "new-workspace.sh: $WS already exists"; exit 1; }

git -C "$ROOT" worktree add -q -b "$NAME" "$WS"
cd "$WS"

# the kernel tree, without build output (it is full of absolute paths)
mkdir -p xnu-src
cp -a --reflink=auto "$ROOT/xnu-src/xnu-8796.121.2" xnu-src/
K=xnu-src/xnu-8796.121.2
rm -rf $K/BUILD
(
	cd $K
	export GIT_INDEX_FILE=$PWD/.git/index.baseline
	git add -A . 2>/dev/null
	git update-ref refs/redwood/baseline "$(git commit-tree "$(git write-tree)" -p HEAD -m 'the state the workspace started from')"
	rm -f "$GIT_INDEX_FILE"
)
# what the kernel build needs besides the tree
cp -a --reflink=auto "$ROOT/xnu-deps/." xnu-deps/
# The files of iOS and the system image made of them. Every one of these is taken only if it is
# there: a main copy that has not built its images yet has no system.apfs nor root.apfs, and
# cryptex-app is unpacked only where somebody has needed it -- under "set -e" a copy of a file that
# is not there used to end the script halfway, leaving a workspace without its QEMU instance number
# and without a kernel. What a missing cryptex-app costs, when there is one to take: the images come
# out with no /System/Cryptexes/App at all, and Safari's daemons and AuthenticationServicesAgent --
# eight jobs -- then look to mkjobs.py like programs nobody has (build.sh at cryptex-app, present()).
for d in rootfs cryptex cryptex-app "20F66__iPhone10,3_6"; do
	if [ -e "$ROOT/ios165/$d" ]; then cp -al "$ROOT/ios165/$d" ios165/; fi
done
for f in system.files system.manifest var.skel .done-rootfs .done-cryptex .done-cryptex-app a2s.json; do
	if [ -e "$ROOT/ios165/$f" ]; then cp -a "$ROOT/ios165/$f" ios165/; fi
done
for f in rootfs.dmg rootfs.slt cryptex-app.dmg extracted; do
	if [ -e "$ROOT/ios165/$f" ]; then ln -s "$ROOT/ios165/$f" ios165/; fi
done
# The firmware the build puts into the images -- the radio's, the audio DSP's, the GPU's. A copy
# without them builds an image with no /fw and says nothing about it: gpu=1 then starts no GPU.
# Links, not copies: there is one set of these on this computer and they are not small.
for d in "$ROOT"/fw-*; do
	if [ -d "$d" ]; then ln -s "$d" ./; fi
done
for f in obj system.apfs root.apfs hello; do
	if [ -e "$ROOT/userland/$f" ]; then cp -a --reflink=auto "$ROOT/userland/$f" userland/; fi
done
[ -f "$ROOT/xnuloader/loader.efi" ] && cp -a "$ROOT/xnuloader/loader.efi" xnuloader/
[ -f "$ROOT/qemu/kernel.syms" ] && cp -a "$ROOT/qemu/kernel.syms" qemu/
echo "$INSTANCE" > qemu/instance
mkdir -p logs

echo "new-workspace.sh: building the kernel in $WS"
./xnu-build.sh -j"$(nproc)" -k > xnu-build.log 2>&1 || true
[ -f $K/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 ] ||
	{ echo "new-workspace.sh: the kernel did not build, see $WS/xnu-build.log"; exit 1; }
echo "new-workspace.sh: $WS is ready (branch $NAME, QEMU instance $INSTANCE)"
