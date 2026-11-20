#!/bin/bash
# tools/make-kernel.sh [make args]: build the kernel, and judge the result by what failed, not by the exit status.
#
#   tools/make-kernel.sh -j8         what `make kernel` runs
#   tools/make-kernel.sh --judge LOG   judge a log of an earlier build (no build; this is how it is tested)
#
# xnu-build.sh has to be run with -k ("keep going"), and with it the build exits 2 on this configuration
# even though the kernel is linked: the last steps of XNU's own build make the kernel extensions' symbol
# sets (config/*.symbolset) and run check_all_exports, and they fail with "exported name not in import
# list" for names this kernel does not have (DriverKit's IOService methods, Apple silicon's _t8110dart_*):
# the machine configuration for an Android phone leaves those parts out. Nothing here loads a kext. But
# an exit status that is always 2 says nothing, and ignoring it would let a real compile or link error
# pass. So this accepts exactly the failures that belong to those symbol-set steps, and any other make
# target that fails (a compile, a link) fails the build. Then the kernel file must exist.
cd "$(dirname "$0")/.." || exit 1
KERNEL=${KERNEL:-xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}

# the make targets that failed, less the ones that are the symbol-set steps and the targets that only
# stand above them (config_all and the recursion into config/, and the top-level `all`). GNU make 4 names the makefile's line
# in a failed target ([Makefile:12: all]); macOS's make 3.81 does not ([all]).
unexpected_failures() {
	grep -E '^make(\[[0-9]+\])?: \*\*\* ' "$1" |
		grep -vE '\.symbolset\]|check_all_exports|config_all|_recurse_into_config|\[(Makefile:[0-9]+: )?all\] Error'
}

judge() { # judge LOG: 0 if the kernel is built and only the symbol-set steps failed
	local bad
	bad=$(unexpected_failures "$1")
	if [ -n "$bad" ]; then
		echo "make-kernel: a build step failed that is not one of the kext symbol-set checks:"
		echo "$bad" | cut -c1-200
		return 1
	fi
	[ -f "$KERNEL" ] || { echo "make-kernel: no kernel was produced: $KERNEL"; return 1; }
	return 0
}

if [ "$1" = --judge ]; then
	judge "$2" && echo "make-kernel: the log shows a built kernel; only the kext symbol-set checks failed"
	exit
fi

# what was edited in drivers/ goes into the kernel tree first (only what is newer than the tree's copy), and then the
# drivers the .config builds (CONFIG_HAKIOS_*; every driver when there is no .config): docs/drivers-config.md
if [ -d xnu-src/xnu-8796.121.2 ]; then
	xnu/sync-drivers.sh xnu-src/xnu-8796.121.2 || exit 1
	# the kernel debugger's password (drivers/core/hakios.dbg.c) comes from a file outside the repository
	dbg_token_file=${HAKIOS_DBG_TOKEN_FILE:-$HOME/hakios-work/dbg-token}
	if [ -f "$dbg_token_file" ]; then
		printf '#define DBG_TOKEN "%s"\n' "$(tr -d '\n' < "$dbg_token_file")" > xnu-src/xnu-8796.121.2/bsd/dev/dbg_token_local.h
	else
		rm -f xnu-src/xnu-8796.121.2/bsd/dev/dbg_token_local.h
	fi
	python3 tools/hakios-config.py kernel --tree xnu-src/xnu-8796.121.2 || exit 1
fi

# Khaje's Kryo 265 cores implement ARMv8.0 and do not have PSTATE.PAN.  The
# shared BCM2837 machine target otherwise enables PAN for newer ARMv8.2 phones,
# which makes a clean sapphiren build trap on `msr PAN, #1` during arm_init.
# Derive the compiler switch from the selected device so plain `make kernel`
# is safe and reproducible; an explicitly supplied value still wins.
if [ -z "${HAKIOS_ARMV8_0+x}" ] && grep -qE '^CONFIG_(DEVICE="sapphiren"|SOC="khaje")$' .config 2>/dev/null; then
	export HAKIOS_ARMV8_0=1
fi
log=${KERNEL_LOG:-xnu-build.log}
./xnu-build.sh -k "$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
# The kernel's name is HakiOS's: XNU's build calls its output mach.<config>.<machine>, and the copy every tool takes
# (xl, make phone, make qemu-open, tools/update-prebuilt.sh) is ./hakios-kernel
named() { cp -p "$KERNEL" hakios-kernel && echo "make-kernel: built: hakios-kernel ($KERNEL)"; }
if [ "$status" -eq 0 ]; then
	named
	exit 0
fi
if judge "$log"; then
	named
	echo "make-kernel: (exit status $status is the kext symbol-set checks failing, as they do on this configuration; see $0)"
	exit 0
fi
exit 1
