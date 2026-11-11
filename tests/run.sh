#!/bin/bash
# Host tests: of the tools, and of kernel code that can run without a phone or a kernel build. Each of the latter includes the
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

echo "== the repository's own checks (tools/check-links.py, tools/components.py)"
python3 "$HERE/tools_test.py" 2>&1 | tail -3 || failed=1

echo "== boards (devices/, soc/, tools/boards.py)"
python3 "$HERE/boards_test.py" 2>&1 | tail -3 || failed=1
"$HERE/../tools/boards.py" check || failed=1
echo "== device packages (tools/device.py)"
python3 "$HERE/device_test.py" 2>&1 | tail -3 || failed=1
# The tables the code under test is built with: the repository's own boards and the tests' phones
# (tests/devices, with a chip of their own in tests/soc), which stand in for real ones -- device
# packages are not part of this repository.
BOARDS=$WORK/boards
"$HERE/../tools/boards.py" --devices "$HERE/devices" --soc "$HERE/soc" check || failed=1
"$HERE/../tools/boards.py" --devices "$HERE/devices" --soc "$HERE/soc" gen --out "$BOARDS" || failed=1

# the loader with the generated platforms: it compiles without a warning and links (clang and lld, as
# loader/build.sh; left out where they are not installed)
if command -v clang > /dev/null && command -v lld-link > /dev/null; then
	echo "== loader (loader/loader.c with the generated platforms.h)"
	if clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
		-mgeneral-regs-only -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I"$BOARDS" -c "$HERE/../loader/loader.c" -o "$WORK/loader.obj" &&
	    lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro \
		"/out:$WORK/loader.efi" "$WORK/loader.obj"; then
		echo "loader.efi: $(stat -c %s "$WORK/loader.efi") bytes, ok"
	else
		failed=1
	fi
else
	echo "== (no clang or lld-link: the loader is not built)"
fi

echo "== boot-args reference (tools/bootargs.py)"
python3 "$HERE/bootargs_test.py" 2>&1 | tail -3 || failed=1

echo "== porting_test (docs/porting.md on a made-up phone)"
"$HERE/porting_test.sh" || failed=1

echo "== kernel_tools_test"
"$HERE/kernel_tools_test.sh" || failed=1

run backlight_test -I"$SRC" "$HERE/backlight_test.c"
run thermal_test -I"$SRC" "$HERE/thermal_test.c"

# the drivers themselves against models of the hardware, one scenario a process; on the host, then built for
# aarch64 and run under qemu-aarch64 when both are there (apt: gcc-aarch64-linux-gnu qemu-user)
DRIVER_FLAGS=(-std=gnu11 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -O2 -pthread
	-I"$HERE" -I"$HERE/fakekernel" -I"$SRC" -I"$SRC/.." -I"$BOARDS" "$HERE/fakekernel/fakekernel.c")
# (the last is the example driver of docs/writing-a-driver.md, which lives beside the guide)
EXAMPLES=$HERE/../docs/examples
scenarios() {
	local runner=$1 binary=$2 scenario
	for scenario in $($runner "$binary"); do
		timeout 120 $runner "$binary" "$scenario" || failed=1
	done
}
for test in board_test spmi_test cmd_db_test rpmh_test tlmm_test smem_test backlight_driver_test thermal_driver_test cpufreq_driver_test ddr_driver_test buttons_driver_test torch_driver_test battery_driver_test example_led_test; do
	source=$HERE/$test.c
	[ -f "$source" ] || source=$EXAMPLES/$test.c
	echo "== $test (host)"
	if "$CC" "${DRIVER_FLAGS[@]}" "$source" -o "$WORK/$test"; then
		scenarios "" "$WORK/$test"
	else
		failed=1
	fi
	if command -v aarch64-linux-gnu-gcc > /dev/null && command -v qemu-aarch64 > /dev/null; then
		echo "== $test (aarch64, qemu-aarch64)"
		if aarch64-linux-gnu-gcc -static "${DRIVER_FLAGS[@]}" "$source" -o "$WORK/$test.a64"; then
			scenarios qemu-aarch64 "$WORK/$test.a64"
		else
			failed=1
		fi
	else
		echo "== (no aarch64-linux-gnu-gcc or qemu-aarch64: the aarch64 run is left out)"
	fi
done

exit $failed
