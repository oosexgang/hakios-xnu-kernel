#!/bin/bash
# Host tests of what the Renoir patches add that can run without a phone or a kernel build: each test
# takes the files it tests out of the patch they are in (so it tests what the kernel compiles) and is
# built with the host's cc.
#
#   renoir/tests/run.sh             all of them
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PATCHES=$HERE/../kernel/patches
CC=${CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# extract FILE from PATCH (a file the patch creates) into $WORK/src
extract() {
	mkdir -p "$WORK/src"
	(cd "$WORK/src" && git apply --include="$2" "$PATCHES/$1")
}

failed=0
run() {
	local name=$1; shift
	echo "== $name"
	if "$CC" -std=c11 -Wall -Wextra -Werror -O2 "$@" -o "$WORK/$name" && "$WORK/$name"; then
		:
	else
		failed=1
	fi
}

extract 0023-universal-backlight.patch bsd/dev/hakios_backlight_core.h
extract 0023-universal-backlight.patch bsd/dev/hakios_backlight.c
run backlight_test -I"$WORK/src/bsd/dev" "$HERE/backlight_test.c"

# the driver itself against a model of the hardware (backlight_driver_test.c), one scenario a process; on the
# host, then built for aarch64 and run under qemu-aarch64 when both are there (apt: gcc-aarch64-linux-gnu qemu-user)
DRIVER_FLAGS=(-std=gnu11 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -O2 -pthread
	-I"$HERE/fakekernel" -I"$WORK/src/bsd/dev" "$HERE/backlight_driver_test.c")
scenarios() {
	local runner=$1 binary=$2 scenario
	for scenario in $($runner "$binary"); do
		timeout 120 $runner "$binary" "$scenario" || failed=1
	done
}
echo "== backlight_driver_test (host)"
if "$CC" "${DRIVER_FLAGS[@]}" -o "$WORK/backlight_driver_test"; then
	scenarios "" "$WORK/backlight_driver_test"
else
	failed=1
fi
if command -v aarch64-linux-gnu-gcc > /dev/null && command -v qemu-aarch64 > /dev/null; then
	echo "== backlight_driver_test (aarch64, qemu-aarch64)"
	if aarch64-linux-gnu-gcc -static "${DRIVER_FLAGS[@]}" -o "$WORK/backlight_driver_test.a64"; then
		scenarios qemu-aarch64 "$WORK/backlight_driver_test.a64"
	else
		failed=1
	fi
else
	echo "== (no aarch64-linux-gnu-gcc or qemu-aarch64: the aarch64 run is left out)"
fi

exit $failed
