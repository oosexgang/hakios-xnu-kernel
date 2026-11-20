#!/bin/bash
# Host tests: of the tools, and of kernel code that can run without a phone or a kernel build. Each of the latter includes the
# kernel's own source files (kernel/overlay), so it tests what the kernel compiles, built with the host's cc.
#
#   workbench/run.sh             all of them
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CC=${CC:-cc}
WORK=$(mktemp -d)
# the kernel's files of ours at their paths in its tree (drivers/board as bsd/dev/board, include/board as
# bsd/sys/board, ...: tools/kernel-view.sh), and the drivers under test there, as the kernel compiles them
"$HERE/../tools/kernel-view.sh" "$WORK/kernel"
SRC=$WORK/kernel/bsd/dev
# the iPhone identity with nothing of Apple's in it, for everything here that builds the loader or its parts:
# identity.h is made from an IPSW, and one made on this machine must not decide what the tests see
mkdir -p "$WORK/identity"
python3 "$HERE/../arch/arm64/boot/mkidentity.py" --placeholder > "$WORK/identity/identity.h"
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
python3 "$HERE/maintainers_test.py" 2>&1 | tail -3 || failed=1
python3 "$HERE/logcheck_test.py" 2>&1 | tail -3 || failed=1
python3 "$HERE/phone_day_test.py" 2>&1 | tail -3 || failed=1
python3 "$HERE/phone_ready_test.py" 2>&1 | tail -3 || failed=1
python3 "$HERE/release_test.py" 2>&1 | tail -3 || failed=1
python3 "$HERE/kconfig_test.py" 2>&1 | tail -3 || failed=1
python3 "$HERE/bench_test.py" 2>&1 | tail -3 || failed=1

echo "== boards (arch/arm64/boot/dts/, tools/boards.py)"
python3 "$HERE/boards_test.py" 2>&1 | tail -3 || failed=1
"$HERE/../tools/boards.py" check || failed=1
echo "== device packages (tools/device.py)"
python3 "$HERE/device_test.py" 2>&1 | tail -3 || failed=1
echo "== the boot image a phone is started with (arch/arm64/boot/mkboot.py)"
python3 "$HERE/mkboot_test.py" 2>&1 | tail -3 || failed=1
python3 "$HERE/image_test.py" 2>&1 | tail -3 || failed=1
# The tables the code under test is built with: the repository's own boards and the tests' phones
# (workbench/devices), which stand in for real ones -- device
# packages are not part of this repository.
BOARDS=$WORK/boards
"$HERE/../tools/boards.py" --devices "$HERE/devices" check || failed=1
"$HERE/../tools/gen_headers.py" --devices "$HERE/devices" --out "$BOARDS" hakios || failed=1
# (one touch driver includes a controller's firmware, which is not ours to publish: the stub the kernel's build has without it)
(source "$HERE/../kernel/lib.sh" && focaltech_stub > "$BOARDS/focaltech_fw.h") || failed=1

# the loader with the generated platforms: it compiles without a warning and links (clang and lld, as
# arch/arm64/boot/build.sh; left out where they are not installed)
if command -v clang > /dev/null && command -v lld-link > /dev/null; then
	echo "== loader (arch/arm64/boot/loader.c and its parts in arch/arm64/boot/src, with the generated platforms.h)"
	# (in a directory of its own, with the identity that has nothing of Apple's: identity.h is made from an IPSW,
	# and one made here must not decide whether this compiles)
	mkdir -p "$WORK/loader"
	cp -r "$HERE/../arch/arm64/boot/loader.c" "$HERE/../arch/arm64/boot/efi.h" "$HERE/../arch/arm64/boot/src" "$WORK/loader/"
	cp "$WORK/identity/identity.h" "$WORK/loader/"
	if clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
		-mgeneral-regs-only -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I"$BOARDS" -c "$WORK/loader/loader.c" -o "$WORK/loader.obj" &&
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

