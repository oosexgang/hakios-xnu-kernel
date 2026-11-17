#!/bin/bash
# What this repository needs of an Ubuntu 24.04 machine (or Debian's), by what it is for, installed or only looked for.
#
#   scripts/setup-ubuntu.sh [--check] [WHAT...]
#
# WHAT is any of the groups below (all of them when none is named); --check installs nothing and says what is
# missing, its status 1 when something is. Installing asks apt (sudo when not root). The SDK's headers and theos are
# not packages: Documentation/building.md says where they come from.
#
#   check    make check: the lint, the boards, the host tests and their aarch64 half under qemu-user, the loader in QEMU
#   kernel   kernel/build.sh with LLVM's own Mach-O tools (clang 20: XNU_CLANG_DIR=/usr/lib/llvm-20/bin)
#   loader   arch/arm64/boot/build.sh: the loader, a UEFI application made with clang and lld
#   qemu     tools/qemu-kernel.sh, make qemu: the kernel booted in QEMU's virt machine under a real UEFI
#   phone    ./bench: the phone on USB, through fastboot and adb
set -uo pipefail
die() { echo "setup-ubuntu.sh: $*" >&2; exit 2; }
declare -A GROUP=(
	[check]="build-essential git python3 gcc-aarch64-linux-gnu qemu-user shellcheck clang lld zlib1g-dev libssl-dev file qemu-system-arm qemu-efi-aarch64"
	[kernel]="build-essential git curl python3 perl xz-utils flex bison m4 autoconf automake gawk clang-20 lld-20 llvm-20"
	[loader]="clang lld"
	[qemu]="qemu-system-arm qemu-efi-aarch64"
	[phone]="fastboot adb"
)
ORDER=(check kernel loader qemu phone)
check='' wanted=()
for argument in "$@"; do
	case $argument in
	--check) check=1 ;;
	-h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option $argument" ;;
	*) [ -n "${GROUP[$argument]+x}" ] || die "no group \"$argument\" (${ORDER[*]})"; wanted+=("$argument") ;;
	esac
done
[ ${#wanted[@]} -gt 0 ] || wanted=("${ORDER[@]}")
command -v dpkg-query > /dev/null || die "no dpkg-query: this is for Debian and Ubuntu (Documentation/building.md has the rest)"

missing=()
for group in "${wanted[@]}"; do
	lacking=()
	for package in ${GROUP[$group]}; do
		dpkg-query -W -f='${Status}' "$package" 2> /dev/null | grep -q 'install ok installed' || lacking+=("$package")
	done
	if [ ${#lacking[@]} -eq 0 ]; then
		echo "$group: everything is there"
	else
		echo "$group: missing ${lacking[*]}"
		missing+=("${lacking[@]}")
	fi
done
[ ${#missing[@]} -gt 0 ] || exit 0
# (a package two groups want is asked for once)
mapfile -t missing < <(printf '%s\n' "${missing[@]}" | sort -u)
if [ -n "$check" ]; then
	echo "setup-ubuntu.sh: scripts/setup-ubuntu.sh ${wanted[*]} installs them"
	exit 1
fi
sudo=
[ "$(id -u)" = 0 ] || sudo=sudo
if ! { $sudo apt-get update -q && $sudo apt-get install -y -q "${missing[@]}"; }; then
	die "apt-get did not install ${missing[*]}"
fi
echo "setup-ubuntu.sh: installed ${missing[*]}"
