#!/bin/bash
# docs/porting.md, run: a phone nobody has described and a SoC nobody has described are taken through the
# guide's steps in a copy of the repository, with nothing but the tools a porter has. The phone is made
# up, and so are its addresses -- none of them is one the two real phones have, so a value that is still
# written into the code somewhere shows as a wrong answer here.
#
#   step 1   tools/boards.py new from the device tree; what it cannot know is refused by name, then filled in
#   step 3   the loader knows the board by its framebuffer and builds
#   step 4   the kernel finds its row, and nothing of a phone is on until asked
#   step 5   features and sections are added one at a time; the drivers' tests of board data see them
#
# The repository itself is not touched.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
failed=0 checks=0
check() {
	local what=$1
	shift
	checks=$((checks + 1))
	if ! "$@" > "$W/out" 2>&1; then
		failed=$((failed + 1))
		echo "FAIL: $what"
		sed 's/^/    /' "$W/out" | head -15
	fi
}
refuses() {
	local what=$1 text=$2
	shift 2
	checks=$((checks + 1))
	if "$@" > "$W/out" 2>&1 || ! grep -q -- "$text" "$W/out"; then
		failed=$((failed + 1))
		echo "FAIL: $what (wanted a refusal saying \"$text\")"
		sed 's/^/    /' "$W/out" | head -15
	fi
}
says() {
	local what=$1 text=$2
	shift 2
	checks=$((checks + 1))
	if ! "$@" > "$W/out" 2>&1 || ! grep -q -- "$text" "$W/out"; then
		failed=$((failed + 1))
		echo "FAIL: $what (wanted \"$text\")"
		sed 's/^/    /' "$W/out" | head -15
	fi
}

# --- a copy of what a port touches ----------------------------------------------------------------
R=$W/repo
mkdir -p "$R/kernel/overlay/bsd/dev" "$R/tests"
cp -r "$REPO/devices" "$REPO/soc" "$REPO/tools" "$REPO/loader" "$R/"
cp "$REPO"/kernel/overlay/bsd/dev/hakios_* "$R/kernel/overlay/bsd/dev/"
cp -r "$REPO/tests/fakekernel" "$R/tests/"
B="$R/tools/boards.py"

# --- the phone's device tree, as `dtc -I dtb -O dts /sys/firmware/fdt` gives it (cut down) ----------
# Six cores in two groups, its blocks elsewhere than on the 7-series, a video-mode panel of 8-bit
# brightness sent least significant byte first... everything unlike the phones the code was written on.
cat > "$W/phone.dts" <<'EOF'
/ {
	cpus {
		cpu@0 { device_type = "cpu"; reg = <0x00 0x00>; enable-method = "psci"; capacity-dmips-mhz = <0x400>; qcom,freq-domain = <0x21 0x00 0x04>; };
		cpu@100 { device_type = "cpu"; reg = <0x00 0x100>; enable-method = "psci"; capacity-dmips-mhz = <0x400>; qcom,freq-domain = <0x21 0x00 0x04>; };
		cpu@200 { device_type = "cpu"; reg = <0x00 0x200>; enable-method = "psci"; capacity-dmips-mhz = <0x400>; qcom,freq-domain = <0x21 0x00 0x04>; };
		cpu@300 { device_type = "cpu"; reg = <0x00 0x300>; enable-method = "psci"; capacity-dmips-mhz = <0x400>; qcom,freq-domain = <0x21 0x00 0x04>; };
		cpu@400 { device_type = "cpu"; reg = <0x00 0x400>; enable-method = "psci"; capacity-dmips-mhz = <0x700>; qcom,freq-domain = <0x21 0x01 0x04>; };
		cpu@500 { device_type = "cpu"; reg = <0x00 0x500>; enable-method = "psci"; capacity-dmips-mhz = <0x700>; qcom,freq-domain = <0x21 0x01 0x04>; };
	};
	psci { compatible = "arm,psci-1.0"; method = "smc"; };
	soc {
		interrupt-controller@17100000 { compatible = "arm,gic-v3"; reg = <0x00 0x17100000 0x00 0x10000 0x00 0x17180000 0x00 0x100000>; };
		clock-controller@1400000 { compatible = "qcom,madeup-gcc\0syscon"; reg = <0x00 0x1400000 0x00 0x1f0000>; };
		clock-controller@5f00000 { compatible = "qcom,madeup-dispcc\0syscon"; reg = <0x00 0x5f00000 0x00 0x20000>; };
		qcom,cpufreq-hw { compatible = "qcom,cpufreq-hw-epss"; reg = <0x00 0x17d91000 0x00 0x1000 0x00 0x17d92000 0x00 0x1000>; reg-names = "freq-domain0\0freq-domain1"; };
		tsens@4410000 { compatible = "qcom,tsens24xx"; reg = <0x00 0x4410000 0x00 0x08 0x00 0x4411000 0x00 0x1ff>; reg-names = "tsens_srot_physical\0tsens_tm_physical"; };
		qcom,mdss_dsi_ctrl0@5e94000 { compatible = "qcom,dsi-ctrl-hw-v2.4"; cell-index = <0x00>; reg = <0x00 0x5e94000 0x00 0x400 0x00 0x5f08000 0x00 0x04>; reg-names = "dsi_ctrl\0disp_cc_base"; };
		qcom,mdss_dsi_madeup_video {
			qcom,mdss-dsi-panel-name = "a made-up video panel";
			qcom,mdss-dsi-panel-type = "dsi_video_mode";
			qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_dcs";
			qcom,mdss-dsi-bl-min-level = <0x02>;
			qcom,mdss-dsi-bl-max-level = <0xff>;
			qcom,mdss-brightness-init-level = <0x80>;
		};
	};
};
EOF