echo "== the packages put into the prefix of tweaks (components/tweaks/tools/jb-preinstall.py)"
python3 "$HERE/jb_preinstall_test.py" 2>&1 | tail -3 || failed=1

echo "== what the QEMU boot test judges with (workbench/tone_in_wav.py, workbench/screen_shows_draw.py)"
python3 "$HERE/qemu_helpers_test.py" 2>&1 | tail -3 || failed=1

echo "== boot-args reference (tools/bootargs.py)"
python3 "$HERE/bootargs_test.py" 2>&1 | tail -3 || failed=1

echo "== porting_test (Documentation/porting.md on a made-up phone)"
"$HERE/porting_test.sh" || failed=1

echo "== kernel_tools_test"
"$HERE/kernel_tools_test.sh" || failed=1
echo "== place_kernel_test (kernel/host/llvm/place-kernel.py: the kernel ld64.lld links, laid out to boot)"
python3 "$HERE/place_kernel_test.py" 2>&1 | tail -3 || failed=1

echo "== APFS images (tools/apfs/mkapfs.py) read back by the kernel's reader, and the drivers' firmware lookup in them"
"$HERE/apfs_image_test.sh" || failed=1

echo "== qmi (drivers/board/qmi.c under the sanitizers)"
"$HERE/qmi/run.sh" 2>&1 | grep -v "said once" || failed=1

run backlight_test -I"$SRC" "$HERE/backlight_test.c"
run thermal_test -I"$SRC" "$HERE/thermal_test.c"
run synaptics_report_test -I"$SRC" "$HERE/synaptics_report_test.c"
# the data volume's store (init/var_store.c), against libmini's header as on the phone, over a sparse file for the partition
run var_store_test -I"$HERE/../lib/libmini" "$HERE/var_store_test.c"
# process 1 (init/init.c), the same way, its files under a directory of the test's and its children only noted
run init_test -I"$HERE/../lib/libmini" "$HERE/init_test.c"

# the loader's parts that are plain C, and the drivers themselves against models of the hardware, one scenario a process; on the host, then built for
# aarch64 and run under qemu-aarch64 when both are there (apt: gcc-aarch64-linux-gnu qemu-user)
DRIVER_FLAGS=(-std=gnu11 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -O2 -pthread
	-I"$HERE" -I"$HERE/fakekernel" -I"$SRC" -I"$SRC/.." -I"$BOARDS" -I"$WORK/identity" -I"$HERE/../arch/arm64/boot" "$HERE/fakekernel/fakekernel.c")
# (the last is the example driver of Documentation/writing-a-driver.md, which lives beside the guide)
EXAMPLES=$HERE/../Documentation/examples
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
for test in bt_uart_test bt_hci_test bt_qca_test bt_queue_test bt_host_test loader_test loader_window_test loader_download_test loader_dsu_test loader_log_partition_test loader_own_partition_test board_test drivers_list_test touch_goodix_wiring_test touch_fts_wiring_test touch_fts_read_test touch_goodix_poll_test touch_reports_test touch_fts_reports_test touch_fts_thread_test touch_ft5x06_test geni_spi_test spmi_test cmd_db_test rpmh_test tlmm_test i2c_gpio_test smem_test glink_queue_test qrtr_frame_test mdt_test ipc_test backlight_driver_test thermal_driver_test cpufreq_driver_test ddr_driver_test buttons_driver_test rtc_driver_test torch_driver_test battery_driver_test plug_watch_test haptic_driver_test imu_driver_test adreno_driver_test cell_driver_test mpss_driver_test nas_test speaker_driver_test scmi_perf_test audio_device_test station_test apple80211_test lx_test camera_wiring_test camera_driver_test sound_driver_test wifi_driver_test mic_driver_test adsp_driver_test sparse_driver_test ufs_driver_test ufs_whitelist_test ufs_native_read_test input_queue_test guard_test gic_v3_irq_test psci_smp_test a64emu_test user_exception_trace_test memory_floor_test sandbox_blastdoor_test drivers_kconfig_test orientation_test sensor_axes_test sensors_test lsm6dso_test bmi160_test example_led_test; do
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

