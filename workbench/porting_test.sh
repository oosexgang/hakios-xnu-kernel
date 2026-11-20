#!/bin/bash
# Documentation/porting.md, run: a phone nobody has described and a SoC nobody has described are taken through the
# guide's steps in a copy of the repository, with nothing but the tools a porter has. The phone is made
# up, and so are its addresses -- none of them is one the two real phones have, so a value that is still
# written into the code somewhere shows as a wrong answer here.
#
#   step 1   tools/boards.py new from the device tree; what it cannot know is refused by name, then filled in
#   step 3   the loader knows the board by its framebuffer and builds
#   step 4   the kernel finds its row, and nothing of a phone is on until asked
#   step 5   features and sections are added one at a time (CPU frequency, thermal floors, the keys); the
#            kernel's own board code answers with what was written
#   step 8   the package is exported to where packages are kept, taken out, and brought in again
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
mkdir -p "$R/drivers" "$R/workbench" "$R/arch/arm64"
cp -r "$REPO/tools" "$R/"
cp -r "$REPO/arch/arm64/boot" "$R/arch/arm64/"
# (the iPhone identity with nothing of Apple's in it: the one made from an IPSW is somebody's own)
python3 "$R/arch/arm64/boot/mkidentity.py" --placeholder > "$R/arch/arm64/boot/identity.h"
cp -r "$REPO"/drivers/board "$R/drivers/"
mkdir -p "$W/kernel/bsd/dev"
ln -sfn "$R/drivers/board" "$W/kernel/bsd/dev/board"   # (the drivers at their path in the kernel's tree, as the kernel includes them)
cp -r "$REPO/workbench/fakekernel" "$R/workbench/"
B="$R/tools/boards.py"

# --- the phone's device tree, as `dtc -I dtb -O dts /sys/firmware/fdt` gives it (cut down) ----------
# Six cores in two groups, its blocks elsewhere than on the 7-series, a video-mode panel of 8-bit
# brightness sent least significant byte first, its keys on the third PMIC of the bus... everything
# unlike the phones the code was written on.
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
		qcom,spmi@4440000 {
			compatible = "qcom,spmi-pmic-arb";
			reg = <0x00 0x4440000 0x00 0x1100 0x00 0x4600000 0x00 0x2000000 0x00 0x6600000 0x00 0x100000 0x00 0x6700000 0x00 0xa0000 0x00 0x440a000 0x00 0x26000>;
			reg-names = "core\0chnls\0obsrvr\0intr\0cnfg";
			qcom,ee = <0x00>;
			qcom,pmx100@0 { compatible = "qcom,spmi-pmic"; reg = <0x00 0x00>; };
			qcom,pmx200@2 {
				compatible = "qcom,spmi-pmic";
				reg = <0x02 0x00>;
				pon_hlos@800 { compatible = "qcom,qpnp-power-on"; reg = <0x800>; };
				pinctrl@c000 { compatible = "qcom,pmx200-gpio"; reg = <0xc000>; phandle = <0x77>; };
			};
		};
		gpio_keys {
			compatible = "gpio-keys";
			vol_up { label = "volume_up"; gpios = <0x77 0x09 0x01>; linux,code = <0x73>; };
		};
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
says "a board is started from the device tree" "wrote arch/arm64/boot/dts/testphone/board.dts" \
	"$B" new testphone --soc sm9999 --dts "$W/phone.dts" --description "Maker Testphone"
check "... and its SoC, which is new" test -f "$R/arch/arm64/boot/dts/testphone/soc.dtsi"
refuses "what a device tree cannot say is asked for by name" "gcc-disp-ahb-cbcr" "$B" check
check "... and nothing else is wrong with what was written" \
	sh -c "\"$B\" check 2>&1 | grep -v 'gcc-disp-ahb-cbcr, dispcc-mdss-gdscr, dispcc-mdss-ahb-cbcr in clocks { }' | grep -q . && exit 1 || exit 0"
sed -i -e 's|// gcc-disp-ahb-cbcr = <0x\.\.\.>;|gcc-disp-ahb-cbcr = <0x33004>;|' \
	-e 's|// dispcc-mdss-gdscr = <0x\.\.\.>;|dispcc-mdss-gdscr = <0x4000>;|' \
	-e 's|// dispcc-mdss-ahb-cbcr = <0x\.\.\.>;|dispcc-mdss-ahb-cbcr = <0x4080>;|' "$R/arch/arm64/boot/dts/testphone/soc.dtsi"
