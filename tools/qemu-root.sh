#!/bin/bash
# A root for the kernel in QEMU, made of nothing of Apple's: an APFS image (tools/apfs/mkapfs.py)
# with /dev, where the kernel mounts devfs, /etc/motd, and /sbin/launchd, tools/qemu-init/ built as a static arm64
# program with clang and ld64.lld (kernel/host/llvm/place-kernel.py --program makes it one a kernel runs
# without dyld).
#
#   tools/qemu-root.sh OUT.img
#   tools/qemu-kernel.sh --ramdisk OUT.img --boot-args rd=md0 KERNEL
#
# XNU_CLANG_DIR: the directory of clang and ld64.lld (default: where clang is found).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=$(realpath -m "${1:?usage: tools/qemu-root.sh OUT.img}")
L=${XNU_CLANG_DIR:-$(dirname "$(command -v clang)")}
[ -x "$L/ld64.lld" ] || { echo "qemu-root: no ld64.lld beside clang in $L (XNU_CLANG_DIR=DIR)" >&2; exit 1; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
# (the platform as the kernel's link gives it: 2, the phones')
objects=()
for c in "$HERE"/tools/qemu-init/*.c; do
	IPHONEOS_DEPLOYMENT_TARGET=16.5 "$L/clang" -target arm64-apple-darwin -O2 -ffreestanding -fno-builtin \
		-fno-stack-protector -Wall -Wextra -c "$c" -o "$W/$(basename "$c" .c).o"
	objects+=("$W/$(basename "$c" .c).o")
done
"$L/ld64.lld" -arch arm64 -platform_version 2 16.5 16.5 -e _start -no_adhoc_codesign -no_fixup_chains \
	-o "$W/init.lld" "${objects[@]}"
mkdir -p "$W/root/sbin" "$W/root/dev" "$W/root/etc" "$W/root/mnt" "$W/root/tmp"
printf '%s\n' "This root was made by tools/qemu-root.sh: an APFS image of nothing of Apple's, with" \
	"/sbin/launchd, which is tools/qemu-init/, and /dev, where the kernel mounts devfs." > "$W/root/etc/motd"
python3 "$HERE/kernel/host/llvm/place-kernel.py" --program "$W/init.lld" "$W/root/sbin/launchd" > /dev/null
chmod 755 "$W/root/sbin/launchd"
python3 "$HERE/tools/apfs/mkapfs.py" -o "$out" Root="$W/root" | tail -1
