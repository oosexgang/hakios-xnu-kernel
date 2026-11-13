#!/bin/bash
# Host tests: of the tools, and of kernel code that can run without a phone or a kernel build. Each of the latter includes the
# kernel's own source files (kernel/overlay), so it tests what the kernel compiles, built with the host's cc.
#
#   workbench/run.sh             all of them
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

echo "== boards (devices/, tools/boards.py)"
python3 "$HERE/boards_test.py" 2>&1 | tail -3 || failed=1
"$HERE/../tools/boards.py" check || failed=1
echo "== device packages (tools/device.py)"
python3 "$HERE/device_test.py" 2>&1 | tail -3 || failed=1
# The tables the code under test is built with: the repository's own boards and the tests' phones
# (workbench/devices), which stand in for real ones -- device
# packages are not part of this repository.
BOARDS=$WORK/boards
"$HERE/../tools/boards.py" --devices "$HERE/devices" check || failed=1
"$HERE/../tools/boards.py" --devices "$HERE/devices" gen --out "$BOARDS" || failed=1
# (one touch driver includes a controller's firmware, which is not ours to publish: the stub the kernel's build has without it)
(source "$HERE/../kernel/lib.sh" && focaltech_stub > "$BOARDS/focaltech_fw.h") || failed=1

# the loader with the generated platforms: it compiles without a warning and links (clang and lld, as
# loader/build.sh; left out where they are not installed)
if command -v clang > /dev/null && command -v lld-link > /dev/null; then
	echo "== loader (loader/loader.c and its parts in loader/src, with the generated platforms.h)"
	if clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
		-mgeneral-regs-only -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I"$BOARDS" -c "$HERE/../loader/loader.c" -o "$WORK/loader.obj" &&
	    lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro \
		"/out:$WORK/loader.efi" "$WORK/loader.obj"; then
		echo "loader.efi: $(stat -c %s "$WORK/loader.efi") bytes, ok"
		echo "== the loader booted by a UEFI in QEMU (workbench/loader_boot_test.sh)"
		"$HERE/loader_boot_test.sh" "$WORK/loader.efi" || failed=1
	else
		failed=1
	fi
else
	echo "== (no clang or lld-link: the loader is not built)"
fi

echo "== a serial engine's clocks out of Linux's clock driver (tools/qupclocks.py)"
python3 "$HERE/qupclocks_test.py" 2>&1 | tail -3 || failed=1

echo "== a phone's identity (tools/identity.py)"
python3 "$HERE/identity_test.py" 2>&1 | tail -3 || failed=1

echo "== the Darwin image tool's helpers (darwin/tools/mkimage26.py)"
python3 "$HERE/darwin_tools_test.py" 2>&1 | tail -3 || failed=1

echo "== boot-args reference (tools/bootargs.py)"
python3 "$HERE/bootargs_test.py" 2>&1 | tail -3 || failed=1

echo "== porting_test (docs/porting.md on a made-up phone)"
"$HERE/porting_test.sh" || failed=1

echo "== kernel_tools_test"
"$HERE/kernel_tools_test.sh" || failed=1
echo "== place_kernel_test (kernel/host/llvm/place-kernel.py: the kernel ld64.lld links, laid out to boot)"
python3 "$HERE/place_kernel_test.py" 2>&1 | tail -3 || failed=1

echo "== APFS images (userland/tools/apfs/mkapfs.py) read back by the kernel's reader, and the drivers' firmware lookup in them"
"$HERE/apfs_image_test.sh" || failed=1

echo "== qmi (kernel/overlay/bsd/dev/board/qmi.c under the sanitizers)"
"$HERE/qmi/run.sh" 2>&1 | grep -v "said once" || failed=1

run backlight_test -I"$SRC" "$HERE/backlight_test.c"
run thermal_test -I"$SRC" "$HERE/thermal_test.c"
run synaptics_report_test -I"$SRC" "$HERE/synaptics_report_test.c"