# plain C without the fake kernel, a scenario a process, on the host under the sanitizers and under qemu-aarch64 as the
# drivers' tests are, with the maths library: the cameras' picture processor of Darwin's capture device
# (components/camera/src/camera_isp.c) as libhakios-media builds it, and the WPA2 handshake of the Wi-Fi driver's object
# (kernel/lx/src/lx_wpa.c), which needs nothing of the kernel's outside it
for test in camera_isp_test lx_wpa_test; do
	echo "== $test (host${SANITIZE[0]:+, sanitizers})"
	if "$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -pthread "${SANITIZE[@]}" "$HERE/$test.c" -o "$WORK/$test" -lm; then
		scenarios "" "$WORK/$test"
	else
		failed=1
	fi
	if command -v aarch64-linux-gnu-gcc > /dev/null && command -v qemu-aarch64 > /dev/null; then
		echo "== $test (aarch64, qemu-aarch64)"
		if aarch64-linux-gnu-gcc -static -std=gnu11 -Wall -Wextra -Werror -O2 -pthread "$HERE/$test.c" -o "$WORK/$test.a64" -lm; then
			scenarios qemu-aarch64 "$WORK/$test.a64"
		else
			failed=1
		fi
	fi
done

# the layer under the Wi-Fi driver (kernel/lx/src/lx_base.c): its work, timers, waits and memory on the host's threads, under
# the sanitizers. The headers of FreeBSD's and LinuxKPI's it includes are empty files of the same names here, made from the
# file's own #include lines (the test has the little of them the file uses)
LXKPI_STUBS=$WORK/lxkpi-headers
for header in $(sed -n 's/^#include <\(.*\)>$/\1/p' "$HERE/../kernel/lx/src/lx_base.c") linux/types.h; do
	mkdir -p "$LXKPI_STUBS/$(dirname "$header")"
	: > "$LXKPI_STUBS/$header"
done
echo "== lx_base_test (host${SANITIZE[0]:+, sanitizers})"
if "$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -pthread "${SANITIZE[@]}" -I"$LXKPI_STUBS" "$HERE/lx_base_test.c" -o "$WORK/lx_base_test"; then
	scenarios "" "$WORK/lx_base_test"
else
	failed=1
fi

# Darwin's side of the sound, the files themselves included in a test that has what they need of Darwin's frameworks
# (the audio server's plug-in, components/audio/audio_plugin.c, and what the phone's recording test,
# components/audio/record_test.c, makes of its file), and the brightness slider's library injected into SpringBoard,
# backboardd and Settings (components/brightness/inject_brightness.c), on the host under the sanitizers: Darwin's headers they
# include are empty files of the same names here, and the HAL's four-character constants are no warning
DARWIN_STUBS=$WORK/darwin-headers
for header in CoreFoundation/CoreFoundation.h CoreFoundation/CFPlugInCOM.h CoreAudioTypes/CoreAudioTypes.h mach/mach_time.h \
	sys/ioccom.h objc/message.h objc/runtime.h dispatch/dispatch.h sys/sysctl.h CoreMedia/CoreMedia.h CoreVideo/CoreVideo.h \
	IOSurface/IOSurfaceRef.h; do
	mkdir -p "$DARWIN_STUBS/$(dirname "$header")"
	: > "$DARWIN_STUBS/$header"
done
for test in audio_plugin_test record_analysis_test brightness_slider_test; do
	echo "== $test (host${SANITIZE[0]:+, sanitizers})"
	if "$CC" -std=gnu11 -Wall -Wextra -Werror -Wno-multichar -O2 -pthread "${SANITIZE[@]}" -I"$DARWIN_STUBS" "$HERE/$test.c" \
		-o "$WORK/$test" -lm; then
		scenarios "" "$WORK/$test"
	else
		failed=1
	fi
done