# --- step 1: describe the board -------------------------------------------------------------------
says "a board is started from the device tree" "wrote devices/testphone/board.toml" \
	"$B" new testphone --soc sm9999 --dts "$W/phone.dts" --description "Maker Testphone"
check "... and its SoC, which is new" test -f "$R/soc/sm9999.toml"
refuses "what a device tree cannot say is asked for by name" "gcc_disp_ahb_cbcr" "$B" check
refuses "... the I/O window too" '"io_base" is missing' "$B" check
check "... and nothing else is wrong with what was written" \
	sh -c "\"$B\" check 2>&1 | grep -v 'gcc_disp_ahb_cbcr\\|dispcc_mdss_gdscr\\|dispcc_mdss_ahb_cbcr\\|io_base\\|io_size' | grep -q . && exit 1 || exit 0"
sed -i -e 's/^# gcc_disp_ahb_cbcr = 0x\.\.\..*/gcc_disp_ahb_cbcr = 0x33004/' \
	-e 's/^# dispcc_mdss_gdscr = 0x\.\.\..*/dispcc_mdss_gdscr = 0x4000/' \
	-e 's/^# dispcc_mdss_ahb_cbcr = 0x\.\.\..*/dispcc_mdss_ahb_cbcr = 0x4080/' \
	-e 's/^# io_base = 0x00400000.*/io_base = 0x01000000/' -e 's/^# io_size = 0x3FC00000.*/io_size = 0x3F000000/' "$R/soc/sm9999.toml"
says "with those filled in, everything is consistent" "ok" "$B" check
check "the tables are regenerated" "$B" gen
says "the board is listed" "testphone" "$B" list
check "the boards that were there are as they were" \
	sh -c "\"$B\" show renoir > \"$W/after\" && \"$REPO/tools/boards.py\" show renoir > \"$W/before\" && cmp \"$W/before\" \"$W/after\""

# --- step 3: the loader ---------------------------------------------------------------------------
check "without a [loader] section the loader does not list the board" \
	sh -c "! grep -q platform_testphone \"$R/loader/platforms.h\""
printf '\n[loader]\nfb_base = 0x9C000000\n' >> "$R/devices/testphone/board.toml"
check "with its framebuffer base it does" sh -c "\"$B\" gen && grep -q '&platform_testphone,' \"$R/loader/platforms.h\""
check "... with the machine as the SoC file has it" \
	sh -c "grep -A14 'platform_testphone = ' \"$R/loader/platforms.h\" | tr -d '\n\t' | grep -q '.io_base = 0x01000000,.io_size = 0x3f000000,.gic_dist = 0x17100000,.gic_redist = 0x17180000,.psci_hvc = 0,.log_other_cpu = 0,.cores = 6,.core_shift = 8,.timeout_scale = 0,.small_cores = 0xf,.fb_base = 0x9c000000,'"
if command -v clang > /dev/null && command -v lld-link > /dev/null; then
	check "the loader builds with the new board" \
		sh -c "clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar -mgeneral-regs-only -O1 -Wall -Wextra -Werror -Wno-unused-parameter -c \"$R/loader/loader.c\" -o \"$W/loader.obj\" && lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro \"/out:$W/loader.efi\" \"$W/loader.obj\""
	check "... and carries the board's name for the kernel" grep -q testphone "$W/loader.efi"
fi

