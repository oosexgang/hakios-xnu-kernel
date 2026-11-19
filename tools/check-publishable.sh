#!/bin/bash
# Look for what must not be published before every commit/push: binaries, firmware, Apple's shader code,
# home directories, keys. Prints what it finds; exit status 1 if anything.
# What is looked at is what git could publish: the tracked files and the untracked ones it does not ignore
# (tools/files.py --others; in a release's tree, the files the release lists).
# Build outputs (loader.efi, build/) and brought-in device packages are ignored, and so are not looked at.
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R" || exit 1
bad=0
say() { echo "$*"; bad=1; }
# (NUL-separated; every grep below reads the same list)
list=$(mktemp)
trap 'rm -f "$list"' EXIT
tools/files.py -z --others | while IFS= read -r -d '' f; do
	[ -f "$f" ] && [ ! -L "$f" ] && printf './%s\0' "$f"
done > "$list"
scan() { xargs -0 -r grep "$@" < "$list"; }

while IFS= read -r -d '' f; do
	case "$f" in *.png|*.jpg|*.jpeg) continue ;; esac
	t=$(file -b "$f")
	case "$t" in
	*"Device Tree"*text*) ;;        # a device tree's source (a board.dts, a SoC's .dtsi), not a compiled one
	*Mach-O*|*ELF*|*"PE32"*|*"Device Tree"*|LLVM*|*"compiled Java"*) say "binary: $f ($t)" ;;
	esac
	case "$f" in
	*/rc.jobs.*|*/rc.*) ;;
	*.bin|*.mbn|*.elf|*.efi|*.fd|*.dtb|*.metallib|*.bc|*.air|*.ipsw|*.img|*.apfs|*_fw.h|*.payload|*.im4p) say "firmware/Apple file name: $f" ;;
	esac
	# (by its bytes: an image's payload, an identity blob, an Apple image file -- whatever the file is called)
	case "$(head -c 4 "$f" 2> /dev/null | od -An -c | tr -d ' ')" in
	XNUP) say "an image's payload: $f" ;;
	IDNT) say "an identity blob (Apple's data): $f" ;;
	esac
done < "$list"
scan -lI -e "/Users/[a-z]" -e "/private/tmp/[a-z]*-[0-9]*/" | grep -v "^./tools/check-publishable.sh$" | while read -r f; do echo "home path: $f"; done | grep . && bad=1
# (and a Linux home of somebody's: /home/NAME/ -- the build machine's generic /home/user/ and /home/runner/ aside)
scan -HIoE "/home/[a-z][a-z0-9_-]*/" | grep -vE ":/home/(user|runner)/$" | grep -v "^./tools/check-publishable.sh:" | while read -r f; do echo "home path: $f"; done | grep . && bad=1
scan -lIE "(ghp_[A-Za-z0-9]{20}|github_pat_|AKIA[0-9A-Z]{16}|BEGIN (RSA|OPENSSH|EC) PRIVATE KEY)" | grep -v "^./tools/check-publishable.sh$" | while read -r f; do echo "secret: $f"; done | grep . && bad=1
# e-mail addresses other than the author's (git config user.email)
me=$(git config user.email)
scan -HIoE "[A-Za-z0-9._%+-]+@(gmail|qq|163|outlook|hotmail|icloud|me)\\.com" | grep -v ":$me\$" | while read -r f; do echo "e-mail: $f"; done | grep . && bad=1
# names of the tools used to write this (the repository is the author's own)
scan -lIi "cl[a]ude\\|anthrop[i]c" | while read -r f; do echo "tool name: $f"; done | grep . && bad=1
# the project's old name (it is HakiOS now)
scan -lIi --exclude=export.local "hac[k]ios" | while read -r f; do echo "old name: $f"; done | grep . && bad=1
tr '\0' '\n' < "$list" | grep -i "hac[k]ios" | while read -r f; do echo "old name in path: $f"; done | grep . && bad=1
# embedded byte arrays (a firmware turned into a header) above 64 rows; the Metal library's test fixtures are
# the bitcode of its own kernels (components/metal/plugin/tools/kernels/*.ll, made by components/metal/plugin/Makefile), not someone else's
for f in $(scan -lIE "0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x"); do
	n=$(grep -cE "0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x" "$f")
	case "$f" in ./components/metal/plugin/tests/fixtures/*_bc.hpp) continue ;; esac
	[ "$n" -gt 64 ] && say "byte array ($n rows): $f"
done
[ $bad = 0 ] && echo "nothing found"
exit $bad
