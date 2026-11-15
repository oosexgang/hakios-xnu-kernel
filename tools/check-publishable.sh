#!/bin/bash
# Look for what must not be published before every commit/push: binaries, firmware, Apple's shader code,
# home directories, keys. Prints what it finds; exit status 1 if anything.
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R" || exit 1
bad=0
say() { echo "$*"; bad=1; }
# what could be committed: what git tracks and what it would take (a build's output, which .gitignore names,
# is not); outside a git checkout, every file
# (the same list for every check below: a build's own tree -- userland/build's Mesa, say -- holds other
# people's code and addresses, and is not ours to publish or to judge)
if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
	mapfile -t list < <(git ls-files --cached --others --exclude-standard | sed 's|^|./|')
else
	mapfile -t list < <(find . -type f -not -path './.git/*')
fi
files=${list[*]}
# what is in those files: grep over the list, the file names said as ./PATH
in_files() { [ "${#list[@]}" -eq 0 ] || printf '%s\0' "${list[@]}" | xargs -0 grep "$@" 2> /dev/null; }

for f in $files; do
	case "$f" in *.png|*.jpg|*.jpeg) continue ;; esac
	t=$(file -b "$f")
	case "$t" in
	*"Device Tree"*text*) ;;        # a device tree's source (a board.dts, a SoC's .dtsi), not a compiled one
	*Mach-O*|*ELF*|*"PE32"*|*"Device Tree"*|LLVM*|*"compiled Java"*) say "binary: $f ($t)" ;;
	esac
	case "$f" in
	*/rc.jobs.*|*/rc.*) ;;
	*.bin|*.mbn|*.elf|*.efi|*.fd|*.dtb|*.metallib|*.bc|*.air|*.ipsw|*.img|*.apfs|*_fw.h) say "firmware/Apple file name: $f" ;;
	esac
done
in_files -lI --exclude=check-publishable.sh -e "/Users/[a-z]" -e "/private/tmp/[a-z]*-[0-9]*/" | while read -r f; do echo "home path: $f"; done | grep . && bad=1
in_files -lIE --exclude=check-publishable.sh "(ghp_[A-Za-z0-9]{20}|github_pat_|AKIA[0-9A-Z]{16}|BEGIN (RSA|OPENSSH|EC) PRIVATE KEY)" | while read -r f; do echo "secret: $f"; done | grep . && bad=1
# e-mail addresses other than the author's (git config user.email)
me=$(git config user.email)
in_files -HIoE "[A-Za-z0-9._%+-]+@(gmail|qq|163|outlook|hotmail|icloud|me)\\.com" | grep -v ":$me\$" | while read -r f; do echo "e-mail: $f"; done | grep . && bad=1
# names of the tools used to write this (the repository is the author's own)
in_files -lIi "cl[a]ude\\|anthrop[i]c" | while read -r f; do echo "tool name: $f"; done | grep . && bad=1
# the project's old name (it is HakiOS now)
in_files -lIi --exclude=export.local "hac[k]ios" | while read -r f; do echo "old name: $f"; done | grep . && bad=1
printf '%s\n' "${list[@]}" | grep -i "hac[k]ios" | while read -r f; do echo "old name in path: $f"; done | grep . && bad=1
# embedded byte arrays (a firmware turned into a header) above 64 rows
for f in $(in_files -lIE "0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x"); do
	n=$(grep -cE "0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x[0-9a-f]{2}, 0x" "$f")
	[ "$n" -gt 64 ] && say "byte array ($n rows): $f"
done
[ $bad = 0 ] && echo "nothing found"
exit $bad