says "with those filled in, everything is consistent" "ok" "$B" check
check "the tables are regenerated" "$B" gen
says "the board is listed" "testphone" "$B" list
check "the boards that were there are as they were" \
	sh -c "\"$B\" show qemu-virt > \"$W/after\" && \"$REPO/tools/boards.py\" show qemu-virt > \"$W/before\" && cmp \"$W/before\" \"$W/after\""

# --- step 3: the loader ---------------------------------------------------------------------------
check "without a loader { } section the loader does not list the board" \
	sh -c "! grep -q platform_testphone \"$R/build/boards/platforms.h\""
printf '\n/ {\n\tloader {\n\t\tfb-base = <0x9C000000>;\n\t};\n};\n' >> "$R/arch/arm64/boot/dts/testphone/board.dts"
refuses "the I/O window, which no tree has, is asked for when the loader is to know the board" \
	'loader { } needs io-base and io-size in platform { }' "$B" check
sed -i -e 's|// io-base = <0x00400000>;|io-base = <0x01000000>;|' -e 's|// io-size = <0x3fc00000>;|io-size = <0x3F000000>;|' "$R/arch/arm64/boot/dts/testphone/soc.dtsi"
check "with its framebuffer base it does" sh -c "\"$B\" gen && grep -q '&platform_testphone,' \"$R/build/boards/platforms.h\""
check "... with the machine as the SoC file has it" \
	sh -c "grep -A14 'platform_testphone = ' \"$R/build/boards/platforms.h\" | tr -d '\n\t' | grep -q '.io_base = 0x01000000,.io_size = 0x3f000000,.gic_dist = 0x17100000,.gic_redist = 0x17180000,.psci_hvc = 0,.log_other_cpu = 0,.uefi_charger = 0,.cores = 6,.core_shift = 8,.timeout_scale = 0,.small_cores = 0xf,.fb_base = 0x9c000000,'"
if command -v clang > /dev/null && command -v lld-link > /dev/null; then
	check "the loader builds with the new board" \
		sh -c "clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar -mgeneral-regs-only -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I\"$R/build/boards\" -c \"$R/arch/arm64/boot/loader.c\" -o \"$W/loader.obj\" && lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /Brepro \"/out:$W/loader.efi\" \"$W/loader.obj\""
	check "... and carries the board's name for the kernel" grep -q testphone "$W/loader.efi"
fi

# --- steps 4 and 5: what the kernel takes the board for --------------------------------------------
# A program of the kernel's own board code (board/board.c with the regenerated table) that answers for
# the board named on its command line.
cat > "$W/ask.c" <<'EOF'
#include "fakekernel.h"
#include "board/board.c"
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
	printf("spmi core=%#x cnfg=%#x observer=%#x channels=%#x ee=%u\n", b->spmi.core, b->spmi.cnfg, b->spmi.observer,
	    b->spmi.channels, b->spmi.ee);
	printf("buttons=%d pon=%#x (%s) volume_up=%#x (%s)\n", hakios_board_has(HAKIOS_HAS_BUTTONS_PMIC), b->buttons.pon,
	    b->buttons.pon_name ? b->buttons.pon_name : "-", b->buttons.volume_up,
	    b->buttons.volume_up_name ? b->buttons.volume_up_name : "-");
	printf("epss=%d touch=%d torch=%d\n", hakios_board_has(HAKIOS_HAS_CPUFREQ_EPSS), hakios_board_has(HAKIOS_HAS_TOUCH_GOODIX) ||
	    hakios_board_has(HAKIOS_HAS_TOUCH_FT3658U) || hakios_board_has(HAKIOS_HAS_TOUCH_FT5X06), hakios_board_has(HAKIOS_HAS_TORCH_PM8350C));
	return 0;
}
EOF
ask() {
	${CC:-cc} -std=gnu11 -w -pthread -I"$R/workbench/fakekernel" -I"$W/kernel/bsd/dev" -I"$R/build/boards" "$W/ask.c" \
		"$R/workbench/fakekernel/fakekernel.c" -o "$W/ask" && "$W/ask" "$1"
}
says "the kernel finds the new board's row" "name=testphone soc=sm9999 features=\[\]" ask testphone
says "... with no driver of a phone on" "epss=0 touch=0 torch=0" ask testphone
says "... its panel as the device tree has it: video mode, one byte, 2 to 255, left at 128" \
	"backlight type=1 video=1 bytes=1 lsb=0 dsi=0x5e94000 min=2 max=255 restore=128" ask testphone
