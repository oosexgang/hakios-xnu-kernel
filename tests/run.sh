#!/bin/bash
# Host tests of kernel code that can run without a phone or a kernel build: each test includes the
# kernel's own source files (kernel/overlay), so it tests what the kernel compiles, built with the host's cc.
#
#   tests/run.sh             all of them
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=$HERE/../kernel/overlay/bsd/dev          # the drivers under test, as the kernel compiles them
CC=${CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

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

echo "== boards (devices/, soc/, tools/boards.py)"
python3 "$HERE/boards_test.py" 2>&1 | tail -3 || failed=1
"$HERE/../tools/boards.py" check || failed=1
"$HERE/../tools/boards.py" gen --check || failed=1

echo "== kernel_tools_test"
"$HERE/kernel_tools_test.sh" || failed=1

run backlight_test -I"$SRC" "$HERE/backlight_test.c"
run thermal_test -I"$SRC" "$HERE/thermal_test.c"

# the drivers themselves against models of the hardware, one scenario a process; on the host, then built for
# aarch64 and run under qemu-aarch64 when both are there (apt: gcc-aarch64-linux-gnu qemu-user)
DRIVER_FLAGS=(-std=gnu11 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -O2 -pthread
	-I"$HERE/fakekernel" -I"$SRC" "$HERE/fakekernel/fakekernel.c")
scenarios() {
	local runner=$1 binary=$2 scenario
	for scenario in $($runner "$binary"); do
		timeout 120 $runner "$binary" "$scenario" || failed=1
	done
}
for test in board_test backlight_driver_test thermal_driver_test; do
	echo "== $test (host)"
	if "$CC" "${DRIVER_FLAGS[@]}" "$HERE/$test.c" -o "$WORK/$test"; then
		scenarios "" "$WORK/$test"
	else
		failed=1
	fi
	if command -v aarch64-linux-gnu-gcc > /dev/null && command -v qemu-aarch64 > /dev/null; then
		echo "== $test (aarch64, qemu-aarch64)"
		if aarch64-linux-gnu-gcc -static "${DRIVER_FLAGS[@]}" "$HERE/$test.c" -o "$WORK/$test.a64"; then
			scenarios qemu-aarch64 "$WORK/$test.a64"
		else
			failed=1
		fi
	else
		echo "== (no aarch64-linux-gnu-gcc or qemu-aarch64: the aarch64 run is left out)"
	fi
done

exit $failed
