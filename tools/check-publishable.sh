#!/bin/bash
# Look for what must not be published before every commit/push: binaries, firmware, Apple's shader code,
# home directories, keys. Prints what it finds; exit status 1 if anything.
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R"
bad=0
say() { echo "$*"; bad=1; }
files=$(find . -type f -not -path './.git/*')

for f in $files; do
	case "$f" in *.png|*.jpg|*.jpeg) continue ;; esac
	t=$(file -b "$f")
	case "$t" in
	*Mach-O*|*ELF*|*"PE32"*|*"Device Tree"*|*LLVM*|*"compiled Java"*) say "binary: $f ($t)" ;;
	esac
	case "$f" in
	*/rc.jobs.*|*/rc.*) ;;
	*.bin|*.mbn|*.elf|*.efi|*.fd|*.dtb|*.metallib|*.bc|*.air|*.ipsw|*.img|*.apfs|*_fw.h) say "firmware/Apple file name: $f" ;;
	esac
done
grep -rlI --exclude-dir=.git --exclude=check-publishable.sh --exclude=export-from-work.sh -e "/Users/[a-z]" -e "/private/tmp/[a-z]*-[0-9]*/" . | while read -r f; do echo "home path: $f"; done | grep . && bad=1
grep -rlIE --exclude-dir=.git --exclude=check-publishable.sh "(ghp_[A-Za-z0-9]{20}|github_pat_|AKIA[0-9A-Z]{16}|BEGIN (RSA|OPENSSH|EC) PRIVATE KEY)" . | while read -r f; do echo "secret: $f"; done | grep . && bad=1
# e-mail addresses other than the author's (git config user.email)
me=$(git config user.email)
grep -rIoE --exclude-dir=.git "[A-Za-z0-9._%+-]+@(gmail|qq|163|outlook|hotmail|icloud|me)\\.com" . | grep -v ":$me\$" | while read -r f; do echo "e-mail: $f"; done | grep . && bad=1
# names of the tools used to write this (the repository is the author's own)
grep -rlIi --exclude-dir=.git "cl[a]ude\\|anthrop[i]c" . | while read -r f; do echo "tool name: $f"; done | grep . && bad=1
# the project's old name (it is HakiOS now)
grep -rlIi --exclude-dir=.git --exclude=export.local "hac[k]ios" . | while read -r f; do echo "old name: $f"; done | grep . && bad=1
find . -path ./.git -prune -o -iname "*hac[k]ios*" -print | while read -r f; do echo "old name in path: $f"; done | grep . && bad=1
# embedded byte arrays (a firmware turned into a header) above 64 rows
for f in $(grep -rlIE --exclude-dir=.git "0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x" .); do
	n=$(grep -cE "0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x" "$f")
	[ "$n" -gt 64 ] && say "byte array ($n rows): $f"
done
[ $bad = 0 ] && echo "nothing found"
exit $bad
