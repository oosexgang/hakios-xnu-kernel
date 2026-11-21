#!/bin/bash
# Fetch pinned U-Boot, apply the Redwood EFI USB support patch, and overlay this port's files.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../../.." && pwd)
UB=${U_BOOT:-$TOP/build/u-boot-redwood-src}
TAG=$(cat "$HERE/BASE")
PIN=$(cat "$HERE/BASE_COMMIT")
URL=${U_BOOT_URL:-https://source.denx.de/u-boot/u-boot.git}

if [ ! -d "$UB/.git" ]; then
	if [ -e "$UB" ]; then
		echo "apply.sh: $UB exists but is not a Git checkout" >&2
		exit 1
	fi
	mkdir -p "$(dirname "$UB")"
	git clone --quiet --depth 1 --branch "$TAG" "$URL" "$UB"
fi

actual=$(git -C "$UB" rev-parse HEAD)
if [ "$actual" != "$PIN" ]; then
	echo "apply.sh: expected U-Boot $TAG at $PIN, found $actual in $UB" >&2
	exit 1
fi

if git -C "$UB" apply --check "$HERE/patches/hakios.patch" 2>/dev/null; then
	git -C "$UB" apply --whitespace=nowarn "$HERE/patches/hakios.patch"
elif ! grep -q 'config CMD_HAKIOS' "$UB/cmd/Kconfig" || ! grep -q 'EFI_QCOM_USB_DEVICE' "$UB/lib/efi_loader/Kconfig"; then
	echo "apply.sh: patch does not apply cleanly to the pinned source" >&2
	exit 1
fi

cp -R "$HERE/files/." "$UB/"
echo "apply.sh: U-Boot $TAG prepared at $UB"