# Darwin's capture device in cameracaptured (components/camera/src/camera_device.c, with the picture processor it is built
# with), the file itself included in a test that has what it needs of CoreFoundation, CoreMedia, CoreVideo and dlsym(), and a
# /dev/hakios-camera of its own: with clang (CLANG), for the file's one block type, under the sanitizers where that clang
# has them; left out where there is no clang
CLANG=${CLANG:-clang}
if command -v "$CLANG" > /dev/null; then
	CLANG_SANITIZE=()
	if echo 'int main(void) { return 0; }' | "$CLANG" -fsanitize=address,undefined -x c - -o "$WORK/sanitizers-clang" 2> /dev/null; then
		CLANG_SANITIZE=(-O1 '-fsanitize=address,undefined' -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
	fi
	echo "== camera_device_test (host, clang${CLANG_SANITIZE[0]:+, sanitizers})"
	if "$CLANG" -std=gnu11 -fblocks -Wall -Wextra -Werror -O2 -pthread "${CLANG_SANITIZE[@]}" -I"$DARWIN_STUBS" \
		"$HERE/camera_device_test.c" -o "$WORK/camera_device_test" -lm; then
		TMPDIR=$WORK scenarios "" "$WORK/camera_device_test"
	else
		failed=1
	fi
else
	echo "== (no clang: camera_device_test is left out)"
fi

# backboardd's touch screen and buttons on Darwin 26 (userland/userland/src/inject26.c): the part of the file between its
# "---- backboardd" and "---- big thread stacks" lines, cut out unchanged (the rest is Objective-C runtime calls and
# blocks), with what it needs of Darwin's frameworks and of the device in the test, on the host under the sanitizers;
# the part includes the phone's orientation service and the accelerometer's from beside it
# (userland/userland/src/hid_orientation.h, hid_motion.h)
mkdir -p "$WORK/inject26"
sed -n '/^\/\* ---- backboardd: the touch screen and the buttons/,/^\/\* ---- big thread stacks/p' \
	"$HERE/../userland/userland/src/inject26.c" > "$WORK/inject26/inject26_input.c"
echo "== backboard_input_test (host${SANITIZE[0]:+, sanitizers})"
if ! grep -q '^input_thread(' "$WORK/inject26/inject26_input.c"; then
	echo "FAIL: no input part between the two lines in userland/userland/src/inject26.c"
	failed=1
elif "$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -pthread "${SANITIZE[@]}" -I"$WORK/inject26" -I"$HERE/../userland/userland/src" "$HERE/backboard_input_test.c" \
	-o "$WORK/backboard_input_test" -lm; then
	scenarios "" "$WORK/backboard_input_test"
else
	failed=1
fi

# the same on the main Darwin 26/27 image, where backboardd's library is libhakios.dylib from userland/userland/src/inject.c
# (darwin/userland/inj26.sh), not inject26.c: its part between "---- backboardd: the touch screen and the buttons" and
# "---- containermanagerd", cut out unchanged, in backboard_input_test.c's scenarios (backboard_input16_test.c)
mkdir -p "$WORK/inject"
sed -n '/^\/\* ---- backboardd: the touch screen and the buttons/,/^\/\* ---- containermanagerd/p' \
	"$HERE/../userland/userland/src/inject.c" > "$WORK/inject/inject_input.c"
echo "== backboard_input16_test (host${SANITIZE[0]:+, sanitizers})"
if ! grep -q '^input_thread(' "$WORK/inject/inject_input.c"; then
	echo "FAIL: no input part between the two lines in userland/userland/src/inject.c"
	failed=1
elif "$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -pthread "${SANITIZE[@]}" -I"$WORK/inject" -I"$HERE/../userland/userland/src" "$HERE/backboard_input16_test.c" \
	-o "$WORK/backboard_input16_test" -lm; then
	scenarios "" "$WORK/backboard_input16_test"
else
	failed=1
fi

# SpringBoard's remote control (userland/userland/src/inject_sb.c), what any program's com.hakios.layers and
# com.hakios.switcher make it do: the part of the file from its SEND macros to Control Center's fades, cut out unchanged
# but for those two macros (the test's own call a method of its runtime with the types given), on the host under the sanitizers
mkdir -p "$WORK/inject_sb"
sed -n '/^#define SEND(type/,/^#define CC_FADE_MS/p' "$HERE/../userland/userland/src/inject_sb.c" |
	sed -e '/^#define SEND(/d' -e '/^#define SEND1(/,/objc_msgSend/d' > "$WORK/inject_sb/inject_sb_remote.c"
echo "== springboard_remote_test (host${SANITIZE[0]:+, sanitizers})"
if ! grep -q '^layer_line(' "$WORK/inject_sb/inject_sb_remote.c" || ! grep -q '^switcher(' "$WORK/inject_sb/inject_sb_remote.c"; then
	echo "FAIL: no layer_line() and switcher() between the two lines in userland/userland/src/inject_sb.c"
	failed=1
elif "$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -pthread "${SANITIZE[@]}" -I"$WORK/inject_sb" "$HERE/springboard_remote_test.c" \
	-o "$WORK/springboard_remote_test"; then
	scenarios "" "$WORK/springboard_remote_test"
else
	failed=1
fi

# the kernel's stand-in for corecrypto (crypto/, as osfmk/minicrypto) with the parts of XNU's own corecrypto it rests on,
# from the kernel tree kernel/env.sh names (XNU_SRC): on the host under the sanitizers, and under qemu-aarch64 as the
# picture processor's (built there without the DIT register's code: gcc has no builtin for it, and it would not run).
# Left out where that tree is not there.
XNU=$(source "$HERE/../kernel/env.sh" > /dev/null 2>&1 && echo "$XNU_SRC") || XNU=''
if [ -n "$XNU" ] && [ -f "$XNU/osfmk/corecrypto/cchmac.c" ] && [ -d "$XNU/EXTERNAL_HEADERS/corecrypto" ]; then
	MINICRYPTO=$WORK/kernel/osfmk/minicrypto
	MINICRYPTO_SOURCES=("$HERE/minicrypto_test.c" "$MINICRYPTO/mc_aes.c" "$MINICRYPTO/mc_digests.c")
	for part in ccdigest_init ccdigest_update ccdigest_final_64be cchmac_init cchmac_update cchmac_final cchmac cc_clear cc_cmp_safe; do
		MINICRYPTO_SOURCES+=("$XNU/osfmk/corecrypto/$part.c")
	done
	MINICRYPTO_FLAGS=(-std=gnu11 -Wall -Wextra -Werror -O2 -I"$MINICRYPTO" -I"$XNU/EXTERNAL_HEADERS" -I"$XNU/osfmk/corecrypto"
		'-D__unused=__attribute__((unused))' -Wno-parentheses)
	echo "== minicrypto_test (host${SANITIZE[0]:+, sanitizers})"
	if "$CC" "${MINICRYPTO_FLAGS[@]}" "${SANITIZE[@]}" "${MINICRYPTO_SOURCES[@]}" -o "$WORK/minicrypto_test" && "$WORK/minicrypto_test"; then
		:
	else
		failed=1
	fi
	if command -v aarch64-linux-gnu-gcc > /dev/null && command -v qemu-aarch64 > /dev/null; then
		echo "== minicrypto_test (aarch64, qemu-aarch64)"
		if aarch64-linux-gnu-gcc -static "${MINICRYPTO_FLAGS[@]}" -DCC_DIT_MAYBE_SUPPORTED=0 "${MINICRYPTO_SOURCES[@]}" -o "$WORK/minicrypto_test.a64" &&
		    qemu-aarch64 "$WORK/minicrypto_test.a64"; then
			:
		else
			failed=1
		fi
	fi
else
	echo "== (no kernel tree at ${XNU:-the place kernel/env.sh names}: minicrypto_test is left out)"
fi

# the helper that breaks a driver on purpose (workbench/mutate.sh), on the example driver: a change that
# matters is caught, one that changes nothing is not, one that does not compile is said to be that
echo "== mutate.sh (on Documentation/examples/example_led.c)"
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