# the loader's parts that are plain C, and the drivers themselves against models of the hardware, one scenario a process; on the host, then built for
# aarch64 and run under qemu-aarch64 when both are there (apt: gcc-aarch64-linux-gnu qemu-user)
DRIVER_FLAGS=(-std=gnu11 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -O2 -pthread
	-I"$HERE" -I"$HERE/fakekernel" -I"$SRC" -I"$SRC/.." -I"$BOARDS" -I"$HERE/../loader" "$HERE/fakekernel/fakekernel.c")
# (the last is the example driver of docs/writing-a-driver.md, which lives beside the guide)
EXAMPLES=$HERE/../docs/examples
# On the host, under the address and undefined-behaviour sanitizers where the compiler has them: a read or
# a write past a buffer, or a load of a 64-bit number from an address that is not a multiple of 8, fails
# the scenario (what the drivers leak on purpose -- a run's memory is never given back -- is not looked for)
SANITIZE=()
if echo 'int main(void) { return 0; }' | "$CC" -fsanitize=address,undefined -x c - -o "$WORK/sanitizers" 2> /dev/null; then
	SANITIZE=(-O1 '-fsanitize=address,undefined' -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
	export ASAN_OPTIONS=detect_leaks=0
fi
scenarios() {
	local runner=$1 binary=$2 scenario list
	list=$($runner "$binary")
	# a test that names no scenario runs none, and would pass for that
	if [ -z "$list" ]; then
		echo "FAIL: $(basename "$binary") lists no scenario"
		failed=1
	fi
	for scenario in $list; do
		timeout 120 $runner "$binary" "$scenario" || failed=1
	done
}
for test in bt_uart_test loader_test loader_window_test loader_download_test loader_dsu_test loader_own_partition_test board_test drivers_list_test touch_goodix_wiring_test touch_fts_wiring_test spmi_test cmd_db_test rpmh_test tlmm_test i2c_gpio_test smem_test glink_queue_test qrtr_frame_test mdt_test ipc_test backlight_driver_test thermal_driver_test cpufreq_driver_test ddr_driver_test buttons_driver_test torch_driver_test battery_driver_test haptic_driver_test speaker_driver_test scmi_perf_test audio_device_test station_test apple80211_test camera_wiring_test sound_pins_test mic_wiring_test sparse_driver_test ufs_driver_test ufs_whitelist_test input_queue_test guard_test example_led_test; do
	source=$HERE/$test.c
	[ -f "$source" ] || source=$EXAMPLES/$test.c
	echo "== $test (host${SANITIZE[0]:+, sanitizers})"
	if "$CC" "${DRIVER_FLAGS[@]}" "${SANITIZE[@]}" "$source" -o "$WORK/$test"; then
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

# the helper that breaks a driver on purpose (workbench/mutate.sh), on the example driver: a change that
# matters is caught, one that changes nothing is not, one that does not compile is said to be that
echo "== mutate.sh (on docs/examples/example_led.c)"
mutation() {    # mutation WANTED-EXIT OLD NEW
	local wanted=$1 got=0
	"$HERE/mutate.sh" example_led_test example_led.c "$2" "$3" > "$WORK/mutation" 2>&1 || got=$?
	if [ "$got" -ne "$wanted" ]; then
		echo "FAIL: replacing \"$2\" by \"$3\": exit $got, wanted $wanted"
		sed 's/^/    /' "$WORK/mutation"
		failed=1
	fi
}
mutation 0 'if (((found_cfg >> 2) & 0xf) != 0) {' 'if (0) {'
mutation 1 'static bool taken;' 'static bool taken; '
mutation 2 'static bool taken;' 'static bool taken'
mutation 2 'text that is not there' ''
# ... and a file the test never sees cannot be broken through it
if "$HERE/mutate.sh" example_led_test board/ddr.c 'uint32_t level = 2;' 'uint32_t level = 3;' > "$WORK/mutation" 2>&1 ||
	! grep -q "does not include board/ddr.c" "$WORK/mutation"; then
	echo "FAIL: a file the test does not include was taken for one it does"
	failed=1
fi
[ "$failed" -ne 0 ] || echo "mutate.sh: 5 checks, ok"

exit $failed
