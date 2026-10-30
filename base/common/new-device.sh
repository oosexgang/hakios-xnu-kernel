#!/bin/bash
# Start a port of HakiOS to another device: a profile and a loader platform description to fill in.
#
# usage: common/new-device.sh <codename> [path/to/device.dtb]
#
# Writes common/devices/<codename>.conf (from _template.conf) and common/devices/<codename>.platform.c,
# a `struct platform` for xnuloader/loader.c. Given the phone's device tree (from its vendor_boot, or
# `adb pull /sys/firmware/fdt`), it fills in what can be read from it: the GIC addresses, the number of
# CPU cores and how MPIDR numbers them, and which cores are the small ones. What it cannot read is left
# as TODO; docs/PORTING.md walks through all of it.
. "$(dirname "$(readlink -f "$0")")/lib.sh"
CODE=$1; DTB=$2
[ -n "$CODE" ] || die "usage: common/new-device.sh <codename> [device.dtb]"
[[ "$CODE" =~ ^[a-z0-9_-]+$ ]] || die "codename: lower-case letters, digits, - and _ only"
CONF=common/devices/$CODE.conf PLATC=common/devices/$CODE.platform.c
[ -e "$CONF" ] && die "$CONF exists already"

sed -e "s/^NAME=.*/NAME=\"TODO vendor model ($CODE)\"/" -e "s/^UEFI_TARGET=.*/UEFI_TARGET=$CODE/" \
	common/devices/_template.conf > "$CONF"
[ -n "$DTB" ] && sed -i "s|^DTB=.*|DTB=$(readlink -f "$DTB")|" "$CONF"

GIC_DIST=TODO GIC_REDIST=TODO CORES=8 SHIFT=8 SMALL=0x0
if [ -n "$DTB" ]; then
	command -v dtc >/dev/null || die "dtc is needed to read $DTB (package dtc)"
	# GIC (compatible "arm,gic-v3": reg = <dist size redist size>, one or two cells each), the cores
	# (cpu@N), how MPIDR numbers them (Qualcomm: reg 0x100, 0x200... -- the second affinity field) and
	# which are small (the cluster with the lowest capacity-dmips-mhz)
	read -r GIC_DIST GIC_REDIST CORES SHIFT SMALL < <(python3 - "$DTB" <<'PY'
import re, subprocess, sys
dts = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', sys.argv[1]], capture_output=True, text=True).stdout
gic = ['TODO', 'TODO']
m = re.search(r'compatible = "arm,gic-v3";(.*?)\};', dts, re.S)
if m:
    cells = [int(x, 16) for x in re.search(r'reg = <([^>]*)>', m.group(1)).group(1).split()]
    if len(cells) == 4: gic = [hex(cells[0]), hex(cells[2])]
    elif len(cells) >= 8: gic = [hex((cells[0] << 32) | cells[1]), hex((cells[4] << 32) | cells[5])]
cpus = []
for m in re.finditer(r'\n\t*cpu@([0-9a-f]+) \{(.*?)\n\t*\};', dts, re.S):
    cap = re.search(r'capacity-dmips-mhz = <(0x[0-9a-f]+)>', m.group(2))
    cpus.append((int(m.group(1), 16), int(cap.group(1), 16) if cap else 0))
shift = 8 if any(r >= 0x100 for r, _ in cpus) else 0
caps = [c for _, c in cpus if c]
small = 0
for r, c in cpus:
    # the little cluster: the lowest capacity, when the cores are not all alike
    if caps and c == min(caps) and min(caps) < max(caps): small |= 1 << (r >> shift)
print(gic[0], gic[1], len(cpus) or 8, shift, hex(small))
PY
)
	echo "new-device: from $DTB: GIC $GIC_DIST / $GIC_REDIST, $CORES cores, MPIDR shift $SHIFT, small cores $SMALL"
fi

cat > "$PLATC" <<EOF
/*
 * $CODE: paste into xnuloader/loader.c next to platform_redwood and add &platform_$CODE to platforms[].
 * Every value is explained in docs/PORTING.md ("The loader").
 */
static const struct platform platform_$CODE = {
	.name = "$CODE",
	.io_base = 0x00400000, .io_size = 0x3FC00000,   /* TODO: one window over the SoC's peripherals, non-zero base */
	.gic_dist = $GIC_DIST, .gic_redist = $GIC_REDIST,
	.uart = 0,                                      /* phones: no PL011 */
	.log_other_cpu = 1,
	.cores = $CORES, .core_shift = $SHIFT,
	.small_cores = $SMALL,                          /* TODO: a bit per "small" core (capacity-dmips-mhz in the DT) */
	.fb_base = 0x0,                                 /* TODO: the framebuffer base the UEFI prints ("display ... at") */
};
EOF
echo "new-device: wrote $CONF and $PLATC"
echo "new-device: next -- docs/PORTING.md, then: common/build.sh $CODE && common/run.sh $CODE"