# --- steps 4 and 5: what the kernel takes the board for --------------------------------------------
# A program of the kernel's own board code (hakios_board.c with the regenerated table) that answers for
# the board named on its command line.
cat > "$W/ask.c" <<'EOF'
#include "fakekernel.h"
#include "hakios_board.c"
int
main(int argc, char **argv)
{
	const struct hakios_board *b;

	board = argv[1];
	b = hakios_board();
	if (b == NULL) {
		printf("unknown\n");
		return 0;
	}
	printf("name=%s soc=%s features=[%s]\n", b->name, b->soc, get_string("features"));
	printf("backlight type=%u video=%u bytes=%u lsb=%u dsi=%#x min=%u max=%u restore=%u\n", b->backlight.type, b->backlight.video_mode,
	    b->backlight.bytes, b->backlight.lsb_first, b->backlight.dsi, b->backlight.raw_min, b->backlight.raw_max, b->backlight.raw_restore);
	printf("clocks gcc=%#x+%#x dispcc=%#x gdsc=%#x ahb=%#x\n", b->clocks.gcc, b->clocks.gcc_disp_ahb_cbcr, b->clocks.dispcc,
	    b->clocks.dispcc_mdss_gdscr, b->clocks.dispcc_mdss_ahb_cbcr);
	printf("cpufreq epss=%#x size=%#x groups=%u group_of(3)=%d group_of(4)=%d group_of(6)=%d\n", b->cpufreq.epss,
	    b->cpufreq.epss_domain_size, hakios_board_cpu_groups(), hakios_board_cpu_group_of(3), hakios_board_cpu_group_of(4),
	    hakios_board_cpu_group_of(6));
	printf("thermal tsens=%#x,%#x floors=%u,%u\n", b->thermal.tsens[0], b->thermal.tsens[1], b->thermal.floor_mhz[0],
	    b->thermal.floor_mhz[1]);
	printf("epss=%d touch=%d torch=%d\n", hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS), hakios_board_has(HAKIOS_HAS_TOUCH_GOODIX) ||
	    hakios_board_has(HAKIOS_HAS_TOUCH_FT3658U), hakios_board_has(HAKIOS_HAS_TORCH_PM8350C));
	return 0;
}
EOF
ask() {
	${CC:-cc} -std=gnu11 -w -pthread -I"$R/tests/fakekernel" -I"$R/kernel/overlay/bsd/dev" "$W/ask.c" \
		"$R/tests/fakekernel/fakekernel.c" -o "$W/ask" && "$W/ask" "$1"
}
says "the kernel finds the new board's row" "name=testphone soc=sm9999 features=\[\]" ask testphone
says "... with no driver of a phone on" "epss=0 touch=0 torch=0" ask testphone
says "... its panel as the device tree has it: video mode, one byte, 2 to 255, left at 128" \
	"backlight type=1 video=1 bytes=1 lsb=0 dsi=0x5e94000 min=2 max=255 restore=128" ask testphone
says "... its clocks as the SoC file has them" "clocks gcc=0x1400000+0x33004 dispcc=0x5f00000 gdsc=0x4000 ahb=0x4080" ask testphone
says "... its two groups of cores: four and two" \
	"cpufreq epss=0x17d91000 size=0x1000 groups=2 group_of(3)=0 group_of(4)=1 group_of(6)=-1" ask testphone
says "a board the kernel was not told of is still unknown" "unknown" ask otherphone
says "the phones that were there answer as before" "name=renoir soc=sm7350" ask renoir

# step 5: one thing at a time
sed -i 's/^features = \[\]/features = ["cpufreq-epss"]/' "$R/devices/testphone/board.toml"
check "a feature is added and the tables regenerated" sh -c "\"$B\" check && \"$B\" gen"
says "... and the kernel has it, and only it" "epss=1 touch=0 torch=0" ask testphone
printf '\n[thermal]\nfloor_mhz = [900, 1200, 1300]\n' >> "$R/devices/testphone/board.toml"
refuses "thermal floors that do not match the SoC's groups are refused" "one value for each of the 2 groups" "$B" check
sed -i 's/^floor_mhz = \[900, 1200, 1300\]/floor_mhz = [900, 1200]/' "$R/devices/testphone/board.toml"
check "... and taken when they do" sh -c "\"$B\" check && \"$B\" gen"
says "the thermal limiter gets the SoC's sensor and the board's floors" "thermal tsens=0x4411000,0 floors=900,1200" ask testphone
sed -i 's/"cpufreq-epss"/"cpufreq-epss", "torch-pm8350c", "warp-drive"/' "$R/devices/testphone/board.toml"
refuses "a feature the kernel has no driver for is refused" 'feature "warp-drive" is not one the kernel knows' "$B" check

# --- the repository itself --------------------------------------------------------------------------
check "the repository was not touched" sh -c "cd \"$REPO\" && test ! -e devices/testphone && test ! -e soc/sm9999.toml"

echo "porting_test: $checks checks, $failed failed"
[ $failed = 0 ]