says "... its clocks as the SoC file has them" "clocks gcc=0x1400000+0x33004 dispcc=0x5f00000 gdsc=0x4000 ahb=0x4080" ask testphone
says "... its two groups of cores: four and two" \
	"cpufreq epss=0x17d91000 size=0x1000 groups=2 group_of(3)=0 group_of(4)=1 group_of(6)=-1" ask testphone
says "a board the kernel was not told of is still unknown" "unknown" ask otherphone
says "the board that was there answers as before" "name=qemu-virt soc=qemu-virt" ask qemu-virt

# step 5: one thing at a time
sed -i 's/features = <>;/features = "cpufreq-epss";/' "$R/arch/arm64/boot/dts/testphone/board.dts"
check "a feature is added and the tables regenerated" sh -c "\"$B\" check && \"$B\" gen"
says "... and the kernel has it, and only it" "epss=1 touch=0 torch=0" ask testphone
printf '\n/ {\n\tthermal {\n\t\tfloor-mhz = <900 1200 1300>;\n\t};\n};\n' >> "$R/arch/arm64/boot/dts/testphone/board.dts"
refuses "thermal floors that do not match the SoC's groups are refused" "one value for each of the 2 groups" "$B" check
sed -i 's/floor-mhz = <900 1200 1300>;/floor-mhz = <900 1200>;/' "$R/arch/arm64/boot/dts/testphone/board.dts"
check "... and taken when they do" sh -c "\"$B\" check && \"$B\" gen"
says "the thermal limiter gets the SoC's sensor and the board's floors" "thermal tsens=0x4411000,0 floors=900,1200" ask testphone
says "the tool says what can be turned on next" "buttons-pmic *can be listed" "$B" status testphone
says "... and what the rest waits for" "gpu-a660 *needs adreno { } in soc.dtsi, .*gpu { } in the board file" "$B" status testphone
# the keys: the tool found the arbiter and both peripherals in the tree, so the feature is all there is to add
says "the SoC file has the PMIC arbiter of the tree" "spmi core=0x4440000 cnfg=0x440a000 observer=0x6600000 channels=0x4600000 ee=0" \
	ask testphone
says "the keys were found, and are not on until listed" \
	"buttons=0 pon=0x208 (the PMX200's power-on block) volume_up=0x2c8 (GPIO9 of the PMX200)" ask testphone
sed -i 's/"cpufreq-epss"/"cpufreq-epss", "buttons-pmic"/' "$R/arch/arm64/boot/dts/testphone/board.dts"
check "the keys' feature is added" sh -c "\"$B\" check && \"$B\" gen"
says "... and the kernel has it" "buttons=1 pon=0x208" ask testphone
sed -i '/^\tbuttons {/,/^\t};/d' "$R/arch/arm64/boot/dts/testphone/board.dts"
refuses "without its section the feature is refused" 'feature "buttons-pmic" needs buttons { } in the board file' "$B" check
printf '\n/ {\n\tbuttons {\n\t\tpon = <0x208>;\n\t\tvolume-up = <0x2c8>;\n\t};\n};\n' >> "$R/arch/arm64/boot/dts/testphone/board.dts"
check "... and taken again with it written by hand" "$B" check
sed -i 's/"cpufreq-epss"/"cpufreq-epss", "torch-pm8350c", "warp-drive"/' "$R/arch/arm64/boot/dts/testphone/board.dts"
refuses "a feature the kernel has no driver for is refused" 'feature "warp-drive" is not one the kernel knows' "$B" check

# --- step 8: the package is sent to where packages are kept, and brought in from there -------------
sed -i 's/, "torch-pm8350c", "warp-drive"//' "$R/arch/arm64/boot/dts/testphone/board.dts"
D="$R/tools/device.py"
mkdir "$W/packages"
says "the package is exported to a checkout of the devices branch" "the package testphone" "$D" export testphone "$W/packages"
check "... as one directory, with its board file" test -f "$W/packages/testphone/board.dts"
says "... and is listed there" "testphone  *sm9999  *Maker Testphone" "$D" available --from "$W/packages"
check "it is taken out of the checkout" "$D" remove testphone
check "(the tables, made anew as a build would)" "$B" gen
says "... and the kernel's code no longer knows it" "unknown" ask testphone
check "another checkout brings it in" "$D" add testphone --from "$W/packages"
says "... validated against the code that is there" "ok" "$B" check
check "(the tables again)" "$B" gen
says "... and has its board again, with what was turned on" "buttons=1 pon=0x208" ask testphone

# --- the repository itself --------------------------------------------------------------------------
check "the repository was not touched" sh -c "cd \"$REPO\" && test ! -e arch/arm64/boot/dts/testphone"

echo "porting_test: $checks checks, $failed failed"
[ $failed = 0 ]
